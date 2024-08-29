/*-------------------------------------------------------------------------
 *
 * polar_worker.h
 *
 *	  Do some backgroud things for PolarDB periodically. Such as:
 *	  (1) auto prealloc wal files,
 * 	  (2) auto clean core dump files,
 *	  (3) auto clean xlog temp files.
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
 *	  external/polar_worker/polar_worker.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef POLAR_WORKER_H
#define POLAR_WORKER_H

#include "postgres.h"

extern bool polar_read_core_pattern(const char *core_pattern_path, char *buf);

#endif							/* POLAR_WORKER_H */
