/*-------------------------------------------------------------------------
 *
 * walreceiver.c
 *
 * The WAL receiver process (walreceiver) is new as of Postgres 9.0. It
 * is the process in the standby server that takes charge of receiving
 * XLOG records from a primary server during streaming replication.
 *
 * When the startup process determines that it's time to start streaming,
 * it instructs postmaster to start walreceiver. Walreceiver first connects
 * to the primary server (it will be served by a walsender process
 * in the primary server), and then keeps receiving XLOG records and
 * writing them to the disk as long as the connection is alive. As XLOG
 * records are received and flushed to disk, it updates the
 * WalRcv->flushedUpto variable in shared memory, to inform the startup
 * process of how far it can proceed with XLOG replay.
 *
 * A WAL receiver cannot directly load GUC parameters used when establishing
 * its connection to the primary. Instead it relies on parameter values
 * that are passed down by the startup process when streaming is requested.
 * This applies, for example, to the replication slot and the connection
 * string to be used for the connection with the primary.
 *
 * If the primary server ends streaming, but doesn't disconnect, walreceiver
 * goes into "waiting" mode, and waits for the startup process to give new
 * instructions. The startup process will treat that the same as
 * disconnection, and will rescan the archive/pg_wal directory. But when the
 * startup process wants to try streaming replication again, it will just
 * nudge the existing walreceiver process that's waiting, instead of launching
 * a new one.
 *
 * Normal termination is by SIGTERM, which instructs the walreceiver to
 * exit(0). Emergency termination is by SIGQUIT; like any postmaster child
 * process, the walreceiver will simply abort and exit on SIGQUIT. A close
 * of the connection and a FATAL error are treated not as a crash but as
 * normal operation.
 *
 * This file contains the server-facing parts of walreceiver. The libpq-
 * specific parts are in the libpqwalreceiver module. It's loaded
 * dynamically to avoid linking the server with libpq.
 *
 * Portions Copyright (c) 2024, Alibaba Group Holding Limited
 * Portions Copyright (c) 2010-2022, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/replication/walreceiver.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>

#include "access/htup_details.h"
#include "access/timeline.h"
#include "access/transam.h"
#include "access/xlog_internal.h"
#include "access/xlogarchive.h"
#include "access/xlogrecovery.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_type.h"
#include "common/ip.h"
#include "funcapi.h"
#include "libpq/pqformat.h"
#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "polar_datamax/polar_datamax.h"
#include "postmaster/interrupt.h"
#include "replication/walreceiver.h"
#include "replication/walsender.h"
#include "replication/walsender_private.h"
#include "storage/ipc.h"
#include "storage/pmsignal.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "storage/procsignal.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/pg_lsn.h"
#include "utils/ps_status.h"
#include "utils/resowner.h"
#include "utils/timestamp.h"
#include "utils/timeout.h"

/* POLAR */
#include "access/polar_logindex_redo.h"
#include "postmaster/polar_async_lock_replay.h"
#include "storage/polar_fd.h"


/*
 * GUC variables.  (Other variables that affect walreceiver are in xlog.c
 * because they're passed down from the startup process, for better
 * synchronization.)
 */
int			wal_receiver_status_interval;
int			wal_receiver_timeout;
bool		hot_standby_feedback;

/* libpqwalreceiver connection */
static WalReceiverConn *wrconn = NULL;
WalReceiverFunctionsType *WalReceiverFunctions = NULL;

#define NAPTIME_PER_CYCLE 100	/* max sleep time between cycles (100ms) */

/* POLAR: timeout (ms) for re-sending a promote request to the walsender */
#define POLAR_SEND_PROMOTE_REQUEST_TIMEOUT	500

/*
 * These variables are used similarly to openLogFile/SegNo,
 * but for walreceiver to write the XLOG. recvFileTLI is the TimeLineID
 * corresponding the filename of recvFile.
 */
static int	recvFile = -1;
static TimeLineID recvFileTLI = 0;
static XLogSegNo recvSegNo = 0;

/*
 * POLAR: set true between IDENTIFY_SYSTEM and the first received WAL record
 * when an initial datamax node negotiated its streaming timeline from the
 * primary; used to seed the datamax meta's min received info once.
 */
static bool polar_is_initial_datamax = false;

/*
 * LogstreamResult indicates the byte positions that we have already
 * written/fsynced.
 */
static struct
{
	XLogRecPtr	Write;			/* last byte + 1 written out in the standby */
	XLogRecPtr	Flush;			/* last byte + 1 flushed in the standby */
}			LogstreamResult;

static StringInfoData reply_message;
static StringInfoData incoming_message;

/*
 * POLAR: in datamax mode, records the primary's last valid LSN reported
 * with each WAL message so the datamax can advance its own last valid
 * received LSN only up to a record boundary the primary has confirmed.
 */
static polar_datamax_valid_lsn_list *polar_datamax_received_valid_lsn_list = NULL;

/* Prototypes for private functions */
static void WalRcvFetchTimeLineHistoryFiles(TimeLineID first, TimeLineID last);
static void WalRcvWaitForStartPosition(XLogRecPtr *startpoint, TimeLineID *startpointTLI);
static void WalRcvDie(int code, Datum arg);
static void XLogWalRcvProcessMsg(unsigned char type, char *buf, Size len,
								 TimeLineID tli);
static void XLogWalRcvWrite(char *buf, Size nbytes, XLogRecPtr recptr,
							TimeLineID tli);
static void XLogWalRcvFlush(bool dying, TimeLineID tli);
static void XLogWalRcvClose(XLogRecPtr recptr, TimeLineID tli);
static void XLogWalRcvSendReply(bool force, bool requestReply);
static void XLogWalRcvSendHSFeedback(bool immed);
static void ProcessWalSndrMessage(XLogRecPtr walEnd, TimestampTz sendTime);

/* POLAR: callback function when waiting free space from polar_xlog_queue */
static void polar_receiver_xlog_queue_callback(polar_ringbuf_t rbuf);
static void polar_recv_push_storage_begin_callback(polar_ringbuf_t rbuf);
static void polar_notify_read_wal_file(int code, Datum arg);

/* POLAR end */

/*
 * Process any interrupts the walreceiver process may have received.
 * This should be called any time the process's latch has become set.
 *
 * Currently, only SIGTERM is of interest.  We can't just exit(1) within the
 * SIGTERM signal handler, because the signal might arrive in the middle of
 * some critical operation, like while we're holding a spinlock.  Instead, the
 * signal handler sets a flag variable as well as setting the process's latch.
 * We must check the flag (by calling ProcessWalRcvInterrupts) anytime the
 * latch has become set.  Operations that could block for a long time, such as
 * reading from a remote server, must pay attention to the latch too; see
 * libpqrcv_PQgetResult for example.
 */
void
ProcessWalRcvInterrupts(void)
{
	/*
	 * Although walreceiver interrupt handling doesn't use the same scheme as
	 * regular backends, call CHECK_FOR_INTERRUPTS() to make sure we receive
	 * any incoming signals on Win32, and also to make sure we process any
	 * barrier events.
	 */
	CHECK_FOR_INTERRUPTS();

	if (ShutdownRequestPending)
	{
		ereport(FATAL,
				(errcode(ERRCODE_ADMIN_SHUTDOWN),
				 errmsg("terminating walreceiver process due to administrator command")));
	}
}


/* Main entry point for walreceiver process */
void
WalReceiverMain(void)
{
	char		conninfo[MAXCONNINFO];
	char	   *tmp_conninfo;
	char		slotname[NAMEDATALEN];
	bool		is_temp_slot;
	XLogRecPtr	startpoint;
	TimeLineID	startpointTLI;
	TimeLineID	primaryTLI;
	bool		first_stream;
	WalRcvData *walrcv = WalRcv;
	TimestampTz last_recv_timestamp;
	TimestampTz now;
	bool		ping_sent;
	char	   *err;
	char	   *sender_host = NULL;
	int			sender_port = 0;

	/*
	 * WalRcv should be set up already (if we are a backend, we inherit this
	 * by fork() or EXEC_BACKEND mechanism from the postmaster).
	 */
	Assert(walrcv != NULL);

	now = GetCurrentTimestamp();

	/*
	 * Mark walreceiver as running in shared memory.
	 *
	 * Do this as early as possible, so that if we fail later on, we'll set
	 * state to STOPPED. If we die before this, the startup process will keep
	 * waiting for us to start up, until it times out.
	 */
	SpinLockAcquire(&walrcv->mutex);
	Assert(walrcv->pid == 0);
	switch (walrcv->walRcvState)
	{
		case WALRCV_STOPPING:
			/* If we've already been requested to stop, don't start up. */
			walrcv->walRcvState = WALRCV_STOPPED;
			/* fall through */

		case WALRCV_STOPPED:
			SpinLockRelease(&walrcv->mutex);
			ConditionVariableBroadcast(&walrcv->walRcvStoppedCV);
			proc_exit(1);
			break;

		case WALRCV_STARTING:
			/* The usual case */
			break;

		case WALRCV_WAITING:
		case WALRCV_STREAMING:
		case WALRCV_RESTARTING:
		default:
			/* Shouldn't happen */
			SpinLockRelease(&walrcv->mutex);
			elog(PANIC, "walreceiver still running according to shared memory state");
	}
	/* Advertise our PID so that the startup process can kill us */
	walrcv->pid = MyProcPid;
	walrcv->walRcvState = WALRCV_STREAMING;

	/* Fetch information required to start streaming */
	walrcv->ready_to_display = false;
	strlcpy(conninfo, (char *) walrcv->conninfo, MAXCONNINFO);
	strlcpy(slotname, (char *) walrcv->slotname, NAMEDATALEN);
	is_temp_slot = walrcv->is_temp_slot;
	startpoint = walrcv->receiveStart;
	startpointTLI = walrcv->receiveStartTLI;

	/*
	 * At most one of is_temp_slot and slotname can be set; otherwise,
	 * RequestXLogStreaming messed up.
	 */
	Assert(!is_temp_slot || (slotname[0] == '\0'));

	/* Initialise to a sanish value */
	walrcv->lastMsgSendTime =
		walrcv->lastMsgReceiptTime = walrcv->latestWalEndTime = now;

	/* Report the latch to use to awaken this process */
	walrcv->latch = &MyProc->procLatch;

	/*
	 * POLAR: Reset consistent lsn received from primary node while starting
	 * up walreceiver.
	 */
	pg_atomic_write_u64(&WalRcv->curr_primary_consistent_lsn, InvalidXLogRecPtr);

	SpinLockRelease(&walrcv->mutex);

	pg_atomic_write_u64(&WalRcv->writtenUpto, 0);

	/*
	 * POLAR: decide once, for the whole life of this walreceiver, whether WAL
	 * path resolution should route through POLAR_DATAMAX_WAL_DIR. On a
	 * datamax node every segment this process writes lives there; on any
	 * other node it stays in the regular pg_wal. Node type is fixed at
	 * startup, so set the flag here and never toggle it per-operation.
	 */
	polar_is_datamax_mode = polar_is_datamax();

	/* POLAR:notify startup to read wal file instead of logindex queue */
	before_shmem_exit(polar_notify_read_wal_file, 0);

	/* Arrange to clean up at walreceiver exit */
	on_shmem_exit(WalRcvDie, PointerGetDatum(&startpointTLI));

	/* Properly accept or ignore signals the postmaster might send us */
	pqsignal(SIGHUP, SignalHandlerForConfigReload); /* set flag to read config
													 * file */
	pqsignal(SIGINT, SIG_IGN);
	pqsignal(SIGTERM, SignalHandlerForShutdownRequest); /* request shutdown */
	/* SIGQUIT handler was already set up by InitPostmasterChild */
	pqsignal(SIGALRM, SIG_IGN);
	pqsignal(SIGPIPE, SIG_IGN);
	pqsignal(SIGUSR1, procsignal_sigusr1_handler);
	pqsignal(SIGUSR2, SIG_IGN);

	/* Reset some signals that are accepted by postmaster but not here */
	pqsignal(SIGCHLD, SIG_DFL);

	/*
	 * POLAR: Establishes SIGALRM handler and initialize parameters to
	 * facilitate the running of scheduled tasks. Some scheduled tasks will
	 * cause assertion errors when parameters are not initialized.
	 */
	InitializeTimeouts();

	/* Load the libpq-specific functions */
	load_file("libpqwalreceiver", false);
	if (WalReceiverFunctions == NULL)
		elog(ERROR, "libpqwalreceiver didn't initialize correctly");

	/* Unblock signals (they were blocked when the postmaster forked us) */
	PG_SETMASK(&UnBlockSig);

	/* Establish the connection to the primary for XLOG streaming */
	wrconn = walrcv_connect(conninfo, false,
							cluster_name[0] ? cluster_name : "walreceiver",
							&err);
	if (!wrconn)
		ereport(ERROR,
				(errcode(ERRCODE_CONNECTION_FAILURE),
				 errmsg("could not connect to the primary server: %s", err)));

	/*
	 * Save user-visible connection string.  This clobbers the original
	 * conninfo, for security. Also save host and port of the sender server
	 * this walreceiver is connected to.
	 */
	tmp_conninfo = walrcv_get_conninfo(wrconn);
	walrcv_get_senderinfo(wrconn, &sender_host, &sender_port);
	SpinLockAcquire(&walrcv->mutex);
	memset(walrcv->conninfo, 0, MAXCONNINFO);
	if (tmp_conninfo)
		strlcpy((char *) walrcv->conninfo, tmp_conninfo, MAXCONNINFO);

	memset(walrcv->sender_host, 0, NI_MAXHOST);
	if (sender_host)
		strlcpy((char *) walrcv->sender_host, sender_host, NI_MAXHOST);

	walrcv->sender_port = sender_port;
	walrcv->ready_to_display = true;
	SpinLockRelease(&walrcv->mutex);

	if (tmp_conninfo)
		pfree(tmp_conninfo);

	if (sender_host)
		pfree(sender_host);

	first_stream = true;

	/* POLAR: create and init the valid-LSN list when in datamax mode */
	if (polar_is_datamax())
	{
		polar_datamax_received_valid_lsn_list = polar_datamax_create_valid_lsn_list();

		/*
		 * POLAR: register the cleanup handler only after the list exists, so
		 * before_shmem_exit() captures the real pointer instead of NULL.
		 */
		before_shmem_exit(polar_datamax_free_valid_lsn_list,
						  PointerGetDatum(polar_datamax_received_valid_lsn_list));
	}

	for (;;)
	{
		char	   *primary_sysid;
		char		standby_sysid[32];
		WalRcvStreamOptions options;
		bool		polar_replica = false;

		/* POLAR: enable replica stream replication */
		if (polar_is_replica())
			polar_replica = true;

		/* POLAR: force log */
		if (polar_replica)
			ereport(LOG, (errmsg("Start wal receiver on PolarDB")));

		/*
		 * Check that we're connected to a valid server using the
		 * IDENTIFY_SYSTEM replication command.
		 */
		primary_sysid = walrcv_identify_system(wrconn, &primaryTLI);

		snprintf(standby_sysid, sizeof(standby_sysid), UINT64_FORMAT,
				 GetSystemIdentifier());
		if (strcmp(primary_sysid, standby_sysid) != 0)
		{
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("database system identifier differs between the primary and standby"),
					 errdetail("The primary's identifier is %s, the standby's identifier is %s.",
							   primary_sysid, standby_sysid)));
		}

		/* POLAR: update datamax timeline if current is an initial one */
		if (polar_is_datamax() && polar_datamax_is_initial(polar_datamax_ctl))
		{
			Assert(startpointTLI == POLAR_INVALID_TIMELINE_ID);

			polar_is_initial_datamax = true;

			/*
			 * POLAR: when a datamax cascades from another datamax, the
			 * upstream may itself be an initial datamax that has not yet
			 * learned its timeline from its own primary, so IDENTIFY_SYSTEM
			 * reports timeline 0. Adopting that invalid timeline would make
			 * the TIMELINE_HISTORY request below fail with "invalid timeline
			 * 0" and take the walreceiver down. Bail out instead and let the
			 * startup process reconnect after wal_retrieve_retry_interval, by
			 * which time the upstream should have advanced to a valid
			 * timeline.
			 */
			if (primaryTLI == POLAR_INVALID_TIMELINE_ID)
				ereport(ERROR,
						(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						 errmsg("primary server has not established a timeline yet"),
						 errdetail("The upstream datamax node reported timeline 0; will retry.")));

			/*
			 * POLAR: an initial datamax requested streaming with an invalid
			 * timeline / lsn, so fetch as much WAL as possible from the
			 * primary's current timeline: adopt the primary's timeline that
			 * IDENTIFY_SYSTEM just reported. The start lsn stays invalid, so
			 * the primary's walsender computes the smallest available lsn.
			 */
			SpinLockAcquire(&walrcv->mutex);
			walrcv->receiveStartTLI = startpointTLI = primaryTLI;
			SpinLockRelease(&walrcv->mutex);
			ereport(LOG,
					(errmsg("initial datamax node updated requested streaming timeline with primary's current one %u",
							primaryTLI)));
		}
		else
			polar_is_initial_datamax = false;

		/*
		 * Confirm that the current timeline of the primary is the same or
		 * ahead of ours.
		 */
		if (primaryTLI < startpointTLI)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("highest timeline %u of the primary is behind recovery timeline %u",
							primaryTLI, startpointTLI)));

		/*
		 * Get any missing history files. We do this always, even when we're
		 * not interested in that timeline, so that if we're promoted to
		 * become the primary later on, we don't select the same timeline that
		 * was already used in the current primary. This isn't bullet-proof -
		 * you'll need some external software to manage your cluster if you
		 * need to ensure that a unique timeline id is chosen in every case,
		 * but let's avoid the confusion of timeline id collisions where we
		 * can.
		 */
		WalRcvFetchTimeLineHistoryFiles(startpointTLI, primaryTLI);

		/*
		 * Create temporary replication slot if requested, and update slot
		 * name in shared memory.  (Note the slot name cannot already be set
		 * in this case.)
		 */
		if (is_temp_slot)
		{
			snprintf(slotname, sizeof(slotname),
					 "pg_walreceiver_%lld",
					 (long long int) walrcv_get_backend_pid(wrconn));

			walrcv_create_slot(wrconn, slotname, true, false, 0, NULL);

			SpinLockAcquire(&walrcv->mutex);
			strlcpy(walrcv->slotname, slotname, NAMEDATALEN);
			SpinLockRelease(&walrcv->mutex);
		}

		/*
		 * Start streaming.
		 *
		 * We'll try to start at the requested starting point and timeline,
		 * even if it's different from the server's latest timeline. In case
		 * we've already reached the end of the old timeline, the server will
		 * finish the streaming immediately, and we will go back to await
		 * orders from the startup process. If recovery_target_timeline is
		 * 'latest', the startup process will scan pg_wal and find the new
		 * history file, bump recovery target timeline, and ask us to restart
		 * on the new timeline.
		 */
		options.logical = false;
		options.startpoint = startpoint;
		options.slotname = slotname[0] != '\0' ? slotname : NULL;
		options.proto.physical.startpointTLI = startpointTLI;

		/* POLAR: Set current replication mode */
		options.polar_repl_mode = polar_gen_replication_mode();

		if (walrcv_startstreaming(wrconn, &options))
		{
			if (first_stream)
				ereport(LOG,
						(errmsg("started streaming WAL from primary at %X/%X on timeline %u",
								LSN_FORMAT_ARGS(startpoint), startpointTLI)));
			else
				ereport(LOG,
						(errmsg("restarted WAL streaming at %X/%X on timeline %u",
								LSN_FORMAT_ARGS(startpoint), startpointTLI)));
			first_stream = false;

			/* Initialize LogstreamResult and buffers for processing messages */
			if (!polar_is_datamax())
				LogstreamResult.Write = LogstreamResult.Flush = GetXLogReplayRecPtr(NULL);

			/*
			 * POLAR: datamax does not replay, the last received lsn comes
			 * from the datamax meta instead.
			 */
			else
				LogstreamResult.Write = LogstreamResult.Flush =
					polar_datamax_get_last_valid_received_lsn(polar_datamax_ctl, NULL);
			initStringInfo(&reply_message);
			initStringInfo(&incoming_message);

			/* Initialize the last recv timestamp */
			last_recv_timestamp = GetCurrentTimestamp();
			ping_sent = false;

			/* Loop until end-of-streaming or error */
			for (;;)
			{
				char	   *buf;
				int			len;
				bool		endofwal = false;
				pgsocket	wait_fd = PGINVALID_SOCKET;
				int			rc;

				/*
				 * Exit walreceiver if we're not in recovery. This should not
				 * happen, but cross-check the status here.
				 */
				if (!RecoveryInProgress())
					ereport(FATAL,
							(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
							 errmsg("cannot continue WAL streaming, recovery has already ended")));

				/* Process any requests or signals received recently */
				ProcessWalRcvInterrupts();

				if (ConfigReloadPending)
				{
					ConfigReloadPending = false;
					ProcessConfigFile(PGC_SIGHUP);
					XLogWalRcvSendHSFeedback(true);
				}

				/* POLAR: ask the walsender whether promote can be executed */
				if (polar_send_promote_request())
				{
					elog(LOG, "ask walsender whether promote can be executed");
					polar_walrcv_send_promote(true);
				}

				/* POLAR: if all WAL has been received, promote is allowed */
				polar_promote_check_received_all_wal();

				/* See if we can read data immediately */
				len = walrcv_receive(wrconn, &buf, &wait_fd);
				if (len != 0)
				{
					/*
					 * Process the received data, and any subsequent data we
					 * can read without blocking.
					 */
					for (;;)
					{
						if (len > 0)
						{
							/*
							 * Something was received from primary, so reset
							 * timeout
							 */
							last_recv_timestamp = GetCurrentTimestamp();
							ping_sent = false;
							XLogWalRcvProcessMsg(buf[0], &buf[1], len - 1,
												 startpointTLI);
						}
						else if (len == 0)
							break;
						else if (len < 0)
						{
							ereport(LOG,
									(errmsg("replication terminated by primary server"),
									 errdetail("End of WAL reached on timeline %u at %X/%X.",
											   startpointTLI,
											   LSN_FORMAT_ARGS(LogstreamResult.Write))));
							endofwal = true;
							break;
						}
						len = walrcv_receive(wrconn, &buf, &wait_fd);
					}

					/* Let the primary know that we received some data. */
					XLogWalRcvSendReply(false, false);

					/*
					 * If we've written some records, flush them to disk and
					 * let the startup process and primary server know about
					 * them.
					 */
					XLogWalRcvFlush(false, startpointTLI);
				}

				/* Check if we need to exit the streaming loop. */
				if (endofwal)
					break;

				/*
				 * Ideally we would reuse a WaitEventSet object repeatedly
				 * here to avoid the overheads of WaitLatchOrSocket on epoll
				 * systems, but we can't be sure that libpq (or any other
				 * walreceiver implementation) has the same socket (even if
				 * the fd is the same number, it may have been closed and
				 * reopened since the last time).  In future, if there is a
				 * function for removing sockets from WaitEventSet, then we
				 * could add and remove just the socket each time, potentially
				 * avoiding some system calls.
				 */
				Assert(wait_fd != PGINVALID_SOCKET);
				rc = WaitLatchOrSocket(MyLatch,
									   WL_EXIT_ON_PM_DEATH | WL_SOCKET_READABLE |
									   WL_TIMEOUT | WL_LATCH_SET,
									   wait_fd,
									   NAPTIME_PER_CYCLE,
									   WAIT_EVENT_WAL_RECEIVER_MAIN);
				if (rc & WL_LATCH_SET)
				{
					ResetLatch(MyLatch);
					ProcessWalRcvInterrupts();

					if (walrcv->force_reply)
					{
						/*
						 * The recovery process has asked us to send apply
						 * feedback now.  Make sure the flag is really set to
						 * false in shared memory before sending the reply, so
						 * we don't miss a new request for a reply.
						 */
						walrcv->force_reply = false;
						pg_memory_barrier();
						XLogWalRcvSendReply(true, false);
					}
				}
				if (rc & WL_TIMEOUT)
				{
					/*
					 * We didn't receive anything new. If we haven't heard
					 * anything from the server for more than
					 * wal_receiver_timeout / 2, ping the server. Also, if
					 * it's been longer than wal_receiver_status_interval
					 * since the last update we sent, send a status update to
					 * the primary anyway, to report any progress in applying
					 * WAL.
					 */
					bool		requestReply = false;

					/*
					 * Check if time since last receive from primary has
					 * reached the configured limit.
					 */
					if (wal_receiver_timeout > 0)
					{
						TimestampTz now = GetCurrentTimestamp();
						TimestampTz timeout;

						timeout =
							TimestampTzPlusMilliseconds(last_recv_timestamp,
														wal_receiver_timeout);

						if (now >= timeout)
							ereport(ERROR,
									(errcode(ERRCODE_CONNECTION_FAILURE),
									 errmsg("terminating walreceiver due to timeout")));

						/*
						 * We didn't receive anything new, for half of
						 * receiver replication timeout. Ping the server.
						 */
						if (!ping_sent)
						{
							timeout = TimestampTzPlusMilliseconds(last_recv_timestamp,
																  (wal_receiver_timeout / 2));
							if (now >= timeout)
							{
								requestReply = true;
								ping_sent = true;
							}
						}
					}

					XLogWalRcvSendReply(requestReply, requestReply);
					XLogWalRcvSendHSFeedback(false);
				}
			}

			/*
			 * The backend finished streaming. Exit streaming COPY-mode from
			 * our side, too.
			 */
			walrcv_endstreaming(wrconn, &primaryTLI);

			/*
			 * If the server had switched to a new timeline that we didn't
			 * know about when we began streaming, fetch its timeline history
			 * file now.
			 */
			WalRcvFetchTimeLineHistoryFiles(startpointTLI, primaryTLI);
		}
		else
			ereport(LOG,
					(errmsg("primary server contains no more WAL on requested timeline %u",
							startpointTLI)));

		/*
		 * End of WAL reached on the requested timeline. Close the last
		 * segment, and await for new orders from the startup process.
		 */
		if (recvFile >= 0)
		{
			char		xlogfname[MAXFNAMELEN];

			XLogWalRcvFlush(false, startpointTLI);
			XLogFileName(xlogfname, recvFileTLI, recvSegNo, wal_segment_size);
			if (polar_close(recvFile) != 0)
				ereport(PANIC,
						(errcode_for_file_access(),
						 errmsg("could not close log segment %s: %m",
								xlogfname)));

			/*
			 * Create .done file forcibly to prevent the streamed segment from
			 * being archived later.
			 */
			if (XLogArchiveMode != ARCHIVE_MODE_ALWAYS)
				XLogArchiveForceDone(xlogfname);
			else
				XLogArchiveNotify(xlogfname);
		}
		recvFile = -1;

		elog(DEBUG1, "walreceiver ended streaming and awaits new instructions");
		WalRcvWaitForStartPosition(&startpoint, &startpointTLI);
	}
	/* not reached */
}

/*
 * Wait for startup process to set receiveStart and receiveStartTLI.
 */
static void
WalRcvWaitForStartPosition(XLogRecPtr *startpoint, TimeLineID *startpointTLI)
{
	WalRcvData *walrcv = WalRcv;
	int			state;

	SpinLockAcquire(&walrcv->mutex);
	state = walrcv->walRcvState;
	if (state != WALRCV_STREAMING)
	{
		SpinLockRelease(&walrcv->mutex);
		if (state == WALRCV_STOPPING)
			proc_exit(0);
		else
			elog(FATAL, "unexpected walreceiver state");
	}
	walrcv->walRcvState = WALRCV_WAITING;
	walrcv->receiveStart = InvalidXLogRecPtr;
	walrcv->receiveStartTLI = 0;
	SpinLockRelease(&walrcv->mutex);

	set_ps_display("idle");

	/*
	 * nudge startup process to notice that we've stopped streaming and are
	 * now waiting for instructions.
	 */
	WakeupRecovery();
	for (;;)
	{
		ResetLatch(MyLatch);

		ProcessWalRcvInterrupts();

		SpinLockAcquire(&walrcv->mutex);
		Assert(walrcv->walRcvState == WALRCV_RESTARTING ||
			   walrcv->walRcvState == WALRCV_WAITING ||
			   walrcv->walRcvState == WALRCV_STOPPING);
		if (walrcv->walRcvState == WALRCV_RESTARTING)
		{
			/*
			 * No need to handle changes in primary_conninfo or
			 * primary_slot_name here. Startup process will signal us to
			 * terminate in case those change.
			 */
			*startpoint = walrcv->receiveStart;
			*startpointTLI = walrcv->receiveStartTLI;
			walrcv->walRcvState = WALRCV_STREAMING;
			SpinLockRelease(&walrcv->mutex);
			break;
		}
		if (walrcv->walRcvState == WALRCV_STOPPING)
		{
			/*
			 * We should've received SIGTERM if the startup process wants us
			 * to die, but might as well check it here too.
			 */
			SpinLockRelease(&walrcv->mutex);
			exit(1);
		}
		SpinLockRelease(&walrcv->mutex);

		(void) WaitLatch(MyLatch, WL_LATCH_SET | WL_EXIT_ON_PM_DEATH, 0,
						 WAIT_EVENT_WAL_RECEIVER_WAIT_START);
	}

	if (update_process_title)
	{
		char		activitymsg[50];

		snprintf(activitymsg, sizeof(activitymsg), "restarting at %X/%X",
				 LSN_FORMAT_ARGS(*startpoint));
		set_ps_display(activitymsg);
	}
}

/*
 * Fetch any missing timeline history files between 'first' and 'last'
 * (inclusive) from the server.
 */
static void
WalRcvFetchTimeLineHistoryFiles(TimeLineID first, TimeLineID last)
{
	TimeLineID	tli;

	/* WalReceiverMain() pinned the flag to the node type for this process */
	Assert(polar_is_datamax_mode == polar_is_datamax());

	/*
	 * POLAR: a datamax keeps its WAL and timeline history under
	 * POLAR_DATAMAX_WAL_DIR rather than the regular pg_wal.
	 * existsTimeLineHistory() and writeTimeLineHistoryFile() pick the
	 * directory from polar_is_datamax_mode, which WalReceiverMain() already
	 * pinned for the life of this process. Otherwise the history file would
	 * be written to (and looked up in) the wrong directory, and the datamax
	 * would never see the upstream timeline switch.
	 */
	for (tli = first; tli <= last; tli++)
	{
		/* there's no history file for timeline 1 */
		if (tli != 1 && !existsTimeLineHistory(tli))
		{
			char	   *fname;
			char	   *content;
			int			len;
			char		expectedfname[MAXFNAMELEN];

			ereport(LOG,
					(errmsg("fetching timeline history file for timeline %u from primary server",
							tli)));

			walrcv_readtimelinehistoryfile(wrconn, tli, &fname, &content, &len);

			/*
			 * Check that the filename on the primary matches what we
			 * calculated ourselves. This is just a sanity check, it should
			 * always match.
			 */
			TLHistoryFileName(expectedfname, tli);
			if (strcmp(fname, expectedfname) != 0)
				ereport(ERROR,
						(errcode(ERRCODE_PROTOCOL_VIOLATION),
						 errmsg_internal("primary reported unexpected file name for timeline history file of timeline %u",
										 tli)));

			/*
			 * Write the file to pg_wal.
			 */
			writeTimeLineHistoryFile(tli, content, len);

			/*
			 * Mark the streamed history file as ready for archiving if
			 * archive_mode is always.
			 */
			if (XLogArchiveMode != ARCHIVE_MODE_ALWAYS)
				XLogArchiveForceDone(fname);
			else
				XLogArchiveNotify(fname);

			pfree(fname);
			pfree(content);
		}
	}
}

/*
 * Mark us as STOPPED in shared memory at exit.
 */
static void
WalRcvDie(int code, Datum arg)
{
	WalRcvData *walrcv = WalRcv;
	TimeLineID *startpointTLI_p = (TimeLineID *) DatumGetPointer(arg);

	/*
	 * Ensure that all WAL records received are flushed to disk.
	 *
	 * POLAR: an initial datamax exits here with an invalid (0) timeline when
	 * it errors out before establishing streaming (e.g. its upstream datamax
	 * has not learned its own timeline yet). Nothing has been received in
	 * that case, so the flush would be a no-op; skip it rather than trip the
	 * tli != 0 assertions, which would turn a recoverable walreceiver error
	 * into a SIGABRT that crashes the whole node.
	 */
	if (*startpointTLI_p != POLAR_INVALID_TIMELINE_ID)
		XLogWalRcvFlush(true, *startpointTLI_p);

	/* Mark ourselves inactive in shared memory */
	SpinLockAcquire(&walrcv->mutex);
	Assert(walrcv->walRcvState == WALRCV_STREAMING ||
		   walrcv->walRcvState == WALRCV_RESTARTING ||
		   walrcv->walRcvState == WALRCV_STARTING ||
		   walrcv->walRcvState == WALRCV_WAITING ||
		   walrcv->walRcvState == WALRCV_STOPPING);
	Assert(walrcv->pid == MyProcPid);
	walrcv->walRcvState = WALRCV_STOPPED;
	walrcv->pid = 0;
	walrcv->ready_to_display = false;
	walrcv->latch = NULL;
	SpinLockRelease(&walrcv->mutex);

	ConditionVariableBroadcast(&walrcv->walRcvStoppedCV);

	/* Terminate the connection gracefully. */
	if (wrconn != NULL)
		walrcv_disconnect(wrconn);

	/* Wake up the startup process to notice promptly that we're gone */
	WakeupRecovery();
}

/*
 * Accept the message from XLOG stream, and process it.
 */
static void
XLogWalRcvProcessMsg(unsigned char type, char *buf, Size len, TimeLineID tli)
{
	int			hdrlen;
	XLogRecPtr	dataStart;
	XLogRecPtr	walEnd;
	TimestampTz sendTime;
	bool		replyRequested;

	/* POLAR */
	XLogRecPtr	consistent_lsn;

	/* POLAR: extra fields carried by the datamax 'e' message */
	uint32		polar_primary_next_xid;
	uint32		polar_primary_epoch;
	XLogRecPtr	polar_primary_last_lsn;
	XLogSegNo	polar_upstream_last_removed_segno;

	resetStringInfo(&incoming_message);

	switch (type)
	{
		case 'w':				/* WAL records */
			{
				/* copy message to StringInfo */
				hdrlen = sizeof(int64) + sizeof(int64) + sizeof(int64);
				if (len < hdrlen)
					ereport(ERROR,
							(errcode(ERRCODE_PROTOCOL_VIOLATION),
							 errmsg_internal("invalid WAL message received from primary")));
				appendBinaryStringInfo(&incoming_message, buf, hdrlen);

				/* read the fields */
				dataStart = pq_getmsgint64(&incoming_message);
				walEnd = pq_getmsgint64(&incoming_message);
				sendTime = pq_getmsgint64(&incoming_message);
				ProcessWalSndrMessage(walEnd, sendTime);

				buf += hdrlen;
				len -= hdrlen;
				XLogWalRcvWrite(buf, len, dataStart, tli);
				break;
			}

			/*
			 * POLAR: WAL records for a datamax node.  Same as 'w', but also
			 * carries the primary's nextXid and epoch (needed to feed back a
			 * sane standby xmin), the primary's last valid LSN (used to keep
			 * WAL consistent with the upstream node) and the primary's last
			 * removed segment number (so the datamax does not remove WAL
			 * which has not been removed upstream).
			 */
		case 'e':
			{
				/* copy message to StringInfo */
				hdrlen = sizeof(int64) + sizeof(int64) + sizeof(int32) +
					sizeof(int32) + sizeof(int64) + sizeof(int64) + sizeof(int64);
				if (len < hdrlen)
					ereport(ERROR,
							(errcode(ERRCODE_PROTOCOL_VIOLATION),
							 errmsg_internal("invalid WAL message received from primary")));
				appendBinaryStringInfo(&incoming_message, buf, hdrlen);

				/* read the fields */
				dataStart = pq_getmsgint64(&incoming_message);
				walEnd = pq_getmsgint64(&incoming_message);
				polar_primary_next_xid = pq_getmsgint(&incoming_message, 4);
				polar_primary_epoch = pq_getmsgint(&incoming_message, 4);
				polar_primary_last_lsn = pq_getmsgint64(&incoming_message);
				polar_upstream_last_removed_segno = pq_getmsgint64(&incoming_message);
				sendTime = pq_getmsgint64(&incoming_message);
				ProcessWalSndrMessage(walEnd, sendTime);

				/* record nextXid and epoch of the primary */
				POLAR_DATAMAX_SET_PRIMARY_NEXTXID(polar_primary_next_xid);
				POLAR_DATAMAX_SET_PRIMARY_NEXTEPOCH(polar_primary_epoch);
				/* record polar_primary_last_lsn into the list */
				polar_datamax_insert_last_valid_lsn(polar_datamax_received_valid_lsn_list,
													polar_primary_last_lsn);
				/* record polar_upstream_last_removed_segno */
				polar_datamax_update_upstream_last_removed_segno(polar_datamax_ctl,
																 polar_upstream_last_removed_segno);

				buf += hdrlen;
				len -= hdrlen;
				XLogWalRcvWrite(buf, len, dataStart, tli);

				/*
				 * POLAR: seed the datamax meta's min received info once, from
				 * the first WAL record received by an initial datamax node.
				 * The timeline was negotiated from the primary in
				 * WalReceiverMain (startpointTLI = primaryTLI), and dataStart
				 * is the smallest lsn the primary chose to stream.
				 */
				if (unlikely(polar_is_initial_datamax))
				{
					polar_datamax_update_min_received_info(polar_datamax_ctl, tli, dataStart);
					polar_datamax_write_meta(polar_datamax_ctl, true);
					polar_is_initial_datamax = false;
				}
				break;
			}
		case 'k':				/* Keepalive */
			{
				/* copy message to StringInfo */
				hdrlen = sizeof(int64) + sizeof(int64) + sizeof(char);
				if (len != hdrlen)
					ereport(ERROR,
							(errcode(ERRCODE_PROTOCOL_VIOLATION),
							 errmsg_internal("invalid keepalive message received from primary")));
				appendBinaryStringInfo(&incoming_message, buf, hdrlen);

				/* read the fields */
				walEnd = pq_getmsgint64(&incoming_message);
				sendTime = pq_getmsgint64(&incoming_message);
				replyRequested = pq_getmsgbyte(&incoming_message);

				ProcessWalSndrMessage(walEnd, sendTime);

				/* If the primary requested a reply, send one immediately */
				if (replyRequested)
					XLogWalRcvSendReply(true, false);
				break;
			}
			/* POLAR: receive lsn info */
		case 'p':
			{
				/*
				 * POLAR: replica mode, does not contain any wal data, copy
				 * message to StringInfo
				 */
				hdrlen = sizeof(int64) + sizeof(int64) + sizeof(int64) + sizeof(int64);
				if (len < hdrlen)
					ereport(ERROR,
							(errcode(ERRCODE_PROTOCOL_VIOLATION),
							 errmsg_internal("invalid consist lsn message received from primary")));
				appendBinaryStringInfo(&incoming_message, buf, hdrlen);

				dataStart = pq_getmsgint64(&incoming_message);
				walEnd = pq_getmsgint64(&incoming_message);
				consistent_lsn = pq_getmsgint64(&incoming_message);
				sendTime = pq_getmsgint64(&incoming_message);
				ProcessWalSndrMessage(walEnd, sendTime);

				if (WalRcv->polar_use_xlog_queue)
				{
					Assert(polar_logindex_redo_instance);

					polar_xlog_recv_queue_push_storage_begin(polar_logindex_redo_instance->xlog_queue, polar_recv_push_storage_begin_callback);

					SpinLockAcquire(&WalRcv->mutex);
					WalRcv->polar_use_xlog_queue = false;
					SpinLockRelease(&WalRcv->mutex);

					elog(LOG, "primary xlog queue is full, changed to send from file");
				}

				/*
				 * POLAR: As a replica, we do not write the xlog, we just
				 * update the LogstreamResult.write and call XLogWalRcvFlush
				 * to update shared memory status as the case 'w'.
				 */
				LogstreamResult.Write = walEnd;
				XLogWalRcvFlush(false, tli);

				/* POLAR: Update consistent lsn */
				polar_set_primary_consistent_lsn(consistent_lsn);

				if (polar_enable_debug)
				{
					elog(LOG, "Receive primary on PolarDB flush xlog from %X/%X to %X/%X, ",
						 LSN_FORMAT_ARGS(dataStart),
						 LSN_FORMAT_ARGS(walEnd));
				}
				break;
			}
			/* POLAR: keepalive with consistent lsn */
		case 'K':
			{
				hdrlen = sizeof(int64) + sizeof(int64) + sizeof(int64) + sizeof(char);
				if (len != hdrlen)
				{
					ereport(ERROR,
							(errcode(ERRCODE_PROTOCOL_VIOLATION),
							 errmsg_internal("invalid keepalive message received from primary")));
				}

				appendBinaryStringInfo(&incoming_message, buf, hdrlen);

				/* POLAR: Read the fields */
				walEnd = pq_getmsgint64(&incoming_message);
				consistent_lsn = pq_getmsgint64(&incoming_message);
				sendTime = pq_getmsgint64(&incoming_message);
				replyRequested = pq_getmsgbyte(&incoming_message);

				ProcessWalSndrMessage(walEnd, sendTime);

				/*
				 * POLAR: As a PolarDB replica, we do not write the xlog, we
				 * just update the LogstreamResult.write and call
				 * XLogWalRcvFlush to update shared-memory status as the case
				 * 'w'.
				 */
				LogstreamResult.Write = walEnd;
				XLogWalRcvFlush(false, tli);

				/* POLAR: Update consistent lsn */
				polar_set_primary_consistent_lsn(consistent_lsn);

				if (polar_enable_debug)
				{
					elog(LOG, "Receive primary on PolarDB keepalive with walEnd %X/%X and consistent lsn %X/%X",
						 LSN_FORMAT_ARGS(walEnd),
						 LSN_FORMAT_ARGS(consistent_lsn));
				}

				/*
				 * POLAR: If the primary requested a reply, send one
				 * immediately
				 */
				if (replyRequested)
					XLogWalRcvSendReply(true, false);
				break;
			}
			/* POLAR: streaming xlog meta */
		case 'y':
			{
				/* POLAR: copy message to StringInfo */
				hdrlen = sizeof(int64) + sizeof(int64) + sizeof(int64);
				if (len < hdrlen)
					ereport(ERROR,
							(errcode(ERRCODE_PROTOCOL_VIOLATION),
							 errmsg_internal("invalid WAL message received from primary")));
				appendBinaryStringInfo(&incoming_message, buf, hdrlen);

				/* POLAR: read the fields */
				walEnd = pq_getmsgint64(&incoming_message);
				consistent_lsn = pq_getmsgint64(&incoming_message);
				sendTime = pq_getmsgint64(&incoming_message);
				ProcessWalSndrMessage(walEnd, sendTime);

				buf += hdrlen;
				len -= hdrlen;

				if (len > 0)
				{
					polar_xlog_recv_queue_push(polar_logindex_redo_instance->xlog_queue, buf, len,
											   polar_receiver_xlog_queue_callback);

					if (!WalRcv->polar_use_xlog_queue)
					{
						SpinLockAcquire(&WalRcv->mutex);
						WalRcv->polar_use_xlog_queue = true;
						SpinLockRelease(&WalRcv->mutex);

						elog(LOG, "primary send data changed from file to queue");
					}
				}

				/*
				 * POLAR: As a polardb replica, we do not write the xlog, we
				 * just update the LogstreamResult.write and call
				 * XLogWalRcvFlush to update shared-memory status as the case
				 * 'w'.
				 */
				LogstreamResult.Write = walEnd;
				XLogWalRcvFlush(false, tli);

				/* POLAR: Update new consistent lsn */
				polar_set_primary_consistent_lsn(consistent_lsn);

				if (polar_enable_debug)
				{
					elog(LOG, "Receive XLOG without payload, and end lsn is %X/%X",
						 LSN_FORMAT_ARGS(walEnd));
				}
				break;
			}

			/*
			 * POLAR: end_lsn reply from walsender; when received lsn =
			 * end_lsn, promote is ready
			 */
		case 'l':
			{
				bool		is_promote_allowed;
				XLogRecPtr	end_lsn;

				hdrlen = sizeof(char) + sizeof(int64);
				if (len != hdrlen)
					ereport(ERROR,
							(errcode(ERRCODE_PROTOCOL_VIOLATION),
							 errmsg_internal("invalid endlsn message received from primary")));
				appendBinaryStringInfo(&incoming_message, buf, hdrlen);

				/* read the fields */
				is_promote_allowed = pq_getmsgbyte(&incoming_message);
				end_lsn = pq_getmsgint64(&incoming_message);
				elog(LOG, "received reply from walsender, is_promote_allowed:%d, end_lsn:%X/%X",
					 is_promote_allowed, LSN_FORMAT_ARGS(end_lsn));
				polar_process_walsender_reply(is_promote_allowed, end_lsn);
				break;
			}
			/* POLAR end */
		default:
			ereport(ERROR,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg_internal("invalid replication message type %d",
									 type)));
	}
}

/*
 * Write XLOG data to disk.
 */
static void
XLogWalRcvWrite(char *buf, Size nbytes, XLogRecPtr recptr, TimeLineID tli)
{
	int			startoff;
	int			byteswritten;

	Assert(tli != 0);
	/* WalReceiverMain() pinned the flag to the node type for this process */
	Assert(polar_is_datamax_mode == polar_is_datamax());

	while (nbytes > 0)
	{
		int			segbytes;

		/* Close the current segment if it's completed */
		if (recvFile >= 0 && !XLByteInSeg(recptr, recvSegNo, wal_segment_size))
			XLogWalRcvClose(recptr, tli);

		if (recvFile < 0)
		{
			/* Create/use new log file */
			XLByteToSeg(recptr, recvSegNo, wal_segment_size);
			recvFile = XLogFileInit(recvSegNo, tli);
			recvFileTLI = tli;
		}

		/* Calculate the start offset of the received logs */
		startoff = XLogSegmentOffset(recptr, wal_segment_size);

		if (startoff + nbytes > wal_segment_size)
			segbytes = wal_segment_size - startoff;
		else
			segbytes = nbytes;

		/* OK to write the logs */
		errno = 0;

		byteswritten = polar_pwrite(recvFile, buf, segbytes, (off_t) startoff);
		if (byteswritten <= 0)
		{
			char		xlogfname[MAXFNAMELEN];
			int			save_errno;

			/* if write didn't set errno, assume no disk space */
			if (errno == 0)
				errno = ENOSPC;

			save_errno = errno;
			XLogFileName(xlogfname, recvFileTLI, recvSegNo, wal_segment_size);
			errno = save_errno;
			ereport(PANIC,
					(errcode_for_file_access(),
					 errmsg("could not write to log segment %s "
							"at offset %u, length %lu: %m",
							xlogfname, startoff, (unsigned long) segbytes)));
		}

		/* Update state for write */
		recptr += byteswritten;

		nbytes -= byteswritten;
		buf += byteswritten;

		LogstreamResult.Write = recptr;
	}

	/* Update shared-memory status */
	pg_atomic_write_u64(&WalRcv->writtenUpto, LogstreamResult.Write);

	/*
	 * Close the current segment if it's fully written up in the last cycle of
	 * the loop, to create its archive notification file soon. Otherwise WAL
	 * archiving of the segment will be delayed until any data in the next
	 * segment is received and written.
	 */
	if (recvFile >= 0 && !XLByteInSeg(recptr, recvSegNo, wal_segment_size))
		XLogWalRcvClose(recptr, tli);
}

/*
 * Flush the log to disk.
 *
 * If we're in the midst of dying, it's unwise to do anything that might throw
 * an error, so we skip sending a reply in that case.
 */
static void
XLogWalRcvFlush(bool dying, TimeLineID tli)
{
	Assert(tli != 0);
	/* WalReceiverMain() pinned the flag to the node type for this process */
	Assert(polar_is_datamax_mode == polar_is_datamax());

	if (LogstreamResult.Flush < LogstreamResult.Write)
	{
		WalRcvData *walrcv = WalRcv;

		/* POLAR */
		XLogRecPtr	consistent_lsn = InvalidXLogRecPtr;

		/* POLAR: only replica mode not write data */
		if (!polar_is_replica())
			issue_xlog_fsync(recvFile, recvSegNo, tli);

		LogstreamResult.Flush = LogstreamResult.Write;

		/* POLAR: persist datamax meta on each flush. */
		if (polar_is_datamax())
		{
			polar_datamax_update_received_info(polar_datamax_ctl, tli,
											   LogstreamResult.Flush);
			/* advance last valid received lsn up to the primary-confirmed lsn */
			polar_datamax_update_cur_valid_lsn(polar_datamax_received_valid_lsn_list,
											   LogstreamResult.Flush);
			polar_datamax_write_meta(polar_datamax_ctl, true);
		}
		/* POLAR end */

		/* Update shared-memory status */
		SpinLockAcquire(&walrcv->mutex);
		if (walrcv->flushedUpto < LogstreamResult.Flush)
		{
			walrcv->latestChunkStart = walrcv->flushedUpto;
			walrcv->flushedUpto = LogstreamResult.Flush;
			walrcv->receivedTLI = tli;

			/* POLAR: set consistent lsn */
			consistent_lsn = pg_atomic_read_u64(&walrcv->curr_primary_consistent_lsn);
		}
		SpinLockRelease(&walrcv->mutex);

		/* POLAR: update latest flush lsn for the promote-wait subsystem */
		pg_atomic_write_u64(&WalRcv->polar_latest_flush_lsn, LogstreamResult.Flush);

		/* Signal the startup process and walsender that new WAL has arrived */
		WakeupRecovery();
		if (AllowCascadeReplication())
			WalSndWakeup();

		/* Report XLOG streaming progress in PS display */
		if (update_process_title)
		{
			char		activitymsg[50];

			/* POLAR */
			if (polar_is_replica())
				snprintf(activitymsg,
						 sizeof(activitymsg),
						 "streaming %X/%X, consistent lsn %X/%X",
						 LSN_FORMAT_ARGS(LogstreamResult.Write),
						 LSN_FORMAT_ARGS(consistent_lsn));
			else
				snprintf(activitymsg, sizeof(activitymsg), "streaming %X/%X",
						 LSN_FORMAT_ARGS(LogstreamResult.Write));
			set_ps_display(activitymsg);
		}

		/* Also let the primary know that we made some progress */
		if (!dying)
		{
			XLogWalRcvSendReply(false, false);
			XLogWalRcvSendHSFeedback(false);
		}
	}
}

/*
 * Close the current segment.
 *
 * Flush the segment to disk before closing it. Otherwise we have to
 * reopen and fsync it later.
 *
 * Create an archive notification file since the segment is known completed.
 */
static void
XLogWalRcvClose(XLogRecPtr recptr, TimeLineID tli)
{
	char		xlogfname[MAXFNAMELEN];

	Assert(recvFile >= 0 && !XLByteInSeg(recptr, recvSegNo, wal_segment_size));
	Assert(tli != 0);
	/* WalReceiverMain() pinned the flag to the node type for this process */
	Assert(polar_is_datamax_mode == polar_is_datamax());

	/*
	 * fsync() and close current file before we switch to next one. We would
	 * otherwise have to reopen this file to fsync it later
	 */
	XLogWalRcvFlush(false, tli);

	XLogFileName(xlogfname, recvFileTLI, recvSegNo, wal_segment_size);

	/*
	 * XLOG segment files will be re-read by recovery in startup process soon,
	 * so we don't advise the OS to release cache pages associated with the
	 * file like XLogFileClose() does.
	 */
	if (polar_close(recvFile) != 0)
		ereport(PANIC,
				(errcode_for_file_access(),
				 errmsg("could not close log segment %s: %m",
						xlogfname)));

	/*
	 * Create .done file forcibly to prevent the streamed segment from being
	 * archived later.
	 */
	if (XLogArchiveMode != ARCHIVE_MODE_ALWAYS)
		XLogArchiveForceDone(xlogfname);
	else
		XLogArchiveNotify(xlogfname);

	recvFile = -1;
}

/*
 * Send reply message to primary, indicating our current WAL locations, oldest
 * xmin and the current time.
 *
 * If 'force' is not set, the message is only sent if enough time has
 * passed since last status update to reach wal_receiver_status_interval.
 * If wal_receiver_status_interval is disabled altogether and 'force' is
 * false, this is a no-op.
 *
 * If 'requestReply' is true, requests the server to reply immediately upon
 * receiving this message. This is used for heartbeats, when approaching
 * wal_receiver_timeout.
 */
static void
XLogWalRcvSendReply(bool force, bool requestReply)
{
	static XLogRecPtr writePtr = 0;
	static XLogRecPtr flushPtr = 0;
	XLogRecPtr	applyPtr;
	static TimestampTz sendTime = 0;
	TimestampTz now;

	/*
	 * If the user doesn't want status to be reported to the primary, be sure
	 * to exit before doing anything at all.
	 */
	if (!force && wal_receiver_status_interval <= 0)
		return;

	/* Get current timestamp. */
	now = GetCurrentTimestamp();

	/*
	 * We can compare the write and flush positions to the last message we
	 * sent without taking any lock, but the apply position requires a spin
	 * lock, so we don't check that unless something else has changed or 10
	 * seconds have passed.  This means that the apply WAL location will
	 * appear, from the primary's point of view, to lag slightly, but since
	 * this is only for reporting purposes and only on idle systems, that's
	 * probably OK.
	 */
	if (!force
		&& writePtr == LogstreamResult.Write
		&& flushPtr == LogstreamResult.Flush
		&& !TimestampDifferenceExceeds(sendTime, now,
									   wal_receiver_status_interval * 1000))
		return;
	sendTime = now;

	/* Construct a new message */
	writePtr = LogstreamResult.Write;
	flushPtr = LogstreamResult.Flush;
	/* POLAR: a datamax never replays WAL, report the flush position instead */
	applyPtr = polar_is_datamax() ? LogstreamResult.Flush : GetXLogReplayRecPtr(NULL);

	resetStringInfo(&reply_message);
	pq_sendbyte(&reply_message, 'r');
	pq_sendint64(&reply_message, writePtr);
	pq_sendint64(&reply_message, flushPtr);
	pq_sendint64(&reply_message, applyPtr);
	pq_sendint64(&reply_message, GetCurrentTimestamp());
	pq_sendbyte(&reply_message, requestReply ? 1 : 0);

	if (polar_is_replica())
	{
		XLogRecPtr	bg_replayed_lsn = InvalidXLogRecPtr;
		XLogRecPtr	lockPtr = InvalidXLogRecPtr;

		Assert(!XLogRecPtrIsInvalid(applyPtr));

		/* POLAR: return the oldest ddl lock lsn if enable async lock */
		lockPtr = polar_allow_alr() ? polar_alr_ctl->lsn : InvalidXLogRecPtr;

		/*
		 * POLAR: lockPtr is the next record begin position of the lock
		 * record. If lockPtr is invalid, means there is no lock in replaying,
		 * just return applyPtr as lockPtr. And lockPtr might be larger than
		 * applyPtr, it's because async replay worker replay the lock but
		 * startup has not read the next record. We don't allow it, for the
		 * next time here, lockPtr might be invalid and use a smaller applyPtr
		 * as lockPtr, making lockPtr not monotonically increasing.
		 */
		if (XLogRecPtrIsInvalid(lockPtr) || lockPtr > applyPtr)
			lockPtr = applyPtr;

		pq_sendint64(&reply_message, lockPtr);

		/*
		 * POLAR: Send background replay lsn. Even if page outdate is
		 * disabled, it also send a lsn to keep protocol compatibility.
		 */
		if (polar_logindex_redo_instance)
		{
			static TimestampTz last_update_time = 0;
			static XLogRecPtr last_bg_replayed_lsn = InvalidXLogRecPtr;
			TimestampTz now;

			now = GetCurrentTimestamp();

			/*
			 * POLAR: Page replay in backend process need xlog after
			 * consistent lsn, so we should keep xlog after consisten lsn. To
			 * avoid holding ProcArrayLock too frequently, we call
			 * polar_get_read_min_lsn() every second.
			 */
			if (TimestampDifferenceExceeds(last_update_time, now, POLAR_UPDATE_BACKEND_LSN_INTERVAL))
			{
				last_bg_replayed_lsn = polar_get_read_min_lsn(polar_get_primary_consistent_lsn());
				last_update_time = now;
			}
			bg_replayed_lsn = last_bg_replayed_lsn;
		}
		else
			bg_replayed_lsn = InvalidXLogRecPtr;
		pq_sendint64(&reply_message, bg_replayed_lsn);
	}

	/* Send it */
	elog(DEBUG2, "sending write %X/%X flush %X/%X apply %X/%X%s",
		 LSN_FORMAT_ARGS(writePtr),
		 LSN_FORMAT_ARGS(flushPtr),
		 LSN_FORMAT_ARGS(applyPtr),
		 requestReply ? " (reply requested)" : "");

	walrcv_send(wrconn, reply_message.data, reply_message.len);
}

/*
 * Send hot standby feedback message to primary, plus the current time,
 * in case they don't have a watch.
 *
 * If the user disables feedback, send one final message to tell sender
 * to forget about the xmin on this standby. We also send this message
 * on first connect because a previous connection might have set xmin
 * on a replication slot. (If we're not using a slot it's harmless to
 * send a feedback message explicitly setting InvalidTransactionId).
 */
static void
XLogWalRcvSendHSFeedback(bool immed)
{
	TimestampTz now;
	FullTransactionId nextFullXid;
	TransactionId nextXid;
	uint32		xmin_epoch,
				catalog_xmin_epoch;
	TransactionId xmin,
				catalog_xmin;
	static TimestampTz sendTime = 0;

	/* initially true so we always send at least one feedback message */
	static bool primary_has_standby_xmin = true;

	/*
	 * If the user doesn't want status to be reported to the primary, be sure
	 * to exit before doing anything at all.
	 */
	if ((wal_receiver_status_interval <= 0 || !hot_standby_feedback) &&
		!primary_has_standby_xmin)
		return;

	/* Get current timestamp. */
	now = GetCurrentTimestamp();

	if (!immed)
	{
		/*
		 * Send feedback at most once per wal_receiver_status_interval.
		 */
		if (!TimestampDifferenceExceeds(sendTime, now,
										wal_receiver_status_interval * 1000))
			return;
		sendTime = now;
	}

	/*
	 * If Hot Standby is not yet accepting connections there is nothing to
	 * send. Check this after the interval has expired to reduce number of
	 * calls.
	 *
	 * Bailing out here also ensures that we don't send feedback until we've
	 * read our own replication slot state, so we don't tell the primary to
	 * discard needed xmin or catalog_xmin from any slots that may exist on
	 * this replica.
	 */
	/* POLAR: a datamax never accepts connections but still feeds back */
	if (!HotStandbyActive() && !polar_is_datamax())
		return;

	/*
	 * Make the expensive call to get the oldest xmin once we are certain
	 * everything else has been checked.
	 */
	if (hot_standby_feedback)
	{
		GetReplicationHorizons(&xmin, &catalog_xmin);

		/*
		 * POLAR: a datamax holds no primary data and runs no local
		 * transactions; its frozen xid state would otherwise produce a
		 * horizon older than the cascaded standby's slot xmin and infect the
		 * primary's vacuum.  Feed back only the xmin of our own replication
		 * slots - the primary only cares about the standby's xmin, which the
		 * datamax just records and forwards.
		 */
		if (polar_is_datamax())
		{
			TransactionId slot_xmin;

			ProcArrayGetReplicationSlotXmin(&slot_xmin, &catalog_xmin);
			xmin = slot_xmin;
		}
	}
	else
	{
		xmin = InvalidTransactionId;
		catalog_xmin = InvalidTransactionId;
	}

	/*
	 * Get epoch and adjust if nextXid and oldestXmin are different sides of
	 * the epoch boundary.
	 *
	 * POLAR: in datamax mode there is no local xid state; use the primary's
	 * nextXid and epoch recorded in polar_datamax_ctl from the WAL stream.
	 */
	if (!polar_is_datamax())
	{
		nextFullXid = ReadNextFullTransactionId();
		nextXid = XidFromFullTransactionId(nextFullXid);
		xmin_epoch = EpochFromFullTransactionId(nextFullXid);
	}
	else
	{
		nextXid = pg_atomic_read_u32(&polar_datamax_ctl->polar_primary_next_xid);
		xmin_epoch = pg_atomic_read_u32(&polar_datamax_ctl->polar_primary_epoch);
	}
	catalog_xmin_epoch = xmin_epoch;
	if (nextXid < xmin)
		xmin_epoch--;
	if (nextXid < catalog_xmin)
		catalog_xmin_epoch--;

	elog(DEBUG2, "sending hot standby feedback xmin %u epoch %u catalog_xmin %u catalog_xmin_epoch %u",
		 xmin, xmin_epoch, catalog_xmin, catalog_xmin_epoch);

	/* Construct the message and send it. */
	resetStringInfo(&reply_message);
	pq_sendbyte(&reply_message, 'h');
	pq_sendint64(&reply_message, GetCurrentTimestamp());
	pq_sendint32(&reply_message, xmin);
	pq_sendint32(&reply_message, xmin_epoch);
	pq_sendint32(&reply_message, catalog_xmin);
	pq_sendint32(&reply_message, catalog_xmin_epoch);
	walrcv_send(wrconn, reply_message.data, reply_message.len);
	if (TransactionIdIsValid(xmin) || TransactionIdIsValid(catalog_xmin))
		primary_has_standby_xmin = true;
	else
		primary_has_standby_xmin = false;
}

/*
 * Update shared memory status upon receiving a message from primary.
 *
 * 'walEnd' and 'sendTime' are the end-of-WAL and timestamp of the latest
 * message, reported by primary.
 */
static void
ProcessWalSndrMessage(XLogRecPtr walEnd, TimestampTz sendTime)
{
	WalRcvData *walrcv = WalRcv;

	TimestampTz lastMsgReceiptTime = GetCurrentTimestamp();

	/* Update shared-memory status */
	SpinLockAcquire(&walrcv->mutex);
	if (walrcv->latestWalEnd < walEnd)
		walrcv->latestWalEndTime = sendTime;
	walrcv->latestWalEnd = walEnd;
	walrcv->lastMsgSendTime = sendTime;
	walrcv->lastMsgReceiptTime = lastMsgReceiptTime;
	SpinLockRelease(&walrcv->mutex);

	if (message_level_is_interesting(DEBUG2))
	{
		char	   *sendtime;
		char	   *receipttime;
		int			applyDelay;

		/* Copy because timestamptz_to_str returns a static buffer */
		sendtime = pstrdup(timestamptz_to_str(sendTime));
		receipttime = pstrdup(timestamptz_to_str(lastMsgReceiptTime));
		applyDelay = GetReplicationApplyDelay();

		/* apply delay is not available */
		if (applyDelay == -1)
			elog(DEBUG2, "sendtime %s receipttime %s replication apply delay (N/A) transfer latency %d ms",
				 sendtime,
				 receipttime,
				 GetReplicationTransferLatency());
		else
			elog(DEBUG2, "sendtime %s receipttime %s replication apply delay %d ms transfer latency %d ms",
				 sendtime,
				 receipttime,
				 applyDelay,
				 GetReplicationTransferLatency());

		pfree(sendtime);
		pfree(receipttime);
	}
}

/*
 * Wake up the walreceiver main loop.
 *
 * This is called by the startup process whenever interesting xlog records
 * are applied, so that walreceiver can check if it needs to send an apply
 * notification back to the primary which may be waiting in a COMMIT with
 * synchronous_commit = remote_apply.
 */
void
WalRcvForceReply(void)
{
	Latch	   *latch;

	WalRcv->force_reply = true;
	/* fetching the latch pointer might not be atomic, so use spinlock */
	SpinLockAcquire(&WalRcv->mutex);
	latch = WalRcv->latch;
	SpinLockRelease(&WalRcv->mutex);
	if (latch)
		SetLatch(latch);
}

/*
 * Return a string constant representing the state. This is used
 * in system functions and views, and should *not* be translated.
 */
static const char *
WalRcvGetStateString(WalRcvState state)
{
	switch (state)
	{
		case WALRCV_STOPPED:
			return "stopped";
		case WALRCV_STARTING:
			return "starting";
		case WALRCV_STREAMING:
			return "streaming";
		case WALRCV_WAITING:
			return "waiting";
		case WALRCV_RESTARTING:
			return "restarting";
		case WALRCV_STOPPING:
			return "stopping";
	}
	return "UNKNOWN";
}

/*
 * Returns activity of WAL receiver, including pid, state and xlog locations
 * received from the WAL sender of another server.
 */
Datum
pg_stat_get_wal_receiver(PG_FUNCTION_ARGS)
{
	TupleDesc	tupdesc;
	Datum	   *values;
	bool	   *nulls;
	int			pid;
	bool		ready_to_display;
	WalRcvState state;
	XLogRecPtr	receive_start_lsn;
	TimeLineID	receive_start_tli;
	XLogRecPtr	written_lsn;
	XLogRecPtr	flushed_lsn;
	TimeLineID	received_tli;
	TimestampTz last_send_time;
	TimestampTz last_receipt_time;
	XLogRecPtr	latest_end_lsn;
	TimestampTz latest_end_time;
	char		sender_host[NI_MAXHOST];
	int			sender_port = 0;
	char		slotname[NAMEDATALEN];
	char		conninfo[MAXCONNINFO];

	/* Take a lock to ensure value consistency */
	SpinLockAcquire(&WalRcv->mutex);
	pid = (int) WalRcv->pid;
	ready_to_display = WalRcv->ready_to_display;
	state = WalRcv->walRcvState;
	receive_start_lsn = WalRcv->receiveStart;
	receive_start_tli = WalRcv->receiveStartTLI;
	flushed_lsn = WalRcv->flushedUpto;
	received_tli = WalRcv->receivedTLI;
	last_send_time = WalRcv->lastMsgSendTime;
	last_receipt_time = WalRcv->lastMsgReceiptTime;
	latest_end_lsn = WalRcv->latestWalEnd;
	latest_end_time = WalRcv->latestWalEndTime;
	strlcpy(slotname, (char *) WalRcv->slotname, sizeof(slotname));
	strlcpy(sender_host, (char *) WalRcv->sender_host, sizeof(sender_host));
	sender_port = WalRcv->sender_port;
	strlcpy(conninfo, (char *) WalRcv->conninfo, sizeof(conninfo));
	SpinLockRelease(&WalRcv->mutex);

	/*
	 * No WAL receiver (or not ready yet), just return a tuple with NULL
	 * values
	 */
	if (pid == 0 || !ready_to_display)
		PG_RETURN_NULL();

	/*
	 * Read "writtenUpto" without holding a spinlock.  Note that it may not be
	 * consistent with the other shared variables of the WAL receiver
	 * protected by a spinlock, but this should not be used for data integrity
	 * checks.
	 */
	written_lsn = pg_atomic_read_u64(&WalRcv->writtenUpto);

	/* determine result type */
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	values = palloc0(sizeof(Datum) * tupdesc->natts);
	nulls = palloc0(sizeof(bool) * tupdesc->natts);

	/* Fetch values */
	values[0] = Int32GetDatum(pid);

	if (!has_privs_of_role(GetUserId(), ROLE_PG_READ_ALL_STATS))
	{
		/*
		 * Only superusers and roles with privileges of pg_read_all_stats can
		 * see details. Other users only get the pid value to know whether it
		 * is a WAL receiver, but no details.
		 */
		MemSet(&nulls[1], true, sizeof(bool) * (tupdesc->natts - 1));
	}
	else
	{
		values[1] = CStringGetTextDatum(WalRcvGetStateString(state));

		if (XLogRecPtrIsInvalid(receive_start_lsn))
			nulls[2] = true;
		else
			values[2] = LSNGetDatum(receive_start_lsn);
		values[3] = Int32GetDatum(receive_start_tli);
		if (XLogRecPtrIsInvalid(written_lsn))
			nulls[4] = true;
		else
			values[4] = LSNGetDatum(written_lsn);
		if (XLogRecPtrIsInvalid(flushed_lsn))
			nulls[5] = true;
		else
			values[5] = LSNGetDatum(flushed_lsn);
		values[6] = Int32GetDatum(received_tli);
		if (last_send_time == 0)
			nulls[7] = true;
		else
			values[7] = TimestampTzGetDatum(last_send_time);
		if (last_receipt_time == 0)
			nulls[8] = true;
		else
			values[8] = TimestampTzGetDatum(last_receipt_time);
		if (XLogRecPtrIsInvalid(latest_end_lsn))
			nulls[9] = true;
		else
			values[9] = LSNGetDatum(latest_end_lsn);
		if (latest_end_time == 0)
			nulls[10] = true;
		else
			values[10] = TimestampTzGetDatum(latest_end_time);
		if (*slotname == '\0')
			nulls[11] = true;
		else
			values[11] = CStringGetTextDatum(slotname);
		if (*sender_host == '\0')
			nulls[12] = true;
		else
			values[12] = CStringGetTextDatum(sender_host);
		if (sender_port == 0)
			nulls[13] = true;
		else
			values[13] = Int32GetDatum(sender_port);
		if (*conninfo == '\0')
			nulls[14] = true;
		else
			values[14] = CStringGetTextDatum(conninfo);
	}

	/* Returns the record as Datum */
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}

/* POLAR */

/* Set new consistent lsn received from primary node */
void
polar_set_primary_consistent_lsn(XLogRecPtr new_consistent_lsn)
{
	/*
	 * Called by the startup process once (at consistency) and then by the WAL
	 * receiver; the two never overlap, so a plain atomic write is safe.
	 */
	if (pg_atomic_read_u64(&WalRcv->curr_primary_consistent_lsn) < new_consistent_lsn)
		pg_atomic_write_u64(&WalRcv->curr_primary_consistent_lsn, new_consistent_lsn);
}

/*
 * Primary node send the consistent lsn by walsender process,
 * replica walreceiver process receives consistent lsn, and saves
 * it in WalRcv->curr_primary_consistent_lsn
 */
XLogRecPtr
polar_get_primary_consistent_lsn(void)
{
	return pg_atomic_read_u64(&WalRcv->curr_primary_consistent_lsn);
}

/*
 * POLAR: get lastMsgReceiptTime
 */
TimestampTz
polar_get_walrcv_last_msg_receipt_time(void)
{
	WalRcvData *walrcv = WalRcv;
	TimestampTz last_msg_receipt_time = 0;

	SpinLockAcquire(&walrcv->mutex);
	last_msg_receipt_time = walrcv->lastMsgReceiptTime;
	SpinLockRelease(&walrcv->mutex);
	return last_msg_receipt_time;
}

/*
 * POLAR: Judge whether a promote request must be sent to the walsender.
 *
 * 1) if promote is triggered in the current instance, send a request when
 *    polar_enable_promote_wait_for_walreceive_done = on and the walreceiver
 *    received the promote trigger;
 * 2) if promote is triggered in a downstream instance, send a request when
 *    the walsender received a promote trigger from downstream;
 * 3) at last, re-send the request when we haven't received the promote reply
 *    from the walsender after the timeout.
 *
 * Returns true if it is necessary to send a request to the walsender.
 */
bool
polar_send_promote_request(void)
{
	static TimestampTz last_send_time = 0;

	/* walrcv already exists when this function is called */
	if (((polar_enable_promote_wait_for_walreceive_done && POLAR_PROMOTE_IS_TRIGGERED()) ||
		 POLAR_WALSNDCTL_RECEIVE_PROMOTE_TRIGGER()) &&
		!POLAR_PROMOTE_REPLY_IS_RECEIVED())
	{
		TimestampTz send_now = GetCurrentTimestamp();

		if (TimestampDifferenceExceeds(last_send_time, send_now, POLAR_SEND_PROMOTE_REQUEST_TIMEOUT))
		{
			last_send_time = send_now;
			return true;
		}
	}
	if (POLAR_PROMOTE_REPLY_IS_RECEIVED())
		last_send_time = 0;

	return false;
}

/* POLAR: send promote information to the walsender */
void
polar_walrcv_send_promote(bool polar_request_reply)
{
	bool		polar_promote_trigger = true;

	resetStringInfo(&reply_message);
	pq_sendbyte(&reply_message, 'p');
	pq_sendbyte(&reply_message, polar_promote_trigger);
	pq_sendbyte(&reply_message, polar_request_reply ? 1 : 0);
	elog(LOG, "send promote trigger %d, polar_request_reply:%d",
		 polar_promote_trigger, polar_request_reply);
	walrcv_send(wrconn, reply_message.data, reply_message.len);
}

/* POLAR: process a promote reply received from the walsender */
void
polar_process_walsender_reply(bool is_promote_allowed, XLogRecPtr end_lsn)
{
	Assert(WalRcv);

	/* already received and processed the reply */
	if (POLAR_PROMOTE_REPLY_IS_RECEIVED())
		return;

	/* promote is allowed */
	if (is_promote_allowed)
	{
		if (!XLogRecPtrIsInvalid(end_lsn))
			POLAR_SET_END_LSN(end_lsn);
	}
	/* disable promote */
	else
		POLAR_SET_PROMOTE_NOT_ALLOWED();

	/* having received the reply, don't send the promote request again */
	POLAR_SET_RECEIVE_PROMOTE_REPLY();

	/* tell the walsender we received the reply, so it won't reply again */
	polar_walrcv_send_promote(false);
}

/* POLAR: get end_lsn when a promote request is received from downstream */
XLogRecPtr
polar_promote_get_end_lsn(void)
{
	XLogRecPtr	end_lsn = InvalidXLogRecPtr;

	end_lsn = pg_atomic_read_u64(&WalRcv->polar_latest_flush_lsn);
	/* polar_latest_flush_lsn is 0 when datamax/standby restart and no stream */
	if (XLogRecPtrIsInvalid(end_lsn))
	{
		if (polar_is_datamax())
			end_lsn = polar_datamax_get_last_received_lsn(polar_datamax_ctl, NULL);
		else
			end_lsn = GetXLogReplayRecPtr(NULL);
	}
	return end_lsn;
}

/*
 * POLAR: judge whether the upstream node is alive via the WAL stream.
 * Returns true when the upstream can be connected to, i.e. the walreceiver
 * is ready.
 */
bool
polar_upstream_node_is_alive(void)
{
	int			pid = 0;
	bool		ready_to_display = false;

	Assert(WalRcv);

	SpinLockAcquire(&WalRcv->mutex);
	pid = (int) WalRcv->pid;
	ready_to_display = WalRcv->ready_to_display;
	SpinLockRelease(&WalRcv->mutex);

	return (pid != 0 && ready_to_display);
}

/*
 * POLAR: judge whether all WAL has been received; if so, set
 * polar_is_promote_allowed = true to indicate that promote can be executed.
 */
void
polar_promote_check_received_all_wal(void)
{
	Assert(WalRcv);

	if (!POLAR_IS_PROMOTE_NOT_ALLOWED() &&
		!POLAR_IS_END_LSN_INVALID() &&
		!POLAR_IS_PROMOTE_ALLOWED() &&
		pg_atomic_read_u64(&WalRcv->polar_end_lsn) == LogstreamResult.Flush)
	{
		elog(LOG, "polar_end_lsn:%X/%X, flush_lsn:%X/%X, received all wal, promote is allowed",
			 LSN_FORMAT_ARGS(pg_atomic_read_u64(&WalRcv->polar_end_lsn)),
			 LSN_FORMAT_ARGS(LogstreamResult.Flush));
		POLAR_SET_PROMOTE_ALLOWED();
		/* wake the startup process so it re-checks the promote trigger */
		WakeupRecovery();
	}
}

/*
 * POLAR: This is callback function used when waiting free space from
 * polar_xlog_queue.It will send feedback and handle interrupts
 */
static void
polar_receiver_xlog_queue_callback(polar_ringbuf_t rbuf)
{
	ProcessWalRcvInterrupts();
	XLogWalRcvSendReply(false, false);
	XLogWalRcvSendHSFeedback(false);
}

static inline void
polar_recv_push_storage_begin_callback(polar_ringbuf_t rbuf)
{
	ProcessWalRcvInterrupts();
}

static void
polar_notify_read_wal_file(int code, Datum arg)
{
	/*
	 * POLAR: The wal receiver is exiting, tell startup to read from file if
	 * it want to read more xlog.
	 */
	if (!ShutdownRequestPending && polar_is_replica() && polar_logindex_redo_instance)
	{
		elog(LOG, "PolarDB replica exit wal receiver and request to read from WAL file");
		polar_xlog_recv_queue_push_storage_begin(polar_logindex_redo_instance->xlog_queue, polar_recv_push_storage_begin_callback);
	}
}
