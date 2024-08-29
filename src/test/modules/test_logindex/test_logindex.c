/*-------------------------------------------------------------------------
 *
 * test_logindex.c
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
 *	  src/test/modules/test_logindex/test_logindex.c
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
#include "test_module_init.h"
#include "utils/elog.h"
#include "utils/memutils.h"
#include "utils/polar_bitpos.h"
#include "utils/ps_status.h"

#define LSN_TEST_STEP 100
#define TEST_MAX_BLOCK_NUMBER 10
static pid_t bgwriter_pid = 0;
static BackgroundWorkerHandle *bgwriter_handle;
static bool shutdown_requested = false;

static logindex_snapshot_t test_logindex_snapshot = NULL;
static XLogRecPtr test_max_lsn = InvalidXLogRecPtr;
static XLogRecPtr test_start_lsn[TEST_MAX_BLOCK_NUMBER];
static XLogRecPtr test_end_lsn[TEST_MAX_BLOCK_NUMBER];
static XLogRecPtr test_only_two[2];

void		test_logindex_bgwriter_main(Datum main_arg);

static void
test_insert_lsn_to_mem(log_index_snapshot_t * logindex_snapshot, BufferTag *tag, XLogRecPtr max_lsn, uint32 key)
{
	log_mem_table_t *table;
	log_item_head_t *item;
	log_item_seg_t *item_seg;
	int			i,
				j;
	XLogRecPtr	plsn = InvalidXLogRecPtr;
	log_index_lsn_t lsn_info;

	/* Test insert into log_item_head_t */
	for (i = 1; i <= LOG_INDEX_ITEM_HEAD_LSN_NUM; i++)
	{
		lsn_info.tag = tag;
		lsn_info.lsn = LSN_TEST_STEP * i + max_lsn;
		lsn_info.prev_lsn = plsn;
		log_index_insert_lsn(logindex_snapshot, &lsn_info, key);
		plsn = LSN_TEST_STEP * i + max_lsn;
	}

	table = LOG_INDEX_MEM_TBL_ACTIVE();

	Assert(LOG_INDEX_MEM_TBL_STATE(table) == LOG_INDEX_MEM_TBL_STATE_ACTIVE);

	item = log_index_tbl_find(tag, &table->data, key);

	Assert(item != NULL);
	Assert(item->number == LOG_INDEX_ITEM_HEAD_LSN_NUM);

	for (i = 1; i <= LOG_INDEX_ITEM_HEAD_LSN_NUM; i++)
		Assert(LOG_INDEX_COMBINE_LSN(&table->data, item->suffix_lsn[i - 1]) == LSN_TEST_STEP * i + max_lsn);

	Assert(LOG_INDEX_MEM_ITEM_IS(
								 log_index_item_head(&table->data,
													 LOG_INDEX_TBL_SLOT_VALUE(&table->data, key)),
								 tag));

	Assert(item->next_seg == LOG_INDEX_TBL_INVALID_SEG);

	/* Test insert log_item_seg_t after log_item_head_t */
	for (j = 0; j < LOG_INDEX_ITEM_SEG_LSN_NUM; i++, j++)
	{
		lsn_info.tag = tag;
		lsn_info.lsn = LSN_TEST_STEP * i + max_lsn;
		lsn_info.prev_lsn = plsn;
		log_index_insert_lsn(logindex_snapshot, &lsn_info, key);
		plsn = LSN_TEST_STEP * i + max_lsn;
	}

	Assert(item->next_seg != LOG_INDEX_TBL_INVALID_SEG);

	item_seg = log_index_item_seg(&table->data, item->next_seg);
	Assert(item_seg->number == LOG_INDEX_ITEM_SEG_LSN_NUM);
	Assert(item_seg->next_seg == LOG_INDEX_TBL_INVALID_SEG);

	for (j = 0, i = LOG_INDEX_ITEM_HEAD_LSN_NUM + 1;
		 j < LOG_INDEX_ITEM_SEG_LSN_NUM; j++, i++)
		Assert(LOG_INDEX_COMBINE_LSN(&table->data, item_seg->suffix_lsn[j]) == i * LSN_TEST_STEP + max_lsn);

	/* Test insert log_item_seg_t after log_item_seg_t */

	for (j = 0; j < LOG_INDEX_ITEM_SEG_LSN_NUM; i++, j++)
	{
		lsn_info.tag = tag;
		lsn_info.lsn = LSN_TEST_STEP * i + max_lsn;
		lsn_info.prev_lsn = plsn;
		log_index_insert_lsn(logindex_snapshot, &lsn_info, key);
		plsn = LSN_TEST_STEP * i + max_lsn;
	}

	test_max_lsn = LSN_TEST_STEP * (i - 1) + max_lsn;

	Assert(item_seg->next_seg != LOG_INDEX_TBL_INVALID_SEG);

	item_seg = log_index_item_seg(&table->data, item_seg->next_seg);

	Assert(item_seg->number == LOG_INDEX_ITEM_SEG_LSN_NUM);
	Assert(item_seg->next_seg == LOG_INDEX_TBL_INVALID_SEG);

	for (j = 0, i =
		 (LOG_INDEX_ITEM_HEAD_LSN_NUM + LOG_INDEX_ITEM_SEG_LSN_NUM) + 1;
		 j < LOG_INDEX_ITEM_HEAD_LSN_NUM; j++, i++)
		Assert(LOG_INDEX_COMBINE_LSN(&table->data, item_seg->suffix_lsn[j]) == i * LSN_TEST_STEP + max_lsn);
}

static void
test_insert_one_mem_table_full(log_index_snapshot_t * logindex_snapshot, BufferTag *tag)
{
	XLogRecPtr	start_lsn = test_max_lsn;
	int			i,
				total;
	struct timespec start_time,
				end_time;
	long		cost;
	uint32		tid;
	log_mem_table_t *table;
	XLogRecPtr	plsn = InvalidXLogRecPtr;

	total = LOG_INDEX_MEM_TBL_SEG_NUM * LOG_INDEX_ITEM_SEG_LSN_NUM;

	clock_gettime(CLOCK_MONOTONIC, &start_time);

	for (i = 1; i <= total; i++)
	{
		polar_logindex_add_lsn(logindex_snapshot, tag, plsn, LSN_TEST_STEP * i + start_lsn);
		plsn = LSN_TEST_STEP * i + start_lsn;
	}

	test_max_lsn = LSN_TEST_STEP * (i - 1) + start_lsn;

	start_lsn = test_max_lsn;

	clock_gettime(CLOCK_MONOTONIC, &end_time);

	cost = (end_time.tv_sec - start_time.tv_sec) * 1000000000 +
		(end_time.tv_nsec - start_time.tv_nsec);

	ereport(LOG, (errmsg(
						 "insert %d ,cost %ld, qps=%ld", total, cost,
						 total * (1000000000 / cost))));


	sleep(1);
	tid = LOG_INDEX_MEM_TBL_ACTIVE_ID;
	table = LOG_INDEX_MEM_TBL(LOG_INDEX_MEM_TBL_PREV_ID(tid));
	Assert(LOG_INDEX_MEM_TBL_FREE_HEAD(table)
		   == LOG_INDEX_MEM_TBL_SEG_NUM);
	ereport(LOG, (errmsg("mem tbl state %d", LOG_INDEX_MEM_TBL_STATE(table))));
	Assert(LOG_INDEX_MEM_TBL_STATE(table)
		   == LOG_INDEX_MEM_TBL_STATE_FLUSHED);
	Assert(LOG_INDEX_MEM_TBL_TID(table) == 1);
	table = LOG_INDEX_MEM_TBL(tid);
	Assert(LOG_INDEX_MEM_TBL_STATE(table)
		   == LOG_INDEX_MEM_TBL_STATE_ACTIVE);
	Assert(LOG_INDEX_MEM_TBL_TID(table) == 2);

	table = LOG_INDEX_MEM_TBL(LOG_INDEX_MEM_TBL_NEXT_ID(tid));
	Assert(LOG_INDEX_MEM_TBL_STATE(table)
		   == LOG_INDEX_MEM_TBL_STATE_FREE);
	Assert(LOG_INDEX_MEM_TBL_TID(table)
		   == LOG_INDEX_TABLE_INVALID_ID);

	for (i = 1; i <= total; i++)
	{
		polar_logindex_add_lsn(logindex_snapshot, tag, plsn, LSN_TEST_STEP * i + start_lsn);
		plsn = LSN_TEST_STEP * i + start_lsn;
	}

	test_max_lsn = LSN_TEST_STEP * (i - 1) + start_lsn;
}

static void
test_lsn_iterator(log_index_snapshot_t * logindex_snapshot, log_index_lsn_iter_t iter, XLogRecPtr start, XLogRecPtr end)
{
	XLogRecPtr	i;
	log_index_lsn_t *lsn_info;

	for (i = start; i <= end; i += LSN_TEST_STEP)
	{
		lsn_info = polar_logindex_lsn_iterator_next(logindex_snapshot, iter);
		Assert(lsn_info != NULL);
		Assert(lsn_info->lsn == i);
	}
}

static void
test_lsn_iterate_from_invalid(log_index_snapshot_t * logindex_snapshot)
{
	log_index_lsn_iter_t lsn_iter = polar_logindex_create_lsn_iterator(logindex_snapshot, InvalidXLogRecPtr);

	test_lsn_iterator(logindex_snapshot, lsn_iter, LSN_TEST_STEP, test_max_lsn);

	polar_logindex_release_lsn_iterator(lsn_iter);

}

static void
test_lsn_iterate_from_super_max(log_index_snapshot_t * logindex_snapshot)
{
	log_index_lsn_t *lsn_info;

	log_index_lsn_iter_t lsn_iter = polar_logindex_create_lsn_iterator(logindex_snapshot, test_max_lsn + 1);

	lsn_info = polar_logindex_lsn_iterator_next(logindex_snapshot, lsn_iter);
	Assert(lsn_info == NULL);

	polar_logindex_release_lsn_iterator(lsn_iter);

}

static void
test_lsn_iterate_from_table_interval(log_index_snapshot_t * logindex_snapshot)
{
	log_index_lsn_t *lsn_info;
	BufferTag	tag;
	XLogRecPtr	start_lsn = InvalidXLogRecPtr,
				plsn = InvalidXLogRecPtr;
	int			i;
	uint32		total = LOG_INDEX_MEM_TBL_SEG_NUM * LOG_INDEX_ITEM_SEG_LSN_NUM;
	log_mem_table_t *prev_tbl;
	log_index_lsn_iter_t lsn_iter;

	tag.rnode.spcNode = 10;
	tag.rnode.dbNode = 11;
	tag.rnode.relNode = 12;
	tag.forkNum = MAIN_FORKNUM;

	tag.blockNum = 8;

	/* insert one table */
	for (i = 1; i <= total; i++)
	{
		polar_logindex_add_lsn(logindex_snapshot, &tag, plsn, LSN_TEST_STEP * i + test_max_lsn);
		plsn = LSN_TEST_STEP * i + test_max_lsn;
	}

	test_max_lsn += (LSN_TEST_STEP * (i - 1));

	prev_tbl = LOG_INDEX_MEM_TBL(LOG_INDEX_MEM_TBL_PREV_ID(LOG_INDEX_MEM_TBL_ACTIVE_ID));
	start_lsn = prev_tbl->data.max_lsn;

	lsn_iter = polar_logindex_create_lsn_iterator(logindex_snapshot, start_lsn + (LSN_TEST_STEP / 2));
	lsn_info = polar_logindex_lsn_iterator_next(logindex_snapshot, lsn_iter);
	Assert(lsn_info != NULL);
	Assert(lsn_info->lsn == start_lsn + LSN_TEST_STEP);
}

static void
test_lsn_iterate_from_file(log_index_snapshot_t * logindex_snapshot, XLogRecPtr start)
{
	log_index_lsn_iter_t lsn_iter = polar_logindex_create_lsn_iterator(logindex_snapshot, start);

	test_lsn_iterator(logindex_snapshot, lsn_iter, start, test_max_lsn);

	polar_logindex_release_lsn_iterator(lsn_iter);
}

static void
test_insert_lsn_after_force_flush(log_index_snapshot_t * logindex_snapshot)
{
	BufferTag	tag;
	uint32		total = LOG_INDEX_MEM_TBL_SEG_NUM * LOG_INDEX_ITEM_SEG_LSN_NUM;
	log_index_lsn_iter_t lsn_iter = polar_logindex_create_lsn_iterator(logindex_snapshot, test_max_lsn + LSN_TEST_STEP);
	XLogRecPtr	plsn = InvalidXLogRecPtr;
	log_mem_table_t *table = LOG_INDEX_MEM_TBL_ACTIVE();
	log_mem_table_t *next_table;
	XLogRecPtr	start_lsn = test_max_lsn + LSN_TEST_STEP;
	uint32		i;

	tag.rnode.spcNode = 10;
	tag.rnode.dbNode = 11;
	tag.rnode.relNode = 12;
	tag.forkNum = MAIN_FORKNUM;
	tag.blockNum = 100;


	for (i = 1; i <= total; i++)
	{
		polar_logindex_add_lsn(logindex_snapshot, &tag, plsn, LSN_TEST_STEP * i + test_max_lsn);
		plsn = LSN_TEST_STEP * i + test_max_lsn;
	}

	test_max_lsn += (LSN_TEST_STEP * (i - 1));
	next_table = LOG_INDEX_MEM_TBL_ACTIVE();

	Assert((LOG_INDEX_MEM_TBL_TID(next_table) - LOG_INDEX_MEM_TBL_TID(table)) == 1);
	test_lsn_iterator(logindex_snapshot, lsn_iter, start_lsn, test_max_lsn);

	polar_logindex_release_lsn_iterator(lsn_iter);
}

#define MAX_FULL_FILE_TEST 5
static XLogRecPtr test_full_lsn[MAX_FULL_FILE_TEST];

static void
test_insert_file_full(log_index_snapshot_t * logindex_snapshot)
{
	BufferTag	tag;
	uint32		total = LOG_INDEX_MEM_TBL_SEG_NUM * LOG_INDEX_ITEM_SEG_LSN_NUM;
	int			i,
				j,
				k;
	XLogRecPtr	plsn = InvalidXLogRecPtr;
	struct timespec start_time,
				end_time;
	long		cost;

	tag.rnode.spcNode = 10;
	tag.rnode.dbNode = 11;
	tag.rnode.relNode = 12;
	tag.forkNum = MAIN_FORKNUM;
	tag.blockNum = 100;

	clock_gettime(CLOCK_MONOTONIC, &start_time);

	for (k = 0; k < MAX_FULL_FILE_TEST; k++)
	{
		for (j = 0; j < LOG_INDEX_TABLE_NUM_PER_FILE; j++)
		{
			for (i = 1; i <= total; i++)
			{
				polar_logindex_add_lsn(logindex_snapshot, &tag, plsn, LSN_TEST_STEP * i + test_max_lsn);
				plsn = LSN_TEST_STEP * i + test_max_lsn;
			}

			test_max_lsn += (LSN_TEST_STEP * (i - 1));
		}

		test_full_lsn[k] = test_max_lsn;
	}

	clock_gettime(CLOCK_MONOTONIC, &end_time);
	cost = (end_time.tv_sec - start_time.tv_sec) * 1000000000 +
		(end_time.tv_nsec - start_time.tv_nsec);

	ereport(LOG, (errmsg("Insert full file test, cost=%.2lf,qps=%.2lf", cost / 1000000000.0,
						 MAX_FULL_FILE_TEST * LOG_INDEX_TABLE_NUM_PER_FILE * total
						 / (cost / 1000000000.0))));
}

static void
test_insert_lsn(log_index_snapshot_t * logindex_snapshot)
{
	BufferTag	tag;
	log_item_head_t *item1,
			   *item9;
	log_item_head_t *item;
	log_mem_table_t *table = LOG_INDEX_MEM_TBL_ACTIVE();
	uint32		key;
	log_index_lsn_t lsn_info;
	log_index_lsn_iter_t lsn_iter = polar_logindex_create_lsn_iterator(logindex_snapshot, test_max_lsn + LSN_TEST_STEP);

	tag.rnode.spcNode = 10;
	tag.rnode.dbNode = 11;
	tag.rnode.relNode = 12;
	tag.forkNum = MAIN_FORKNUM;

	Assert(lsn_iter != NULL);

	test_start_lsn[1] = test_max_lsn;
	tag.blockNum = 1;
	test_insert_lsn_to_mem(logindex_snapshot, &tag, test_max_lsn, LOG_INDEX_MEM_TBL_HASH_PAGE(&tag));
	test_end_lsn[1] = test_max_lsn;

	test_lsn_iterator(logindex_snapshot, lsn_iter, test_start_lsn[1] + LSN_TEST_STEP, test_end_lsn[1]);

	tag.blockNum = 2;
	test_start_lsn[2] = test_max_lsn;
	test_insert_lsn_to_mem(logindex_snapshot, &tag, test_max_lsn, LOG_INDEX_MEM_TBL_HASH_PAGE(&tag));
	test_end_lsn[2] = test_max_lsn;

	test_lsn_iterator(logindex_snapshot, lsn_iter, test_start_lsn[2] + LSN_TEST_STEP, test_end_lsn[2]);
	/* Test hash conflict */
	tag.blockNum = 1;
	key = LOG_INDEX_MEM_TBL_HASH_PAGE(&tag);
	tag.blockNum = 9;
	test_start_lsn[9] = test_max_lsn;
	test_insert_lsn_to_mem(logindex_snapshot, &tag, test_max_lsn, key);
	test_end_lsn[9] = test_max_lsn;
	test_lsn_iterator(logindex_snapshot, lsn_iter, test_start_lsn[9] + LSN_TEST_STEP, test_end_lsn[9]);

	tag.blockNum = 1;
	Assert(log_index_tbl_find(&tag, &(LOG_INDEX_MEM_TBL_ACTIVE()->data),
							  LOG_INDEX_MEM_TBL_HASH_PAGE(&tag))
		   != NULL);

	tag.blockNum = 2;
	Assert(log_index_tbl_find(&tag, &(LOG_INDEX_MEM_TBL_ACTIVE()->data),
							  LOG_INDEX_MEM_TBL_HASH_PAGE(&tag))
		   != NULL);


	/* Test find non-exists item */
	tag.blockNum = 3;
	Assert(log_index_tbl_find(&tag, &(LOG_INDEX_MEM_TBL_ACTIVE()->data),
							  LOG_INDEX_MEM_TBL_HASH_PAGE(&tag))
		   == NULL);


	/* Test hash conflict */
	tag.blockNum = 1;
	key = LOG_INDEX_MEM_TBL_HASH_PAGE(&tag);
	item1 = log_index_tbl_find(&tag, &table->data, key);
	tag.blockNum = 9;
	item9 = log_index_tbl_find(&tag, &table->data, key);

	Assert(item1->next_item == LOG_INDEX_TBL_INVALID_SEG);
	item = log_index_item_head(&table->data, item9->next_item);
	Assert(item == item1);
	item = log_index_item_head(&table->data, LOG_INDEX_TBL_SLOT_VALUE(&table->data, key));
	Assert(item == item9);

	/* Test insert only one lsn */
	tag.rnode.spcNode = 9;
	tag.blockNum = 3;

	lsn_info.tag = &tag;
	lsn_info.lsn = test_max_lsn + LSN_TEST_STEP;
	lsn_info.prev_lsn = InvalidXLogRecPtr;
	log_index_insert_lsn(logindex_snapshot, &lsn_info, LOG_INDEX_MEM_TBL_HASH_PAGE(&tag));

	test_only_two[0] = test_max_lsn + LSN_TEST_STEP;
	test_max_lsn += LSN_TEST_STEP;

	test_lsn_iterator(logindex_snapshot, lsn_iter, test_only_two[0], test_only_two[0]);
	/* Test insert one mem table full */
	tag.rnode.spcNode = 10;
	tag.blockNum = 3;

	{
		XLogRecPtr	start = test_max_lsn;

		test_insert_one_mem_table_full(logindex_snapshot, &tag);
		test_lsn_iterator(logindex_snapshot, lsn_iter, start + LSN_TEST_STEP, test_max_lsn);
	}

	/* Test insert the other one lsn */
	tag.rnode.spcNode = 9;
	tag.blockNum = 3;
	lsn_info.lsn = test_max_lsn + LSN_TEST_STEP;
	lsn_info.prev_lsn = test_only_two[0];
	log_index_insert_lsn(logindex_snapshot, &lsn_info, LOG_INDEX_MEM_TBL_HASH_PAGE(&tag));

	test_only_two[1] = test_max_lsn + LSN_TEST_STEP;
	test_lsn_iterator(logindex_snapshot, lsn_iter, test_only_two[1], test_only_two[1]);
	polar_logindex_release_lsn_iterator(lsn_iter);

	test_max_lsn += LSN_TEST_STEP;
}

static void
test_logindex_sigterm(SIGNAL_ARGS)
{
	int			save_errno = errno;

	shutdown_requested = true;
	SetLatch(MyLatch);
	errno = save_errno;
}

static void
test_iterate_non_exists_block_lsn(log_index_snapshot_t * logindex_snapshot, BlockNumber block)
{
	log_index_page_iter_t iter;
	BufferTag	tag;

	tag.rnode.spcNode = 10;
	tag.rnode.dbNode = 11;
	tag.rnode.relNode = 12;
	tag.forkNum = MAIN_FORKNUM;
	tag.blockNum = block;

	iter = polar_logindex_create_page_iterator(logindex_snapshot, &tag,
											   LSN_TEST_STEP, test_max_lsn, false);

	Assert(iter->state == ITERATE_STATE_FINISHED);

	Assert(iter != NULL);
	Assert(polar_logindex_page_iterator_end(iter));
	Assert(polar_logindex_page_iterator_next(iter) == NULL);
	polar_logindex_release_page_iterator(iter);
}


static void
test_iterate_in_table0(log_index_snapshot_t * logindex_snapshot)
{
	BufferTag	tag;
	log_index_page_iter_t iter;
	int			i = LSN_TEST_STEP;
	XLogRecPtr	min_lsn = LSN_TEST_STEP;
	XLogRecPtr	max_lsn =
		(LOG_INDEX_ITEM_SEG_LSN_NUM * 2 + LOG_INDEX_ITEM_HEAD_LSN_NUM) * LSN_TEST_STEP;
	log_index_lsn_t *lsn_info;

	max_lsn = LSN_TEST_STEP * 2;

	/*
	 * In this test case we first push lsn in set [min_lsn, max_lsn] Create
	 * iterator to get this set
	 */
	tag.rnode.spcNode = 10;
	tag.rnode.dbNode = 11;
	tag.rnode.relNode = 12;
	tag.forkNum = MAIN_FORKNUM;
	tag.blockNum = 1;

	iter = polar_logindex_create_page_iterator(logindex_snapshot, &tag, min_lsn, max_lsn, false);

	Assert(iter->state == ITERATE_STATE_FINISHED);

	while ((lsn_info = polar_logindex_page_iterator_next(iter)) != NULL)
	{
		if (i == LSN_TEST_STEP)
			Assert(lsn_info->prev_lsn == InvalidXLogRecPtr);
		else
			Assert(lsn_info->prev_lsn == i - LSN_TEST_STEP);

		Assert(lsn_info->lsn == i);
		i += LSN_TEST_STEP;
	}

	Assert(i == max_lsn + LSN_TEST_STEP);
	polar_logindex_release_page_iterator(iter);
}

static void
test_iterate_release(log_index_snapshot_t * logindex_snapshot)
{
	BufferTag	tag;
	log_index_page_iter_t iter;

	XLogRecPtr	min_lsn = LSN_TEST_STEP;
	XLogRecPtr	max_lsn =
		(LOG_INDEX_ITEM_SEG_LSN_NUM * 2 + LOG_INDEX_ITEM_HEAD_LSN_NUM) * LSN_TEST_STEP;

	tag.rnode.spcNode = 10;
	tag.rnode.dbNode = 11;
	tag.rnode.relNode = 12;
	tag.forkNum = MAIN_FORKNUM;
	tag.blockNum = 1;

	iter = polar_logindex_create_page_iterator(logindex_snapshot, &tag, min_lsn, max_lsn, false);

	Assert(iter->state == ITERATE_STATE_FINISHED);
	Assert(polar_logindex_page_iterator_end(iter) == false);
	polar_logindex_release_page_iterator(iter);
}

static void
test_iterate_only_two_lsn(log_index_snapshot_t * logindex_snapshot)
{
	BufferTag	tag;
	log_index_page_iter_t iter;
	log_index_lsn_t *lsn_info;

	tag.rnode.spcNode = 9;
	tag.rnode.dbNode = 11;
	tag.rnode.relNode = 12;
	tag.forkNum = MAIN_FORKNUM;
	tag.blockNum = 3;


	iter = polar_logindex_create_page_iterator(logindex_snapshot, &tag, test_only_two[0], test_only_two[1], false);
	Assert(iter->state == ITERATE_STATE_FINISHED);
	Assert((lsn_info = polar_logindex_page_iterator_next(iter)) != NULL);
	Assert(lsn_info->lsn == test_only_two[0]);
	Assert(lsn_info->prev_lsn == InvalidXLogRecPtr);
	Assert((lsn_info = polar_logindex_page_iterator_next(iter)) != NULL);
	Assert(lsn_info->lsn == test_only_two[1]);
	Assert(lsn_info->prev_lsn == test_only_two[0]);
	polar_logindex_release_page_iterator(iter);

	iter = polar_logindex_create_page_iterator(logindex_snapshot, &tag, test_only_two[0] - 1, test_only_two[0] + 1, false);
	Assert(iter->state == ITERATE_STATE_FINISHED);
	Assert((lsn_info = polar_logindex_page_iterator_next(iter)) != NULL);
	Assert(lsn_info->lsn == test_only_two[0]);
	Assert(lsn_info->prev_lsn == InvalidXLogRecPtr);
	polar_logindex_release_page_iterator(iter);

	iter = polar_logindex_create_page_iterator(logindex_snapshot, &tag, test_only_two[1] - 1, test_only_two[1] + 1, false);
	Assert(iter->state == ITERATE_STATE_FINISHED);
	Assert((lsn_info = polar_logindex_page_iterator_next(iter)) != NULL);
	Assert(lsn_info->lsn == test_only_two[1]);
	Assert(lsn_info->prev_lsn == test_only_two[0]);
	polar_logindex_release_page_iterator(iter);
}


static void
test_iterate_lsn(log_index_snapshot_t * logindex_snapshot)
{
	test_iterate_only_two_lsn(logindex_snapshot);

	test_iterate_non_exists_block_lsn(logindex_snapshot, 999);
	test_iterate_in_table0(logindex_snapshot);

	test_iterate_release(logindex_snapshot);
}

static void
test_save_memtable(log_index_snapshot_t * logindex_snapshot)
{
	sleep(1);
	Assert(logindex_snapshot->meta.max_idx_table_id > 1);

	/*
	 * Test iterate lsn from saved file
	 */
	test_iterate_in_table0(logindex_snapshot);
}

static void
test_force_flush_table(log_index_snapshot_t * logindex_snapshot)
{
	uint32		mid = LOG_INDEX_MEM_TBL_ACTIVE_ID;
	log_mem_table_t *table = LOG_INDEX_MEM_TBL(mid);

	/* Wait bgwriter to flush inactive table */
	sleep(1);

	Assert(LOG_INDEX_MEM_TBL_STATE(table) == LOG_INDEX_MEM_TBL_STATE_ACTIVE);

	log_index_force_save_table(logindex_snapshot, table);
	table->data.max_lsn = InvalidXLogRecPtr;
	table->data.min_lsn = UINT64_MAX;
	table->free_head = 1;
}

static void
test_check_saved(log_index_snapshot_t * logindex_snapshot, BufferTag *tag)
{
	log_mem_table_t *table;

	/* Add LSN which is already exists */
	polar_logindex_add_lsn(logindex_snapshot, tag, InvalidXLogRecPtr, test_max_lsn);

	table = LOG_INDEX_MEM_TBL_ACTIVE();

	Assert(LOG_INDEX_MEM_TBL_STATE(table) == LOG_INDEX_MEM_TBL_STATE_FREE ||
		   LOG_INDEX_MEM_TBL_IS_NEW(table));

	tag->blockNum++;
	polar_logindex_add_lsn(logindex_snapshot, tag, InvalidXLogRecPtr, test_max_lsn);

	Assert(LOG_INDEX_MEM_TBL_STATE(table) == LOG_INDEX_MEM_TBL_STATE_ACTIVE);
	Assert(LOG_INDEX_MEM_TBL_FREE_HEAD(table) > 1);
}

static void
test_iterate_after_truncate(log_index_snapshot_t * logindex_snapshot, XLogRecPtr start_lsn)
{
	BufferTag	tag;
	log_index_page_iter_t iter;
	log_index_lsn_t *lsn_info;
	XLogRecPtr	i = start_lsn;

	tag.rnode.spcNode = 10;
	tag.rnode.dbNode = 11;
	tag.rnode.relNode = 12;
	tag.forkNum = MAIN_FORKNUM;
	tag.blockNum = 100;

	iter = polar_logindex_create_page_iterator(logindex_snapshot, &tag, start_lsn, test_max_lsn, false);
	Assert(iter->state == ITERATE_STATE_FINISHED);

	while ((lsn_info = polar_logindex_page_iterator_next(iter)) != NULL)
	{
		Assert(lsn_info->lsn == i);
		i += LSN_TEST_STEP;
	}

	Assert(i == test_max_lsn + LSN_TEST_STEP);
	polar_logindex_release_page_iterator(iter);
}

static void
test_truncate_log(log_index_snapshot_t * logindex_snapshot)
{
	int			i;
	uint64		max_seg_no = LOG_INDEX_FILE_TABLE_SEGMENT_NO(logindex_snapshot->meta.max_idx_table_id);
	char		path[MAXPGPATH];

	for (i = 0; i < MAX_FULL_FILE_TEST - 1; i++)
	{
		uint64		seg_no = logindex_snapshot->meta.min_segment_info.segment_no;
		int			fd;

		polar_logindex_truncate(logindex_snapshot, test_full_lsn[i]);

		Assert(logindex_snapshot->meta.min_segment_info.segment_no - seg_no == 1);

		test_lsn_iterate_from_file(logindex_snapshot, test_full_lsn[i]);
		test_iterate_after_truncate(logindex_snapshot, test_full_lsn[i]);

		LOG_INDEX_FILE_TABLE_NAME(path, max_seg_no + i + 1);

		fd = polar_open(path, O_RDONLY, 0);
		Assert(fd > 0);
		polar_close(fd);
	}
}

static void
test_change_lsn_prefix(log_index_snapshot_t * logindex_snapshot)
{
	log_mem_table_t *table,
			   *next_table;
	BufferTag	tag;
	XLogRecPtr	start_lsn,
				end_lsn;
	log_index_lsn_iter_t lsn_iter;

	tag.rnode.spcNode = 100;
	tag.rnode.dbNode = 11;
	tag.rnode.relNode = 12;
	tag.forkNum = MAIN_FORKNUM;
	tag.blockNum = 100;

	table = LOG_INDEX_MEM_TBL(LOG_INDEX_MEM_TBL_ACTIVE_ID);
	start_lsn = (((test_max_lsn >> 32) + 1) << 32);

	test_insert_lsn_to_mem(logindex_snapshot, &tag, start_lsn, LOG_INDEX_MEM_TBL_HASH_PAGE(&tag));
	end_lsn = test_max_lsn;

	next_table = LOG_INDEX_MEM_TBL(LOG_INDEX_MEM_TBL_ACTIVE_ID);

	lsn_iter = polar_logindex_create_lsn_iterator(logindex_snapshot, start_lsn + LSN_TEST_STEP);

	Assert(lsn_iter != NULL);
	Assert(next_table->data.idx_table_id == table->data.idx_table_id + 1);

	test_lsn_iterator(logindex_snapshot, lsn_iter, start_lsn + LSN_TEST_STEP, end_lsn);

	polar_logindex_release_lsn_iterator(lsn_iter);
}

static bool
test_logindex_table_flushable(log_mem_table_t * table, void *data)
{
	return true;
}

void
test_logindex_bgwriter_main(Datum main_arg)
{
	logindex_snapshot_t logindex_snapshot;

	pqsignal(SIGTERM, test_logindex_sigterm);

	logindex_snapshot = polar_logindex_snapshot_shmem_init("test_logindex_snapshot", 3, 8, LWTRANCHE_WAL_LOGINDEX_BEGIN, LWTRANCHE_WAL_LOGINDEX_END, test_logindex_table_flushable, NULL);
	/* We're now ready to receive signals */
	BackgroundWorkerUnblockSignals();

	elog(LOG, "start logindex_bgwriter test");

	while (!shutdown_requested)
	{
		WaitLatch(&MyProc->procLatch,
				  WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
				  100, PG_WAIT_EXTENSION);

		/* Reset the latch, bail out if postmaster died, otherwise loop. */
		ResetLatch(&MyProc->procLatch);

		polar_logindex_bg_write(logindex_snapshot);
	}

	proc_exit(0);
}

static void
test_logindex_bgwriter(void)
{
	BackgroundWorker worker;

	/* set up common data for all our workers */
	memset(&worker, 0, sizeof(worker));
	worker.bgw_flags = BGWORKER_SHMEM_ACCESS |
		BGWORKER_BACKEND_DATABASE_CONNECTION;
	worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
	worker.bgw_restart_time = BGW_NEVER_RESTART;
	sprintf(worker.bgw_library_name, "test_logindex");
	sprintf(worker.bgw_function_name, "test_logindex_bgwriter_main");
	snprintf(worker.bgw_name, BGW_MAXLEN, "test_logindex_bgwriter");
	snprintf(worker.bgw_type, BGW_MAXLEN, "test_logindex_bgwriter");
	worker.bgw_notify_pid = MyProcPid;

	Assert(RegisterDynamicBackgroundWorker(&worker, &bgwriter_handle));

	Assert(WaitForBackgroundWorkerStartup(bgwriter_handle, &bgwriter_pid) == BGWH_STARTED);
}

static void
test_force_flush_full_table(logindex_snapshot_t logindex_snapshot, BufferTag *tag)
{
	log_mem_table_t *table;
	LWLock	   *lock;

	tag->rnode.spcNode = 13;
	tag->rnode.dbNode = 11;
	tag->rnode.relNode = 12;
	tag->forkNum = MAIN_FORKNUM;
	tag->blockNum = 1;

	table = LOG_INDEX_MEM_TBL_ACTIVE();
	lock = LOG_INDEX_MEM_TBL_LOCK(table);

	LWLockAcquire(lock, LW_EXCLUSIVE);

	while (!LOG_INDEX_MEM_TBL_FULL(table))
	{
		test_max_lsn += LSN_TEST_STEP;
		tag->blockNum++;
		polar_logindex_add_lsn(logindex_snapshot, tag, InvalidXLogRecPtr, test_max_lsn);
	}

	LOG_INDEX_MEM_TBL_SET_STATE(table, LOG_INDEX_MEM_TBL_STATE_INACTIVE);
	log_index_force_save_table(logindex_snapshot, table);
	LWLockRelease(lock);
}

PG_FUNCTION_INFO_V1(test_logindex);
/*
 * SQL-callable entry point to perform all tests.
 *
 * If a 1% false positive threshold is not met, emits WARNINGs.
 *
 * See README for details of arguments.
 */
Datum
test_logindex(PG_FUNCTION_ARGS)
{
	XLogRecPtr	start_lsn;
	logindex_snapshot_t logindex_snapshot = test_logindex_snapshot;

	Assert(logindex_snapshot != NULL);
	test_logindex_bgwriter();

	MemSet(test_start_lsn, 0, sizeof(XLogRecPtr) * TEST_MAX_BLOCK_NUMBER);
	MemSet(test_end_lsn, 0, sizeof(XLogRecPtr) * TEST_MAX_BLOCK_NUMBER);
	pg_atomic_init_u32(&logindex_snapshot->state, 0);

	polar_logindex_snapshot_init(logindex_snapshot, LSN_TEST_STEP, 1, false, false);
	polar_logindex_set_start_lsn(logindex_snapshot, LSN_TEST_STEP);

	test_insert_lsn(logindex_snapshot);
	test_lsn_iterate_from_file(logindex_snapshot, LSN_TEST_STEP * 2);
	test_lsn_iterate_from_invalid(logindex_snapshot);
	test_lsn_iterate_from_super_max(logindex_snapshot);
	test_lsn_iterate_from_table_interval(logindex_snapshot);
	test_iterate_lsn(logindex_snapshot);
	test_save_memtable(logindex_snapshot);
	test_lsn_iterate_from_file(logindex_snapshot, LSN_TEST_STEP * 2);
	test_lsn_iterate_from_invalid(logindex_snapshot);
	test_force_flush_table(logindex_snapshot);

	polar_load_logindex_snapshot_from_storage(logindex_snapshot, LSN_TEST_STEP);

	test_lsn_iterate_from_file(logindex_snapshot, LSN_TEST_STEP * 2);
	{
		BufferTag	tag;

		test_force_flush_full_table(logindex_snapshot, &tag);

		pg_atomic_init_u32(&logindex_snapshot->state, 0);
		polar_logindex_snapshot_init(logindex_snapshot, LSN_TEST_STEP, 1, false, false);
		test_check_saved(logindex_snapshot, &tag);
	}

	start_lsn = test_max_lsn + LSN_TEST_STEP;
	test_insert_lsn_after_force_flush(logindex_snapshot);

	test_insert_file_full(logindex_snapshot);
	test_iterate_lsn(logindex_snapshot);
	test_lsn_iterate_from_file(logindex_snapshot, start_lsn);

	test_truncate_log(logindex_snapshot);

	test_change_lsn_prefix(logindex_snapshot);

	kill(bgwriter_pid, SIGTERM);
	Assert(WaitForBackgroundWorkerShutdown(bgwriter_handle) == BGWH_STOPPED);

	MemoryContextResetAndDeleteChildren(polar_logindex_memory_context());

	PG_RETURN_INT32(0);
}

Size
test_logindex_request_shmem_size(void)
{
	Size		sz = polar_logindex_redo_shmem_size();

	return sz;
}

void
test_logindex_shmem_startup(void)
{
	Assert(test_logindex_snapshot == NULL);
	test_logindex_snapshot =
		polar_logindex_snapshot_shmem_init("test_logindex_snapshot", 3, 8, LWTRANCHE_WAL_LOGINDEX_BEGIN, LWTRANCHE_WAL_LOGINDEX_END, test_logindex_table_flushable, NULL);
	Assert(test_logindex_snapshot);
}
