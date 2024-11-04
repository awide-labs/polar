/*-------------------------------------------------------------------------
 *
 * polar_parallel_bgwriter.c
 *	  Parallel background writer process manager.
 *
 * Copyright (c) 2022, Alibaba Group Holding Limited
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
 *	  src/backend/postmaster/polar_parallel_bgwriter.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "miscadmin.h"
#include "pgstat.h"

#include "access/xlog.h"
#include "libpq/pqsignal.h"
#include "postmaster/bgworker.h"
#include "postmaster/bgwriter.h"
#include "postmaster/interrupt.h"
#include "postmaster/polar_parallel_bgwriter.h"
#include "replication/walsender.h"
#include "storage/buf_internals.h"
#include "storage/condition_variable.h"
#include "storage/fd.h"
#include "storage/latch.h"
#include "storage/pmsignal.h"
#include "storage/polar_bufmgr.h"
#include "storage/polar_fd.h"
#include "storage/polar_flush.h"
#include "storage/procsignal.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/polar_log.h"
#include "utils/timeout.h"
#include "utils/timestamp.h"

#define HIBERNATE_FACTOR 50
#define ILLEGAL_GENERATION 0
#define ILLEGAL_SLOT -1

static int	find_one_unused_handle_slot(void);
static int	find_one_idle_used_handle_slot(void);
static parallel_flush_type_t polar_get_flush_task(void);

ParallelBgwriterInfo *polar_parallel_bgwriter_info = NULL;

static bool
handle_is_unused(BackgroundWorkerHandle *handle)
{
	bool		res = handle->slot == ILLEGAL_SLOT &&
		handle->generation == ILLEGAL_GENERATION;

	return res;
}

static bool
handle_is_used(BackgroundWorkerHandle *handle)
{
	return handle->slot != ILLEGAL_SLOT &&
		handle->generation != ILLEGAL_GENERATION;
}

static void
reset_bgwriter_handle(BackgroundWorkerHandle *handle)
{
	handle->slot = ILLEGAL_SLOT;
	handle->generation = ILLEGAL_GENERATION;
}

/*
 * Initialize parallel bgwriter manager.
 *
 * This is called once during shared-memory initialization (either in the
 * postmaster, or in a standalone backend).
 */
void
polar_init_parallel_bgwriter(void)
{
	bool		found;
	int			i;

	if (!polar_parallel_bgwriter_enabled())
		return;

	polar_parallel_bgwriter_info = (ParallelBgwriterInfo *) ShmemInitStruct(
																			"Parallel bgwriter info",
																			sizeof(ParallelBgwriterInfo),
																			&found);

	if (!found)
	{
		/* Init background worker handle */
		for (i = 0; i < MAX_NUM_OF_PARALLEL_BGWRITER; i++)
		{
			polar_parallel_bgwriter_info->polar_flush_work_info[i].latch = NULL;
			reset_bgwriter_handle(&polar_parallel_bgwriter_info->polar_flush_work_info[i].handle);
			pg_atomic_init_u32(&polar_parallel_bgwriter_info->polar_flush_work_info[i].state, FLUSH_IDLE);
			polar_parallel_bgwriter_info->flush_task[i] = INVALID_TASK;
			polar_parallel_bgwriter_info->polar_flush_work_info[i].task = INVALID_TASK;
		}

		SpinLockInit(&polar_parallel_bgwriter_info->lock);

		for (i = 0; i < MAX_FLUSH_TASK; i++)
			pg_atomic_init_u32(&polar_parallel_bgwriter_info->flush_workers[i], 0);

		pg_atomic_init_u32(&polar_parallel_bgwriter_info->current_workers, 0);
		polar_parallel_bgwriter_info->read_worker_idx = 0;
		polar_parallel_bgwriter_info->write_worker_idx = 0;
	}
}

Size
polar_parallel_bgwriter_shmem_size(void)
{
	Size		size = 0;

	if (!polar_parallel_bgwriter_enabled())
		return size;

	size = add_size(size, sizeof(ParallelBgwriterInfo));

	return size;
}

/* Start parallel background writer workers. */
void
polar_launch_parallel_bgwriter_workers(void)
{
	uint32		current_workers;
	int			launch_workers = 0;
	int			guc_workers;

	if (!polar_parallel_bgwriter_enabled())
		return;

	if ((polar_is_primary_in_recovery() &&
		 !polar_enable_early_launch_parallel_bgwriter) ||
		polar_is_replica() || polar_is_standby())
		return;

	Assert(MyBackendType == B_BG_WRITER);

	current_workers = CURRENT_PARALLEL_WORKERS;
	guc_workers = polar_parallel_flush_workers;

	if (current_workers < guc_workers)
		launch_workers = guc_workers - current_workers;
	else if (current_workers > POLAR_MAX_BGWRITER_WORKERS)
	{
		launch_workers = POLAR_MAX_BGWRITER_WORKERS - current_workers;
		elog(LOG,
			 "Current parallel background workers %d is greater than the max %d, try to stop %d worker.",
			 current_workers, POLAR_MAX_BGWRITER_WORKERS, abs(launch_workers));
	}

	if (launch_workers > 0)
		polar_register_parallel_bgwriter_workers(launch_workers);
	else if (launch_workers < 0)
		polar_shutdown_parallel_bgwriter_workers(abs(launch_workers));
}

/* Register #workers background writer workers. */
void
polar_register_parallel_bgwriter_workers(int workers)
{
	BackgroundWorker worker;
	uint32		current_workers;
	int			slot;
	int			i;

	Assert(polar_parallel_bgwriter_enabled());
	Assert(MyBackendType == B_BG_WRITER);

	memset(&worker, 0, sizeof(BackgroundWorker));
	worker.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_CRASH_ON_ERROR;
	worker.bgw_start_time = BgWorkerStart_PostmasterStart;
	worker.bgw_restart_time = BGW_NEVER_RESTART;
	sprintf(worker.bgw_library_name, "postgres");
	sprintf(worker.bgw_function_name, "polar_parallel_bgwriter_worker_main");
	snprintf(worker.bgw_name, BGW_MAXLEN, "polar parallel bgwriter");
	snprintf(worker.bgw_type, BGW_MAXLEN, "polar parallel bgwriter");
	worker.bgw_notify_pid = MyProcPid;

	current_workers = CURRENT_PARALLEL_WORKERS;

	/* Start workers */
	for (i = 0; i < workers; i++)
	{
		BgwHandleStatus status;
		pid_t		pid;
		BackgroundWorkerHandle *worker_handle;

		/* Check the total number of parallel background workers. */
		if (current_workers >= POLAR_MAX_BGWRITER_WORKERS)
		{
			ereport(WARNING,
					(errmsg(
							"Can not start new parallel background workers, current workers: %d, at most workers %d",
							current_workers,
							POLAR_MAX_BGWRITER_WORKERS)));
			break;
		}

		/* Find an empty handle slot to register one background writer. */
		slot = find_one_unused_handle_slot();
		if (slot == -1)
		{
			ereport(WARNING,
					(errmsg(
							"Can not find an empty background worker handle slot to register a worker, total number of workers is %d",
							current_workers)));
			break;
		}

		worker.bgw_main_arg = Int32GetDatum(slot);
		if (!RegisterDynamicBackgroundWorker(&worker, &worker_handle))
		{
			ereport(WARNING,
					(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
					 errmsg("could not register background process"),
					 errhint("You may need to increase max_worker_processes.")));
			continue;
		}

		status = WaitForBackgroundWorkerStartup(worker_handle, &pid);
		if (status != BGWH_STARTED)
		{
			ereport(WARNING,
					(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
					 errmsg("could not start background process"),
					 errhint("More details may be available in the server log.")));

			continue;
		}

		polar_parallel_bgwriter_info->polar_flush_work_info[slot].handle = *worker_handle;
		pfree(worker_handle);
		pg_atomic_fetch_add_u32(&polar_parallel_bgwriter_info->current_workers, 1);

		if (polar_enable_debug)
			ereport(LOG,
					(errmsg(
							"Register one background writer worker, its handle slot index is %d",
							slot)));
	}
}

/*
 * polar_parallel_bgwriter_worker_main - main entry point for the parallel
 * bgwriter worker.
 *
 * Based on BackgroundWriterMain.
 */
void
polar_parallel_bgwriter_worker_main(Datum main_arg)
{
	sigjmp_buf	local_sigjmp_buf;
	MemoryContext bgwriter_context;
	bool		prev_hibernate;
	WritebackContext wb_context;
	int			slot = DatumGetInt32(main_arg);

	/*
	 * Properly accept or ignore signals that might be sent to us.
	 */
	pqsignal(SIGHUP, SignalHandlerForConfigReload);
	pqsignal(SIGINT, SIG_IGN);
	pqsignal(SIGTERM, SignalHandlerForShutdownRequest);
	/* SIGQUIT handler was already set up by InitPostmasterChild */
	pqsignal(SIGALRM, SIG_IGN);
	pqsignal(SIGPIPE, SIG_IGN);
	pqsignal(SIGUSR1, procsignal_sigusr1_handler);
	pqsignal(SIGUSR2, SIG_IGN);

	/*
	 * Reset some signals that are accepted by postmaster but not here
	 */
	pqsignal(SIGCHLD, SIG_DFL);

	/* We are also bgwriter, for stat use */
	MyAuxProcType = BgWriterProcess;

	/* Add bg writer into backends for showing them in pg_stat_activity */
	polar_init_dynamic_bgworker_in_backends();

	/*
	 * Create a memory context that we will do all our work in.  We do this so
	 * that we can reset the context during error recovery and thereby avoid
	 * possible memory leaks.  Formerly this code just ran in
	 * TopMemoryContext, but resetting that would be a really bad idea.
	 */
	bgwriter_context = AllocSetContextCreate(TopMemoryContext,
											 "Parallel Background Writer",
											 ALLOCSET_DEFAULT_SIZES);
	MemoryContextSwitchTo(bgwriter_context);

	WritebackContextInit(&wb_context, &bgwriter_flush_after);

	/*
	 * create resowner before sigsetjmp to avoid recreate error when exception
	 * is encountered.
	 */
	CreateAuxProcessResourceOwner();

	/*
	 * If an exception is encountered, processing resumes here.
	 *
	 * See notes in bgwriter.c about the design of this coding.
	 */
	if (sigsetjmp(local_sigjmp_buf, 1) != 0)
	{
		/* Since not using PG_TRY, must reset error stack by hand */
		error_context_stack = NULL;

		/* Prevent interrupts while cleaning up */
		HOLD_INTERRUPTS();

		/* Report the error to the server log */
		EmitErrorReport();

		/*
		 * These operations are really just a minimal subset of
		 * AbortTransaction().  We don't have very many resources to worry
		 * about in bgwriter, but we do have LWLocks, buffers, and temp files.
		 */
		LWLockReleaseAll();
		ConditionVariableCancelSleep();
		UnlockBuffers();
		ReleaseAuxProcessResources(false);
		AtEOXact_Buffers(false);
		AtEOXact_SMgr();
		AtEOXact_Files(false);
		AtEOXact_HashTables(false);

		/*
		 * Now return to normal top-level context and clear ErrorContext for
		 * next time.
		 */
		MemoryContextSwitchTo(bgwriter_context);
		FlushErrorState();

		/* Flush any leaked data in the top-level context */
		MemoryContextResetAndDeleteChildren(bgwriter_context);

		/* re-initialize to avoid repeated errors causing problems */
		WritebackContextInit(&wb_context, &bgwriter_flush_after);

		/* Now we can allow interrupts again */
		RESUME_INTERRUPTS();

		/*
		 * Sleep at least 1 second after any error.  A write error is likely
		 * to be repeated, and we don't want to be filling the error logs as
		 * fast as we can.
		 */
		pg_usleep(1000000L);

		/*
		 * Close all open files after any error.  This is helpful on Windows,
		 * where holding deleted files open causes various strange errors.
		 * It's not clear we need it elsewhere, but shouldn't hurt.
		 */
		smgrcloseall();

		/* Report wait end here, when there is no further possibility of wait */
		pgstat_report_wait_end();
	}

	/* We can now handle ereport(ERROR) */
	PG_exception_stack = &local_sigjmp_buf;

	/*
	 * Unblock signals (they were blocked when the postmaster forked us)
	 */
	PG_SETMASK(&UnBlockSig);

	/*
	 * Reset hibernation state after any error.
	 */
	prev_hibernate = false;

	/*
	 * set worker latch
	 */
	POLAR_ASSERT_PANIC(slot > -1 && slot < MAX_NUM_OF_PARALLEL_BGWRITER);
	polar_parallel_bgwriter_info->polar_flush_work_info[slot].latch = MyLatch;

	/*
	 * Loop forever
	 */
	for (;;)
	{
		XLogRecPtr	consistent_lsn = InvalidXLogRecPtr;
		parallel_flush_type_t task_type = INVALID_TASK;
		bool		can_hibernate;
		int			rc;

		/* Clear any already-pending wakeups */
		ResetLatch(MyLatch);

		HandleMainLoopInterrupts();

		if (polar_enable_flush_dispatcher &&
			pg_atomic_read_u32(&polar_parallel_bgwriter_info->polar_flush_work_info[slot].state) == FLUSH_WORK)
			task_type = polar_parallel_bgwriter_info->polar_flush_work_info[slot].task;
		else
		{
			pg_atomic_write_u32(&polar_parallel_bgwriter_info->polar_flush_work_info[slot].state, FLUSH_WORK);

			/* Get flush task */
			if (polar_enable_flush_dispatcher)
				task_type = polar_get_flush_task();
			else
				task_type = FLUSHLIST_TASK;

			polar_parallel_bgwriter_info->polar_flush_work_info[slot].task = task_type;
		}

		switch (task_type)
		{
				/*
				 * Do one cycle of flushlist dirty-buffer writing.
				 *
				 * It's no need for parallel bgwriter to delay when flush
				 * dispatcher is enabled. The parallel bgwriter won't flush
				 * frequently because flush tasks is dispatched according to
				 * current consistent_lsn lag.
				 */
			case FLUSHLIST_TASK:
				can_hibernate = polar_buffer_sync(&wb_context, &consistent_lsn, false, 0) && !polar_enable_flush_dispatcher;
				break;

				/*
				 * Do one cycle of lru dirty-buffer writing.
				 */
			case FLUSHLRU_TASK:
				polar_lru_sync_buffer(&wb_context, 0);

				/*
				 * Parallel bgwriter will do lru-flush only when
				 * polar_enable_flush_dispatcher is on.
				 */
				can_hibernate = false;
				break;

			default:
				break;
		}

		/* work done */
		if (task_type != INVALID_TASK && polar_enable_flush_dispatcher)
		{
			int			flush_works;

			flush_works = pg_atomic_sub_fetch_u32(&polar_parallel_bgwriter_info->flush_workers[task_type], 1);
			POLAR_ASSERT_PANIC(flush_works >= 0);
		}

		pg_atomic_write_u32(&polar_parallel_bgwriter_info->polar_flush_work_info[slot].state, FLUSH_IDLE);

		if (FirstCallSinceLastCheckpoint())
		{
			/*
			 * After any checkpoint, close all smgr files.  This is so we
			 * won't hang onto smgr references to deleted files indefinitely.
			 */
			smgrcloseall();
		}

		/*
		 * Sleep until we are signaled or BgWriterDelay has elapsed.
		 */
		rc = WaitLatch(MyLatch,
					   WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
					   polar_parallel_bgwriter_delay /* ms */ ,
					   WAIT_EVENT_BGWRITER_MAIN);

		if (rc == WL_TIMEOUT && can_hibernate && prev_hibernate)
			/* Sleep ... */
			(void) WaitLatch(MyLatch,
							 WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
							 polar_parallel_bgwriter_delay * HIBERNATE_FACTOR,
							 WAIT_EVENT_BGWRITER_HIBERNATE);

		prev_hibernate = can_hibernate;
	}
}

/* Shut down #workers parallel background workers. */
void
polar_shutdown_parallel_bgwriter_workers(int workers)
{
	int			used_slot;
	int			i;
	int			read_worker_idx,
				write_worker_idx;

	Assert(polar_parallel_bgwriter_enabled() && MyBackendType == B_BG_WRITER);

	SpinLockAcquire(&polar_parallel_bgwriter_info->lock);
	read_worker_idx = polar_parallel_bgwriter_info->read_worker_idx;
	write_worker_idx = polar_parallel_bgwriter_info->write_worker_idx;
	SpinLockRelease(&polar_parallel_bgwriter_info->lock);

	if (read_worker_idx != write_worker_idx)
		return;

	for (i = 0; i < workers; i++)
	{
		used_slot = find_one_idle_used_handle_slot();
		if (used_slot == -1)
		{
			ereport(WARNING,
					(errmsg(
							"Can not find a used background worker handle slot to shutdown a worker, total number of workers is %d",
							CURRENT_PARALLEL_WORKERS)));
			break;
		}

		TerminateBackgroundWorker(&polar_parallel_bgwriter_info->polar_flush_work_info[used_slot].handle);
		polar_wait_bg_worker_shutdown(&polar_parallel_bgwriter_info->polar_flush_work_info[used_slot].handle, true);

		polar_parallel_bgwriter_info->polar_flush_work_info[used_slot].latch = NULL;
		reset_bgwriter_handle(&polar_parallel_bgwriter_info->polar_flush_work_info[used_slot].handle);
		pg_atomic_write_u32(&polar_parallel_bgwriter_info->polar_flush_work_info[used_slot].state, FLUSH_IDLE);
		pg_atomic_fetch_sub_u32(&polar_parallel_bgwriter_info->current_workers, 1);

		if (polar_enable_debug)
		{
			ereport(DEBUG1,
					(errmsg(
							"Shut down one background writer worker, its handle slot index is %d",
							used_slot)));
		}
	}
}

/* Check the parallel background writers, if some writers are stopped, start some. */
void
polar_check_parallel_bgwriter_worker(void)
{
	static TimestampTz last_check_ts = 0;

	TimestampTz timeout;
	TimestampTz now;

	if (!polar_parallel_bgwriter_enabled() ||
		polar_is_replica())
		return;

	if (last_check_ts == 0)
	{
		last_check_ts = GetCurrentTimestamp();
		return;
	}

	Assert(MyBackendType == B_BG_WRITER);

	now = GetCurrentTimestamp();
	timeout = TimestampTzPlusMilliseconds(last_check_ts,
										  polar_parallel_bgwriter_check_interval * 1000L);
	if (now >= timeout)
	{
		polar_launch_parallel_bgwriter_workers();
		last_check_ts = now;
	}
}

/*
 * polar_new_parallel_bgwriter_useful - Is it meaningful to add bgwriter?
 *
 * Increasing the bgwriter when IO reaches its limit does not
 * bring benefits. So it is necessary to consider the
 * IO throughput of the bgwriter. If it is found that increasing
 * the bgwriter does not improve the IO throughput, there
 * is no need to consider increasing the bgwriter.
 */
bool
polar_new_parallel_bgwriter_useful(void)
{
	TimestampTz ed_tz = 0;
	uint64		cur_flush_count = 0;
	int			cur_flush_rate = 0.0;
	int			cur_workers;
	bool		useful;
	long		secs;
	int			usecs;

	polar_sync_buffer_io *sync_buffer_io;

	cur_workers = CURRENT_PARALLEL_WORKERS;
	sync_buffer_io = &polar_flush_ctl->flush_buffer_io;

	if (sync_buffer_io->first ||
		cur_workers < sync_buffer_io->pre_workers)
	{
		sync_buffer_io->pre_flush_count = pg_atomic_read_u64(&sync_buffer_io->bgwriter_flush);
		sync_buffer_io->pre_flush_rate = 0;
		sync_buffer_io->pre_workers = cur_workers;
		sync_buffer_io->st_tz = GetCurrentTimestamp();
		sync_buffer_io->first = false;

		return true;
	}

	ed_tz = GetCurrentTimestamp();
	cur_flush_count = pg_atomic_read_u64(&sync_buffer_io->bgwriter_flush);

	TimestampDifference(sync_buffer_io->st_tz, ed_tz, &secs, &usecs);

	if (unlikely(secs == 0 && usecs == 0))
		return true;

	cur_flush_rate = (int) (cur_flush_count - sync_buffer_io->pre_flush_count) /
		(secs + usecs / 1000000.0);

	if (sync_buffer_io->pre_flush_rate == 0)
	{
		sync_buffer_io->pre_workers = cur_workers;
		sync_buffer_io->pre_flush_rate = cur_flush_rate;
		sync_buffer_io->pre_flush_count = cur_flush_count;
		sync_buffer_io->st_tz = GetCurrentTimestamp();

		return true;
	}

	if (polar_new_bgwriter_flush_factor > 0 &&
		(cur_flush_rate - sync_buffer_io->pre_flush_rate < polar_new_bgwriter_flush_factor))
		useful = false;
	else
		useful = true;

	sync_buffer_io->pre_flush_rate = cur_flush_rate;
	sync_buffer_io->pre_workers = cur_workers;
	sync_buffer_io->pre_flush_count = 0;
	pg_atomic_write_u64(&sync_buffer_io->bgwriter_flush, 0);
	sync_buffer_io->st_tz = GetCurrentTimestamp();

	return useful;
}

static int
find_one_unused_handle_slot(void)
{
	int			i;

	for (i = 0; i < MAX_NUM_OF_PARALLEL_BGWRITER; i++)
	{
		if (handle_is_unused(&polar_parallel_bgwriter_info->polar_flush_work_info[i].handle))
			return i;
	}
	return -1;
}

static int
find_one_idle_used_handle_slot(void)
{
	int			i;

	for (i = 0; i < MAX_NUM_OF_PARALLEL_BGWRITER; i++)
	{
		if (handle_is_used(&polar_parallel_bgwriter_info->polar_flush_work_info[i].handle) &&
			pg_atomic_read_u32(&polar_parallel_bgwriter_info->polar_flush_work_info[i].state) == FLUSH_IDLE)
			return i;
	}
	return -1;
}

static parallel_flush_type_t
polar_get_flush_task(void)
{
	int			read_worker_idx;
	int			write_worker_idx;

	parallel_flush_type_t task_type = INVALID_TASK;

	SpinLockAcquire(&polar_parallel_bgwriter_info->lock);
	read_worker_idx = polar_parallel_bgwriter_info->read_worker_idx;
	write_worker_idx = polar_parallel_bgwriter_info->write_worker_idx;

	if (read_worker_idx != write_worker_idx)
	{
		task_type = polar_parallel_bgwriter_info->flush_task[read_worker_idx];

		Assert(task_type != INVALID_TASK);

		pg_atomic_fetch_add_u32(&polar_parallel_bgwriter_info->flush_workers[task_type], 1);
		polar_parallel_bgwriter_info->flush_task[read_worker_idx] = INVALID_TASK;

		if (++read_worker_idx >= MAX_NUM_OF_PARALLEL_BGWRITER)
			read_worker_idx = 0;

		polar_parallel_bgwriter_info->read_worker_idx = read_worker_idx;
	}
	SpinLockRelease(&polar_parallel_bgwriter_info->lock);

	return task_type;
}
