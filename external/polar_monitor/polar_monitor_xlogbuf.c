/*-------------------------------------------------------------------------
 *
 * polar_monitor_xlogbuf.c
 *	  xlog buffer monitor
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
 *	  external/polar_monitor/polar_monitor_xlogbuf.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <stdio.h>
#include <stdlib.h>

#include "access/htup_details.h"
#include "funcapi.h"
#include "storage/pg_shmem.h"
#include "storage/polar_xlogbuf.h"
#include "storage/procarray.h"
#include "storage/shmem.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/pg_lsn.h"

/*
 * return the xlog buffer detail statitics
 */
PG_FUNCTION_INFO_V1(polar_xlog_buffer_stat_info);
Datum
polar_xlog_buffer_stat_info(PG_FUNCTION_ARGS)
{
#define XLOG_BUFFER_STAT_INFO_COL_SIZE 4
	TupleDesc	tupdesc;
	Datum		values[XLOG_BUFFER_STAT_INFO_COL_SIZE];
	bool		nulls[XLOG_BUFFER_STAT_INFO_COL_SIZE];
	Datum		result;
	HeapTuple	tuple;

	int64		hit_count = 0;
	int64		io_count = 0;
	int64		others_append_count = 0;
	int64		startup_append_count = 0;

	if (!polar_xlog_buffer_ins)
		PG_RETURN_NULL();

	tupdesc = CreateTemplateTupleDesc(XLOG_BUFFER_STAT_INFO_COL_SIZE);
	TupleDescInitEntry(tupdesc, (AttrNumber) 1, "hit_count", INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 2, "io_count", INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 3, "others_append_count", INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 4, "startup_append_count", INT8OID, -1, 0);
	tupdesc = BlessTupleDesc(tupdesc);

	MemSet(nulls, 0, sizeof(nulls));

	hit_count = pg_atomic_read_u64(&polar_xlog_buffer_ins->hit_count);
	io_count = pg_atomic_read_u64(&polar_xlog_buffer_ins->io_count);
	others_append_count = pg_atomic_read_u64(&polar_xlog_buffer_ins->others_append_count);
	startup_append_count = pg_atomic_read_u64(&polar_xlog_buffer_ins->startup_append_count);

	values[0] = Int64GetDatum(hit_count);
	values[1] = Int64GetDatum(io_count);
	values[2] = Int64GetDatum(others_append_count);
	values[3] = Int64GetDatum(startup_append_count);

	tuple = heap_form_tuple(tupdesc, values, nulls);
	result = HeapTupleGetDatum(tuple);

	PG_RETURN_DATUM(result);
}

/*
 * polar_xlog_buffer_stat_reset
 *
 * Reset all the polar xlog buffer stat counters.
 */
PG_FUNCTION_INFO_V1(polar_xlog_buffer_stat_reset);
Datum
polar_xlog_buffer_stat_reset(PG_FUNCTION_ARGS)
{
	if (!polar_xlog_buffer_ins)
		PG_RETURN_VOID();

	pg_atomic_write_u64(&polar_xlog_buffer_ins->hit_count, 0);
	pg_atomic_write_u64(&polar_xlog_buffer_ins->io_count, 0);
	pg_atomic_write_u64(&polar_xlog_buffer_ins->others_append_count, 0);
	pg_atomic_write_u64(&polar_xlog_buffer_ins->startup_append_count, 0);

	PG_RETURN_VOID();
}
