/*-------------------------------------------------------------------------
 *
 * test_coredump_handler.c
 *	  unit test for coredump handler
 *
 * Copyright (c) 2024, Alibaba Group Holding Limited
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
 *	  src/test/modules/test_coredump_handler/test_coredump_handler.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <signal.h>

#include "utils/builtins.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(test_sigsegv);
Datum
test_sigsegv(PG_FUNCTION_ARGS)
{
	char	   *ptr = NULL;

	*ptr = 'c';

	PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(test_panic);
Datum
test_panic(PG_FUNCTION_ARGS)
{
	elog(PANIC, "TEST PANIC");

	PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(test_backtrace);
Datum
test_backtrace(PG_FUNCTION_ARGS)
{
	ereport(LOG,
			(errbacktrace(),
			 errmsg("TEST BACKTRACE")));

	PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(test_assert);
Datum
test_assert(PG_FUNCTION_ARGS)
{
	Assert(false);

	PG_RETURN_VOID();
}
