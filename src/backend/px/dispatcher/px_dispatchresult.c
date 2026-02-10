/*-------------------------------------------------------------------------
 *
 * px_dispatchresult.c
 *	  Functions for handling dispatch results.
 *
 *
 * Portions Copyright (c) 2005-2008, Greenplum inc
 * Portions Copyright (c) 2012-Present Pivotal Software, Inc.
 * Portions Copyright (c) 2021, Alibaba Group Holding Limited
 *
 * IDENTIFICATION
 *	  src/backend/px/dispatcher/px_dispatchresult.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "libpq-fe.h"			/* prerequisite for libpq-int.h */
#include "libpq-int.h"			/* PQExpBufferData */
#include "utils/guc.h"			/* log_min_messages */

#include "px/px_conn.h"			/* PxWorkerDescriptor */
#include "px/px_dispatchresult.h"
#include "px/px_sreh.h"
#include "px/px_vars.h"
#include "utils/faultinjector.h"

static void
noTrailingNewlinePQ(PQExpBuffer buf)
{
	while (buf->len > 0 && buf->data[buf->len - 1] <= ' ' && buf->data[buf->len - 1] > '\0')
		buf->data[--buf->len] = '\0';
}

static void
oneTrailingNewlinePQ(PQExpBuffer buf)
{
	noTrailingNewlinePQ(buf);
	if (buf->len > 0)
		appendPQExpBufferChar(buf, '\n');
}

/*
 * Create a PxDispatchResult object, appending it to the
 * resultArray of a given PxDispatchResults object.
 */
PxDispatchResult *
pxdisp_makeResult(struct PxDispatchResults *meleeResults,
				  struct PxWorkerDescriptor *pxWorkerDesc, int sliceIndex)
{
	PxDispatchResult *dispatchResult;
	int			meleeIndex;

	Assert(meleeResults &&
		   meleeResults->resultArray &&
		   meleeResults->resultCount < meleeResults->resultCapacity);

	/*
	 * Allocate a slot for the new PxDispatchResult object.
	 */
	meleeIndex = meleeResults->resultCount++;
	dispatchResult = &meleeResults->resultArray[meleeIndex];

	/*
	 * Initialize PxDispatchResult.
	 */
	dispatchResult->meleeResults = meleeResults;
	dispatchResult->meleeIndex = meleeIndex;
	dispatchResult->pxWorkerDesc = pxWorkerDesc;
	dispatchResult->resultbuf = createPQExpBuffer();
	dispatchResult->error_message = createPQExpBuffer();
	dispatchResult->numrowsrejected = 0;
	dispatchResult->numrowscompleted = 0;

	if (PQExpBufferBroken(dispatchResult->resultbuf) ||
		PQExpBufferBroken(dispatchResult->error_message)
#ifdef FAULT_INJECTOR
		|| SIMPLE_FAULT_INJECTOR("pxdisp_makeresult_error") == FaultInjectorTypeEnable
#endif
		)
	{
		destroyPQExpBuffer(dispatchResult->resultbuf);
		dispatchResult->resultbuf = NULL;

		destroyPQExpBuffer(dispatchResult->error_message);
		dispatchResult->error_message = NULL;

		/*
		 * caller is responsible for cleanup -- can't elog(ERROR, ...) from
		 * here.
		 */
		return NULL;
	}

	/*
	 * Reset summary indicators.
	 */
	pxdisp_resetResult(dispatchResult);

	/*
	 * Update slice map entry.
	 */
	if (sliceIndex >= 0 && sliceIndex < meleeResults->sliceCapacity)
	{
		PxDispatchResults_SliceInfo *si = &meleeResults->sliceMap[sliceIndex];

		if (si->resultBegin == si->resultEnd)
		{
			si->resultBegin = meleeIndex;
			si->resultEnd = meleeIndex + 1;
		}
		else
		{
			if (si->resultBegin > meleeIndex)
				si->resultBegin = meleeIndex;
			if (si->resultEnd <= meleeIndex)
				si->resultEnd = meleeIndex + 1;
		}
	}

	return dispatchResult;
}

/*
 * Destroy a PxDispatchResult object.
 */
void
pxdisp_termResult(PxDispatchResult *dispatchResult)
{
	PQExpBuffer trash;

	dispatchResult->pxWorkerDesc = NULL;

	/*
	 * Free the PGresult objects.
	 */
	pxdisp_resetResult(dispatchResult);

	/*
	 * Free the error message buffer and result buffer.
	 */
	trash = dispatchResult->resultbuf;
	dispatchResult->resultbuf = NULL;
	destroyPQExpBuffer(trash);

	trash = dispatchResult->error_message;
	dispatchResult->error_message = NULL;
	destroyPQExpBuffer(trash);
}

/*
 * Reset a PxDispatchResult object for possible reuse.
 */
void
pxdisp_resetResult(PxDispatchResult *dispatchResult)
{
	PQExpBuffer buf = dispatchResult->resultbuf;
	PGresult  **begp = (PGresult **) buf->data;
	PGresult  **endp = (PGresult **) (buf->data + buf->len);
	PGresult  **p;

	/*
	 * Free the PGresult objects.
	 */
	for (p = begp; p < endp; ++p)
	{
		Assert(*p != NULL);
		PQclear(*p);
	}

	/*
	 * Reset summary indicators.
	 */
	dispatchResult->errcode = 0;
	dispatchResult->okindex = -1;

	/*
	 * Reset progress indicators.
	 */
	dispatchResult->hasDispatched = false;
	dispatchResult->stillRunning = false;
	dispatchResult->sentSignal = DISPATCH_WAIT_NONE;
	dispatchResult->wasCanceled = false;

	/*
	 * Empty (but don't free) the error message buffer and result buffer.
	 */
	resetPQExpBuffer(dispatchResult->resultbuf);
	resetPQExpBuffer(dispatchResult->error_message);
}

/*
 * Take note of an error.
 * 'errcode' is the ERRCODE_xxx value for setting the client's SQLSTATE.
 */
void
pxdisp_seterrcode(int errcode,	/* ERRCODE_xxx or 0 */
				  int resultIndex,	/* -1 if no PGresult */
				  PxDispatchResult *dispatchResult)
{
	PxDispatchResults *meleeResults = dispatchResult->meleeResults;

	/*
	 * We must ensure a nonzero errcode.
	 */
	if (!errcode)
		errcode = ERRCODE_INTERNAL_ERROR;

	/*
	 * Was the command canceled?
	 */
	if (errcode == ERRCODE_PX_OPERATION_CANCELED ||
		errcode == ERRCODE_QUERY_CANCELED)
		dispatchResult->wasCanceled = true;

	/*
	 * If this is the first error from this PX, save the error code and the
	 * index of the PGresult buffer entry. We assume the caller has not yet
	 * added the item to the PGresult buffer.
	 */
	if (!dispatchResult->errcode)
	{
		dispatchResult->errcode = errcode;
	}

	if (!meleeResults)
		return;

	/*
	 * Remember which PX reported an error first among the gangs, but keep
	 * quiet about cancellation done at our request.
	 *
	 * Interconnection errors are given lower precedence because often they
	 * are secondary to an earlier and more interesting error.
	 */
	if (errcode == ERRCODE_PX_OPERATION_CANCELED &&
		dispatchResult->sentSignal == DISPATCH_WAIT_CANCEL)
	{
		/* nop */
	}
	else if (meleeResults->errcode == 0 ||
			 (meleeResults->errcode == ERRCODE_PX_INTERCONNECTION_ERROR &&
			  errcode != ERRCODE_PX_INTERCONNECTION_ERROR))
	{
		meleeResults->errcode = errcode;
		meleeResults->iFirstError = dispatchResult->meleeIndex;
	}
}

/*
 * NonThread version of pxdisp_appendMessage.
 *
 * It's safe to use palloc/pfree or elog/ereport if not in thread.
 */
void
pxdisp_appendMessageNonThread(PxDispatchResult *dispatchResult,
							  int elevel, const char *fmt,...)
{
	va_list		args;
	int			msgoff;

	/*
	 * Remember first error.
	 */
	pxdisp_seterrcode(ERRCODE_PX_INTERCONNECTION_ERROR, -1, dispatchResult);

	/*
	 * Allocate buffer if first message. Insert newline between previous
	 * message and new one.
	 */
	Assert(dispatchResult->error_message != NULL);
	oneTrailingNewlinePQ(dispatchResult->error_message);

	msgoff = dispatchResult->error_message->len;

	/*
	 * Format the message and append it to the buffer.
	 */
	va_start(args, fmt);
	appendPQExpBufferVA(dispatchResult->error_message, fmt, args);
	va_end(args);

	/*
	 * Display the message on stderr for debugging, if requested. This helps
	 * to clarify the actual timing of threaded events.
	 */
	if (elevel >= log_min_messages)
	{
		oneTrailingNewlinePQ(dispatchResult->error_message);
		elog(LOG, "%s", dispatchResult->error_message->data + msgoff);
	}

	/*
	 * In case the caller wants to hand the buffer to ereport(), follow the
	 * ereport() convention of not ending with a newline.
	 */
	noTrailingNewlinePQ(dispatchResult->error_message);
}

/*
 * Thread version of pxdisp_appendMessage.
 *
 * It's unsafe to use palloc/pfree or elog/ereport if in thread.
 */
void
pxdisp_appendMessageThread(PxDispatchResult *dispatchResult,
						   int elevel, const char *fmt,...)
{
	va_list		args;
	int			msgoff;

	/*
	 * Remember first error.
	 */
	pxdisp_seterrcode(ERRCODE_PX_INTERCONNECTION_ERROR, -1, dispatchResult);

	/*
	 * Allocate buffer if first message. Insert newline between previous
	 * message and new one.
	 */
	Assert(dispatchResult->error_message != NULL);
	oneTrailingNewlinePQ(dispatchResult->error_message);

	msgoff = dispatchResult->error_message->len;

	/*
	 * Format the message and append it to the buffer.
	 */
	va_start(args, fmt);
	appendPQExpBufferVA(dispatchResult->error_message, fmt, args);
	va_end(args);

	/*
	 * Display the message on stderr for debugging, if requested. This helps
	 * to clarify the actual timing of threaded events.
	 */
	if (elevel >= log_min_messages)
	{
		oneTrailingNewlinePQ(dispatchResult->error_message);
		write_log("LOG: %s", dispatchResult->error_message->data + msgoff);
	}

	/*
	 * In case the caller wants to hand the buffer to ereport(), follow the
	 * ereport() convention of not ending with a newline.
	 */
	noTrailingNewlinePQ(dispatchResult->error_message);
}

/*
 * Store a PGresult object ptr in the result buffer.
 * NB: Caller must not PQclear() the PGresult object.
 */
void
pxdisp_appendResult(PxDispatchResult *dispatchResult, struct pg_result *res)
{
	Assert(dispatchResult && res);

	/*
	 * Attach the PX identification string to the PGresult
	 */
	if (dispatchResult->pxWorkerDesc && dispatchResult->pxWorkerDesc->whoami)
		pqSaveMessageField(res, PG_DIAG_PX_PROCESS_TAG, dispatchResult->pxWorkerDesc->whoami);

	appendBinaryPQExpBuffer(dispatchResult->resultbuf, (char *) &res, sizeof(res));
}

/*
 * Return the i'th PGresult object ptr (if i >= 0), or
 * the n+i'th one (if i < 0), or NULL (if i out of bounds).
 * NB: Caller must not PQclear() the PGresult object.
 */
struct pg_result *
pxdisp_getPGresult(PxDispatchResult *dispatchResult, int i)
{
	if (dispatchResult)
	{
		PQExpBuffer buf = dispatchResult->resultbuf;
		PGresult  **begp = (PGresult **) buf->data;
		PGresult  **endp = (PGresult **) (buf->data + buf->len);
		PGresult  **p = (i >= 0) ? &begp[i] : &endp[i];

		if (p >= begp && p < endp)
		{
			Assert(*p != NULL);
			return *p;
		}
	}
	return NULL;
}

/*
 * Return the number of PGresult objects in the result buffer.
 */
int
pxdisp_numPGresult(PxDispatchResult *dispatchResult)
{
	return dispatchResult ? dispatchResult->resultbuf->len / sizeof(PGresult *) : 0;
}

/*
 * Construct an ErrorData from the dispatch results.
 */
ErrorData *
pxdisp_dumpDispatchResult(PxDispatchResult *dispatchResult)
{
	int			ires;
	int			nres;
	ErrorData  *errdata;

	if (!dispatchResult)
		return NULL;

	/*
	 * Format PGresult messages
	 */
	nres = pxdisp_numPGresult(dispatchResult);
	for (ires = 0; ires < nres; ++ires)
	{
		PGresult   *pgresult = pxdisp_getPGresult(dispatchResult, ires);

		errdata = pxdisp_get_PXerror(pgresult);

		if (errdata)
			return errdata;
	}

	/*
	 * Error found on our side of the libpq interface?
	 */
	if (dispatchResult->error_message &&
		dispatchResult->error_message->len > 0)
	{
		if (errstart(ERROR, TEXTDOMAIN))
		{
			errcode(ERRCODE_PX_INTERCONNECTION_ERROR);
			errmsg("%s", dispatchResult->error_message->data);
			errdata = px_errfinish_and_return(__FILE__, __LINE__, PG_FUNCNAME_MACRO);
		}
		else
			pg_unreachable();

		return errdata;
	}

	return NULL;
}

/*
 * The returned error object is allocated in TopTransactionContext.
 *
 * Caution: do not use the returned object across transaction boundary.
 * Current usages of this API are such that the returned object is either
 * logged using elog() or rethrown, both within a transaction context, at the
 * time of finishing a dispatched command.  The caution applies to future uses
 * of this function.
 */
ErrorData *
pxdisp_get_PXerror(PGresult *pgresult)
{
	MemoryContext oldcontext;
	ExecStatusType resultStatus = PQresultStatus(pgresult);

	/*
	 * These will be overwritten below with the values from PX, if the PX sent
	 * them.
	 */
	char	   *filename = __FILE__;
	int			lineno = __LINE__;
	const char *funcname = PG_FUNCNAME_MACRO;
	int			px_errcode = ERRCODE_PX_INTERCONNECTION_ERROR;

	char	   *whoami;
	char	   *fld;
	ErrorData  *edata;

	/*
	 * PX success
	 */
	if (resultStatus == PGRES_COMMAND_OK ||
		resultStatus == PGRES_TUPLES_OK ||
		resultStatus == PGRES_COPY_IN ||
		resultStatus == PGRES_COPY_OUT ||
		resultStatus == PGRES_EMPTY_QUERY)
	{
		return NULL;
	}

	/*
	 * errstart need a const filename and funcname, make sure they are at
	 * least const in this transaction.
	 */
	oldcontext = MemoryContextSwitchTo(TopTransactionContext);
	fld = PQresultErrorField(pgresult, PG_DIAG_SOURCE_FILE);
	if (fld)
		filename = pstrdup(fld);

	fld = PQresultErrorField(pgresult, PG_DIAG_SOURCE_LINE);
	if (fld)
		lineno = atoi(fld);

	fld = PQresultErrorField(pgresult, PG_DIAG_SOURCE_FUNCTION);
	if (fld)
		funcname = pstrdup(fld);
	MemoryContextSwitchTo(oldcontext);

	/*
	 * We should only get errors with ERROR level or above, if the command
	 * failed. And if a PX disconnected with FATAL, or PANICed, we don't want
	 * to do the same in the QC. So, always an ERROR.
	 */
	if (!errstart(ERROR, TEXTDOMAIN))
		pg_unreachable();		/* unexpected path. */

	fld = PQresultErrorField(pgresult, PG_DIAG_SQLSTATE);
	if (fld)
		px_errcode = px_sqlstate_to_errcode(fld);
	errcode(px_errcode);

	whoami = PQresultErrorField(pgresult, PG_DIAG_PX_PROCESS_TAG);
	fld = PQresultErrorField(pgresult, PG_DIAG_MESSAGE_PRIMARY);
	if (!fld)
		fld = "no primary message received";

	if (whoami)
		errmsg("%s  (%s)", fld, whoami);
	else
		errmsg("%s", fld);

	fld = PQresultErrorField(pgresult, PG_DIAG_MESSAGE_DETAIL);
	if (fld)
		errdetail("%s", fld);

	fld = PQresultErrorField(pgresult, PG_DIAG_MESSAGE_HINT);
	if (fld)
		errhint("%s", fld);

	fld = PQresultErrorField(pgresult, PG_DIAG_CONTEXT);
	if (fld)
		errcontext("%s", fld);

	oldcontext = MemoryContextSwitchTo(TopTransactionContext);
	edata = px_errfinish_and_return(filename, lineno, funcname);

	MemoryContextSwitchTo(oldcontext);
	return edata;
}

/*
 * Format a PxDispatchResults object.
 * Returns an ErrorData object in *pxError if some error was found, or NIL if no errors.
 * Before calling this function, you must call PxCheckDispatchResult().
 */
void
pxdisp_dumpDispatchResults(struct PxDispatchResults *meleeResults,
						   ErrorData **pxError)
{
	PxDispatchResult *dispatchResult;

	/*
	 * Quick exit if no error (not counting ERRCODE_PX_OPERATION_CANCELED).
	 */
	if (!meleeResults || !meleeResults->errcode)
	{
		*pxError = NULL;
		return;
	}

	/*
	 * Find the PxDispatchResult of the first PX that got an error.
	 */
	Assert(meleeResults->iFirstError >= 0 &&
		   meleeResults->iFirstError < meleeResults->resultCount);

	dispatchResult = &meleeResults->resultArray[meleeResults->iFirstError];

	Assert(dispatchResult->meleeResults == meleeResults &&
		   dispatchResult->errcode != 0);

	/*
	 * Format one PX's result.
	 */
	*pxError = pxdisp_dumpDispatchResult(dispatchResult);
}

/*
 * Return sum of the cmdTuples values from PxDispatchResult
 * entries that have a successful PGresult. If sliceIndex >= 0,
 * uses only the results belonging to the specified slice.
 */
int64
pxdisp_sumCmdTuples(PxDispatchResults *results, int sliceIndex)
{
	PxDispatchResult *dispatchResult;
	PxDispatchResult *resultEnd = pxdisp_resultEnd(results, sliceIndex);
	PGresult   *pgresult;
	int64		sum = 0;

	for (dispatchResult = pxdisp_resultBegin(results, sliceIndex);
		 dispatchResult < resultEnd; ++dispatchResult)
	{
		pgresult = pxdisp_getPGresult(dispatchResult, dispatchResult->okindex);
		if (pgresult && !dispatchResult->errcode)
		{
			char	   *cmdTuples = PQcmdTuples(pgresult);

			if (cmdTuples)
				sum += atoll(cmdTuples);
		}
	}
	return sum;
}

/*
 * If several tuples were eliminated/rejected from the result because of
 * bad data formatting (this is currenly only possible in external tables
 * with single row error handling) - sum up the total rows rejected from
 * all PX's and notify the client.
 */
/*
 * ReportSrehResults
 *
 * When necessary emit a NOTICE that describes the end result of the
 * SREH operations. Information includes the total number of rejected
 * rows, and whether rows were ignored or logged into an error log file.
 */
void
ReportSrehResults(PxSreh *pxsreh, uint64 total_rejected)
{
	if (total_rejected > 0)
	{
		ereport(NOTICE,
				(errmsg("found " INT64_FORMAT " data formatting errors (" INT64_FORMAT " or more input rows), rejected related input data",
						total_rejected, total_rejected)));
	}
}

void
pxdisp_sumRejectedRows(PxDispatchResults *results)
{
	PxDispatchResult *dispatchResult;
	PxDispatchResult *resultEnd = pxdisp_resultEnd(results, -1);
	PGresult   *pgresult;
	uint64		totalRejected = 0;

	for (dispatchResult = pxdisp_resultBegin(results, -1);
		 dispatchResult < resultEnd; ++dispatchResult)
	{
		pgresult = pxdisp_getPGresult(dispatchResult, dispatchResult->okindex);
		if (pgresult && !dispatchResult->errcode)
		{
			/*
			 * add num rows rejected from this PX to the total
			 */
			totalRejected += dispatchResult->numrowsrejected;
		}
	}

	if (totalRejected > 0)
		ReportSrehResults(NULL, totalRejected);

}


/*
 * Find the max of the lastOid values returned from the QEs
 */
Oid
pxdisp_maxLastOid(PxDispatchResults *results, int sliceIndex)
{
	PxDispatchResult *dispatchResult;
	PxDispatchResult *resultEnd = pxdisp_resultEnd(results, sliceIndex);
	PGresult   *pgresult;
	Oid			oid = InvalidOid;

	for (dispatchResult = pxdisp_resultBegin(results, sliceIndex);
		 dispatchResult < resultEnd; ++dispatchResult)
	{
		pgresult = pxdisp_getPGresult(dispatchResult, dispatchResult->okindex);
		if (pgresult && !dispatchResult->errcode)
		{
			Oid			tmpoid = PQoidValue(pgresult);

			if (tmpoid > oid)
				oid = tmpoid;
		}
	}

	return oid;
}

/*
 * Return ptr to first resultArray entry for a given sliceIndex.
 */
PxDispatchResult *
pxdisp_resultBegin(PxDispatchResults *results, int sliceIndex)
{
	PxDispatchResults_SliceInfo *si;

	if (!results)
		return NULL;

	if (sliceIndex < 0)
		return &results->resultArray[0];

	Assert(sliceIndex < results->sliceCapacity);

	si = &results->sliceMap[sliceIndex];

	Assert(si->resultBegin >= 0 &&
		   si->resultBegin <= si->resultEnd &&
		   si->resultEnd <= results->resultCount);

	return &results->resultArray[si->resultBegin];
}

/*
 * Return ptr to last+1 resultArray entry for a given sliceIndex.
 */
PxDispatchResult *
pxdisp_resultEnd(PxDispatchResults *results, int sliceIndex)
{
	PxDispatchResults_SliceInfo *si;

	if (!results)
		return NULL;

	if (sliceIndex < 0)
		return &results->resultArray[results->resultCount];

	si = &results->sliceMap[sliceIndex];

	return &results->resultArray[si->resultEnd];
}

/*
 * used in the interconnect on the dispatcher to avoid error-cleanup deadlocks.
 */
bool
pxdisp_checkResultsErrcode(struct PxDispatchResults *meleeResults)
{
	if (meleeResults == NULL)
	{
		return false;
	}

	if (meleeResults->errcode)
	{
		return true;
	}

	return false;
}

/*
 * pxdisp_makeDispatchResults:
 * Allocates a PxDispatchResults object in the current memory context.
 * Will be freed in function pxdisp_destroyDispatcherState by deleting the
 * memory context.
 */
void
pxdisp_makeDispatchResults(PxDispatcherState *ds,
						   int sliceCapacity,
						   bool cancelOnError)
{
	PxDispatchResults *results;
	MemoryContext oldContext;
	int			resultCapacity;
	int			nbytes;

	Assert(px_DispatcherContext);
	oldContext = MemoryContextSwitchTo(px_DispatcherContext);

	resultCapacity = ds->largestGangSize * sliceCapacity;
	nbytes = resultCapacity * sizeof(results->resultArray[0]);

	results = palloc0(sizeof(*results));
	results->resultArray = palloc0(nbytes);
	results->resultCapacity = resultCapacity;
	results->resultCount = 0;
	results->iFirstError = -1;
	results->errcode = 0;
	results->cancelOnError = cancelOnError;

	results->sliceMap = NULL;
	results->sliceCapacity = sliceCapacity;
	if (sliceCapacity > 0)
	{
		nbytes = sliceCapacity * sizeof(results->sliceMap[0]);
		results->sliceMap = palloc0(nbytes);
	}

	MemoryContextSwitchTo(oldContext);

	ds->primaryResults = results;
}
