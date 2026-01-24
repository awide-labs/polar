#include "postgres.h"
#include "utils/elog.h"
#include "utils/guc.h"
#include "utils/snapmgr.h"
#include "utils/snapshot.h"
#include "storage/lwlock.h"
#include "storage/procarray.h"
#include "storage/proc.h"
#include "access/transam.h"
#include "access/polar_csn_mvcc_vars.h"
#include "access/polar_csnlog.h"
#include "access/xlogutils.h"

PG_MODULE_MAGIC;

static struct SnapshotData TestSnapshotDataMVCC;

static void
set_next_xid_info(FullTransactionId xid)
{
	ShmemVariableCache->nextXid = xid;
}

static void
print_next_xid_info()
{
	elog(INFO, "xid info -- nextXid:%d",
		 XidFromFullTransactionId(ShmemVariableCache->nextXid));
}

static void
print_mvcc_info()
{
	elog(INFO, "mvcc info -- polar_oldest_active_xid:%d, polar_next_csn:" UINT64_FORMAT ", polar_latest_completed_xid:" UINT64_FORMAT,
		 pg_atomic_read_u32(&polar_shmem_csn_mvcc_var_cache->polar_oldest_active_xid),
		 pg_atomic_read_u64(&polar_shmem_csn_mvcc_var_cache->polar_next_csn),
		 pg_atomic_read_u64(&polar_shmem_csn_mvcc_var_cache->polar_latest_completed_xid));
}

static void
set_xmin_info(PGPROC *proc, TransactionId recent_xmin, TransactionId transaction_xmin,
			  TransactionId replication_slot_xmin, TransactionId replication_slot_catalog_xmin)
{
	RecentXmin = recent_xmin;
	TransactionXmin = transaction_xmin;
	ProcArraySetReplicationSlotXmin(replication_slot_xmin, replication_slot_catalog_xmin, false);
}

static void
print_xmin_info()
{
	TransactionId data_xmin;
	TransactionId catalog_xmin;

	ProcArrayGetReplicationSlotXmin(&data_xmin, &catalog_xmin);

	elog(INFO, "xmin info -- RecentXmin:%d, TransactionXmin:%d, replication_slot_xmin:%d, replication_slot_catalog_xmin:%d",
		 RecentXmin, TransactionXmin, data_xmin, catalog_xmin);
}

static void
set_pgxact_info(PGPROC *proc, TransactionId xid, TransactionId xmin, CommitSeqNo csn, uint8 vacuum_flags, int delayChckptFlags)
{
	PGPROC	   *pgproc = &ProcGlobal->allProcs[proc->pgprocno];

	pgproc->xid = xid;
	pgproc->xmin = xmin;
	pgproc->polar_csn = csn;
	pgproc->statusFlags = vacuum_flags;
	pgproc->subxidStatus.overflowed = false;
	pgproc->delayChkptFlags = delayChckptFlags;
	pgproc->subxidStatus.count = 0;

	ProcGlobal->xids[pgproc->pgxactoff] = xid;
}

static void
print_pgxact_info()
{
	elog(INFO, "pgxact info -- xid:%d, xmin:%d, polar_csn:" UINT64_FORMAT ", statusFlags:%d, overflowed:%d, delayChkptFlags:%d, nxids:%d",
		 MyProc->xid, MyProc->xmin, MyProc->polar_csn, MyProc->statusFlags,
		 MyProc->subxidStatus.overflowed, MyProc->delayChkptFlags, MyProc->subxidStatus.count);
}

static void
set_snapshot_info(Snapshot snapshot)
{
	snapshot->xmin = InvalidTransactionId;
	snapshot->xmax = InvalidTransactionId;
	snapshot->polar_snapshot_csn = InvalidCommitSeqNo;
	snapshot->polar_csn_xid_snapshot = false;
	snapshot->xcnt = 0;
	snapshot->subxcnt = 0;
	snapshot->suboverflowed = false;
	snapshot->whenTaken = 0;
	snapshot->lsn = InvalidXLogRecPtr;
}

static void
print_snapshot_info(Snapshot snapshot)
{
	if (snapshot->polar_snapshot_csn != InvalidCommitSeqNo)
	{
		if (snapshot->polar_csn_xid_snapshot)
		{
			int			i;

			elog(INFO, "snapshot info -- xmin:%d, polar_snapshot_csn:" UINT64_FORMAT ", xmax:%d, subxcnt:%d, suboverflowed:%d",
				 snapshot->xmin, snapshot->polar_snapshot_csn, snapshot->xmax, snapshot->subxcnt, snapshot->suboverflowed);

			elog(INFO, "subxids:");
			for (i = 0; i < snapshot->subxcnt; i++)
			{
				elog(INFO, "%d", snapshot->subxip[i]);
			}
		}
		else
			elog(INFO, "snapshot info -- xmin:%d, polar_snapshot_csn:" UINT64_FORMAT ", xmax:%d",
				 snapshot->xmin, snapshot->polar_snapshot_csn, snapshot->xmax);
	}
	else
	{
		int			i;

		elog(INFO, "snapshot info -- xmin:%d, xmax:%d, xcnt:%d, subxcnt:%d, suboverflowed:%d",
			 snapshot->xmin, snapshot->xmax, snapshot->xcnt, snapshot->subxcnt, snapshot->suboverflowed);

		elog(INFO, "xids:");
		for (i = 0; i < snapshot->xcnt; i++)
		{
			elog(INFO, "%d", snapshot->xip[i]);
		}

		elog(INFO, "subxids:");
		for (i = 0; i < snapshot->subxcnt; i++)
		{
			elog(INFO, "%d", snapshot->subxip[i]);
		}
	}
}

static void
print_info(bool next_xid, bool mvcc, bool xmin, bool pgxact)
{
	if (next_xid)
		print_next_xid_info();
	if (mvcc)
		print_mvcc_info();
	if (xmin)
		print_xmin_info();
	if (pgxact)
		print_pgxact_info();
}

static void
test_ProcArrayInitRecovery()
{
	TransactionId xid;

	elog(INFO, "------------------------------");
	elog(INFO, "%s", __FUNCTION__);

	xid = FirstNormalTransactionId;

	polar_csn_mvcc_var_cache_set(InvalidTransactionId, InvalidCommitSeqNo, InvalidFullTransactionId);
	polar_set_latestObservedXid(InvalidTransactionId);
	elog(INFO, "before init");
	print_info(false, true, false, false);
	elog(INFO, "latestObservedXid:%d", polar_get_latestObservedXid());
	/* In case of assert fail */
	standbyState = STANDBY_INITIALIZED;
	ProcArrayInitRecovery(xid + 1, xid);
	standbyState = STANDBY_DISABLED;
	elog(INFO, "after init");
	print_info(false, true, false, false);
	elog(INFO, "latestObservedXid:%d", polar_get_latestObservedXid());
}

static void
test_ProcArrayClearTransaction()
{
	TransactionId xid;
	CommitSeqNo csn;

	elog(INFO, "------------------------------");
	elog(INFO, "%s", __FUNCTION__);

	xid = FirstNormalTransactionId;
	csn = POLAR_CSN_FIRST_NORMAL;

	set_pgxact_info(MyProc, xid, xid, csn, 0, 0);
	set_xmin_info(MyProc, xid, xid, xid, xid);
	elog(INFO, "before clear");
	print_info(false, false, true, true);
	ProcArrayClearTransaction(MyProc);
	elog(INFO, "after clear");
	print_info(false, false, true, true);
}

static void
test_ProcArrayEndTransaction()
{
	TransactionId xid;

	elog(INFO, "------------------------------");
	elog(INFO, "%s", __FUNCTION__);

	xid = FirstNormalTransactionId;

	set_pgxact_info(MyProc, xid, InvalidTransactionId, InvalidCommitSeqNo, 0, DELAY_CHKPT_START);
	set_xmin_info(MyProc, xid, xid, xid, xid);
	polar_csn_mvcc_var_cache_set(xid, InvalidCommitSeqNo, InvalidFullTransactionId);
	set_next_xid_info(FullTransactionIdFromEpochAndXid(0, xid + 1));
	elog(INFO, "before xact end var info");
	print_info(true, true, true, true);
	ProcArrayEndTransaction(MyProc, xid);
	elog(INFO, "after xact end var info");
	print_info(true, true, true, true);
}

static void
test_AdvanceOldestActiveXidCSN()
{
	TransactionId xid1;
	TransactionId xid2;

	elog(INFO, "------------------------------");
	elog(INFO, "%s", __FUNCTION__);

	xid1 = FirstNormalTransactionId;
	xid2 = FirstNormalTransactionId + 1;

	/*
	 * case 1 test xid different with polar_oldest_active_xid, should do
	 * nothing
	 */
	polar_csn_mvcc_var_cache_shmem_init();
	set_pgxact_info(MyProc, xid2, InvalidTransactionId, InvalidCommitSeqNo, 0, DELAY_CHKPT_START);
	elog(INFO, "case 1");
	elog(INFO, "before advance");
	print_info(false, true, false, true);
	AdvanceOldestActiveXidCSNWrapper(xid2);
	elog(INFO, "after advance");
	print_info(false, true, false, false);

	/*
	 * case 2 test xid same with polar_oldest_active_xid and no other active
	 * xid
	 */
	polar_csn_mvcc_var_cache_set(xid1, InvalidCommitSeqNo, InvalidFullTransactionId);
	set_pgxact_info(MyProc, xid1, InvalidTransactionId, InvalidCommitSeqNo, 0, DELAY_CHKPT_START);
	set_next_xid_info(FullTransactionIdFromEpochAndXid(0, xid2));
	elog(INFO, "case 2");
	elog(INFO, "before advance");
	print_info(true, true, false, true);
	AdvanceOldestActiveXidCSNWrapper(xid1);
	elog(INFO, "after advance");
	print_info(false, true, false, false);

	/*
	 * case 3 test xid same with polar_oldest_active_xid and have other active
	 * xid
	 */
	polar_csn_mvcc_var_cache_set(xid1, InvalidCommitSeqNo, InvalidFullTransactionId);
	set_pgxact_info(MyProc, xid1, InvalidTransactionId, InvalidCommitSeqNo, 0, DELAY_CHKPT_START);
	set_next_xid_info(FullTransactionIdFromEpochAndXid(0, xid2 + 1));
	polar_csnlog_set_csn(xid2, 0, NULL, POLAR_CSN_INPROGRESS, InvalidXLogRecPtr);
	elog(INFO, "case 3");
	elog(INFO, "before advance");
	print_info(true, true, false, true);
	AdvanceOldestActiveXidCSNWrapper(xid1);
	elog(INFO, "after advance");
	print_info(false, true, false, false);
}

static void
test_GetSnapshotData()
{
	TransactionId xid1;
	TransactionId xid2;
	TransactionId xid3;
	CommitSeqNo csn1;
	CommitSeqNo csn2;

	elog(INFO, "------------------------------");
	elog(INFO, "%s", __FUNCTION__);

	xid1 = FirstNormalTransactionId;
	xid2 = xid1 + 1;
	xid3 = xid2 + 1;
	csn1 = POLAR_CSN_FIRST_NORMAL;
	csn2 = csn1 + 1;

	/* case 1 test csn snapshot */
	polar_csn_mvcc_var_cache_set(xid1, csn1, FullTransactionIdFromEpochAndXid(0, xid3));
	set_xmin_info(MyProc, InvalidTransactionId, InvalidTransactionId, InvalidTransactionId, InvalidTransactionId);
	set_pgxact_info(MyProc, InvalidTransactionId, InvalidTransactionId, InvalidCommitSeqNo, 0, DELAY_CHKPT_START);
	set_snapshot_info(&TestSnapshotDataMVCC);
	elog(INFO, "case 1");
	elog(INFO, "before get");
	print_info(false, true, true, true);
	print_snapshot_info(&TestSnapshotDataMVCC);
	GetSnapshotData(&TestSnapshotDataMVCC);
	elog(INFO, "after get");
	print_info(false, true, true, true);
	print_snapshot_info(&TestSnapshotDataMVCC);

	/* case 2 test csn xid snapshot */
	polar_csn_xid_snapshot = true;
	polar_csn_mvcc_var_cache_set(xid1, csn1, FullTransactionIdFromEpochAndXid(0, xid3));
	set_xmin_info(MyProc, InvalidTransactionId, InvalidTransactionId, InvalidTransactionId, InvalidTransactionId);
	set_pgxact_info(MyProc, InvalidTransactionId, InvalidTransactionId, InvalidCommitSeqNo, 0, DELAY_CHKPT_START);
	set_snapshot_info(&TestSnapshotDataMVCC);
	polar_csnlog_set_csn(xid2, 0, NULL, csn2, InvalidXLogRecPtr);
	polar_csnlog_set_csn(xid3, 0, NULL, POLAR_CSN_INPROGRESS, InvalidXLogRecPtr);
	elog(INFO, "case 2");
	elog(INFO, "before get");
	print_info(false, true, true, true);
	print_snapshot_info(&TestSnapshotDataMVCC);
	GetSnapshotData(&TestSnapshotDataMVCC);
	elog(INFO, "after get");
	polar_csn_xid_snapshot = false;
	print_info(false, true, true, true);
	print_snapshot_info(&TestSnapshotDataMVCC);

	/* case 3 test csn snapshot with old_snapshot_threshold enable */
	polar_csn_mvcc_var_cache_set(xid1, csn1, FullTransactionIdFromEpochAndXid(0, xid3));
	set_xmin_info(MyProc, InvalidTransactionId, InvalidTransactionId, InvalidTransactionId, InvalidTransactionId);
	set_pgxact_info(MyProc, InvalidTransactionId, InvalidTransactionId, InvalidCommitSeqNo, 0, DELAY_CHKPT_START);
	set_snapshot_info(&TestSnapshotDataMVCC);
	elog(INFO, "case 3");
	elog(INFO, "before get");
	print_snapshot_info(&TestSnapshotDataMVCC);
	elog(INFO, "snapshot extra info -- whenTaken:%d, lsn:%d",
		 (&TestSnapshotDataMVCC)->whenTaken ? 1 : 0, (&TestSnapshotDataMVCC)->lsn ? 1 : 0);
	old_snapshot_threshold = 0;
	GetSnapshotData(&TestSnapshotDataMVCC);
	old_snapshot_threshold = -1;
	elog(INFO, "after get");
	print_snapshot_info(&TestSnapshotDataMVCC);
	elog(INFO, "snapshot extra info -- whenTaken:%d, lsn:%d",
		 (&TestSnapshotDataMVCC)->whenTaken ? 1 : 0, (&TestSnapshotDataMVCC)->lsn ? 1 : 0);
}

static void
test_polar_csnlog_get_set_csn()
{
	TransactionId xid1;
	TransactionId xid2;
	TransactionId xid3;
	TransactionId xid4;
	CommitSeqNo csn;
	TransactionId subxids[3];

	elog(INFO, "------------------------------");
	elog(INFO, "%s", __FUNCTION__);

	xid1 = FirstNormalTransactionId;
	xid2 = 1023;
	xid3 = xid2 + 1;
	xid4 = xid3 + 1;
	subxids[0] = xid3;
	subxids[1] = xid4;
	csn = 10000;

	/* case 1 test normal case */
	set_xmin_info(MyProc, xid1, xid1, InvalidTransactionId, InvalidTransactionId);
	polar_csnlog_set_csn(xid2, 2, subxids, csn, InvalidXLogRecPtr);
	elog(INFO, "case 1");
	elog(INFO, "xid2:%d, csn2:" UINT64_FORMAT, xid2, polar_csnlog_get_csn(xid2));
	elog(INFO, "xid3:%d, csn3:" UINT64_FORMAT, xid3, polar_csnlog_get_csn(xid3));
	elog(INFO, "xid4:%d, csn4:" UINT64_FORMAT, xid4, polar_csnlog_get_csn(xid4));

	/* case 2 test get InvalidTransactionId */
	set_xmin_info(MyProc, InvalidTransactionId, InvalidTransactionId, InvalidTransactionId, InvalidTransactionId);
	elog(INFO, "case 2");
	elog(INFO, "xid:%d, csn:" UINT64_FORMAT, InvalidTransactionId, polar_csnlog_get_csn(InvalidTransactionId));

	/* case 3 test get FrozenTransactionId */
	set_xmin_info(MyProc, InvalidTransactionId, InvalidTransactionId, InvalidTransactionId, InvalidTransactionId);
	elog(INFO, "case 3");
	elog(INFO, "xid:%d, csn:" UINT64_FORMAT, FrozenTransactionId, polar_csnlog_get_csn(FrozenTransactionId));

	/* case 4 test get BootstrapTransactionId */
	set_xmin_info(MyProc, InvalidTransactionId, InvalidTransactionId, InvalidTransactionId, InvalidTransactionId);
	elog(INFO, "case 4");
	elog(INFO, "xid:%d, csn:" UINT64_FORMAT, BootstrapTransactionId, polar_csnlog_get_csn(BootstrapTransactionId));

	/* case 5 test subtrans */
	set_xmin_info(MyProc, xid1, xid1, InvalidTransactionId, InvalidTransactionId);
	polar_csnlog_set_csn(xid2, 0, NULL, POLAR_CSN_COMMITTING, InvalidXLogRecPtr);
	polar_csnlog_set_csn(xid3, 0, NULL, POLAR_CSN_INPROGRESS, InvalidXLogRecPtr);
	polar_csnlog_set_csn(xid4, 0, NULL, POLAR_CSN_INPROGRESS, InvalidXLogRecPtr);
	polar_csnlog_set_parent(xid3, xid2);
	polar_csnlog_set_parent(xid4, xid3);
	elog(INFO, "case 5");
	elog(INFO, "xid:%d, csn:" UINT64_FORMAT, xid4, polar_csnlog_get_csn(xid4));
}

static void
test_polar_csnlog_get_set_parent()
{
	TransactionId xid1;
	TransactionId xid2;

	elog(INFO, "------------------------------");
	elog(INFO, "%s", __FUNCTION__);

	xid1 = FirstNormalTransactionId;
	xid2 = xid1 + 1;

	set_xmin_info(MyProc, xid1, xid1, InvalidTransactionId, InvalidTransactionId);
	polar_csnlog_set_csn(xid2, 0, NULL, POLAR_CSN_INPROGRESS, InvalidXLogRecPtr);
	polar_csnlog_set_parent(xid2, xid1);

	elog(INFO, "child:%d, parent:%d", xid2, polar_csnlog_get_parent(xid2));
}

static void
test_polar_csnlog_get_next_active_xid()
{
	TransactionId xid1;
	TransactionId xid2;
	TransactionId xid3;
	CommitSeqNo csn;

	elog(INFO, "------------------------------");
	elog(INFO, "%s", __FUNCTION__);

	xid1 = FirstNormalTransactionId;
	xid2 = xid1 + 1;
	xid3 = xid2 + 1;
	csn = 10000;

	set_next_xid_info(FullTransactionIdFromEpochAndXid(0, xid3 + 1));
	polar_csnlog_set_csn(xid1, 0, NULL, csn, InvalidXLogRecPtr);
	polar_csnlog_set_csn(xid2, 0, NULL, csn, InvalidXLogRecPtr);
	polar_csnlog_set_csn(xid3, 0, NULL, POLAR_CSN_INPROGRESS, InvalidXLogRecPtr);

	print_next_xid_info();
	elog(INFO, "next active xid:%d", polar_csnlog_get_next_active_xid(xid1, xid3 + 1));
}

static void
test_polar_csnlog_get_running_xids()
{
	int			i;
	TransactionId xid1;
	TransactionId xid2;
	TransactionId xid3;
	CommitSeqNo csn1;
	CommitSeqNo csn2;
	CommitSeqNo csn3;
	int			max_xids = 3;
	int			nxids = 3;
	TransactionId xids[3];
	bool		overflowed;

	elog(INFO, "------------------------------");
	elog(INFO, "%s", __FUNCTION__);

	xid1 = FirstNormalTransactionId;
	xid2 = xid1 + 1;
	xid3 = xid2 + 1;
	csn1 = POLAR_CSN_FIRST_NORMAL;
	csn2 = csn1 + 1;
	csn3 = csn2 + 1;

	polar_csnlog_set_csn(xid1, 0, NULL, csn1, InvalidXLogRecPtr);
	polar_csnlog_set_csn(xid2, 0, NULL, POLAR_CSN_INPROGRESS, InvalidXLogRecPtr);
	polar_csnlog_set_csn(xid3, 0, NULL, csn3, InvalidXLogRecPtr);
	polar_csnlog_get_running_xids(xid1, xid3 + 1, csn1, max_xids, &nxids, xids, &overflowed);
	elog(INFO, "nxids:%d, overflowed:%d", nxids, overflowed);
	for (i = 0; i < nxids; i++)
	{
		elog(INFO, "xid:%d", xids[i]);
	}
}

static void
test_polar_csnlog_get_top()
{
	TransactionId xid1;
	TransactionId xid2;
	TransactionId xid3;

	elog(INFO, "------------------------------");
	elog(INFO, "%s", __FUNCTION__);

	xid1 = FirstNormalTransactionId;
	xid2 = xid1 + 1;
	xid3 = xid2 + 1;

	set_xmin_info(MyProc, xid1, xid1, InvalidTransactionId, InvalidTransactionId);
	polar_csnlog_set_csn(xid1, 0, NULL, POLAR_CSN_INPROGRESS, InvalidXLogRecPtr);
	polar_csnlog_set_csn(xid2, 0, NULL, POLAR_CSN_INPROGRESS, InvalidXLogRecPtr);
	polar_csnlog_set_csn(xid3, 0, NULL, POLAR_CSN_INPROGRESS, InvalidXLogRecPtr);
	polar_csnlog_set_parent(xid2, xid1);
	polar_csnlog_set_parent(xid3, xid2);

	elog(INFO, "child:%d, top:%d", xid3, polar_csnlog_get_top(xid3));
}

static void
test_polar_csnlog_extend_truncate()
{
	TransactionId xid;

	elog(INFO, "------------------------------");
	elog(INFO, "%s", __FUNCTION__);

	xid = 131072;

	set_xmin_info(MyProc, xid, xid, InvalidTransactionId, InvalidTransactionId);
	polar_csnlog_extend(xid, true);
	polar_csnlog_checkpoint();
	polar_csnlog_truncate(xid);

	elog(INFO, "xid:%d, csn:" UINT64_FORMAT, xid, polar_csnlog_get_csn(xid));
}

static void
test_polar_csnlog_zero_page_redo()
{
	TransactionId xid;

	elog(INFO, "------------------------------");
	elog(INFO, "%s", __FUNCTION__);

	xid = 1025;

	set_xmin_info(MyProc, xid, xid, InvalidTransactionId, InvalidTransactionId);
	polar_csnlog_zero_page_redo(0);
	elog(INFO, "xid:%d, csn:" UINT64_FORMAT, xid, polar_csnlog_get_csn(xid));
}

static void
test_csnlog_mgr()
{
	test_polar_csnlog_get_set_csn();

	test_polar_csnlog_get_set_parent();

	test_polar_csnlog_get_next_active_xid();

	test_polar_csnlog_get_running_xids();

	test_polar_csnlog_get_top();

	test_polar_csnlog_extend_truncate();

	test_polar_csnlog_zero_page_redo();
}

static void
test_snapshot_mgr()
{
	test_AdvanceOldestActiveXidCSN();

	test_ProcArrayInitRecovery();

	test_ProcArrayClearTransaction();

	test_ProcArrayEndTransaction();

	/* test_GetRecentGlobalDataXminCSN(); */

	test_GetSnapshotData();
}

PG_FUNCTION_INFO_V1(test_csn);

/*
 * SQL-callable entry point to perform all tests.
 */
Datum
test_csn(PG_FUNCTION_ARGS)
{
	polar_csn_enable = true;

	polar_csnlog_validate_dir();

	polar_csnlog_shmem_init();

	polar_csnlog_bootstrap();

	polar_csnlog_startup(FirstNormalTransactionId);

	test_snapshot_mgr();

	/* test last, because truncate test */
	test_csnlog_mgr();

	polar_csnlog_shutdown();

	polar_csn_enable = false;

	/* clear MyPgXact in case of assert fail */
	set_pgxact_info(MyProc, InvalidTransactionId, InvalidTransactionId, InvalidCommitSeqNo, 0, DELAY_CHKPT_START);

	PG_RETURN_VOID();
}
