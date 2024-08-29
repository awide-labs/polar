/*-------------------------------------------------------------------------
 *
 * test_module_init.h
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
 *	  src/test/modules/test_logindex/test_module_init.h
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include "utils/polar_log.h"

extern Size test_logindex_request_shmem_size(void);
extern Size test_mini_trans_request_shmem_size(void);
extern Size test_rel_size_cache_request_shmem_size(void);
extern Size test_ringbuf_request_shmem_size(void);

extern void test_logindex_shmem_startup(void);
extern void test_mini_trans_shmem_startup(void);
extern void test_rel_size_cache_shmem_startup(void);
extern void test_ringbuf_shmem_startup(void);

extern void _PG_init(void);

#ifdef Assert
#undef Assert
#endif
#define Assert(condition) POLAR_ASSERT_PANIC(condition)
