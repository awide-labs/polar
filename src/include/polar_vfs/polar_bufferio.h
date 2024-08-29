/*-------------------------------------------------------------------------
 *
 * polar_bufferio.h
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
 *    src/include/polar_vfs/polar_bufferio.h
 *
 *
 *-------------------------------------------------------------------------
 */
#ifndef POLAR_BUFFERIO_H
#define POLAR_BUFFERIO_H

#include <unistd.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <fcntl.h>
#include "postgres.h"
#ifndef FRONTEND
#include "funcapi.h"
#endif

#include "storage/fd.h"
#include "storage/polar_fd.h"

extern const vfs_mgr polar_vfs_bio;

#endif
