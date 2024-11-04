/*-------------------------------------------------------------------------
 *
 * test_bulkio.c
 *	  Test module for bulk IO interface
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
 *	  src/test/modules/test_bulkio/test_bulkio.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "fmgr.h"
#include "miscadmin.h"
#include "storage/smgr.h"

PG_MODULE_MAGIC;

static int	blk_range = 20;
static ForkNumber forknum = MAIN_FORKNUM;
PGIOAlignedBlock write_buffers[32];
PGIOAlignedBlock read_buffers[32];

static void
test_bulkread(SMgrRelation smgr, BlockNumber begin_blkno)
{
	MemSet(read_buffers, 0, BLCKSZ * blk_range);

	for (int i = 0; i < blk_range; i++)
	{
		smgrextend(smgr, forknum, begin_blkno + i, &write_buffers[i], true);
		smgrread(smgr, forknum, begin_blkno + i, &read_buffers[i]);

		/* Cross validation */
		if (memcmp(&write_buffers[i], &read_buffers[i], BLCKSZ) != 0)
			elog(ERROR, "bulkio test read failed");
	}

	MemSet(read_buffers, 0, BLCKSZ * blk_range);
	polar_smgrbulkread(smgr, forknum, begin_blkno, blk_range, &read_buffers);

	if (memcmp(&write_buffers, &read_buffers, BLCKSZ * blk_range) != 0)
		elog(ERROR, "bulkio test bulk read failed");

	smgrtruncate(smgr, &forknum, 1, &begin_blkno);
}

static void
test_bulkwrite(SMgrRelation smgr, BlockNumber begin_blkno)
{
	smgrzeroextend(smgr, forknum, begin_blkno, blk_range, true);

	polar_smgrbulkwrite(smgr, forknum, begin_blkno, blk_range, &write_buffers, true);

	MemSet(read_buffers, 0, BLCKSZ * blk_range);
	polar_smgrbulkread(smgr, forknum, begin_blkno, blk_range, &read_buffers);

	if (memcmp(&write_buffers, &read_buffers, BLCKSZ * blk_range) != 0)
		elog(ERROR, "bulkio test bulk write failed");

	smgrtruncate(smgr, &forknum, 1, &begin_blkno);
}

static void
test_bulkextend(SMgrRelation smgr, BlockNumber begin_blkno)
{
	polar_smgrbulkextend(smgr, forknum, begin_blkno, blk_range, &write_buffers, true);

	MemSet(read_buffers, 0, BLCKSZ * blk_range);
	polar_smgrbulkread(smgr, forknum, begin_blkno, blk_range, &read_buffers);

	if (memcmp(&write_buffers, &read_buffers, BLCKSZ * blk_range) != 0)
		elog(ERROR, "bulkio test bulk extend failed");

	smgrtruncate(smgr, &forknum, 1, &begin_blkno);
}

static void
test_bulkio_aux(SMgrRelation smgr, BlockNumber begin_blkno)
{
	BlockNumber nblocks = smgrnblocks(smgr, forknum);

	if (begin_blkno - nblocks > 0)
		smgrzeroextend(smgr, forknum, nblocks, begin_blkno - nblocks, true);

	for (int i = 0; i < blk_range; i++)
		MemSet(&write_buffers[i], begin_blkno + i, BLCKSZ);

	test_bulkread(smgr, begin_blkno);
	test_bulkwrite(smgr, begin_blkno);
	test_bulkextend(smgr, begin_blkno);
}

PG_FUNCTION_INFO_V1(test_bulkio);
Datum
test_bulkio(PG_FUNCTION_ARGS)
{
	BlockNumber zero_blkno = 0;
	RelFileNode perf_rlocator = {MyDatabaseTableSpace, MyDatabaseId, 1};
	SMgrRelation smgr = smgropen(perf_rlocator, InvalidBackendId);

	if (!smgrexists(smgr, forknum))
		smgrcreate(smgr, forknum, false);
	else
		smgrtruncate(smgr, &forknum, 1, &zero_blkno);

	test_bulkio_aux(smgr, 0);
	test_bulkio_aux(smgr, 1 * RELSEG_SIZE - 10);
	test_bulkio_aux(smgr, 2 * RELSEG_SIZE - 10);
	test_bulkio_aux(smgr, 3 * RELSEG_SIZE - 10);

	smgrdounlinkall(&smgr, 1, false);
	smgrclose(smgr);

	PG_RETURN_VOID();
}
