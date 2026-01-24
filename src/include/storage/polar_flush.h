/*-------------------------------------------------------------------------
 *
 * polar_flush.h
 *	  Routines for managing the buffer pool's flush list.
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
 *	  src/include/storage/polar_flush.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef POLAR_FLUSH_H
#define POLAR_FLUSH_H

#include "postgres.h"

#include "storage/buf_internals.h"
#include "utils/guc.h"

#define FLUSH_LIST_LEN(ctl) (pg_atomic_read_u32(&ctl->count))

#define polar_flush_list_is_empty(ctl) \
    (ctl->first_flush_buffer == POLAR_FLUSHNEXT_END_OF_LIST && \
    ctl->last_flush_buffer == POLAR_FLUSHNEXT_END_OF_LIST)

#define polar_flush_list_enabled() \
	(polar_enable_shared_storage_mode && polar_enable_flushlist)

#define polar_normal_buffer_sync_enabled() \
	(!polar_enable_shared_storage_mode || polar_is_replica() || !polar_flush_list_enabled())

/*
 * The free_next field is either the index of the next flush list entry,
 * or one of these special values:
 */
#define POLAR_FLUSHNEXT_END_OF_LIST	(-1)
#define POLAR_FLUSHNEXT_NOT_IN_LIST	(-2)

typedef struct polar_sync_buffer_io
{
	/* flush woker flush buffer count */
	pg_atomic_uint64 bgwriter_flush;

	/* work flush buffer info */
	TimestampTz st_tz;
	int			pre_flush_rate;
	int			pre_workers;
	int			pre_flush_count;
	bool		first;

} polar_sync_buffer_io;


/* The shared flush list control information. */
typedef struct FlushList
{
	/* The number of buffers in flush list */
	pg_atomic_uint32 count;

	/* Spinlock: protects flushlist values below */
	slock_t		flushlist_lock;

	int			first_flush_buffer; /* Head of list of dirty buffers */
	int			last_flush_buffer;	/* Tail of list of dirty buffers */
	int			current_pos;	/* Parallel workers get buffer from this
								 * position */
	int			latest_flush_count; /* The number of buffers all parallel
									 * bgwriters flushed latest */

	XLogRecPtr	min_lsn;
	int			index;
	bool		flushing;

	/* statistic info */
	pg_atomic_uint64 insert;
	pg_atomic_uint64 remove;
	pg_atomic_uint64 find;
	pg_atomic_uint64 batch_read;
	pg_atomic_uint64 cbuf;
	pg_atomic_uint64 vm_insert;
	pg_atomic_uint64 vm_remove;
} FlushList;

typedef struct FLushListHeapNode
{
	int			id;
	XLogRecPtr	lsn;
} FLushListHeapNode;

#define POLAR_FLUSHLIST_PARTITIONS (64)

typedef struct FlushListMinHeap
{
	int			size;
	int			pos[POLAR_FLUSHLIST_PARTITIONS];
	FLushListHeapNode heap[POLAR_FLUSHLIST_PARTITIONS];
} FlushListMinHeap;

typedef struct FlushControl
{
	polar_sync_buffer_io flush_buffer_io;
	FlushList	lists[POLAR_FLUSHLIST_PARTITIONS];
	FlushListMinHeap heap;
	slock_t		lock;

	pg_atomic_uint64 backend_flush;

	/* LWlock: flush copy buffer */
	LWLock		cbuflock;

	/* Spinlock: protects lru values below */
	slock_t		lru_lock;

	uint32		lru_buffer_id;
	uint32		lru_complete_passes;
} FlushControl;

extern FlushControl *polar_flush_ctl;

extern pg_atomic_uint32 polar_flush_list_next;

#define POLAR_FLUSHLIST_PARTITIONS (64)

#define polar_buffer_get_flushlist_partition(buf) (buf->buf_id % POLAR_FLUSHLIST_PARTITIONS)

extern int	polar_get_batch_buffer(int *batch_buf, int bgwriter_flush_batch_size, FlushList *list);
extern void polar_remove_buffer_from_flush_list(BufferDesc *buf);
extern void polar_put_buffer_to_flush_list(BufferDesc *buf, XLogRecPtr lsn);
extern void polar_adjust_position_in_flush_list(BufferDesc *buf);
extern Size polar_flush_list_ctl_shmem_size(void);
extern void polar_init_flush_list_ctl(bool init);

extern FlushList *polar_flush_list_flush_begin(void);
extern void polar_flush_list_flush_end(FlushList *list);

#endif							/* POLAR_FLUSH_H */
