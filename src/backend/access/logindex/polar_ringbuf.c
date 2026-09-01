/*-------------------------------------------------------------------------
 *
 * polar_ringbuf.c
 *	  PolarDB ring buffer interface routines.
 *
 * Copyright (c) 2022, Alibaba Group Holding Limited
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * IDENTIFICATION
 *	  src/backend/access/logindex/polar_ringbuf.c
 *
 *-------------------------------------------------------------------------
 */

/*
 * Principal entry points:
 *
 * polar_ringbuf_pkt_reserve() -- Reserve space from ring buffer for future write.
 *          And this function should be protected by exclusive lock.
 *
 * polar_ringbuf_pkt_write() -- Write packet data to reserved space and it's safe for multi process
 *          to write different packet
 *
 * polar_ringbuf_new_ref() -- Create new ring buffer reference
 *
 * polar_ringbuf_read_next_pkt() -- Read data from ring buffer sequentially
 *
 */

#include "postgres.h"

#include "access/polar_logindex_redo.h"
#include "access/polar_ringbuf.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/ipc.h"
#include "utils/polar_bitpos.h"

static bool polar_ringbuf_update_pread(polar_ringbuf_t rbuf);

#define RINGBUF_REF_MATCH(ref) \
	((ref)->slot >= 0 && (ref)->slot < POLAR_RINGBUF_MAX_SLOT \
	 && (ref)->rbuf->slot[(ref)->slot].ref_num == (ref)->ref_num)

/*
 * Minimum and maximum sleep, in microseconds, between reclaim attempts in
 * polar_ringbuf_wait_for_space().
 */
#define RINGBUF_WAIT_MIN_US 10
#define RINGBUF_WAIT_MAX_US 1000

/*
 * Reset a slot to its unused state.  Must be called with rbuf->lock held
 * exclusively.  The atomics need re-initializing after the MemSet because
 * pg_atomic_uint64 is not guaranteed to be plain memory on every platform.
 */
static void
polar_ringbuf_slot_clear(polar_ringbuf_slot_t *slot)
{
	MemSet(slot, 0, sizeof(polar_ringbuf_slot_t));
	pg_atomic_init_u64(&slot->pread, 0);
	pg_atomic_init_u64(&slot->visit, 0);
}

/*
 * Initialize ring buffer from allocated memory.
 */
polar_ringbuf_t
polar_ringbuf_init(uint8 *data, size_t len, int tranche_id)
{
	polar_ringbuf_t rbuf = (polar_ringbuf_t) data;
	int			i;

	LWLockInitialize(&rbuf->lock, tranche_id);

	pg_atomic_init_u64(&rbuf->pread, 0);
	pg_atomic_init_u64(&rbuf->pwrite, 0);

	pg_atomic_init_u64(&rbuf->prs.push_cnt, 0);
	pg_atomic_init_u64(&rbuf->prs.free_up_cnt, 0);
	pg_atomic_init_u64(&rbuf->prs.send_phys_io_cnt, 0);
	pg_atomic_init_u64(&rbuf->prs.total_written, 0);
	pg_atomic_init_u64(&rbuf->prs.total_read, 0);
	pg_atomic_init_u64(&rbuf->prs.prev_pop_cnt, 0);
	pg_atomic_init_u64(&rbuf->prs.evict_ref_cnt, 0);

	for (i = 0; i < POLAR_RINGBUF_MAX_SLOT; i++)
		polar_ringbuf_slot_clear(&rbuf->slot[i]);

	rbuf->size = len - offsetof(polar_ringbuf_data_t, data);
	rbuf->occupied = 0;

	MemSet(rbuf->data, POLAR_RINGBUF_PKT_FREE, rbuf->size);

	return rbuf;
}

/*
 * Create new reference for ring buffer and return true on success
 */
bool
polar_ringbuf_new_ref(polar_ringbuf_t rbuf, bool strong,
					  polar_ringbuf_ref_t *ref, char *ref_name)
{
	int			i;
	uint64		unoccupied;
	bool		succeed = false;

	elog(LOG, "create reference for %s", ref_name);

	LWLockAcquire(&rbuf->lock, LW_EXCLUSIVE);
	unoccupied = ~(rbuf->occupied);
	POLAR_BIT_LEAST_POS(unoccupied, i);

	/* The position start from 1 */
	if (i > 0 && i <= POLAR_RINGBUF_MAX_SLOT)
	{
		POLAR_BIT_OCCUPY(rbuf->occupied, i);
		i--;
		rbuf->slot[i].strong = strong;
		pg_atomic_write_u64(&rbuf->slot[i].pread, pg_atomic_read_u64(&rbuf->pread));
		pg_atomic_write_u64(&rbuf->slot[i].visit, rbuf->min_visit);
		rbuf->slot[i].ref_num = ++rbuf->ref_num;
		ref->rbuf = rbuf;
		ref->ref_num = rbuf->slot[i].ref_num;
		ref->slot = i;
		ref->strong = strong;
		ref->promoted = false;

		if (ref_name != NULL)
		{
			strncpy(rbuf->slot[i].ref_name, ref_name, POLAR_RINGBUF_MAX_REF_NAME);
			rbuf->slot[i].ref_name[POLAR_RINGBUF_MAX_REF_NAME] = '\0';
		}
		else
			rbuf->slot[i].ref_name[0] = '\0';

		strcpy(ref->ref_name, rbuf->slot[i].ref_name);

		succeed = true;
	}

	LWLockRelease(&rbuf->lock);

	return succeed;
}

/*
 * release ring buffer reference
 */
void
polar_ringbuf_release_ref(polar_ringbuf_ref_t *ref)
{
	polar_ringbuf_t rbuf = ref->rbuf;

	if (rbuf == NULL)
		return;

	LWLockAcquire(&rbuf->lock, LW_EXCLUSIVE);

	if (RINGBUF_REF_MATCH(ref))
	{
		POLAR_BIT_RELEASE_OCCUPIED(rbuf->occupied, ref->slot + 1);
		polar_ringbuf_slot_clear(&rbuf->slot[ref->slot]);
		polar_ringbuf_update_pread(rbuf);
	}

	LWLockRelease(&rbuf->lock);

	ref->rbuf = NULL;
}

/*
 * The callback function to do auto release reference
 */
static void
polar_ringbuf_auto_release(int code, Datum arg)
{
	polar_ringbuf_ref_t *ref = (polar_ringbuf_ref_t *) arg;

	polar_ringbuf_release_ref(ref);
}

/*
 * Register auto release for the reference
 * It will be released automatically when proc_exit
 */
void
polar_ringbuf_auto_release_ref(polar_ringbuf_ref_t *ref)
{
	/* check whether it is able to register a before_shmem_exit callback */
	if (polar_check_before_shmem_exit(polar_ringbuf_auto_release, (Datum) ref, false))
		before_shmem_exit(polar_ringbuf_auto_release, (Datum) ref);
}

/*
 * Get strong reference from weak reference
 */
bool
polar_ringbuf_get_ref(polar_ringbuf_ref_t *ref)
{
	bool		got = false;
	polar_ringbuf_t rbuf = ref->rbuf;

	/*
	 * The strong reference property is changed in the same process, and it's
	 * safe to use shared lock
	 */
	LWLockAcquire(&rbuf->lock, LW_SHARED);

	if (RINGBUF_REF_MATCH(ref))
	{
		rbuf->slot[ref->slot].strong = true;
		ref->promoted = !ref->strong;
		got = true;
	}

	LWLockRelease(&rbuf->lock);

	return got;
}

/*
 * Check whether it's valid reference
 */
bool
polar_ringbuf_valid_ref(polar_ringbuf_ref_t *ref)
{
	bool		ret = false;

	if (!ref || !ref->rbuf)
		return ret;

	LWLockAcquire(&(ref->rbuf->lock), LW_SHARED);

	ret = RINGBUF_REF_MATCH(ref);

	LWLockRelease(&(ref->rbuf->lock));

	return ret;
}

/*
 * Undo temporary promotion of a reference created weak.  References created
 * strong must remain strong until released, since their owners publish read
 * positions without locking and rely on the slot never being evicted.
 */
bool
polar_ringbuf_clear_ref(polar_ringbuf_ref_t *ref)
{
	bool		cleared = false;
	polar_ringbuf_t rbuf = ref->rbuf;

	Assert(!ref->strong);

	/*
	 * The strong reference property is changed in the same process, and it's
	 * safe to use shared lock
	 */
	LWLockAcquire(&rbuf->lock, LW_SHARED);

	if (RINGBUF_REF_MATCH(ref))
	{
		rbuf->slot[ref->slot].strong = false;
		ref->promoted = false;
		cleared = true;
	}

	LWLockRelease(&rbuf->lock);

	return cleared;
}

/*
 * The reference read packet sequentially.
 * Support read from the offset of packet
 */
ssize_t
polar_ringbuf_read_next_pkt(polar_ringbuf_ref_t *ref,
							int offset, uint8 *buf, size_t len)
{
	size_t		todo;
	size_t		split;
	size_t		pktlen;
	size_t		phys;
	polar_ringbuf_t rbuf = ref->rbuf;
	uint64		idx = pg_atomic_read_u64(&rbuf->slot[ref->slot].pread);

	pktlen = polar_ringbuf_pkt_len(rbuf, idx);

	if (unlikely(offset >= pktlen))
		return -1;

	if ((offset + len) > pktlen)
		len = pktlen - offset;

	phys = POLAR_RINGBUF_IDX(rbuf, idx + POLAR_RINGBUF_PKTHDRSIZE + offset);
	todo = len;
	split = ((phys + len) > rbuf->size) ? rbuf->size - phys : 0;

	if (unlikely(split > 0))
	{
		memcpy(buf, rbuf->data + phys, split);
		buf += split;
		todo -= split;
		phys = 0;
	}

	memcpy(buf, rbuf->data + phys, todo);

	return len;
}

/*
 * Write packet data and the idx is the start position of the packet.
 * Support write from the offset of packet.
 */
ssize_t
polar_ringbuf_pkt_write(polar_ringbuf_t rbuf, uint64 idx, int offset, uint8 *buf, size_t len)
{
	size_t		todo;
	size_t		split;
	size_t		phys;
	size_t		pktlen = polar_ringbuf_pkt_len(rbuf, idx);

	if (offset >= pktlen || (offset + len) > pktlen)
		return -1;

	phys = POLAR_RINGBUF_IDX(rbuf, idx + POLAR_RINGBUF_PKTHDRSIZE + offset);
	split = (phys + len > rbuf->size) ? rbuf->size - phys : 0;
	todo = len;

	if (split > 0)
	{
		memcpy(rbuf->data + phys, buf, split);
		buf += split;
		todo -= split;
		phys = 0;
	}

	memcpy(rbuf->data + phys, buf, todo);

	return len;
}

/*
 * Update the ring buffer least read position and to get more free space
 *
 * Since positions are monotonic, the least read position is simply the
 * minimum over the live slots' pread - no need to go through the visit
 * counters.  That matters because a strong reference publishes its pread
 * without taking rbuf->lock (see polar_ringbuf_update_ref): a (visit, pread)
 * pair read here would not be coherent, and pairing a stale visit with a
 * fresh pread would let us free data another reference has not consumed yet.
 * Any value we read from a slot is an under-estimate of its current position,
 * so taking the minimum of those is always safe.
 *
 * Caller must hold rbuf->lock exclusively.
 */
bool
polar_ringbuf_update_pread(polar_ringbuf_t rbuf)
{
	int			i;
	uint64		min_slot = POLAR_RINGBUF_MAX_SLOT;
	uint64		min_pread = UINT64_MAX;
	uint64		min_visit = UINT64_MAX;
	bool		updated = false;
	uint64		occupied = rbuf->occupied;
	uint64		last_pread = pg_atomic_read_u64(&rbuf->pread);

	while (occupied)
	{
		uint64		slot_pread;
		uint64		slot_visit;

		POLAR_BIT_LEAST_POS(occupied, i);
		i--;

		slot_pread = pg_atomic_read_u64(&rbuf->slot[i].pread);
		slot_visit = pg_atomic_read_u64(&rbuf->slot[i].visit);

		if (min_pread > slot_pread)
		{
			min_pread = slot_pread;
			min_slot = i;
		}

		/* Statistics only, see polar_prs_stat_reset() */
		if (min_visit > slot_visit)
			min_visit = slot_visit;

		POLAR_BIT_CLEAR_LEAST(occupied);
	}

	if (min_slot != POLAR_RINGBUF_MAX_SLOT)
		rbuf->min_visit = min_visit;

	if (min_slot != POLAR_RINGBUF_MAX_SLOT && min_pread > last_pread)
	{
		uint64		new_pread = min_pread;
		uint64		bytes_to_free = new_pread - last_pread;
		size_t		phys_start = POLAR_RINGBUF_IDX(rbuf, last_pread);
		size_t		phys_end = POLAR_RINGBUF_IDX(rbuf, new_pread);

		Assert(bytes_to_free > 0);

		/*
		 * Clear freed region in the physical data array.  With monotonic
		 * counters, new_pread > last_pread here, but the physical region may
		 * wrap around the buffer boundary.
		 */
		if (phys_start < phys_end)
		{
			MemSet(&rbuf->data[phys_start], POLAR_RINGBUF_PKT_FREE,
				   phys_end - phys_start);
		}
		else
		{
			/* Wraps around the buffer boundary */
			MemSet(&rbuf->data[phys_start], POLAR_RINGBUF_PKT_FREE,
				   rbuf->size - phys_start);
			if (phys_end > 0)
				MemSet(&rbuf->data[0], POLAR_RINGBUF_PKT_FREE, phys_end);
		}

		pg_write_barrier();
		pg_atomic_write_u64(&rbuf->pread, new_pread);
		updated = true;

		pg_atomic_fetch_add_u64(&rbuf->prs.total_read, bytes_to_free);
	}

	return updated;
}

/*
 * Update the reference's read position
 *
 * This is the only way space is ever handed back to writers, and it runs once
 * per consumed packet.  A slot that is strong publishes its position without
 * touching rbuf->lock: a strong slot is never evicted (see
 * polar_ringbuf_evict_ref) and is only released by its own owner, so nobody
 * else ever writes it.  That keeps the consumer off the lock the blocked
 * writers contend for in polar_ringbuf_wait_for_space() - otherwise a queue
 * full of waiting writers starves the very readers that would drain it.
 *
 * That covers a reference created strong, and equally one that promoted its
 * slot with polar_ringbuf_get_ref() and has not cleared it yet, which is how
 * every sustained drain runs: polar_send_physical_by_queue() and
 * polar_xlog_send_queue_check() both wrap their pop loop in get_ref/clear_ref.
 */
void
polar_ringbuf_update_ref(polar_ringbuf_ref_t *ref)
{
	polar_ringbuf_t rbuf = ref->rbuf;
	uint64		idx = pg_atomic_read_u64(&rbuf->slot[ref->slot].pread);
	uint32		pktlen;

	/*
	 * The length below comes straight out of the packet header, so the packet
	 * must already be published; callers check polar_ringbuf_next_ready_pkt()
	 * first.
	 */
	Assert((rbuf->data[POLAR_RINGBUF_IDX(rbuf, idx)] & POLAR_RINGBUF_PKT_STATE_MASK)
		   == POLAR_RINGBUF_PKT_READY);

	pktlen = polar_ringbuf_pkt_len(rbuf, idx);

	/* must be a strong ref so that pread can be updated without lock */
	Assert(ref->strong || ref->promoted);

	/*
	 * The caller has already copied the packet out
	 * (polar_ringbuf_read_next_pkt); advancing pread is what releases that
	 * space to writers, so make sure those loads have completed before a
	 * writer can overwrite the bytes.
	 */
	pg_memory_barrier();
	pg_atomic_write_u64(&rbuf->slot[ref->slot].pread,
						idx + POLAR_RINGBUF_PKTHDRSIZE + pktlen);
	pg_atomic_fetch_add_u64(&rbuf->slot[ref->slot].visit, 1);
}

/*
 * No enouth space to write and eliminate one weak reference with least read position
 */
static bool
polar_ringbuf_evict_ref(polar_ringbuf_t rbuf)
{
	int			i;
	bool		evicted = false;
	uint8		min_slot = POLAR_RINGBUF_MAX_SLOT;
	uint64		min_pread = UINT64_MAX;
	uint64		occupied = rbuf->occupied;

	while (occupied)
	{
		POLAR_BIT_LEAST_POS(occupied, i);
		i--;

		if (!rbuf->slot[i].strong
			&& min_pread > pg_atomic_read_u64(&rbuf->slot[i].pread))
		{
			min_pread = pg_atomic_read_u64(&rbuf->slot[i].pread);
			min_slot = i;
		}

		POLAR_BIT_CLEAR_LEAST(occupied);
	}

	if (min_slot != POLAR_RINGBUF_MAX_SLOT
		&& pg_atomic_read_u64(&rbuf->pread) == pg_atomic_read_u64(&rbuf->slot[min_slot].pread))
	{
		elog(LOG, "polar evict slot=%d,name=%s which is weak reference", min_slot, rbuf->slot[min_slot].ref_name);
		POLAR_BIT_RELEASE_OCCUPIED(rbuf->occupied, min_slot + 1);
		polar_ringbuf_slot_clear(&rbuf->slot[min_slot]);
		evicted = true;
		pg_atomic_fetch_add_u64(&rbuf->prs.evict_ref_cnt, 1);
	}

	return evicted;
}

/*
 * Update the least read position of the ring buffer to get more space for write
 */
void
polar_ringbuf_update_keep_data(polar_ringbuf_t rbuf)
{
	LWLockAcquire(&rbuf->lock, LW_EXCLUSIVE);
	polar_ringbuf_update_pread(rbuf);
	LWLockRelease(&rbuf->lock);
}

/*
 * Wait until a previously reserved region [idx, idx+len) is safe to write.
 *
 * With monotonic pwrite, a reservation can advance pwrite past what pread
 * allows (optimistic reservation).  This function blocks until readers
 * have consumed enough data that the physical region no longer overlaps
 * unconsumed data, i.e. pread >= idx + len - (size - 1).
 *
 * Locking: must be called with NO WAL insert locks held.  Holding them
 * here would re-create the walwriter+logindex-writer deadlock (walwriter
 * blocks on WaitXLogInsertionsToFinish, the logindex writer can't drain,
 * the queue stays full).
 *
 * Reclaiming is a single-writer job, so we only ever try the lock
 * conditionally: whoever holds it is already doing the work for everybody.
 * Queueing up behind it instead would put every blocked inserter on the same
 * wait list, which collapses into a convoy on the wait list spinlock and
 * starves polar_ringbuf_new_ref()/polar_ringbuf_release_ref() as well.
 */
void
polar_ringbuf_wait_for_space(polar_ringbuf_t rbuf, uint64 idx, size_t len)
{
	uint64		end = idx + len;
	uint64		last_pread = pg_atomic_read_u64(&rbuf->pread);
	long		delay = RINGBUF_WAIT_MIN_US;

	/*
	 * Fast path: our region is already free.  This is the common case when
	 * the queue is large relative to the write rate.
	 */
	if (polar_ringbuf_reservation_ready(rbuf, end, last_pread))
		return;

	pgstat_report_wait_start(WAIT_EVENT_LOGINDEX_QUEUE_SPACE);
	pg_atomic_fetch_add_u64(&rbuf->prs.free_up_cnt, 1);

	while (!polar_ringbuf_reservation_ready(rbuf, end,
											pg_atomic_read_u64(&rbuf->pread)))
	{
		bool		progress = false;
		uint64		current_pread;

		if (LWLockConditionalAcquire(&rbuf->lock, LW_EXCLUSIVE))
		{
			progress = (polar_ringbuf_update_pread(rbuf) ||
						polar_ringbuf_evict_ref(rbuf));
			LWLockRelease(&rbuf->lock);
		}

		/*
		 * Another waiter may have reclaimed our region even if we failed to
		 * acquire the lock.  Check before sleeping, and reset the backoff on
		 * shared progress rather than only on our own reclaim attempts.
		 */
		current_pread = pg_atomic_read_u64(&rbuf->pread);
		if (polar_ringbuf_reservation_ready(rbuf, end, current_pread))
			break;

		if (current_pread != last_pread || progress)
			delay = RINGBUF_WAIT_MIN_US;
		last_pread = current_pread;

		/* Retry immediately after reclaiming space or evicting a reference. */
		if (progress)
			continue;

		CHECK_FOR_INTERRUPTS();
		pg_usleep(delay);
		delay = Min(delay * 2, RINGBUF_WAIT_MAX_US);
	}

	pgstat_report_wait_end();
}

/*
 * Free up space until free size larger than aimed length
 */
void
polar_ringbuf_free_up(polar_ringbuf_t rbuf, size_t len, polar_interrupt_callback callback)
{
	bool		free_up = false;

	pgstat_report_wait_start(WAIT_EVENT_LOGINDEX_QUEUE_SPACE);

	pg_atomic_fetch_add_u64(&rbuf->prs.free_up_cnt, 1);

	do
	{
		LWLockAcquire(&rbuf->lock, LW_EXCLUSIVE);
		free_up = (polar_ringbuf_free_size(rbuf) > len);

		if (!free_up)
		{
			if (!polar_ringbuf_update_pread(rbuf)
				&& !polar_ringbuf_evict_ref(rbuf))
			{
				/*
				 * 1. Try to update reference's pread to get more space 2. Try
				 * to evict weak reference 3. Otherwise wait for reference to
				 * comsume data
				 */
				LWLockRelease(&rbuf->lock);
				CHECK_FOR_INTERRUPTS();

				if (callback != NULL)
					callback(rbuf);

				pg_usleep(10);
				continue;
			}
		}

		LWLockRelease(&rbuf->lock);
	}
	while (!free_up);

	pgstat_report_wait_end();
}

/*
 * Skip this reference forward so it holds back at most ratio of the ring.
 *
 * Only walks over packets the producer has actually published.  How far we
 * would like to go is measured against pwrite, but that is a claim marker
 * since the optimistic reservation, so the bytes above the last published
 * packet are claimed but unwritten.  Stop at the publish frontier, read from
 * each header's state byte before trusting its length.
 *
 * This is discard, not read: nothing consumes the packets we step over, so the
 * position is tracked in a local and published once at the end rather than
 * through polar_ringbuf_update_ref() per packet.  Catching up across a large
 * queue is tens of millions of packets; paying a barrier and two atomics on
 * the slot's cache line for each of them costs seconds, and every writer is
 * blocked on this reference the whole time.
 */
void
polar_ringbuf_ref_keep_data(polar_ringbuf_ref_t *ref, float ratio)
{
	polar_ringbuf_t rbuf = ref->rbuf;
	ssize_t		keep_size = rbuf->size * ratio;
	uint64		pos = pg_atomic_read_u64(&rbuf->slot[ref->slot].pread);
	uint64		pwrite = pg_atomic_read_u64(&rbuf->pwrite);
	uint64		skipped = 0;

	Assert(ref->strong || ref->promoted);

	while ((ssize_t) (pwrite - pos) > keep_size)
	{
		size_t		phys = POLAR_RINGBUF_IDX(rbuf, pos);

		/* Stop at the publish frontier, see above */
		if ((rbuf->data[phys] & POLAR_RINGBUF_PKT_STATE_MASK) != POLAR_RINGBUF_PKT_READY)
			break;

		/* Don't read the length before the flag that publishes it */
		pg_read_barrier();

		pos += POLAR_RINGBUF_PKTHDRSIZE + polar_ringbuf_pkt_len(rbuf, pos);
		skipped++;
	}

	if (skipped == 0)
		return;

	pg_atomic_write_u64(&rbuf->slot[ref->slot].pread, pos);
	pg_atomic_fetch_add_u64(&rbuf->slot[ref->slot].visit, skipped);

	polar_ringbuf_update_keep_data(rbuf);
}

void
polar_ringbuf_reset(polar_ringbuf_t rbuf)
{
	for (;;)
	{
		LWLockAcquire(&rbuf->lock, LW_EXCLUSIVE);

		if (rbuf->occupied != 0)
		{
			LWLockRelease(&rbuf->lock);
			continue;
		}

		pg_atomic_write_u64(&rbuf->pread, 0);
		pg_atomic_write_u64(&rbuf->pwrite, 0);
		MemSet(rbuf->data, POLAR_RINGBUF_PKT_FREE, rbuf->size);
		LWLockRelease(&rbuf->lock);
		break;
	}
}

void
polar_prs_stat_reset(polar_ringbuf_t queue)
{
	uint64		min_visit;

	if (!queue)
		return;

	LWLockAcquire(&queue->lock, LW_EXCLUSIVE);
	min_visit = queue->min_visit;
	LWLockRelease(&queue->lock);

	pg_atomic_write_u64(&queue->prs.push_cnt, 0);
	pg_atomic_write_u64(&queue->prs.prev_pop_cnt, min_visit);
	pg_atomic_write_u64(&queue->prs.free_up_cnt, 0);
	pg_atomic_write_u64(&queue->prs.send_phys_io_cnt, 0);
	pg_atomic_write_u64(&queue->prs.total_written, 0);
	pg_atomic_write_u64(&queue->prs.total_read, 0);
	pg_atomic_write_u64(&queue->prs.evict_ref_cnt, 0);
}
