/*-------------------------------------------------------------------------
 *
 * test_read_core_pattern.c
 *	  test file
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
 *	  external/polar_worker/test_read_core_pattern.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>


#include "fmgr.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "polar_worker.h"

#include "common/file_perm.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/lwlock.h"
#include "storage/polar_fd.h"
#include "storage/proc.h"
#include "storage/procsignal.h"


static void
test_core_pattern_path(void)
{
	int			fd;
	char		core_pattern[MAXPGPATH] = {0};
	char		core_file_path_temp[MAXPGPATH] = {0};
	char		core_file_target[MAXPGPATH] = {0};
	char		core_pattern_path[MAXPGPATH] = {0};
	char		cwd[MAXPGPATH] = {0};

	if (!getcwd(cwd, MAXPGPATH))
		exit(EXIT_FAILURE);
	snprintf(core_pattern_path, MAXPGPATH, "%s/polar_test_core_pattern", cwd);

	/* case1 */
	fd = open(core_pattern_path, O_CREAT | O_WRONLY | PG_BINARY, pg_file_create_mode);
	if (write(fd, &core_pattern, sizeof(core_pattern)) != sizeof(core_pattern))
		exit(EXIT_FAILURE);
	close(fd);
	polar_read_core_pattern(core_pattern_path, core_file_path_temp);
	Assert(!strcmp(core_file_path_temp, cwd));

	/* case2 */
	fd = open(core_pattern_path, O_CREAT | O_WRONLY | PG_BINARY, pg_file_create_mode);
	snprintf(core_pattern, MAXPGPATH, "core");
	if (write(fd, &core_pattern, sizeof(core_pattern)) != sizeof(core_pattern))
		exit(EXIT_FAILURE);
	close(fd);
	polar_read_core_pattern(core_pattern_path, core_file_path_temp);
	Assert(!strcmp(core_file_path_temp, cwd));

	/* case3 */
	fd = open(core_pattern_path, O_CREAT | O_WRONLY | PG_BINARY, pg_file_create_mode);
	snprintf(core_pattern, MAXPGPATH, "corefile/core");
	if (write(fd, &core_pattern, sizeof(core_pattern)) != sizeof(core_pattern))
		exit(EXIT_FAILURE);
	close(fd);
	polar_read_core_pattern(core_pattern_path, core_file_path_temp);
	snprintf(core_file_target, MAXPGPATH, "%s/corefile/", cwd);
	Assert(!strcmp(core_file_path_temp, core_file_target));

	/* case4 */
	fd = open(core_pattern_path, O_CREAT | O_WRONLY | PG_BINARY, pg_file_create_mode);
	snprintf(core_pattern, MAXPGPATH, "/tmp/corefile/core");
	if (write(fd, &core_pattern, sizeof(core_pattern)) != sizeof(core_pattern))
		exit(EXIT_FAILURE);
	close(fd);
	polar_read_core_pattern(core_pattern_path, core_file_path_temp);
	snprintf(core_file_target, MAXPGPATH, "/tmp/corefile/");
	Assert(!strcmp(core_file_path_temp, core_file_target));
}

PG_FUNCTION_INFO_V1(test_read_core_pattern);
/*
 *  The module is used to test function polar_read_core_pattern()
 */
Datum
test_read_core_pattern(PG_FUNCTION_ARGS)
{
	test_core_pattern_path();

	PG_RETURN_VOID();
}
