/*-------------------------------------------------------------------------
 *
 * polar_monitor_proxy.c
 *	  views of PolarDB proxy
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
 *    external/polar_monitor/polar_monitor_proxy.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "funcapi.h"
#include "utils/backend_status.h"
#include "utils/builtins.h"

static char *polar_stat_proxy_infos[] = {
	"total",
	"splittable",
	"disablesplit",
	"unsplittable",
	"error",
	"lock",
	"combocid",
	"createenum",
	"autoxact",
	"readimplicit",
	"readbeforewrite",
	"readafterwrite",
};

/*
 * POLAR: return the proxy stat info
 */
PG_FUNCTION_INFO_V1(polar_stat_proxy_info);
Datum
polar_stat_proxy_info(PG_FUNCTION_ARGS)
{
#define POLARPROXYSTATSIZE 2
	int			i = 0;
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	TupleDesc	tupdesc;
	MemoryContext per_query_ctx;
	MemoryContext oldcontext;
	Tuplestorestate *tupstore;
	Datum		values[POLARPROXYSTATSIZE];
	bool		nulls[POLARPROXYSTATSIZE];

	StaticAssertStmt((sizeof(polar_stat_proxy_infos) / sizeof(char *)) ==
					 (sizeof(PolarStat_Proxy) / sizeof(uint64)), "polar_stat_proxy_infos should match PolarStat_Proxy");

	MemSet(nulls, 0, sizeof(nulls));

	tupdesc = CreateTemplateTupleDesc(POLARPROXYSTATSIZE);
	TupleDescInitEntry(tupdesc, (AttrNumber) 1, "reason", TEXTOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 2, "count", INT8OID, -1, 0);

	per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
	oldcontext = MemoryContextSwitchTo(per_query_ctx);

	tupstore = tuplestore_begin_heap(true, false, work_mem);
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = tupstore;
	rsinfo->setDesc = tupdesc;

	MemoryContextSwitchTo(oldcontext);

	for (i = 0; i < sizeof(polar_stat_proxy_infos) / sizeof(char *); ++i)
	{
		values[0] = CStringGetTextDatum(polar_stat_proxy_infos[i]);
		values[1] = UInt64GetDatum((&polar_stat_proxy->proxy_total)[i]);
		tuplestore_putvalues(tupstore, tupdesc, values, nulls);
	}

	/* clean up and return the tuplestore */
	tuplestore_donestoring(tupstore);

	return (Datum) 0;
}

/*
 * POLAR: reset the proxy stat info
 */
PG_FUNCTION_INFO_V1(polar_stat_reset_proxy_info);
Datum
polar_stat_reset_proxy_info(PG_FUNCTION_ARGS)
{
	MemSet(polar_stat_proxy, 0, sizeof(PolarStat_Proxy));
	PG_RETURN_NULL();
}

/*
 * POLAR: get real pid and virtual pid
 */
PG_FUNCTION_INFO_V1(polar_stat_get_pid);
Datum
polar_stat_get_pid(PG_FUNCTION_ARGS)
{
	int			pid = PG_ARGISNULL(0) ? MyProcPid : PG_GETARG_INT32(0);

	if (!PG_ARGISNULL(0) && !POLAR_IS_PROXY_SID(pid))
		elog(ERROR, "POLAR: Invalid virtual pid: %d, should between (10^7, INT_MAX]", pid);
	PG_RETURN_INT32(polar_proxy_get_pid(pid, 0, false));
}

PG_FUNCTION_INFO_V1(polar_stat_get_sid);
Datum
polar_stat_get_sid(PG_FUNCTION_ARGS)
{
	int			pid = PG_ARGISNULL(0) ? MyProcPid : PG_GETARG_INT32(0);

	if (!PG_ARGISNULL(0) && !POLAR_IS_PID(pid))
		elog(ERROR, "POLAR: Invalid real pid: %d, should between (0, 10^7)", pid);
	PG_RETURN_INT32(polar_proxy_get_sid(pid, NULL));
}
