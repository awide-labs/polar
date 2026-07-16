/*-------------------------------------------------------------------------
 *
 * polar_tools.h
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
 *	  src/bin/polar_tools/polar_tools.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef POLAR_TOOLS_H
#define POLAR_TOOLS_H

#include "postgres.h"

#include "common/fe_memutils.h"
#include "common/logging.h"
#include "getopt_long.h"

extern int	block_header_dump_main(int argc, char **argv);
extern int	control_data_change_main(int argc, char **argv);
extern int	logindex_meta_main(int argc, char **argv);
extern int	logindex_bloom_main(int argc, char **argv);
extern int	logindex_table_main(int argc, char **argv);
extern int	logindex_page_main(int argc, char **argv);
extern int	datamax_meta_main(int argc, char **argv);
extern int	datamax_get_wal_main(int argc, char **argv);

#endif
