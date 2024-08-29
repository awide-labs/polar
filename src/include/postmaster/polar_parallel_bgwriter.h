/*-------------------------------------------------------------------------
 *
 * polar_parallel_bgwriter.h
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
 *	  src/include/postmaster/polar_parallel_bgwriter.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef PALAR_PARALLEL_BGWRITER_H
#define PALAR_PARALLEL_BGWRITER_H

#include "utils/guc.h"
#include "port/atomics.h"
#include "postmaster/bgworker.h"
#include "storage/latch.h"
#include "storage/s_lock.h"

#define polar_parallel_bgwriter_enabled() \
	(polar_flush_list_enabled() && polar_parallel_flush_workers)

#define CURRENT_PARALLEL_WORKERS \
	(pg_atomic_read_u32(&polar_parallel_bgwriter_info->current_workers))

#define DOUBLE_GUC_BGWRITER_WORKERS \
	(2 * polar_parallel_flush_workers)

typedef enum parallel_flush_type_t
{
	INVALID_TASK = -1,
	FLUSHLIST_TASK,
	FLUSHLRU_TASK,
	MAX_FLUSH_TASK
} parallel_flush_type_t;

typedef enum flush_work_status_t
{
	FLUSH_IDLE = 0,
	FLUSH_WORK
} flush_work_status_t;

/* Server can have 2 * polar_parallel_flush_workers workers at most */
#define POLAR_MAX_BGWRITER_WORKERS \
	(DOUBLE_GUC_BGWRITER_WORKERS < MAX_NUM_OF_PARALLEL_BGWRITER ? DOUBLE_GUC_BGWRITER_WORKERS : MAX_NUM_OF_PARALLEL_BGWRITER)

typedef struct polar_flush_work_t
{
	BackgroundWorkerHandle handle;
	Latch	   *latch;
	pg_atomic_uint32 state;
	parallel_flush_type_t task;
} polar_flush_work_t;

typedef struct ParallelBgwriterInfo
{
	polar_flush_work_t polar_flush_work_info[MAX_NUM_OF_PARALLEL_BGWRITER];

	/* Spinlock: protects values below */
	slock_t		lock;
	parallel_flush_type_t flush_task[MAX_NUM_OF_PARALLEL_BGWRITER];
	int			read_worker_idx;
	int			write_worker_idx;
	pg_atomic_uint32 flush_workers[MAX_FLUSH_TASK];
	pg_atomic_uint32 current_workers;

} ParallelBgwriterInfo;

extern ParallelBgwriterInfo *polar_parallel_bgwriter_info;

extern void polar_init_parallel_bgwriter(void);
extern Size polar_parallel_bgwriter_shmem_size(void);
extern void polar_parallel_bgwriter_worker_main(Datum main_arg);
extern void polar_launch_parallel_bgwriter_workers(void);
extern void polar_register_parallel_bgwriter_workers(int workers);
extern void polar_shutdown_parallel_bgwriter_workers(int workers);
extern void polar_check_parallel_bgwriter_worker(void);
extern bool polar_new_parallel_bgwriter_useful(void);
#endif							/* PALAR_PARALLEL_BGWRITER_H */
