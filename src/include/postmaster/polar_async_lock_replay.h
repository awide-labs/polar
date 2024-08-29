/*-------------------------------------------------------------------------
 *
 * polar_async_lock_replay.h
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
 *    src/include/postmaster/polar_async_lock_replay.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef POLAR_ASYNC_LOCK_REPLAY_H
#define POLAR_ASYNC_LOCK_REPLAY_H

#include "access/hash.h"
#include "lib/ilist.h"
#include "postmaster/bgworker.h"
#include "storage/latch.h"
#include "storage/lwlock.h"

#define POLAR_ALR_WORKER_NAME "async lock replay worker"
#define POLAR_ALR_LOG(elevel, role, fmt, ...) \
		elog(elevel, "async lock replay: %s@%s: " fmt, \
			 role, PG_FUNCNAME_MACRO, __VA_ARGS__)
#define POLAR_ALR_DEBUG(role, ...) \
	do { \
		if (polar_enable_async_lock_replay_debug) \
			POLAR_ALR_LOG(LOG, role, __VA_ARGS__); \
	} while(0)
#define polar_allow_alr() (polar_enable_async_lock_replay && polar_is_replica())

typedef enum polar_alr_state
{
	POLAR_ALR_IDLE,
	POLAR_ALR_GETTING,
	POLAR_ALR_GOT,
	POLAR_ALR_SKIP
} polar_alr_state;

extern char *polar_alr_state_names[];

typedef struct polar_alr_lock
{
	XLogRecPtr	lsn;
	xl_standby_lock lock;
	TimestampTz rtime;
	bool		from_stream;
	polar_alr_state state;
	dlist_node	rply_node;
	dlist_node	xact_node;
} polar_alr_lock;

typedef struct polar_alr_xact
{
	TransactionId xid;
	bool		running;
	dlist_head	locks;
} polar_alr_xact;

typedef struct polar_alr_ctl_t
{
	BackgroundWorkerHandle *handle;
	XLogRecPtr	lsn;
	HTAB	   *xact_tbl;
	HTAB	   *lock_tbl;
	dlist_head	locks;
	LWLock		lock;
	bool		startup_waiting;
	Latch	   *latch;
} polar_alr_ctl_t;

extern polar_alr_ctl_t *polar_alr_ctl;

/* worker operation interface */
extern Size polar_alr_shmem_size(void);
extern void polar_alr_shmem_init(void);
extern bool polar_alr_check_worker(void);
extern void polar_alr_launch_worker(void);
extern void polar_alr_terminate_worker(void);
extern void polar_alr_worker_main(Datum main_arg);

/* lock operation interface */
extern void polar_alr_add_async_lock(xl_standby_lock *lock, XLogRecPtr lsn,
									 XLogRecPtr last_lsn, TimestampTz rtime, bool from_stream);
extern void polar_alr_release_locks(TransactionId xid, char *role);
extern void polar_alr_release_old_locks(TransactionId oldxid);
extern bool polar_alr_xact_is_replaying(TransactionId xid);

#endif							/* POLAR_ASYNC_LOCK_REPLAY_H */
