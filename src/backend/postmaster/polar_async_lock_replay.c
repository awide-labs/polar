/*-------------------------------------------------------------------------
 *
 * polar_async_lock_replay.c
 *	  async lock replay routines.
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
 *    src/backend/postmaster/polar_async_lock_replay.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/twophase.h"
#include "access/xlogutils.h"
#include "access/xlogrecovery.h"
#include "miscadmin.h"
#include "postmaster/interrupt.h"
#include "postmaster/polar_async_lock_replay.h"
#include "replication/walreceiver.h"
#include "storage/procarray.h"
#include "utils/timeout.h"
#include "utils/wait_event.h"

#define ALR_LOCK_FORMAT "lock (%u, %u, %u) %X/%X %s"
#define ALR_LOCK_FORMAT_ARGS(plock) plock->lock.xid, plock->lock.dbOid, \
	plock->lock.relOid, LSN_FORMAT_ARGS(plock->lsn), \
	polar_alr_state_names[plock->state]
#define ALR_XACT_FORMAT "xact(%u) %s"
#define ALR_XACT_FORMAT_ARGS(pxact) pxact->xid, pxact->running ? "running" : "not running"

/* For convenient access */
static HTAB *xact_tbl;
static HTAB *lock_tbl;

/* For debugging, record last add and replay infomation */
static polar_alr_lock last_add_lock;
static polar_alr_lock last_replay_lock;

polar_alr_ctl_t *polar_alr_ctl;
char	   *polar_alr_state_names[] =
{
	"idle",
	"getting",
	"got",
	"skip",
};

/* GUCs */
bool		polar_enable_async_lock_replay;
bool		polar_enable_async_lock_replay_debug;

/* Remove idle locks from replaying queue */
static void
alr_remove_idle_lock(XLogRecPtr lsn)
{
	dlist_iter	iter;

	dlist_foreach(iter, &polar_alr_ctl->locks)
	{
		polar_alr_lock *alr_lock = dlist_container(polar_alr_lock, rply_node, iter.cur);

		if (alr_lock->lsn == lsn)
		{
			dlist_delete(iter.cur);
			return;
		}
	}
	POLAR_ALR_LOG(ERROR, "worker", "cannot find idle lock: %X/%X",
				  LSN_FORMAT_ARGS(lsn));
}

/*
 * Release all locks of a xact, the lock might be IDLE/GOT/SKIP.
 * IDLE: skip release it and remove it from replaying queue.
 * GOT: release it.
 * SKIP: skip release it.
 */
static void
alr_release_locks(polar_alr_xact *alr_xact)
{
	dlist_iter	iter;

	dlist_foreach(iter, &alr_xact->locks)
	{
		LOCKTAG		locktag;
		polar_alr_lock *alr_lock = dlist_container(polar_alr_lock, xact_node, iter.cur);

		Assert(alr_lock->state != POLAR_ALR_GETTING);

		if (alr_lock->state == POLAR_ALR_GOT)
		{
			SET_LOCKTAG_RELATION(locktag, alr_lock->lock.dbOid, alr_lock->lock.relOid);
			if (!LockRelease(&locktag, AccessExclusiveLock, true))
				POLAR_ALR_LOG(ERROR, "worker", "release failed " ALR_LOCK_FORMAT,
							  ALR_LOCK_FORMAT_ARGS(alr_lock));
			else
				POLAR_ALR_DEBUG("worker", "release success " ALR_LOCK_FORMAT,
								ALR_LOCK_FORMAT_ARGS(alr_lock));
		}
		else
			POLAR_ALR_DEBUG("worker", "release skip " ALR_LOCK_FORMAT,
							ALR_LOCK_FORMAT_ARGS(alr_lock));

		/* This lock is still idle, must be in replaying queue, remove it */
		if (alr_lock->state == POLAR_ALR_IDLE)
			alr_remove_idle_lock(alr_lock->lsn);

		hash_search(lock_tbl, &alr_lock->lsn, HASH_REMOVE, NULL);
	}
}

/*
 * Try to release xact if any. A xact which is not running can be released. To
 * release it, we should remove all locks of this xact. And we will remove it
 * and its locks from HTAB.
 */
static void
alr_try_to_release_xact(void)
{
	HASH_SEQ_STATUS hash_seq;
	polar_alr_xact *alr_xact;

	LWLockAcquire(&polar_alr_ctl->lock, LW_EXCLUSIVE);
	hash_seq_init(&hash_seq, xact_tbl);
	while ((alr_xact = hash_seq_search(&hash_seq)) != NULL)
	{
		if (!alr_xact->running)
		{
			POLAR_ALR_DEBUG("worker", "release " ALR_XACT_FORMAT,
							ALR_XACT_FORMAT_ARGS(alr_xact));
			alr_release_locks(alr_xact);
			hash_search(xact_tbl, &alr_xact->xid, HASH_REMOVE, NULL);
			hash_seq_term(&hash_seq);
			hash_seq_init(&hash_seq, xact_tbl);
		}
	}
	LWLockRelease(&polar_alr_ctl->lock);
}

/* Try to acquire lock if any. */
static bool
alr_try_to_acquire_lock(void)
{
	LOCKTAG		locktag;
	polar_alr_lock *alr_lock;
	xl_standby_lock *lock;

	LWLockAcquire(&polar_alr_ctl->lock, LW_EXCLUSIVE);
	if (dlist_is_empty(&polar_alr_ctl->locks))
	{
		polar_alr_ctl->lsn = InvalidXLogRecPtr;
		LWLockRelease(&polar_alr_ctl->lock);
		return false;
	}

	alr_lock = dlist_container(polar_alr_lock, rply_node,
							   dlist_pop_head_node(&polar_alr_ctl->locks));

	/* Record lock when it's poped */
	Assert(alr_lock->lsn >= last_replay_lock.lsn);
	last_replay_lock = *alr_lock;

	Assert(alr_lock->state == POLAR_ALR_IDLE);

	alr_lock->state = POLAR_ALR_GETTING;
	POLAR_ALR_DEBUG("worker", "acquire begin " ALR_LOCK_FORMAT,
					ALR_LOCK_FORMAT_ARGS(alr_lock));

	lock = &alr_lock->lock;

	/* Already processed? */
	if (!TransactionIdIsValid(lock->xid) ||
		TransactionIdDidCommit(lock->xid) ||
		TransactionIdDidAbort(lock->xid))
		alr_lock->state = POLAR_ALR_SKIP;

	/* dbOid is InvalidOid when we are locking a shared relation. */
	Assert(OidIsValid(lock->relOid));

	if (alr_lock->state == POLAR_ALR_SKIP)
	{
		POLAR_ALR_DEBUG("worker", "acquire skip " ALR_LOCK_FORMAT,
						ALR_LOCK_FORMAT_ARGS(alr_lock));
	}
	else
	{
		LWLockRelease(&polar_alr_ctl->lock);
		polar_set_receipt_time(alr_lock->rtime, alr_lock->from_stream);
		SET_LOCKTAG_RELATION(locktag, alr_lock->lock.dbOid, alr_lock->lock.relOid);
		(void) LockAcquire(&locktag, AccessExclusiveLock, true, false);
		alr_lock->state = POLAR_ALR_GOT;
		POLAR_ALR_DEBUG("worker", "acquire success " ALR_LOCK_FORMAT,
						ALR_LOCK_FORMAT_ARGS(alr_lock));
		LWLockAcquire(&polar_alr_ctl->lock, LW_EXCLUSIVE);
	}
	polar_alr_ctl->lsn = alr_lock->lsn;

	/* Some infomation changed, record it again */
	last_replay_lock = *alr_lock;

	LWLockRelease(&polar_alr_ctl->lock);

	if (polar_alr_ctl->startup_waiting)
		WakeupRecovery();
	polar_alr_ctl->startup_waiting = false;

	WalRcvForceReply();

	return true;
}

Size
polar_alr_shmem_size(void)
{
	Size		size = 0;

	if (!polar_allow_alr())
		return size;

	size = sizeof(polar_alr_ctl_t);
	size = add_size(size, hash_estimate_size(1024, sizeof(polar_alr_lock)));
	size = add_size(size, hash_estimate_size(256, sizeof(polar_alr_xact)));

	return size;
}

void
polar_alr_shmem_init(void)
{
	bool		found = false;

	if (!polar_allow_alr())
		return;

	polar_alr_ctl = (polar_alr_ctl_t *)
		ShmemInitStruct("async lock replay control",
						sizeof(polar_alr_ctl_t),
						&found);

	if (!IsUnderPostmaster)
	{
		HASHCTL		hash_ctl;

		memset(&hash_ctl, 0, sizeof(hash_ctl));
		hash_ctl.keysize = sizeof(XLogRecPtr);
		hash_ctl.entrysize = sizeof(polar_alr_lock);
		lock_tbl = ShmemInitHash("async lock replay locks",
								 64, 1024,
								 &hash_ctl,
								 HASH_ELEM | HASH_BLOBS);

		memset(&hash_ctl, 0, sizeof(hash_ctl));
		hash_ctl.keysize = sizeof(TransactionId);
		hash_ctl.entrysize = sizeof(polar_alr_xact);
		xact_tbl = ShmemInitHash("async lock replay xacts",
								 64, 256,
								 &hash_ctl,
								 HASH_ELEM | HASH_BLOBS);

		polar_alr_ctl->lock_tbl = lock_tbl;
		polar_alr_ctl->xact_tbl = xact_tbl;
		polar_alr_ctl->latch = NULL;
		polar_alr_ctl->startup_waiting = false;

		last_add_lock.lsn = last_replay_lock.lsn = InvalidXLogRecPtr;

		dlist_init(&polar_alr_ctl->locks);

		LWLockInitialize(&polar_alr_ctl->lock, LWTRANCHE_POLAR_ASYNC_LOCK_REPLAY);
	}
	else
		Assert(found);
}

/* Main Loop for async lock replay worker */
void
polar_alr_worker_main(Datum main_arg)
{
	/* SIGINT/SIGQUIT/SIGPIPE/SIGCHLD/InitializeTimeouts are already set up */
	pqsignal(SIGHUP, SignalHandlerForConfigReload); /* reload config file */
	pqsignal(SIGTERM, SignalHandlerForShutdownRequest); /* request shutdown */
	BackgroundWorkerUnblockSignals();

	/*
	 * Register timeouts needed for standby mode
	 */
	RegisterTimeout(STANDBY_DEADLOCK_TIMEOUT, StandbyDeadLockHandler);
	RegisterTimeout(STANDBY_TIMEOUT, StandbyTimeoutHandler);
	RegisterTimeout(STANDBY_LOCK_TIMEOUT, StandbyLockTimeoutHandler);

	InRecovery = true;
	standbyState = STANDBY_SNAPSHOT_READY;

	CurrentResourceOwner = ResourceOwnerCreate(NULL, POLAR_ALR_WORKER_NAME);
	polar_alr_ctl->latch = MyLatch;

	POLAR_ALR_LOG(LOG, "worker", "worker started, pid: %d", MyProcPid);

	for (;;)
	{
		bool		got_lock = false;

		do
		{
			if (ShutdownRequestPending)
			{
				polar_alr_release_locks(InvalidTransactionId, "worker");
				alr_try_to_release_xact();
			}
			HandleMainLoopInterrupts();
			got_lock = alr_try_to_acquire_lock();
			alr_try_to_release_xact();
		} while (got_lock);

		WaitLatch(MyLatch,
				  WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
				  1000 /* ms */ ,
				  WAIT_EVENT_ASYNC_LOCK_REPLAY_MAIN);

		ResetLatch(MyLatch);
	}
}

bool
polar_alr_check_worker(void)
{
	pid_t		pid;

	return GetBackgroundWorkerPid(polar_alr_ctl->handle, &pid) == BGWH_STARTED;
}

void
polar_alr_launch_worker(void)
{
	BackgroundWorker worker;
	BackgroundWorkerHandle *handle;
	BgwHandleStatus status;
	pid_t		pid;
	int			times = 0;

	if (!polar_allow_alr())
		return;

	POLAR_ALR_LOG(LOG, "startup", "%s", "launch worker");

	memset(&worker, 0, sizeof(BackgroundWorker));
	worker.bgw_flags = BGWORKER_SHMEM_ACCESS;
	worker.bgw_start_time = BgWorkerStart_PostmasterStart;
	worker.bgw_restart_time = BGW_NEVER_RESTART;
	worker.bgw_notify_pid = MyProcPid;
	sprintf(worker.bgw_library_name, "postgres");
	sprintf(worker.bgw_function_name, "polar_alr_worker_main");
	snprintf(worker.bgw_name, BGW_MAXLEN, POLAR_ALR_WORKER_NAME);
	snprintf(worker.bgw_type, BGW_MAXLEN, POLAR_ALR_WORKER_NAME);

	if (!RegisterDynamicBackgroundWorker(&worker, &handle))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
				 errmsg("could not register async lock replay process"),
				 errhint("You may need to increase max_worker_processes.")));

	status = WaitForBackgroundWorkerStartup(handle, &pid);
	if (status != BGWH_STARTED)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
				 errmsg("could not start background process"),
				 errhint("More details may be available in the server log.")));

	polar_alr_ctl->handle = handle;

	while (polar_alr_ctl->latch == NULL)
	{
		pg_usleep(100000L);		/* 100ms */
		if (++times > 10)		/* wait for 1s at most */
			POLAR_ALR_LOG(ERROR, "startup", "%s", "launch worker failed");
	}
}

void
polar_alr_terminate_worker(void)
{
	if (!polar_allow_alr())
		return;

	POLAR_ALR_LOG(LOG, "startup", "%s", "terminate worker");
	TerminateBackgroundWorker(polar_alr_ctl->handle);

	(void) WaitForBackgroundWorkerShutdown(polar_alr_ctl->handle);

	pfree(polar_alr_ctl->handle);
}

/*
 * Add lock to async table, and record the lsn, rtime. Async lock replay worker
 * will handle these locks.
 */
void
polar_alr_add_async_lock(xl_standby_lock *lock, XLogRecPtr lsn,
						 XLogRecPtr last_lsn, TimestampTz rtime, bool from_stream)
{
	bool		found;
	polar_alr_lock *alr_lock;
	polar_alr_xact *alr_xact;

	LWLockAcquire(&polar_alr_ctl->lock, LW_EXCLUSIVE);

	alr_lock = hash_search(lock_tbl, &lsn, HASH_ENTER, &found);
	if (!found)
	{
		alr_lock->lock = *lock;
		alr_lock->rtime = rtime;
		alr_lock->from_stream = from_stream;
		alr_lock->state = POLAR_ALR_IDLE;
		POLAR_ALR_DEBUG("startup", "add " ALR_LOCK_FORMAT,
						ALR_LOCK_FORMAT_ARGS(alr_lock));
	}
	else
		goto ret;
	dlist_push_tail(&polar_alr_ctl->locks, &alr_lock->rply_node);

	alr_xact = hash_search(xact_tbl, &alr_lock->lock.xid, HASH_ENTER, &found);
	if (!found)
	{
		alr_xact->running = true;
		dlist_init(&alr_xact->locks);
		POLAR_ALR_DEBUG("startup", "add " ALR_XACT_FORMAT,
						ALR_XACT_FORMAT_ARGS(alr_xact));
	}
	dlist_push_tail(&alr_xact->locks, &alr_lock->xact_node);

	Assert(alr_lock->lsn >= last_add_lock.lsn);
	last_add_lock = *alr_lock;

	if (XLogRecPtrIsInvalid(polar_alr_ctl->lsn))
		polar_alr_ctl->lsn = last_lsn;

ret:
	LWLockRelease(&polar_alr_ctl->lock);

	if (!polar_alr_check_worker())
		POLAR_ALR_LOG(ERROR, "startup", "%s", "worker is not running");

	if (polar_alr_ctl->latch)
		SetLatch(polar_alr_ctl->latch);
}

/* Mark xid as not running, to release xact and its locks. */
void
polar_alr_release_locks(TransactionId xid, char *role)
{
	polar_alr_xact *alr_xact;
	HASH_SEQ_STATUS hash_seq;

	LWLockAcquire(&polar_alr_ctl->lock, LW_SHARED);
	if (TransactionIdIsValid(xid))
	{
		alr_xact = hash_search(xact_tbl, &xid, HASH_FIND, NULL);
		if (alr_xact)
		{
			POLAR_ALR_DEBUG(role, "release " ALR_XACT_FORMAT,
							ALR_XACT_FORMAT_ARGS(alr_xact));
			alr_xact->running = false;
		}
	}
	else
	{
		hash_seq_init(&hash_seq, xact_tbl);
		while ((alr_xact = hash_seq_search(&hash_seq)))
		{
			POLAR_ALR_DEBUG(role, "release " ALR_XACT_FORMAT,
							ALR_XACT_FORMAT_ARGS(alr_xact));
			alr_xact->running = false;
		}
	}
	LWLockRelease(&polar_alr_ctl->lock);

	if (polar_alr_ctl->latch)
		SetLatch(polar_alr_ctl->latch);
}


/*
 * polar_alr_release_old_locks
 *		Release standby locks held by top-level XIDs that aren't running,
 *		as long as they're not prepared transactions.
 */
void
polar_alr_release_old_locks(TransactionId oldxid)
{
	HASH_SEQ_STATUS hash_seq;
	polar_alr_xact *alr_xact;

	LWLockAcquire(&polar_alr_ctl->lock, LW_SHARED);
	hash_seq_init(&hash_seq, xact_tbl);
	while ((alr_xact = hash_seq_search(&hash_seq)) != NULL)
	{
		Assert(TransactionIdIsValid(alr_xact->xid));

		/* Skip if prepared transaction. */
		if (StandbyTransactionIdIsPrepared(alr_xact->xid))
			continue;

		/* Skip if >= oldxid. */
		if (!TransactionIdPrecedes(alr_xact->xid, oldxid))
			continue;

		POLAR_ALR_DEBUG("startup", "release old " ALR_XACT_FORMAT,
						ALR_XACT_FORMAT_ARGS(alr_xact));

		alr_xact->running = false;
	}
	LWLockRelease(&polar_alr_ctl->lock);

	if (polar_alr_ctl->latch)
		SetLatch(polar_alr_ctl->latch);
}

/* Return true if this xid's lock is still need to replay */
bool
polar_alr_xact_is_replaying(TransactionId xid)
{
	dlist_iter	iter;
	polar_alr_xact *alr_xact;

	LWLockAcquire(&polar_alr_ctl->lock, LW_SHARED);
	alr_xact = hash_search(xact_tbl, &xid, HASH_FIND, NULL);
	if (alr_xact && !alr_xact->running)
		POLAR_ALR_LOG(PANIC, "startup", "%s", "replaying WAL on finished xact");
	LWLockRelease(&polar_alr_ctl->lock);

	if (!alr_xact)
		return false;

	dlist_foreach(iter, &alr_xact->locks)
	{
		polar_alr_lock *alr_lock = dlist_container(polar_alr_lock,
												   xact_node, iter.cur);

		if (alr_lock->state < POLAR_ALR_GOT)
			return true;
	}

	return false;
}
