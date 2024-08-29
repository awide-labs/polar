/*--------------------------------------------------------------------------
 *
 * test_polar_bulk_read.c
 *		Test correctness of bulk read operations.
 *
 * Portions Copyright (c) 2022, Alibaba Group Holding Limited
 * Copyright (c) 2009-2019, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/test/modules/test_polar_bulk_read/test_polar_bulk_read.c
 *
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/heapam.h"
#include "catalog/namespace.h"
#include "miscadmin.h"
#include "nodes/primnodes.h"
#include "storage/bufmgr.h"
#include "storage/smgr.h"
#include "utils/builtins.h"
#include "utils/rel.h"
#include "utils/varlena.h"

PG_MODULE_MAGIC;

static void
drop_relfile_node_buffers_internal(text *relname, ForkNumber forknum,
								   BlockNumber first_del_block)
{
	RangeVar   *relrv;
	Relation	rel;
	SMgrRelation rd_smgr;


	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 (errmsg("must be superuser to use raw functions"))));

	relrv = makeRangeVarFromNameList(textToQualifiedNameList(relname));
	rel = relation_openrv(relrv, AccessShareLock);

	/* Check that this relation has storage */
	if (rel->rd_rel->relkind == RELKIND_VIEW)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot drop buffers from view \"%s\"",
						RelationGetRelationName(rel))));
	if (rel->rd_rel->relkind == RELKIND_COMPOSITE_TYPE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot drop buffers from composite type \"%s\"",
						RelationGetRelationName(rel))));
	if (rel->rd_rel->relkind == RELKIND_FOREIGN_TABLE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot drop buffers from foreign table \"%s\"",
						RelationGetRelationName(rel))));
	if (rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot drop buffers from partitioned table \"%s\"",
						RelationGetRelationName(rel))));
	if (rel->rd_rel->relkind == RELKIND_PARTITIONED_INDEX)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot drop buffers from partitioned index \"%s\"",
						RelationGetRelationName(rel))));

	/*
	 * Reject attempts to read non-local temporary relations; we would be
	 * likely to get wrong data since we have no visibility into the owning
	 * session's local buffers.
	 */
	if (RELATION_IS_OTHER_TEMP(rel))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot access temporary tables of other sessions")));

	if (first_del_block >= RelationGetNumberOfBlocksInFork(rel, forknum))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("block number %u is out of range for relation \"%s\"",
						first_del_block, RelationGetRelationName(rel))));

	RelationGetSmgr(rel);
	rd_smgr = rel->rd_smgr;

	relation_close(rel, AccessShareLock);

	DropRelFileNodeBuffers(rd_smgr, &forknum, 1, &first_del_block);
}

/*
 * SQL-callable entry point to perform all tests
 *
 * Argument is the number of entries to put in the trees
 */
PG_FUNCTION_INFO_V1(polar_drop_relation_buffers);

Datum
polar_drop_relation_buffers(PG_FUNCTION_ARGS)
{
	text	   *relname;
	text	   *forkname;
	uint32		first_del_block;
	ForkNumber	forknum;

	/*
	 * We don't normally bother to check the number of arguments to a C
	 * function, but here it's needed for safety because early 8.4 beta
	 * releases mistakenly redefined get_raw_page() as taking three arguments.
	 */
	if (PG_NARGS() != 3)
		ereport(ERROR,
				(errmsg("wrong number of arguments to drop_relation_buffers()"),
				 errhint("Run the updated test_bulk_read.sql script.")));

	relname = PG_GETARG_TEXT_PP(0);
	forkname = PG_GETARG_TEXT_PP(1);
	first_del_block = PG_GETARG_UINT32(2);
	forknum = forkname_to_number(text_to_cstring(forkname));

	drop_relfile_node_buffers_internal(relname, forknum, first_del_block);

	PG_RETURN_VOID();
}
