/*-------------------------------------------------------------------------
 *
 * polar_rsc_replica.c
 *	  Replica redo of relation size cache.
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
 *	  src/backend/storage/smgr/polar_rsc_replica.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/ginxlog.h"
#include "access/gistxlog.h"
#include "access/heapam_xlog.h"
#include "access/nbtxlog.h"
#include "catalog/pg_control.h"
#include "catalog/storage_xlog.h"
#include "commands/dbcommands_xlog.h"
#include "commands/tablespace.h"
#include "storage/smgr.h"

static bool
_nblocks_greater_than(BlockNumber newval, BlockNumber cached)
{
	return newval > cached;
}

static void
rsc_update_with_max_blkno(XLogReaderState *record)
{
	RelFileLocator rlocator;
	ForkNumber	forknum = MAIN_FORKNUM;
	BlockNumber blkno;
	BlockNumber max_blkno = InvalidBlockNumber;

	for (int i = 0; i <= XLogRecMaxBlockId(record); i++)
	{
		ForkNumber	_forknum;

		if (!XLogRecHasBlockRefInUse(record, i))
			continue;

		XLogRecGetBlockTag(record, i, &rlocator, &_forknum, &blkno);
		Assert(_forknum == forknum);

		if (max_blkno == InvalidBlockNumber)
			max_blkno = blkno;
		else
			max_blkno = Max(max_blkno, blkno);
	}

	polar_rsc_update_if_exists(&rlocator,
							   forknum,
							   max_blkno + 1,
							   _nblocks_greater_than);
}

/*
 * rsc_replica_redo_smgr
 *
 * Deal with FPI. It has the effect of extending a relation.
 */
static void
rsc_replica_redo_xlog(XLogReaderState *record)
{
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

	Assert(XLogRecGetRmid(record) == RM_XLOG_ID);

	switch (info)
	{
		case XLOG_FPI:
		case XLOG_FPI_FOR_HINT:
			{
				RelFileLocator rlocator;
				ForkNumber	forknum = MAIN_FORKNUM;
				BlockNumber blkno;

				for (uint8 block_id = 0; block_id <= XLogRecMaxBlockId(record); block_id++)
				{
					if (!XLogRecHasBlockRef(record, block_id))
						continue;

					XLogRecGetBlockTag(record, block_id, &rlocator, &forknum, &blkno);
					polar_rsc_update_if_exists(&rlocator,
											   forknum,
											   blkno + 1,
											   _nblocks_greater_than);
				}
				break;
			}

		default:
			break;
	}
}

/*
 * rsc_replica_redo_smgr
 *
 * Deal with SMGR_CREATE: allocate all-zero RSC entry for faster dropping buffers.
 *
 * Deal with SMGR_TRUNCATE: after truncating a file, the nblocks value should be
 * changed. So the cached value on replica should be invalidated.
 */
static void
rsc_replica_redo_smgr(XLogReaderState *record)
{
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

	Assert(XLogRecGetRmid(record) == RM_SMGR_ID);
	Assert(!XLogRecHasAnyBlockRefs(record));

	switch (info)
	{
		case XLOG_SMGR_CREATE:
			{
				xl_smgr_create *xlrec = (xl_smgr_create *) XLogRecGetData(record);

				if (polar_rsc_optimize_drop_buffers && xlrec->forkNum == MAIN_FORKNUM)
				{
					polar_rsc_init_empty_entry(&xlrec->rlocator, xlrec->forkNum);
				}

				break;
			}

			/*
			 * Do nothing here, keep the old nblocks in RSC for later dropping
			 * buffers. Update the new nblocks when buffers are actually
			 * dropped during logindex redo.
			 */
		case XLOG_SMGR_TRUNCATE:
			break;
		case XLOG_SMGR_BULK_EXTEND:
			{
				xl_smgr_bulk_extend *xlrec = (xl_smgr_bulk_extend *) XLogRecGetData(record);

				polar_rsc_drop_entry(&xlrec->rlocator);
				break;
			}

		default:
			break;
	}
}

/*
 * rsc_replica_redo_dbase
 *
 * Deal with DROP DATABASE: after dropping a database, all the entries belongs
 * to this database should be invalidated.
 */
static void
rsc_replica_redo_dbase(XLogReaderState *record)
{
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

	Assert(XLogRecGetRmid(record) == RM_DBASE_ID);
	Assert(!XLogRecHasAnyBlockRefs(record));

	switch (info)
	{
		case XLOG_DBASE_DROP:
			{
				xl_dbase_drop_rec *xlrec = (xl_dbase_drop_rec *) XLogRecGetData(record);

				polar_rsc_drop_entries(xlrec->db_id, InvalidOid);
				break;
			}

		default:
			break;
	}
}

/*
 * rsc_replica_redo_tblspc
 *
 * Deal with DROP TABLESPACE: after dropping a tablespace, all the entries
 * belongs to this tablespace should be invalidated.
 */
static void
rsc_replica_redo_tblspc(XLogReaderState *record)
{
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

	Assert(XLogRecGetRmid(record) == RM_TBLSPC_ID);
	Assert(!XLogRecHasAnyBlockRefs(record));

	switch (info)
	{
		case XLOG_TBLSPC_DROP:
			{
				xl_tblspc_drop_rec *xlrec = (xl_tblspc_drop_rec *) XLogRecGetData(record);

				polar_rsc_drop_entries(InvalidOid, xlrec->ts_id);
				break;
			}
		default:
			break;
	}
}

static void
rsc_replica_redo_heap2(XLogReaderState *record)
{
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

	Assert(XLogRecGetRmid(record) == RM_HEAP2_ID);

	switch (info & XLOG_HEAP_OPMASK)
	{
			/*
			 * VM page extension.
			 */
		case XLOG_HEAP2_VISIBLE:

			/*
			 * Multi-insert heap page.
			 */
		case XLOG_HEAP2_MULTI_INSERT:
			{
				RelFileLocator rlocator;
				ForkNumber	forknum = MAIN_FORKNUM;
				BlockNumber blkno;

				XLogRecGetBlockTag(record, 0, &rlocator, &forknum, &blkno);
				polar_rsc_update_if_exists(&rlocator,
										   forknum,
										   blkno + 1,
										   _nblocks_greater_than);
				break;
			}

		default:
			break;
	}
}

static void
rsc_replica_redo_heap(XLogReaderState *record)
{
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

	Assert(XLogRecGetRmid(record) == RM_HEAP_ID);

	switch (info & XLOG_HEAP_OPMASK)
	{
			/*
			 * TRUNCATE handling is done by SMGR WAL records.
			 */
		case XLOG_HEAP_TRUNCATE:
			break;

		case XLOG_HEAP_INSERT:
		case XLOG_HEAP_UPDATE:
			{
				RelFileLocator rlocator;
				ForkNumber	forknum = MAIN_FORKNUM;
				BlockNumber blkno;

				XLogRecGetBlockTag(record, 0, &rlocator, &forknum, &blkno);
				polar_rsc_update_if_exists(&rlocator,
										   forknum,
										   blkno + 1,
										   _nblocks_greater_than);
				break;
			}

		default:
			break;
	}
}

static void
rsc_replica_redo_btree(XLogReaderState *record)
{
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

	Assert(XLogRecGetRmid(record) == RM_BTREE_ID);

	switch (info)
	{
			/*
			 * These B-Tree WAL types may change nblocks.
			 */
		case XLOG_BTREE_SPLIT_L:
		case XLOG_BTREE_SPLIT_R:
		case XLOG_BTREE_NEWROOT:
			rsc_update_with_max_blkno(record);
			break;

		default:
			break;
	}
}

static void
rsc_replica_redo_gin(XLogReaderState *record)
{
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

	Assert(XLogRecGetRmid(record) == RM_GIN_ID);

	switch (info)
	{
			/*
			 * These Gin WAL types may change nblocks.
			 */
		case XLOG_GIN_CREATE_PTREE:
		case XLOG_GIN_SPLIT:
		case XLOG_GIN_INSERT_LISTPAGE:
			rsc_update_with_max_blkno(record);
			break;

		default:
			break;
	}
}

static void
rsc_replica_redo_gist(XLogReaderState *record)
{
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

	Assert(XLogRecGetRmid(record) == RM_GIST_ID);

	switch (info)
	{
			/*
			 * These Gist WAL types may change nblocks.
			 */
		case XLOG_GIST_PAGE_SPLIT:
			rsc_update_with_max_blkno(record);
			break;

		default:
			break;
	}
}

/*
 * rsc_replica_redo
 *
 * We cannot tell how these WAL records take effects on nblock values, so the
 * safest way is to invalidate all the related cached values. It is mostly used
 * by indexes, or custom AMs using generic WAL format.
 */
static void
rsc_replica_redo(XLogReaderState *record)
{
	int			block_id;
	RelFileLocator rlocator;

	for (block_id = 0; block_id <= XLogRecMaxBlockId(record); block_id++)
	{
		if (XLogRecHasBlockRefInUse(record, block_id))
		{
			XLogRecGetBlockTag(record, block_id, &rlocator, NULL, NULL);
			polar_rsc_drop_entry(&rlocator);
		}
	}
}

/*
 * RSC callbacks registration for replica redo.
 *
 * If we are sure that a WAL type will never change nblocks, then it is no need
 * to implement a callback; if we are sure a WAL type MAY change nblocks, but
 * not how, then the safest way is to use default redo callback which will
 * invalidate all the related cached values; if we are sure about how a WAL
 * type may change nblocks value, then we can implement a specific callback to
 * work effectively.
 */
const polar_rsc_replica_redo_cb_t polar_rsc_replica_redo_cb[] =
{
	{rsc_replica_redo_xlog},	/* RM_XLOG_ID */
	{NULL},						/* RM_XACT_ID */
	{rsc_replica_redo_smgr},	/* RM_SMGR_ID */
	{NULL},						/* RM_CLOG_ID */
	{rsc_replica_redo_dbase},	/* RM_DBASE_ID */
	{rsc_replica_redo_tblspc},	/* RM_TBLSPC_ID */
	{NULL},						/* RM_MULTIXACT_ID */
	{NULL},						/* RM_RELMAP_ID */
	{NULL},						/* RM_STANDBY_ID */
	{rsc_replica_redo_heap2},	/* RM_HEAP2_ID */
	{rsc_replica_redo_heap},	/* RM_HEAP_ID */
	{rsc_replica_redo_btree},	/* RM_BTREE_ID */
	{rsc_replica_redo},			/* RM_HASH_ID */
	{rsc_replica_redo_gin},		/* RM_GIN_ID */
	{rsc_replica_redo_gist},	/* RM_GIST_ID */
	{rsc_replica_redo},			/* RM_SEQ_ID */
	{rsc_replica_redo},			/* RM_SPGIST_ID */
	{rsc_replica_redo},			/* RM_BRIN_ID */
	{NULL},						/* RM_COMMIT_TS_ID */
	{NULL},						/* RM_REPLORIGIN_ID */
	{rsc_replica_redo},			/* RM_GENERIC_ID */
	{NULL},						/* RM_LOGICALMSG_ID */
};

StaticAssertDecl(lengthof(polar_rsc_replica_redo_cb) == RM_N_BUILTIN_IDS,
				 "missing callback for replica redo of RSC");
