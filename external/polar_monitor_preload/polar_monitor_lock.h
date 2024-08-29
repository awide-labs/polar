/*-------------------------------------------------------------------------
 *
 * polar_monitor_lock.h
 *	  show lwlock and regular lock monitor
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
 *	  external/polar_monitor_preload/polar_monitor_lock.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef POLAR_MONITOR_LOCK_H
#define POLAR_MONITOR_LOCK_H

extern Size lwlock_stat_shmem_size(void);
extern Size lock_stat_shmem_size(void);
extern void polar_lock_stat_shmem_startup(void);

#endif							/* POLAR_MONITOR_LOCK_H */
