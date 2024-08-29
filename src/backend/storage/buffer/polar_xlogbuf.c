/*-------------------------------------------------------------------------
 *
 * polar_xlogbuf.c
 *	  xlog buffer manager.
 *
 * Copyright (c) 2021, Alibaba Group Holding Limited
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
 *	  src/backend/storage/buffer/polar_xlogbuf.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "miscadmin.h"
#include "storage/ipc.h"
#include "storage/polar_xlogbuf.h"
#include "storage/shmem.h"
#include "utils/guc.h"
#include "utils/polar_log.h"

int			polar_xlog_page_buffers = 0;
bool		polar_enable_xlog_buffer_test = false;
polar_xlog_buffer_ctl polar_xlog_buffer_ins = NULL;

static uint64 total_count = 0;
static uint64 hit_count = 0;
static uint64 io_count = 0;
static uint64 others_append_count = 0;
static uint64 startup_append_count = 0;

#define POLAR_LOG_XLOG_BUFFER_STAT() \
do \
{ \
	ereport(LOG, \
			errmsg("XLog Buffer Hit Ratio: total_count=%ld, hit_count=%ld(%lf), " \
				   "io_count=%ld(%lf), others_append_count=%ld(%lf), startup_append_count=%ld(%lf)", \
				   total_count, hit_count, (double) hit_count / (double) total_count, \
				   io_count, (double) io_count / (double) total_count, \
				   others_append_count, (double) others_append_count / (double) total_count, \
				   startup_append_count, (double) startup_append_count / (double) total_count)); \
} while (0)

static void
polar_log_xlog_buffer_at_exit(int code, Datum arg)
{
	/* log hit ratio after 0x20000(1GB WAL) pages' lookup */
	if (total_count > 0x20000)
		POLAR_LOG_XLOG_BUFFER_STAT();
}

void
polar_init_xlog_buffer(char *prefix)
{
	bool		found,
				foundDesc,
				foundBlock,
				foundLock;
	Size		block_count;
	char		name[MAXPGPATH];

	if (polar_xlog_page_buffers <= 0 ||
		(polar_is_primary() && !polar_enable_xlog_buffer_test))
		return;

	block_count = (Size) POLAR_XLOG_BUFFER_TOTAL_COUNT();

	sprintf(name, "%s CTL", prefix);
	polar_xlog_buffer_ins = (polar_xlog_buffer_ctl)
		ShmemInitStruct(name,
						MAXALIGN(sizeof(polar_xlog_buffer_ctl_t)),
						&found);

	sprintf(name, "%s Descriptor", prefix);
	polar_xlog_buffer_ins->buffer_descriptors = (polar_xlog_buffer_desc_padded *)
		ShmemInitStruct(name,
						block_count * sizeof(polar_xlog_buffer_desc_padded),
						&foundDesc);

	sprintf(name, "%s Blocks", prefix);
	polar_xlog_buffer_ins->buffers = (char *)
		ShmemInitStruct(name,
						block_count * XLOG_BLCKSZ,
						&foundBlock);

	sprintf(name, "%s Locks", prefix);
	polar_xlog_buffer_ins->buffer_lock_array = (polar_lwlock_mini_padded *)
		ShmemInitStruct(name,
						block_count * sizeof(polar_lwlock_mini_padded),
						&foundLock);

	if (!IsUnderPostmaster)
	{
		int			i;

		Assert(!found && !foundDesc && !foundBlock && !foundLock);

		for (i = 0; i < block_count; ++i)
		{
			polar_xlog_buffer_desc *buf = polar_get_xlog_buffer_desc(i);

			buf->buf_id = i;
			buf->start_lsn = InvalidXLogRecPtr;
			buf->end_lsn = InvalidXLogRecPtr;
			LWLockInitialize(polar_xlog_buffer_desc_get_lock(buf), LWTRANCHE_XLOG_BUFFER_CONTENT);
		}

		pg_atomic_init_u64(&polar_xlog_buffer_ins->hit_count, 0);
		pg_atomic_init_u64(&polar_xlog_buffer_ins->io_count, 0);
		pg_atomic_init_u64(&polar_xlog_buffer_ins->others_append_count, 0);
		pg_atomic_init_u64(&polar_xlog_buffer_ins->startup_append_count, 0);
	}
	else
		Assert(found && foundDesc && foundBlock && foundLock);
}

Size
polar_xlog_buffer_shmem_size(void)
{
	Size		size = 0;
	Size		block_count;

	if (polar_xlog_page_buffers <= 0 ||
		(polar_is_primary() && !polar_enable_xlog_buffer_test))
		return size;

	block_count = (Size) POLAR_XLOG_BUFFER_TOTAL_COUNT();
	/* size of xlog buffer ctl */
	size = add_size(size, MAXALIGN(sizeof(polar_xlog_buffer_ctl_t)));
	/* size of xlog block buffer descriptor */
	size = add_size(size, mul_size(block_count, sizeof(polar_xlog_buffer_desc_padded)));
	/* to allow aligning the above */
	size = add_size(size, PG_CACHE_LINE_SIZE);

	/* size of xlog buffer blocks */
	size = add_size(size, mul_size(block_count, XLOG_BLCKSZ));

	/* size of xlog buffer lock */
	size = add_size(size, mul_size(block_count, sizeof(polar_lwlock_mini_padded)));
	/* to allow aligning the above */
	size = add_size(size, PG_CACHE_LINE_SIZE);

	return size;
}

static void
xlog_buffer_precheck(XLogRecPtr cur_page_lsn, int len)
{
	static bool register_xlog_buffer_hook = false;

	if (unlikely(!register_xlog_buffer_hook))
	{
		on_proc_exit(polar_log_xlog_buffer_at_exit, 0);
		register_xlog_buffer_hook = true;
	}

	POLAR_ASSERT_PANIC((cur_page_lsn % XLOG_BLCKSZ) == 0 &&
					   len > 0 && len <= XLOG_BLCKSZ);

	/* Log hit ratio every 0x200000(16GB WAL) page lookup */
	if ((total_count & 0x1FFFFFL) == 0 && total_count > 0)
		POLAR_LOG_XLOG_BUFFER_STAT();
}

/*
 * POLAR: Lookup the buffer page using lsn.
 *
 * buf_id will be set when buffer matched or current buffer should be evicted.
 * When buffer should be evicted, buf_id will be set the id of evicted buffer,
 * and because requested page is not matched, so return false.
 *
 * If buffer is matched, return true, otherwise return false.
 * Force to recheck xlog buffer after switch lock from LW_SHARED to LW_EXCLUSIVE,
 * then the buffer may be matched and returned with LW_EXCLUSIVE.
 */
bool
polar_xlog_buffer_lookup(XLogRecPtr cur_page_lsn, int len, char *cur_page)
{
	polar_xlog_buffer_desc *buf;
	int			buf_id;
	bool		found = false;

	xlog_buffer_precheck(cur_page_lsn, len);

	total_count++;

	buf_id = polar_get_xlog_buffer_id(cur_page_lsn);
	Assert(buf_id >= 0);

	buf = polar_get_xlog_buffer_desc(buf_id);

	/* Obtain shared lock to check meta info */
	LWLockAcquire(polar_xlog_buffer_desc_get_lock(buf), LW_SHARED);

	if (buf->start_lsn == cur_page_lsn &&
		buf->end_lsn >= (cur_page_lsn + len - 1))
	{
		memcpy(cur_page, polar_get_xlog_buffer(buf_id), len);
		found = true;
	}

	LWLockRelease(polar_xlog_buffer_desc_get_lock(buf));

	if (found)
	{
		hit_count++;
		pg_atomic_fetch_add_u64(&polar_xlog_buffer_ins->hit_count, 1);
	}
	else
	{
		io_count++;
		pg_atomic_fetch_add_u64(&polar_xlog_buffer_ins->io_count, 1);
	}

	return found;
}

/*
 * POLAR: try to append one new xlog page after read it from disk.
 *
 * If xlog page is successful to appended, return true, otherwise return false.
 */
bool
polar_xlog_buffer_append(XLogRecPtr cur_page_lsn, int len, char *cur_page)
{
	polar_xlog_buffer_desc *buf;
	int			buf_id;

	xlog_buffer_precheck(cur_page_lsn, len);

	total_count++;

	buf_id = polar_get_xlog_buffer_id(cur_page_lsn);
	Assert(buf_id >= 0);
	buf = polar_get_xlog_buffer_desc(buf_id);

	/* Obtain shared lock to check meta info */
	LWLockAcquire(polar_xlog_buffer_desc_get_lock(buf), LW_SHARED);

	if ((buf->start_lsn == cur_page_lsn && buf->end_lsn >= (cur_page_lsn + len - 1)) ||
		!polar_xlog_buffer_should_evict(buf, cur_page_lsn, len))
	{
		LWLockRelease(polar_xlog_buffer_desc_get_lock(buf));
		return false;
	}

	LWLockRelease(polar_xlog_buffer_desc_get_lock(buf));
	LWLockAcquire(polar_xlog_buffer_desc_get_lock(buf), LW_EXCLUSIVE);

	/*
	 * check again to prevent xlog buffer from being updated by other
	 * processes
	 */
	if ((buf->start_lsn == cur_page_lsn && buf->end_lsn >= (cur_page_lsn + len - 1)) ||
		!polar_xlog_buffer_should_evict(buf, cur_page_lsn, len))
	{
		LWLockRelease(polar_xlog_buffer_desc_get_lock(buf));
		return false;
	}

	buf->start_lsn = cur_page_lsn;
	buf->end_lsn = cur_page_lsn + len - 1;
	memcpy(polar_get_xlog_buffer(buf_id), cur_page, len);
	LWLockRelease(polar_xlog_buffer_desc_get_lock(buf));

	if (AmStartupProcess())
	{
		startup_append_count++;
		pg_atomic_fetch_add_u64(&polar_xlog_buffer_ins->startup_append_count, 1);
	}
	else
	{
		others_append_count++;
		pg_atomic_fetch_add_u64(&polar_xlog_buffer_ins->others_append_count, 1);
	}

	return true;
}

/*
 * POLAR: Update xlog buffer page meta.
 *
 * It will update the end_lsn of the page containing this lsn to lsn.
 */
void
polar_xlog_buffer_update(XLogRecPtr lsn)
{
	XLogRecPtr	page_off = lsn - (lsn % XLOG_BLCKSZ);
	int			buf_id = polar_get_xlog_buffer_id(lsn);
	polar_xlog_buffer_desc *buf = NULL;

	Assert(buf_id >= 0);

	buf = polar_get_xlog_buffer_desc(buf_id);
	LWLockAcquire(polar_xlog_buffer_desc_get_lock(buf), LW_EXCLUSIVE);

	/*
	 * Ensure page buffer the expected one. While stream replication broken,
	 * xlog page may be read by twophase related logic. After startup replay
	 * all xlog at local storage, it will invalidation xlog buffer data
	 * related to last record. In some situation, this invalidation operation
	 * may enlarge xlog buffer size at page without more data filled, so it
	 * may cause zero data being read by other twophase related operation
	 * which will print ERROR log when cannot read record. So we add the check
	 * here to ensure updated lsn not larger than original buffer meta info,
	 * because this update func is only used while meet invalid xlog record.
	 */
	if (buf->start_lsn == page_off && buf->end_lsn > lsn)
		buf->end_lsn = lsn;

	LWLockRelease(polar_xlog_buffer_desc_get_lock(buf));
}

/*
 * POLAR: Remove page from xlog buffer.
 *
 * If buffer is io-progress, it do nothing.
 */
void
polar_xlog_buffer_remove(XLogRecPtr lsn)
{
	XLogRecPtr	page_off = lsn - (lsn % XLOG_BLCKSZ);
	int			buf_id = polar_get_xlog_buffer_id(lsn);
	polar_xlog_buffer_desc *buf = NULL;

	Assert(buf_id >= 0);

	buf = polar_get_xlog_buffer_desc(buf_id);
	LWLockAcquire(polar_xlog_buffer_desc_get_lock(buf), LW_EXCLUSIVE);

	/* Ensure page buffer the expected one */
	if (buf->start_lsn == page_off)
	{
		buf->start_lsn = InvalidXLogRecPtr;
		buf->end_lsn = InvalidXLogRecPtr;
	}

	LWLockRelease(polar_xlog_buffer_desc_get_lock(buf));
}
