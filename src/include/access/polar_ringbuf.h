/*-------------------------------------------------------------------------
 *
 * polar_ringbuf.h
 *	  polar ring buffer definitions.
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
 *	  src/include/access/polar_ringbuf.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef POLAR_LOGINDEX_RINGBUF_H
#define POLAR_LOGINDEX_RINGBUF_H

#include "port/atomics.h"
#include "storage/lwlock.h"

/*
 * Each ring buffer reference occupy one slot.
 * Define the upper limit for ring buffer reference
 */
#define POLAR_RINGBUF_MAX_SLOT              32
#define POLAR_RINGBUF_MAX_REF_NAME          63
typedef struct polar_ringbuf_data_t *polar_ringbuf_t;

typedef void (*polar_interrupt_callback) (polar_ringbuf_t);

typedef struct polar_ringbuf_slot_t
{
	/* Is this a strong reference? */
	bool		strong;
	/* The read position of the ring buffer */
	uint64		pread;

	/*
	 * The times of read operation And compare visit times to get least read
	 * position
	 */
	uint64		visit;
	/* Each reference has one identity number */
	uint64		ref_num;
	char		ref_name[POLAR_RINGBUF_MAX_REF_NAME + 1];
} polar_ringbuf_slot_t;

/* The ring buffer reference struct */
typedef struct polar_ringbuf_ref_t
{
	/* referenced ring buffer */
	polar_ringbuf_t rbuf;
	/* The identity number for this reference */
	uint64		ref_num;
	/* The slot number this reference occupied */
	int			slot;
	/* Is this a strong reference? */
	bool		strong;
	char		ref_name[POLAR_RINGBUF_MAX_REF_NAME + 1];
} polar_ringbuf_ref_t;

typedef struct polar_ringbuf_stat
{
	/* The push counter of this ring buffer. */
	pg_atomic_uint64 push_cnt;

	/* The recorded number of pops during the last reset. */
	pg_atomic_uint64 prev_pop_cnt;

	/* The free up counter of this ring buffer. */
	pg_atomic_uint64 free_up_cnt;

	/* The SendPhysical IO counter. */
	pg_atomic_uint64 send_phys_io_cnt;

	/* The cumulative bytes advanced by write pointer. */
	pg_atomic_uint64 total_written;

	/* The cumulative bytes advanced by read pointer. */
	pg_atomic_uint64 total_read;

	/* The evicted reference conter of this ring buffer */
	pg_atomic_uint64 evict_ref_cnt;
} polar_ringbuf_stat;

typedef struct polar_ringbuf_data_t
{
	/* This lock is used to manage slot */
	LWLock		lock;
	/* Break cache line to avoid false sharing */
	char		pad1[PG_CACHE_LINE_SIZE];
	/* The least read position of this ring buffer */
	pg_atomic_uint64 pread;
	/* Break cache line to avoid false sharing */
	char		pad2[PG_CACHE_LINE_SIZE];
	/* The write position of this ring buffer */
	pg_atomic_uint64 pwrite;
	/* Increase this counter for each new reference */
	uint64		ref_num;

	/*
	 * Map to slot array, if slot is in use then corresponding bit is set in
	 * occupied
	 */
	uint64		occupied;
	/* Used to manage reference */
	polar_ringbuf_slot_t slot[POLAR_RINGBUF_MAX_SLOT];

	/*
	 * For ring buffer we cant compare read position to get least read
	 * position And we record each reference's read times. The read position
	 * with least read times is the least read position
	 */
	uint64		min_visit;
	/* ring buffer data size */
	size_t		size;

	/* staticstics of ring buffer */
	polar_ringbuf_stat prs;

	uint8		data[FLEXIBLE_ARRAY_MEMBER];
} polar_ringbuf_data_t;

/*
 * The data record in ring buffer as packet.
 * The packet head include 1 byte as flag and 4 byte record the data length
 */
#define POLAR_RINGBUF_PKTHDRSIZE 5
/* The whole packet size include packet head size and data size */
#define POLAR_RINGBUF_PKT_SIZE(len) ((len) + POLAR_RINGBUF_PKTHDRSIZE)

/* Each packet has one bit state flag */
#define POLAR_RINGBUF_PKT_FREE                  (0x00)	/* The packet data is
														 * not ready for read */
#define POLAR_RINGBUF_PKT_READY                 (0x01)	/* The packet data is
														 * ready for read */
#define POLAR_RINGBUF_PKT_STATE_MASK            (0x01)	/* The packet state mask */
/*
 * Define packet type about WAL.
 * Anyone can define it in your file, but the
 * POLAR_RINGBUF_PKT_INVALID_TYPE and POLAR_RINGBUF_PKT_TYPE_MASK
 * are stable.
 */
#define POLAR_RINGBUF_PKT_INVALID_TYPE          (0x00)	/* The packet type is
														 * invalid */
#define POLAR_RINGBUF_PKT_WAL_META              (0x10)	/* The packet content is
														 * xlog meta or clog */
#define POLAR_RINGBUF_PKT_WAL_STORAGE_BEGIN     (0x20)	/* Indicate we need to
														 * read from storage
														 * directly from this
														 * position */
#define POLAR_RINGBUF_PKT_WAL_STORAGE_END       (0x30)	/* Indicate we will read
														 * from queue after this
														 * position */
#define POLAR_RINGBUF_PKT_TYPE_MASK             (0xF0)	/* The packet type mask */

/*
 * Map a monotonic position to a physical offset in the data array.
 * pread, pwrite, and slot[].pread grow monotonically (never wrap);
 * all rbuf->data[] access must go through this macro.
 */
#define POLAR_RINGBUF_IDX(rbuf, pos) ((size_t)((pos) % (rbuf)->size))

/* Get the packet data size; idx is the monotonic start position of the packet */
static inline uint32
polar_ringbuf_pkt_len(polar_ringbuf_t rbuf, uint64 idx)
{
	uint32		len;
	uint8	   *buf = (uint8 *) &len;
	size_t		phys,
				split,
				todo = sizeof(len);

	/* packet len is saved from idx+1 */
	phys = POLAR_RINGBUF_IDX(rbuf, idx + 1);
	split = ((phys + todo) > rbuf->size) ? rbuf->size - phys : 0;

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

extern polar_ringbuf_t polar_ringbuf_init(uint8 *data, size_t len, int tranche_id);
extern bool polar_ringbuf_new_ref(polar_ringbuf_t rbuf, bool strong, polar_ringbuf_ref_t *ref, char *ref_name);
extern void polar_ringbuf_release_ref(polar_ringbuf_ref_t *ref);
extern bool polar_ringbuf_get_ref(polar_ringbuf_ref_t *ref);
extern bool polar_ringbuf_clear_ref(polar_ringbuf_ref_t *ref);
extern void polar_ringbuf_update_ref(polar_ringbuf_ref_t *ref);

extern ssize_t polar_ringbuf_pkt_write(polar_ringbuf_t rbuf, uint64 idx, int offset, uint8 *buf, size_t len);
extern ssize_t polar_ringbuf_read_next_pkt(polar_ringbuf_ref_t *ref,
										   int offset, uint8 *buf, size_t len);
extern void polar_ringbuf_update_keep_data(polar_ringbuf_t rbuf);
extern void polar_ringbuf_free_up(polar_ringbuf_t rbuf, size_t len, polar_interrupt_callback callback);
extern void polar_ringbuf_wait_for_space(polar_ringbuf_t rbuf, uint64 idx, size_t len);
extern void polar_ringbuf_auto_release_ref(polar_ringbuf_ref_t *ref);
extern bool polar_ringbuf_valid_ref(polar_ringbuf_ref_t *ref);

extern void polar_ringbuf_ref_keep_data(polar_ringbuf_ref_t *ref, float ratio);
extern void polar_ringbuf_reset(polar_ringbuf_t rbuf);

/*
 * Get the free size of the ring buffer.
 *
 * With monotonic counters: used = pwrite - pread, free = size - 1 - used.
 * Clamped to 0 when the queue is fully claimed (or over-claimed by an
 * optimistic reservation), so observability paths never see a negative
 * value.
 */
static inline ssize_t
polar_ringbuf_free_size(polar_ringbuf_t rbuf)
{
	uint64		pwrite = pg_atomic_read_u64(&rbuf->pwrite);
	uint64		pread = pg_atomic_read_u64(&rbuf->pread);
	uint64		used = pwrite - pread;

	if (used >= rbuf->size - 1)
		return 0;
	return (ssize_t) (rbuf->size - 1 - used);
}

/*
 * The data that is available or reserved for read.
 * With monotonic counters: avail = pwrite - slot_pread (always >= 0).
 */
static inline ssize_t
polar_ringbuf_avail(polar_ringbuf_ref_t *ref)
{
	polar_ringbuf_t rbuf = ref->rbuf;

	return (ssize_t) (pg_atomic_read_u64(&rbuf->pwrite) -
					  rbuf->slot[ref->slot].pread);
}

/*
 * Reserve space from the ring buffer for a future write.
 *
 * Advances pwrite by len unconditionally — there is NO free-space check.
 * pwrite is therefore a "claim" marker, not a publish marker: callers may
 * over-reserve when the queue is full and must call
 * polar_ringbuf_wait_for_space() before writing into the returned region.
 *
 * Returns the monotonic position; use POLAR_RINGBUF_IDX() when accessing
 * rbuf->data[].
 *
 * Concurrent callers must serialize on an exclusive lock (e.g. the caller's
 * own spinlock, as XLogInsertRecord uses insertpos_lck).  Future work: the
 * read+write pair can be replaced with pg_atomic_fetch_add_u64() to make
 * reservation lock-free, once the surrounding critical section (e.g. the
 * paired CurrBytePos/PrevBytePos update in ReserveXLogInsertLocation) no
 * longer needs the spinlock.
 */
static inline uint64
polar_ringbuf_pkt_reserve(polar_ringbuf_t rbuf, size_t len)
{
	uint64		idx = pg_atomic_read_u64(&rbuf->pwrite);

	pg_atomic_write_u64(&rbuf->pwrite, idx + len);

	return idx;
}

/*
 * Check whether the next packet for this reference is ready.
 * And return packet length when data is ready for read.
 */
static inline uint8
polar_ringbuf_next_ready_pkt(polar_ringbuf_ref_t *ref, uint32 *pktlen)
{
	polar_ringbuf_t rbuf = ref->rbuf;
	uint64		idx = rbuf->slot[ref->slot].pread;
	size_t		phys = POLAR_RINGBUF_IDX(rbuf, idx);

	*pktlen = 0;

	if ((rbuf->data[phys] & POLAR_RINGBUF_PKT_STATE_MASK) != POLAR_RINGBUF_PKT_READY)
		return POLAR_RINGBUF_PKT_INVALID_TYPE;

	/* Make sure we don't see the packet flag value before get packet length */
	pg_read_barrier();

	*pktlen = polar_ringbuf_pkt_len(rbuf, idx);

	return rbuf->data[phys] & POLAR_RINGBUF_PKT_TYPE_MASK;
}

/*
 * Set packet data length.
 * The param idx is the monotonic start position of this packet.
 */
static inline void
polar_ringbuf_set_pkt_length(polar_ringbuf_t rbuf, uint64 idx, uint32 len)
{
	size_t		phys,
				split,
				todo = sizeof(len);
	uint8	   *buf = (uint8 *) &len;

	/* The first byte is flag and packet length is saved in next 4 bytes */
	phys = POLAR_RINGBUF_IDX(rbuf, idx + 1);

	split = ((phys + todo) > rbuf->size) ? rbuf->size - phys : 0;

	if (split > 0)
	{
		memcpy(rbuf->data + phys, buf, split);
		buf += split;
		todo -= split;
		phys = 0;
	}

	memcpy(rbuf->data + phys, buf, todo);

	pg_atomic_fetch_add_u64(&rbuf->prs.push_cnt, 1);
	pg_atomic_fetch_add_u64(&rbuf->prs.total_written, len);
}

/*
 * Set packet flag which it's saved in idx position.
 * idx is a monotonic position.
 */
static inline void
polar_ringbuf_set_pkt_flag(polar_ringbuf_t rbuf, uint64 idx, uint8 flag)
{
	/* ensure all previous writes are visible before follower continues. */
	pg_write_barrier();
	rbuf->data[POLAR_RINGBUF_IDX(rbuf, idx)] = flag;
}

extern polar_ringbuf_slot_t *polar_get_min_visit_slot_except_keep(polar_ringbuf_t rbuf, int keep_slot);
extern void polar_prs_stat_reset(polar_ringbuf_t rbuf);

#endif
