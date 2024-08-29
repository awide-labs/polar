/*-------------------------------------------------------------------------
 *
 * test_polar_rel_size_cache.c
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
 *	  src/test/modules/test_logindex/test_polar_rel_size_cache.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>

#include "access/polar_rel_size_cache.h"
#include "access/transam.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/polar_fd.h"
#include "test_module_init.h"

#define MAX_BLOCKS (2)

static void
test_rel_exists(polar_rel_size_cache_t cache)
{
	RelFileNode node;
	BufferTag	tag;
	bool		valid;
	XLogRecPtr	lsn_changed;

	node.spcNode = 1;
	node.dbNode = 100;
	node.relNode = 200;

	tag.rnode = node;
	tag.forkNum = MAIN_FORKNUM;
	tag.blockNum = 0;

	/* It should be valid if we did not add information for this relation */
	valid = polar_check_rel_block_valid_only(cache, 0x4000, &tag);
	Assert(valid);

	/* The block is invalid if it's dropped */
	polar_record_db_state_with_lock(cache, 0x5000, node.spcNode, node.dbNode, POLAR_DB_DROPED);
	valid = polar_check_rel_block_valid_and_lsn(cache, 0x4000, &tag, &lsn_changed);
	Assert(!valid);
	Assert(lsn_changed == 0x5000);

	/* The block is valid if it's new created db */
	polar_record_db_state_with_lock(cache, 0x5100, node.spcNode, node.dbNode, POLAR_DB_NEW);

	tag.blockNum = 99;
	valid = polar_check_rel_block_valid_and_lsn(cache, 0x5110, &tag, &lsn_changed);
	Assert(valid);
	Assert(lsn_changed == 0x5110);

	tag.blockNum = 98;
	valid = polar_check_rel_block_valid_and_lsn(cache, 0x5100, &tag, &lsn_changed);
	Assert(valid);
	Assert(lsn_changed == 0x5100);

	/* The block is invalid if it's truncated */
	polar_record_rel_size_with_lock(cache, 0x5200, &node, MAIN_FORKNUM, 100);
	tag.blockNum = 200;
	valid = polar_check_rel_block_valid_and_lsn(cache, 0x5110, &tag, &lsn_changed);
	Assert(!valid);
	Assert(lsn_changed == 0x5200);

	/*
	 * The block is valid if we didn't add infomation for this relation after
	 * 0x6000
	 */
	valid = polar_check_rel_block_valid_and_lsn(cache, 0x6000, &tag, &lsn_changed);
	Assert(valid);
	Assert(lsn_changed == 0x6000);

	valid = polar_check_rel_block_valid_and_lsn(cache, 0x5110, &tag, &lsn_changed);
	Assert(!valid);
	Assert(lsn_changed == 0x5200);

	node.dbNode = 1000;
	tag.rnode = node;
	polar_record_db_state_with_lock(cache, 0x6100, node.spcNode, node.dbNode, POLAR_DB_NEW);
	valid = polar_check_rel_block_valid_and_lsn(cache, 0x6110, &tag, &lsn_changed);
	Assert(valid);
	Assert(lsn_changed == 0x6110);


	valid = polar_check_rel_block_valid_and_lsn(cache, 0x6100, &tag, &lsn_changed);
	Assert(valid);
	Assert(lsn_changed == 0x6100);

	polar_record_rel_size_with_lock(cache, 0x6200, &node, MAIN_FORKNUM, 100);

	tag.blockNum = 99;
	valid = polar_check_rel_block_valid_and_lsn(cache, 0x6100, &tag, &lsn_changed);
	Assert(valid);
	Assert(lsn_changed == 0x6100);

	tag.blockNum = 100;
	valid = polar_check_rel_block_valid_and_lsn(cache, 0x6100, &tag, &lsn_changed);
	Assert(!valid);
	Assert(lsn_changed == 0x6200);
}

static void
test_rel_size_table_full(polar_rel_size_cache_t cache)
{
	int			total = REL_INFO_TOTAL_SIZE / sizeof(polar_relation_size_t);
	RelFileNode node;
	int			i;
	XLogRecPtr	lsn = 0x7000,
				lsn_changed;
	BlockNumber size = 100000,
				size101,
				size102;
	XLogRecPtr	max_tid2_lsn;
	BufferTag	tag;
	bool		valid;

	extern int	polar_rel_size_cache_blocks;

	node.spcNode = 1;
	node.dbNode = 101;
	node.relNode = 200;

	tag.rnode = node;
	tag.forkNum = MAIN_FORKNUM;

	/* Write table full, which tid=1 */
	for (i = 1; i <= total; i++)
	{
		lsn += i * 10;
		size -= i;
		polar_record_rel_size_with_lock(cache, lsn, &node, MAIN_FORKNUM, size);
	}

	tag.blockNum = size - 1;
	valid = polar_check_rel_block_valid_and_lsn(cache, 0x4000, &tag, &lsn_changed);
	Assert(valid);
	Assert(lsn_changed == 0x4000);

	tag.blockNum = size;
	valid = polar_check_rel_block_valid_and_lsn(cache, 0x4000, &tag, &lsn_changed);
	Assert(!valid);
	Assert(lsn_changed == lsn);
	size101 = size;
	max_tid2_lsn = lsn;

	node.dbNode = 100;
	tag.rnode = node;
	tag.blockNum = 200;

	valid = polar_check_rel_block_valid_and_lsn(cache, 0x5110, &tag, &lsn_changed);
	Assert(!valid);
	Assert(lsn_changed == 0x5200);

	node.dbNode = 102;
	tag.rnode = node;

	size = 200000;

	/* Write table full and tid=1 and tid=2 should be flushed */
	for (i = 1; i <= (total * polar_rel_size_cache_blocks); i++)
	{
		lsn += i * 10;
		size -= i;
		polar_record_rel_size_with_lock(cache, lsn, &node, MAIN_FORKNUM, size);
	}

	size102 = size;
	tag.blockNum = size - 1;
	valid = polar_check_rel_block_valid_and_lsn(cache, 0x4000, &tag, &lsn_changed);
	Assert(valid);

	tag.blockNum = size;
	valid = polar_check_rel_block_valid_and_lsn(cache, 0x4000, &tag, &lsn_changed);
	Assert(!valid);
	Assert(lsn_changed == lsn);

	node.dbNode = 101;
	tag.rnode = node;
	tag.blockNum = size101 - 1;
	valid = polar_check_rel_block_valid_only(cache, 0x4000, &tag);
	Assert(valid);

	tag.blockNum = size101;
	valid = polar_check_rel_block_valid_and_lsn(cache, 0x4000, &tag, &lsn_changed);
	Assert(!valid);
	Assert(lsn_changed == max_tid2_lsn);

	node.dbNode = 100;
	tag.rnode = node;
	tag.blockNum = 200;

	valid = polar_check_rel_block_valid_and_lsn(cache, 0x5110, &tag, &lsn_changed);
	Assert(!valid);
	Assert(lsn_changed == 0x5200);

	/* Keep table which tid=2 */
	polar_truncate_rel_size_cache(cache, max_tid2_lsn - 1);

	valid = polar_check_rel_block_valid_only(cache, 0x4000, &tag);
	Assert(valid);

	node.dbNode = 101;
	tag.rnode = node;
	tag.blockNum = size101;
	valid = polar_check_rel_block_valid_and_lsn(cache, 0x4000, &tag, &lsn_changed);
	Assert(!valid);
	Assert(lsn_changed == max_tid2_lsn);

	polar_truncate_rel_size_cache(cache, max_tid2_lsn - 1);
	node.dbNode = 102;
	tag.rnode = node;
	tag.blockNum = size102;

	valid = polar_check_rel_block_valid_and_lsn(cache, 0x4000, &tag, &lsn_changed);
	Assert(!valid);
	Assert(lsn_changed == lsn);

	tag.blockNum = size102 - 1;
	valid = polar_check_rel_block_valid_and_lsn(cache, 0x4000, &tag, &lsn_changed);
	Assert(valid);
}

PG_FUNCTION_INFO_V1(test_polar_rel_size_cache);
/*
 * SQL-callable entry point to perform all tests.
 *
 * If a 1% false positive threshold is not met, emits WARNINGs.
 *
 * See README for details of arguments.
 */
Datum
test_polar_rel_size_cache(PG_FUNCTION_ARGS)
{
	polar_rel_size_cache_t cache;

	cache = polar_rel_size_shmem_init("test_rel_cache", MAX_BLOCKS);
	Assert(cache != NULL);

	test_rel_exists(cache);
	test_rel_size_table_full(cache);

	PG_RETURN_VOID();
}

Size
test_rel_size_cache_request_shmem_size(void)
{
	return polar_rel_size_shmem_size(MAX_BLOCKS);
}

void
test_rel_size_cache_shmem_startup(void)
{
	polar_rel_size_cache_t cache;

	Assert(polar_rel_size_shmem_init("test_rel_cache", 0) == NULL);
	cache = polar_rel_size_shmem_init("test_rel_cache", MAX_BLOCKS);
	Assert(cache);
}
