/*-------------------------------------------------------------------------
 *
 * test_procpool.c
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
 *	  src/test/modules/test_procpool/test_procpool.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <time.h>
#include <stdlib.h>
#include <unistd.h>

#include "lib/rbtree.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/polar_procpool.h"
#include "utils/guc.h"
#include "utils/polar_log.h"

#ifdef Assert
#undef Assert
#endif
#define Assert(condition) POLAR_ASSERT_PANIC(condition)

PG_MODULE_MAGIC;

#define TEST_MAX_TASK_NODES_NUM (8)
#define TEST_TASK_QUEUE_DEPTH (32)

typedef struct test_calc_task_node_t
{
	polar_task_node_t node;
	uint32		key;
	uint32		index;
	uint64		value;
}			test_calc_task_node_t;

static shmem_request_hook_type prev_shmem_request_hook = NULL;
static shmem_startup_hook_type prev_shmem_startup_hook = NULL;

void		_PG_init(void);

static bool
test_node_handle_task(polar_task_sched_t *sched, polar_task_node_t *task)
{
	test_calc_task_node_t *task_node = (test_calc_task_node_t *) task;
	uint64		i = task_node->index;

	task_node->value = i * (i + 1);

	return true;
}

static void
test_node_task_finished(polar_task_node_t *task, void *arg)
{
	test_calc_task_node_t *task_node = (test_calc_task_node_t *) task;
	uint64	   *total = (uint64 *) arg;

	*total += task_node->value;
}

static void *
test_node_get_tag(polar_task_node_t *task)
{
	test_calc_task_node_t *task_node = (test_calc_task_node_t *) task;

	return &task_node->key;
}

static uint32
test_tag_hash(const void *key, Size keysize)
{
	const uint32 i = *(const uint32 *) key;

	return i % 32;
}

static int
test_tag_compare(const void *key1, const void *key2, Size keysize)
{
	const uint32 k1 = *(const uint32 *) key1;
	const uint32 k2 = *(const uint32 *) key2;

	return k1 - k2;
}

static void
test_calc_startup(void *arg)
{
	elog(LOG, "Call startup function");
}

static bool
test_calc_cleanup(void *arg)
{
	elog(LOG, "Call cleanup function");

	return true;
}

static uint64 total = 0;

static polar_task_sched_ctl_t *
test_node_create_task_ctl(void)
{
	polar_task_sched_ctl_t *ctl = NULL;
	polar_task_sched_t *sched;
	MemoryContext oldcontext;

	sched = polar_create_proc_task_sched("polar_test_procpool",
										 TEST_MAX_TASK_NODES_NUM,
										 sizeof(test_calc_task_node_t),
										 TEST_TASK_QUEUE_DEPTH, &total);

	polar_sched_reg_handler(sched, test_calc_startup, test_node_handle_task, test_calc_cleanup, test_node_get_tag);

	oldcontext = MemoryContextSwitchTo(TopMemoryContext);
	ctl = polar_create_task_sched_ctl(sched, sizeof(uint32), test_tag_hash, test_tag_compare);
	MemoryContextSwitchTo(oldcontext);

	polar_sched_ctl_reg_handler(ctl, test_node_task_finished, &total);

	return ctl;
}

static uint64
test_multi_calc(polar_task_sched_ctl_t *ctl, int max_calc_num)
{
	uint32		i = 1;
	uint64		n = max_calc_num;
	uint64		expected = n * (n + 1) * (n + 2) / 3;
	test_calc_task_node_t node;
	int			rc;
	int			removed;

	total = 0;

	do
	{
		bool		added;

		node.key = i % 256;
		node.index = i;
		node.value = 0;

		added = polar_sched_add_task(ctl, (polar_task_node_t *) &node);

		if (added)
			i++;

		removed = polar_sched_remove_finished_task(ctl);

		if (!added && removed <= 0)
		{
			rc = WaitLatch(MyLatch, WL_LATCH_SET | WL_POSTMASTER_DEATH,
						   -1, WAIT_EVENT_POLAR_SUB_TASK_MAIN);
			Assert(!(rc & WL_POSTMASTER_DEATH));

			if (rc & WL_LATCH_SET)
				ResetLatch(MyLatch);
		}
	}
	while (i <= max_calc_num);

	while (!polar_sched_empty_running_task(ctl))
	{
		removed = polar_sched_remove_finished_task(ctl);

		if (removed <= 0)
		{
			rc = WaitLatch(MyLatch, WL_LATCH_SET | WL_POSTMASTER_DEATH,
						   -1, WAIT_EVENT_POLAR_SUB_TASK_MAIN);
			Assert(!(rc & WL_POSTMASTER_DEATH));

			if (rc & WL_LATCH_SET)
				ResetLatch(MyLatch);
		}
	}

	Assert(total == expected);

	return total;
}

PG_FUNCTION_INFO_V1(test_procpool);
/*
 * SQL-callable entry point to perform all tests.
 *
 * If a 1% false positive threshold is not met, emits WARNINGs.
 *
 * See README for details of arguments.
 */
Datum
test_procpool(PG_FUNCTION_ARGS)
{
	int			max_calc_num = PG_GETARG_INT32(0);
	uint64		value;

	polar_task_sched_ctl_t *ctl = test_node_create_task_ctl();

	polar_start_proc_pool(ctl);

	value = test_multi_calc(ctl, max_calc_num);

	polar_release_task_sched_ctl(ctl);

	return value;
}

static void
test_procpool_shmem_request(void)
{
	Size		sz = 0;

	if (prev_shmem_request_hook)
		prev_shmem_request_hook();

	sz = polar_calc_task_sched_shmem_size(TEST_MAX_TASK_NODES_NUM,
										  sizeof(test_calc_task_node_t),
										  TEST_TASK_QUEUE_DEPTH);
	RequestAddinShmemSpace(sz);
}

static void
test_procpool_shmem_startup(void)
{
	polar_task_sched_t *sched;

	sched = polar_create_proc_task_sched("polar_test_procpool",
										 TEST_MAX_TASK_NODES_NUM,
										 sizeof(test_calc_task_node_t),
										 TEST_TASK_QUEUE_DEPTH,
										 &total);
	if (sched == NULL)
		ereport(PANIC, errmsg("failed to create procpool!"));
}

void
_PG_init(void)
{
	if (!process_shared_preload_libraries_in_progress)
		ereport(FATAL, errmsg("module should be in shared_preload_libraries"));

	prev_shmem_request_hook = shmem_request_hook;
	shmem_request_hook = test_procpool_shmem_request;
	prev_shmem_startup_hook = shmem_startup_hook;
	shmem_startup_hook = test_procpool_shmem_startup;
}
