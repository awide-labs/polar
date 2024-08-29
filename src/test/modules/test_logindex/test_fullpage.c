/*-------------------------------------------------------------------------
 *
 * test_fullpage.c
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
 *	  src/test/modules/test_logindex/test_fullpage.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <time.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "access/heapam.h"
#include "access/polar_logindex.h"
#include "access/polar_logindex_internal.h"
#include "access/polar_logindex_redo.h"
#include "catalog/pg_control.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "postmaster/postmaster.h"
#include "pgstat.h"
#include "storage/ipc.h"
#include "storage/proc.h"
#include "utils/elog.h"
#include "utils/memutils.h"
#include "utils/polar_bitpos.h"
#include "utils/ps_status.h"

#include "test_module_init.h"

static void test_write_fullpage(polar_fullpage_ctl_t ctl);

static void
test_flush_fullpage_table(polar_fullpage_ctl_t ctl)
{
	logindex_snapshot_t logindex_snapshot = ctl->logindex_snapshot;
	log_mem_table_t *active = LOG_INDEX_MEM_TBL_ACTIVE();
	log_idx_table_data_t table;

	/* Wait for fullpage write to logindex */
	sleep(1);

	polar_logindex_flush_table(logindex_snapshot, InvalidXLogRecPtr, true);
	elog(LOG, "Read table which tid=%ld", LOG_INDEX_MEM_TBL_TID(active));

	log_index_read_table_data(logindex_snapshot, &table, LOG_INDEX_MEM_TBL_TID(active), LOG);
	Assert(table.idx_table_id == LOG_INDEX_MEM_TBL_TID(active));
	Assert(table.max_lsn == polar_get_logindex_snapshot_max_lsn(logindex_snapshot));
}

static void
test_fullpage_segment_file(polar_fullpage_ctl_t ctl)
{
	char		path[MAXPGPATH] = {0};
	uint64		min_fullpage_seg_no = 0;
	struct stat statbuf;
	int			i = 0;

	polar_fullpage_file_init(ctl, 10000000);
	polar_fullpage_file_init(ctl, 20000000);
	polar_fullpage_file_init(ctl, 30000000);
	polar_fullpage_file_init(ctl, 40000000);

	min_fullpage_seg_no = FULLPAGE_FILE_SEG_NO(10000000);
	FULLPAGE_SEG_FILE_NAME(ctl, path, min_fullpage_seg_no);
	polar_remove_old_fullpage_file(ctl, path, min_fullpage_seg_no);
	Assert(polar_lstat(path, &statbuf) != 0);

	min_fullpage_seg_no = FULLPAGE_FILE_SEG_NO(20000000);
	FULLPAGE_SEG_FILE_NAME(ctl, path, min_fullpage_seg_no);
	polar_remove_old_fullpage_file(ctl, path, min_fullpage_seg_no);
	Assert(polar_lstat(path, &statbuf) != 0);

	min_fullpage_seg_no = FULLPAGE_FILE_SEG_NO(30000000);
	FULLPAGE_SEG_FILE_NAME(ctl, path, min_fullpage_seg_no);
	polar_remove_old_fullpage_file(ctl, path, min_fullpage_seg_no);
	Assert(polar_lstat(path, &statbuf) != 0);

	min_fullpage_seg_no = FULLPAGE_FILE_SEG_NO(40000000);
	FULLPAGE_SEG_FILE_NAME(ctl, path, min_fullpage_seg_no);
	polar_remove_old_fullpage_file(ctl, path, min_fullpage_seg_no);
	Assert(polar_lstat(path, &statbuf) != 0);

	polar_update_max_fullpage_no(ctl, 500000000);

	for (i = 0; i < 10000; i++)
		test_write_fullpage(ctl);
}

static void
test_write_fullpage(polar_fullpage_ctl_t ctl)
{
	Relation	rel;
	Buffer		buf;
	XLogRecPtr	start_lsn;
	XLogRecPtr	end_lsn;
	static XLogReaderState *state = NULL;
	uint64		fullpage_no;
	Page		page;

	/* Open relation and check privileges. */
	rel = relation_open(1260, AccessShareLock);

	/* pg_authid block_no=0 */
	buf = ReadBufferExtended(rel, MAIN_FORKNUM, 0, RBM_NORMAL, NULL);

	end_lsn = polar_log_fullpage_snapshot_image(ctl, buf, InvalidXLogRecPtr);

	ReleaseBuffer(buf);

	start_lsn = ProcLastRecPtr;

	/* Close relation, release lock. */
	relation_close(rel, AccessShareLock);

	XLogFlush(end_lsn);

	if (state == NULL)
	{
		state = XLogReaderAllocate(wal_segment_size, NULL,
								   XL_ROUTINE(.page_read = &read_local_xlog_page,
											  .segment_open = wal_segment_open,
											  .segment_close = wal_segment_close),
								   NULL);

		if (!state)
		{
			ereport(FATAL,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory"),
					 errdetail("Failed while allocating a WAL reading processor.")));
		}
	}

	polar_logindex_read_xlog(state, start_lsn);

	Assert(state->record->header.xl_rmid == RM_XLOG_ID);
	Assert((state->record->header.xl_info & ~XLR_INFO_MASK) == POLAR_WAL);
	Assert(*((PolarWalType *) XLogRecGetData(state)) == PWT_FPSI);

	page = palloc0(BLCKSZ);
	/* get fullpage_no from record */
	memcpy(&fullpage_no, XLogRecGetData(state) + sizeof(PolarWalType), sizeof(uint64));
	/* read fullpage from file */
	polar_read_fullpage(ctl, page, fullpage_no);

	Assert(memcmp((char *) BufferGetPage(buf), (char *) page, BLCKSZ) == 0);
}

PG_FUNCTION_INFO_V1(test_fullpage);
/*
 * SQL-callable entry point to perform all tests.
 *
 * If a 1% false positive threshold is not met, emits WARNINGs.
 *
 * See README for details of arguments.
 */
Datum
test_fullpage(PG_FUNCTION_ARGS)
{

	polar_fullpage_ctl_t ctl;
	logindex_snapshot_t logindex_snapshot;

	Assert(polar_logindex_redo_instance->wal_logindex_snapshot->flush_active_table == false);
	Assert(polar_logindex_redo_instance->fullpage_logindex_snapshot->flush_active_table == true);

	ctl = polar_logindex_redo_instance->fullpage_ctl;
	logindex_snapshot = polar_logindex_redo_instance->fullpage_logindex_snapshot;

	Assert(ctl != NULL && ctl->logindex_snapshot != NULL);

	pg_atomic_init_u32(&logindex_snapshot->state, 0);

	polar_logindex_snapshot_init(logindex_snapshot, 1000, 1, false, false);
	polar_logindex_set_start_lsn(logindex_snapshot, 1000);

	polar_logindex_redo_instance->fullpage_ctl = ctl;
	polar_logindex_redo_instance->fullpage_logindex_snapshot = logindex_snapshot;

	/* test fullpage */
	test_write_fullpage(ctl);
	test_write_fullpage(ctl);
	test_write_fullpage(ctl);

	/* test flush active table */
	test_flush_fullpage_table(ctl);

	/* test fullpage segment file */
	test_fullpage_segment_file(ctl);

	MemoryContextResetAndDeleteChildren(polar_logindex_memory_context());

	PG_RETURN_INT32(0);
}
