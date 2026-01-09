/*-------------------------------------------------------------------------
 *
 * polar_wal_pipeliner.c
 *		Wal pipeline implementation.
 *
 * Copyright (c) 2021, Alibaba Group Holding Limited
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
 *      src/backend/postmaster/polar_wal_pipeliner.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <signal.h>
#include <unistd.h>

#include "access/xlog.h"
#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "postmaster/polar_wal_pipeliner.h"
#include "postmaster/interrupt.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/procsignal.h"
#include "storage/s_lock.h"
#include "storage/spin.h"
#include "storage/lwlock.h"
#include "storage/proc.h"
#include "storage/smgr.h"
#include "storage/pmsignal.h"
#include "utils/guc.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/resowner.h"
#include "utils/timestamp.h"

/* POLAR wal pipeline */
#include "utils/elog.h"
#ifdef ENABLE_THREAD_SAFETY
/* Use platform-dependent pthread capability */
#include <pthread.h>
#endif

/* POLAR wal pipeline */
#ifdef ENABLE_THREAD_SAFETY

#define INVALID_THREAD								((pthread_t) 0)

pthread_t	worker_handles[POLAR_WAL_PIPELINE_MAX_THREAD_NUM];

#endif

typedef struct worker_args
{
	int thread_no;
	int ident;
	bool (*fp)(int);
	int spin_delay;
	int timeout;
	polar_wait_object_t *wait_obj;
} worker_args;

worker_args args[POLAR_WAL_PIPELINE_MAX_THREAD_NUM];

/*
 * postgres elog is not thread safe,
 * and wal pipeline is multi thread,
 * need special process
 */
bool multi_thread_elog = false;

/*
 * polar vfs is not thread safe,
 * and wal pipeline is multi thread,
 * need special process
 */
bool multi_thread_vfs;
slock_t polar_wal_pipeline_vfs_lck;

static void polar_wal_pipeliner_create_advance_worker(void);
static void polar_wal_pipeliner_create_flush_worker(void);
static void polar_wal_pipeliner_create_notify_worker(void);

static bool polar_wal_pipeliner_main_mode_1(int ident);
static bool polar_wal_pipeliner_main_mode_2(int ident);
static bool polar_wal_pipeliner_main_mode_3(int ident);
static bool polar_wal_pipeliner_main_mode_4(int ident);
static bool polar_wal_pipeliner_main_mode_5(int ident);

static void polar_wal_pipeliner_init_mode_1(void);
static void polar_wal_pipeliner_init_mode_2(void);
static void polar_wal_pipeliner_init_mode_3(void);
static void polar_wal_pipeliner_init_mode_4(void);
static void polar_wal_pipeliner_init_mode_5(void);

/* POLAR wal pipeline */
static void *polar_wal_pipeliner_worker(void *arg);
static void polar_wal_pipeliner_init(void);

extern polar_wait_object_t* polar_wal_pipeline_get_worker_wait_obj(int thread_no);

/*
 * Main entry point for wal pipeline process
 *
 * This is invoked from AuxiliaryProcessMain, which has already created the
 * basic execution environment, but not enabled signals yet.
 */
void
polar_wal_pipeliner_main(void)
{
	polar_spin_delay_status_t status;

	/*
	 * Properly accept or ignore signals the postmaster might send us
	 *
	 * We have no particular use for SIGINT and SIGTERM at the moment.
	 */
	pqsignal(SIGHUP, SignalHandlerForConfigReload);
	pqsignal(SIGINT, SIG_IGN);
	pqsignal(SIGTERM, SIG_IGN);
	/* SIGQUIT handler was already set up by InitPostmasterChild */
	pqsignal(SIGALRM, SIG_IGN);
	pqsignal(SIGPIPE, SIG_IGN);
	pqsignal(SIGUSR1, procsignal_sigusr1_handler);
	pqsignal(SIGUSR2, SignalHandlerForShutdownRequest);

	/*
	 * Reset some signals that are accepted by postmaster but not here
	 */
	pqsignal(SIGCHLD, SIG_DFL);
	pqsignal(SIGTTIN, SIG_DFL);
	pqsignal(SIGTTOU, SIG_DFL);
	pqsignal(SIGCONT, SIG_DFL);
	pqsignal(SIGWINCH, SIG_DFL);

	/* We allow SIGQUIT (quickdie) at all times */
	sigdelset(&BlockSig, SIGQUIT);

	before_shmem_exit(pgstat_before_server_shutdown, 0);

	/*
	 * Unblock signals (they were blocked when the postmaster forked us)
	 */
	PG_SETMASK(&UnBlockSig);

	polar_wal_pipeliner_init();

	/*
	 * Advertise our latch that backends can use to wake us up while we're
	 * sleeping.
	 */
	ProcGlobal->polar_wal_pipeliner_latch = &MyProc->procLatch;

	polar_init_spin_delay_mt(&status, polar_wal_pipeline_get_worker_wait_obj(WRITE_WORKER_THREAD_NO), 
							 polar_wal_pipeline_write_worker_spin_delay, polar_wal_pipeline_write_worker_timeout);

	/*
	 * Loop forever
	 */
	for (;;)
	{
		int res;

		if (ProcSignalBarrierPending)
			ProcessProcSignalBarrier();

		/*
		 * Process any requests or signals received recently.
		 */
		if (ShutdownRequestPending || !PostmasterIsAlive())
		{
			int i;

			for (i = 0; i < POLAR_WAL_PIPELINE_MAX_THREAD_NUM; i++)
				if (worker_handles[i] != INVALID_THREAD)
					pthread_join(worker_handles[i], NULL);

			pgstat_report_wal(true);

			/*
			 * Only when all wal pipeline thread have exited, we
			 * can set polar_wal_pipeliner_latch to tell others that wal
			 * pipeliner has exited.
			 */
			ProcGlobal->polar_wal_pipeliner_latch = NULL;

			/* Exit as a process */
			proc_exit(0);
		}
		else
		{
			/* report pending statistics to the cumulative stats system */
			pgstat_report_wal(false);
		}

		switch (polar_wal_pipeline_mode)
		{
			case 1:
				res = polar_wal_pipeliner_main_mode_1(0);
				break;
			case 2:
				res = polar_wal_pipeliner_main_mode_2(0);
				break;
			case 3:
				res = polar_wal_pipeliner_main_mode_3(0);
				break;
			case 4:
				res = polar_wal_pipeliner_main_mode_4(0);
				break;
			case 5:
				res = polar_wal_pipeliner_main_mode_5(0);
				break;
			default:
				elog(ERROR, "polar wal pipeline in wrong mode %d", polar_wal_pipeline_mode);
		}

		if (res)
			polar_reset_spin_delay_mt(&status);
		else
			polar_perform_spin_delay_mt(&status, true, true);
	}
}

static void polar_wal_pipeliner_create_advance_worker(void)
{
	int err;
	int thread_no = ADVANCE_WORKER_THREAD_NO;

	args[thread_no].thread_no = thread_no;
	args[thread_no].ident = 0;
	args[thread_no].fp = polar_wal_pipeline_advance;
	args[thread_no].spin_delay = polar_wal_pipeline_advance_worker_spin_delay;
	args[thread_no].timeout = polar_wal_pipeline_advance_worker_timeout;
	args[thread_no].wait_obj = polar_wal_pipeline_get_worker_wait_obj(thread_no);
	err = pthread_create(&worker_handles[thread_no], NULL, polar_wal_pipeliner_worker, &args[thread_no]);
	if (err != 0 || worker_handles[thread_no] == INVALID_THREAD)
		elog(PANIC, "create wal pipeline advance worker failed");
}

static void polar_wal_pipeliner_create_flush_worker(void)
{
	int err;
	int thread_no = FLUSH_WORKER_THREAD_NO;

	args[thread_no].thread_no = thread_no;
	args[thread_no].ident = 0;
	args[thread_no].fp = polar_wal_pipeline_flush;
	args[thread_no].spin_delay = polar_wal_pipeline_flush_worker_spin_delay;
	args[thread_no].timeout = polar_wal_pipeline_flush_worker_timeout;
	args[thread_no].wait_obj = polar_wal_pipeline_get_worker_wait_obj(thread_no);
	err = pthread_create(&worker_handles[thread_no], NULL, polar_wal_pipeliner_worker, &args[thread_no]);
	if (err != 0 || worker_handles[thread_no] == INVALID_THREAD)
		elog(PANIC, "create wal pipeline flush worker failed");
}

static void polar_wal_pipeliner_create_notify_worker(void)
{
	int i;
	int err;
	int thread_no = NOTIFY_WORKER_THREAD_NO;

	for (i = 0; i < polar_wal_pipeline_notify_worker_num; i++)
	{
		polar_wal_pipeline_set_last_notify_lsn(i, InvalidXLogRecPtr);

		args[thread_no].thread_no = thread_no;
		args[thread_no].ident = i;
		args[thread_no].fp = polar_wal_pipeline_notify;
		args[thread_no].spin_delay = polar_wal_pipeline_notify_worker_spin_delay;
		args[thread_no].timeout = polar_wal_pipeline_notify_worker_timeout;
		args[thread_no].wait_obj = polar_wal_pipeline_get_worker_wait_obj(thread_no);
		err = pthread_create(&worker_handles[thread_no], NULL, polar_wal_pipeliner_worker, &args[thread_no]);
		if (err != 0 || worker_handles[thread_no] == INVALID_THREAD)
			elog(PANIC, "create wal pipeline notify worker failed");

		thread_no++;
	}
}

static bool
polar_wal_pipeliner_main_mode_1(int ident)
{
	bool res1;
	bool res2;
	bool res3;

	res1 = polar_wal_pipeline_advance(ident);

	/* Disable pipeline mode to use original postgres write/flush logic */
	polar_wal_pipeline_enable = false;
	res2 = polar_wal_pipeline_write(ident);

	res3 = polar_wal_pipeline_notify(ident);

	return res1 || res2 || res3; 
}

static void
polar_wal_pipeliner_init_mode_1(void)
{
	return;
}

static bool
polar_wal_pipeliner_main_mode_2(int ident)
{
	bool res1;
	bool res2;

	res1 = polar_wal_pipeline_advance(ident);

	/* Disable pipeline mode to use original postgres write/flush logic */
	polar_wal_pipeline_enable = false;
	res2 = polar_wal_pipeline_write(ident);

	return res1 || res2;
}

static void
polar_wal_pipeliner_init_mode_2(void)
{
	polar_wal_pipeliner_create_notify_worker();
}

static bool
polar_wal_pipeliner_main_mode_3(int ident)
{
	/* Disable pipeline mode to use original postgres write/flush logic */
	polar_wal_pipeline_enable = false;

	return polar_wal_pipeline_write(ident);
}

static void
polar_wal_pipeliner_init_mode_3(void)
{
	polar_wal_pipeliner_create_advance_worker();
	polar_wal_pipeliner_create_notify_worker();
}

static bool
polar_wal_pipeliner_main_mode_4(int ident)
{
	bool res1;
	bool res2;

	res1 = polar_wal_pipeline_advance(ident);
	res2 = polar_wal_pipeline_write(ident);

	return res1 || res2;
}

static void
polar_wal_pipeliner_init_mode_4(void)
{
	polar_wal_pipeliner_create_flush_worker();
	polar_wal_pipeliner_create_notify_worker();
}

static bool
polar_wal_pipeliner_main_mode_5(int ident)
{
	return polar_wal_pipeline_write(ident);
}

static void
polar_wal_pipeliner_init_mode_5(void)
{
	polar_wal_pipeliner_create_advance_worker();
	polar_wal_pipeliner_create_flush_worker();
	polar_wal_pipeliner_create_notify_worker();
}

static void
polar_wal_pipeliner_init(void)
{
	int i;

	for (i = 0; i < POLAR_WAL_PIPELINE_MAX_THREAD_NUM; i++)
		worker_handles[i] = INVALID_THREAD;

	if (polar_wal_pipeline_mode > 3)
	{
		/*
		* Should disable wait stat, wait stat in polar vfs is not thread-safe;
		* Or else maybe crash
		*/
		polar_enable_stat_wait_info = false;

		/* ELOG is not thread safe */
		multi_thread_elog = true;

		/* vfs is not thread safe */
		multi_thread_vfs = true;
		SpinLockInit(&polar_wal_pipeline_vfs_lck);
	}

	/* we need to work in critical section */
	START_CRIT_SECTION();

	/* Must set before advance thread */
	polar_wal_pipeline_set_ready_write_lsn(polar_wal_pipeline_get_current_insert_lsn());

	switch (polar_wal_pipeline_mode)
	{
		case 1:
			polar_wal_pipeliner_init_mode_1();
			break;
		case 2:
			polar_wal_pipeliner_init_mode_2();
			break;
		case 3:
			polar_wal_pipeliner_init_mode_3();
			break;
		case 4:
			polar_wal_pipeliner_init_mode_4();
			break;
		case 5:
			polar_wal_pipeliner_init_mode_5();
			break;
		default:
			elog(ERROR, "polar wal pipeline in wrong mode %d", polar_wal_pipeline_mode);
	}
}

static void *
polar_wal_pipeliner_worker(void *arg)
{
	worker_args *args = (worker_args *)arg;
	polar_spin_delay_status_t status;

	polar_init_spin_delay_mt(&status, args->wait_obj, args->spin_delay, args->timeout);
	for (;;)
	{
		if (ShutdownRequestPending || !PostmasterIsAlive())
			pthread_exit(NULL);

		if (args->fp(args->ident))
		{
			polar_reset_spin_delay_mt(&status);
		}
		else
		{
			polar_perform_spin_delay_mt(&status, true, true);
		}
	}
}

void
polar_wal_pipeliner_wakeup(void)
{
	polar_wait_object_t *flush_worker_wait_obj;

	switch (polar_wal_pipeline_mode)
	{
		case 1:
		case 2:
		case 3:
			flush_worker_wait_obj = polar_wal_pipeline_get_worker_wait_obj(WRITE_WORKER_THREAD_NO);
			break;
		case 4:
		case 5:
			flush_worker_wait_obj = polar_wal_pipeline_get_worker_wait_obj(FLUSH_WORKER_THREAD_NO);
			break;
		default:
			elog(ERROR, "polar wal pipeline in wrong mode %d", polar_wal_pipeline_mode);
	}

	pthread_cond_signal(&flush_worker_wait_obj->cond);
}

void
polar_wal_pipeline_wakeup_notifier(void)
{
	int i;

	if (polar_wal_pipeline_mode <= 1)
	{
		/* There are no dedicated notifier threads in this mode, notficateions are done
		unconditionally by the same thread that does write/flush operations */
	}

	for (i=0; i<polar_wal_pipeline_notify_worker_num; i++)
	{
		polar_wait_object_t *notify_worker_wait_obj = polar_wal_pipeline_get_worker_wait_obj(NOTIFY_WORKER_THREAD_NO + i);

		pthread_cond_signal(&notify_worker_wait_obj->cond);
	}
}

/* POLAR wal pipeline END */
