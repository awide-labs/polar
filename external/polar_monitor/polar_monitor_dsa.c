/*-------------------------------------------------------------------------
 *
 * polar_monitor_dsa.c
 *    views of polardb dsa and ShmAllocSet.
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
 *    external/polar_monitor/polar_monitor_dsa.c
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "catalog/pg_type.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/memnodes.h"
#include "storage/lwlock.h"
#include "storage/polar_memutils.h"
#include "utils/builtins.h"
#include "utils/freepage.h"
#include "utils/guc.h"
#include "utils/memutils.h"

static Datum
build_dsa_name(ShmAsetType type)
{
	switch (type)
	{
		case SHM_ASET_TYPE_GPC:
			return CStringGetTextDatum("GPC");
		case SHM_ASET_TYPE_SHARED_SERVER:
			return CStringGetTextDatum("SHARED_SERVER");
		default:
			return (Datum) 0;
	}
}

PG_FUNCTION_INFO_V1(polar_stat_dsa);
Datum
polar_stat_dsa(PG_FUNCTION_ARGS)
{
#define STAT_GET_DSA_COLS	8
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	TupleDesc	tupdesc;
	Tuplestorestate *tupstore;
	MemoryContext per_query_ctx;
	MemoryContext oldcontext;
	Datum		values[STAT_GET_DSA_COLS];
	bool		nulls[STAT_GET_DSA_COLS];
	DSAContextCounters stat;
	dsa_area   *area;
	int			i;

	/* check to see if caller supports us returning a tuplestore */
	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that cannot accept a set")));
	if (!(rsinfo->allowedModes & SFRM_Materialize))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("materialize mode required, but it is not allowed in this context")));

	/* Build a tuple descriptor for our result type */
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
	oldcontext = MemoryContextSwitchTo(per_query_ctx);

	tupstore = tuplestore_begin_heap(true, false, work_mem);
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = tupstore;
	rsinfo->setDesc = tupdesc;

	MemoryContextSwitchTo(oldcontext);

	tupdesc = CreateTemplateTupleDesc(STAT_GET_DSA_COLS);
	TupleDescInitEntry(tupdesc, (AttrNumber) 1, "name", TEXTOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 2, "total_segment_size", INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 3, "pinned", BOOLOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 4, "ref_cnt", INT4OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 5, "usable_pages", INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 6, "max_contiguous_pages", INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 7, "max_total_segment_size", INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 8, "usable_pages_pct", FLOAT4OID, -1, 0);

	/* Gets the current DSA area allocation status */
	for (i = 0; i < SHM_ASET_TYPE_NUM; i++)
	{
		area = polar_shm_aset_get_area(i);
		if (area == NULL)
			continue;
		else
		{
			memset(&stat, 0, sizeof(stat));
			polar_dsa_monitor(area, &stat);

			/* Examine the context itself */
			memset(nulls, 0, sizeof(nulls));
			values[0] = build_dsa_name(i);
			values[1] = Int64GetDatum(stat.total_segment_size);
			values[2] = BoolGetDatum(stat.pinned);
			values[3] = Int32GetDatum(stat.refcnt);
			values[4] = Int64GetDatum(stat.usable_pages);
			values[5] = Int64GetDatum(stat.max_contiguous_pages);
			values[6] = Int64GetDatum(stat.max_total_segment_size);
			values[7] = Float4GetDatum((float) stat.usable_pages * FPM_PAGE_SIZE
									   / (float) stat.total_segment_size);

			tuplestore_putvalues(tupstore, tupdesc, values, nulls);
		}
	}

	/* clean up and return the tuplestore */
	tuplestore_donestoring(tupstore);

	return (Datum) 0;
}

PG_FUNCTION_INFO_V1(polar_shm_aset_ctl);
Datum
polar_shm_aset_ctl(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	TupleDesc	tupdesc;
	Tuplestorestate *tupstore;
	MemoryContext per_query_ctx;
	MemoryContext oldcontext;
	Datum		values[3];
	bool		nulls[3];
	int			i;

	if (!polar_shm_aset_enabled())
		PG_RETURN_NULL();

	/* check to see if caller supports us returning a tuplestore */
	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that cannot accept a set")));
	if (!(rsinfo->allowedModes & SFRM_Materialize))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("materialize mode required, but it is not allowed in this context")));

	/* Build a tuple descriptor for our result type */
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
	oldcontext = MemoryContextSwitchTo(per_query_ctx);

	tupstore = tuplestore_begin_heap(true, false, work_mem);
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = tupstore;
	rsinfo->setDesc = tupdesc;

	MemoryContextSwitchTo(oldcontext);

	tupdesc = CreateTemplateTupleDesc(3);
	TupleDescInitEntry(tupdesc, (AttrNumber) 1, "type", INT2OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 2, "name", TEXTOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 3, "size", INT8OID, -1, 0);

	/* Gets the current DSA area allocation status */
	for (i = 0; i < SHM_ASET_TYPE_NUM; i++)
	{
		ShmAsetCtl *ctl = &shm_aset_ctl[i];

		if (ctl == NULL)
			continue;

		MemSet(nulls, 0, sizeof(nulls));
		values[0] = Int16GetDatum(ctl->type);
		values[1] = build_dsa_name(ctl->type);
		values[2] = UInt64GetDatum(ctl->size);
		tuplestore_putvalues(tupstore, tupdesc, values, nulls);
	}

	/* clean up and return the tuplestore */
	tuplestore_donestoring(tupstore);

	return (Datum) 0;
}

PG_FUNCTION_INFO_V1(polar_dump_dsa);
Datum
polar_dump_dsa(PG_FUNCTION_ARGS)
{
	char	   *cstr = text_to_cstring(PG_GETARG_TEXT_PP(0));

	if (pg_strcasecmp("gpc", cstr) == 0)
		dsa_dump(polar_shm_aset_get_area(shm_aset_ctl[SHM_ASET_TYPE_GPC].type));

	if (pg_strcasecmp("shared_server", cstr) == 0)
		dsa_dump(polar_shm_aset_get_area(shm_aset_ctl[SHM_ASET_TYPE_SHARED_SERVER].type));

	PG_RETURN_VOID();
}
