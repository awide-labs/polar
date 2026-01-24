/*-------------------------------------------------------------------------
 *
 * transam.c
 *	  postgres transaction (commit) log interface routines
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/transam/transam.c
 *
 * NOTES
 *	  This file contains the high level access-method interface to the
 *	  transaction system.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/clog.h"
#include "access/subtrans.h"
#include "access/transam.h"
#include "utils/snapmgr.h"

/* POLAR csn */
#include "access/polar_csnlog.h"
#include "access/polar_csn_mvcc_vars.h"
#include "storage/procarray.h"
#include "storage/proc.h"
#include "utils/guc.h"
/* POLAR end */

/*
 * Single-item cache for results of TransactionLogFetch.  It's worth having
 * such a cache because we frequently find ourselves repeatedly checking the
 * same XID, for example when scanning a table just after a bulk insert,
 * update, or delete.
 */
static TransactionId cachedFetchXid = InvalidTransactionId;
static XidStatus cachedFetchXidStatus;
static XLogRecPtr cachedCommitLSN;

/* POLAR: Single-item cache for results of CLogGetCommitLSN */
static TransactionId polar_cached_csn_xid = InvalidTransactionId;
static CommitSeqNo polar_cached_csn;

/* Local functions */
static XidStatus TransactionLogFetch(TransactionId transactionId);

/* POLAR csn */
static bool polar_xact_did_commit(TransactionId xid);
static bool polar_xact_did_abort(TransactionId xid);

static void polar_xact_abort_tree_csn(TransactionId xid, int nxids, TransactionId *xids);

/* POLAR end */


/* ----------------------------------------------------------------
 *		Postgres log access method interface
 *
 *		TransactionLogFetch
 * ----------------------------------------------------------------
 */

/*
 * TransactionLogFetch --- fetch commit status of specified transaction id
 */
static XidStatus
TransactionLogFetch(TransactionId transactionId)
{
	XidStatus	xidstatus;
	XLogRecPtr	xidlsn;

	/*
	 * Before going to the commit log manager, check our single item cache to
	 * see if we didn't just check the transaction status a moment ago.
	 */
	if (TransactionIdEquals(transactionId, cachedFetchXid))
		return cachedFetchXidStatus;

	/*
	 * Also, check to see if the transaction ID is a permanent one.
	 */
	if (!TransactionIdIsNormal(transactionId))
	{
		if (TransactionIdEquals(transactionId, BootstrapTransactionId))
			return TRANSACTION_STATUS_COMMITTED;
		if (TransactionIdEquals(transactionId, FrozenTransactionId))
			return TRANSACTION_STATUS_COMMITTED;
		return TRANSACTION_STATUS_ABORTED;
	}

	/*
	 * Get the transaction status.
	 */
	xidstatus = TransactionIdGetStatus(transactionId, &xidlsn);

	/*
	 * Cache it, but DO NOT cache status for unfinished or sub-committed
	 * transactions!  We only cache status that is guaranteed not to change.
	 */
	if (xidstatus != TRANSACTION_STATUS_IN_PROGRESS &&
		xidstatus != TRANSACTION_STATUS_SUB_COMMITTED)
	{
		cachedFetchXid = transactionId;
		cachedFetchXidStatus = xidstatus;
		cachedCommitLSN = xidlsn;
	}

	return xidstatus;
}

/* ----------------------------------------------------------------
 *						Interface functions
 *
 *		TransactionIdDidCommit
 *		TransactionIdDidAbort
 *		========
 *		   these functions test the transaction status of
 *		   a specified transaction id.
 *
 *		TransactionIdCommitTree
 *		TransactionIdAsyncCommitTree
 *		TransactionIdAbortTree
 *		========
 *		   these functions set the transaction status of the specified
 *		   transaction tree.
 *
 * See also TransactionIdIsInProgress, which once was in this module
 * but now lives in procarray.c.
 * ----------------------------------------------------------------
 */

/*
 * TransactionIdDidCommit
 *		True iff transaction associated with the identifier did commit.
 *
 * Note:
 *		Assumes transaction identifier is valid and exists in clog.
 */
bool							/* true if given transaction committed */
TransactionIdDidCommit(TransactionId transactionId)
{
	XidStatus	xidstatus;

	if (polar_csn_enable)
	{
		return polar_xact_did_commit(transactionId);
	}

	xidstatus = TransactionLogFetch(transactionId);

	/*
	 * If it's marked committed, it's committed.
	 */
	if (xidstatus == TRANSACTION_STATUS_COMMITTED)
		return true;

	/*
	 * If it's marked subcommitted, we have to check the parent recursively.
	 * However, if it's older than TransactionXmin, we can't look at
	 * pg_subtrans; instead assume that the parent crashed without cleaning up
	 * its children.
	 *
	 * Originally we Assert'ed that the result of SubTransGetParent was not
	 * zero. However with the introduction of prepared transactions, there can
	 * be a window just after database startup where we do not have complete
	 * knowledge in pg_subtrans of the transactions after TransactionXmin.
	 * StartupSUBTRANS() has ensured that any missing information will be
	 * zeroed.  Since this case should not happen under normal conditions, it
	 * seems reasonable to emit a WARNING for it.
	 */
	if (xidstatus == TRANSACTION_STATUS_SUB_COMMITTED)
	{
		TransactionId parentXid;

		if (TransactionIdPrecedes(transactionId, TransactionXmin))
			return false;
		parentXid = SubTransGetParent(transactionId);
		if (!TransactionIdIsValid(parentXid))
		{
			elog(WARNING, "no pg_subtrans entry for subcommitted XID %u",
				 transactionId);
			return false;
		}
		return TransactionIdDidCommit(parentXid);
	}

	/*
	 * It's not committed.
	 */
	return false;
}

/*
 * TransactionIdDidAbort
 *		True iff transaction associated with the identifier did abort.
 *
 * Note:
 *		Assumes transaction identifier is valid and exists in clog.
 */
bool							/* true if given transaction aborted */
TransactionIdDidAbort(TransactionId transactionId)
{
	XidStatus	xidstatus;

	if (polar_csn_enable)
	{
		return polar_xact_did_abort(transactionId);
	}

	xidstatus = TransactionLogFetch(transactionId);

	/*
	 * If it's marked aborted, it's aborted.
	 */
	if (xidstatus == TRANSACTION_STATUS_ABORTED)
		return true;

	/*
	 * If it's marked subcommitted, we have to check the parent recursively.
	 * However, if it's older than TransactionXmin, we can't look at
	 * pg_subtrans; instead assume that the parent crashed without cleaning up
	 * its children.
	 */
	if (xidstatus == TRANSACTION_STATUS_SUB_COMMITTED)
	{
		TransactionId parentXid;

		if (TransactionIdPrecedes(transactionId, TransactionXmin))
			return true;
		parentXid = SubTransGetParent(transactionId);
		if (!TransactionIdIsValid(parentXid))
		{
			/* see notes in TransactionIdDidCommit */
			elog(WARNING, "no pg_subtrans entry for subcommitted XID %u",
				 transactionId);
			return true;
		}
		return TransactionIdDidAbort(parentXid);
	}

	/*
	 * It's not aborted.
	 */
	return false;
}

/*
 * TransactionIdCommitTree
 *		Marks the given transaction and children as committed
 *
 * "xid" is a toplevel transaction commit, and the xids array contains its
 * committed subtransactions.
 *
 * This commit operation is not guaranteed to be atomic, but if not, subxids
 * are correctly marked subcommit first.
 */
void
TransactionIdCommitTree(TransactionId xid, int nxids, TransactionId *xids)
{
	TransactionIdSetTreeStatus(xid, nxids, xids,
							   TRANSACTION_STATUS_COMMITTED,
							   InvalidXLogRecPtr);
}

/*
 * TransactionIdAsyncCommitTree
 *		Same as above, but for async commits.  The commit record LSN is needed.
 */
void
TransactionIdAsyncCommitTree(TransactionId xid, int nxids, TransactionId *xids,
							 XLogRecPtr lsn)
{
	TransactionIdSetTreeStatus(xid, nxids, xids,
							   TRANSACTION_STATUS_COMMITTED, lsn);
}

/*
 * TransactionIdAbortTree
 *		Marks the given transaction and children as aborted.
 *
 * "xid" is a toplevel transaction commit, and the xids array contains its
 * committed subtransactions.
 *
 * We don't need to worry about the non-atomic behavior, since any onlookers
 * will consider all the xacts as not-yet-committed anyway.
 */
void
TransactionIdAbortTree(TransactionId xid, int nxids, TransactionId *xids)
{
	if (polar_csn_enable)
	{
		polar_xact_abort_tree_csn(xid, nxids, xids);
	}

	TransactionIdSetTreeStatus(xid, nxids, xids,
							   TRANSACTION_STATUS_ABORTED, InvalidXLogRecPtr);
}

/*
 * TransactionIdPrecedes --- is id1 logically < id2?
 */
bool
TransactionIdPrecedes(TransactionId id1, TransactionId id2)
{
	/*
	 * If either ID is a permanent XID then we can just do unsigned
	 * comparison.  If both are normal, do a modulo-2^32 comparison.
	 */
	int32		diff;

	if (!TransactionIdIsNormal(id1) || !TransactionIdIsNormal(id2))
		return (id1 < id2);

	diff = (int32) (id1 - id2);
	return (diff < 0);
}

/*
 * TransactionIdPrecedesOrEquals --- is id1 logically <= id2?
 */
bool
TransactionIdPrecedesOrEquals(TransactionId id1, TransactionId id2)
{
	int32		diff;

	if (!TransactionIdIsNormal(id1) || !TransactionIdIsNormal(id2))
		return (id1 <= id2);

	diff = (int32) (id1 - id2);
	return (diff <= 0);
}

/*
 * TransactionIdFollows --- is id1 logically > id2?
 */
bool
TransactionIdFollows(TransactionId id1, TransactionId id2)
{
	int32		diff;

	if (!TransactionIdIsNormal(id1) || !TransactionIdIsNormal(id2))
		return (id1 > id2);

	diff = (int32) (id1 - id2);
	return (diff > 0);
}

/*
 * TransactionIdFollowsOrEquals --- is id1 logically >= id2?
 */
bool
TransactionIdFollowsOrEquals(TransactionId id1, TransactionId id2)
{
	int32		diff;

	if (!TransactionIdIsNormal(id1) || !TransactionIdIsNormal(id2))
		return (id1 >= id2);

	diff = (int32) (id1 - id2);
	return (diff >= 0);
}


/*
 * TransactionIdLatest --- get latest XID among a main xact and its children
 */
TransactionId
TransactionIdLatest(TransactionId mainxid,
					int nxids, const TransactionId *xids)
{
	TransactionId result;

	/*
	 * In practice it is highly likely that the xids[] array is sorted, and so
	 * we could save some cycles by just taking the last child XID, but this
	 * probably isn't so performance-critical that it's worth depending on
	 * that assumption.  But just to show we're not totally stupid, scan the
	 * array back-to-front to avoid useless assignments.
	 */
	result = mainxid;
	while (--nxids >= 0)
	{
		if (TransactionIdPrecedes(result, xids[nxids]))
			result = xids[nxids];
	}
	return result;
}


/*
 * TransactionIdGetCommitLSN
 *
 * This function returns an LSN that is late enough to be able
 * to guarantee that if we flush up to the LSN returned then we
 * will have flushed the transaction's commit record to disk.
 *
 * The result is not necessarily the exact LSN of the transaction's
 * commit record!  For example, for long-past transactions (those whose
 * clog pages already migrated to disk), we'll return InvalidXLogRecPtr.
 * Also, because we group transactions on the same clog page to conserve
 * storage, we might return the LSN of a later transaction that falls into
 * the same group.
 */
XLogRecPtr
TransactionIdGetCommitLSN(TransactionId xid)
{
	XLogRecPtr	result;

	/*
	 * Currently, all uses of this function are for xids that were just
	 * reported to be committed by TransactionLogFetch, so we expect that
	 * checking TransactionLogFetch's cache will usually succeed and avoid an
	 * extra trip to shared memory.
	 */
	if (TransactionIdEquals(xid, cachedFetchXid))
		return cachedCommitLSN;

	/* Special XIDs are always known committed */
	if (!TransactionIdIsNormal(xid))
		return InvalidXLogRecPtr;

	/*
	 * Get the transaction status.
	 */
	(void) TransactionIdGetStatus(xid, &result);

	return result;
}

/* POLAR: snapshot based Commit Sequence Number */

/*
 * TransactionIdGetCommitSeqNo --- fetch CSN of specified transaction id
 *
 * committed: whether the xact if committed
 * snapCSN: snapshot CSN to check MVCC visiblity for committed xact
 *
 * if the upperbound CSN is less than it, just return upperbound CSN
 *
 * Replacing: TransactionLogFetch
 */
CommitSeqNo
polar_xact_get_csn(TransactionId transactionId, CommitSeqNo snapCSN,
				   bool committed)
{
	CommitSeqNo csn;
	XLogRecPtr	lsn;

	/*
	 * Before going to the commit log manager, check our single item cache to
	 * see if we didn't just check the transaction status a moment ago.
	 */
	if (TransactionIdEquals(transactionId, polar_cached_csn_xid))
		return polar_cached_csn;

	/*
	 * Also, check to see if the transaction ID is a permanent one.
	 */
	if (!TransactionIdIsNormal(transactionId))
	{
		if (TransactionIdEquals(transactionId, BootstrapTransactionId))
			return POLAR_CSN_FROZEN;
		if (TransactionIdEquals(transactionId, FrozenTransactionId))
			return POLAR_CSN_FROZEN;
		/* no cover line */
		return POLAR_CSN_ABORTED;
	}

	if (polar_csnlog_upperbound_enable)
	{
		if (committed)
		{
			CommitSeqNo upperBoundCSN = polar_csnlog_get_upperbound_csn(transactionId);

			if (upperBoundCSN < snapCSN)
			{
				polar_csnlog_count_upperbound_fetch(1, 1, 1);
				return upperBoundCSN;
			}
		}
		polar_csnlog_count_upperbound_fetch(1, committed ? 1 : 0, 0);
	}

	/*
	 * If the XID is older than TransactionXmin, check the clog. Otherwise
	 * check the csnlog.
	 */
	Assert(TransactionIdIsValid(TransactionXmin));
	if (TransactionIdPrecedes(transactionId, TransactionXmin))
	{
		if (TransactionIdGetStatus(transactionId, &lsn) == TRANSACTION_STATUS_COMMITTED)
			csn = POLAR_CSN_FROZEN;
		else
			csn = POLAR_CSN_ABORTED;

		polar_cached_csn_xid = transactionId;
		polar_cached_csn = csn;
		cachedFetchXid = transactionId;
		cachedCommitLSN = lsn;
	}
	else
	{
		csn = polar_csnlog_get_csn(transactionId);

		if (csn == POLAR_CSN_COMMITTING)
		{
			/*
			 * If the transaction is committing at this very instant, and
			 * hasn't set its CSN yet, wait for it to finish doing so.
			 *
			 * XXX: Alternatively, we could wait on the heavy-weight lock on
			 * the XID. that'd make TransactionIdCommitTree() slightly
			 * cheaper, as it wouldn't need to acquire CommitSeqNoLock (even
			 * in shared mode).
			 */
			/* no cover begin */
			LWLockAcquire(CommitSeqNoLock, LW_EXCLUSIVE);
			LWLockRelease(CommitSeqNoLock);

			csn = polar_csnlog_get_csn(transactionId);
			Assert(csn != POLAR_CSN_COMMITTING);
			/* no cover end */
		}

		/*
		 * Cache it, but DO NOT cache status for unfinished transactions! We
		 * only cache status that is guaranteed not to change.
		 */
		if (POLAR_CSN_IS_COMMITTED(csn) ||
			POLAR_CSN_IS_ABORTED(csn))
		{
			polar_cached_csn_xid = transactionId;
			polar_cached_csn = csn;
		}
	}


	return csn;
}

/*
 * polar_xact_did_commit
 *    True iff transaction associated with the identifier did commit.
 * Replacing: TransactionIdDidCommit
 */
bool
polar_xact_did_commit(TransactionId xid)
{
	CommitSeqNo csn;

	csn = polar_xact_get_csn(xid, POLAR_CSN_MAX_NORMAL, false);

	if (POLAR_CSN_IS_COMMITTED(csn))
		return true;
	else
		return false;
}

/*
 * polar_xact_did_abort
 *    True iff transaction associated with the identifier did abort.
 * Replacing:  TransactionIdDidAbort
 */
bool
polar_xact_did_abort(TransactionId xid)
{
	CommitSeqNo csn;

	csn = polar_xact_get_csn(xid, POLAR_CSN_MAX_NORMAL, false);

	if (POLAR_CSN_IS_ABORTED(csn))
		return true;
	else
		return false;
}

/*
 * polar_xact_get_status
 * 	Returns the status of the tranaction.
 *
 * Note that this treats a a crashed transaction as still in-progress,
 * until it falls off the xmin horizon.
 */

XidCommitStatus
polar_xact_get_status(TransactionId xid)
{
	CommitSeqNo csn;

	csn = polar_xact_get_csn(xid, POLAR_CSN_MAX_NORMAL, false);

	if (POLAR_CSN_IS_COMMITTED(csn))
		return XID_COMMITTED;
	else if (POLAR_CSN_IS_ABORTED(csn))
		return XID_ABORTED;
	else
		return XID_IN_PROGRESS;
}

static void
polar_update_latest_completed(TransactionId latestXid)
{
	uint64		currentLatestCompleted;
	TransactionId currentLatestCompletedXid;
	FullTransactionId currentLatestCompletedFullXid;
	FullTransactionId latestXidFull;

	currentLatestCompleted =
		pg_atomic_read_u64(&polar_shmem_csn_mvcc_var_cache->polar_latest_completed_xid);
	currentLatestCompletedFullXid = FullTransactionIdFromU64(currentLatestCompleted);
	currentLatestCompletedXid = XidFromFullTransactionId(currentLatestCompletedFullXid);
	while (TransactionIdFollows(latestXid, currentLatestCompletedXid))
	{
		uint32		epoch = EpochFromFullTransactionId(currentLatestCompletedFullXid);

		/*
		 * Since latestXid follows currentLatestCompletedXid is the same or
		 * greater
		 */
		if (latestXid < currentLatestCompletedXid)
			epoch++;
		latestXidFull = FullTransactionIdFromEpochAndXid(epoch, latestXid);
		if (pg_atomic_compare_exchange_u64(&polar_shmem_csn_mvcc_var_cache->polar_latest_completed_xid,
										   &currentLatestCompleted,
										   U64FromFullTransactionId(latestXidFull)))
			break;
		currentLatestCompletedFullXid = FullTransactionIdFromU64(currentLatestCompleted);
		currentLatestCompletedXid = XidFromFullTransactionId(currentLatestCompletedFullXid);
	}
}

/*
 * polar_xact_commit_tree_csn: async commits in csn log.
 * "xid" is a toplevel transaction commit, and the xids array contains its
 * committed subtransactions.
 */
void
polar_xact_commit_tree_csn(TransactionId xid, int nxids, TransactionId *xids,
						   XLogRecPtr lsn)
{
	CommitSeqNo csn;
	TransactionId latestXid;

	latestXid = TransactionIdLatest(xid, nxids, xids);

	/*
	 * Grab the CommitSeqNoLock, in shared mode. This is only used to provide
	 * a way for a concurrent transaction to wait for us to complete (see
	 * polar_xact_get_csn()).
	 *
	 * GetRunningTransactionData use this lock to block transaction commit
	 * when get running xacts from ProcArray
	 */
	LWLockAcquire(CommitSeqNoLock, LW_SHARED);

	/*
	 * First update latestCompletedXid to cover this xid. We do this before
	 * assigning a CSN, so that if someone acquires a new snapshot at the same
	 * time, the xmax it computes is sure to cover our XID.
	 */
	polar_update_latest_completed(latestXid);

	/*
	 * Mark our top transaction id as commit-in-progress.
	 */
	polar_csnlog_set_csn(xid, 0, NULL, POLAR_CSN_COMMITTING, lsn);

	/* Get our CSN and increment */
	csn = pg_atomic_fetch_add_u64(&polar_shmem_csn_mvcc_var_cache->polar_next_csn, 1);
	Assert(csn >= POLAR_CSN_FIRST_NORMAL);

	/* Stamp this XID (and sub-XIDs) with the CSN */
	polar_csnlog_set_csn(xid, nxids, xids, csn, lsn);

	/*
	 * We set MyPgXact when hold CommitSeqNoLock in share mode, see
	 * GetRunningTransactionData comment for detail reason.
	 *
	 * Need volatile access MyPgXact?
	 */
	MyProc->polar_csn = csn;

	LWLockRelease(CommitSeqNoLock);
}


/*
 * polar_xact_abort_tree_csn
 *    Marks the given transaction and children as aborted in csn log.
 *
 * "xid" is a toplevel transaction commit, and the xids array contains its
 * committed subtransactions.
 *
 * We don't need to worry about the non-atomic behavior, since any onlookers
 * will consider all the xacts as not-yet-committed anyway.
 */
void
polar_xact_abort_tree_csn(TransactionId xid, int nxids, TransactionId *xids)
{
	TransactionId latestXid;

	latestXid = TransactionIdLatest(xid, nxids, xids);

	polar_update_latest_completed(latestXid);

	polar_csnlog_set_csn(xid, nxids, xids, POLAR_CSN_ABORTED, InvalidXLogRecPtr);
}

/* POLAR end */
