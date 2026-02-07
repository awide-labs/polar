/*-------------------------------------------------------------------------
 *
 * polar_flush.c
 *	  routines for managing the buffer pool's flush list.
 *
 * Copyright (c) 2024, Alibaba Group Holding Limited
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
 *	  src/backend/storage/buffer/polar_flush.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/polar_logindex_redo.h"
#include "access/xlog.h"
#include "storage/lwlock.h"
#include "storage/polar_bufmgr.h"
#include "storage/polar_flush.h"
#include "utils/guc.h"
#include "utils/polar_log.h"

#define polar_fake_oldest_lsn()	\
	(polar_bg_redo_state_is_parallel(polar_logindex_redo_instance) ? polar_logindex_replayed_oldest_lsn() : GetXLogInsertRecPtr())

#define polar_buffer_set_fake_oldest_lsn(bufHdr) \
	(polar_buffer_set_oldest_lsn(bufHdr, polar_fake_oldest_lsn()))

#define polar_buffer_set_oldest_lsn(bufHdr,lsn) \
	(pg_atomic_write_u64((pg_atomic_uint64 *) &((bufHdr)->oldest_lsn), (lsn)))

#define buffer_not_in_flush_list(buf) \
    (buf->flush_prev == POLAR_FLUSHNEXT_NOT_IN_LIST && \
	 buf->flush_next == POLAR_FLUSHNEXT_NOT_IN_LIST)

#define current_pos_is_unavailable(ctl) \
	(ctl->current_pos == POLAR_FLUSHNEXT_NOT_IN_LIST)

FlushControl *polar_flush_ctl = NULL;

static void remove_one_buffer(FlushList *list, BufferDesc *buf);
static void append_one_buffer(FlushList *list, BufferDesc *buf, XLogRecPtr lsn);

/*
 * Compare two LSN values.
 *
 * Returns:
 *    -1 if a < b
 *     0 if a == b
 *     1 if a > b
 *
 * Note: InvalidXLogRecPtr is considered greater than any valid LSN.
 */
static inline int
polar_flushlist_lsn_compare(XLogRecPtr a, XLogRecPtr b)
{
	if (XLogRecPtrIsInvalid(a) && XLogRecPtrIsInvalid(b))
		return 0;
	if (XLogRecPtrIsInvalid(a))
		return 1;
	if (XLogRecPtrIsInvalid(b))
		return -1;

	if (a < b)
		return -1;
	else if (a > b)
		return 1;
	else
		return 0;
}

/*
 * Indexed minheap implementation
 *
 * Unlike lib/binaryheap.h this implementation allows to remove
 * items by index and update items LSN by index
 */
static void
polar_flushlist_minheap_init(FlushListMinHeap *h)
{
	h->size = 0;
	for (int i = 0; i < POLAR_FLUSHLIST_PARTITIONS; i++)
		h->pos[i] = -1;
}

static void
polar_flushlist_swap_nodes(FlushListMinHeap *h, int i, int j)
{
	FLushListHeapNode tmp = h->heap[i];

	h->heap[i] = h->heap[j];
	h->heap[j] = tmp;
	h->pos[h->heap[i].id] = i;
	h->pos[h->heap[j].id] = j;
}

static void
polar_flushlist_sift_up(FlushListMinHeap *h, int idx)
{
	while (idx > 0)
	{
		int			parent = (idx - 1) / 2;

		if (polar_flushlist_lsn_compare(h->heap[parent].lsn, h->heap[idx].lsn) <= 0)
			break;
		polar_flushlist_swap_nodes(h, idx, parent);
		idx = parent;
	}
}

static void
polar_flushlist_sift_down(FlushListMinHeap *h, int idx)
{
	for (;;)
	{
		int			left = idx * 2 + 1;
		int			right = left + 1;
		int			smallest = idx;

		if (left < h->size &&
			polar_flushlist_lsn_compare(h->heap[left].lsn, h->heap[smallest].lsn) < 0)
			smallest = left;

		if (right < h->size &&
			polar_flushlist_lsn_compare(h->heap[right].lsn, h->heap[smallest].lsn) < 0)
			smallest = right;

		if (smallest == idx)
			break;

		polar_flushlist_swap_nodes(h, idx, smallest);
		idx = smallest;
	}
}

static bool
polar_flushlist_minheap_insert(FlushListMinHeap *h, int id, XLogRecPtr lsn)
{
	int			idx;

	if (XLogRecPtrIsInvalid(lsn))
	{
		return false;
	}

	if (h->size >= POLAR_FLUSHLIST_PARTITIONS || h->pos[id] != -1)
	{
		return false;
	}

	idx = h->size++;
	h->heap[idx].id = id;
	h->heap[idx].lsn = lsn;
	h->pos[id] = idx;
	polar_flushlist_sift_up(h, idx);

	return true;
}

static FLushListHeapNode
polar_flushlist_minheap_pop(FlushListMinHeap *h)
{
	FLushListHeapNode root;

	if (h->size == 0)
	{
		FLushListHeapNode dummy;

		dummy.id = -1;
		dummy.lsn = InvalidXLogRecPtr;
		return dummy;
	}

	root = h->heap[0];

	h->pos[root.id] = -1;
	h->size--;

	if (h->size > 0)
	{
		h->heap[0] = h->heap[h->size];
		h->pos[h->heap[0].id] = 0;
		polar_flushlist_sift_down(h, 0);
	}

	return root;
}

static void
polar_flushlist_minheap_update(FlushListMinHeap *h, int id, XLogRecPtr new_lsn)
{
	int			idx;
	XLogRecPtr	old;

	idx = h->pos[id];

	if (idx == -1)
	{
		return;					/* not present */
	}

	old = h->heap[idx].lsn;
	h->heap[idx].lsn = new_lsn;
	if (polar_flushlist_lsn_compare(new_lsn, old) < 0)
		polar_flushlist_sift_up(h, idx);
	else if (polar_flushlist_lsn_compare(new_lsn, old) > 0)
		polar_flushlist_sift_down(h, idx);
}

static bool
polar_flushlist_minheap_remove(FlushListMinHeap *h, int id)
{
	int			idx;

	idx = h->pos[id];
	if (idx == -1)
	{
		return false;			/* not present */
	}

	h->pos[id] = -1;			/* mark as gone */

	if (idx == h->size - 1)
	{
		/* removing the last element — nothing else to do */
		h->size--;
		return true;
	}

	/* Move last element into idx */
	h->heap[idx] = h->heap[h->size - 1];
	h->pos[h->heap[idx].id] = idx;
	h->size--;

	/* Restore heap order */
	polar_flushlist_sift_up(h, idx);
	polar_flushlist_sift_down(h, idx);

	return true;
}

/*
 * Pick one flush list partition for flushing
 *
 * Returns flush list partition, or NULL when there is nothing to flush at the moment.
 */
FlushList *
polar_flush_list_flush_begin()
{
	FLushListHeapNode heap_node;
	FlushList  *list;

	SpinLockAcquire(&polar_flush_ctl->lock);
	heap_node = polar_flushlist_minheap_pop(&polar_flush_ctl->heap);
	if (heap_node.id < 0)
	{
		/* All lists are being flushed by other bgwriters */
		SpinLockRelease(&polar_flush_ctl->lock);
		return NULL;
	}
	list = &polar_flush_ctl->lists[heap_node.id];
	Assert(!list->flushing);
	list->flushing = true;
	SpinLockRelease(&polar_flush_ctl->lock);

	return list;
}

/*
 * End flushing for partition
 */
void
polar_flush_list_flush_end(FlushList *list)
{
	SpinLockAcquire(&polar_flush_ctl->lock);
	Assert(list->flushing);
	list->flushing = false;
	polar_flushlist_minheap_insert(&polar_flush_ctl->heap, list->index, list->min_lsn);
	SpinLockRelease(&polar_flush_ctl->lock);
}


/*
 * polar_flush_list_ctl_shmem_size
 *
 * Estimate the size of shared memory used by the flush list related structure.
 */
Size
polar_flush_list_ctl_shmem_size(void)
{
	Size		size = 0;

	if (!polar_flush_list_enabled())
		return size;

	/* Size of the shared flush list control block */
	size = add_size(size, MAXALIGN(sizeof(FlushControl)) * POLAR_FLUSHLIST_PARTITIONS);

	return size;
}


/*
 * polar_init_flush_list_ctl -- initialize the flush list control
 */
void
polar_init_flush_list_ctl(bool init)
{
	bool		found;
	int			i;

	if (!polar_flush_list_enabled())
		return;

	/* Get or create the shared memory for flush list control block */
	polar_flush_ctl = (FlushControl *)
		ShmemInitStruct("Flush control status",
						sizeof(FlushControl), &found);

	if (!found)
	{
		/* Only done once, usually in postmaster */
		Assert(init);

		for (i = 0; i < POLAR_FLUSHLIST_PARTITIONS; i++)
		{
			FlushList  *list = &polar_flush_ctl->lists[i];

			pg_atomic_init_u32(&list->count, 0);

			SpinLockInit(&list->flushlist_lock);

			list->first_flush_buffer = POLAR_FLUSHNEXT_END_OF_LIST;
			list->last_flush_buffer = POLAR_FLUSHNEXT_END_OF_LIST;
			list->current_pos = POLAR_FLUSHNEXT_NOT_IN_LIST;
			list->latest_flush_count = 0;

			pg_atomic_init_u64(&list->insert, 0);
			pg_atomic_init_u64(&list->remove, 0);
			pg_atomic_init_u64(&list->find, 0);
			pg_atomic_init_u64(&list->batch_read, 0);
			pg_atomic_init_u64(&list->cbuf, 0);
			pg_atomic_init_u64(&list->vm_insert, 0);
			pg_atomic_init_u64(&list->vm_remove, 0);

			list->min_lsn = InvalidXLogRecPtr;
			list->index = i;
			list->flushing = false;
		}

		SpinLockInit(&polar_flush_ctl->lru_lock);
		LWLockInitialize(&polar_flush_ctl->cbuflock, LWTRANCHE_POLAR_COPY_BUFFER);

		MemSet(&polar_flush_ctl->flush_buffer_io, 0, sizeof(polar_flush_ctl->flush_buffer_io));
		pg_atomic_init_u64(&polar_flush_ctl->flush_buffer_io.bgwriter_flush, 0);

		pg_atomic_init_u64(&polar_flush_ctl->backend_flush, 0);

		polar_flush_ctl->lru_buffer_id = 0;
		polar_flush_ctl->lru_complete_passes = 0;

		polar_flushlist_minheap_init(&polar_flush_ctl->heap);
		SpinLockInit(&polar_flush_ctl->lock);
	}
	else
		Assert(!init);
}

/*
 * polar_get_batch_flush_buffer
 *
 * Get a batch of buffers from flush list and do not remove it, FlushBuffer will
 * remove them from flush list.
 */
int
polar_get_batch_buffer(int *batch_buf, int bgwriter_flush_batch_size, FlushList *list)
{
	int			num = 0;
	int			buffer_id;
	int			flush_count;

	Assert(polar_flush_list_enabled());

	SpinLockAcquire(&list->flushlist_lock);
	if (polar_flush_list_is_empty(list))
	{
		SpinLockRelease(&list->flushlist_lock);
		return num;
	}

	flush_count = list->latest_flush_count;
	if (current_pos_is_unavailable(list))
		list->current_pos = list->first_flush_buffer;

	buffer_id = list->current_pos;
	for (num = 0; num < bgwriter_flush_batch_size; num++)
	{
		Assert(buffer_id != POLAR_FLUSHNEXT_NOT_IN_LIST);

		if (buffer_id == POLAR_FLUSHNEXT_END_OF_LIST)
			break;

		batch_buf[num] = buffer_id;
		buffer_id = GetBufferDescriptor(buffer_id)->flush_next;
	}

	/*
	 * If latest flush count greater than polar_bgwriter_max_batch_size,
	 * revert it to first buffer.
	 */
	if (buffer_id == POLAR_FLUSHNEXT_END_OF_LIST ||
		(flush_count + num) > polar_bgwriter_batch_size)
	{
		list->current_pos = list->first_flush_buffer;
		list->latest_flush_count = 0;
	}
	else
	{
		list->current_pos = buffer_id;
		list->latest_flush_count += num;
	}

	SpinLockRelease(&list->flushlist_lock);
	pg_atomic_fetch_add_u64(&list->batch_read, 1);

	return num;
}

/*
 * polar_remove_buffer_from_flush_list
 *
 * If the buffer has been flushed, remove it from flush list.
 */
void
polar_remove_buffer_from_flush_list(BufferDesc *buf)
{
	FlushList  *list;

	if (!polar_flush_list_enabled())
		return;

	list = &polar_flush_ctl->lists[polar_buffer_get_flushlist_partition(buf)];

	SpinLockAcquire(&list->flushlist_lock);
	polar_buffer_set_oldest_lsn(buf, InvalidXLogRecPtr);
	remove_one_buffer(list, buf);
	SpinLockRelease(&list->flushlist_lock);

	pg_atomic_fetch_sub_u32(&list->count, 1);
	pg_atomic_fetch_add_u64(&list->remove, 1);

	if (buf->tag.forkNum == VISIBILITYMAP_FORKNUM)
		pg_atomic_fetch_add_u64(&list->vm_remove, 1);
}

/*
 * polar_put_buffer_to_flush_list
 *
 * When buffer is modified for the first time, add it to flush list. If lsn is
 * invalid, we will set a fake lsn. Caller should have required the buffer
 * content exclusive lock already.
 */
void
polar_put_buffer_to_flush_list(BufferDesc *buf,
							   XLogRecPtr lsn)
{
	int			idx = polar_buffer_get_flushlist_partition(buf);
	FlushList  *list = &polar_flush_ctl->lists[idx];

	SpinLockAcquire(&list->flushlist_lock);

	/* The buffer must be not in flush list */
	Assert(buffer_not_in_flush_list(buf));

	/* Allocate the current insert lsn as a fake oldest lsn */
	if (XLogRecPtrIsInvalid(lsn))
		lsn = polar_fake_oldest_lsn();
	polar_buffer_set_oldest_lsn(buf, lsn);

	append_one_buffer(list, buf, lsn);
	SpinLockRelease(&list->flushlist_lock);

	/* Outside the spin lock to update statistic info. */
	pg_atomic_fetch_add_u32(&list->count, 1);
	pg_atomic_fetch_add_u64(&list->insert, 1);

	if (buf->tag.forkNum == VISIBILITYMAP_FORKNUM)
		pg_atomic_fetch_add_u64(&list->vm_insert, 1);
}

/*
 * polar_adjust_position_in_flush_list
 *
 * Adjust the position of buffer to keep the flush list order, only set a fake
 * oldest lsn.
 */
void
polar_adjust_position_in_flush_list(BufferDesc *buf)
{
	FlushList  *list = &polar_flush_ctl->lists[polar_buffer_get_flushlist_partition(buf)];
	XLogRecPtr	lsn;

	SpinLockAcquire(&list->flushlist_lock);

	/* Buffer must be in flush list */
	Assert(!buffer_not_in_flush_list(buf));
	lsn = polar_fake_oldest_lsn();
	polar_buffer_set_oldest_lsn(buf, lsn);

	/*
	 * If it is the last one, its oldest lsn is the greatest, so do not need
	 * to adjust its position.
	 */
	if (buf->flush_next != POLAR_FLUSHNEXT_END_OF_LIST)
	{
		Assert(buf->flush_next != POLAR_FLUSHNEXT_NOT_IN_LIST);

		/* Not the tail, remove and append it into flush list */
		remove_one_buffer(list, buf);
		append_one_buffer(list, buf, lsn);
	}

	SpinLockRelease(&list->flushlist_lock);
	pg_atomic_fetch_add_u64(&list->cbuf, 1);
}

/*
 * Remove one buffer from flush list, caller should already acquired the
 * flush list lock.
 */
static void
remove_one_buffer(FlushList *list, BufferDesc *buf)
{
	int			prev_flush_id;
	int			next_flush_id;
	BufferDesc *prev_buf;
	BufferDesc *next_buf;

	/* The buffer must be in flush list */
	Assert(!buffer_not_in_flush_list(buf));

	/* Flushlist must be not empty */
	Assert(!polar_flush_list_is_empty(list));

	prev_flush_id = buf->flush_prev;
	next_flush_id = buf->flush_next;

	if (unlikely(polar_enable_debug))
		POLAR_LOG_BUFFER_DESC_WITH_FLUSHLIST(buf, list);

	if (prev_flush_id == POLAR_FLUSHNEXT_END_OF_LIST &&
		next_flush_id == POLAR_FLUSHNEXT_END_OF_LIST)
	{
		/* Only this buffer in flush list */
		list->first_flush_buffer = POLAR_FLUSHNEXT_END_OF_LIST;
		list->last_flush_buffer = POLAR_FLUSHNEXT_END_OF_LIST;
		SpinLockAcquire(&polar_flush_ctl->lock);
		list->min_lsn = InvalidXLogRecPtr;
		if (!list->flushing)
		{
			bool		succ = polar_flushlist_minheap_remove(&polar_flush_ctl->heap, list->index);

			Assert(succ);
			(void) succ;
		}
		SpinLockRelease(&polar_flush_ctl->lock);
	}
	else if (prev_flush_id == POLAR_FLUSHNEXT_END_OF_LIST &&
			 next_flush_id != POLAR_FLUSHNEXT_END_OF_LIST)
	{
		/* First one, and has next buffer */
		next_buf = GetBufferDescriptor(next_flush_id);
		next_buf->flush_prev = prev_flush_id;
		list->first_flush_buffer = next_flush_id;
		SpinLockAcquire(&polar_flush_ctl->lock);
		list->min_lsn = next_buf->oldest_lsn;
		if (!list->flushing)
		{
			polar_flushlist_minheap_update(&polar_flush_ctl->heap, list->index, list->min_lsn);
		}
		SpinLockRelease(&polar_flush_ctl->lock);
	}
	else if (prev_flush_id != POLAR_FLUSHNEXT_END_OF_LIST &&
			 next_flush_id == POLAR_FLUSHNEXT_END_OF_LIST)
	{
		/* Last one, and has prev buffer */
		prev_buf = GetBufferDescriptor(prev_flush_id);
		prev_buf->flush_next = next_flush_id;
		list->last_flush_buffer = prev_flush_id;
	}
	else
	{
		/* Middle */
		next_buf = GetBufferDescriptor(next_flush_id);
		prev_buf = GetBufferDescriptor(prev_flush_id);
		prev_buf->flush_next = next_flush_id;
		next_buf->flush_prev = prev_flush_id;
	}

	if (buf->buf_id == list->current_pos)
	{
		if (next_flush_id == POLAR_FLUSHNEXT_END_OF_LIST)
			list->current_pos = list->first_flush_buffer;
		else
			list->current_pos = next_flush_id;
	}

	/* Remove buffer from flush list */
	buf->flush_next = POLAR_FLUSHNEXT_NOT_IN_LIST;
	buf->flush_prev = POLAR_FLUSHNEXT_NOT_IN_LIST;
}

/*
 * Append one buffer to flush list, caller should already acquired the
 * flush list lock.
 */
static void
append_one_buffer(FlushList *list, BufferDesc *buf, XLogRecPtr lsn)
{
	if (unlikely(polar_enable_debug))
		POLAR_LOG_BUFFER_DESC_WITH_FLUSHLIST(buf, list);

	if (unlikely(polar_flush_list_is_empty(list)))
	{
		buf->flush_next = POLAR_FLUSHNEXT_END_OF_LIST;
		buf->flush_prev = POLAR_FLUSHNEXT_END_OF_LIST;

		list->first_flush_buffer = buf->buf_id;
		list->last_flush_buffer = buf->buf_id;
		SpinLockAcquire(&polar_flush_ctl->lock);
		list->min_lsn = lsn;
		if (!list->flushing)
		{
			polar_flushlist_minheap_insert(&polar_flush_ctl->heap, list->index, list->min_lsn);
		}
		SpinLockRelease(&polar_flush_ctl->lock);
	}
	else
	{
		BufferDesc *tail = GetBufferDescriptor(list->last_flush_buffer);

		if (unlikely(tail->oldest_lsn > buf->oldest_lsn))
			elog(PANIC, "Append buffer with a small oldest lsn than last buffer in flush list.");

		Assert(tail->flush_next == POLAR_FLUSHNEXT_END_OF_LIST);

		buf->flush_prev = tail->buf_id;
		buf->flush_next = tail->flush_next;
		tail->flush_next = buf->buf_id;

		/* Append at the tail */
		list->last_flush_buffer = buf->buf_id;
	}
}
