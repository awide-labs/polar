/*-------------------------------------------------------------------------
 *
 * px_disp.c
 *	  Functions to dispatch commands to PX executors.
 *
 *
 * Portions Copyright (c) 2005-2008, Greenplum inc
 * Portions Copyright (c) 2012-Present Pivotal Software, Inc.
 * Portions Copyright (c) 2021, Alibaba Group Holding Limited
 *
 *
 * IDENTIFICATION
 *	  src/backend/px/dispatcher/px_disp.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "executor/execUtils_px.h"
#include "libpq-fe.h"
#include "libpq-int.h"
#include "storage/ipc.h"		/* For proc_exit_inprogress */
#include "tcop/tcopprot.h"
#include "utils/resowner.h"

#include "px/px_disp.h"
#include "px/px_disp_async.h"
#include "px/px_dispatchresult.h"
#include "px/px_gang.h"
#include "px/px_sreh.h"
#include "px/px_vars.h"

static int	numNonExtendedDispatcherState = 0;

dispatcher_handle_t *open_dispatcher_handles;

static dispatcher_handle_t *find_dispatcher_handle(PxDispatcherState *ds);
static dispatcher_handle_t *allocate_dispatcher_handle(void);
static void destroy_dispatcher_handle(dispatcher_handle_t *h);
static char *segmentsListToString(const char *prefix, List *segments);

static DispatcherInternalFuncs *pDispatchFuncs = &DispatcherAsyncFuncs;

/*
 * pxdisp_dispatchToGang:
 * Send the strCommand SQL statement to the subset of all segdbs in the cluster
 * specified by the gang parameter. cancelOnError indicates whether an error
 * occurring on one of the qExec segdbs should cause all still-executing commands to cancel
 * on other qExecs. Normally this would be true. The commands are sent over the libpq
 * connections that were established during pxlink_setup.
 *
 * The caller must provide a PxDispatchResults object having available
 * resultArray slots sufficient for the number of PXs to be dispatched:
 * i.e., resultCapacity - resultCount >= gp->size.	This function will
 * assign one resultArray slot per PX of the Gang, paralleling the Gang's
 * db_descriptors array. Success or failure of each PX will be noted in
 * the PX's PxDispatchResult entry; but before examining the results, the
 * caller must wait for execution to end by calling pxdisp_checkDispatchResult().
 *
 * The PxDispatchResults object owns some malloc'ed storage, so the caller
 * must make certain to free it by calling pxdisp_destroyDispatcherState().
 *
 * When dispatchResults->cancelOnError is false, strCommand is to be
 * dispatched to every connected gang member if possible, despite any
 * cancellation requests, PX errors, connection failures, etc.
 *
 * NB: This function should return normally even if there is an error.
 * It should not longjmp out via elog(ERROR, ...), ereport(ERROR, ...),
 * PG_THROW, CHECK_FOR_INTERRUPTS, etc.
 */
void
pxdisp_dispatchToGang(struct PxDispatcherState *ds,
					  struct Gang *gp,
					  int sliceIndex)
{
	Assert(px_role == PX_ROLE_QC);
	Assert(gp && gp->size > 0);
	Assert(ds->primaryResults && ds->primaryResults->resultArray);

	(pDispatchFuncs->dispatchToGang) (ds, gp, sliceIndex);
}

/*
 * For asynchronous dispatcher, we have to wait all dispatch to finish before we move on to query execution,
 * otherwise we may get into a deadlock situation, e.g, gather motion node waiting for data,
 * while segments waiting for plan.
 */
void
pxdisp_waitDispatchFinish(struct PxDispatcherState *ds)
{
	if (pDispatchFuncs->waitDispatchFinish != NULL)
		(pDispatchFuncs->waitDispatchFinish) (ds);
}

/*
 * pxdisp_checkDispatchResult:
 *
 * Waits for completion of threads launched by pxdisp_dispatchToGang().
 *
 * PXs that were dispatched with 'cancelOnError' true and are not yet idle
 * will be canceled/finished according to waitMode.
 */
void
pxdisp_checkDispatchResult(struct PxDispatcherState *ds,
						   DispatchWaitMode waitMode)
{
	(pDispatchFuncs->checkResults) (ds, waitMode);

	if (px_log_dispatch_stats)
		ShowUsage("DISPATCH STATISTICS");

	if (DEBUG1 >= log_min_messages)
	{
		char		msec_str[32];

		switch (check_log_duration(msec_str, false))
		{
			case 1:
			case 2:
				ereport(LOG,
						(errmsg("duration to dispatch result received from all PXs: %s ms", msec_str)));
				break;
		}
	}
}

/*
 * pxdisp_getDispatchResults:
 *
 * Block until all PXs return results or report errors.
 *
 * Return Values:
 *   Return NULL If one or more PXs got Error. In that case, *pxErrors contains
 *   a list of ErrorDatas.
 */
struct PxDispatchResults *
pxdisp_getDispatchResults(struct PxDispatcherState *ds, ErrorData **pxError)
{
	int			errorcode;

	if (!ds || !ds->primaryResults)
	{
		/*
		 * Fallback in case we have no dispatcher state.  Since the caller is
		 * likely to output the errors on NULL return, add an error message to
		 * aid debugging.
		 */
		if (errstart(ERROR, TEXTDOMAIN))
		{
			errcode(ERRCODE_INTERNAL_ERROR);
			errmsg("no dispatcher state");
			*pxError = px_errfinish_and_return(__FILE__, __LINE__, PG_FUNCNAME_MACRO);
		}
		else
			pg_unreachable();

		return NULL;
	}

	/* check if any error reported */
	errorcode = ds->primaryResults->errcode;

	if (errorcode)
	{
		pxdisp_dumpDispatchResults(ds->primaryResults, pxError);
		return NULL;
	}

	return ds->primaryResults;
}

/*
 * PxDispatchHandleError
 *
 * When caller catches an error, the PG_CATCH handler can use this
 * function instead of pxdisp_finishCommand to wait for all PXs
 * to finish, and report PX errors if appropriate.
 * This function should be called only from PG_CATCH handlers.
 *
 * This function doesn't cleanup dispatcher state, dispatcher state
 * will be destroyed as part of the resource owner cleanup.
 *
 * On return, the caller is expected to finish its own cleanup and
 * exit via PG_RE_THROW().
 */
void
PxDispatchHandleError(struct PxDispatcherState *ds)
{
	int			qderrcode;
	bool		useQeError = false;
	ErrorData  *error = NULL;

	/*
	 * If pxdisp_dispatchToGang() wasn't called, don't wait.
	 */
	if (!ds || !ds->primaryResults)
		return;

	/*
	 * Request any remaining commands executing on qExecs to stop. We need to
	 * wait for the threads to finish. This allows for proper cleanup of the
	 * results from the async command executions. Cancel any PXs still
	 * running.
	 */
	pxdisp_cancelDispatch(ds);

	/*
	 * When a PX stops executing a command due to an error, as a consequence
	 * there can be a cascade of interconnect errors (usually "sender closed
	 * connection prematurely") thrown in downstream processes (PXs and QC).
	 * So if we are handling an interconnect error, and a PX hit a more
	 * interesting error, we'll let the PX's error report take precedence.
	 */
	qderrcode = px_elog_geterrcode();
	if (qderrcode == ERRCODE_PX_INTERCONNECTION_ERROR)
	{
		bool		qd_lost_flag = false;
		char	   *qderrtext = elog_message();

		if (qderrtext
			&& strcmp(qderrtext, PX_MOTION_LOST_CONTACT_STRING) == 0)
			qd_lost_flag = true;

		if (ds->primaryResults && ds->primaryResults->errcode)
		{
			if (qd_lost_flag
				&& ds->primaryResults->errcode == ERRCODE_PX_INTERCONNECTION_ERROR)
				useQeError = true;
			else if (ds->primaryResults->errcode != ERRCODE_PX_INTERCONNECTION_ERROR)
				useQeError = true;
		}
	}

	if (useQeError)
	{
		/*
		 * Throw the PX's error, catch it, and fall thru to return normally so
		 * caller can finish cleaning up. Afterwards caller must exit via
		 * PG_RE_THROW().
		 */
		MemoryContext oldcontext;

		/*
		 * During abort processing, we are running in ErrorContext. Avoid
		 * doing these heavy things in ErrorContext. (There's one particular
		 * issue: these calls use CopyErrorData(), which asserts that we are
		 * not in ErrorContext.)
		 */
		oldcontext = MemoryContextSwitchTo(CurTransactionContext);

		PG_TRY();
		{
			pxdisp_getDispatchResults(ds, &error);
			if (error != NULL)
			{
				FlushErrorState();
				ReThrowError(error);
			}
		}
		PG_CATCH();
		{
		}						/* nop; fall thru */
		PG_END_TRY();

		MemoryContextSwitchTo(oldcontext);
	}
}



/*
 * Allocate memory and initialize PxDispatcherState.
 *
 * Call pxdisp_destroyDispatcherState to free it.
 */
PxDispatcherState *
pxdisp_makeDispatcherState(bool isExtendedQuery)
{
	dispatcher_handle_t *handle;

	if (!isExtendedQuery)
	{
		if (numNonExtendedDispatcherState == 1)
		{
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("query plan with multiple segworker groups is not supported"),
					 errhint("likely caused by a function that reads or modifies data in a distributed table")));


		}

		numNonExtendedDispatcherState++;
	}

	handle = allocate_dispatcher_handle();
	handle->dispatcherState->forceDestroyGang = false;
	handle->dispatcherState->isExtendedQuery = isExtendedQuery;
#ifdef USE_ASSERT_CHECKING
	handle->dispatcherState->isGangDestroying = false;
#endif
	handle->dispatcherState->allocatedGangs = NIL;
	handle->dispatcherState->largestGangSize = 0;

	return handle->dispatcherState;
}

void
pxdisp_makeDispatchParams(PxDispatcherState *ds,
						  int maxSlices,
						  char *queryText,
						  int queryTextLen)
{
	MemoryContext oldContext;
	void	   *dispatchParams;

	Assert(px_DispatcherContext);
	oldContext = MemoryContextSwitchTo(px_DispatcherContext);

	dispatchParams = (pDispatchFuncs->makeDispatchParams) (maxSlices, ds->largestGangSize, queryText, queryTextLen);

	ds->dispatchParams = dispatchParams;

	MemoryContextSwitchTo(oldContext);
}

/*
 * Free memory in PxDispatcherState
 *
 * Free dispatcher memory context.
 */
void
pxdisp_destroyDispatcherState(PxDispatcherState *ds)
{
	ListCell   *lc;
	PxDispatchResults *results;
	dispatcher_handle_t *h;

	if (!ds)
		return;
#ifdef USE_ASSERT_CHECKING
	/* Disallow reentrance. */
	Assert(!ds->isGangDestroying);
	ds->isGangDestroying = true;
#endif

	if (!ds->isExtendedQuery)
	{
		numNonExtendedDispatcherState--;
		Assert(numNonExtendedDispatcherState == 0);
	}

	results = ds->primaryResults;
	h = find_dispatcher_handle(ds);

	if (results != NULL && results->resultArray != NULL)
	{
		int			i;

		for (i = 0; i < results->resultCount; i++)
		{
			pxdisp_termResult(&results->resultArray[i]);
		}
		results->resultArray = NULL;
	}

	/*
	 * Recycle or destroy gang accordingly.
	 *
	 * We must recycle them in the reverse order of AllocateGang() to restore
	 * the original order of the idle gangs.
	 */
	foreach(lc, ds->allocatedGangs)
	{
		Gang	   *gp = lfirst(lc);

		RecycleGang(gp, ds->forceDestroyGang);
	}

	ds->allocatedGangs = NIL;
	ds->dispatchParams = NULL;
	ds->primaryResults = NULL;
	ds->largestGangSize = 0;

	if (h != NULL)
		destroy_dispatcher_handle(h);
}

void
pxdisp_cancelDispatch(PxDispatcherState *ds)
{
	pxdisp_checkDispatchResult(ds, DISPATCH_WAIT_CANCEL);
}

bool
pxdisp_checkForCancel(PxDispatcherState *ds)
{
	if (pDispatchFuncs == NULL || pDispatchFuncs->checkForCancel == NULL)
		return false;
	return (pDispatchFuncs->checkForCancel) (ds);
}

/*
 * Return a file descriptor to wait for events from the PXs after dispatching
 * a query.
 *
 * This is intended for use with pxdisp_checkForCancel(). First call
 * pxdisp_getWaitSocketFd(), and wait on that socket to become readable
 * e.g. with select() or poll(). When it becomes readable, call
 * pxdisp_checkForCancel() to process the incoming data, and repeat.
 *
 * XXX: This returns only one fd, but we might be waiting for results from
 * multiple PXs. In that case, this returns arbitrarily one of them. You
 * should still have a timeout, and call pxdisp_checkForCancel()
 * periodically, to process results from the other PXs.
 */
int
pxdisp_getWaitSocketFd(PxDispatcherState *ds)
{
	if (pDispatchFuncs == NULL || pDispatchFuncs->getWaitSocketFd == NULL)
		return -1;
	return (pDispatchFuncs->getWaitSocketFd) (ds);
}

dispatcher_handle_t *
allocate_dispatcher_handle(void)
{
	dispatcher_handle_t *h;

	if (px_DispatcherContext == NULL)
		px_DispatcherContext = AllocSetContextCreate(TopMemoryContext,
													 "Dispatch Context",
													 ALLOCSET_DEFAULT_MINSIZE,
													 ALLOCSET_DEFAULT_INITSIZE,
													 ALLOCSET_DEFAULT_MAXSIZE);


	h = MemoryContextAllocZero(px_DispatcherContext, sizeof(dispatcher_handle_t));

	h->dispatcherState = MemoryContextAllocZero(px_DispatcherContext, sizeof(PxDispatcherState));
	h->owner = CurrentResourceOwner;
	h->next = open_dispatcher_handles;
	h->prev = NULL;
	if (open_dispatcher_handles)
		open_dispatcher_handles->prev = h;
	open_dispatcher_handles = h;

	return h;
}

static void
destroy_dispatcher_handle(dispatcher_handle_t *h)
{
	h->dispatcherState = NULL;

	/* unlink from linked list first */
	if (h->prev)
		h->prev->next = h->next;
	else
		open_dispatcher_handles = h->next;
	if (h->next)
		h->next->prev = h->prev;

	pfree(h);

	if (open_dispatcher_handles == NULL)
		MemoryContextReset(px_DispatcherContext);
}

static dispatcher_handle_t *
find_dispatcher_handle(PxDispatcherState *ds)
{
	dispatcher_handle_t *head = open_dispatcher_handles;

	while (head != NULL)
	{
		if (head->dispatcherState == ds)
			return head;
		head = head->next;
	}
	return NULL;
}

static void
cleanup_dispatcher_handle(dispatcher_handle_t *h)
{
	if (h->dispatcherState == NULL)
	{
		destroy_dispatcher_handle(h);
		return;
	}

	pxdisp_cancelDispatch(h->dispatcherState);
	pxdisp_destroyDispatcherState(h->dispatcherState);
}

void
pxdisp_cleanupDispatcherHandle(const struct ResourceOwnerData *owner)
{
	dispatcher_handle_t *curr;
	dispatcher_handle_t *next;

	next = open_dispatcher_handles;
	while (next)
	{
		curr = next;
		next = curr->next;

		if (curr->owner == owner)
		{
			cleanup_dispatcher_handle(curr);
		}
	}
}

/*
 * segmentsListToString
 *		Utility routine to convert a segment list into a string.
 */
static char *
segmentsListToString(const char *prefix, List *segments)
{
	StringInfoData string;
	ListCell   *l;

	initStringInfo(&string);
	appendStringInfo(&string, "%s: ", prefix);

	foreach(l, segments)
	{
		int			segID = lfirst_int(l);

		appendStringInfo(&string, "%d ", segID);
	}

	return string.data;
}

char *
segmentsToContentStr(List *segments)
{
	int			size = list_length(segments);

	if (size == 0)
		return "ALL contents";
	else if (size == 1)
		return "SINGLE content";
	else if (size < getPxWorkerCount())
		return segmentsListToString("PARTIAL contents", segments);
	else
		return segmentsListToString("ALL contents", segments);
}

/*
 * Cleanup all dispatcher state that belong to
 * current resource owner and its childrens
 */
void
AtAbort_DispatcherState(void)
{
	if (px_role != PX_ROLE_QC)
		return;

	if (CurrentGangCreating != NULL)
	{
		RecycleGang(CurrentGangCreating, true);
		CurrentGangCreating = NULL;
	}

	/*
	 * Cleanup all outbound dispatcher states belong to current resource owner
	 * and its children
	 */
	PxResourceOwnerWalker(CurrentResourceOwner, pxdisp_cleanupDispatcherHandle);

	Assert(open_dispatcher_handles == NULL);
}
