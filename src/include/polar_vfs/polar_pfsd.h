/*-------------------------------------------------------------------------
 *
 * polar_pfsd.h
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
 *    src/include/polar_vfs/polar_pfsd.h
 *
 *
 *-------------------------------------------------------------------------
 */
#ifndef POLAR_PFSD_H
#define POLAR_PFSD_H

#include <unistd.h>
#include <sys/time.h>
#include <sys/stat.h>
#include "postgres.h"
#ifndef FRONTEND
#include "funcapi.h"
#endif

#include "storage/fd.h"
#include "storage/polar_fd.h"


#define PFSD_MIN_MAX_IOSIZE         (4 * 1024)
#define PFSD_DEFAULT_MAX_IOSIZE (4 * 1024 * 1024)
#define PFSD_MAX_MAX_IOSIZE         (128 * 1024 * 1024)

extern int	max_pfsd_io_size;
extern const vfs_mgr polar_vfs_pfsd;

extern int	polar_pfsd_mount_growfs(const char *pbdname);
extern unsigned long polar_pfsd_meta_version_get(void);
extern const char *polar_pfsd_build_version_get(void);

#endif							/* POLAR_PFSD_H */
