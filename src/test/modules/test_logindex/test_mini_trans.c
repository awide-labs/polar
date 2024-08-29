/*-------------------------------------------------------------------------
 *
 * test_mini_trans.c
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
 *	  src/test/modules/test_logindex/test_mini_trans.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/polar_mini_transaction.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "storage/buf_internals.h"
#include "test_module_init.h"
#include "utils/polar_bitpos.h"

static mini_trans_t test_trans = NULL;

static void
test_mini_trans_page(mini_trans_t trans, BufferTag *tag, XLogRecPtr lsn)
{
	uint32		h;
	XLogRecPtr	plsn;
	mini_trans_info_t *info = trans->info;

	Assert(polar_logindex_mini_trans_find(trans, tag) == InvalidXLogRecPtr);
	Assert(polar_logindex_mini_trans_cond_lock(trans, tag, LW_EXCLUSIVE, &plsn)
		   == 0);

	h = polar_logindex_mini_trans_lock(trans, tag, LW_EXCLUSIVE, &plsn);
	Assert(h > 0);
	Assert(POLAR_BIT_IS_OCCUPIED(trans->occupied, h));
	Assert(pg_atomic_read_u32(&info[h - 1].refcount) == 1);
	Assert(plsn == lsn);
	polar_logindex_mini_trans_unlock(trans, h);
	Assert(POLAR_BIT_IS_OCCUPIED(trans->occupied, h));
	Assert(pg_atomic_read_u32(&info[h - 1].refcount) == 0);

	Assert(polar_logindex_mini_trans_find(trans, tag) == lsn);
	Assert(POLAR_BIT_IS_OCCUPIED(trans->occupied, h));
	Assert(pg_atomic_read_u32(&info[h - 1].refcount) == 0);

	plsn = InvalidXLogRecPtr;
	Assert(polar_logindex_mini_trans_lock(trans, tag, LW_SHARED, &plsn) == h);
	Assert(plsn == lsn);

	Assert(POLAR_BIT_IS_OCCUPIED(trans->occupied, h));
	Assert(pg_atomic_read_u32(&info[h - 1].refcount) == 1);
	polar_logindex_mini_trans_unlock(trans, h);
	Assert(POLAR_BIT_IS_OCCUPIED(trans->occupied, h));
	Assert(pg_atomic_read_u32(&info[h - 1].refcount) == 0);
	plsn = InvalidXLogRecPtr;

	Assert(polar_logindex_mini_trans_cond_lock(trans, tag, LW_EXCLUSIVE, &plsn)
		   == h);
	Assert(plsn == lsn);

	Assert(POLAR_BIT_IS_OCCUPIED(trans->occupied, h));
	Assert(pg_atomic_read_u32(&info[h - 1].refcount) == 1);
	polar_logindex_mini_trans_unlock(trans, h);
	Assert(POLAR_BIT_IS_OCCUPIED(trans->occupied, h));
	Assert(pg_atomic_read_u32(&info[h - 1].refcount) == 0);
}

static void
test_mini_trans_hash_conflict(mini_trans_t trans, BufferTag *tag, uint32 key, XLogRecPtr lsn)
{
	uint32		h;
	XLogRecPtr	plsn;
	mini_trans_info_t *info = trans->info;

	Assert(polar_logindex_mini_trans_find(trans, tag) == InvalidXLogRecPtr);
	Assert(polar_logindex_mini_trans_cond_lock(trans, tag, LW_EXCLUSIVE, &plsn)
		   == 0);

	h = polar_logindex_mini_trans_key_lock(trans, tag, key, LW_EXCLUSIVE, &plsn);
	Assert(h > 0);
	Assert(POLAR_BIT_IS_OCCUPIED(trans->occupied, h));
	Assert(pg_atomic_read_u32(&info[h - 1].refcount) == 1);
	Assert(plsn == lsn);
	polar_logindex_mini_trans_unlock(trans, h);
	Assert(POLAR_BIT_IS_OCCUPIED(trans->occupied, h));
	Assert(pg_atomic_read_u32(&info[h - 1].refcount) == 0);

	Assert(polar_logindex_mini_trans_key_find(trans, tag, key) == lsn);
	Assert(POLAR_BIT_IS_OCCUPIED(trans->occupied, h));
	Assert(pg_atomic_read_u32(&info[h - 1].refcount) == 0);

	plsn = InvalidXLogRecPtr;
	Assert(polar_logindex_mini_trans_key_lock(trans, tag, key, LW_SHARED, &plsn) == h);
	Assert(plsn == lsn);

	Assert(POLAR_BIT_IS_OCCUPIED(trans->occupied, h));
	Assert(pg_atomic_read_u32(&info[h - 1].refcount) == 1);
	polar_logindex_mini_trans_unlock(trans, h);
	Assert(POLAR_BIT_IS_OCCUPIED(trans->occupied, h));
	Assert(pg_atomic_read_u32(&info[h - 1].refcount) == 0);
	plsn = InvalidXLogRecPtr;

	Assert(polar_logindex_mini_trans_cond_key_lock(trans, tag, key, LW_EXCLUSIVE, &plsn)
		   == h);
	Assert(plsn == lsn);

	Assert(POLAR_BIT_IS_OCCUPIED(trans->occupied, h));
	Assert(pg_atomic_read_u32(&info[h - 1].refcount) == 1);
	polar_logindex_mini_trans_unlock(trans, h);
	Assert(POLAR_BIT_IS_OCCUPIED(trans->occupied, h));
	Assert(pg_atomic_read_u32(&info[h - 1].refcount) == 0);
}

PG_FUNCTION_INFO_V1(test_mini_trans);

Datum
test_mini_trans(PG_FUNCTION_ARGS)
{
	BufferTag	tag;
	XLogRecPtr	lsn = 1000,
				plsn;
	uint32		key;
	uint32		i;
	mini_trans_t trans = test_trans;

	Assert(trans != NULL);

	Assert(polar_logindex_mini_trans_start(trans, lsn) == 0);

	tag.rnode.spcNode = 10;
	tag.rnode.dbNode = 11;
	tag.rnode.relNode = 12;
	tag.forkNum = MAIN_FORKNUM;
	tag.blockNum = 1;

	test_mini_trans_page(trans, &tag, lsn);
	tag.blockNum = 2;
	test_mini_trans_page(trans, &tag, lsn);
	tag.blockNum = 3;
	test_mini_trans_page(trans, &tag, lsn);
	key = MINI_TRANSACTION_HASH_PAGE(&tag);
	tag.blockNum = 4;

	test_mini_trans_hash_conflict(trans, &tag, key, lsn);
	tag.blockNum = 5;

	test_mini_trans_hash_conflict(trans, &tag, key, lsn);

	/* Test hash table full */
	for (i = 6; i <= MINI_TRANSACTION_TABLE_SIZE; i++)
	{
		tag.blockNum = i;
		test_mini_trans_page(trans, &tag, lsn);
	}

	Assert(polar_logindex_mini_trans_key_lock(trans, &tag, MINI_TRANSACTION_HASH_PAGE(&tag), LW_SHARED, &plsn)
		   != POLAR_INVALID_PAGE_LOCK);

	tag.blockNum = i;
	Assert(polar_logindex_mini_trans_key_lock(trans, &tag, MINI_TRANSACTION_HASH_PAGE(&tag), LW_SHARED, &plsn)
		   == POLAR_INVALID_PAGE_LOCK);

	polar_logindex_abort_mini_transaction(trans);

	polar_logindex_mini_trans_end(trans, lsn);

	Assert(polar_logindex_mini_trans_find(trans, &tag) == InvalidXLogRecPtr);
	Assert(polar_logindex_mini_trans_cond_lock(trans, &tag, LW_EXCLUSIVE, &plsn)
		   == 0);

	tag.blockNum = 2;
	Assert(polar_logindex_mini_trans_find(trans, &tag) == InvalidXLogRecPtr);
	Assert(polar_logindex_mini_trans_cond_lock(trans, &tag, LW_EXCLUSIVE, &plsn)
		   == 0);

	PG_RETURN_INT32(0);
}

Size
test_mini_trans_request_shmem_size(void)
{
	return polar_logindex_mini_trans_shmem_size();
}

void
test_mini_trans_shmem_startup(void)
{
	Assert(test_trans == NULL);
	test_trans = polar_logindex_mini_trans_shmem_init("test_mini_trans");
	Assert(test_trans);
}
