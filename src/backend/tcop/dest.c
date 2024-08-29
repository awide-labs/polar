/*-------------------------------------------------------------------------
 *
 * dest.c
 *	  support for communication destinations
 *
 *
 * Portions Copyright (c) 2024, Alibaba Group Holding Limited
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
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
#include "commands/matview.h"
#include "executor/functions.h"
#include "executor/tqueue.h"
#include "executor/tstoreReceiver.h"
#include "libpq/libpq.h"
#include "libpq/pqformat.h"
#include "utils/portal.h"

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
	CommandTag	tag;
	const char *tagname;

	switch (dest)
	{
		case DestRemote:
		case DestRemoteExecute:
		case DestRemoteSimple:

			/*
			 * We assume the tagname is plain ASCII and therefore requires no
			 * encoding conversion.
			 *
			 * We no longer display LastOid, but to preserve the wire
			 * protocol, we write InvalidOid where the LastOid used to be
			 * written.
			 *
			 * All cases where LastOid was written also write nprocessed
			 * count, so just Assert that rather than having an extra test.
			 */
			tag = qc->commandTag;
			tagname = GetCommandTagName(tag);

			if (command_tag_display_rowcount(tag) && !force_undecorated_output)
				snprintf(completionTag, COMPLETION_TAG_BUFSIZE,
						 tag == CMDTAG_INSERT ?
						 "%s 0 " UINT64_FORMAT : "%s " UINT64_FORMAT,
						 tagname, qc->nprocessed);
			else
				snprintf(completionTag, COMPLETION_TAG_BUFSIZE, "%s", tagname);
			pq_putmessage('C', completionTag, strlen(completionTag) + 1);

		case DestNone:
		case DestDebug:
		case DestSPI:
		case DestTuplestore:
		case DestIntoRel:
		case DestCopyOut:
		case DestSQLFunction:
		case DestTransientRel:
		case DestTupleQueue:
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
	pq_putmessage('C', commandTag, strlen(commandTag) + 1);
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
			pq_putemptymessage('I');
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

				pq_beginmessage(&buf, 'Z');
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
			break;
	}
}

/* POLAR: send proxy info, including lsn and xact split info, also collects stats */
static void
polar_send_proxy_info(StringInfo buf)
{
	/* POLAR: send lsn to maxscale if needed */
	if (MyProcPort->polar_proxy_send_lsn)
	{
		if (RecoveryInProgress())
			pq_sendint64(buf, (uint64) GetXLogReplayRecPtr(NULL));
		else
			pq_sendint64(buf, (uint64) GetXLogInsertRecPtr());
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

		if (xids)
		{
			if (strlen(xids) != 0)
			{
				/* POLAR: send xids info */
				pq_sendbyte(buf, 'x');
				pq_sendstring(buf, xids);
			}
			pfree(xids);
			polar_stat_update_proxy_info(polar_stat_proxy->proxy_splittable);
		}
		else
			polar_stat_update_proxy_info(polar_stat_proxy->proxy_unsplittable);

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
				polar_stat_update_proxy_info(polar_stat_proxy->proxy_combocid);
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
