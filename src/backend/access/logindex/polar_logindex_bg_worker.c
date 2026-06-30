/*-------------------------------------------------------------------------
 *
 * polar_logindex_bg_worker.c
 *	  logindex background writer and saver bgworker
 *
 * Copyright (c) 2024, Alibaba Group Holding Limited
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
 *	  src/backend/access/logindex/polar_logindex_bg_worker.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>

#include "access/polar_logindex_redo.h"
#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "postmaster/auxprocess.h"
#include "postmaster/bgworker.h"
#include "postmaster/walwriter.h"
#include "storage/condition_variable.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/procarray.h"
#include "utils/backend_status.h"
#include "utils/guc.h"
#include "utils/injection_point.h"
#include "utils/memutils.h"
#include "utils/resowner.h"
#include "utils/timeout.h"
#include "utils/wait_event.h"

/* state for polar_logindex_saver_wakeup_req */
bool		polar_wake_logindex_saver = false;

/*
 * Flags set by interrupt handlers for later service in the main loop.
 */
static volatile sig_atomic_t polar_online_promote_req = false;

/* Signal handlers */

/* POLAR: SIGUSR2 : used for online promote */
static void
online_promote_trigger(SIGNAL_ARGS)
{
	int			save_errno = errno;

	polar_online_promote_req = true;
	SetLatch(MyLatch);

	errno = save_errno;
}


static void
logindex_worker_handle_online_promote(polar_logindex_redo_ctl_t instance)
{
	/*
	 * Only run polar_replica_promote_logindex when promoting replica.
	 *
	 * Standby promoting doesn't need it to change logindex state, which is
	 * originally writable.
	 */
	if (instance && instance->fullpage_logindex_snapshot)
		polar_replica_promote_logindex(instance->fullpage_logindex_snapshot, true);

	if (instance && instance->wal_logindex_snapshot)
		polar_replica_promote_logindex(instance->wal_logindex_snapshot, false);

	elog(LOG, "Before online promote bg_replayed_lsn=%X/%X",
		 LSN_FORMAT_ARGS(polar_get_bg_replayed_lsn(instance)));

	polar_set_bg_redo_state(instance, POLAR_BG_WAITING_RESET);

	/* POLAR: Notify startup process that background replay state is changed */
	WakeupRecovery();
}

static void
set_logindex_bg_worker_latch(polar_logindex_redo_ctl_t instance)
{
	POLAR_ASSERT_PANIC(MyLatch);

	instance->bg_worker_latch = MyLatch;
	polar_logindex_set_bgworker_latch(instance->wal_logindex_snapshot, MyLatch);
}

static polar_logindex_bg_redo_ctl_t *
create_logindex_bg_redo_ctl(polar_logindex_redo_ctl_t instance)
{
	polar_logindex_bg_redo_ctl_t *bg_redo_ctl = NULL;
	uint32		state;

	if (!polar_need_do_bg_replay(instance))
		return bg_redo_ctl;

	state = polar_get_bg_redo_state(instance);

	switch (state)
	{
		case POLAR_BG_REPLICA_BUF_REPLAYING:
			bg_redo_ctl = polar_create_bg_redo_ctl(instance, false);
			polar_bg_replaying_process = POLAR_LOGINDEX_DISPATCHER;
			break;

		case POLAR_BG_PARALLEL_REPLAYING:
		case POLAR_BG_ONLINE_PROMOTE:
			bg_redo_ctl = polar_create_bg_redo_ctl(instance, true);
			polar_bg_replaying_process = POLAR_LOGINDEX_DISPATCHER;
			break;

		case POLAR_BG_REDO_NOT_START:
		case POLAR_BG_WAITING_RESET:
			break;

		default:
			elog(PANIC, "Got unexpected bg_redo_state=%d", state);
	}

	return bg_redo_ctl;
}

static bool
parallel_replay_should_exit(polar_logindex_redo_ctl_t instance)
{
	uint32		state = polar_get_bg_redo_state(instance);

	/*
	 * During an online promote, replay_done alone is not enough to tell that
	 * the promote has finished: it only means the latest dispatch pass
	 * drained, not that bg_redo_state has advanced to POLAR_BG_REDO_NOT_START
	 * (which additionally requires backend_min_replay_lsn to be invalid, see
	 * polar_logindex_bg_online_promote()). Exiting while the state is still
	 * POLAR_BG_ONLINE_PROMOTE leaves bg_redo_state stuck forever, so
	 * polar_cal_cur_consistent_lsn() keeps returning the now-frozen
	 * replayed_oldest_lsn instead of polar_max_valid_lsn(), and the shutdown
	 * checkpoint hangs waiting for consistent_lsn to reach checkpoint.redo.
	 * So keep looping until the promote is fully done.
	 */
	if (state == POLAR_BG_ONLINE_PROMOTE)
		return false;

	/*
	 * Otherwise make sure the rule that the parallel replay workers exit
	 * after the startup process: while a standby is still replaying in
	 * parallel and the startup process is alive, keep looping so the workers
	 * outlive it.
	 */
	if (state == POLAR_BG_PARALLEL_REPLAYING &&
		POLAR_STARTUP_IN_STATUS(instance, POLAR_STARTUP_RUNNING))
		return false;

	return true;
}

void
polar_logindex_bg_worker_main(char *startup_data, size_t startup_data_len)
{
	/*
	 * POLAR: Flags check whether finish online promote to avoid receive more
	 * than one times of SIGUSR2
	 */
	bool		polar_online_promoting = false;
	polar_logindex_bg_redo_ctl_t *bg_redo_ctl = NULL;
	MemoryContext bgworker_context;

	Assert(startup_data_len == 0);

	MyBackendType = B_BG_LOGINDEX;
	AuxiliaryProcessMainCommon();

	/*
	 * Properly accept or ignore signals the postmaster might send us.
	 *
	 * bgwriter doesn't participate in ProcSignal signalling, but a SIGUSR1
	 * handler is still needed for latch wakeups.
	 */
	pqsignal(SIGHUP, SignalHandlerForConfigReload); /* reload config */
	pqsignal(SIGINT, SIG_IGN);
	pqsignal(SIGTERM, SignalHandlerForShutdownRequest); /* shutdown */
	pqsignal(SIGQUIT, SignalHandlerForCrashExit);	/* hard crash time */
	pqsignal(SIGALRM, SIG_IGN);
	pqsignal(SIGPIPE, SIG_IGN);
	pqsignal(SIGUSR1, procsignal_sigusr1_handler);
	pqsignal(SIGUSR2, online_promote_trigger);

	/*
	 * Reset some signals that are accepted by postmaster but not here
	 */
	pqsignal(SIGCHLD, SIG_DFL);
	pqsignal(SIGTTIN, SIG_DFL);
	pqsignal(SIGTTOU, SIG_DFL);
	pqsignal(SIGCONT, SIG_DFL);
	pqsignal(SIGWINCH, SIG_DFL);

	/*
	 * POLAR: Establishes SIGALRM handler and initialize parameters to
	 * facilitate the running of scheduled tasks. Some scheduled tasks will
	 * cause assertion errors when parameters are not initialized.
	 */
	InitializeTimeouts();

	/* We allow SIGQUIT (quickdie) at all times */
	sigdelset(&BlockSig, SIGQUIT);

	/*
	 * Create a resource owner to keep track of our resources (currently only
	 * buffer pins).
	 */
	CurrentResourceOwner = ResourceOwnerCreate(NULL, "LogindexBgWorker");

	/*
	 * Create a memory context that we will do all our work in.  We do this so
	 * that we can reset the context during error recovery and thereby avoid
	 * possible memory leaks.  Formerly this code just ran in
	 * TopMemoryContext, but resetting that would be a really bad idea.
	 */
	bgworker_context = AllocSetContextCreate(TopMemoryContext,
											 "LogIndex background worker",
											 ALLOCSET_DEFAULT_SIZES);
	MemoryContextSwitchTo(bgworker_context);

	set_logindex_bg_worker_latch(polar_logindex_redo_instance);

	/*
	 * Unblock signals (they were blocked when the postmaster forked us)
	 */
	sigprocmask(SIG_SETMASK, &UnBlockSig, NULL);

	elog(LOG, "Start logindex background worker");

	for (;;)
	{
		bool		replay_done = true;
		bool		flush_done = true;
		bool		can_hold = false;
		long		timeout = -1;

		/* Clear any already-pending wakeups */
		ResetLatch(MyLatch);

		if (ProcSignalBarrierPending)
			ProcessProcSignalBarrier();

		if (ConfigReloadPending)
		{
			ConfigReloadPending = false;
			ProcessConfigFile(PGC_SIGHUP);
		}

		/* Perform logging of memory contexts of this process */
		if (LogMemoryContextPending)
			ProcessLogMemoryContextInterrupt();

		/*
		 * Only do online pomote when current node is replica and startup is
		 * ready.
		 */
		if (unlikely(polar_online_promote_req) &&
			polar_get_bg_redo_state(polar_logindex_redo_instance) == POLAR_BG_REPLICA_BUF_REPLAYING)
		{
			if (!polar_online_promoting)
			{
				/*
				 * We release background redo control, and recreate it when
				 * startup process reset background replay lsn
				 */
				if (bg_redo_ctl)
				{
					polar_release_bg_redo_ctl(bg_redo_ctl);
					bg_redo_ctl = NULL;
				}

				logindex_worker_handle_online_promote(polar_logindex_redo_instance);
				polar_online_promoting = true;

				INJECTION_POINT("polar_delay_wal_replay");
			}

			polar_online_promote_req = false;
		}

		if (unlikely(!bg_redo_ctl))
			bg_redo_ctl = create_logindex_bg_redo_ctl(polar_logindex_redo_instance);

		if (bg_redo_ctl)
		{
			replay_done = polar_logindex_redo_bg_replay(bg_redo_ctl, &can_hold, MyLatch);

			/*
			 * Release background redo control when online promote or instant
			 * recovery is finished
			 */
			if (unlikely(polar_logindex_finished_bg_replay(bg_redo_ctl->instance)))
			{
				polar_release_bg_redo_ctl(bg_redo_ctl);
				bg_redo_ctl = NULL;
				if (polar_online_promoting)
					polar_online_promoting = false;
			}
		}

		/*
		 * POLAR: Flush log index memory table which is full
		 */
		flush_done = polar_logindex_redo_bg_flush_data(polar_logindex_redo_instance);

		if (unlikely(ShutdownRequestPending))
		{
			/*
			 * The logindex worker and parallel replay subprocs should exit
			 * after online promote is finished in primary node during online
			 * promote, and after the startup process exited in parallel
			 * replay mode. Both conditions are checked by
			 * parallel_replay_should_exit().
			 */
			if (polar_is_replica() ||
				(replay_done && parallel_replay_should_exit(polar_logindex_redo_instance)))
			{
				if (bg_redo_ctl)
					polar_release_bg_redo_ctl(bg_redo_ctl);

				/* Exit this process */
				break;
			}
		}

		if (flush_done && (replay_done || can_hold))
			timeout = 10;

		if (timeout > 0)
		{
			WaitLatch(MyLatch,
					  WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
					  timeout /* ms */ , WAIT_EVENT_LOGINDEX_BG_MAIN);
		}
	}

	elog(LOG, "Exit logindex background worker");

	/*
	 * From here on, elog(ERROR) should end with exit(1), not send control
	 * back to the sigsetjmp block above
	 */
	ExitOnAnyError = true;
	proc_exit(0);
}

void
polar_logindex_saver_main(Datum main_arg)
{
	MemoryContext bgworker_context;

	/*
	 * Properly accept or ignore signals the postmaster might send us
	 */
	pqsignal(SIGHUP, SignalHandlerForConfigReload); /* reload config */
	pqsignal(SIGINT, SIG_IGN);
	pqsignal(SIGTERM, SignalHandlerForShutdownRequest); /* shutdown */
	pqsignal(SIGUSR1, procsignal_sigusr1_handler);

	/*
	 * SIGQUIT handler was already set up by InitPostmasterChild. Reset some
	 * signals that are accepted by postmaster but not here
	 */
	pqsignal(SIGCHLD, SIG_DFL);
	pqsignal(SIGTTIN, SIG_DFL);
	pqsignal(SIGTTOU, SIG_DFL);
	pqsignal(SIGCONT, SIG_DFL);
	pqsignal(SIGWINCH, SIG_DFL);

	/* Add saver into backends for showing them in pg_stat_activity */
	polar_init_dynamic_bgworker_in_backends();

	/*
	 * Create a resource owner to keep track of our resources (currently only
	 * buffer pins).
	 */
	CurrentResourceOwner = ResourceOwnerCreate(NULL, "LogindexSaver");

	/*
	 * Create a memory context that we will do all our work in.  We do this so
	 * that we can reset the context during error recovery and thereby avoid
	 * possible memory leaks.  Formerly this code just ran in
	 * TopMemoryContext, but resetting that would be a really bad idea.
	 */
	bgworker_context = AllocSetContextCreate(TopMemoryContext,
											 "LogIndex saver",
											 ALLOCSET_DEFAULT_SIZES);
	MemoryContextSwitchTo(bgworker_context);

	/*
	 * Unblock signals (they were blocked when the postmaster forked us)
	 */
	BackgroundWorkerUnblockSignals();

	/*
	 * Advertise our latch that backends can use to wake us up while we're
	 * sleeping.
	 */
	POLAR_ASSERT_PANIC(polar_logindex_redo_instance && polar_logindex_redo_instance->xlog_queue);
	polar_logindex_redo_instance->logindex_saver_latch = &MyProc->procLatch;

	pgstat_report_appname("polar logindex saver");

	elog(LOG, "Start polar logindex saver");

	/*
	 * Loop forever
	 */
	for (;;)
	{
		long		timeout;

		timeout = WalWriterDelay;

		/* Clear any already-pending wakeups */
		ResetLatch(MyLatch);

		HandleMainLoopInterrupts();

		if (timeout > 0)
		{
			WaitLatch(MyLatch,
					  WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
					  timeout /* ms */ , WAIT_EVENT_LOGINDEX_SAVER_MAIN);
		}

		/* POLAR: Save logindex data */
		polar_logindex_primary_save(polar_logindex_redo_instance);
	}

	elog(LOG, "Exit polar logindex saver");
}

void
polar_register_logindex_primary_saver(void)
{
	BackgroundWorker worker;

	if (polar_logindex_mem_size <= 0 ||
		polar_xlog_queue_buffers < 0)
		return;

	MemSet(&worker, 0, sizeof(BackgroundWorker));

	/*
	 * POLAR: The saver process will only be started when the current node is
	 * in rw mode. It will not be initiated on ro and standby nodes. When a ro
	 * or standby node is promoted to rw mode, the saver process will be
	 * started once the postmaster state reaches PM_RUN.
	 */
	worker.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_CRASH_ON_ERROR;
	worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
	snprintf(worker.bgw_library_name, BGW_MAXLEN, "postgres");
	snprintf(worker.bgw_function_name, BGW_MAXLEN, "polar_logindex_saver_main");
	snprintf(worker.bgw_name, BGW_MAXLEN, "polar logindex saver");
	snprintf(worker.bgw_type, BGW_MAXLEN, "polar logindex saver");
	worker.bgw_restart_time = 3;
	worker.bgw_notify_pid = 0;
	RegisterBackgroundWorker(&worker);
	return;
}
