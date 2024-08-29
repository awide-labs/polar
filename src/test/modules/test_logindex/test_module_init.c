/*-------------------------------------------------------------------------
 *
 * test_module_init.c
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
 *	  src/test/modules/test_logindex/test_module_init.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <time.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "fmgr.h"
#include "miscadmin.h"
#include "storage/ipc.h"
#include "storage/shmem.h"
#include "test_module_init.h"

PG_MODULE_MAGIC;

static shmem_request_hook_type prev_shmem_request_hook = NULL;
static shmem_startup_hook_type prev_shmem_startup_hook = NULL;

static void
test_local_cache_shmem_request(void)
{
	if (prev_shmem_request_hook)
		prev_shmem_request_hook();

	RequestAddinShmemSpace(test_logindex_request_shmem_size());
	RequestAddinShmemSpace(test_mini_trans_request_shmem_size());
	RequestAddinShmemSpace(test_rel_size_cache_request_shmem_size());
	RequestAddinShmemSpace(test_ringbuf_request_shmem_size());
}

static void
test_local_cache_shmem_startup(void)
{
	if (prev_shmem_startup_hook)
		prev_shmem_startup_hook();

	test_logindex_shmem_startup();
	test_mini_trans_shmem_startup();
	test_rel_size_cache_shmem_startup();
	test_ringbuf_shmem_startup();
}

void
_PG_init(void)
{
	if (!process_shared_preload_libraries_in_progress)
		ereport(FATAL, errmsg("module should be in shared_preload_libraries"));

	prev_shmem_request_hook = shmem_request_hook;
	shmem_request_hook = test_local_cache_shmem_request;
	prev_shmem_startup_hook = shmem_startup_hook;
	shmem_startup_hook = test_local_cache_shmem_startup;
}
