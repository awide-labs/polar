/*-------------------------------------------------------------------------
 *
 * polar_xlogbuf.h
 *	  xlog buffer manager data types.
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
 *	  src/include/storage/polar_xlogbuf.h
 *
 *-------------------------------------------------------------------------
 */

/*
 * Xlog buffer will reserve recent xlog pages in memory to reduce io overhead.
 * When lookup a xlog page, if buffer matched or current buffer is older than
 * requested page, we will return buffer id and hold content lock of buffer.
 * Caller should release content lock after read or update buffer.
 *
 * For now, replace strategy is to reserve newer xlog pages than older ones. So
 * in polar_xlog_buffer_should_evict() we will check lsn from buffer meta and
 * requested page, if requested page is newer we will evict current buffer,
 * otherwise requested page should be obtained by io directly.
 */

#ifndef POLAR_XLOGBUF_H
#define POLAR_XLOGBUF_H

#include "access/xlogdefs.h"
#include "storage/buf.h"
#include "storage/lwlock.h"

/*
 * Enable xlog buffer in the following cases:
 * (1) In replica node.
 * The startup process, the logindex bgwriter and the backends
 * could read the same xlog block more than one time.
 * (2) In primary node while doing online promote.
 * The parallel replayers and the backends could read the same
 * xlog block more than one time.
 * (3) In standby node with parallel replay.
 * The startup process, the parallel replayers and the backends
 * could read the same xlog block more than one time.
 */
#define POLAR_ENABLE_XLOG_BUFFER() \
	(IsUnderPostmaster && \
	 polar_xlog_buffer_ins && \
	 (polar_is_replica() || polar_bg_redo_state_is_parallel(polar_logindex_redo_instance)))

#define POLAR_XLOG_BUFFER_TOTAL_COUNT() (polar_xlog_page_buffers * 1024 / (XLOG_BLCKSZ / 1024))
#define polar_get_xlog_buffer_id(lsn) (((lsn) / XLOG_BLCKSZ) % POLAR_XLOG_BUFFER_TOTAL_COUNT())
#define polar_get_xlog_buffer_desc(buf_id) (&(polar_xlog_buffer_ins->buffer_descriptors[(buf_id)].desc))
#define polar_get_xlog_buffer(buf_id) ((char *)(polar_xlog_buffer_ins->buffers + ((Size)(buf_id)) * XLOG_BLCKSZ))
#define polar_xlog_buffer_desc_get_lock(buf) ((LWLock*)&(polar_xlog_buffer_ins->buffer_lock_array[(buf)->buf_id].lock))

#define XLOGBUFFERDESC_PAD_TO_SIZE	(SIZEOF_VOID_P == 8 ? 64 : 1)
#define MAX_READ_AHEAD_XLOGS 200

/*
 * Split xlog buffer ops into two modes for polar_xlog_buffer_lookup(...):
 * POLAR_XLOG_BUFFER_SEARCH: try to lookup one xlog page in xlog buffer.
 * Then return true if found and lock it with LW_SHARED, oth return
 * false.
 * POLAR_XLOG_BUFFER_REPLACE: like POLAR_XLOG_BUFFER_SEARCH except for
 * replacing it with xlog page from disk if not found. Then return false
 * if not found and lock it with LW_EXCLUSIVE, or return true is found
 * and lock it with LW_SHARED.
 */
typedef enum
{
	POLAR_XLOG_BUFFER_SEARCH,
	POLAR_XLOG_BUFFER_REPLACE
} polar_xlog_buffer_mode;

typedef struct polar_xlog_buffer_desc
{
	int			buf_id;			/* buffer index */
	XLogRecPtr	start_lsn;
	XLogRecPtr	end_lsn;
} polar_xlog_buffer_desc;

typedef union polar_xlog_buffer_desc_padded
{
	polar_xlog_buffer_desc desc;
	char		pad[XLOGBUFFERDESC_PAD_TO_SIZE];
} polar_xlog_buffer_desc_padded;

StaticAssertDecl(sizeof(polar_xlog_buffer_desc) <= XLOGBUFFERDESC_PAD_TO_SIZE,
				 "padding size is too small to fit polar_xlog_buffer_desc");

typedef struct polar_xlog_buffer_ctl_t
{
	polar_xlog_buffer_desc_padded *buffer_descriptors;
	char	   *buffers;
	polar_lwlock_mini_padded *buffer_lock_array;
	pg_atomic_uint64 hit_count;
	pg_atomic_uint64 io_count;
	pg_atomic_uint64 others_append_count;
	pg_atomic_uint64 startup_append_count;
} polar_xlog_buffer_ctl_t;
typedef polar_xlog_buffer_ctl_t *polar_xlog_buffer_ctl;

/* in polar_xlogbuf.c */
extern bool polar_enable_xlog_buffer_test;
extern int	polar_xlog_page_buffers;
extern polar_xlog_buffer_ctl polar_xlog_buffer_ins;

extern Size polar_xlog_buffer_shmem_size(void);
extern void polar_init_xlog_buffer(char *prefix);

extern bool polar_xlog_buffer_lookup(XLogRecPtr cur_page_lsn, int len, char *cur_page);
extern bool polar_xlog_buffer_append(XLogRecPtr cur_page_lsn, int len, char *cur_page);
extern void polar_xlog_buffer_update(XLogRecPtr lsn);
extern void polar_xlog_buffer_remove(XLogRecPtr lsn);

static inline void
polar_xlog_buffer_unlock(int buf_id)
{
	LWLockRelease(polar_xlog_buffer_desc_get_lock(polar_get_xlog_buffer_desc(buf_id)));
}

/*
 * POLAR: Judge whether evict the buffer page from desc or not.
 *
 * Caller should hold the desc lock.
 *
 * For now, if requested page is older than buffed page, it won't evict buffer.
 */
static inline bool
polar_xlog_buffer_should_evict(polar_xlog_buffer_desc *buf, XLogRecPtr lsn, int len)
{
	/* If buffer valid size is smaller than current data, evict it.  */
	if (buf->end_lsn >= lsn + len - 1)
		return false;

	return true;
}

static inline void
polar_xlog_buffer_remove_range(XLogRecPtr start_lsn, XLogRecPtr end_lsn)
{
	start_lsn = start_lsn - (start_lsn % XLOG_BLCKSZ);
	end_lsn = end_lsn - (end_lsn % XLOG_BLCKSZ);

	while (start_lsn <= end_lsn)
	{
		polar_xlog_buffer_remove(start_lsn);
		start_lsn += XLOG_BLCKSZ;
	}
}

#endif							/* POLAR_XLOGBUF_H */
