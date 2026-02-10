
/*-------------------------------------------------------------------------
 *
 * px_disp_async.c
 *	  Functions for asynchronous implementation of dispatching
 *	  commands to PXxecutors.
 *
 * Portions Copyright (c) 2005-2008, Greenplum inc
 * Portions Copyright (c) 2012-Present Pivotal Software, Inc.
 * Portions Copyright (c) 2021, Alibaba Group Holding Limited
 *
 * IDENTIFICATION
 *	  src/backend/px/dispatcher/px_disp_async.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#ifdef HAVE_POLL_H
#include <poll.h>
#endif
#ifdef HAVE_SYS_POLL_H
#include <sys/poll.h>
#endif

#include "access/xact.h"
#include "commands/sequence.h"
#include "libpq-fe.h"
#include "libpq-int.h"
#include "miscadmin.h"
#include "storage/ipc.h"		/* For proc_exit_inprogress  */
#include "storage/lwlock.h"
#include "tcop/tcopprot.h"
#include "utils/guc.h"
#include "utils/timestamp.h"

#include "px/px_disp.h"
#include "px/px_disp_async.h"
#include "px/px_dispatchresult.h"
#include "px/px_gang.h"
#include "px/px_vars.h"
#include "px/px_pq.h"
#include "px/px_conn.h"
#include "utils/faultinjector.h"

#define DISPATCH_WAIT_TIMEOUT_MSEC 2000

/*
 * Ideally, we should set timeout to zero to cancel PXs as soon as possible,
 * but considering the cost of sending cancel signal is high, we want to process
 * as many finishing PXs as possible before cancelling
 */
#define DISPATCH_WAIT_CANCEL_TIMEOUT_MSEC 100

static void *pxdisp_makeDispatchParams_async(int maxSlices, int largestGangSize,
											 char *queryText, int len);

static void pxdisp_checkDispatchResult_async(struct PxDispatcherState *ds,
											 DispatchWaitMode waitMode);

static void pxdisp_dispatchToGang_async(struct PxDispatcherState *ds,
										struct Gang *gp,
										int sliceIndex);
static void pxdisp_waitDispatchFinish_async(struct PxDispatcherState *ds);

static bool pxdisp_checkForCancel_async(struct PxDispatcherState *ds);
static int	pxdisp_getWaitSocketFd_async(struct PxDispatcherState *ds);

DispatcherInternalFuncs DispatcherAsyncFuncs =
{
	pxdisp_checkForCancel_async,
	pxdisp_getWaitSocketFd_async,
	pxdisp_makeDispatchParams_async,
	pxdisp_checkDispatchResult_async,
	pxdisp_dispatchToGang_async,
	pxdisp_waitDispatchFinish_async
};


static void dispatchCommand(PxDispatchResult *dispatchResult,
							char *query_text,
							int query_text_len);

static void checkDispatchResult(PxDispatcherState *ds,
								bool wait);

static bool processResults(PxDispatchResult *dispatchResult);

static void
			signalPXs(PxDispatchCmdAsync *pParms);

static void
			checkSegmentAlive(PxDispatchCmdAsync *pParms);

static void
			handlePollError(PxDispatchCmdAsync *pParms);

static void
			handlePollSuccess(PxDispatchCmdAsync *pParms, struct pollfd *fds);

static void
			checkDispatchResult_thread(PxDispatcherState *ds, bool wait);

/*
 * Check dispatch result.
 * Don't wait all dispatch commands to complete.
 *
 * Return true if any connection received error.
 */
static bool
pxdisp_checkForCancel_async(struct PxDispatcherState *ds)
{
	Assert(ds);

	if (pxdisp_isDsThreadRuning())
		px_adps_dispatch_wait = false;

	checkDispatchResult(ds, false);
	return pxdisp_checkResultsErrcode(ds->primaryResults);
}

/*
 * Return a FD to wait for, after dispatching.
 */
static int
pxdisp_getWaitSocketFd_async(struct PxDispatcherState *ds)
{
	PxDispatchCmdAsync *pParms = (PxDispatchCmdAsync *) ds->dispatchParams;
	int			i;

	Assert(ds);

	if (proc_exit_inprogress)
		return PGINVALID_SOCKET;

	/* If libpq-thread, should not wait for socket any more */
	if (pxdisp_isDsThreadRuning())
		return PGINVALID_SOCKET;

	/*
	 * This should match the logic in pxdisp_checkForCancel_async(). In
	 * particular, when pxdisp_checkForCancel_async() is called, it must
	 * process any incoming data from the socket we return here, or we will
	 * busy wait.
	 */
	for (i = 0; i < pParms->dispatchCount; i++)
	{
		PxDispatchResult *dispatchResult;
		PxWorkerDescriptor *pxWorkerDesc;

		dispatchResult = pParms->dispatchResultPtrArray[i];
		pxWorkerDesc = dispatchResult->pxWorkerDesc;

		/*
		 * Already finished with this PX?
		 */
		if (!dispatchResult->stillRunning)
			continue;

		Assert(!pxconn_isBadConnection(pxWorkerDesc));

		return PQsocket(pxWorkerDesc->conn);
	}

	return PGINVALID_SOCKET;
}

/*
 * Block until all data are dispatched.
 */
static void
pxdisp_waitDispatchFinish_async(struct PxDispatcherState *ds)
{
	const static int DISPATCH_POLL_TIMEOUT = 500;
	struct pollfd *fds;
	int			nfds,
				i;
	char	   *msg;
	PxDispatchCmdAsync *pParms = (PxDispatchCmdAsync *) ds->dispatchParams;
	int			dispatchCount = pParms->dispatchCount;

	fds = (struct pollfd *) palloc(dispatchCount * sizeof(struct pollfd));

	while (true)
	{
		int			pollRet;

		nfds = 0;
		memset(fds, 0, dispatchCount * sizeof(struct pollfd));

		for (i = 0; i < dispatchCount; i++)
		{
			PxDispatchResult *pxResult = pParms->dispatchResultPtrArray[i];
			PxWorkerDescriptor *pxWorkerDesc = pxResult->pxWorkerDesc;
			PGconn	   *conn = pxWorkerDesc->conn;
			int			ret;

#ifdef FAULT_INJECTOR
			if (SIMPLE_FAULT_INJECTOR("pxdisp_flush_nonblock") == FaultInjectorTypeEnable)
				goto FAULT_INJECTOR_FLUSH_NONBLOCK_LABEL;
#endif

			/* skip already completed connections */
			if (conn->outCount == 0)
				continue;

#ifdef FAULT_INJECTOR
	FAULT_INJECTOR_FLUSH_NONBLOCK_LABEL:
#endif

			/*
			 * call send for this connection regardless of its POLLOUT status,
			 * because it may be writable NOW
			 */
			ret = pqFlushNonBlocking(conn);

#ifdef FAULT_INJECTOR
			if (SIMPLE_FAULT_INJECTOR("pxdisp_nonblock_zero") == FaultInjectorTypeEnable)
				ret = 0;
			else if (SIMPLE_FAULT_INJECTOR("pxdisp_nonblock_over_zero") == FaultInjectorTypeEnable)
				ret = 1;
			else if (SIMPLE_FAULT_INJECTOR("pxdisp_nonblock_less_zero") == FaultInjectorTypeEnable)
				ret = -1;
#endif

			if (ret == 0)
				continue;
			else if (ret > 0)
			{
				int			sock = PQsocket(pxWorkerDesc->conn);

				Assert(sock >= 0);
				fds[nfds].fd = sock;
				fds[nfds].events = POLLOUT;
				nfds++;
			}
			else if (ret < 0)
			{
				/*
				 * error message should be set up already TODO: pg15_px:
				 * verify it according to the changes made by 1f39a1c commit
				 */
				msg = PQerrorMessage(conn);

				pxResult->stillRunning = false;
				ereport(ERROR,
						(errcode(ERRCODE_PX_INTERCONNECTION_ERROR),
						 errmsg("Command could not be dispatch to segment %s: %s", pxResult->pxWorkerDesc->whoami, msg ? msg : "unknown error")));
			}
		}

#ifdef FAULT_INJECTOR
		if (SIMPLE_FAULT_INJECTOR("pxdisp_poll_fd") == FaultInjectorTypeEnable)
			goto FAULT_INJECTOR_POLL_FD;
#endif

		if (nfds == 0)
			break;

#ifdef FAULT_INJECTOR
FAULT_INJECTOR_POLL_FD:
#endif

		/* guarantee poll() is interruptible */
		do
		{
			CHECK_FOR_INTERRUPTS();

			pollRet = poll(fds, nfds, DISPATCH_POLL_TIMEOUT);
			if (pollRet == 0)
				ELOG_DISPATCHER_DEBUG("pxdisp_waitDispatchFinish_async(): Dispatch poll timeout after %d ms", DISPATCH_POLL_TIMEOUT);
		}
		while (pollRet == 0 || (pollRet < 0 && (SOCK_ERRNO == EINTR || SOCK_ERRNO == EAGAIN)));

		if (pollRet < 0)
			elog(ERROR, "Poll failed during dispatch");
	}

	pfree(fds);
}

/*
 * Dispatch command to gang.
 *
 * Throw out error to upper try-catch block if anything goes wrong. This function only kicks off dispatching,
 * call pxdisp_waitDispatchFinish_async to ensure the completion
 */
static void
pxdisp_dispatchToGang_async(struct PxDispatcherState *ds,
							struct Gang *gp,
							int sliceIndex)
{
	int			i;

	PxDispatchCmdAsync *pParms = NULL;

	pParms = (PxDispatchCmdAsync *) ds->dispatchParams;

	/*
	 * Start the dispatching
	 */
	for (i = 0; i < gp->size; i++)
	{
		PxDispatchResult *pxResult;

		PxWorkerDescriptor *pxWorkerDesc = gp->db_descriptors[i];

		Assert(pxWorkerDesc != NULL);

		/*
		 * Initialize the PX's PxDispatchResult object.
		 */
		pxResult = pxdisp_makeResult(ds->primaryResults, pxWorkerDesc, sliceIndex);
		if (pxResult == NULL)
		{
			elog(FATAL, "could not allocate resources for segworker communication");
		}
		pParms->dispatchResultPtrArray[pParms->dispatchCount++] = pxResult;

		dispatchCommand(pxResult, pParms->query_text, pParms->query_text_len);
	}
}

/*
 * Check dispatch result.
 *
 * Wait all dispatch work to complete, either success or fail.
 * (Set stillRunning to true when one dispatch work is completed)
 */
static void
pxdisp_checkDispatchResult_async(struct PxDispatcherState *ds,
								 DispatchWaitMode waitMode)
{
	PxDispatchCmdAsync *pParms = NULL;

	Assert(ds != NULL);
	pParms = (PxDispatchCmdAsync *) ds->dispatchParams;

	/* pxdisp_destroyDispatcherState is called */
	if (pParms == NULL)
		return;

	/*
	 * Don't overwrite DISPATCH_WAIT_CANCEL or DISPATCH_WAIT_FINISH with
	 * DISPATCH_WAIT_NONE
	 */
	if (waitMode != DISPATCH_WAIT_NONE)
		pParms->waitMode = waitMode;

	checkDispatchResult(ds, true);

	/*
	 * It looks like everything went fine, make sure we don't miss a user
	 * cancellation?
	 *
	 * The waitMode argument is NONE when we are doing "normal work".
	 */
	if (waitMode == DISPATCH_WAIT_NONE || waitMode == DISPATCH_WAIT_FINISH)
		CHECK_FOR_INTERRUPTS();
}

/*
 * Allocates memory for a PxDispatchCmdAsync structure and do the initialization.
 *
 * Memory will be freed in function pxdisp_destroyDispatcherState by deleting the
 * memory context.
 */
static void *
pxdisp_makeDispatchParams_async(int maxSlices, int largestGangSize, char *queryText, int len)
{
	int			maxResults = maxSlices * largestGangSize;
	int			size = 0;

	PxDispatchCmdAsync *pParms = palloc0(sizeof(PxDispatchCmdAsync));

	size = maxResults * sizeof(PxDispatchResult *);
	pParms->dispatchResultPtrArray = (PxDispatchResult **) palloc0(size);
	pParms->dispatchCount = 0;
	pParms->waitMode = DISPATCH_WAIT_NONE;
	pParms->query_text = queryText;
	pParms->query_text_len = len;

	return (void *) pParms;
}

/*
 * Receive and process results from all running PXs.
 *
 * wait: true, wait until all dispatch works are completed.
 *       false, return immediate when there's no more data.
 *
 * Don't throw out error, instead, append the error message to
 * PxDispatchResult.error_message.
 */
static void
checkDispatchResult(PxDispatcherState *ds,
					bool wait)
{
	PxDispatchCmdAsync *pParms = (PxDispatchCmdAsync *) ds->dispatchParams;
	PxDispatchResults *meleeResults = ds->primaryResults;
	PxWorkerDescriptor *pxWorkerDesc;
	PxDispatchResult *dispatchResult;
	int			i;
	int			db_count = 0;
	int			timeout = 0;
	bool		sentSignal = false;
	struct pollfd *fds;

	if (pxdisp_isDsThreadRuning())
		return checkDispatchResult_thread(ds, wait);

	db_count = pParms->dispatchCount;
	fds = (struct pollfd *) palloc(db_count * sizeof(struct pollfd));

	/*
	 * OK, we are finished submitting the command to the segdbs. Now, we have
	 * to wait for them to finish.
	 */
	for (;;)
	{
		int			sock;
		int			n;
		int			nfds = 0;
		PGconn	   *conn;

		/*
		 * bail-out if we are dying. Once QC dies, PX will recognize it
		 * shortly anyway.
		 */
		if (proc_exit_inprogress)
			break;

		/*
		 * escalate waitMode to cancel if: - user interrupt has occurred, - or
		 * an error has been reported by any PX, - in case the caller wants
		 * cancelOnError
		 */
		if ((IS_PX_NEED_CANCELED() || meleeResults->errcode) && meleeResults->cancelOnError)
			pParms->waitMode = DISPATCH_WAIT_CANCEL;

		/*
		 * Which PXs are still running and could send results to us?
		 */
		for (i = 0; i < db_count; i++)
		{
			dispatchResult = pParms->dispatchResultPtrArray[i];
			pxWorkerDesc = dispatchResult->pxWorkerDesc;
			conn = pxWorkerDesc->conn;

			/*
			 * Already finished with this PX?
			 */
			if (!dispatchResult->stillRunning)
				continue;

			Assert(!pxconn_isBadConnection(pxWorkerDesc));

			/*
			 * Flush out buffer in case some commands are not fully dispatched
			 * to PXs, this can prevent QC from polling on such PXs forever.
			 */
			if (conn->outCount > 0)
			{
				/*
				 * Don't error out here, let following poll() routine to
				 * handle it.
				 */
				if (pqFlush(conn) < 0)
					elog(LOG, "Failed flushing outbound data to %s:%s",
						 pxWorkerDesc->whoami, PQerrorMessage(conn));
			}

			/*
			 * Add socket to fd_set if still connected.
			 */
			sock = PQsocket(conn);
			Assert(sock >= 0);
			fds[nfds].fd = sock;
			fds[nfds].events = POLLIN;
			nfds++;
		}

		/*
		 * Break out when no PXs still running.
		 */
		if (nfds <= 0)
			break;

		/*
		 * Wait for results from PXs
		 *
		 * Don't wait if: - this is called from interconnect to check if
		 * there's any error.
		 *
		 * Lower the timeout if: - we need send signal to PXs.
		 */
		if (!wait)
			timeout = 0;
		else if (pParms->waitMode == DISPATCH_WAIT_NONE || sentSignal)
			timeout = DISPATCH_WAIT_TIMEOUT_MSEC;
		else
			timeout = DISPATCH_WAIT_CANCEL_TIMEOUT_MSEC;

		n = poll(fds, nfds, timeout);

		/*
		 * poll returns with an error, including one due to an interrupted
		 * call
		 */
		if (n < 0
#ifdef FAULT_INJECTOR
			|| SIMPLE_FAULT_INJECTOR("px_check_dispatch_no_result") == FaultInjectorTypeEnable
#endif
			)
		{
			int			sock_errno = SOCK_ERRNO;

			if (sock_errno == EINTR)
				continue;

			elog(LOG, "handlePollError poll() failed; errno=%d", sock_errno);

			handlePollError(pParms);

			/*
			 * Since an error was detected for the segment, request FTS to
			 * perform a probe before checking the segment state.
			 */
			checkSegmentAlive(pParms);

			if (pParms->waitMode != DISPATCH_WAIT_NONE)
			{
				signalPXs(pParms);
				sentSignal = true;
			}

			if (!wait)
				break;
		}
		/* If the time limit expires, poll() returns 0 */
		else if (n == 0)
		{
			if (pParms->waitMode != DISPATCH_WAIT_NONE)
			{
				signalPXs(pParms);
				sentSignal = true;
			}

			if (!wait)
				break;
		}
		/* We have data waiting on one or more of the connections. */
		else
			handlePollSuccess(pParms, fds);
	}

	pfree(fds);
}

/*
 * Helper function that actually kicks off the command on the libpq connection.
 */
static void
dispatchCommand(PxDispatchResult *dispatchResult,
				char *query_text,
				int query_text_len)
{
	TimestampTz beforeSend = 0;
	long		secs;
	int			usecs;

	if (DEBUG1 >= log_min_messages)
		beforeSend = GetCurrentTimestamp();

	/*
	 * Submit the command asynchronously.
	 */
	if (PQsendPxQuery_shared(dispatchResult->pxWorkerDesc->conn,
							 (char *) query_text,
							 query_text_len,
							 px_enable_dispatch_async) == 0
#ifdef FAULT_INJECTOR
		|| SIMPLE_FAULT_INJECTOR("px_dispatch_command_error") == FaultInjectorTypeEnable
#endif
		)
	{
		char	   *msg = PQerrorMessage(dispatchResult->pxWorkerDesc->conn);

		dispatchResult->stillRunning = false;
		ereport(ERROR,
				(errcode(ERRCODE_PX_INTERCONNECTION_ERROR),
				 errmsg("Command could not be dispatch to segment %s: %s",
						dispatchResult->pxWorkerDesc->whoami, msg ? msg : "unknown error")));
	}

	if (DEBUG1 >= log_min_messages)
	{
		TimestampDifference(beforeSend, GetCurrentTimestamp(), &secs, &usecs);

		if (secs != 0 || usecs > 1000)	/* Time > 1ms? */
			elog(LOG, "time for PQsendPxQuery_shared %ld.%06d", secs, usecs);
	}

	/*
	 * We'll keep monitoring this PX -- whether or not the command was
	 * dispatched -- in order to check for a lost connection or any other
	 * errors that libpq might have in store for us.
	 */
	dispatchResult->stillRunning = true;
	dispatchResult->hasDispatched = true;

	ELOG_DISPATCHER_DEBUG("Command dispatched to PX (%s)", dispatchResult->pxWorkerDesc->whoami);
}

/*
 * Helper function to checkDispatchResult that handles errors that occur
 * during the poll() call.
 *
 * NOTE: The cleanup of the connections will be performed by handlePollTimeout().
 */
static void
handlePollError(PxDispatchCmdAsync *pParms)
{
	int			i;

	for (i = 0; i < pParms->dispatchCount; i++)
	{
		PxDispatchResult *dispatchResult = NULL;
		PxWorkerDescriptor *pxWorkerDesc = NULL;

		dispatchResult = pParms->dispatchResultPtrArray[i];
		pxWorkerDesc = dispatchResult->pxWorkerDesc;

		/* Skip if already finished or didn't dispatch. */
		if (!dispatchResult->stillRunning)
			continue;

		/* We're done with this PX, sadly. */
		if (PQstatus(pxWorkerDesc->conn) == CONNECTION_BAD
#ifdef FAULT_INJECTOR
			|| SIMPLE_FAULT_INJECTOR("handle_poll_bad") == FaultInjectorTypeEnable
#endif
			)
		{
			char	   *msg = PQerrorMessage(pxWorkerDesc->conn);

			if (msg)
				elog(LOG, "Dispatcher encountered connection error on %s: %s", pxWorkerDesc->whoami, msg);

			elog(LOG, "Dispatcher noticed bad connection in handlePollError()");

			/* Save error info for later. */
			pxdisp_appendMessageNonThread(dispatchResult, LOG,
										  "Error after dispatch from %s: %s",
										  pxWorkerDesc->whoami,
										  msg ? msg : "unknown error");

			PQfinish(pxWorkerDesc->conn);
			pxWorkerDesc->conn = NULL;
			dispatchResult->stillRunning = false;
		}
	}

	return;
}

/*
 * Receive and process results from PXs.
 */
static void
handlePollSuccess(PxDispatchCmdAsync *pParms,
				  struct pollfd *fds)
{
	int			currentFdNumber = 0;
	int			i = 0;

	/*
	 * We have data waiting on one or more of the connections.
	 */
	for (i = 0; i < pParms->dispatchCount; i++)
	{
		bool		finished;
		int			sock pg_attribute_unused();
		PxDispatchResult *dispatchResult = pParms->dispatchResultPtrArray[i];
		PxWorkerDescriptor *pxWorkerDesc = dispatchResult->pxWorkerDesc;

		/*
		 * Skip if already finished or didn't dispatch.
		 */
		if (!dispatchResult->stillRunning)
			continue;

		ELOG_DISPATCHER_DEBUG("looking for results from %d of %d (%s)",
							  i + 1, pParms->dispatchCount, pxWorkerDesc->whoami);

		sock = PQsocket(pxWorkerDesc->conn);
		Assert(sock >= 0);
		Assert(sock == fds[currentFdNumber].fd);

		/*
		 * Skip this connection if it has no input available.
		 */
		if (!(fds[currentFdNumber++].revents & POLLIN))
			continue;

		ELOG_DISPATCHER_DEBUG("PQsocket says there are results from %d of %d (%s)",
							  i + 1, pParms->dispatchCount, pxWorkerDesc->whoami);

		/*
		 * Receive and process results from this PX.
		 */
		finished = processResults(dispatchResult);

		/*
		 * Are we through with this PX now?
		 */
		if (finished)
		{
			dispatchResult->stillRunning = false;

			ELOG_DISPATCHER_DEBUG("processResults says we are finished with %d of %d (%s)",
								  i + 1, pParms->dispatchCount, pxWorkerDesc->whoami);

			if (DEBUG1 >= log_min_messages)
			{
				char		msec_str[32];

				switch (check_log_duration(msec_str, false))
				{
					case 1:
					case 2:
						elog(LOG, "duration to dispatch result received from %d (seg %d): %s ms",
							 i + 1, dispatchResult->pxWorkerDesc->logicalWorkerInfo.idx, msec_str);
						break;
				}
			}

			if (PQisBusy(dispatchResult->pxWorkerDesc->conn))
				elog(LOG, "We thought we were done, because finished==true, but libpq says we are still busy");
		}
		else
			ELOG_DISPATCHER_DEBUG("processResults says we have more to do with %d of %d (%s)",
								  i + 1, pParms->dispatchCount, pxWorkerDesc->whoami);
	}
}

/*
 * Send finish or cancel signal to PXs if needed.
 */
static void
signalPXs(PxDispatchCmdAsync *pParms)
{
	int			i;
	DispatchWaitMode waitMode = pParms->waitMode;

	for (i = 0; i < pParms->dispatchCount; i++)
	{
		char		errbuf[256];
		bool		sent = false;
		PxWorkerDescriptor *pxWorkerDesc;
		PxDispatchResult *dispatchResult = pParms->dispatchResultPtrArray[i];

		Assert(dispatchResult != NULL);
		pxWorkerDesc = dispatchResult->pxWorkerDesc;

		/*
		 * Don't send the signal if - PX is finished or canceled - the signal
		 * was already sent - connection is dead
		 */

		if (!dispatchResult->stillRunning ||
			dispatchResult->wasCanceled ||
			pxconn_isBadConnection(pxWorkerDesc))
			continue;

		memset(errbuf, 0, sizeof(errbuf));

		sent = pxconn_signalPX(pxWorkerDesc, errbuf, waitMode == DISPATCH_WAIT_CANCEL);
		if (sent)
			dispatchResult->sentSignal = waitMode;
		else
			elog(LOG, "Unable to cancel: %s",
				 strlen(errbuf) == 0 ? "cannot allocate PGCancel" : errbuf);
	}
}

/*
 * Check if any segment DB down is detected by FTS.
 */
static void
checkSegmentAlive(PxDispatchCmdAsync *pParms)
{
	int			i;

	/*
	 * check the connection still valid
	 */
	for (i = 0; i < pParms->dispatchCount; i++)
	{
		PxDispatchResult *dispatchResult = pParms->dispatchResultPtrArray[i];
		PxWorkerDescriptor *pxWorkerDesc = dispatchResult->pxWorkerDesc;

		/*
		 * Skip if already finished or didn't dispatch.
		 */
		if (!dispatchResult->stillRunning)
			continue;

		/*
		 * Skip the entry db.
		 */
		if (pxWorkerDesc->logicalWorkerInfo.idx < 0)
			continue;

		ELOG_DISPATCHER_DEBUG("FTS testing connection %d of %d (%s)",
							  i + 1, pParms->dispatchCount, pxWorkerDesc->whoami);
	}
}

static inline void
send_sequence_response(PGconn *conn, Oid oid, int64 last, int64 cached, int64 increment, bool overflow, bool error)
{
	if (pqPutMsgStart(SEQ_NEXTVAL_QUERY_RESPONSE, conn) < 0 ||
		pqPutInt(oid, 4, conn) < 0 ||
		pqPutInt(last >> 32, 4, conn) < 0 ||
		pqPutInt(last, 4, conn) < 0 ||
		pqPutInt(cached >> 32, 4, conn) < 0 ||
		pqPutInt(cached, 4, conn) < 0 ||
		pqPutInt(increment >> 32, 4, conn) < 0 ||
		pqPutInt(increment, 4, conn) < 0 ||
		pqPutc(overflow ? SEQ_NEXTVAL_TRUE : SEQ_NEXTVAL_FALSE, conn) < 0 ||
		pqPutc(error ? SEQ_NEXTVAL_TRUE : SEQ_NEXTVAL_FALSE, conn) < 0 ||
		pqPutMsgEnd(conn) < 0 ||
		pqFlush(conn) < 0)

		/*
		 * error message should be set up already TODO: pg15_px: verify it
		 * according to the changes made by 1f39a1c commit
		 */
		;
	return;
}

/*
 * Receive and process input from one PX.
 *
 * Return true if all input are consumed or the connection went wrong.
 * Return false if there'er still more data expected.
 */
static bool
processResults(PxDispatchResult *dispatchResult)
{
	PxWorkerDescriptor *pxWorkerDesc = dispatchResult->pxWorkerDesc;
	PGnotify   *nextval = NULL;
	char	   *msg;

	/*
	 * Receive input from PX.
	 */
	if (PQconsumeInput(pxWorkerDesc->conn) == 0
#ifdef FAULT_INJECTOR
		|| SIMPLE_FAULT_INJECTOR("px_process_results_input") == FaultInjectorTypeEnable
#endif
		)
	{
		msg = PQerrorMessage(pxWorkerDesc->conn);
		pxdisp_appendMessageNonThread(dispatchResult, LOG,
									  "Error on receive from %s: %s",
									  pxWorkerDesc->whoami, msg ? msg : "unknown error");
		return true;
	}

	/*
	 * If we have received one or more complete messages, process them.
	 */
	while (!PQisBusy(pxWorkerDesc->conn))
	{
		/* loop to call PQgetResult; won't block */
		PGresult   *pRes;
		ExecStatusType resultStatus;
		int			resultIndex;

		/*
		 * PQisBusy() does some error handling, which can cause the connection
		 * to die -- we can't just continue on as if the connection is happy
		 * without checking first.
		 *
		 * For example, pxdisp_numPGresult() will return a completely bogus
		 * value!
		 */
		if (pxconn_isBadConnection(pxWorkerDesc)
#ifdef FAULT_INJECTOR
			|| SIMPLE_FAULT_INJECTOR("px_process_bad_connection") == FaultInjectorTypeEnable
#endif
			)
		{
			msg = PQerrorMessage(pxWorkerDesc->conn);
			pxdisp_appendMessageNonThread(dispatchResult, LOG,
										  "Connection lost when receiving from %s: %s",
										  pxWorkerDesc->whoami, msg ? msg : "unknown error");
			return true;
		}

		/*
		 * Get one message.
		 */
		ELOG_DISPATCHER_DEBUG("PQgetResult");
		pRes = PQgetResult(pxWorkerDesc->conn);

		/*
		 * Command is complete when PGgetResult() returns NULL. It is critical
		 * that for any connection that had an asynchronous command sent thru
		 * it, we call PQgetResult until it returns NULL. Otherwise, the next
		 * time a command is sent to that connection, it will return an error
		 * that there's a command pending.
		 */
		if (!pRes)
		{
			ELOG_DISPATCHER_DEBUG("%s -> idle", pxWorkerDesc->whoami);
			/* this is normal end of command */
			return true;
		}

		/*
		 * Attach the PGresult object to the PxDispatchResult object.
		 */
		resultIndex = pxdisp_numPGresult(dispatchResult);
		pxdisp_appendResult(dispatchResult, pRes);

		/*
		 * Did a command complete successfully?
		 */
		resultStatus = PQresultStatus(pRes);
		if (resultStatus == PGRES_COMMAND_OK ||
			resultStatus == PGRES_TUPLES_OK ||
			resultStatus == PGRES_COPY_IN ||
			resultStatus == PGRES_COPY_OUT ||
			resultStatus == PGRES_EMPTY_QUERY)
		{
			ELOG_DISPATCHER_DEBUG("%s -> ok %s",
								  pxWorkerDesc->whoami,
								  PQcmdStatus(pRes) ? PQcmdStatus(pRes) : "(no cmdStatus)");

			if (resultStatus == PGRES_EMPTY_QUERY)
				ELOG_DISPATCHER_DEBUG("PX received empty query.");

			/*
			 * Save the index of the last successful PGresult. Can be given to
			 * pxdisp_getPGresult() to get tuple count, etc.
			 */
			dispatchResult->okindex = resultIndex;

			/*
			 * SREH - get number of rows rejected from PX if any
			 */
			if (pRes->numRejected > 0)
				dispatchResult->numrowsrejected += pRes->numRejected;

			/*
			 * COPY FROM ON SEGMENT - get the number of rows completed by PX
			 * if any
			 */
			if (pRes->numCompleted > 0)
				dispatchResult->numrowscompleted += pRes->numCompleted;

			if (resultStatus == PGRES_COPY_IN ||
				resultStatus == PGRES_COPY_OUT)
				return true;
		}

		/*
		 * Note PX error. Cancel the whole statement if requested.
		 */
		else
		{
			/* PX reported an error */
			char	   *sqlstate = PQresultErrorField(pRes, PG_DIAG_SQLSTATE);
			int			errcode = 0;

			msg = PQresultErrorMessage(pRes);

			ELOG_DISPATCHER_DEBUG("%s -> %s %s  %s",
								  pxWorkerDesc->whoami,
								  PQresStatus(resultStatus),
								  sqlstate ? sqlstate : "(no SQLSTATE)",
								  msg);

			/*
			 * Convert SQLSTATE to an error code (ERRCODE_xxx). Use a generic
			 * nonzero error code if no SQLSTATE.
			 */
			if (sqlstate && strlen(sqlstate) == 5)
				errcode = px_sqlstate_to_errcode(sqlstate);

			/*
			 * Save first error code and the index of its PGresult buffer
			 * entry.
			 */
			pxdisp_seterrcode(errcode, resultIndex, dispatchResult);
		}
	}

	/*
	 * If there was nextval request then respond back on this libpq connection
	 * with the next value. Check and process nextval message only if QC has
	 * not already hit the error. Since QC could have hit the error while
	 * processing the previous nextval_qc() request itself and since full
	 * error handling is not complete yet like releasing all the locks, etc..,
	 * shouldn't attempt to call nextval_qc() again.
	 */
	nextval = PQnotifies(pxWorkerDesc->conn);

	if (((px_elog_geterrcode() == 0) && nextval &&
		 strcmp(nextval->relname, "nextval") == 0)
#ifdef FAULT_INJECTOR
		|| SIMPLE_FAULT_INJECTOR("nextval_inject") == FaultInjectorTypeEnable
#endif
		)
	{
		int64		last;
		int64		cached;
		int64		increment;
		bool		overflow;
		int			dbid;
		int			seq_oid;

		if (sscanf(nextval->extra, "%d:%d", &dbid, &seq_oid) != 2)
			elog(ERROR, "invalid nextval message");

		if (dbid != MyDatabaseId)
			elog(ERROR, "nextval message database id:%d doesn't match my database id:%d",
				 dbid, MyDatabaseId);

		PG_TRY();
		{
			nextval_qc(seq_oid, &last, &cached, &increment, &overflow);
		}
		PG_CATCH();
		{
			send_sequence_response(pxWorkerDesc->conn, seq_oid, last, cached, increment, overflow, true /* error */ );
			PG_RE_THROW();
		}
		PG_END_TRY();

		/*
		 * respond back on this libpq connection with the next value
		 */
		send_sequence_response(pxWorkerDesc->conn, seq_oid, last, cached, increment, overflow, false /* error */ );
	}
	if (nextval)
		PQfreemem(nextval);

	return false;				/* we must keep on monitoring this socket */
}

static void
checkDispatchResult_thread(PxDispatcherState *ds, bool wait)
{
	PxDispatchCmdAsync *pParms = (PxDispatchCmdAsync *) ds->dispatchParams;
	PxDispatchResult *dispatchResult;

	elog(DEBUG5, "pq_thread: begin check dispatch result from libpq thread.");
	for (;;)
	{
		int			i;
		bool		is_all_finish = true;

		if (proc_exit_inprogress)
			break;
		CHECK_FOR_INTERRUPTS();
		for (i = 0; i < pParms->dispatchCount; i++)
		{
			dispatchResult = pParms->dispatchResultPtrArray[i];

			/*
			 * Already finished with this PX
			 */
			if (!dispatchResult->stillRunning)
				continue;

			if (pxconn_isBadConnection(dispatchResult->pxWorkerDesc))
				elog(ERROR, "px check dispatch result error");

			is_all_finish = false;
		}
		if (!wait || is_all_finish)
			break;
		pg_usleep(1);
	}
	elog(DEBUG5, "pq_thread: finish check dispatch result from libpq thread.");
}
