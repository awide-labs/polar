/*-------------------------------------------------------------------------
 *
 * px_gang_async.c
 *	  Functions for asynchronous implementation of creating gang.
 *
 * Portions Copyright (c) 2005-2008, Greenplum inc
 * Portions Copyright (c) 2012-Present Pivotal Software, Inc.
 * Portions Copyright (c) 2021, Alibaba Group Holding Limited
 *
 *
 * IDENTIFICATION
 *	    src/backend/px/dispatcher/px_gang_async.c
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

#include "libpq-fe.h"
#include "libpq-int.h"
#include "miscadmin.h"
#include "storage/ipc.h"		/* For proc_exit_inprogress  */
#include "tcop/tcopprot.h"

#include "px/px_conn.h"
#include "px/px_gang.h"
#include "px/px_gang_async.h"
#include "px/px_vars.h"
#include "px/px_snapshot.h"
#include "utils/faultinjector.h"

static int	getPollTimeout(const struct timeval *startTS);

static inline bool
failure_on_decryption(const char *errmsg)
{
	const char	FAILED_TO_DECRYPT_MSG[] = "failed to decrypt message";

	return strstr(errmsg, FAILED_TO_DECRYPT_MSG) != NULL;
}

/*
 * Creates a new gang by logging on a session to each segDB involved.
 *
 * call this function in GangContext memory context.
 * elog ERROR or return a non-NULL gang.
 */
Gang *
pxgang_createGang_async(List *segments, SegmentType segmentType)
{
	PostgresPollingStatusType *pollingStatus = NULL;
	PxWorkerDescriptor *pxWorkerDesc = NULL;
	struct timeval startTS;
	Gang	   *newGangDefinition;
	int			create_gang_retry_counter = 0;
	int			in_recovery_mode_count = 0;
	int			successful_connections = 0;
	int			poll_timeout = 0;
	int			i = 0;
	int			size = 0;
	bool		retry = false;
	int			totalPxNodes = 0;

	struct pollfd *fds;

	/*
	 * true means connection status is confirmed, either established or in
	 * recovery mode
	 */
	bool	   *connStatusDone = NULL;

	size = list_length(segments);

	ELOG_DISPATCHER_DEBUG("createGang size = %d, segment type = %d", size, segmentType);

	Assert(CurrentGangCreating == NULL);

	/* If we're in a retry, we may need to reset our initial state, a bit */
	newGangDefinition = NULL;
	/* allocate and initialize a gang structure */
	newGangDefinition = buildGangDefinition(segments, segmentType);
	CurrentGangCreating = newGangDefinition;
	totalPxNodes = getPxWorkerCount();
	Assert(totalPxNodes > 0);

create_gang_retry:
	Assert(newGangDefinition != NULL);
	Assert(newGangDefinition->size == size);
	successful_connections = 0;
	in_recovery_mode_count = 0;
	retry = false;

	/*
	 * allocate memory within perGangContext and will be freed automatically
	 * when gang is destroyed
	 */
	pollingStatus = palloc(sizeof(PostgresPollingStatusType) * size);
	connStatusDone = palloc(sizeof(bool) * size);

	PG_TRY();
	{
		int			pxid_buf_len = 1000;
		char	   *pxid = palloc(pxid_buf_len);

		for (i = 0; i < size; i++)
		{
			bool		ret;
			char	   *options;

			/*
			 * Create the connection requests.	If we find a segment without a
			 * valid segdb we error out.  Also, if this segdb is invalid, we
			 * must fail the connection.
			 */
			pxWorkerDesc = newGangDefinition->db_descriptors[i];

			/* if it's a cached PX, skip */
			if (pxWorkerDesc->conn != NULL && !pxconn_isBadConnection(pxWorkerDesc))
			{
				connStatusDone[i] = true;
				successful_connections++;
				continue;
			}

			/*
			 * Build the connection string.  Writer-ness needs to be processed
			 * early enough now some locks are taken before command line
			 * options are recognized.
			 */
			ret = build_pxid_param(pxid, pxid_buf_len,
								   pxWorkerDesc->identifier,
								   totalPxNodes * 5);

			if (!ret)
				ereport(ERROR,
						(errcode(ERRCODE_PX_INTERCONNECTION_ERROR),
						 errmsg("failed to construct connectionstring")));

			options = makeOptions();

			/* start connection in asynchronous way */
			pxconn_doConnectStart(pxWorkerDesc, pxid, options, segmentType);

			if (pxconn_isBadConnection(pxWorkerDesc))
				ereport(ERROR, (errcode(ERRCODE_PX_INTERCONNECTION_ERROR),
								errmsg("failed to acquire resources on one or more segments"),
								errdetail("%s (%s)", PQerrorMessage(pxWorkerDesc->conn), pxWorkerDesc->whoami)));

			connStatusDone[i] = false;

			/*
			 * If connection status is not CONNECTION_BAD after
			 * PQconnectStart(), we must act as if the PQconnectPoll() had
			 * returned PGRES_POLLING_WRITING
			 */
			pollingStatus[i] = PGRES_POLLING_WRITING;
		}
		pfree(pxid);

		/*
		 * Ok, we've now launched all the connection attempts. Start the
		 * timeout clock (= get the start timestamp), and poll until they're
		 * all completed or we reach timeout.
		 */
		gettimeofday(&startTS, NULL);
		fds = (struct pollfd *) palloc0(sizeof(struct pollfd) * size);

		for (;;)
		{
			int			nready;
			int			nfds = 0;

			poll_timeout = getPollTimeout(&startTS);

			for (i = 0; i < size; i++)
			{
				pxWorkerDesc = newGangDefinition->db_descriptors[i];

				/*
				 * Skip established connections and in-recovery-mode
				 * connections
				 */
				if (connStatusDone[i])
				{
					if (pxWorkerDesc->serialized_snap)
						pxsn_set_oldest_snapshot(RestoreSnapshot(pxWorkerDesc->serialized_snap));
					continue;
				}

#ifdef FAULT_INJECTOR
				if (SIMPLE_FAULT_INJECTOR("pgres_polling_failed") == FaultInjectorTypeEnable)
					goto FAULT_INJECTOR_POLLING_FAILED;
#endif

				switch (pollingStatus[i])
				{
					case PGRES_POLLING_OK:
						pxconn_doConnectComplete(pxWorkerDesc);
						if (pxWorkerDesc->motionListener == 0)
							ereport(ERROR, (errcode(ERRCODE_PX_INTERCONNECTION_ERROR),
											errmsg("failed to acquire resources on one or more segments"),
											errdetail("Internal error: No motion listener port (%s)", pxWorkerDesc->whoami)));
						successful_connections++;
						connStatusDone[i] = true;

						if (pxWorkerDesc->serialized_snap)
							pxsn_set_oldest_snapshot(RestoreSnapshot(pxWorkerDesc->serialized_snap));

						continue;

					case PGRES_POLLING_READING:
						fds[nfds].fd = PQsocket(pxWorkerDesc->conn);
						fds[nfds].events = POLLIN;
						nfds++;
						break;

					case PGRES_POLLING_WRITING:
						fds[nfds].fd = PQsocket(pxWorkerDesc->conn);
						fds[nfds].events = POLLOUT;
						nfds++;
						break;

					case PGRES_POLLING_FAILED:
#ifdef FAULT_INJECTOR
				FAULT_INJECTOR_POLLING_FAILED:
#endif
						if (segment_failure_due_to_recovery(PQerrorMessage(pxWorkerDesc->conn)))
						{
							in_recovery_mode_count++;
							connStatusDone[i] = true;
							elog(LOG, "segment is in recovery mode (%s)", pxWorkerDesc->whoami);
						}
						else
						{
							if (failure_on_decryption(PQerrorMessage(pxWorkerDesc->conn)))
								pqDropConnection(pxWorkerDesc->conn, true);
							ereport(ERROR, (errcode(ERRCODE_PX_INTERCONNECTION_ERROR),
											errmsg("failed to acquire resources on one or more segments"),
											errdetail("%s (%s)", PQerrorMessage(pxWorkerDesc->conn), pxWorkerDesc->whoami)));
						}
						break;

					default:
						ereport(ERROR, (errcode(ERRCODE_PX_INTERCONNECTION_ERROR),
										errmsg("failed to acquire resources on one or more segments"),
										errdetail("unknow pollstatus (%s)", pxWorkerDesc->whoami)));
						break;
				}

				if (poll_timeout == 0)
					ereport(ERROR, (errcode(ERRCODE_PX_INTERCONNECTION_ERROR),
									errmsg("failed to acquire resources on one or more segments"),
									errdetail("timeout expired\n (%s)", pxWorkerDesc->whoami)));
			}

			if (nfds == 0)
				break;

			CHECK_FOR_INTERRUPTS();

			/* Wait until something happens */
			nready = poll(fds, nfds, poll_timeout);

			if (nready < 0)
			{
				int			sock_errno = SOCK_ERRNO;

				if (sock_errno == EINTR)
					continue;

				ereport(ERROR, (errcode(ERRCODE_PX_INTERCONNECTION_ERROR),
								errmsg("failed to acquire resources on one or more segments"),
								errdetail("poll() failed: errno = %d", sock_errno)));
			}
			else if (nready > 0)
			{
				int			currentFdNumber = 0;

				for (i = 0; i < size; i++)
				{
					pxWorkerDesc = newGangDefinition->db_descriptors[i];
					if (connStatusDone[i])
						continue;

					Assert(PQsocket(pxWorkerDesc->conn) > 0);
					Assert(PQsocket(pxWorkerDesc->conn) == fds[currentFdNumber].fd);

					if (fds[currentFdNumber].revents & fds[currentFdNumber].events ||
						fds[currentFdNumber].revents & (POLLERR | POLLHUP | POLLNVAL))
						pollingStatus[i] = PQconnectPoll(pxWorkerDesc->conn);

					currentFdNumber++;

				}
			}
		}

		ELOG_DISPATCHER_DEBUG("createGang: %d processes requested; %d successful connections %d in recovery",
							  size, successful_connections, in_recovery_mode_count);

		/* some segments are in recovery mode */
		if (successful_connections != size)
		{
			Assert(successful_connections + in_recovery_mode_count == size);

			if (px_gang_creation_retry_count <= 0 ||
				create_gang_retry_counter++ >= px_gang_creation_retry_count)
				ereport(ERROR, (errcode(ERRCODE_PX_INTERCONNECTION_ERROR),
								errmsg("failed to acquire resources on one or more segments"),
								errdetail("Segments are in recovery mode.")));

			ELOG_DISPATCHER_DEBUG("createGang: gang creation failed, but retryable.");

			retry = true;
		}
	}
	PG_CATCH();
	{
		PG_RE_THROW();
	}
	PG_END_TRY();

	if (retry)
	{
		CHECK_FOR_INTERRUPTS();
		pg_usleep(px_gang_creation_retry_timer * 1000);
		CHECK_FOR_INTERRUPTS();

		goto create_gang_retry;
	}

	CurrentGangCreating = NULL;

	return newGangDefinition;
}

static int
getPollTimeout(const struct timeval *startTS)
{
	struct timeval now;
	int			timeout = 0;
	int64		diff_us;

	gettimeofday(&now, NULL);

	if (px_worker_connect_timeout > 0)
	{
		diff_us = (now.tv_sec - startTS->tv_sec) * 1000000;
		diff_us += (int) now.tv_usec - (int) startTS->tv_usec;
		if (diff_us >= (int64) px_worker_connect_timeout * 1000000)
			timeout = 0;
		else
			timeout = px_worker_connect_timeout * 1000 - diff_us / 1000;
	}
	else
		/* wait forever */
		timeout = -1;

	return timeout;
}
