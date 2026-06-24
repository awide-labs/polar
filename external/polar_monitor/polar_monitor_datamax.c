/*-------------------------------------------------------------------------
 *
 * polar_monitor_datamax.c
 *    display some information of polardb datamax
 *
 * Copyright (c) 2020, Alibaba Group Holding Limited
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
 *    external/polar_monitor/polar_monitor_datamax.c
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/htup_details.h"
#include "access/tupdesc.h"
#include "catalog/pg_type.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/execnodes.h"
#include "polar_datamax/polar_datamax.h"
#include "utils/pg_lsn.h"
#include "utils/tuplestore.h"

#define NUM_DATAMAX_INFO_ELEM   7

PG_FUNCTION_INFO_V1(polar_get_datamax_info);

Datum
polar_get_datamax_info(PG_FUNCTION_ARGS)
{
	int			i = 0;
	TupleDesc	tupdesc;
	AttrNumber	cols = 1;
	Datum		values[NUM_DATAMAX_INFO_ELEM];
	bool		nulls[NUM_DATAMAX_INFO_ELEM];

	if (!polar_is_datamax())
		elog(ERROR, "current db is not in DataMax mode.");

	tupdesc = CreateTemplateTupleDesc(NUM_DATAMAX_INFO_ELEM);
	TupleDescInitEntry(tupdesc, cols++, "min_received_timeline", INT4OID, -1, 0);
	TupleDescInitEntry(tupdesc, cols++, "min_received_lsn", LSNOID, -1, 0);
	TupleDescInitEntry(tupdesc, cols++, "last_received_timeline", INT4OID, -1, 0);
	TupleDescInitEntry(tupdesc, cols++, "last_received_lsn", LSNOID, -1, 0);
	TupleDescInitEntry(tupdesc, cols++, "last_valid_received_lsn", LSNOID, -1, 0);
	TupleDescInitEntry(tupdesc, cols++, "clean_reserved_lsn", LSNOID, -1, 0);
	TupleDescInitEntry(tupdesc, cols++, "force_clean", BOOLOID, -1, 0);
	tupdesc = BlessTupleDesc(tupdesc);

	MemSet(nulls, 0, sizeof(nulls));

	LWLockAcquire(&polar_datamax_ctl->meta_lock, LW_SHARED);
	values[i++] = UInt32GetDatum(polar_datamax_ctl->meta.min_timeline_id);
	values[i++] = LSNGetDatum(polar_datamax_ctl->meta.min_received_lsn);
	values[i++] = UInt32GetDatum(polar_datamax_ctl->meta.last_timeline_id);
	values[i++] = LSNGetDatum(polar_datamax_ctl->meta.last_received_lsn);
	values[i++] = LSNGetDatum(polar_datamax_ctl->meta.last_valid_received_lsn);
	LWLockRelease(&polar_datamax_ctl->meta_lock);

	SpinLockAcquire(&polar_datamax_ctl->lock);
	values[i++] = LSNGetDatum(polar_datamax_ctl->clean_task.reserved_lsn);
	values[i++] = BoolGetDatum(polar_datamax_ctl->clean_task.force);
	SpinLockRelease(&polar_datamax_ctl->lock);

	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}


PG_FUNCTION_INFO_V1(polar_set_datamax_reserved_lsn);

Datum
polar_set_datamax_reserved_lsn(PG_FUNCTION_ARGS)
{
	XLogRecPtr	reserved_lsn = PG_GETARG_LSN(0);
	bool		force = PG_GETARG_BOOL(1);

	if (!polar_is_datamax())
		elog(ERROR, "current db is not in DataMax mode.");

	PG_RETURN_BOOL(polar_datamax_set_clean_task(reserved_lsn, force));
}
