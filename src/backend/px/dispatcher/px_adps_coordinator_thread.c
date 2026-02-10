/*-------------------------------------------------------------------------
 *
 * px_adps_coordinator_thread.c
 *	  Create a background thread for libpq realtime interactive.
 *
 * Copyright (c) 2020, Alibaba Group Holding Limited
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * IDENTIFICATION
 *	  src/backend/px/dispatcher/px_adps_coordinator_thread.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

/* libpq */
#include "libpq-fe.h"
#include "libpq-int.h"
#include "pqexpbuffer.h"
#include "libpq/libpq.h"
#include "libpq/pqformat.h"

/* px interactive */
#include "px/px_conn.h"
#include "px/px_util.h"
#include "px/px_disp.h"
#include "px/px_disp_async.h"
#include "px/px_dispatchresult.h"

/* dynamic paging */
#include "px/px_adaptive_paging.h"
#include "px/px_vars.h"			/* for write_log */
#include "utils/builtins.h"		/* for from_hex */

/* break poll loop */
#include "storage/ipc.h"
#include "miscadmin.h"

/* system header */
#include <sys/prctl.h>
#include <pthread.h>
#include <unistd.h>
#ifdef HAVE_POLL_H
#include <poll.h>
#endif
#ifdef HAVE_SYS_POLL_H
#include <sys/poll.h>
#endif

/* generator random error */
#ifdef FAULT_INJECTOR
#include "px/px_libpq_fault_injection.h"
#endif

/* Dispatch result timeout from px_disp_async.c */
#define DISPATCH_WAIT_TIMEOUT_MSEC 2000
#define DISPATCH_WAIT_CANCEL_TIMEOUT_MSEC 100

static volatile PxDispatcherState *pxds_backgroup = NULL;
static volatile bool px_task_finished = false;
static pthread_mutex_t px_pq_mutex;
static pthread_cond_t pq_wait_cv = PTHREAD_COND_INITIALIZER;
volatile bool px_adps_dispatch_wait = true;
pg_atomic_uint32 px_adps_eno;

/* If or not the px worker' query has already finished */
static inline bool
pq_is_px_finish(PxDispatchResult *dispatchResult)
{
	return (pxconn_isBadConnection(dispatchResult->pxWorkerDesc) ||
			!dispatchResult->stillRunning);
}

/*
 * If or not the px adaptive scan thread is running.
 * Run this function in main thread
 */
bool
pxdisp_isDsThreadRuning(void)
{
	return (px_adaptive_paging && pxds_backgroup != NULL);
}

/*
 * Check whether there was error in the background thread in main thread.
 * If error found, report it.
 */
void
px_check_adps_thread_error(void)
{
	int			eno;

	eno = pg_atomic_read_u32(&px_adps_eno);
	if (eno != 0)
	{
		errno = eno;
		ereport(ERROR,
				(errcode(ERRCODE_PX_INTERCONNECTION_ERROR),
				 errmsg("Px adaptive scan thread encountered an error")));
	}
}

/*
 * Set the Px adaptive scan error no in background thread.
 * Record the error in background thread. Main thread checks the errors periodically.
 * If main thread will find it, main thread will handle it.
 */
void
px_set_adps_thread_error(int eno)
{
	uint32		expected = 0;

	/* Always let main thread know the error that occurred first. */
	if (pg_atomic_compare_exchange_u32(&px_adps_eno, &expected, (uint32) eno))
		write_log("Px adaptive scan thread starts failed in background, set eno to %d", expected);
}

/*
 * Reset the error no.
 */
void
px_reset_adps_thread_error(void)
{
	pg_atomic_write_u32(&px_adps_eno, 0);
}

/* Px adaptive scan has finished, set the status to false */
static inline void
px_result_finish(PxDispatchResult *dispatchResult)
{
	dispatchResult->stillRunning = false;
}

/*
 * Reset parameter-status in adaptive scan libpq connection.
 */
static void
clearParameterStatus(const PGconn *conn, const char *paramName)
{
	pgParameterStatus *pstatus;

	if (!conn || !paramName)
		return;
	for (pstatus = conn->pstatus; pstatus != NULL; pstatus = pstatus->next)
	{
		if (strcmp(pstatus->name, paramName) == 0)
			pstatus->value = 0;
	}
}

/*
 * Get a valid adaptive scan response and send it to px worker.
 */
static void
px_handle_adaptive_scan(PxDispatchResult *dispatchResult, const char *msg)
{
	PxWorkerDescriptor *pxWorkerDesc = dispatchResult->pxWorkerDesc;
	PGconn	   *conn = pxWorkerDesc->conn;

	if (msg)
	{
		int			msg_len;
		SeqscanPageRequest seqReq;
		SeqscanPageResponse seqRes;
		int			response_size = sizeof(SeqscanPageResponse);

		msg_len = strlen(msg);
		hex_decode(msg, msg_len, (char *) &seqReq);

		Assert(px_adaptive_paging);
		{
			/*
			 * If more than one px workers have the same ip, result always be
			 * 0
			 */
			int			node_idx = pxWorkerDesc->pxNodeInfo->cm_node_idx;
			int			node_count = pxWorkerDesc->pxNodeInfo->cm_node_size;

			seqRes = px_adps_get_response_block(&seqReq, node_idx, node_count);
		}
		if (STATUS_OK != pqPacketSend(conn, PACKET_TYPE_PAGING, &seqRes, response_size))
		{
			/* Can`t write log in thread */
			write_log("px_paging: message send to px failed");
			px_result_finish(dispatchResult);
			pxdisp_seterrcode(0, -1, dispatchResult);
		}
	}
}

/* Do the real work of px adaptive scan */
static void
px_do_real_message(PxDispatchResult *dispatchResult)
{
	PGconn	   *conn = dispatchResult->pxWorkerDesc->conn;
	const char *msg = PQparameterStatus(conn, MSG_ADAPTIVE_PAGING);

	if (msg)
	{
		px_handle_adaptive_scan(dispatchResult, msg);
		/* Clear after use */
		clearParameterStatus(conn, MSG_ADAPTIVE_PAGING);
	}
}

/*
 * Px adaptive scan thread is woken. Parse the message from px worker and
 * response the scan task to px worker.
 */
static void
px_adps_message_handle(PxDispatchResult *dispatchResult)
{
	PxWorkerDescriptor *pxWorkerDesc = dispatchResult->pxWorkerDesc;
	PGconn	   *conn = pxWorkerDesc->conn;

	if (PQconsumeInput(conn) == 0)
	{
		/* no cover begin */
		char	   *msg = PQerrorMessage(pxWorkerDesc->conn);

		pxdisp_appendMessageThread(dispatchResult, LOG,
								   "Error on receive from %s: %s",
								   pxWorkerDesc->whoami, msg ? msg : "unknown error");
		px_result_finish(dispatchResult);
		pxdisp_seterrcode(0, -1, dispatchResult);
		return;
		/* no cover end */
	}
	/* Parse one message and update status */
	PQisBusy(conn);
	if (PQstatus(conn) == CONNECTION_BAD)
	{
		/* no cover begin */
		char	   *msg = PQerrorMessage(pxWorkerDesc->conn);

		if (msg)
			write_log("pq_thread: Dispatcher encountered connection error on %s: %s", pxWorkerDesc->whoami, msg);

		write_log("pq_thread: Dispatcher noticed bad connection in handlePollError()");

		/* Save error info for later. */
		pxdisp_appendMessageThread(dispatchResult, LOG,
								   "Error after dispatch from %s: %s",
								   pxWorkerDesc->whoami,
								   msg ? msg : "unknown error");

		px_result_finish(dispatchResult);
		pxdisp_seterrcode(0, -1, dispatchResult);
		return;
		/* no cover end */
	}

	/*
	 * In query-process message, the px query is not completed. We get the msg
	 * from px to get the request slice.
	 */
	if (conn->asyncStatus == PGASYNC_BUSY)
		px_do_real_message(dispatchResult);

	/*
	 * The same as 'processResults' to deal with one or more complete
	 * messages.
	 */
	while (!PQisBusy(conn))
	{
		PGresult   *pRes;
		int			resultIndex;
		ExecStatusType resultStatus;

		/* pqResult */
		pRes = PQgetResult(pxWorkerDesc->conn);
		if (!pRes)
		{
			/* normal finish way */
			px_result_finish(dispatchResult);
			return;
		}
		/* pRes will free at pxdisp_resetResult */
		resultIndex = pxdisp_numPGresult(dispatchResult);
		pxdisp_appendResult(dispatchResult, pRes);
		resultStatus = PQresultStatus(pRes);
		if (resultStatus == PGRES_COMMAND_OK ||
			resultStatus == PGRES_TUPLES_OK ||
			resultStatus == PGRES_COPY_IN ||
			resultStatus == PGRES_COPY_OUT ||
			resultStatus == PGRES_EMPTY_QUERY)
		{
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
			{
				/* normal finish way */
				px_result_finish(dispatchResult);
			}
		}
		else
		{
			/* PX reported an error */
			char	   *sqlstate = PQresultErrorField(pRes, PG_DIAG_SQLSTATE);
			int			errcode = 0;
			char	   *msg;

			msg = PQresultErrorMessage(pRes);
			write_log("pq_thread: %s -> %s %s  %s", pxWorkerDesc->whoami,
					  PQresStatus(resultStatus), sqlstate ? sqlstate : "(no SQLSTATE)", msg);

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
}

/*
 * Deal with poll events error. Clean the px dynamic states.
 */
static void
pq_thread_handle_error(PxDispatchCmdAsync *pParms)
{
	int			i;

	/* no cover begin */
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
		if (PQstatus(pxWorkerDesc->conn) == CONNECTION_BAD)
		{
			char	   *msg = PQerrorMessage(pxWorkerDesc->conn);

			if (msg)
				write_log("pq_thread: Dispatcher encountered connection error on %s: %s", pxWorkerDesc->whoami, msg);

			write_log("pq_thread: Dispatcher noticed bad connection in handlePollError()");

			/* Save error info for later. */
			pxdisp_appendMessageThread(dispatchResult, LOG,
									   "Error after dispatch from %s: %s",
									   pxWorkerDesc->whoami,
									   msg ? msg : "unknown error");

			px_result_finish(dispatchResult);
			pxdisp_seterrcode(0, -1, dispatchResult);
		}
	}
	/* no cover end */
}

static void
pq_thread_signalPXs(PxDispatchCmdAsync *pParms)
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
			write_log("pq_thread: Unable to cancel: %s", strlen(errbuf) == 0 ? "cannot allocate PGCancel" : errbuf);
	}
}

static void
pq_flush_buffer(PxDispatchResult *dispatchResult)
{
	int			ret;
	PGconn	   *conn;
	PxWorkerDescriptor *pxWorkerDesc;

	pxWorkerDesc = dispatchResult->pxWorkerDesc;
	conn = pxWorkerDesc->conn;
	/* skip already completed connections */
	if (conn->outCount == 0)
		return;

	/*
	 * call send for this connection regardless of its POLLOUT status, because
	 * it may be writable NOW
	 */
	write_log("pq_thread: flush libpq buffer");
	ret = pqFlushNonBlocking(conn);

	if (ret == 0)
		return;
	else if (ret < 0)
	{
		char	   *msg;

		/* error message should be set up already */
		msg = PQerrorMessage(conn);

		px_result_finish(dispatchResult);

		pxdisp_appendMessageThread(dispatchResult, ERROR,
								   "Command could not flush to segment %s: %s",
								   pxWorkerDesc->whoami,
								   msg ? msg : "unknown error");
		pxdisp_seterrcode(0, -1, dispatchResult);
	}
}

/*
 * Function in thread. can not use palloc/pfree.
 * The main thread for adaptive scan in QC. Poll from px workers and response
 * if it is necessary.
 */
static void
px_consumer_ds(volatile PxDispatcherState *ds)
{
	/* Receive from px */
	struct pollfd *fds = NULL;
	PxDispatchCmdAsync *pParms = (PxDispatchCmdAsync *) ds->dispatchParams;
	PxDispatchResults *meleeResults = ds->primaryResults;
	int			px_workers_count = pParms->dispatchCount;
	int			nfds = 0;
	int			i,
				n;
	int			timeout = 0;
	bool		sentSignal = false;
	int			px_finished_tasks_count = 0;

	/*----------
	 * Escalate waitMode to cancel if:
	 *  - user interrupt has occurred,
	 *  - or an error has been reported by any PX,
	 *  - in case the caller wants cancelOnError
	 *----------
	 */
	if ((InterruptPending || meleeResults->errcode) && meleeResults->cancelOnError)
		pParms->waitMode = DISPATCH_WAIT_CANCEL;

	fds = (struct pollfd *) malloc(px_workers_count * sizeof(struct pollfd));
	if (fds == NULL)
		goto finish;
	for (i = 0; i < px_workers_count; i++)
	{
		int			sock;
		PGconn	   *conn;
		PxDispatchResult *dispatchResult;
		PxWorkerDescriptor *pxWorkerDesc;

		dispatchResult = pParms->dispatchResultPtrArray[i];
		pxWorkerDesc = dispatchResult->pxWorkerDesc;
		conn = pxWorkerDesc->conn;

		if (pq_is_px_finish(dispatchResult))
		{
			px_finished_tasks_count++;
			continue;
		}

		px_task_finished = false;

		/* Flush outBuff */
		pq_flush_buffer(dispatchResult);

		/*
		 * Add socket to fd_set if still connected.
		 */
		sock = PQsocket(conn);
		Assert(sock >= 0);
		fds[nfds].fd = sock;
		fds[nfds].events = POLLIN;
		nfds++;
	}
	if (nfds <= 0)
	{
		/*
		 * If the px finished tasks number equal to px workers number, means
		 * one px task has finished.
		 */
		if (px_finished_tasks_count == px_workers_count)
			px_task_finished = true;

		/* Prevent deal loop */
		pg_usleep(1);
		goto finish;
	}
poll:
	if (!px_adps_dispatch_wait)
	{
		timeout = 0;
		px_adps_dispatch_wait = true;
	}
	else if (pParms->waitMode == DISPATCH_WAIT_NONE || sentSignal)
		timeout = DISPATCH_WAIT_TIMEOUT_MSEC;
	else
		timeout = DISPATCH_WAIT_CANCEL_TIMEOUT_MSEC;

	n = poll(fds, nfds, timeout);

	/*
	 * Poll returns with an error, including one due to an interrupted call
	 */
	if (n < 0)
	{
		int			sock_errno = SOCK_ERRNO;

		if (sock_errno == EINTR)
			goto poll;
		/* no cover begin */
		write_log("pq_thread: handlePollError poll() failed; errno=%d", sock_errno);

		pq_thread_handle_error(pParms);

		if (pParms->waitMode != DISPATCH_WAIT_NONE)
		{
			write_log("pq_thread: signal to cancel all px");
			pq_thread_signalPXs(pParms);
			sentSignal = true;
		}
		/* no cover end */
	}
	/* If the time limit expires, poll() returns 0 */
	else if (n == 0)
	{
		if (pParms->waitMode != DISPATCH_WAIT_NONE)
		{
			write_log("pq_thread: signal to cancel all px");
			pq_thread_signalPXs(pParms);
			sentSignal = true;
		}
	}
	/* We have data waiting on one or more of the connections. */
	else
	{
		int			currentFdNumber = 0;

		for (i = 0; i < px_workers_count; i++)
		{
			PxDispatchResult *dispatchResult;

			dispatchResult = pParms->dispatchResultPtrArray[i];
			if (pq_is_px_finish(dispatchResult))
				continue;
			if (!(fds[currentFdNumber++].revents & POLLIN))
				continue;
			px_adps_message_handle(dispatchResult);
		}
	}

finish:
	free(fds);
}

/*
 * PX adaptive scan background thread. It will start in the QC node when
 * px query is running.
 */
static void *
px_adps_coordinator_thread(void *arg)
{
	char		px_adps_thread_name[20];

	sprintf(px_adps_thread_name, "pxadps%d", getpid());
	prctl(PR_SET_NAME, px_adps_thread_name);
	for (;;)
	{
		if (proc_exit_inprogress)
		{
			write_log("pq_thread: break up px_pq thread when exit process");
			break;
		}
		pthread_mutex_lock(&px_pq_mutex);
		if (pxds_backgroup == NULL)
		{
			/* Prevent spurious wake up */
			while (pxds_backgroup == NULL)
			{
				/* Wait will unlock automatic */
				pthread_cond_wait(&pq_wait_cv, &px_pq_mutex);
				/* Leave wait will lock automatic */
			}
			pthread_mutex_unlock(&px_pq_mutex);

			/* Check dynamic state when px query begin */
			if (px_adps_check_valid() == false)
			{
				/* In debug mode we want to get a panic result */
				Assert(false);
				write_log("px adaptive scan state is invalid");
				px_set_adps_thread_error(ENOMEM);
			}
		}
		else
		{
			px_consumer_ds(pxds_backgroup);
			pthread_mutex_unlock(&px_pq_mutex);

			/* sleep for schedule yield */
			if (px_task_finished)
				pg_usleep(1);
		}
	}
	return NULL;
}

/* Create pq thread when PX_SETUP, no matter px_adaptive_paging */
void
pxdisp_createPqThread()
{
	int			pthread_err;
	pthread_t	t_handle;
	pthread_attr_t t_atts;
	pthread_mutexattr_t m_atts;

	/* If guc flag is closed */
	if (!px_adaptive_paging)
		return;

	px_adaptive_scan_setup = true;

	/* init pthread */
	pthread_attr_init(&t_atts);
	/* init mutex */
	pthread_mutexattr_init(&m_atts);
	pthread_mutexattr_settype(&m_atts, PTHREAD_MUTEX_ERRORCHECK);
	pthread_mutex_init(&px_pq_mutex, &m_atts);
	pg_atomic_init_u32(&px_adps_eno, 0);

	elog(DEBUG5, "pq_thread: creating the background libpq thread");
	pthread_attr_setstacksize(&t_atts, Max(PTHREAD_STACK_MIN, (128 * 1024)));
	pthread_err = pthread_create(&t_handle, &t_atts, px_adps_coordinator_thread, NULL);
	pthread_attr_destroy(&t_atts);

	if (pthread_err != 0)
	{
		pxds_backgroup = NULL;
		pthread_cond_destroy(&pq_wait_cv);
		pthread_mutex_destroy(&px_pq_mutex);
		ereport(FATAL,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("failed to create libpq thread"),
				 errdetail("pthread_create() failed with err %d", pthread_err)));
	}
}

/* Run this function in main thread */
void
pxdisp_startPqThread(PxDispatcherState *ds)
{
	/* If guc flag is closed */
	if (!px_adaptive_paging)
		return;
	Assert(pxds_backgroup == NULL || px_task_finished == true);
	elog(DEBUG5, "pq_thread: starting the background libpq thread");
	/* update the ds */
	pthread_mutex_lock(&px_pq_mutex);
	if (pxds_backgroup != NULL)
		px_adps_array_free();
	pxds_backgroup = ds;
	pthread_cond_signal(&pq_wait_cv);
	pthread_mutex_unlock(&px_pq_mutex);
}

/* Run this function in main thread */
void
pxdisp_finishPqThread(void)
{
	if (!px_adaptive_paging)
		return;
	pthread_mutex_lock(&px_pq_mutex);
	pxds_backgroup = NULL;
	px_task_finished = true;
	px_adps_array_free();
	pthread_mutex_unlock(&px_pq_mutex);
	elog(DEBUG5, "pq_thread: stopping the background libpq thread");
}
