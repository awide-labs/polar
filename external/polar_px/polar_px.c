/*-------------------------------------------------------------------------
 *
 * polar_px.c
 *	  POLAR_PX install polar_px_workerid function into pg_proc
 *	  for px subselect.
 *
 * Copyright (c) 2020, Alibaba Group Holding Limited
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
 *	  external/polar_px/polar_px.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <unistd.h>

#include "access/hash.h"
#include "catalog/pg_type.h"
#include "executor/executor.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "postmaster/autovacuum.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/spin.h"
#include "utils/guc.h"
#include "utils/builtins.h"
#include "postmaster/postmaster.h"
#include "px/px_vars.h"
#include "px_optimizer_util/utils/funcs.h"

PG_MODULE_MAGIC;

/*--- Functions --- */

void		_PG_init(void);
void		_PG_fini(void);

void
_PG_init(void)
{
}

void
_PG_fini(void)
{
}

PG_FUNCTION_INFO_V1(polar_px_workerid);

Datum
polar_px_workerid(PG_FUNCTION_ARGS)
{
	int			workerid = get_px_workerid();

	PG_RETURN_INT32(workerid);
}

PG_FUNCTION_INFO_V1(polar_px_workerid_funcid);

Datum
polar_px_workerid_funcid(PG_FUNCTION_ARGS)
{
	uint32		funcid = get_px_workerid_funcid();

	PG_RETURN_INT32(funcid);
}

/*
 * Enables transformations in the optimizer.
 */
PG_FUNCTION_INFO_V1(enable_xform);
Datum
enable_xform(PG_FUNCTION_ARGS)
{
#ifdef USE_PX
	return EnableXform(fcinfo);
#else
	return CStringGetTextDatum("Server has been compiled without ORCA");
#endif
}

/*
 * Disables transformations in the optimizer.
 */
PG_FUNCTION_INFO_V1(disable_xform);
Datum
disable_xform(PG_FUNCTION_ARGS)
{
#ifdef USE_PX
	return DisableXform(fcinfo);
#else
	return CStringGetTextDatum("Server has been compiled without ORCA");
#endif
}

/*  pg15_px: TODO: remove function or return correct version */
#if 0
/*
* Returns the optimizer and px library versions.
*/
Datum
px_orca_version(PG_FUNCTION_ARGS pg_attribute_unused())
{
#ifdef USE_PX
	return LibraryVersion();
#else
	return CStringGetTextDatum("Server has been compiled without ORCA");
#endif
}
#endif
