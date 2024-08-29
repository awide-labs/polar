/*-------------------------------------------------------------------------
 *
 * procstat.h
 *	  wrapper for get information from /proc/pid/ files
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
 *	  external/polar_monitor/procstat.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef PROCSTAT_H
#define PROCSTAT_H

#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <time.h>
#include <linux/limits.h>
#include <sys/times.h>

typedef enum PROCARGTYPE
{
	PROCNUM = 0,
	PROCSTR = 1,
	PROCL = 2,
	PROCLL = 3,
	PROCULL = 4,
	PROCINT = 5
} PROCARGTYPE;

typedef struct polar_proc_stat
{
	uint64_t	utime;			/** user mode jiffies **/
	uint64_t	stime;			/** kernel mode jiffies **/
	uint64_t	rss;			/** RssAnon + RssFile + RssShare **/
	uint64_t	share;			/** RssFile + RssShare */
} polar_proc_stat;

extern int	polar_get_proc_stat(int pid, polar_proc_stat *stat);
#endif							/* PROCSTAT_H */
