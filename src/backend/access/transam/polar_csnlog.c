/*-------------------------------------------------------------------------
 *
 * polar_csnlog.c
 *		Tracking Commit-Sequence-Numbers and in-progress subtransactions
 *
 * The polar_csnlog manager is a pg_clog-like manager that stores the commit
 * sequence number, or parent transaction Id, for each transaction.  It is
 * a fundamental part of MVCC.
 *
 * The csnlog serves two purposes:
 *
 * 1. While a transaction is in progress, it stores the parent transaction
 * Id for each in-progress subtransaction. A main transaction has a parent
 * of InvalidTransactionId, and each subtransaction has its immediate
 * parent. The tree can easily be walked from child to parent, but not in
 * the opposite direction.
 *
 * 2. After a transaction has committed, it stores the Commit Sequence
 * Number of the commit.
 *
 * We can use the same structure for both, because we don't care about the
 * parent-child relationships subtransaction after commit.
 *
 * This code is based on clog.c, but the robustness requirements
 * are completely different from pg_clog, because we only need to remember
 * csn information for currently-open and recently committed
 * transactions.  Thus, there is no need to preserve data over a crash and
 * restart.
 *
 * There are no XLOG interactions since we do not care about preserving
 * data across crashes.  During database startup, we simply force the
 * currently-active page of CSNLOG to zeroes.
 *
 * Portions Copyright (c) 2021, Alibaba Group Holding Limited
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *
 * src/backend/access/transam/polar_csnlog.c
 *
 *-------------------------------------------------------------------------
 */

#include <sys/stat.h>
#include <unistd.h>

#include "access/polar_csnlog.h"
#include "access/polar_csn_mvcc_vars.h"
#include "access/slru.h"
#include "access/transam.h"
#include "access/xlog.h"
#include "access/xloginsert.h"
#include "access/xlogutils.h"
#include "catalog/pg_control.h"
#include "storage/fd.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "storage/proc.h"
#include "utils/snapmgr.h"
#include "miscadmin.h"
#include "pg_trace.h"
#include "port/atomics.h"
#include "utils/guc.h"

/*
 * Defines for CSNLOG page sizes.  A page is the same BLCKSZ as is used
 * everywhere else in Postgres.
 *
 * Note: because TransactionIds are 32 bits and wrap around at 0xFFFFFFFF,
 * CSNLOG page numbering also wraps around at 0xFFFFFFFF/CSNLOG_XACTS_PER_PAGE,
 * and CSNLOG segment numbering at
 * 0xFFFFFFFF/CLOG_XACTS_PER_PAGE/SLRU_PAGES_PER_SEGMENT.  We need take no
 * explicit notice of that fact in this module, except when comparing segment
 * and page numbers in polar_csnlog_truncate (see CSNLOGPagePrecedes).
 */

/* We store the commit LSN for each xid */
#define CSNLOG_XACTS_PER_PAGE (BLCKSZ / sizeof(CommitSeqNo))

#define CSNLOG_XACTS_PER_LSN_GROUP	32	/* keep this a power of 2 */
#define CSNLOG_LSNS_PER_PAGE	(CSNLOG_XACTS_PER_PAGE / CSNLOG_XACTS_PER_LSN_GROUP)

#define CSNLOG_XACTS_PER_UB_GROUP CSNLOG_XACTS_PER_PAGE
#define CSNLOG_UB_GROUPS ((MaxTransactionId + CSNLOG_XACTS_PER_UB_GROUP - 1) / \
		CSNLOG_XACTS_PER_UB_GROUP)

#define GetLSNIndex(slotno, xid)	((slotno) * CSNLOG_LSNS_PER_PAGE + \
	((xid) % (TransactionId) CSNLOG_XACTS_PER_PAGE) / CSNLOG_XACTS_PER_LSN_GROUP)

#define TransactionIdToPageNo(xid)	((xid) / (TransactionId) CSNLOG_XACTS_PER_PAGE)
#define TransactionIdToItemNo(xid)  ((xid) % (TransactionId) CSNLOG_XACTS_PER_PAGE)

#define TransactionIdToUBSlotNo(xid)	((xid) / (TransactionId) CSNLOG_XACTS_PER_UB_GROUP)

/* We allocate new log pages in batches */
#define BATCH_SIZE 128

#define CSNLOG_DIR                   "pg_csnlog"

/* POLAR: Check whether csnlog local file cache is enabled */
#define POLAR_ENABLE_CSNLOG_LOCAL_CACHE() (polar_csnlog_max_local_cache_segments > 0 && polar_enable_shared_storage_mode)

/*
 * Link to shared-memory data structures for csnlog control
 */
static SlruCtlData polar_csnlog_ctl;

/*
 * Link to shared-memory for csn upperbound cache
 */
static char *polar_upperbound_csn_ptr;

static bool polar_csnlog_page_precedes(int page1, int page2);
static int	polar_csnlog_zero_page(int pageno);
static void polar_csnlog_set_csn_by_page(TransactionId xid, int nsubxids,
										 TransactionId *subxids, CommitSeqNo csn,
										 XLogRecPtr lsn, int pageno);
static void polar_csnlog_set_csn_by_itemno(TransactionId xid, int slotno,
											int offset, CommitSeqNo csn,
											XLogRecPtr lsn);
static CommitSeqNo polar_csnlog_get_csn_by_itemno(TransactionId xid, int slotno, int itemno);

static CommitSeqNo polar_csnlog_get_csn_internal(TransactionId xid);
static CommitSeqNo polar_csnlog_get_csn_recursive(TransactionId xid);

static inline void polar_csnlog_set_upperbound_csn(TransactionId xid, CommitSeqNo csn);

static inline SlruCtl
polar_csnlog_get_ctl()
{
	return &polar_csnlog_ctl;
}

/*
 * polar_csnlog_set_csn
 *
 * Record CSN of transaction entries in the csn log for a
 * transaction and its subtransaction tree. Take care to ensure this is
 * efficient, and as atomic as possible.
 *
 * xid is a single xid to set CSN for. This will typically be the
 * top level transactionid for a top level commit or abort. It can
 * also be a subtransaction when we record transaction aborts.
 *
 * subxids is an array of xids of length nsubxids, representing subtransactions
 * in the tree of xid. In various cases nsubxids may be zero.
 *
 * csn is the commit sequence number of the transaction. It should be
 * InvalidCommitSeqNo for abort cases.
 *
 * Note: This doesn't guarantee atomicity. The caller can use the
 * POLAR_CSN_COMMITTING special value for that.
 */
void
polar_csnlog_set_csn(TransactionId xid, int sub_xid_num,
					 TransactionId *sub_xids, CommitSeqNo csn, XLogRecPtr lsn)
{
	int			sub_xid_idx;
	int			top_pageno;
	TransactionId top_xid;

	/*
	 * param check
	 */
	if (IsBootstrapProcessingMode())
		csn = POLAR_CSN_FROZEN;
	else
	{
		/*no cover begin*/
		if (csn == InvalidCommitSeqNo)
			elog(ERROR, "cannot mark transaction committed without CSN");
		/*no cover end*/
	}

	/*
	 * We set the status of child transaction before the status of parent
	 * transactions, so that another process can correctly determine the
	 * resulting status of a child transaction. See
	 * polar_csnlog_get_csn_recursive().
	 */
	top_xid = InvalidTransactionId;
	top_pageno = TransactionIdToPageNo(xid);
	sub_xid_idx = sub_xid_num - 1;
	do
	{
		int			current_pageno = top_pageno;
		int			sub_xid_num_on_page = 0;

		/* Collect sub xids on same page */
		for (; sub_xid_idx >= 0; sub_xid_idx--)
		{
			int			sub_xid_pageno = TransactionIdToPageNo(sub_xids[sub_xid_idx]);

			if (sub_xid_num_on_page == 0)
				current_pageno = sub_xid_pageno;

			if (current_pageno != sub_xid_pageno)
				break;

			sub_xid_num_on_page++;
		}

		/* top_xid can be invalid when main xid is not on current page */
		if (current_pageno == top_pageno)
		{
			Assert(top_xid == InvalidTransactionId);
			top_xid = xid;
		}

		polar_csnlog_set_csn_by_page(top_xid, sub_xid_num_on_page, sub_xids + sub_xid_idx + 1,
									 csn, lsn, current_pageno);
	}
	while (sub_xid_idx >= 0);

	if (top_xid == InvalidTransactionId)
	{
		/*
		 * No subxids were on the same page as the main xid; we have to update
		 * it separately
		 */
		polar_csnlog_set_csn_by_page(xid, 0, NULL, csn, lsn, top_pageno);
	}
}

/*
 * Record the final state of transaction entries in the csn log for
 * all entries on a single page.  Atomic only on this page.
 *
 * xid maybe invalid
 */
static void
polar_csnlog_set_csn_by_page(TransactionId xid, int nsubxids,
							 TransactionId *subxids, CommitSeqNo csn,
							 XLogRecPtr lsn, int pageno)
{
	SlruCtl		csnlog_ctl = polar_csnlog_get_ctl();
	int			slotno;
	int			i;

	/*
	 * We use share lock for high performance, but it depend on 64 bit atomic
	 * ops. We have only 8 bytes for csn, so we can not use 64 bit simulation
	 * mode
	 */
#if !defined(PG_HAVE_ATOMIC_U64_SIMULATION)
	LWLockAcquire(CSNLogControlLock, LW_SHARED);
	slotno = SimpleLruReadPage_ReadOnly_Locked(csnlog_ctl, pageno, xid);
#else
	LWLockAcquire(CSNLogControlLock, LW_EXCLUSIVE);
	slotno = SimpleLruReadPage(csnlog_ctl, pageno, true, xid);
#endif

	/*
	 * We set the status of child transaction before the status of parent
	 * transactions, so that another process can correctly determine the
	 * resulting status of a child transaction. See
	 * polar_csnlog_get_csn_recursive().
	 */
	for (i = nsubxids - 1; i >= 0; i--)
	{
		Assert(csnlog_ctl->shared->page_number[slotno] == TransactionIdToPageNo(subxids[i]));
		polar_csnlog_set_upperbound_csn(subxids[i], csn);
		pg_write_barrier();
		polar_csnlog_set_csn_by_itemno(subxids[i], slotno, TransactionIdToItemNo(subxids[i]), csn, lsn);
		pg_write_barrier();
	}

	if (TransactionIdIsValid(xid))
	{
		polar_csnlog_set_upperbound_csn(xid, csn);
		pg_write_barrier();
		polar_csnlog_set_csn_by_itemno(xid, slotno, TransactionIdToItemNo(xid), csn, lsn);
	}

	csnlog_ctl->shared->page_dirty[slotno] = true;

	LWLockRelease(CSNLogControlLock);
}



/*
 * Record the parent of a subtransaction in CSN log.
 *
 * In some cases we may need to overwrite an existing value.
 *
 * Logic is almost same with SubTransSetParent,
 * but we try to get shared lock on CSNLogControlLock
 */
void
polar_csnlog_set_parent(TransactionId xid, TransactionId parent)
{
	SlruCtl		csnlog_ctl = polar_csnlog_get_ctl();
	int			pageno = TransactionIdToPageNo(xid);
	int			itemno = TransactionIdToItemNo(xid);
	int			slotno;
	CommitSeqNo old_csn;
	CommitSeqNo new_csn;

	/* Called in bootstrap mode is impossible */
	Assert(TransactionIdIsNormal(xid));
	Assert(TransactionIdIsNormal(parent));
	Assert(TransactionIdFollows(xid, parent));

	new_csn = POLAR_CSN_SUBTRANS_BIT | (uint64) parent;

	/*
	 * CSNLogControlLock is acquired by SimpleLruReadPage_ReadOnly.
	 *
	 * Shared page access is enough to set the subtransaction parent. It is
	 * set when the subtransaction is assigned an xid, and can be read only
	 * later after the subtransaction have modified some tuples.
	 */
	slotno = SimpleLruReadPage_ReadOnly(csnlog_ctl, pageno, xid);

	/*
	 * It's possible we'll try to set the parent xid multiple times.
	 * And in 2pc or standby, we may be changing the xid from direct xid to
	 * top xid
	 *
	 * In RW, the order must be:
	 * 1. set subtrans parent from assign txid call
	 * 2. abort subtrans
	 * 3. commit/abort parent trans
	 * these order lead to subtrans csnlog status transition as such:
	 * in progress -> aborted
	 *
	 * In RO, the order may be:
	 * 1. redo abort subtrans (subtrans abort write abort wal in RW)
	 * 2. redo set subtrans parent from assignment wal (subtrans abort does not remove xid from assignment in RW)
	 * 3. redo commit/abort parent trans (parent commit/abort does not have aborted subtrans in wal in RW)
	 * these order lead to subtrans csnlog status transition as such:
	 * in progress -> aborted -> subtrans
	 * We should not permit aborted/committed/committing status transfer to subtrans.
	 * When this happens, just ignore
	 */
	old_csn = polar_csnlog_get_csn_by_itemno(xid, slotno, itemno);
	if (POLAR_CSN_IS_INPROGRESS(old_csn) ||
		(POLAR_CSN_IS_SUBTRANS(old_csn) && old_csn != new_csn))
	{
		polar_csnlog_set_csn_by_itemno(xid, slotno, itemno, new_csn, InvalidXLogRecPtr);
		csnlog_ctl->shared->page_dirty[slotno] = true;
	}

	LWLockRelease(CSNLogControlLock);
}

/*
 * Interrogate the parent of a transaction in the csnlog.
 */
TransactionId
polar_csnlog_get_parent(TransactionId xid)
{
	CommitSeqNo csn;

	LWLockAcquire(CSNLogControlLock, LW_SHARED);

	csn = polar_csnlog_get_csn_internal(xid);

	LWLockRelease(CSNLogControlLock);

	if (POLAR_CSN_IS_SUBTRANS(csn))
		return (TransactionId) (csn & 0xFFFFFFFF);
	else
		return InvalidTransactionId;
}

/*
 * polar_csnlog_get_top
 *
 * Returns the topmost transaction of the given transaction id.
 *
 * Because we cannot look back further than TransactionXmin, it is possible
 * that this function will lie and return an intermediate subtransaction ID
 * instead of the true topmost parent ID.  This is OK, because in practice
 * we only care about detecting whether the topmost parent is still running
 * or is part of a current snapshot's list of still-running transactions.
 * Therefore, any XID before TransactionXmin is as good as any other.
 *
 * Logic is same with SubTransGetTopmostTransaction
 */
TransactionId
polar_csnlog_get_top(TransactionId xid)
{
	TransactionId parentXid = xid,
				previousXid = xid;

	/* Can't ask about stuff that might not be around anymore */
	Assert(TransactionIdFollowsOrEquals(xid, TransactionXmin));

	while (TransactionIdIsValid(parentXid))
	{
		previousXid = parentXid;
		if (TransactionIdPrecedes(parentXid, TransactionXmin))
			break;
		parentXid = polar_csnlog_get_parent(parentXid);

		/*
		 * By convention the parent xid gets allocated first, so should always
		 * precede the child xid. Anything else points to a corrupted data
		 * structure that could lead to an infinite loop, so exit.
		 */
		/*no cover begin*/
		if (!TransactionIdPrecedes(parentXid, previousXid))
			elog(ERROR, "pg_csnlog contains invalid entry: xid %u points to parent xid %u",
				 previousXid, parentXid);
		/*no cover end*/
	}

	Assert(TransactionIdIsValid(previousXid));

	return previousXid;
}

/*
 * Set status or csn of a single transaction atomically.
 *
 * For performance, we only share lock CSNLogControlLock
 * when we support 64 bit atomic ops:
 * 1. one write/multi read csn concurrency may be happen, we should make sure
 *    write or read csn is atomic
 * 2. write/write csn concurrency is impossible
 */
static void
polar_csnlog_set_csn_by_itemno(TransactionId xid, int slotno, int itemno,
							   CommitSeqNo new_csn, XLogRecPtr lsn)
{
	SlruCtl		csnlog_ctl = polar_csnlog_get_ctl();
	volatile	CommitSeqNo *csn_ptr;

	csn_ptr = (CommitSeqNo *) csnlog_ctl->shared->page_buffer[slotno];
	csn_ptr += itemno;

#if !defined(PG_HAVE_ATOMIC_U64_SIMULATION)
	pg_atomic_write_u64((volatile pg_atomic_uint64 *) csn_ptr, new_csn);
#else
	*csn_ptr = new_csn;
#endif

	/*
	 * Update the group LSN if the transaction completion LSN is higher.
	 *
	 * Note: lsn will be invalid when supplied during InRecovery processing,
	 * so we don't need to do anything special to avoid LSN updates during
	 * recovery. After recovery completes the next clog change will set the
	 * LSN correctly.
	 */
	if (!XLogRecPtrIsInvalid(lsn))
	{
		int			lsnindex = GetLSNIndex(slotno, xid);

#if !defined(PG_HAVE_ATOMIC_U64_SIMULATION)
		pg_atomic_uint64 *lsn_ptr = (pg_atomic_uint64 *)
		&csnlog_ctl->shared->group_lsn[lsnindex];
		XLogRecPtr	old_lsn = pg_atomic_read_u64(lsn_ptr);

		while (old_lsn < lsn)
		{
			if (pg_atomic_compare_exchange_u64(lsn_ptr, &old_lsn, lsn))
				break;
		}
#else
		if (csnlog_ctl->shared->group_lsn[lsnindex] < lsn)
			csnlog_ctl->shared->group_lsn[lsnindex] = lsn;
#endif
	}
}

/*
 * Get status or csn of a single transaction atomically.
 *
 * For performance, we may hold share CSNLogControlLock
 * when we support 64 bit atomic ops.
 * Use atomic ops to access in that case.
 */
static CommitSeqNo
polar_csnlog_get_csn_by_itemno(TransactionId xid, int slotno, int itemno)
{
	SlruCtl		csnlog_ctl = polar_csnlog_get_ctl();
	CommitSeqNo *csn_ptr;

	csn_ptr = (CommitSeqNo *) csnlog_ctl->shared->page_buffer[slotno];
	csn_ptr += itemno;

#if !defined(PG_HAVE_ATOMIC_U64_SIMULATION)
	return pg_atomic_read_u64((volatile pg_atomic_uint64 *) csn_ptr);
#else
	return *csn_ptr;
#endif
}

/*
 * Interrogate the state of a transaction in the csn log.
 *
 * NB: this is a low-level routine and is NOT the preferred entry point
 * for most uses; TransactionIdGetCommitSeqNo() in transam.c is the intended caller.
 */
CommitSeqNo
polar_csnlog_get_csn(TransactionId xid)
{
	CommitSeqNo csn;

	LWLockAcquire(CSNLogControlLock, LW_SHARED);

	csn = polar_csnlog_get_csn_recursive(xid);

	LWLockRelease(CSNLogControlLock);

	return csn;
}

/*
 * Determine the CSN of a transaction, walking the subtransaction tree if needed.
 *
 * Hold shared CSNLogControlLock.
 */
static CommitSeqNo
polar_csnlog_get_csn_recursive(TransactionId xid)
{
	CommitSeqNo csn;

	Assert(LWLockHeldByMeInMode(CSNLogControlLock, LW_SHARED));

	csn = polar_csnlog_get_csn_internal(xid);

	if (POLAR_CSN_IS_SUBTRANS(csn))
	{
		TransactionId parent_xid = csn & ~POLAR_CSN_SUBTRANS_BIT;
		CommitSeqNo parent_csn = polar_csnlog_get_csn_recursive(parent_xid);

		Assert(!POLAR_CSN_IS_SUBTRANS(parent_csn));

		/*
		 * The parent and child transaction status update is not atomic. We
		 * must take care not to use the updated parent status with the old
		 * child status, or else we can wrongly see a committed subtransaction
		 * as aborted. This happens when the parent is already marked as
		 * committed and the child is not yet marked.
		 */
		pg_read_barrier();
		csn = polar_csnlog_get_csn_internal(xid);

		if (POLAR_CSN_IS_SUBTRANS(csn))
		{
			/*
			 * We set status or csn in xid descend order(POLAR_CSN_COMMITTING
			 * is only set on main xid). If child is still subtrans, then
			 * parent_csn found before must be POLAR_CSN_COMMITTING or
			 * POLAR_CSN_INPROGRESS. So as child csn.
			 */
			if (POLAR_CSN_IS_INPROGRESS(parent_csn))
				csn = POLAR_CSN_INPROGRESS;
			else if (POLAR_CSN_IS_COMMITTING(parent_csn))
				csn = POLAR_CSN_COMMITTING;
			else
			{
				/*no cover begin*/
				if (polar_csn_elog_panic_enable)
					elog(PANIC, "Wrong csn state, xid: %u, parent_xid: %u, csn: "UINT64_FORMAT, xid, parent_xid, parent_csn);
				else
					elog(LOG, "Wrong csn state, xid: %u, parent_xid: %u, csn: "UINT64_FORMAT, xid, parent_xid, parent_csn);
				/*no cover end*/
			}
		}
	}

	return csn;
}

/*
 * Get the raw CSN value.
 *
 * Logic is almost same with SubTransGetParent,
 * but we have two differences:
 * 1. we return CommitSeqNo not TransactionId
 * 2. we hold share CSNLogControlLock when called in
 *    and we try best to hold share CSNLogControlLock
 *    when exit
 */
static CommitSeqNo
polar_csnlog_get_csn_internal(TransactionId xid)
{
	SlruCtl		csnlog_ctl = polar_csnlog_get_ctl();
	int			pageno = TransactionIdToPageNo(xid);
	int			itemno = TransactionIdToItemNo(xid);
	int			slotno;

	/* Can't ask about stuff that might not be around anymore */
	Assert(TransactionIdFollowsOrEquals(xid, TransactionXmin));

	if (!TransactionIdIsNormal(xid))
	{
		if (xid == InvalidTransactionId)
			return POLAR_CSN_ABORTED;
		if (xid == FrozenTransactionId || xid == BootstrapTransactionId)
			return POLAR_CSN_FROZEN;
	}

	slotno = SimpleLruReadPage_ReadOnly_Locked(csnlog_ctl, pageno, xid);

	return polar_csnlog_get_csn_by_itemno(xid, slotno, itemno);
}

/*
 * Find the next xid that is in progress.
 * We do not care about the subtransactions, they are accounted for
 * by their respective top-level transactions.
 */
TransactionId
polar_csnlog_get_next_active_xid(TransactionId xid,
								 TransactionId end)
{
	SlruCtl		csnlog_ctl = polar_csnlog_get_ctl();

	Assert(TransactionIdIsValid(TransactionXmin));

	LWLockAcquire(CSNLogControlLock, LW_SHARED);

	for (;;)
	{
		int			pageno;
		int			slotno;
		int			itemno;
		CommitSeqNo *csn_ptr;
		CommitSeqNo csn;

		if (!TransactionIdPrecedes(xid, end))
			goto end;

		pageno = TransactionIdToPageNo(xid);
		itemno = TransactionIdToItemNo(xid);
		slotno = SimpleLruReadPage_ReadOnly_Locked(csnlog_ctl, pageno, xid);

		csn_ptr = (CommitSeqNo *) csnlog_ctl->shared->page_buffer[slotno];
		csn_ptr += itemno;

		/* make sure csn unchanged */
#if !defined(PG_HAVE_ATOMIC_U64_SIMULATION)
		csn = pg_atomic_read_u64((volatile pg_atomic_uint64 *) csn_ptr);
#else
		csn = *csn_ptr;
#endif

		if (POLAR_CSN_IS_INPROGRESS(csn)
			|| POLAR_CSN_IS_COMMITTING(csn))
		{
			goto end;
		}

		TransactionIdAdvance(xid);
	}

end:
	LWLockRelease(CSNLogControlLock);

	return xid;
}

/*
 * Report shared-memory space needed by CSN upperbound stat.
 */
static inline Size
polar_csnlog_upperbound_csn_shmem_size(void)
{
	return sizeof(CommitSeqNo) * CSNLOG_UB_GROUPS + sizeof(polar_csnlog_ub_stat);
}

static inline CommitSeqNo*
polar_csnlog_get_upperbound_csn_ptr(int slotno)
{
	return (CommitSeqNo *)(polar_upperbound_csn_ptr + sizeof(CommitSeqNo) * slotno);
}

polar_csnlog_ub_stat*
polar_csnlog_get_upperbound_stat_ptr(void)
{
	return (polar_csnlog_ub_stat*)(polar_upperbound_csn_ptr +
										sizeof(CommitSeqNo) * CSNLOG_UB_GROUPS);
}

static inline void
polar_csnlog_upperbound_csn_init(void)
{
	int	slotno;
	CommitSeqNo	*csn_ptr;
	polar_csnlog_ub_stat *ub_stat;
	for(slotno = 0; slotno < CSNLOG_UB_GROUPS; slotno++)
	{
		csn_ptr = polar_csnlog_get_upperbound_csn_ptr(slotno);
		*csn_ptr = POLAR_CSN_FROZEN;
	}
	ub_stat = polar_csnlog_get_upperbound_stat_ptr();
	memset(ub_stat, 0, sizeof(polar_csnlog_ub_stat));
}

static inline void
polar_csnlog_set_upperbound_csn(TransactionId xid, CommitSeqNo csn)
{
#if !defined(PG_HAVE_ATOMIC_U64_SIMULATION)
	if (polar_csnlog_upperbound_enable && POLAR_CSN_IS_COMMITTED(csn))
	{
		int slotno =TransactionIdToUBSlotNo(xid);
		pg_atomic_uint64 *csn_ptr = (pg_atomic_uint64 *)
							polar_csnlog_get_upperbound_csn_ptr(slotno);
		CommitSeqNo old_csn = pg_atomic_read_u64(csn_ptr);
		while (old_csn < csn)
		{
			if (pg_atomic_compare_exchange_u64(csn_ptr, &old_csn, csn))
				break;
		}
	}
#endif
}

/*no cover begin*/
CommitSeqNo
polar_csnlog_get_upperbound_csn(TransactionId xid)
{
#if !defined(PG_HAVE_ATOMIC_U64_SIMULATION)
	int slotno =TransactionIdToUBSlotNo(xid);
	pg_atomic_uint64 *csn_ptr = (pg_atomic_uint64 *)
							polar_csnlog_get_upperbound_csn_ptr(slotno);
	return pg_atomic_read_u64(csn_ptr);
#endif
	return POLAR_CSN_MAX_NORMAL;
}

void
polar_csnlog_count_upperbound_fetch(int t_all_fetches, int t_ub_fetches, int t_ub_hits)
{
	polar_csnlog_ub_stat *ub_stat = polar_csnlog_get_upperbound_stat_ptr();

	pg_atomic_fetch_add_u64(&ub_stat->t_all_fetches, t_all_fetches);
	if (t_ub_fetches > 0)
		pg_atomic_fetch_add_u64(&ub_stat->t_ub_fetches, t_ub_fetches);
	if (t_ub_hits > 0)
		pg_atomic_fetch_add_u64(&ub_stat->t_ub_hits, t_ub_hits);
}
/*no cover end*/

/*
 *	Search xids from start(inclusive) to end(exclusive),
 *	add running xids(in_progress or committing or committed csn >= csn arg)
 *  to xid array. If array overflowed, set overflow flag
 */
void polar_csnlog_get_running_xids(TransactionId start, TransactionId end, CommitSeqNo snapshot_csn,
								   int max_xids, int *nxids, TransactionId *xids, bool *overflowed)
{
	SlruCtl		csnlog_ctl = polar_csnlog_get_ctl();

	Assert(TransactionIdPrecedes(start, end));

	LWLockAcquire(CSNLogControlLock, LW_SHARED);

	*nxids = 0;
	*overflowed = false;
	for (;;)
	{
		int			pageno;
		int			slotno;
		int			itemno;

		pageno = TransactionIdToPageNo(start);
		slotno = SimpleLruReadPage_ReadOnly_Locked(csnlog_ctl, pageno, start);
		itemno = TransactionIdToItemNo(start);
		for (; itemno < CSNLOG_XACTS_PER_PAGE; itemno++)
		{
			CommitSeqNo *csn_ptr;
			CommitSeqNo csn;

			csn_ptr = (CommitSeqNo *) csnlog_ctl->shared->page_buffer[slotno];
			csn_ptr += itemno;

			/* make sure csn unchanged */
#if !defined(PG_HAVE_ATOMIC_U64_SIMULATION)
			csn = pg_atomic_read_u64((volatile pg_atomic_uint64 *) csn_ptr);
#else
			csn = *csn_ptr;
#endif

			/*
			 * In in_progress and committing status, csn must >= ours,
			 * see these as running also.
			 */
			if (POLAR_CSN_IS_INPROGRESS(csn) ||
				POLAR_CSN_IS_COMMITTING(csn) ||
				(POLAR_CSN_IS_COMMITTED(csn) && csn >= snapshot_csn))
			{
				/* We are overflowed */
				if (*nxids == max_xids)
				{
					*overflowed = true;

					LWLockRelease(CSNLogControlLock);

					/* It is not a common case, log it */
					elog(LOG, "xid snapshot overflowed in tx %d", MyProc->xid);

					return;
				}

				xids[*nxids] = start;
				*nxids = *nxids + 1;
			}

			TransactionIdAdvance(start);

			/* Reach end */
			if (TransactionIdEquals(start, end))
				goto end;

			/* Need swich page */
			if (pageno != TransactionIdToPageNo(start))
			{
				break;
			}
		}
	}

end:
	LWLockRelease(CSNLogControlLock);
}

/*
 * Number of shared CSNLOG buffers.
 */
Size
polar_csnlog_shmem_buffers(void)
{
	return Max(polar_csnlog_slot_size, BATCH_SIZE);
}

/*
 * Initialization of shared memory for CSNLOG
 */
Size
polar_csnlog_shmem_size(void)
{
	Size size;

	size = SimpleLruShmemSize(polar_csnlog_shmem_buffers(), CSNLOG_LSNS_PER_PAGE);

	if (polar_csnlog_upperbound_enable)
		size = add_size(MAXALIGN(size), polar_csnlog_upperbound_csn_shmem_size());

	/* Add size for local cache segments */
	if (POLAR_ENABLE_CSNLOG_LOCAL_CACHE())
		size = add_size(MAXALIGN(size), polar_local_cache_shmem_size(polar_csnlog_max_local_cache_segments));

	return size;
}

void
polar_csnlog_shmem_init(void)
{
	SlruCtl	csnlog_ctl = polar_csnlog_get_ctl();

	if (polar_csnlog_upperbound_enable)
	{
		bool	found;
		Size	size = polar_csnlog_upperbound_csn_shmem_size();

		polar_upperbound_csn_ptr = ShmemInitStruct("CSN upperbound shared", size, &found);

		if (!found)
			polar_csnlog_upperbound_csn_init();
	}

	csnlog_ctl->PagePrecedes = polar_csnlog_page_precedes;
	SimpleLruInit(csnlog_ctl, "csnlog", polar_csnlog_shmem_buffers(), CSNLOG_LSNS_PER_PAGE,
				  CSNLogControlLock, CSNLOG_DIR, LWTRANCHE_CSNLOG_BUFFERS, SYNC_HANDLER_NONE,
				  polar_slru_file_in_shared_storage(true));

	/* Create local segment file cache manager */
	if (POLAR_ENABLE_CSNLOG_LOCAL_CACHE())
	{
		uint32 io_permission = POLAR_CACHE_LOCAL_FILE_READ | POLAR_CACHE_LOCAL_FILE_WRITE;
		polar_local_cache cache;

		if (!polar_is_replica())
			io_permission |= (POLAR_CACHE_SHARED_FILE_READ | POLAR_CACHE_SHARED_FILE_WRITE);

		cache = polar_create_local_cache("csnlog", "pg_csnlog",
			polar_csnlog_max_local_cache_segments, (SLRU_PAGES_PER_SEGMENT * BLCKSZ), LWTRANCHE_POLAR_CSNLOG_LOCAL_CACHE,
			io_permission, NULL);

		polar_slru_reg_local_cache(polar_csnlog_get_ctl(), cache);
	}
}

/*
 * Verify whether pg_csnlog exist. If not exist, recreate it.
 */
void
polar_csnlog_validate_dir(void)
{
	struct stat stat_buf;
	char		path[MAXPGPATH];

	snprintf((path), MAXPGPATH, "%s/%s", polar_enable_shared_storage_mode ?
																polar_datadir : DataDir, CSNLOG_DIR);

	if (polar_enable_shared_storage_mode && polar_mount_pfs_readonly_mode)
		return ;

	if (polar_stat(path, &stat_buf) == 0)
	{
		/*no cover begin*/
		if (!S_ISDIR(stat_buf.st_mode))
			ereport(FATAL,
					(errmsg("required csnlog directory \"%s\" is not a directory", path)));
		/*no cover end*/
	}
	else
	{
		/*no cover begin*/
		ereport(LOG,
				(errmsg("creating missing csnlog directory \"%s\"",
						path)));

		if (polar_make_pg_directory(path) < 0)
			ereport(FATAL,
					(errmsg("could not create csnlog directory \"%s\": %m",
							path)));
		/*no cover end*/
	}
}

/*
 * rm the files in pg_csnlog.
 */
void
polar_csnlog_remove_all(void)
{
	char		path[MAXPGPATH];
	DIR		  *csnlog_dir;
	struct dirent *csnlog_de;

	snprintf((path), MAXPGPATH, "%s/%s", polar_enable_shared_storage_mode ?
																polar_datadir : DataDir, CSNLOG_DIR);
	csnlog_dir = AllocateDir(path);

	/*no cover begin*/
	if (csnlog_dir == NULL)
		return;
	/*no cover end*/

	while ((csnlog_de = ReadDir(csnlog_dir, path)) != NULL)
	{
		char		file_path[MAXPGPATH];

		if (!strcmp(csnlog_de->d_name, ".")
			|| !strcmp(csnlog_de->d_name, ".."))
			continue;

		snprintf(file_path, MAXPGPATH, "%s/%s", path, csnlog_de->d_name);
		durable_unlink(file_path, LOG);
	}

	FreeDir(csnlog_dir);
}


/*
 * This func must be called ONCE on system install.  It creates
 * the initial CSNLOG segment.  (The pg_csnlog directory is assumed to
 * have been created by initdb, and polar_csnlog_shmem_init must have been
 * called already.)
 */
void
polar_csnlog_bootstrap(void)
{
	SlruCtl		csnlog_ctl = polar_csnlog_get_ctl();
	int			slotno;

	LWLockAcquire(CSNLogControlLock, LW_EXCLUSIVE);

	/* Create and zero the first page of the csnlog */
	slotno = polar_csnlog_zero_page(0);

	/* Make sure it's written out */
	SimpleLruWritePage(csnlog_ctl, slotno);
	Assert(!csnlog_ctl->shared->page_dirty[slotno]);

	LWLockRelease(CSNLogControlLock);
}


/*
 * Redo a ZEROPAGE xlog record
 */
void
polar_csnlog_zero_page_redo(int pageno)
{
	int			slotno;
	int			i;
	SlruCtl		csnlog_ctl = polar_csnlog_get_ctl();

	Assert(pageno % BATCH_SIZE == 0);

	LWLockAcquire(CSNLogControlLock, LW_EXCLUSIVE);

	/* Zero the page */
	for (i = pageno; i < pageno + BATCH_SIZE; i++)
	{
		slotno = polar_csnlog_zero_page(i);
		SimpleLruWritePage(csnlog_ctl, slotno);
		Assert(!csnlog_ctl->shared->page_dirty[slotno]);
	}

	LWLockRelease(CSNLogControlLock);
}

/*
 * Write a ZEROPAGE xlog record
 */
static void
polar_csnlog_write_zero_page_xlog_rec(int pageno)
{
	XLogBeginInsert();
	XLogRegisterData((char *) (&pageno), sizeof(int));
	(void) XLogInsert(RM_XLOG_ID, XLOG_CSNLOG_ZEROPAGE);
}

/*
 * Initialize (or reinitialize) a page of CSNLog to zeroes.
 *
 * The page is not actually written, just set up in shared memory.
 * The slot number of the new page is returned.
 *
 * Control lock must be held at entry, and will be held at exit.
 */
static int
polar_csnlog_zero_page(int pageno)
{
	return SimpleLruZeroPage(polar_csnlog_get_ctl(), pageno);
}

/*
 * This must be called ONCE during postmaster or standalone-backend startup,
 * after StartupXLOG has initialized ShmemVariableCache->nextXid.
 *
 * oldestActiveXID is the oldest XID of any prepared transaction, or nextXid
 * if there are none.
 */
void
polar_csnlog_startup(TransactionId oldestActiveXID)
{
	XidStatus	xid_status;
	XLogRecPtr 	xid_lsn;
	TransactionId xid = oldestActiveXID;
	TransactionId end_xid = XidFromFullTransactionId(ShmemVariableCache->nextXid);
	int boundary_pageno = TransactionIdToPageNo(xid) - TransactionIdToPageNo(xid) % BATCH_SIZE;
	TransactionId extend_xid = boundary_pageno * CSNLOG_XACTS_PER_PAGE;

	/* We should give first xid in csnlog boundary page to make csnlog extend work */
	polar_csnlog_extend(extend_xid, false);

	SimpleLruWriteAll(polar_csnlog_get_ctl(), false);

	if (xid == end_xid)
		return;

	/*
	 * Since we don't expect next_csn to be valid across crashes, new
	 * committed xact's csn will start from POLAR_CSN_FIRST_NORMAL. so we
	 * set the committed xact on the currently-active page(s) to POLAR_CSN_FROZEN
	 * during startup. Whenever we advance into a new page,
	 * polar_csnlog_extend will likewise zero the new page without regard to
	 * whatever was previously on disk.
	 */
	while (xid != end_xid)
	{
		xid_status = TransactionIdGetStatus(xid, &xid_lsn);

		/* Make sure the page exist */
		polar_csnlog_extend(xid, false);

		if (xid_status == TRANSACTION_STATUS_COMMITTED)
		{
			/* Set POLAR_CSN_FROZEN means committed and can be seen by anyone */
			polar_csnlog_set_csn(xid, 0, NULL, POLAR_CSN_FROZEN, InvalidXLogRecPtr);
		}

		TransactionIdAdvance(xid);
	}

	SimpleLruWriteAll(polar_csnlog_get_ctl(), false);
}

/*
 * This must be called ONCE during postmaster or standalone-backend shutdown
 */
void
polar_csnlog_shutdown(void)
{
	/*
	 * Flush dirty CLOG pages to disk
	 *
	 * This is not actually necessary from a correctness point of view. We do
	 * it merely as a debugging aid.
	 *
	 * TRACE borrowed from subtrans
	 */
	TRACE_POSTGRESQL_CSNLOG_CHECKPOINT_START(false);
	SimpleLruWriteAll(polar_csnlog_get_ctl(), false);
	TRACE_POSTGRESQL_CSNLOG_CHECKPOINT_DONE(false);
}

/*
 * Perform a checkpoint --- either during shutdown, or on-the-fly
 */
void
polar_csnlog_checkpoint(void)
{
	/*
	 * Flush dirty CSNLog pages to disk
	 *
	 * This is not actually necessary from a correctness point of view. We do
	 * it merely to improve the odds that writing of dirty pages is done by
	 * the checkpoint process and not by backends.
	 *
	 * TRACE borrowed from subtrans
	 */
	TRACE_POSTGRESQL_CSNLOG_CHECKPOINT_START(true);
	SimpleLruWriteAll(polar_csnlog_get_ctl(), true);
	TRACE_POSTGRESQL_CSNLOG_CHECKPOINT_DONE(true);
}


/*
 * Make sure that CSNLOG has room for a newly-allocated XID.
 *
 * NB: this is called while holding XidGenLock.  We want it to be very fast
 * most of the time; even when it's not so fast, no actual I/O need happen
 * unless we're forced to write out a dirty csnlog page to make room
 * in shared memory.
 */
void
polar_csnlog_extend(TransactionId newestXact, bool write_wal)
{
	int			i;
	int			pageno;

	/*
	 * No work except at first XID of a page.  But beware: just after
	 * wraparound, the first XID of page zero is FirstNormalTransactionId.
	 */
	if (TransactionIdToItemNo(newestXact) != 0 &&
		!TransactionIdEquals(newestXact, FirstNormalTransactionId))
		return;

	pageno = TransactionIdToPageNo(newestXact);

	if (pageno % BATCH_SIZE)
		return;

	if (write_wal)
		polar_csnlog_write_zero_page_xlog_rec(pageno);

	LWLockAcquire(CSNLogControlLock, LW_EXCLUSIVE);

	/* Zero the page */
	for (i = pageno; i < pageno + BATCH_SIZE; i++)
		polar_csnlog_zero_page(i);


	LWLockRelease(CSNLogControlLock);
}

void polar_csnlog_truncate_redo(int pageno)
{
	SlruCtl	csnlog_ctl = polar_csnlog_get_ctl();

	/*
	 * During XLOG replay, latest_page_number isn't set up yet; insert a
	 * suitable value to bypass the sanity test in SimpleLruTruncate.
	 */
	csnlog_ctl->shared->latest_page_number = pageno;

	SimpleLruTruncate(csnlog_ctl, pageno);
}

/*
 * Write a CSNLOG TRUNCATE xlog record
 *
 * We must flush the xlog record to disk before returning --- see notes
 * in TruncateCLOG().
 */
static void
polar_csnlog_write_truncate_xlog_rec(int pageno)
{
	XLogRecPtr	recptr;

	XLogBeginInsert();
	XLogRegisterData((char *) (&pageno), sizeof(int));
	recptr = XLogInsert(RM_XLOG_ID, XLOG_CSNLOG_TRUNCATE);
	XLogFlush(recptr);
}

/*
 * Remove all CSNLOG segments before the one holding the passed transaction ID
 *
 * Before removing any CLOG data, we must flush XLOG to disk, to ensure
 * that any recently-emitted HEAP_FREEZE records have reached disk; otherwise
 * a crash and restart might leave us with some unfrozen tuples referencing
 * removed CLOG data.  We choose to emit a special TRUNCATE XLOG record too.
 * Replaying the deletion from XLOG is not critical, since the files could
 * just as well be removed later, but doing so prevents a long-running hot
 * standby server from acquiring an unreasonably bloated CLOG directory.
 *
 * This is normally called during checkpoint, with oldestXact being the
 * oldest TransactionXmin of any running transaction.
 */
void
polar_csnlog_truncate(TransactionId oldest_xact)
{
	SlruCtl		csnlog_ctl = polar_csnlog_get_ctl();
	int			cutoff_pageno;

	/*
	 * The cutoff point is the start of the segment containing oldestXact. We
	 * pass the *page* containing oldestXact to SimpleLruTruncate.
	 */
	cutoff_pageno = TransactionIdToPageNo(oldest_xact);

	/* Check to see if there's any files that could be removed */
	if (!SlruScanDirectory(csnlog_ctl, SlruScanDirCbReportPresence, &cutoff_pageno))
		return;					/* nothing to remove */

	/*
	 * Write XLOG record and flush XLOG to disk.
	 */
	polar_csnlog_write_truncate_xlog_rec(cutoff_pageno);

	SimpleLruTruncate(csnlog_ctl, cutoff_pageno);

	elog(LOG, "CSNLog truncate xid:%d", oldest_xact);
}


/*
 * Decide which of two CSN log page numbers is "older" for truncation purposes.
 *
 * We need to use comparison of TransactionIds here in order to do the right
 * thing with wraparound XID arithmetic.  However, if we are asked about
 * page number zero, we don't want to hand InvalidTransactionId to
 * TransactionIdPrecedes: it'll get weird about permanent xact IDs.  So,
 * offset both xids by FirstNormalTransactionId to avoid that.
 */
static bool
polar_csnlog_page_precedes(int page1, int page2)
{
	TransactionId xid1;
	TransactionId xid2;

	xid1 = ((TransactionId) page1) * CSNLOG_XACTS_PER_PAGE;
	xid1 += FirstNormalTransactionId;
	xid2 = ((TransactionId) page2) * CSNLOG_XACTS_PER_PAGE;
	xid2 += FirstNormalTransactionId;

	return TransactionIdPrecedes(xid1, xid2);
}

/*
 * Do online promote for csnlog
 */
void
polar_promote_csnlog(TransactionId oldest_active_xid)
{
	/* POLAR: During ro promoting, start up the csnlog base on oldest active xid to make sure csn data is consistent with clog */
	polar_csnlog_startup(oldest_active_xid);
	polar_slru_promote(polar_csnlog_get_ctl());
}

/* POLAR: remove csnlog local cache file */
void
polar_remove_csnlog_local_cache_file(void)
{
	polar_slru_remove_local_cache_file(polar_csnlog_get_ctl());
}

/* POLAR: copy csnlog files from shared storage to local when replica start */
void
polar_init_local_csnlog(TransactionId xid, bool copy_all)
{
	int			start_pageno = 0,
				res;
	SlruScanCallback copy_condition = NULL;

	Assert(polar_is_replica());

	if (TransactionIdIsNormal(xid) && !copy_all)
	{
		start_pageno = TransactionIdToPageNo(xid);
		start_pageno -= start_pageno % SLRU_PAGES_PER_SEGMENT;
		copy_condition = polar_trans_file_need_copy;
	}

	res = polar_slru_copy_shared_dir(&polar_csnlog_ctl, copy_condition, &start_pageno);

	if (res)
		polar_handle_init_local_dir_err(polar_csnlog_ctl.Dir, res);
}
