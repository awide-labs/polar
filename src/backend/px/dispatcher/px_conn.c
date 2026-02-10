/*-------------------------------------------------------------------------
 *
 * px_conn.c
 *
 * PxWorkerDescriptor methods
 *
 * Portions Copyright (c) 2005-2008, Greenplum inc
 * Portions Copyright (c) 2012-Present Pivotal Software, Inc.
 * Portions Copyright (c) 2021, Alibaba Group Holding Limited
 *
 * IDENTIFICATION
 *	    src/backend/px/dispatcher/px_conn.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "commands/dbcommands.h"
#include "libpq-fe.h"
#include "libpq-int.h"
#include "libpq/libpq.h"
#include "libpq/libpq-be.h"
#include "libpq/pqformat.h"
#include "miscadmin.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"

#include "px/px_conn.h"			/* me */
#include "px/px_gang.h"
#include "px/px_util.h"			/* PxNodeInfo */
#include "px/px_vars.h"

/* POLAR */
#include "utils/builtins.h"
#include "utils/faultinjector.h"


static uint32 pxconn_get_motion_listener_port(PGconn *conn);
static char *pxconn_get_motion_snapshot(PGconn *conn);
static void pxconn_disconnect(PxWorkerDescriptor *pxWorkerDesc);

static void MPPnoticeReceiver(void *arg, const PGresult *res);

static const char *
transStatusToString(PGTransactionStatusType status)
{
	const char *ret = "";

	switch (status)
	{
		case PQTRANS_IDLE:
			ret = "idle";
			break;
		case PQTRANS_ACTIVE:
			ret = "active";
			break;
		case PQTRANS_INTRANS:
			ret = "idle, within transaction";
			break;
		case PQTRANS_INERROR:
			ret = "idle, within failed transaction";
			break;
		case PQTRANS_UNKNOWN:
			ret = "unknown transaction status";
			break;
		default:
			Assert(false);
	}
	return ret;
}

#ifdef FAULT_INJECTOR
const char *(*test_trans_status_to_string) (PGTransactionStatusType status) = transStatusToString;
#endif

/* Initialize a PX connection descriptor */
PxWorkerDescriptor *
pxconn_createWorkerDescriptor(struct PxNodeInfo *pxinfo, int identifier,
							  int logicalWorkerIdx, int logicalTotalWorkers)
{
	MemoryContext oldContext;
	PxWorkerDescriptor *pxWorkerDesc = NULL;

	oldContext = SwitchToPXWorkerContext();

	pxWorkerDesc = (PxWorkerDescriptor *) palloc0(sizeof(PxWorkerDescriptor));

	/* Segment db info */
	pxWorkerDesc->pxNodeInfo = pxinfo;

	/* Connection info, set in function pxconn_doConnect */
	pxWorkerDesc->conn = NULL;
	pxWorkerDesc->motionListener = 0;
	pxWorkerDesc->backendPid = 0;

	/* whoami */
	pxWorkerDesc->whoami = NULL;
	pxWorkerDesc->identifier = identifier;
	pxWorkerDesc->logicalWorkerInfo.total_count = logicalTotalWorkers;
	/* POLAR px */
	pxWorkerDesc->logicalWorkerInfo.idx = logicalWorkerIdx;
	/* POLAR end */

	MemoryContextSwitchTo(oldContext);
	return pxWorkerDesc;
}

/* Free memory of segment descriptor. */
void
pxconn_termWorkerDescriptor(PxWorkerDescriptor *pxWorkerDesc)
{
	PxNodes    *px_nodes;

	px_nodes = pxWorkerDesc->pxNodeInfo->px_nodes;

	/* put px identifier to free list for reuse */
	px_nodes->freeCounterList = lappend_int(px_nodes->freeCounterList, pxWorkerDesc->identifier);

	pxconn_disconnect(pxWorkerDesc);

	if (pxWorkerDesc->whoami != NULL)
	{
		pfree(pxWorkerDesc->whoami);
		pxWorkerDesc->whoami = NULL;
	}
}								/* pxconn_termWorkerDescriptor */

/*
 * Establish socket connection via libpq.
 * Caller should call PQconnectPoll to finish it up.
 */
void
pxconn_doConnectStart(PxWorkerDescriptor *pxWorkerDesc,
					  const char *pxid,
					  const char *options,
					  SegmentType segmentType)
{
#define MAX_KEYWORDS 11
#define MAX_INT_STRING_LEN 20
	PxNodeInfo *pxinfo = pxWorkerDesc->pxNodeInfo;
	const char *keywords[MAX_KEYWORDS];
	const char *values[MAX_KEYWORDS];
	char		portstr[MAX_INT_STRING_LEN];
	int			nkeywords = 0;
	bool		should_be_localhost = false;

	keywords[nkeywords] = "pxid";
	values[nkeywords] = pxid;
	nkeywords++;

	/*
	 * Build the connection string
	 */
	if (options)
	{
		keywords[nkeywords] = "options";
		values[nkeywords] = options;
		nkeywords++;
	}

	/*
	 * For entry DB connection, we make sure both "hostaddr" and "host" are
	 * empty string. Or else, it will fall back to environment variables and
	 * won't use domain socket in function connectDBStart.
	 *
	 * For other PX connections, we set "hostaddr". "host" is not used.
	 */
	/* When EntryDB or pdml, hostaddr should be localhost(127.0.0.1) */
	should_be_localhost = (pxWorkerDesc->logicalWorkerInfo.idx == MASTER_CONTENT_ID)
		|| (segmentType == SEGMENTTYPE_EXPLICT_WRITER);
	if (px_role == PX_ROLE_QC && should_be_localhost)
	{
		keywords[nkeywords] = "hostaddr";
		values[nkeywords] = "127.0.0.1";
		nkeywords++;
	}
	else
	{
		Assert(pxinfo->config->hostip != NULL);
		keywords[nkeywords] = "hostaddr";
		values[nkeywords] = pxinfo->config->hostip;
		nkeywords++;
	}

	keywords[nkeywords] = "host";
	values[nkeywords] = "";
	nkeywords++;

	snprintf(portstr, sizeof(portstr), "%u", pxinfo->config->port);
	keywords[nkeywords] = "port";
	values[nkeywords] = portstr;
	nkeywords++;

	keywords[nkeywords] = "dbname";
#ifdef FAULT_INJECTOR
	if (SIMPLE_FAULT_INJECTOR("get_database_name") == FaultInjectorTypeEnable)
		goto FAULT_INJECTOR_GET_DB_NAME_LABEL;
#endif
	if (MyProcPort && MyProcPort->database_name)
	{
		values[nkeywords] = MyProcPort->database_name;
	}
	else
	{
#ifdef FAULT_INJECTOR
FAULT_INJECTOR_GET_DB_NAME_LABEL:
#endif

		/*
		 * get database name from MyDatabaseId, which is initialized in
		 * InitPostgres()
		 */
		Assert(MyDatabaseId != InvalidOid);
		values[nkeywords] = get_database_name(MyDatabaseId);
	}
	nkeywords++;

	/*
	 * Set the client encoding to match database encoding in QC->PX
	 * connections.  All the strings dispatched from QC to be in the database
	 * encoding, and all strings sent back to the QC will also be in the
	 * database encoding.
	 *
	 * Most things don't pay attention to client_encoding in PX processes:
	 * query results are normally sent back via the interconnect, and the 'M'
	 * type QC->PX messages, used to dispatch queries, don't perform encoding
	 * conversion.  But some things, like error messages, and internal
	 * commands dispatched directly with PxDispatchCommand, do care.
	 */
	keywords[nkeywords] = "client_encoding";
	values[nkeywords] = GetDatabaseEncodingName();
	nkeywords++;

	keywords[nkeywords] = "user";
#ifdef FAULT_INJECTOR
	if (SIMPLE_FAULT_INJECTOR("get_user_name") == FaultInjectorTypeEnable)
		goto FAULT_INJECTOR_GET_USR_NAME_LABEL;
#endif
	if (MyProcPort && MyProcPort->user_name)
	{
		values[nkeywords] = MyProcPort->user_name;
	}
	else
	{
#ifdef FAULT_INJECTOR
FAULT_INJECTOR_GET_USR_NAME_LABEL:
#endif

		/*
		 * get user name from AuthenticatedUserId which is initialized in
		 * InitPostgres()
		 */
		values[nkeywords] = GetUserNameFromId(GetAuthenticatedUserId(), false);
	}
	nkeywords++;

#ifdef USE_SSL
	if (px_tls_auth_enabled_on_qc())
	{
		if (!px_tls_auth_ready_on_qc())
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("PX TLS authentication error"),
					 errdetail_internal("PX TLS credentials on QC could not be loaded")));

		keywords[nkeywords] = "px_auth_cert";
		values[nkeywords] = px_get_serial_tls_cert();
		nkeywords++;
	}
#endif

	keywords[nkeywords] = NULL;
	values[nkeywords] = NULL;

	Assert(nkeywords < MAX_KEYWORDS);

	pxWorkerDesc->conn = PQconnectStartParams(keywords, values, false);
	return;
}

void
pxconn_doConnectComplete(PxWorkerDescriptor *pxWorkerDesc)
{
	PQsetNoticeReceiver(pxWorkerDesc->conn, &MPPnoticeReceiver, pxWorkerDesc);

	/*
	 * Command the PX to initialize its motion layer. Wait for it to respond
	 * giving us the TCP port number where it listens for connections from the
	 * gang below.
	 */
	pxWorkerDesc->motionListener = pxconn_get_motion_listener_port(pxWorkerDesc->conn);
	pxWorkerDesc->backendPid = PQbackendPID(pxWorkerDesc->conn);
	pxWorkerDesc->serialized_snap = pxconn_get_motion_snapshot(pxWorkerDesc->conn);

	if (pxWorkerDesc->motionListener != 0 &&
		px_log_gang >= PXVARS_VERBOSITY_DEBUG)
	{
		elog(LOG, "Connected to %s motionListenerPorts=%u/%u with options %s",
			 pxWorkerDesc->whoami,
			 (pxWorkerDesc->motionListener & 0x0ffff),
			 ((pxWorkerDesc->motionListener >> 16) & 0x0ffff),
			 PQoptions(pxWorkerDesc->conn));
	}
}

/* Disconnect from PX */
static void
pxconn_disconnect(PxWorkerDescriptor *pxWorkerDesc)
{
	if (PQstatus(pxWorkerDesc->conn) != CONNECTION_BAD)
	{
		PGTransactionStatusType status = PQtransactionStatus(pxWorkerDesc->conn);

		if (px_log_gang >= PXVARS_VERBOSITY_DEBUG)
			elog(LOG, "Finishing connection with %s; %s", pxWorkerDesc->whoami, transStatusToString(status));

		if (status == PQTRANS_ACTIVE
#ifdef FAULT_INJECTOR
			|| SIMPLE_FAULT_INJECTOR("pxconn_disconnect") == FaultInjectorTypeEnable
#endif
			)
		{
			char		errbuf[256];
			bool		sent;

			memset(errbuf, 0, sizeof(errbuf));

			if (px_debug_cancel_print || px_log_gang >= PXVARS_VERBOSITY_DEBUG)
				elog(LOG, "Calling PQcancel for %s", pxWorkerDesc->whoami);

			sent = pxconn_signalPX(pxWorkerDesc, errbuf, true);
			if (!sent)
				elog(LOG, "Unable to cancel: %s", strlen(errbuf) == 0 ? "cannot allocate PGCancel" : errbuf);
		}

		PQfinish(pxWorkerDesc->conn);
		pxWorkerDesc->conn = NULL;
	}
}

/*
 * Read result from connection and discard it.
 *
 * Retry at most N times.
 *
 * Return false if there'er still leftovers.
 */
bool
pxconn_discardResults(PxWorkerDescriptor *pxWorkerDesc,
					  int retryCount)
{
	PGresult   *pRes = NULL;
	PGnotify   *notify = NULL;
	ExecStatusType stat;
	int			i = 0;
	bool		retval = true;

	/* PQstatus() is smart enough to handle NULL */
	while (NULL != (pRes = PQgetResult(pxWorkerDesc->conn)))
	{
		stat = PQresultStatus(pRes);
		PQclear(pRes);

		elog(LOG, "(%s) Leftover result at freeGang time: %s %s", pxWorkerDesc->whoami,
			 PQresStatus(stat),
			 PQerrorMessage(pxWorkerDesc->conn));

		if (stat == PGRES_FATAL_ERROR || stat == PGRES_BAD_RESPONSE)
		{
			retval = true;
			break;
		}

		if (i++ > retryCount)
		{
			retval = false;
			break;
		}
	}

	/*
	 * Clear of all the notify messages as well.
	 */
	notify = pxWorkerDesc->conn->notifyHead;

	while (notify != NULL)
	{
		PGnotify   *prev = notify;

		notify = notify->next;
		PQfreemem(prev);
	}
	pxWorkerDesc->conn->notifyHead = pxWorkerDesc->conn->notifyTail = NULL;

	return retval;
}

/* Return if it's a bad connection */
bool
pxconn_isBadConnection(PxWorkerDescriptor *pxWorkerDesc)
{
	return (pxWorkerDesc->conn == NULL || PQsocket(pxWorkerDesc->conn) < 0 ||
			PQstatus(pxWorkerDesc->conn) == CONNECTION_BAD);
}

/*
 * Build text to identify this PX in error messages.
 * Don't call this function in threads.
 */
void
pxconn_setPXIdentifier(PxWorkerDescriptor *pxWorkerDesc,
					   int sliceIndex)
{
	PxNodeInfo *pxinfo = pxWorkerDesc->pxNodeInfo;
	StringInfoData string;
	MemoryContext oldContext;

	if (!px_info_debug)
		return;

	oldContext = SwitchToPXWorkerContext();

	initStringInfo(&string);

#ifdef FAULT_INJECTOR
	if (SIMPLE_FAULT_INJECTOR("pxconn_entry_db") == FaultInjectorTypeEnable)
		goto FAULT_INJECTOR_ENTRY_DB_LABEL;
#endif

	/* Format the identity of the segment db. */
	if (pxWorkerDesc->logicalWorkerInfo.idx >= 0)
	{
		appendStringInfo(&string, "seg%d", pxWorkerDesc->logicalWorkerInfo.idx);

		/* Format the slice index. */
		if (sliceIndex > 0)
			appendStringInfo(&string, " slice%d", sliceIndex);
	}
	else
	{
#ifdef FAULT_INJECTOR
FAULT_INJECTOR_ENTRY_DB_LABEL:
#endif
		appendStringInfo(&string, "entry db");
	}

	/* Format the identifier. */
	appendStringInfo(&string, " identifer%d", pxWorkerDesc->identifier);

	/* Format the connection info. */
	appendStringInfo(&string, " %s:%d", pxinfo->config->hostip, pxinfo->config->port);

	/* If connected, format the PX's process id. */
	if (pxWorkerDesc->backendPid != 0)
		appendStringInfo(&string, " pid=%d", pxWorkerDesc->backendPid);

	if (pxWorkerDesc->whoami != NULL)
		pfree(pxWorkerDesc->whoami);

	pxWorkerDesc->whoami = string.data;

	MemoryContextSwitchTo(oldContext);
}

/*
 * Send cancel/finish signal to still-running PX through libpq.
 *
 * errbuf is used to return error message(recommended size is 256 bytes).
 *
 * Returns true if we successfully sent a signal
 * (not necessarily received by the target process).
 */
bool
pxconn_signalPX(PxWorkerDescriptor *pxWorkerDesc,
				char *errbuf,
				bool isCancel)
{
	bool		ret;

	PGcancel   *cn = PQgetCancel(pxWorkerDesc->conn);

	if (cn == NULL)
		return false;

	ret = PQcancel(cn, errbuf, 256);

	/* if (isCancel) */
	/* ret = PQcancel(cn, errbuf, 256); */
	/* else */
	/* ret = PQrequestFinish(cn, errbuf, 256); */

	PQfreeCancel(cn);
	return ret;
}


/* GPDB function to retrieve PX-backend details (motion listener) */
static uint32
pxconn_get_motion_listener_port(PGconn *conn)
{
	const char *val;
	char	   *endptr;
	uint32		result;

	val = PQparameterStatus(conn, "px_listener_port");
	if (!val)
		return 0;

	errno = 0;
	result = strtoul(val, &endptr, 10);
	if (endptr == val || *endptr != '\0' || errno == ERANGE)
		return 0;

	return result;
}

/* GPDB function to retrieve PX-backend details (motion listener) */
static char *
pxconn_get_motion_snapshot(PGconn *conn)
{
	const char *encoded_snap;
	char	   *serialized_snap;

	encoded_snap = PQparameterStatus(conn, "snapshot");
	Assert(encoded_snap);
	Assert(strlen(encoded_snap) % 2 == 0);
	if (px_info_debug)
		elog(DEBUG1, "px_snapshot_encode: %d pxconn_get_motion_snapshot: encoded_snap: %s",
			 PxIdentity.dbid, encoded_snap);

	serialized_snap = (char *) MemoryContextAlloc(TopMemoryContext, (strlen(encoded_snap) / 2));
	hex_decode(encoded_snap, strlen(encoded_snap), serialized_snap);

	return serialized_snap + sizeof(int32) * 2;
}


/*-------------------------------------------------------------------------
 * PX Notice receiver support
 *
 * When a PX process emits a NOTICE (or WARNING, INFO, etc.) message, it
 * needs to be delivered to the user. To do that, we install a libpq Notice
 * receiver callback to every QC->PX connection.
 *
 *-------------------------------------------------------------------------
 */

typedef struct PXNotice PXNotice;
struct PXNotice
{
	PXNotice   *next;

	int			elevel;
	char		sqlstate[6];
	char		severity[10];
	char	   *file;
	char		line[10];
	char	   *func;
	char	   *message;
	char	   *detail;
	char	   *hint;
	char	   *context;

	char		buf[];
};

static void forwardPXNotices(PXNotice * notice);

/*
 * libpq Notice receiver callback.
 *
 * NB: This is a callback, so we are very limited in what we can do. In
 * particular, we must not call ereport() or elog(), which might longjmp()
 * out of the callback. Libpq might get confused by that. That also means
 * that we cannot call palloc()!
 *
 * A PXNotice struct is created for each incoming Notice, and put in a
 * queue for later processing. The PXNotices are allocatd with good old
 * malloc()!
 */
static void
MPPnoticeReceiver(void *arg, const PGresult *res)
{
	PGMessageField *pfield;
	int			elevel = INFO;
	char	   *sqlstate = "00000";
	char	   *severity = "WARNING";
	char	   *file = "";
	char	   *line = NULL;
	char	   *func = "";
	char		message[1024];
	char	   *detail = NULL;
	char	   *hint = NULL;
	char	   *context = NULL;

	PxWorkerDescriptor *pxWorkerDesc = (PxWorkerDescriptor *) arg;

	/*
	 * If MyProcPort is NULL, there is no client, so no need to generate
	 * notice. One example is that there is no client for a background worker.
	 */
	if (!res || MyProcPort == NULL)
		return;

	strcpy(message, "missing error text");

	for (pfield = res->errFields; pfield != NULL; pfield = pfield->next)
	{
		switch (pfield->code)
		{
			case PG_DIAG_SEVERITY:
				severity = pfield->contents;
				if (strcmp(pfield->contents, "WARNING") == 0)
					elevel = WARNING;
				else if (strcmp(pfield->contents, "NOTICE") == 0)
					elevel = NOTICE;
				else if (strcmp(pfield->contents, "DEBUG1") == 0 ||
						 strcmp(pfield->contents, "DEBUG") == 0)
					elevel = DEBUG1;
				else if (strcmp(pfield->contents, "DEBUG2") == 0)
					elevel = DEBUG2;
				else if (strcmp(pfield->contents, "DEBUG3") == 0)
					elevel = DEBUG3;
				else if (strcmp(pfield->contents, "DEBUG4") == 0)
					elevel = DEBUG4;
				else if (strcmp(pfield->contents, "DEBUG5") == 0)
					elevel = DEBUG5;
				else
					elevel = INFO;
				break;
			case PG_DIAG_SQLSTATE:
				sqlstate = pfield->contents;
				break;
			case PG_DIAG_MESSAGE_PRIMARY:
				strncpy(message, pfield->contents, 800);
				message[800] = '\0';
				if (pxWorkerDesc && pxWorkerDesc->whoami && strlen(pxWorkerDesc->whoami) < 200)
				{
					strcat(message, "  (");
					strcat(message, pxWorkerDesc->whoami);
					strcat(message, ")");
				}
				break;
			case PG_DIAG_MESSAGE_DETAIL:
				detail = pfield->contents;
				break;
			case PG_DIAG_MESSAGE_HINT:
				hint = pfield->contents;
				break;
			case PG_DIAG_STATEMENT_POSITION:
			case PG_DIAG_INTERNAL_POSITION:
			case PG_DIAG_INTERNAL_QUERY:
				break;
			case PG_DIAG_CONTEXT:
				context = pfield->contents;
				break;
			case PG_DIAG_SOURCE_FILE:
				file = pfield->contents;
				break;
			case PG_DIAG_SOURCE_LINE:
				line = pfield->contents;
				break;
			case PG_DIAG_SOURCE_FUNCTION:
				func = pfield->contents;
				break;
			case PG_DIAG_PX_PROCESS_TAG:
				break;
			default:
				break;

		}
	}

	/*
	 * If this message is filtered out by client_min_messages, we have nothing
	 * to do. (The PX shouldn't have sent it to us in the first place...)
	 */
	if (elevel >= client_min_messages || elevel == INFO)
	{
		PXNotice   *notice;
		uint64		size;
		char	   *bufptr;
		int			file_len;
		int			func_len;
		int			message_len;
		int			detail_len;
		int			hint_len;
		int			context_len;

		/*
		 * We use malloc(), because we are in a libpq callback, and we CANNOT
		 * use palloc(). We allocate space for the PXNotice and the strings in
		 * a single malloc() call.
		 */

		/*
		 * First, compute the required size of the allocation.
		 */

/* helper macro for computing the total allocation size */
#define SIZE_VARLEN_FIELD(fldname) \
		if (fldname != NULL) \
		{ \
			fldname##_len = strlen(fldname) + 1; \
			size += fldname##_len; \
		} \
		else \
			fldname##_len = 0

		size = offsetof(PXNotice, buf);
		SIZE_VARLEN_FIELD(file);
		SIZE_VARLEN_FIELD(func);
		SIZE_VARLEN_FIELD(message);
		SIZE_VARLEN_FIELD(detail);
		SIZE_VARLEN_FIELD(hint);
		SIZE_VARLEN_FIELD(context);

		/*
		 * Perform the allocation.  Put a limit on the max size, as a sanity
		 * check.  (The libpq protocol itself limits the size the message can
		 * be, but better safe than sorry.)
		 *
		 * We can't ereport() if this fails, so we just drop the notice to the
		 * floor. Hope it wasn't important...
		 */
		if (size >= MaxAllocSize)
			return;

		notice = malloc(size);
		if (!notice)
			return;

		/*
		 * Allocation succeeded.  Now fill in the struct.
		 */
		bufptr = notice->buf;

#define COPY_VARLEN_FIELD(fldname) \
		if (fldname != NULL) \
		{ \
			notice->fldname = bufptr; \
			memcpy(bufptr, fldname, fldname##_len); \
			bufptr += fldname##_len; \
		} \
		else \
			notice->fldname = NULL

		notice->elevel = elevel;
		strlcpy(notice->sqlstate, sqlstate, sizeof(notice->sqlstate));
		strlcpy(notice->severity, severity, sizeof(notice->severity));
		COPY_VARLEN_FIELD(file);

		if (line)
			strlcpy(notice->line, line, sizeof(notice->line));
		else
			notice->line[0] = '\0';

		COPY_VARLEN_FIELD(func);
		COPY_VARLEN_FIELD(message);
		COPY_VARLEN_FIELD(detail);
		COPY_VARLEN_FIELD(hint);
		COPY_VARLEN_FIELD(context);

		Assert(bufptr - (char *) notice == size);

		/* Forward the notice message to client */
		forwardPXNotices(notice);
	}
}

/*
 * Send the Notice to the client.
 */
static void
forwardPXNotices(PXNotice * notice)
{
	/* POLAR px: use to inject error */
	bool		polar_px_inject = false;

	StringInfoData msgbuf;

	/*
	 * Use PG_TRY() - PG_CATCH() to make sure we free the struct, no matter
	 * what.
	 */
	PG_TRY();
	{
#ifdef FAULT_INJECTOR
		if (SIMPLE_FAULT_INJECTOR("px_forward_notices_error") == FaultInjectorTypeEnable)
			elog(ERROR, "forwardPXNotices inject error");
		if (SIMPLE_FAULT_INJECTOR("px_forward_notices_old_style") == FaultInjectorTypeEnable)
			polar_px_inject = true;
#endif
		/* 'N' (Notice) is for nonfatal conditions, 'E' is for errors */
		pq_beginmessage(&msgbuf, 'N');

		if (PG_PROTOCOL_MAJOR(FrontendProtocol) >= 3 && !polar_px_inject)
		{
			/* New style with separate fields */
			pq_sendbyte(&msgbuf, PG_DIAG_SEVERITY);
			pq_sendstring(&msgbuf, notice->severity);

			pq_sendbyte(&msgbuf, PG_DIAG_SQLSTATE);
			pq_sendstring(&msgbuf, notice->sqlstate);

			/* M field is required per protocol, so always send something */
			pq_sendbyte(&msgbuf, PG_DIAG_MESSAGE_PRIMARY);
			pq_sendstring(&msgbuf, notice->message);

			if (notice->detail)
			{
				pq_sendbyte(&msgbuf, PG_DIAG_MESSAGE_DETAIL);
				pq_sendstring(&msgbuf, notice->detail);
			}

			if (notice->hint)
			{
				pq_sendbyte(&msgbuf, PG_DIAG_MESSAGE_HINT);
				pq_sendstring(&msgbuf, notice->hint);
			}

			if (notice->context)
			{
				pq_sendbyte(&msgbuf, PG_DIAG_CONTEXT);
				pq_sendstring(&msgbuf, notice->context);
			}

			if (notice->file)
			{
				pq_sendbyte(&msgbuf, PG_DIAG_SOURCE_FILE);
				pq_sendstring(&msgbuf, notice->file);
			}

			if (notice->line[0])
			{
				pq_sendbyte(&msgbuf, PG_DIAG_SOURCE_LINE);
				pq_sendstring(&msgbuf, notice->line);
			}

			if (notice->func)
			{
				pq_sendbyte(&msgbuf, PG_DIAG_SOURCE_FUNCTION);
				pq_sendstring(&msgbuf, notice->func);
			}

			pq_sendbyte(&msgbuf, '\0'); /* terminator */
		}
		else
		{
			/* Old style --- gin up a backwards-compatible message */
			StringInfoData buf;

			initStringInfo(&buf);

			appendStringInfo(&buf, "%s:  ", notice->severity);

			if (notice->func)
				appendStringInfo(&buf, "%s: ", notice->func);

			if (notice->message)
				appendStringInfoString(&buf, notice->message);
			else
				appendStringInfoString(&buf, libpq_gettext("missing error text"));

			appendStringInfoChar(&buf, '\n');

			pq_sendstring(&msgbuf, buf.data);

			pfree(buf.data);
		}

		pq_endmessage(&msgbuf);
		free(notice);
	}
	PG_CATCH();
	{
		free(notice);
		PG_RE_THROW();
	}
	PG_END_TRY();

	pq_flush();
}
