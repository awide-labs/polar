/*-------------------------------------------------------------------------
 *
 * dest.c
 *	  support for communication destinations
 *
 *
 * Portions Copyright (c) 2024, Alibaba Group Holding Limited
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/tcop/dest.c
 *
 *-------------------------------------------------------------------------
 */
/*
 *	 INTERFACE ROUTINES
 *		BeginCommand - initialize the destination at start of command
 *		CreateDestReceiver - create tuple receiver object for destination
 *		EndCommand - clean up the destination at end of command
 *		NullCommand - tell dest that an empty query string was recognized
 *		ReadyForQuery - tell dest that we are ready for a new query
 *
 *	 NOTES
 *		These routines do the appropriate work before and after
 *		tuples are returned by a query to keep the backend and the
 *		"destination" portals synchronized.
 */

#include "postgres.h"

#include "access/printsimple.h"
#include "access/printtup.h"
#include "access/xact.h"
#include "commands/copy.h"
#include "commands/createas.h"
#include "commands/explain.h"
#include "commands/matview.h"
#include "executor/functions.h"
#include "executor/tqueue.h"
#include "executor/tstoreReceiver.h"
#include "libpq/libpq.h"
#include "libpq/pqformat.h"

/* POLAR */
#include "access/xlog.h"
#include "access/xlogrecovery.h"
#include "miscadmin.h"
#include "utils/backend_status.h"
#include "utils/guc.h"
/* POLAR end */

static void polar_send_proxy_info(StringInfo buf);

/* ----------------
 *		dummy DestReceiver functions
 * ----------------
 */
static bool
donothingReceive(TupleTableSlot *slot, DestReceiver *self)
{
	return true;
}

static void
donothingStartup(DestReceiver *self, int operation, TupleDesc typeinfo)
{
}

static void
donothingCleanup(DestReceiver *self)
{
	/* this is used for both shutdown and destroy methods */
}

/* ----------------
 *		static DestReceiver structs for dest types needing no local state
 * ----------------
 */
static const DestReceiver donothingDR = {
	donothingReceive, donothingStartup, donothingCleanup, donothingCleanup,
	DestNone
};

static const DestReceiver debugtupDR = {
	debugtup, debugStartup, donothingCleanup, donothingCleanup,
	DestDebug
};

static const DestReceiver printsimpleDR = {
	printsimple, printsimple_startup, donothingCleanup, donothingCleanup,
	DestRemoteSimple
};

static const DestReceiver spi_printtupDR = {
	spi_printtup, spi_dest_startup, donothingCleanup, donothingCleanup,
	DestSPI
};

/*
 * Globally available receiver for DestNone.
 *
 * It's ok to cast the constness away as any modification of the none receiver
 * would be a bug (which gets easier to catch this way).
 */
DestReceiver *None_Receiver = (DestReceiver *) &donothingDR;

/* ----------------
 *		BeginCommand - initialize the destination at start of command
 * ----------------
 */
void
BeginCommand(CommandTag commandTag, CommandDest dest)
{
	/* Nothing to do at present */
}

/* ----------------
 *		CreateDestReceiver - return appropriate receiver function set for dest
 * ----------------
 */
DestReceiver *
CreateDestReceiver(CommandDest dest)
{
	/*
	 * It's ok to cast the constness away as any modification of the none
	 * receiver would be a bug (which gets easier to catch this way).
	 */

	switch (dest)
	{
		case DestRemote:
		case DestRemoteExecute:
			return printtup_create_DR(dest);

		case DestRemoteSimple:
			return unconstify(DestReceiver *, &printsimpleDR);

		case DestNone:
			return unconstify(DestReceiver *, &donothingDR);

		case DestDebug:
			return unconstify(DestReceiver *, &debugtupDR);

		case DestSPI:
			return unconstify(DestReceiver *, &spi_printtupDR);

		case DestTuplestore:
			return CreateTuplestoreDestReceiver();

		case DestIntoRel:
			return CreateIntoRelDestReceiver(NULL);

		case DestCopyOut:
			return CreateCopyDestReceiver();

		case DestSQLFunction:
			return CreateSQLFunctionDestReceiver();

		case DestTransientRel:
			return CreateTransientRelDestReceiver(InvalidOid);

		case DestTupleQueue:
			return CreateTupleQueueDestReceiver(NULL);

		case DestExplainSerialize:
			return CreateExplainSerializeDestReceiver(NULL);
	}

	/* should never get here */
	pg_unreachable();
}

/* ----------------
 *		EndCommand - clean up the destination at end of command
 * ----------------
 */
void
EndCommand(const QueryCompletion *qc, CommandDest dest, bool force_undecorated_output)
{
	char		completionTag[COMPLETION_TAG_BUFSIZE];
	Size		len;

	switch (dest)
	{
		case DestRemote:
		case DestRemoteExecute:
		case DestRemoteSimple:

			len = BuildQueryCompletionString(completionTag, qc,
											 force_undecorated_output);
			pq_putmessage(PqMsg_CommandComplete, completionTag, len + 1);

		case DestNone:
		case DestDebug:
		case DestSPI:
		case DestTuplestore:
		case DestIntoRel:
		case DestCopyOut:
		case DestSQLFunction:
		case DestTransientRel:
		case DestTupleQueue:
		case DestExplainSerialize:
			break;
	}
}

/* ----------------
 *		EndReplicationCommand - stripped down version of EndCommand
 *
 *		For use by replication commands.
 * ----------------
 */
void
EndReplicationCommand(const char *commandTag)
{
	pq_putmessage(PqMsg_CommandComplete, commandTag, strlen(commandTag) + 1);
}

/* ----------------
 *		NullCommand - tell dest that an empty query string was recognized
 *
 *		This ensures that there will be a recognizable end to the response
 *		to an Execute message in the extended query protocol.
 * ----------------
 */
void
NullCommand(CommandDest dest)
{
	switch (dest)
	{
		case DestRemote:
		case DestRemoteExecute:
		case DestRemoteSimple:

			/* Tell the FE that we saw an empty query string */
			pq_putemptymessage(PqMsg_EmptyQueryResponse);
			break;

		case DestNone:
		case DestDebug:
		case DestSPI:
		case DestTuplestore:
		case DestIntoRel:
		case DestCopyOut:
		case DestSQLFunction:
		case DestTransientRel:
		case DestTupleQueue:
		case DestExplainSerialize:
			break;
	}
}

/* ----------------
 *		ReadyForQuery - tell dest that we are ready for a new query
 *
 *		The ReadyForQuery message is sent so that the FE can tell when
 *		we are done processing a query string.
 *		In versions 3.0 and up, it also carries a transaction state indicator.
 *
 *		Note that by flushing the stdio buffer here, we can avoid doing it
 *		most other places and thus reduce the number of separate packets sent.
 * ----------------
 */
void
ReadyForQuery(CommandDest dest)
{
	switch (dest)
	{
		case DestRemote:
		case DestRemoteExecute:
		case DestRemoteSimple:
			{
				StringInfoData buf;

				pq_beginmessage(&buf, PqMsg_ReadyForQuery);
				pq_sendbyte(&buf, TransactionBlockStatusCode());
				polar_send_proxy_info(&buf);
				pq_endmessage(&buf);
			}
			/* Flush output at end of cycle in any case. */
			pq_flush();
			break;

		case DestNone:
		case DestDebug:
		case DestSPI:
		case DestTuplestore:
		case DestIntoRel:
		case DestCopyOut:
		case DestSQLFunction:
		case DestTransientRel:
		case DestTupleQueue:
		case DestExplainSerialize:
			break;
	}
}

/*----------
 * Transient per-statement split state, re-evaluated on every ReadyForQuery.
 *
 *   POLAR_RFQ_SPLITTABLE       ('x')  xids follow; the proxy may route the
 *                                     next read to a replica using the LSN
 *                                     we just shipped on this 'Z' message.
 *
 *   POLAR_RFQ_UNSPLITTABLE_WAL ('w')  xids follow; the proxy should keep
 *                                     the next statement on the primary
 *                                     for now and cache the xids. The
 *                                     transaction's own WAL is not yet
 *                                     flushed to shared storage, so the
 *                                     walsender has not shipped it and
 *                                     the replica cannot replay it. The
 *                                     state usually clears by the next
 *                                     ReadyForQuery as walwriter or a
 *                                     peer's group-commit catches up.
 *
 *   POLAR_RFQ_UNSPLITTABLE_HARD (no marker)
 *                                     sticky for the whole transaction —
 *                                     error, lock, combocid, createenum,
 *                                     autoxact. Pin everything to primary.
 *
 * The 'w' tier is the back-pressure signal that prevents handing the proxy
 * an LSN target the replica cannot possibly reach yet. Without it the
 * primary would still send 'x' when this session's own LSN (see
 * polar_rfq_session_lsn) is ahead of GetFlushRecPtr(), the proxy would set
 * polar_xact_split_wait_lsn on the replica, and the snapshot wait would
 * either time out or return stale data while walsender is still waiting
 * for the flush.
 *----------
 */
typedef enum
{
	POLAR_RFQ_SPLITTABLE,		/* fully splittable */
	POLAR_RFQ_UNSPLITTABLE_WAL, /* soft: WAL not flushed yet */
	POLAR_RFQ_UNSPLITTABLE_HARD /* sticky: error/lock/combocid/etc */
} PolarRfqSplitState;

/*
 * The LSN this session most recently produced: XactLastRecEnd while a
 * write xact is still in flight, else XactLastCommitEnd left behind by
 * the xact that just committed. Invalid if this session hasn't written
 * anything yet.
 */
static inline XLogRecPtr
polar_rfq_session_lsn(void)
{
	XLogRecPtr	lsn = XactLastRecEnd;

	if (XLogRecPtrIsInvalid(lsn))
		lsn = XactLastCommitEnd;

	return lsn;
}

/*
 * Caller has already computed xids via polar_xact_split_xact_info().
 * A NULL means a sticky hard blocker fired earlier in the transaction
 * (no xids harvested).
 */
static inline PolarRfqSplitState
polar_rfq_split_state(const char *xids)
{
	XLogRecPtr	session_lsn;

	if (xids == NULL)
		return POLAR_RFQ_UNSPLITTABLE_HARD;

	session_lsn = polar_rfq_session_lsn();

	if (XLogRecPtrIsInvalid(session_lsn))
		return POLAR_RFQ_SPLITTABLE;

	if (GetFlushRecPtr(NULL) < session_lsn)
		return POLAR_RFQ_UNSPLITTABLE_WAL;

	return POLAR_RFQ_SPLITTABLE;
}

/* POLAR: send proxy info, including lsn and xact split info, also collects stats */
static void
polar_send_proxy_info(StringInfo buf)
{
	/*
	 * POLAR: send a per-session LSN to the proxy, not the global WAL tip.
	 *
	 * On primary, XactLastRecEnd is non-zero during an in-flight write xact
	 * and is the proxy's correct wait target; after COMMIT it falls back to
	 * XactLastCommitEnd (set in CommitTransaction()). For sessions that did
	 * write, this avoids forcing the replica to replay unrelated WAL from
	 * other backends, which is what GetXLogInsertRecPtr() would have made it
	 * do.
	 *
	 * For a session that has done no writes yet, neither is set; fall back to
	 * the global WAL tip so the proxy still receives a well-formed
	 * cluster-state value rather than 0/0.
	 */
	if (MyProcPort->polar_proxy_send_lsn)
	{
		if (RecoveryInProgress())
			pq_sendint64(buf, (uint64) GetXLogReplayRecPtr(NULL));
		else
		{
			XLogRecPtr	session_lsn = polar_rfq_session_lsn();

			if (XLogRecPtrIsInvalid(session_lsn))
				session_lsn = GetXLogInsertRecPtr();
			pq_sendint64(buf, (uint64) session_lsn);
		}
	}

	if (unlikely(polar_enable_xact_split_debug) && !RecoveryInProgress())
	{
		char	   *xids = polar_xact_split_xact_info();

		elog(LOG, "current xids : %s", xids);

		if (xids)
			pfree(xids);
	}

	polar_stat_update_proxy_info(polar_stat_proxy->proxy_total);

	/* POLAR: send xact split info to proxy if needed */
	if (polar_enable_xact_split &&
		XactIsoLevel == XACT_READ_COMMITTED &&
		MyProcPort->polar_proxy_send_xact &&
		!RecoveryInProgress())
	{
		char	   *xids = polar_xact_split_xact_info();
		PolarRfqSplitState split_state = polar_rfq_split_state(xids);

		if (split_state != POLAR_RFQ_UNSPLITTABLE_HARD)
		{
			/* See PolarRfqSplitState above for what 'x' vs 'w' means. */
			char		marker = (split_state == POLAR_RFQ_SPLITTABLE) ? 'x' : 'w';

			pq_sendbyte(buf, marker);
			pq_sendstring(buf, xids);
			polar_stat_update_proxy_info(polar_stat_proxy->proxy_splittable);
		}
		else
			polar_stat_update_proxy_info(polar_stat_proxy->proxy_unsplittable);

		if (xids)
			pfree(xids);

		switch (polar_unsplittable_reason)
		{
			case POLAR_UNSPLITTABLE_FOR_ERROR:
				polar_stat_update_proxy_info(polar_stat_proxy->proxy_error);
				break;
			case POLAR_UNSPLITTABLE_FOR_LOCK:
				polar_stat_update_proxy_info(polar_stat_proxy->proxy_lock);
				break;
			case POLAR_UNSPLITTABLE_FOR_COMBOCID:
				polar_stat_update_proxy_info(polar_stat_proxy->proxy_combocid);
				break;
			case POLAR_UNSPLITTABLE_FOR_CREATEENUM:
				polar_stat_update_proxy_info(polar_stat_proxy->proxy_createenum);
				break;
			case POLAR_UNSPLITTABLE_FOR_AUTOXACT:
				polar_stat_update_proxy_info(polar_stat_proxy->proxy_autoxact);
				break;
			default:
				break;
		}
	}
	else
		polar_stat_update_proxy_info(polar_stat_proxy->proxy_disablesplit);

	if (RecoveryInProgress() && MyProcPort->polar_proxy)
		polar_stat_update_xact_split_info();
	polar_stat_need_update_proxy_info = false;
}
