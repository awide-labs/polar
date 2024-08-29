/*-------------------------------------------------------------------------
 *
 * test_buffer.c
 *	  Test module for buffer manager.
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
 *	  src/test/modules/test_buffer/test_buffer.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <time.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "access/xlog.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "postmaster/bgworker.h"
#include "postmaster/postmaster.h"
#include "storage/bufmgr.h"
#include "storage/ipc.h"
#include "storage/polar_bufmgr.h"
#include "storage/polar_copybuf.h"
#include "storage/polar_flush.h"
#include "storage/proc.h"
#include "utils/elog.h"
#include "utils/memutils.h"
#include "utils/ps_status.h"

PG_MODULE_MAGIC;

#define CHECK_BUFFER_BATCH	100
#define CHECK_BUFFER_COUNT	100

static bool
check_two_buffers(int front, int back)
{
	XLogRecPtr	front_lsn;
	XLogRecPtr	back_lsn;

	/* There are some buffers in flush list. */
	Assert(front >= 0);
	Assert(back >= 0);

	/* Oldest lsn should be valid. */
	front_lsn = polar_buffer_get_oldest_lsn(GetBufferDescriptor(front));
	back_lsn = polar_buffer_get_oldest_lsn(GetBufferDescriptor(back));
	Assert(!XLogRecPtrIsInvalid(front_lsn));
	Assert(!XLogRecPtrIsInvalid(back_lsn));

	/* back lsn should be less than front lsn. */
	return back_lsn >= front_lsn;
}

static void
check_one_batch_buffer()
{
	int			first,
				last,
				mid,
				check = 0;
	BufferDesc *mid_buf = NULL;
	BufferDesc *first_buf = NULL;

	/* Do not hold this lock too long. */
	SpinLockAcquire(&polar_flush_ctl->flushlist_lock);
	first = polar_flush_ctl->first_flush_buffer;
	last = polar_flush_ctl->last_flush_buffer;

	/* Flushlist is empty. */
	if (first == POLAR_FLUSHNEXT_END_OF_LIST)
	{
		Assert(last == POLAR_FLUSHNEXT_END_OF_LIST);
		SpinLockRelease(&polar_flush_ctl->flushlist_lock);
		return;
	}

	Assert(check_two_buffers(first, last));

	/* Check #CHECK_BUFFER_COUNT buffers in flush list. */
	mid = first_buf->flush_next;
	Assert(mid != POLAR_FLUSHNEXT_NOT_IN_LIST);
	while (mid != POLAR_FLUSHNEXT_END_OF_LIST &&
		   check <= CHECK_BUFFER_COUNT)
	{
		Assert(check_two_buffers(first, mid));
		Assert(check_two_buffers(mid, last));
		first = mid;
		mid_buf = GetBufferDescriptor(mid);
		mid = mid_buf->flush_next;
		check++;
	}

	SpinLockRelease(&polar_flush_ctl->flushlist_lock);
}

static void
check_consistent_lsn()
{
	XLogRecPtr	first_lsn;
	XLogRecPtr	consistent_lsn;
	int			first;

	consistent_lsn = polar_get_consistent_lsn();

	/* Do not hold this lock too much time. */
	SpinLockAcquire(&polar_flush_ctl->flushlist_lock);
	first = polar_flush_ctl->first_flush_buffer;

	/* Flushlist is empty. */
	if (first == POLAR_FLUSHNEXT_END_OF_LIST)
	{
		SpinLockRelease(&polar_flush_ctl->flushlist_lock);
		return;
	}

	first_lsn = polar_buffer_get_oldest_lsn(GetBufferDescriptor(first));
	SpinLockRelease(&polar_flush_ctl->flushlist_lock);

	Assert(!XLogRecPtrIsInvalid(first_lsn));
	/* Check consistent lsn. */
	if (!XLogRecPtrIsInvalid(consistent_lsn) && !XLogRecPtrIsInvalid(first_lsn))
		Assert(consistent_lsn <= first_lsn);
}

static void
check_some_buffers()
{
	int			batch = 0;

	while (batch <= CHECK_BUFFER_BATCH)
	{
		check_one_batch_buffer();
		batch++;
	}
}

static void
test_flushlist()
{
	if (!polar_flush_list_enabled())
		return;

	Assert(polar_flush_ctl != NULL);
	check_some_buffers();
	check_consistent_lsn();
}

static void
test_copybuffer()
{
	if (!polar_copy_buffer_enabled())
		return;

	Assert(polar_copy_buffer_ctl != NULL);
}

PG_FUNCTION_INFO_V1(test_buffer);
/*
 * SQL-callable entry point to perform all tests.
 *
 * If a 1% false positive threshold is not met, emits WARNINGs.
 *
 * See README for details of arguments.
 */
Datum
test_buffer(PG_FUNCTION_ARGS)
{
	if (!polar_enable_shared_storage_mode)
		PG_RETURN_VOID();

	test_flushlist();
	test_copybuffer();
	PG_RETURN_VOID();
}
