#include "postgres.h"

#include "access/heapam.h"
#include "access/hio.h"
#include "access/htup_details.h"
#include "access/multixact.h"
#include "access/transam.h"
#include "access/xact.h"
#include "access/visibilitymap.h"
#include "catalog/pg_am.h"
#include "executor/spi.h"
#include "funcapi.h"
#include "nodes/makefuncs.h"
#include "storage/bufmgr.h"
#include "storage/procarray.h"
#include "utils/guc.h"
#include "utils/rel.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(test_csn_xact_xmin);
PG_FUNCTION_INFO_V1(test_csn_xact_xmax);
PG_FUNCTION_INFO_V1(test_csn_xact_multixact);

Datum
test_csn_xact_xmin(PG_FUNCTION_ARGS)
{
	bool		commit = PG_GETARG_BOOL(0);
	RangeVar   *rv;
	Relation	rel;
	Datum		values[2];
	bool		isnull[2];
	HeapTuple	tup,
				tup2;
	TransactionId xid1,
				xid2;
	int			ret;
	Buffer		buffer;
	Buffer		vmbuffer = InvalidBuffer;
	SnapshotData snapshot1 = {};
	SnapshotData snapshot2 = {};
	SnapshotData snapshot3 = {};

	if (!polar_csn_enable)
		elog(ERROR, "test_csn_xact: polar_csn_enable must be on");

	/* Connect to SPI manager */
	if ((ret = SPI_connect()) < 0)
		/* internal error */
		elog(ERROR, "test_csn_xact: SPI_connect returned %d", ret);

	SPI_execute("DROP TABLE IF EXISTS csn_xact_table_case1", false, 0);
	SPI_execute("CREATE TABLE csn_xact_table_case1(i int4)", false, 0);

	SPI_finish();

	/* Generate a XID */
	xid2 = XidFromFullTransactionId(GetNewTransactionId(false));
	xid1 = XidFromFullTransactionId(GetNewTransactionId(false));

	rv = makeRangeVar(NULL, "csn_xact_table_case1", -1);

	rel = table_openrv(rv, RowExclusiveLock);

	values[0] = Int32GetDatum(0);
	isnull[0] = false;
	values[1] = Int32GetDatum(0);
	isnull[1] = false;

	tup = heap_form_tuple(RelationGetDescr(rel), &values[0], &isnull[0]);
	tup2 = heap_form_tuple(RelationGetDescr(rel), &values[1], &isnull[1]);

	/* Fill the header fields, like heap_prepare_insert does */
	tup->t_data->t_infomask &= ~(HEAP_XACT_MASK);
	tup->t_data->t_infomask2 &= ~(HEAP2_XACT_MASK);
	tup->t_data->t_infomask |= HEAP_XMAX_INVALID;
	HeapTupleHeaderSetXmin(tup->t_data, xid1);
	HeapTupleHeaderSetCmin(tup->t_data, 1);
	HeapTupleHeaderSetXmax(tup->t_data, 0); /* for cleanliness */
	tup->t_tableOid = RelationGetRelid(rel);

	tup2->t_data->t_infomask &= ~(HEAP_XACT_MASK);
	tup2->t_data->t_infomask2 &= ~(HEAP2_XACT_MASK);
	tup2->t_data->t_infomask |= HEAP_XMAX_INVALID;
	HeapTupleHeaderSetXmin(tup2->t_data, xid2);
	HeapTupleHeaderSetCmin(tup2->t_data, 1);
	HeapTupleHeaderSetXmax(tup2->t_data, 0);	/* for cleanliness */
	tup2->t_tableOid = RelationGetRelid(rel);

	/*
	 * Find buffer to insert this tuple into.  If the page is all visible,
	 * this will also pin the requisite visibility map page.
	 */
	buffer = RelationGetBufferForTuple(rel, tup->t_len + tup2->t_len,
									   InvalidBuffer,
									   0, NULL,
									   &vmbuffer, NULL);
	RelationPutHeapTuple(rel, buffer, tup, false);
	RelationPutHeapTuple(rel, buffer, tup2, false);

	table_close(rel, NoLock);

	if (PageIsAllVisible(BufferGetPage(buffer)))
	{
		PageClearAllVisible(BufferGetPage(buffer));
		visibilitymap_clear(rel, ItemPointerGetBlockNumber(&(tup->t_self)),
							vmbuffer, VISIBILITYMAP_VALID_BITS, NULL);
	}

	MarkBufferDirty(buffer);

	GetSnapshotData(&snapshot1);

	if (TransactionIdDidCommit(xid1))
	{
		elog(WARNING, "test1.1: expected xid1's status is not committed, failed");
	}
	if (TransactionIdDidAbort(xid1))
	{
		elog(WARNING, "test 1.2: expected xid1's status is not aborted, failed");
	}
	if (polar_xact_get_status(xid1) != XID_IN_PROGRESS)
	{
		elog(WARNING, "test 1.3: expected xid1's status is in progess, failed");
	}
	else
	{
		elog(NOTICE, "test 1.4: expected xid1's status is in progess, success");
	}

	if (HeapTupleSatisfiesMVCC(tup, &snapshot1, buffer))
	{
		elog(WARNING, "test 1.4: expected HeapTupleSatisfiesMVCC returns false, failed");
	}

	if (HeapTupleSatisfiesUpdate(tup, 2, buffer) != TM_Invisible)
	{
		elog(WARNING, "test 1.5: expected HeapTupleSatisfiesUpdate returns TM_Invisible, failed");
	}

	if (commit)
	{
		TransactionIdCommitTree(xid1, 0, NULL);
		TransactionIdCommitTree(xid2, 0, NULL);
		polar_xact_commit_tree_csn(xid2, 0, NULL, InvalidXLogRecPtr);
		GetSnapshotData(&snapshot2);

		if (TransactionIdDidCommit(xid1))
		{
			elog(WARNING, "test 2.1: expected xid1's status is not committed, failed");
		}

		if (TransactionIdDidAbort(xid1))
		{
			elog(WARNING, "test 2.2: expected xid1's status is not aborted, failed");
		}

		if (polar_xact_get_status(xid1) != XID_IN_PROGRESS)
		{
			elog(WARNING, "test 2.3: expected xid1's status is in progess, failed");
		}
		else
		{
			elog(NOTICE, "test 2.3: expected xid1's status is in progess, success");
		}

		if (HeapTupleSatisfiesMVCC(tup, &snapshot1, buffer))
		{
			elog(WARNING, "test 2.4: expected HeapTupleSatisfiesMVCC returns false, failed");
		}

		if (HeapTupleSatisfiesMVCC(tup, &snapshot2, buffer))
		{
			elog(WARNING, "test 2.5: expected HeapTupleSatisfiesMVCC returns false, failed");
		}

		if (HeapTupleSatisfiesUpdate(tup, 2, buffer) != TM_Invisible)
		{
			elog(WARNING, "test 2.6: expected HeapTupleSatisfiesUpdate's returns TM_Invisible, failed");
		}

		polar_xact_commit_tree_csn(xid1, 0, NULL, InvalidXLogRecPtr);
		GetSnapshotData(&snapshot3);

		if (TransactionIdDidAbort(xid1))
		{
			elog(WARNING, "test 3.1: expected xid1's status is not aborted, failed");
		}

		if (polar_xact_get_status(xid1) == XID_IN_PROGRESS)
		{
			elog(WARNING, "test 3.2: expected xid1's status is not in progess, failed");
		}

		if (!TransactionIdDidCommit(xid1))
		{
			elog(WARNING, "test 3.3: expected xid1's status is committed, failed");
		}
		else
		{
			elog(NOTICE, "test 3.3: expected xid1's status is committed, success");
		}

		if (HeapTupleSatisfiesMVCC(tup, &snapshot1, buffer))
		{
			elog(WARNING, "test 3.4: expected HeapTupleSatisfiesMVCC returns false, failed");
		}

		if (HeapTupleSatisfiesMVCC(tup, &snapshot2, buffer))
		{
			elog(WARNING, "test 3.5: expected HeapTupleSatisfiesMVCC returns false, failed");
		}

		if (!HeapTupleSatisfiesMVCC(tup, &snapshot3, buffer))
		{
			elog(WARNING, "test 3.6: expected HeapTupleSatisfiesMVCC returns true, failed");
		}

		if (!HeapTupleSatisfiesMVCC(tup2, &snapshot3, buffer))
		{
			elog(WARNING, "test 3.7: expected HeapTupleSatisfiesMVCC returns true, failed");
		}

		if (!HeapTupleSatisfiesMVCC(tup, &snapshot3, buffer))
		{
			elog(WARNING, "test 3.8: expected HeapTupleSatisfiesMVCC returns true, failed");
		}

		if (HeapTupleSatisfiesUpdate(tup, 2, buffer) != TM_Ok)
		{
			elog(WARNING, "test 3.9: expected HeapTupleSatisfiesUpdate's result is TM_Ok, failed");
		}
	}
	else
	{
		TransactionIdAbortTree(xid1, 0, NULL);
		GetSnapshotData(&snapshot2);

		if (TransactionIdDidCommit(xid1))
		{
			elog(WARNING, "test 4.1: expected xid1's status is not committed, failed");
		}

		if (polar_xact_get_status(xid1) == XID_IN_PROGRESS)
		{
			elog(WARNING, "test 4.2: expected xid1's status is not in progess, failed");
		}

		if (!TransactionIdDidAbort(xid1))
		{
			elog(WARNING, "test 4.3: expected xid1's status is not aborted, failed");
		}
		else
		{
			elog(NOTICE, "test 4.3: expected xid1's status is aborted, success");
		}

		if (HeapTupleSatisfiesMVCC(tup, &snapshot1, buffer))
		{
			elog(WARNING, "test 4.4: expected HeapTupleSatisfiesMVCC returns false, failed");
		}

		if (HeapTupleSatisfiesMVCC(tup, &snapshot2, buffer))
		{
			elog(WARNING, "test 4.5: expected HeapTupleSatisfiesMVCC returns false, failed");
		}

		if (HeapTupleSatisfiesUpdate(tup, 2, buffer) != TM_Invisible)
		{
			elog(WARNING, "test 4.6: expected HeapTupleSatisfiesUpdate's result is TM_Invisible, failed");
		}
	}

	UnlockReleaseBuffer(buffer);
	heap_freetuple(tup);
	heap_freetuple(tup2);

	if (vmbuffer != InvalidBuffer)
		ReleaseBuffer(vmbuffer);

	/* Connect to SPI manager */
	if ((ret = SPI_connect()) < 0)
		/* internal error */
		elog(ERROR, "test_csn_xact: SPI_connect returned %d", ret);

	SPI_execute("DROP TABLE csn_xact_table_case1;", false, 0);

	SPI_finish();

	PG_RETURN_VOID();
}

Datum
test_csn_xact_xmax(PG_FUNCTION_ARGS)
{
	bool		commit = PG_GETARG_BOOL(0);
	RangeVar   *rv;
	Relation	rel;
	Datum		values[2];
	bool		isnull[2];
	HeapTuple	tup,
				tup2;
	TransactionId xid1,
				xid2;
	int			ret;
	Buffer		buffer;
	Buffer		vmbuffer = InvalidBuffer;
	SnapshotData snapshot1 = {};
	SnapshotData snapshot2 = {};
	SnapshotData snapshot3 = {};

	if (!polar_csn_enable)
		elog(ERROR, "test_csn_xact: polar_csn_enable must be on");

	/* Connect to SPI manager */
	if ((ret = SPI_connect()) < 0)
		/* internal error */
		elog(ERROR, "test_csn_xact: SPI_connect returned %d", ret);

	SPI_execute("DROP TABLE IF EXISTS csn_xact_table_case2", false, 0);
	SPI_execute("CREATE TABLE csn_xact_table_case2(i int4)", false, 0);

	SPI_finish();

	/* Generate a XID */
	xid2 = XidFromFullTransactionId(GetNewTransactionId(false));
	xid1 = XidFromFullTransactionId(GetNewTransactionId(false));

	rv = makeRangeVar(NULL, "csn_xact_table_case2", -1);

	rel = table_openrv(rv, RowExclusiveLock);

	values[0] = Int32GetDatum(0);
	isnull[0] = false;
	values[1] = Int32GetDatum(0);
	isnull[1] = false;

	tup = heap_form_tuple(RelationGetDescr(rel), &values[0], &isnull[0]);
	tup2 = heap_form_tuple(RelationGetDescr(rel), &values[1], &isnull[1]);

	/* Fill the header fields, like heap_prepare_insert does */
	tup->t_data->t_infomask &= ~(HEAP_XACT_MASK);
	tup->t_data->t_infomask2 &= ~(HEAP2_XACT_MASK);
	tup->t_data->t_infomask |= HEAP_XMIN_FROZEN;
	HeapTupleHeaderSetXmin(tup->t_data, FrozenTransactionId);
	HeapTupleHeaderSetCmin(tup->t_data, 1);
	HeapTupleHeaderSetXmax(tup->t_data, xid1);
	tup->t_tableOid = RelationGetRelid(rel);

	tup2->t_data->t_infomask &= ~(HEAP_XACT_MASK);
	tup2->t_data->t_infomask2 &= ~(HEAP2_XACT_MASK);
	tup2->t_data->t_infomask |= HEAP_XMIN_FROZEN;
	HeapTupleHeaderSetXmin(tup2->t_data, FrozenTransactionId);
	HeapTupleHeaderSetCmin(tup2->t_data, 1);
	HeapTupleHeaderSetXmax(tup2->t_data, xid1);
	tup2->t_tableOid = RelationGetRelid(rel);

	/*
	 * Find buffer to insert this tuple into.  If the page is all visible,
	 * this will also pin the requisite visibility map page.
	 */
	buffer = RelationGetBufferForTuple(rel, tup->t_len + tup2->t_len,
									   InvalidBuffer,
									   0, NULL,
									   &vmbuffer, NULL);
	RelationPutHeapTuple(rel, buffer, tup, false);
	RelationPutHeapTuple(rel, buffer, tup2, false);

	tup->t_data->t_ctid = tup->t_self;
	tup2->t_data->t_ctid = tup2->t_self;

	table_close(rel, NoLock);

	if (PageIsAllVisible(BufferGetPage(buffer)))
	{
		PageClearAllVisible(BufferGetPage(buffer));
		visibilitymap_clear(rel, ItemPointerGetBlockNumber(&(tup->t_self)),
							vmbuffer, VISIBILITYMAP_VALID_BITS, NULL);
	}

	MarkBufferDirty(buffer);

	GetSnapshotData(&snapshot1);

	if (TransactionIdDidCommit(xid1))
	{
		elog(WARNING, "test 1.1: expected xid1's status is not committed, failed");
	}
	if (TransactionIdDidAbort(xid1))
	{
		elog(WARNING, "test 1.2: expected xid1's status is not aborted, failed");
	}
	if (polar_xact_get_status(xid1) != XID_IN_PROGRESS)
	{
		elog(WARNING, "test 1.3: expected xid1's status is in progress, failed");
	}
	else
	{
		elog(NOTICE, "test 1.3: expected xid1's status is in progress, success");
	}

	if (!HeapTupleSatisfiesMVCC(tup, &snapshot1, buffer))
	{
		elog(WARNING, "test 1.4: expected HeapTupleSatisfiesMVCC returns true, failed");
	}

	if (HeapTupleSatisfiesUpdate(tup, 2, buffer) != TM_BeingModified)
	{
		elog(WARNING, "test 1.5: expected HeapTupleSatisfiesUpdate returns M_BeingModified, failed");
	}

	if (commit)
	{
		TransactionIdCommitTree(xid1, 0, NULL);
		TransactionIdCommitTree(xid2, 0, NULL);
		polar_xact_commit_tree_csn(xid2, 0, NULL, InvalidXLogRecPtr);
		GetSnapshotData(&snapshot2);

		if (TransactionIdDidCommit(xid1))
		{
			elog(WARNING, "test 2.1: expected xid1's status is not committed, failed");
		}

		if (TransactionIdDidAbort(xid1))
		{
			elog(WARNING, "test 2.2: expected xid1's status is not aborted, failed");
		}

		if (polar_xact_get_status(xid1) != XID_IN_PROGRESS)
		{
			elog(WARNING, "test 2.3: expected xid1's status is in progess, failed");
		}
		else
		{
			elog(NOTICE, "test 2.4: expected xid1's status is in progess, success");
		}

		if (!HeapTupleSatisfiesMVCC(tup, &snapshot1, buffer))
		{
			elog(WARNING, "test 2.5: expected HeapTupleSatisfiesMVCC returns true, failed");
		}

		if (!HeapTupleSatisfiesMVCC(tup, &snapshot2, buffer))
		{
			elog(WARNING, "test 2.6: expected HeapTupleSatisfiesMVCC returns true, failed");
		}

		if (HeapTupleSatisfiesUpdate(tup, 2, buffer) != TM_BeingModified)
		{
			elog(WARNING, "test 2.7: expected HeapTupleSatisfiesUpdate's returns TM_BeingModified, failed");
		}

		polar_xact_commit_tree_csn(xid1, 0, NULL, InvalidXLogRecPtr);
		GetSnapshotData(&snapshot3);

		if (TransactionIdDidAbort(xid1))
		{
			elog(WARNING, "test 3.1: expected xid1's status is not aborted, failed");
		}

		if (polar_xact_get_status(xid1) == XID_IN_PROGRESS)
		{
			elog(WARNING, "test 3.2: expected xid1's status is not in progess, failed");
		}

		if (!TransactionIdDidCommit(xid1))
		{
			elog(WARNING, "test 3.3: expected xid1's status is committed, failed");
		}
		else
		{
			elog(NOTICE, "test 3.3: expected xid1's status is committed, success");
		}

		if (!HeapTupleSatisfiesMVCC(tup, &snapshot1, buffer))
		{
			elog(WARNING, "test 3.4: expected HeapTupleSatisfiesMVCC returns true, failed");
		}

		if (!HeapTupleSatisfiesMVCC(tup, &snapshot2, buffer))
		{
			elog(WARNING, "test 3.5: expected HeapTupleSatisfiesMVCC returns true, failed");
		}

		if (HeapTupleSatisfiesMVCC(tup, &snapshot3, buffer))
		{
			elog(WARNING, "test 3.6: expected HeapTupleSatisfiesMVCC returns false, failed");
		}

		if (HeapTupleSatisfiesMVCC(tup2, &snapshot3, buffer))
		{
			elog(WARNING, "test 3.7: expected HeapTupleSatisfiesMVCC returns false, failed");
		}

		if (HeapTupleSatisfiesMVCC(tup, &snapshot3, buffer))
		{
			elog(WARNING, "test 3.8: expected HeapTupleSatisfiesMVCC returns false, failed");
		}

		if (HeapTupleSatisfiesUpdate(tup, 2, buffer) != TM_Deleted)
		{
			elog(WARNING, "test 3.9: expected HeapTupleSatisfiesUpdate's result is TM_Deleted, failed");
		}
	}
	else
	{
		TransactionIdAbortTree(xid1, 0, NULL);
		GetSnapshotData(&snapshot2);
		snapshot2.xmax = xid1;

		if (TransactionIdDidCommit(xid1))
		{
			elog(WARNING, "test 4.1: expected xid1's status is not committed, failed");
		}

		if (polar_xact_get_status(xid1) == XID_IN_PROGRESS)
		{
			elog(WARNING, "test 4.2: expected xid1's status is not in progess, failed");
		}

		if (!TransactionIdDidAbort(xid1))
		{
			elog(WARNING, "test 4.3: expected xid1's status is not aborted, failed");
		}
		else
		{
			elog(NOTICE, "test 4.3: expected xid1's status is aborted, success");
		}

		if (!HeapTupleSatisfiesMVCC(tup, &snapshot1, buffer))
		{
			elog(WARNING, "test 4.4: expected HeapTupleSatisfiesMVCC returns true, failed");
		}

		if (!HeapTupleSatisfiesMVCC(tup, &snapshot2, buffer))
		{
			elog(WARNING, "test 4.5: expected HeapTupleSatisfiesMVCC returns true, failed");
		}

		if (HeapTupleSatisfiesUpdate(tup, 2, buffer) != TM_Ok)
		{
			elog(WARNING, "test 4.6: expected HeapTupleSatisfiesUpdate's result is TM_Ok, failed");
		}
	}

	UnlockReleaseBuffer(buffer);
	heap_freetuple(tup);
	heap_freetuple(tup2);

	if (vmbuffer != InvalidBuffer)
		ReleaseBuffer(vmbuffer);

	/* Connect to SPI manager */
	if ((ret = SPI_connect()) < 0)
		/* internal error */
		elog(ERROR, "test_csn_xact: SPI_connect returned %d", ret);

	SPI_execute("DROP TABLE csn_xact_table_case2;", false, 0);

	SPI_finish();

	PG_RETURN_VOID();
}



Datum
test_csn_xact_multixact(PG_FUNCTION_ARGS)
{
	bool		commit = PG_GETARG_BOOL(0);
	RangeVar   *rv;
	Relation	rel;
	Datum		values[1];
	bool		isnull[1];
	HeapTuple	tup;
	TransactionId xid1;
	MultiXactId new_xmax;
	int			ret;
	Buffer		buffer;
	Buffer		vmbuffer = InvalidBuffer;
	SnapshotData snapshot1 = {};
	SnapshotData snapshot2 = {};
	SnapshotData snapshot3 = {};
	MultiXactStatus status1,
				status2;

	if (!polar_csn_enable)
		elog(ERROR, "test_csn_xact: polar_csn_enable must be on");

	/* Connect to SPI manager */
	if ((ret = SPI_connect()) < 0)
		/* internal error */
		elog(ERROR, "test_csn_xact: SPI_connect returned %d", ret);

	SPI_execute("DROP TABLE IF EXISTS csn_xact_table_case3", false, 0);
	SPI_execute("CREATE TABLE csn_xact_table_case3(i int4)", false, 0);

	SPI_finish();

	/* Generate a XID */
	xid1 = XidFromFullTransactionId(GetNewTransactionId(false));

	rv = makeRangeVar(NULL, "csn_xact_table_case3", -1);

	rel = table_openrv(rv, RowExclusiveLock);

	values[0] = Int32GetDatum(0);
	isnull[0] = false;

	tup = heap_form_tuple(RelationGetDescr(rel), values, isnull);

	status1 = MultiXactStatusNoKeyUpdate;
	status2 = MultiXactStatusForKeyShare,

		MultiXactIdSetOldestMember();
	new_xmax = MultiXactIdCreate(xid1, status1, FrozenTransactionId, status2);

	/* Fill the header fields, like heap_prepare_insert does */
	tup->t_data->t_infomask &= ~(HEAP_XACT_MASK);
	tup->t_data->t_infomask2 &= ~(HEAP2_XACT_MASK);
	tup->t_data->t_infomask |= HEAP_XMIN_FROZEN;
	tup->t_data->t_infomask |= HEAP_XMAX_IS_MULTI;
	tup->t_data->t_infomask |= HEAP_XMAX_EXCL_LOCK;
	HeapTupleHeaderSetXmin(tup->t_data, FrozenTransactionId);
	HeapTupleHeaderSetCmin(tup->t_data, 1);
	HeapTupleHeaderSetXmax(tup->t_data, new_xmax);
	tup->t_tableOid = RelationGetRelid(rel);

	/*
	 * Find buffer to insert this tuple into.  If the page is all visible,
	 * this will also pin the requisite visibility map page.
	 */
	buffer = RelationGetBufferForTuple(rel, tup->t_len,
									   InvalidBuffer,
									   0, NULL,
									   &vmbuffer, NULL);
	RelationPutHeapTuple(rel, buffer, tup, false);

	tup->t_data->t_ctid = tup->t_self;

	table_close(rel, NoLock);

	if (PageIsAllVisible(BufferGetPage(buffer)))
	{
		PageClearAllVisible(BufferGetPage(buffer));
		visibilitymap_clear(rel, ItemPointerGetBlockNumber(&(tup->t_self)),
							vmbuffer, VISIBILITYMAP_VALID_BITS, NULL);
	}

	MarkBufferDirty(buffer);

	GetSnapshotData(&snapshot1);

	if (TransactionIdDidCommit(xid1))
	{
		elog(WARNING, "test 1.1: expected xid1's status is not committed, failed");
	}
	if (TransactionIdDidAbort(xid1))
	{
		elog(WARNING, "test 1.2: expected xid1's status is not aborted, failed");
	}
	if (polar_xact_get_status(xid1) != XID_IN_PROGRESS)
	{
		elog(WARNING, "test 1.3: expected xid1's status is in progress, failed");
	}
	else
	{
		elog(NOTICE, "test 1.3: expected xid1's status is in progress, success");
	}

	if (!HeapTupleSatisfiesMVCC(tup, &snapshot1, buffer))
	{
		elog(WARNING, "test 1.4: expected HeapTupleSatisfiesMVCC returns true, failed");
	}

	if (HeapTupleSatisfiesUpdate(tup, 2, buffer) != TM_BeingModified)
	{
		elog(WARNING, "test 1.5: expected HeapTupleSatisfiesUpdate returns TM_BeingModified, failed");
	}

	if (commit)
	{
		TransactionIdCommitTree(xid1, 0, NULL);
		GetSnapshotData(&snapshot2);

		if (TransactionIdDidCommit(xid1))
		{
			elog(WARNING, "test 2.1: expected xid1's status is not committed, failed");
		}

		if (TransactionIdDidAbort(xid1))
		{
			elog(WARNING, "test 2.2: expected xid1's status is not aborted, failed");
		}

		if (polar_xact_get_status(xid1) != XID_IN_PROGRESS)
		{
			elog(WARNING, "test 2.3: expected xid1's status is in progess, failed");
		}
		else
		{
			elog(NOTICE, "test 2.3: expected xid1's status is in progess, success");
		}

		if (!HeapTupleSatisfiesMVCC(tup, &snapshot1, buffer))
		{
			elog(WARNING, "test 2.4: expected HeapTupleSatisfiesMVCC returns true, failed");
		}

		if (!HeapTupleSatisfiesMVCC(tup, &snapshot2, buffer))
		{
			elog(WARNING, "test 2.5: expected HeapTupleSatisfiesMVCC returns true, failed");
		}

		if (HeapTupleSatisfiesUpdate(tup, 2, buffer) != TM_BeingModified)
		{
			elog(WARNING, "test 2.6: expected heaptupleSatisfiesUpdate's returns TM_BeingModified, failed");
		}

		polar_xact_commit_tree_csn(xid1, 0, NULL, InvalidXLogRecPtr);
		GetSnapshotData(&snapshot3);

		if (TransactionIdDidAbort(xid1))
		{
			elog(WARNING, "test 3.1: expected xid1's status is not aborted, failed");
		}

		if (polar_xact_get_status(xid1) == XID_IN_PROGRESS)
		{
			elog(WARNING, "test 3.2: expected xid1's status is not in progess, failed");
		}

		if (!TransactionIdDidCommit(xid1))
		{
			elog(WARNING, "test 3.3: expected xid1's status is committed, failed");
		}
		else
		{
			elog(NOTICE, "test 3.3: expected xid1's status is committed, success");
		}

		if (!HeapTupleSatisfiesMVCC(tup, &snapshot1, buffer))
		{
			elog(WARNING, "test 3.4: expected HeapTupleSatisfiesMVCC returns true, failed");
		}

		if (!HeapTupleSatisfiesMVCC(tup, &snapshot2, buffer))
		{
			elog(WARNING, "test 3.5: expected HeapTupleSatisfiesMVCC returns true, failed");
		}

		if (HeapTupleSatisfiesMVCC(tup, &snapshot3, buffer))
		{
			elog(WARNING, "test 3.6: expected HeapTupleSatisfiesMVCC returns false, failed");
		}

		if (HeapTupleSatisfiesMVCC(tup, &snapshot3, buffer))
		{
			elog(WARNING, "test 3.7: expected HeapTupleSatisfiesMVCC returns false, failed");
		}

		if (HeapTupleSatisfiesUpdate(tup, 2, buffer) != TM_Deleted)
		{
			elog(WARNING, "test 3.8: expected HeapTupleSatisfiesUpdate's result is TM_Deleted, failed");
		}
	}
	else
	{
		TransactionIdAbortTree(xid1, 0, NULL);
		GetSnapshotData(&snapshot2);

		if (TransactionIdDidCommit(xid1))
		{
			elog(WARNING, "test 4.1: expected xid1's status is not committed, failed");
		}

		if (polar_xact_get_status(xid1) == XID_IN_PROGRESS)
		{
			elog(WARNING, "test 4.2: expected xid1's status is not in progess, failed");
		}

		if (!TransactionIdDidAbort(xid1))
		{
			elog(WARNING, "test 4.3: expected xid1's status is not aborted, failed");
		}
		else
		{
			elog(NOTICE, "test 4.3: expected xid1's status is aborted, success");
		}

		if (!HeapTupleSatisfiesMVCC(tup, &snapshot1, buffer))
		{
			elog(WARNING, "test 4.4: expected HeapTupleSatisfiesMVCC returns true, failed");
		}

		if (!HeapTupleSatisfiesMVCC(tup, &snapshot2, buffer))
		{
			elog(WARNING, "test 4.5: expected HeapTupleSatisfiesMVCC returns true, failed");
		}

		if (HeapTupleSatisfiesUpdate(tup, 2, buffer) != TM_Ok)
		{
			elog(WARNING, "test 4.7: expected HeapTupleSatisfiesUpdate's result is TM_Ok, failed");
		}
	}

	UnlockReleaseBuffer(buffer);
	heap_freetuple(tup);

	if (vmbuffer != InvalidBuffer)
		ReleaseBuffer(vmbuffer);

	/* Connect to SPI manager */
	if ((ret = SPI_connect()) < 0)
		/* internal error */
		elog(ERROR, "test_csn_xact: SPI_connect returned %d", ret);

	SPI_execute("DROP TABLE csn_xact_table_case3;", false, 0);

	SPI_finish();

	PG_RETURN_VOID();
}
