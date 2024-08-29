/*-------------------------------------------------------------------------
 *
 * test_xlog_buffer.c
 *	  unit test code for xlog buffer
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
 *	  src/test/modules/test_xlog_buffer/test_xlog_buffer.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <time.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "access/polar_logindex_redo.h"
#include "access/xlog.h"
#include "access/xlogreader.h"
#include "access/xlogutils.h"
#include "executor/spi.h"
#include "tcop/tcopprot.h"
#include "utils/snapmgr.h"
#include "storage/polar_xlogbuf.h"

static shmem_request_hook_type prev_shmem_request_hook = NULL;
static polar_xlog_buffer_ctl prev_xlog_buffer_ins = NULL;
static uint64 hit = 0;
static uint64 io = 0;
static uint64 append = 0;
static uint64 startup_append = 0;

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(test_xlog_buffer);

void		_PG_init(void);

static void
test_xlog_buffer_shmem_request(void)
{
	if (prev_shmem_request_hook)
		prev_shmem_request_hook();
	RequestAddinShmemSpace(polar_xlog_buffer_shmem_size());
}

void
_PG_init(void)
{
	if (!process_shared_preload_libraries_in_progress)
		ereport(FATAL,
				errmsg("test_xlog_buffer module should be in shared_preload_libraries."));

	EmitWarningsOnPlaceholders("test_xlog_buffer");

	polar_enable_xlog_buffer_test = true;
	prev_shmem_request_hook = shmem_request_hook;
	shmem_request_hook = test_xlog_buffer_shmem_request;
}

static void
check_xlog_buffer_stat_info(void)
{
	if (pg_atomic_read_u64(&polar_xlog_buffer_ins->hit_count) != hit ||
		pg_atomic_read_u64(&polar_xlog_buffer_ins->io_count) != io ||
		pg_atomic_read_u64(&polar_xlog_buffer_ins->others_append_count) != append ||
		pg_atomic_read_u64(&polar_xlog_buffer_ins->startup_append_count) != startup_append)
	{
		ereport(PANIC,
				errmsg("Unexpected xlog buffer stat info: (%ld, %ld, %ld, %ld) vs (%ld, %ld, %ld, %ld)",
					   pg_atomic_read_u64(&polar_xlog_buffer_ins->hit_count),
					   pg_atomic_read_u64(&polar_xlog_buffer_ins->io_count),
					   pg_atomic_read_u64(&polar_xlog_buffer_ins->others_append_count),
					   pg_atomic_read_u64(&polar_xlog_buffer_ins->startup_append_count),
					   hit, io, append, startup_append));
	}
}

#define XLOG_BLOCK_VALID_LEN (101)

static void
test_xlog_buffer_api(void)
{
	XLogRecPtr	lsn;
	char	   *one_xlog_block = NULL;
	char		cur_page[XLOG_BLCKSZ];

	one_xlog_block = palloc0(XLOG_BLCKSZ);
	Assert(one_xlog_block != NULL);
	memset(one_xlog_block, 'A', XLOG_BLOCK_VALID_LEN);

	/* insert xlog buffer and make it full */
	for (lsn = 0L; lsn < 64 * 1024 * 1024; lsn += XLOG_BLCKSZ)
	{
		if (!polar_xlog_buffer_append(lsn, XLOG_BLOCK_VALID_LEN, one_xlog_block))
			ereport(PANIC, errmsg("Failed to append xlog page for %X/%X, %d!",
								  LSN_FORMAT_ARGS(lsn), XLOG_BLOCK_VALID_LEN));
		append++;
	}
	check_xlog_buffer_stat_info();

	/* replace all xlog buffer blocks with higher valid length */
	memset(one_xlog_block, 'B', 2 * XLOG_BLOCK_VALID_LEN);
	for (lsn = 0L; lsn < 64 * 1024 * 1024; lsn += XLOG_BLCKSZ)
	{
		if (polar_xlog_buffer_lookup(lsn, 2 * XLOG_BLOCK_VALID_LEN, cur_page))
			ereport(PANIC, errmsg("Unexpected found!"));
		io++;
		if (!polar_xlog_buffer_append(lsn, 2 * XLOG_BLOCK_VALID_LEN, one_xlog_block))
			ereport(PANIC, errmsg("Failed to append!"));
		append++;
	}
	check_xlog_buffer_stat_info();

	/* search all xlog buffer blocks */
	for (lsn = 0L; lsn < 64 * 1024 * 1024; lsn += XLOG_BLCKSZ)
	{
		if (!polar_xlog_buffer_lookup(lsn, XLOG_BLOCK_VALID_LEN, cur_page))
			ereport(PANIC, errmsg("Unexpected unfound!"));

		if (memcmp(cur_page, one_xlog_block, XLOG_BLOCK_VALID_LEN) != 0)
			ereport(PANIC, errmsg("Unexpected xlog page in xlog buffer!"));
		hit++;
	}
	check_xlog_buffer_stat_info();

	/* insert xlog buffer with higher lsn and make it full */
	memset(one_xlog_block, 'C', XLOG_BLOCK_VALID_LEN);
	for (lsn = 64 * 1024 * 1024; lsn < 2 * 64 * 1024 * 1024; lsn += XLOG_BLCKSZ)
	{
		if (!polar_xlog_buffer_append(lsn, XLOG_BLOCK_VALID_LEN, one_xlog_block))
			ereport(PANIC, errmsg("Failed to append xlog page for %X/%X, %d!",
								  LSN_FORMAT_ARGS(lsn), XLOG_BLOCK_VALID_LEN));
		append++;
	}
	check_xlog_buffer_stat_info();

	/* search all xlog buffer blocks again, but no one will be matched */
	for (lsn = 0L; lsn < 64 * 1024 * 1024; lsn += XLOG_BLCKSZ)
	{
		if (polar_xlog_buffer_lookup(lsn, XLOG_BLOCK_VALID_LEN, cur_page))
			ereport(PANIC, errmsg("Unexpected found!"));
		io++;
	}
	check_xlog_buffer_stat_info();

	/* failed to update xlog buffer valid length */
	polar_xlog_buffer_update(2L);
	if (polar_xlog_buffer_lookup(0L, 1, cur_page))
		ereport(PANIC, errmsg("Unexpected found!"));
	io++;

	polar_xlog_buffer_update(64 * 1024 * 1024 + XLOG_BLOCK_VALID_LEN + 2);
	if (polar_xlog_buffer_lookup(64 * 1024 * 1024, XLOG_BLOCK_VALID_LEN + 1, cur_page))
		ereport(PANIC, errmsg("Unexpected found!"));
	io++;

	check_xlog_buffer_stat_info();

	/* succ to update xlog buffer valid length */
	/* try to replace all xlog buffer blocks, but no one will be done */
	for (lsn = 64 * 1024 * 1024; lsn < 2 * 64 * 1024 * 1024; lsn += XLOG_BLCKSZ)
	{
		polar_xlog_buffer_update(lsn + XLOG_BLOCK_VALID_LEN - 2);
		if (polar_xlog_buffer_lookup(lsn, XLOG_BLOCK_VALID_LEN, cur_page))
			ereport(PANIC, errmsg("Unexpected found!"));
		io++;

		if (!polar_xlog_buffer_lookup(lsn, XLOG_BLOCK_VALID_LEN - 1, cur_page))
			ereport(PANIC, errmsg("Unexpected unfound!"));
		Assert(cur_page[0] == 'C');
		hit++;
	}
	check_xlog_buffer_stat_info();

	/* remove all xlog buffer blocks */
	polar_xlog_buffer_remove_range(64 * 1024 * 1024, 2 * 64 * 1024 * 1024);
	for (lsn = 64 * 1024 * 1024; lsn < 2 * 64 * 1024 * 1024; lsn += XLOG_BLCKSZ)
	{
		if (polar_xlog_buffer_lookup(lsn, 1, cur_page))
			ereport(PANIC, errmsg("Unexpected found!"));
		io++;
	}
	check_xlog_buffer_stat_info();
}

static void
spi_execute_sql(char *sql)
{
	bool		old_XactReadOnly = XactReadOnly;
	int			ret;
	StringInfoData buf;

	XactReadOnly = false;

	SetCurrentStatementStartTimestamp();
	StartTransactionCommand();
	SPI_connect();
	PushActiveSnapshot(GetTransactionSnapshot());

	initStringInfo(&buf);
	appendStringInfo(&buf, "%s", sql);

	debug_query_string = buf.data;
	ret = SPI_execute(buf.data, false, 0);
	if (ret != SPI_OK_UTILITY && ret != SPI_OK_INSERT)
		ereport(FATAL,
				errmsg("SPI_execute failed: error code %d for '%s'",
					   ret, buf.data));

	debug_query_string = NULL;
	pfree(buf.data);

	SPI_finish();
	PopActiveSnapshot();
	CommitTransactionCommand();

	XactReadOnly = old_XactReadOnly;
}

static void
test_xlog_read_record(XLogReaderState *xlogreader, XLogRecPtr flush_lsn, bool with_append)
{
	XLogRecPtr	prev_lsn = InvalidXLogRecPtr;
	char	   *error_msg = NULL;
	XLogRecord *record;

	do
	{
		record = XLogReadRecord(xlogreader, &error_msg);
		if (record != NULL)
		{
			if (prev_lsn / XLOG_BLCKSZ != xlogreader->ReadRecPtr / XLOG_BLCKSZ)
			{
				io++;
				if (with_append)
					append++;
				prev_lsn = xlogreader->ReadRecPtr;
			}
			if (xlogreader->ReadRecPtr / XLOG_BLCKSZ != xlogreader->EndRecPtr / XLOG_BLCKSZ)
			{
				io++;
				if (with_append)
					append++;
				prev_lsn = xlogreader->EndRecPtr;
			}
		}
	} while (record);

	Assert(((ReadLocalXLogPageNoWaitPrivate *) xlogreader->private_data)->end_of_wal);
	Assert(xlogreader->EndRecPtr == flush_lsn);
}

static void
test_read_record(void)
{
	int			buf_id;
	XLogReaderState *xlogreader;
	XLogRecPtr	start_lsn = GetFlushRecPtr(NULL);
	XLogRecPtr	flush_lsn = InvalidXLogRecPtr;
	ReadLocalXLogPageNoWaitPrivate *private_data;

	spi_execute_sql("CREATE TABLE test_xlog_buffer(id int);");
	do
	{
		spi_execute_sql("INSERT INTO test_xlog_buffer SELECT generate_series(0, 8192);");
		XLogFlush(XactLastRecEnd);
		LWLockAcquire(WALWriteLock, LW_EXCLUSIVE);
		flush_lsn = GetFlushRecPtr(NULL);
		if (flush_lsn % XLOG_BLCKSZ > 0)
			break;
		pg_usleep(1000000L);
	} while (true);

	Assert(polar_logindex_redo_instance == NULL);
	polar_logindex_redo_instance = palloc0(sizeof(polar_logindex_redo_ctl_data_t));
	polar_set_bg_redo_state(polar_logindex_redo_instance, POLAR_BG_ONLINE_PROMOTE);
	polar_set_node_type(POLAR_STANDBY);

	private_data = (ReadLocalXLogPageNoWaitPrivate *)
		palloc0(sizeof(ReadLocalXLogPageNoWaitPrivate));

	/* hold write lock and read the tail xlog block into xlog buffer */
	xlogreader = XLogReaderAllocate(wal_segment_size, NULL,
									XL_ROUTINE(.page_read = &read_local_xlog_page_no_wait,
											   .segment_open = &wal_segment_open,
											   .segment_close = &wal_segment_close),
									private_data);

	if (!xlogreader)
	{
		ereport(FATAL,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory"),
				 errdetail("Failed while allocating a WAL reading processor.")));
	}

	check_xlog_buffer_stat_info();

	/* first loop of XLogReadRecord with invalid bg_replayed_lsn */
	Assert(polar_bg_redo_get_replayed_lsn(polar_logindex_redo_instance) == InvalidXLogRecPtr);
	XLogBeginRead(xlogreader, start_lsn);
	/* the first page of segment file woule be read to varify header */
	io++;
	test_xlog_read_record(xlogreader, flush_lsn, false);
	buf_id = polar_get_xlog_buffer_id(xlogreader->EndRecPtr);
	Assert(polar_xlog_buffer_ins->buffer_descriptors[buf_id].desc.start_lsn == InvalidXLogRecPtr);
	Assert(polar_xlog_buffer_ins->buffer_descriptors[buf_id].desc.end_lsn == InvalidXLogRecPtr);

	/* second loop of XLogReadRecord with max bg_replayed_lsn */
	polar_bg_redo_set_replayed_lsn(polar_logindex_redo_instance, flush_lsn);
	XLogBeginRead(xlogreader, start_lsn);
	io++;
	append++;
	test_xlog_read_record(xlogreader, flush_lsn, true);
	buf_id = polar_get_xlog_buffer_id(xlogreader->EndRecPtr);
	Assert(polar_xlog_buffer_ins->buffer_descriptors[buf_id].desc.start_lsn == flush_lsn - (flush_lsn % XLOG_BLCKSZ));
	Assert(polar_xlog_buffer_ins->buffer_descriptors[buf_id].desc.end_lsn == flush_lsn - 1);

	check_xlog_buffer_stat_info();

	polar_set_node_type(POLAR_PRIMARY);
	pfree(polar_logindex_redo_instance);
	polar_logindex_redo_instance = NULL;
	LWLockRelease(WALWriteLock);

	/* insert more wal records and check read record */
	start_lsn = flush_lsn;
	spi_execute_sql("INSERT INTO test_xlog_buffer SELECT generate_series(0, 8192);");
	XLogFlush(XactLastRecEnd);
	LWLockAcquire(WALWriteLock, LW_EXCLUSIVE);
	flush_lsn = GetFlushRecPtr(NULL);

	Assert(polar_logindex_redo_instance == NULL);
	polar_logindex_redo_instance = palloc0(sizeof(polar_logindex_redo_ctl_data_t));
	polar_set_bg_redo_state(polar_logindex_redo_instance, POLAR_BG_ONLINE_PROMOTE);
	polar_set_node_type(POLAR_STANDBY);

	/*
	 * The first page of segment file woule be read to varify header after
	 * failed and it woule be hited in xlog buffer.
	 */
	hit++;
	polar_bg_redo_set_replayed_lsn(polar_logindex_redo_instance, flush_lsn);
	test_xlog_read_record(xlogreader, flush_lsn, true);
	buf_id = polar_get_xlog_buffer_id(xlogreader->EndRecPtr);
	Assert(polar_xlog_buffer_ins->buffer_descriptors[buf_id].desc.start_lsn == flush_lsn - (flush_lsn % XLOG_BLCKSZ));
	Assert(polar_xlog_buffer_ins->buffer_descriptors[buf_id].desc.end_lsn == flush_lsn - 1);

	check_xlog_buffer_stat_info();

	polar_set_node_type(POLAR_PRIMARY);
	pfree(polar_logindex_redo_instance);
	polar_logindex_redo_instance = NULL;
	LWLockRelease(WALWriteLock);

}

/*
 * SQL-callable entry point to perform all tests.
 *
 * If a 1% false positive threshold is not met, emits WARNINGs.
 *
 * See README for details of arguments.
 */
Datum
test_xlog_buffer(PG_FUNCTION_ARGS)
{
	Assert(polar_xlog_page_buffers == 64);
	prev_xlog_buffer_ins = polar_xlog_buffer_ins;
	IsUnderPostmaster = false;
	polar_init_xlog_buffer("test xlog buffer");
	IsUnderPostmaster = true;

	test_xlog_buffer_api();
	test_read_record();

	polar_xlog_buffer_ins = prev_xlog_buffer_ins;
	PG_RETURN_VOID();
}
