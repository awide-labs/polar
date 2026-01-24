/*-------------------------------------------------------------------------
 *
 * polar_wal_pipeliner.h
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
 *      src/include/postmaster/polar_wal_pipeliner.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef _POLAR_WAL_PIPELINER_H
#define _POLAR_WAL_PIPELINER_H

#include "c.h"
#include "port/atomics.h"

/*
 * thread number allocation scheme:
 * write worker		0
 * advance worker 	1
 * flush worker		2
 * notify worker 	3~6
 *
 * We make write worker as master thread because
 * 1. wal pipeline is multi threaded
 * 2. write worker should call XLogWrite
 * 3. we try to make write worker running like a process,
 *    such as pid equal to thread id
 */
#define WRITE_WORKER_THREAD_NO		0
#define ADVANCE_WORKER_THREAD_NO	1
#define FLUSH_WORKER_THREAD_NO		2
#define NOTIFY_WORKER_THREAD_NO		3

/* advance + write + flush + notify(POLAR_WAL_NOTIFY_WORKER_NUM_MAX) */
#define POLAR_WAL_PIPELINE_MAX_THREAD_NUM 7

#define POLAR_WAL_PIPELINE_STAT_PARTITIONS 256

typedef struct polar_wal_pipeline_user_stats_t
{
	/* satisfied by other user */
	pg_atomic_uint64 total_user_group_commits;
	/* satisfied by spin */
	pg_atomic_uint64 total_user_spin_commits;
	/* satisfied by timeout */
	pg_atomic_uint64 total_user_timeout_commits;
	/* satisfied by wakeup */
	pg_atomic_uint64 total_user_wakeup_commits;
	/* not satisfied by wakeup */
	pg_atomic_uint64 total_user_miss_timeouts;
	/* not satisfied by wakeup */
	pg_atomic_uint64 total_user_miss_wakeups;
} polar_wal_pipeline_user_stats_t;

typedef struct polar_wal_pipeline_stats_t
{
	/*
	 * commit stats for waiting user process user_commits = group_commits +
	 * spin_commits + timeout_commits + wakeup_commits
	 */

	/* satisfied by other user */
	polar_wal_pipeline_user_stats_t user_stats[POLAR_WAL_PIPELINE_STAT_PARTITIONS];

	char		pad0[PG_CACHE_LINE_SIZE];

	/* stats for advance worker */
	pg_atomic_uint64 total_advance_callups;
	pg_atomic_uint64 total_advances;

	char		pad1[PG_CACHE_LINE_SIZE];

	/* stats for write worker */
	pg_atomic_uint64 total_write_callups;
	pg_atomic_uint64 total_writes;
	pg_atomic_uint64 unflushed_xlog_slot_waits;

	char		pad2[PG_CACHE_LINE_SIZE];

	/* stats for flush worker */
	pg_atomic_uint64 total_flush_callups;
	pg_atomic_uint64 total_flushes;
	pg_atomic_uint64 total_flush_merges;

	char		pad3[PG_CACHE_LINE_SIZE];

	/* stats for notify worker */
	pg_atomic_uint64 total_notify_callups;
	pg_atomic_uint64 total_notifies;
	pg_atomic_uint64 total_notified_users;
} polar_wal_pipeline_stats_t;

extern void polar_wal_pipeliner_main(void);
extern void polar_wal_pipeliner_wakeup(void);
extern void polar_wal_pipeline_wakeup_notifier(void);

#endif
