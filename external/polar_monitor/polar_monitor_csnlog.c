/*-------------------------------------------------------------------------
 *
 * polar_monitor_csnlog.c
 *	  display some information of polardb
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
 *	  contrib/polar_monitor/polar_monitor_csnlog.c
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "catalog/pg_type.h"
#include "funcapi.h"
#include "access/polar_csnlog.h"
#include "utils/guc.h"

PG_FUNCTION_INFO_V1(polar_csnlog);

#define COLUMN_SIZE 3

Datum
polar_csnlog(PG_FUNCTION_ARGS)
{
	TupleDesc	tupdesc;
	Datum		values[COLUMN_SIZE];
	bool		nulls[COLUMN_SIZE];
	polar_csnlog_ub_stat *ub_stat;

	if (!polar_csnlog_upperbound_enable)
		PG_RETURN_NULL();

	tupdesc = CreateTemplateTupleDesc(COLUMN_SIZE);
	TupleDescInitEntry(tupdesc, (AttrNumber) 1, "all_fetches", INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 2, "ub_fetches", INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 3, "ub_hits", INT8OID, -1, 0);
	tupdesc = BlessTupleDesc(tupdesc);

	ub_stat = polar_csnlog_get_upperbound_stat_ptr();

	MemSet(nulls, 0, sizeof(nulls));

	values[0] = UInt64GetDatum(pg_atomic_read_u64(&ub_stat->t_all_fetches));
	values[1] = UInt64GetDatum(pg_atomic_read_u64(&ub_stat->t_ub_fetches));
	values[2] = UInt64GetDatum(pg_atomic_read_u64(&ub_stat->t_ub_hits));

	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}
