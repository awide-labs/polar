/*-------------------------------------------------------------------------
 *
 * polar_monitor_logindex.c
 *
 * Copyright (c) 2023, Alibaba Group Holding Limited
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
 *	  external/polar_monitor/polar_monitor_logindex.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <stdio.h>
#include <stdlib.h>

#include "access/htup_details.h"
#include "access/polar_logindex_redo.h"
#include "access/polar_ringbuf.h"
#include "funcapi.h"
#include "storage/pg_shmem.h"
#include "storage/procarray.h"
#include "storage/shmem.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/pg_lsn.h"
#include "utils/polar_bitpos.h"

#define XLOG_QUEUE_INFO_COLUMN_SIZE 3
#define XLOG_QUEUE_STAT_DETIAL_COL_SIZE 10
#define XLOG_QUEUE_SLOTS_INFO_COLUMN_SIZE 5
static polar_ringbuf_slot_t *slots_info = NULL;
static uint64 rbuf_occupied;

/* Get logindex mem table size */
PG_FUNCTION_INFO_V1(polar_get_logindex_mem_tbl_size);
Datum
polar_get_logindex_mem_tbl_size(PG_FUNCTION_ARGS)
{
	log_idx_table_id_t mem_tbl_size = 0;

	if (polar_logindex_redo_instance && polar_logindex_redo_instance->wal_logindex_snapshot)
		mem_tbl_size = polar_logindex_mem_tbl_size(polar_logindex_redo_instance->wal_logindex_snapshot);
	return (Datum) mem_tbl_size;
}

/* Get normal logindex snapshot used mem table size */
PG_FUNCTION_INFO_V1(polar_used_logindex_mem_tbl_size);
Datum
polar_used_logindex_mem_tbl_size(PG_FUNCTION_ARGS)
{
	log_idx_table_id_t mem_tbl_size = 0;

	if (polar_logindex_redo_instance && polar_logindex_redo_instance->wal_logindex_snapshot)
		mem_tbl_size = polar_logindex_used_mem_tbl_size(polar_logindex_redo_instance->wal_logindex_snapshot);

	return mem_tbl_size;
}

/* Used in replica. Check whether wal receiver get xlog from xlog queue */
PG_FUNCTION_INFO_V1(polar_replica_use_xlog_queue);
Datum
polar_replica_use_xlog_queue(PG_FUNCTION_ARGS)
{
	bool		used = false;

	if (polar_is_replica() && WalRcv)
	{
		SpinLockAcquire(&WalRcv->mutex);
		used = WalRcv->polar_use_xlog_queue;
		SpinLockRelease(&WalRcv->mutex);
	}

	return (Datum) used;
}

/* Used in replica.Get background process replayed lsn */
PG_FUNCTION_INFO_V1(polar_replica_bg_replay_lsn);
Datum
polar_replica_bg_replay_lsn(PG_FUNCTION_ARGS)
{
	XLogRecPtr	bg_lsn = polar_bg_redo_get_replayed_lsn(polar_logindex_redo_instance);

	PG_RETURN_LSN(bg_lsn);
}

/*
 * Used in replica and calculate min LSN used by replica
 * backends or background process
 */
PG_FUNCTION_INFO_V1(polar_replica_min_used_lsn);
Datum
polar_replica_min_used_lsn(PG_FUNCTION_ARGS)
{
	XLogRecPtr	min_lsn = InvalidXLogRecPtr;

	min_lsn = polar_get_read_min_lsn(polar_get_primary_consistent_lsn());

	PG_RETURN_LSN(min_lsn);
}

/*
 * Used in primary and calculate min LSN used by cluster.
 * The WAL and logindex which LSN is less than min LSN
 * can be removed
 */
PG_FUNCTION_INFO_V1(polar_min_used_lsn);
Datum
polar_min_used_lsn(PG_FUNCTION_ARGS)
{
	XLogRecPtr	min_lsn = polar_calc_min_used_lsn(true);

	PG_RETURN_LSN(min_lsn);
}

/*
 * return the current XLOG queue free ratio.
 */
PG_FUNCTION_INFO_V1(xlog_queue_stat_info);
Datum
xlog_queue_stat_info(PG_FUNCTION_ARGS)
{
	TupleDesc	tupdesc;
	Datum		values[XLOG_QUEUE_INFO_COLUMN_SIZE];
	bool		nulls[XLOG_QUEUE_INFO_COLUMN_SIZE];
	Datum		result;
	HeapTuple	tuple;

	int64		free_size = 0;
	int64		total_size = 0;
	ssize_t		ret;
	double		xlog_queue_free_ratio = 0.0;


	/*
	 * if the polar_xlog_queue_buffers <= 0, the logindex is disabled.
	 */
	if (polar_xlog_queue_buffers <= 0)
		PG_RETURN_NULL();


	tupdesc = CreateTemplateTupleDesc(XLOG_QUEUE_INFO_COLUMN_SIZE);
	TupleDescInitEntry(tupdesc, (AttrNumber) 1, "xlog_queue_total_size", INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 2, "xlog_queue_free_size", INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 3, "xlog_queue_free_ratio", FLOAT8OID, -1, 0);
	tupdesc = BlessTupleDesc(tupdesc);

	MemSet(nulls, 0, sizeof(nulls));

	/*
	 * when logindex is on, use the polar_ringbuf_free_size func to calc free
	 * size
	 */
	if (polar_logindex_redo_instance != NULL)
	{
		ret = polar_ringbuf_free_size(polar_logindex_redo_instance->xlog_queue);
		free_size = (int64) ret;
	}
	total_size = polar_xlog_queue_buffers * 1024L * 1024L;
	xlog_queue_free_ratio = free_size * 1.0 / total_size;

	values[0] = Int64GetDatum(total_size);
	values[1] = Int64GetDatum(free_size);
	values[2] = Float8GetDatum(xlog_queue_free_ratio);

	tuple = heap_form_tuple(tupdesc, values, nulls);
	result = HeapTupleGetDatum(tuple);

	PG_RETURN_DATUM(result);
}

/*
 * return the xlog queue detail statitics
 */
PG_FUNCTION_INFO_V1(polar_xlog_queue_stat_detail);
Datum
polar_xlog_queue_stat_detail(PG_FUNCTION_ARGS)
{
	TupleDesc	tupdesc;
	Datum		values[XLOG_QUEUE_STAT_DETIAL_COL_SIZE];
	bool		nulls[XLOG_QUEUE_STAT_DETIAL_COL_SIZE];
	Datum		result;
	HeapTuple	tuple;

	int64		push_cnt = 0;
	int64		pop_cnt = 0;
	int64		free_up_cnt = 0;
	int64		send_phys_io_cnt = 0;
	int64		total_written = 0;
	int64		total_read = 0;
	int64		evict_ref_cnt = 0;
	int64		queue_pwrite = 0;
	int64		queue_pread = 0;
	int64		queue_visit = 0;

	if (polar_xlog_queue_buffers <= 0)
		PG_RETURN_NULL();

	tupdesc = CreateTemplateTupleDesc(XLOG_QUEUE_STAT_DETIAL_COL_SIZE);
	TupleDescInitEntry(tupdesc, (AttrNumber) 1, "xlog_queue_push_cnt", INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 2, "xlog_queue_pop_cnt", INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 3, "xlog_queue_free_up_cnt", INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 4, "send_phys_io_cnt", INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 5, "total_written", INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 6, "total_read", INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 7, "evict_ref_cnt", INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 8, "queue_pwrite", INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 9, "queue_pread", INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 10, "queue_visit", INT8OID, -1, 0);
	tupdesc = BlessTupleDesc(tupdesc);

	MemSet(nulls, 0, sizeof(nulls));

	if (polar_logindex_redo_instance != NULL)
	{
		polar_ringbuf_t queue = polar_logindex_redo_instance->xlog_queue;
		int64		min_visit;

		LWLockAcquire(&queue->lock, LW_SHARED);
		min_visit = (int64) queue->min_visit;
		LWLockRelease(&queue->lock);

		push_cnt = pg_atomic_read_u64(&queue->prs.push_cnt);
		pop_cnt = min_visit - pg_atomic_read_u64(&queue->prs.prev_pop_cnt);
		free_up_cnt = pg_atomic_read_u64(&queue->prs.free_up_cnt);
		send_phys_io_cnt = pg_atomic_read_u64(&queue->prs.send_phys_io_cnt);
		total_written = pg_atomic_read_u64(&queue->prs.total_written);
		total_read = pg_atomic_read_u64(&queue->prs.total_read);
		evict_ref_cnt = pg_atomic_read_u64(&queue->prs.evict_ref_cnt);
		queue_pwrite = pg_atomic_read_u64(&queue->pwrite);
		queue_pread = pg_atomic_read_u64(&queue->pread);
		queue_visit = min_visit;
	}

	values[0] = Int64GetDatum(push_cnt);
	values[1] = Int64GetDatum(pop_cnt);
	values[2] = Int64GetDatum(free_up_cnt);
	values[3] = Int64GetDatum(send_phys_io_cnt);
	values[4] = Int64GetDatum(total_written);
	values[5] = Int64GetDatum(total_read);
	values[6] = Int64GetDatum(evict_ref_cnt);
	values[7] = Int64GetDatum(queue_pwrite);
	values[8] = Int64GetDatum(queue_pread);
	values[9] = Int64GetDatum(queue_visit);

	tuple = heap_form_tuple(tupdesc, values, nulls);
	result = HeapTupleGetDatum(tuple);

	PG_RETURN_DATUM(result);
}

/*
 * polar_get_xlog_queue_ref_info_func
 *
 * Get all xlog queue reference infomation
 */
PG_FUNCTION_INFO_V1(polar_get_xlog_queue_ref_info_func);
Datum
polar_get_xlog_queue_ref_info_func(PG_FUNCTION_ARGS)
{
	FuncCallContext *fctx;
	TupleDesc	tupdesc;
	polar_ringbuf_t queue;
	int			i;

	if (polar_xlog_queue_buffers <= 0 || !polar_logindex_redo_instance)
		PG_RETURN_NULL();

	/* Build a tuple descriptor for our result type */
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	if (SRF_IS_FIRSTCALL())
	{
		MemoryContext mctx;

		fctx = SRF_FIRSTCALL_INIT();

		mctx = MemoryContextSwitchTo(fctx->multi_call_memory_ctx);
		queue = polar_logindex_redo_instance->xlog_queue;
		slots_info = (polar_ringbuf_slot_t *) palloc0(sizeof(polar_ringbuf_slot_t) * POLAR_RINGBUF_MAX_SLOT);

		LWLockAcquire(&queue->lock, LW_SHARED);
		rbuf_occupied = queue->occupied;
		memcpy(slots_info, queue->slot, sizeof(polar_ringbuf_slot_t) * POLAR_RINGBUF_MAX_SLOT);
		LWLockRelease(&queue->lock);

		fctx->max_calls = POLAR_RINGBUF_MAX_SLOT;
		MemoryContextSwitchTo(mctx);
	}

	fctx = SRF_PERCALL_SETUP();

	if (fctx->call_cntr < fctx->max_calls)
	{
		HeapTuple	resultTuple;
		Datum		result;
		Datum		values[XLOG_QUEUE_SLOTS_INFO_COLUMN_SIZE];
		bool		nulls[XLOG_QUEUE_SLOTS_INFO_COLUMN_SIZE];

		memset(nulls, 0, sizeof(nulls));
		if (rbuf_occupied)
		{
			POLAR_BIT_LEAST_POS(rbuf_occupied, i);
			i--;
			values[0] = CStringGetTextDatum(slots_info[i].ref_name);
			values[1] = UInt64GetDatum(slots_info[i].pread);
			values[2] = UInt64GetDatum(slots_info[i].visit);
			values[3] = BoolGetDatum(slots_info[i].strong);
			values[4] = Int32GetDatum(slots_info[i].ref_num);
			POLAR_BIT_CLEAR_LEAST(rbuf_occupied);
		}
		else
			memset(nulls, 1, sizeof(nulls));

		/* Build and return the result tuple. */
		resultTuple = heap_form_tuple(tupdesc, values, nulls);
		result = HeapTupleGetDatum(resultTuple);

		SRF_RETURN_NEXT(fctx, result);
	}
	else
		SRF_RETURN_DONE(fctx);

}
