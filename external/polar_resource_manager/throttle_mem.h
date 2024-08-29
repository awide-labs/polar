/*-------------------------------------------------------------------------
 *
 * throttle_mem.h
 *	  API to get memory statistics.
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
 *	  external/polar_resource_manager/throttle_mem.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef THROTTLE_MEM_H
#define THROTTLE_MEM_H

#include "postgres.h"

#include "access/twophase.h"
#include "miscadmin.h"
#include "storage/procarray.h"


/*
 * POLAR: check exceed resource manager at most 16 each time.
 */
#define MAXSTATLEN MAXPGPATH
#define PidIsInvalid(objectId)  ((bool) ((objectId) == InvalidPid))
#define PidIsValid(objectId)  ((bool) ((objectId) != InvalidPid))

#define CGROUPMEMFILE "memory.stat"

extern char polar_cgroup_mem_path[MAXPGPATH];

extern FILE *polar_get_statfd_by_pid(int pid);
extern FILE *polar_get_statmfd_by_pid(int pid);
extern int	polar_get_procrss_by_name(char *procname, int *pid, Size *rss);
extern int	polar_get_procrss_by_pidstat(int pid, Size *rss);
extern int	polar_get_procrss_by_pidstatm(int pid, int procflag, Size *rss);
extern int	polar_get_ins_memorystat(Size *rss, Size *mapped_file, Size *limit);

extern void terminate_backend(PolarProcStatm *cur_proc, Size *rss_release);
extern void cancel_query(PolarProcStatm *cur_proc, Size *rss_release);
extern Size mem_release(PolarProcStatm *allprocs, int num_allprocs, Size exceed_size, int policy);

#endif							/* THROTTLE_MEM_H */
