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
#include "access/polar_fullpage.h"
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
#define XLOG_QUEUE_STAT_DETIAL_COL_SIZE 5
#define XLOG_QUEUE_SLOTS_INFO_COLUMN_SIZE 4
static polar_ringbuf_slot_t *slots_info = NULL;
static uint64 rbuf_occupied;

/* Get logindex mem table size */
PG_FUNCTION_INFO_V1(polar_get_logindex_mem_tbl_size);
Datum
polar_get_logindex_mem_tbl_size(PG_FUNCTION_ARGS)
{
	log_idx_table_id_t mem_tbl_size = 0;

	if (polar_logindex_redo_instance)
		mem_tbl_size = polar_logindex_mem_tbl_size(polar_logindex_redo_instance->wal_logindex_snapshot);
	return (Datum) mem_tbl_size;
}

/* Get normal logindex snapshot used mem table size */
PG_FUNCTION_INFO_V1(polar_used_logindex_mem_tbl_size);
Datum
polar_used_logindex_mem_tbl_size(PG_FUNCTION_ARGS)
{
	log_idx_table_id_t mem_tbl_size = 0;

	if (polar_logindex_redo_instance)
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
	XLogRecPtr	bg_lsn = polar_get_bg_replayed_lsn(polar_logindex_redo_instance);

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

	int64		free_up_cnt = 0;
	int64		recv_phys_io_cnt = 0;
	int64		total_written = 0;
	int64		total_read = 0;
	int64		evict_ref_cnt = 0;

	if (polar_xlog_queue_buffers <= 0)
		PG_RETURN_NULL();

	tupdesc = CreateTemplateTupleDesc(XLOG_QUEUE_STAT_DETIAL_COL_SIZE);
	TupleDescInitEntry(tupdesc, (AttrNumber) 1, "xlog_queue_free_up_cnt", INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 2, "recv_phys_io_cnt", INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 3, "total_written", INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 4, "total_read", INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 5, "evict_ref_cnt", INT8OID, -1, 0);
	tupdesc = BlessTupleDesc(tupdesc);

	MemSet(nulls, 0, sizeof(nulls));

	if (polar_logindex_redo_instance != NULL)
	{
		polar_ringbuf_t queue = polar_logindex_redo_instance->xlog_queue;

		free_up_cnt = pg_atomic_read_u64(&queue->prs.free_up_cnt);
		recv_phys_io_cnt = pg_atomic_read_u64(&queue->prs.recv_phys_io_cnt);
		/* report as physical positions in [0, size) */
		total_written = pg_atomic_read_u64(&queue->pwrite) % queue->size;
		total_read = pg_atomic_read_u64(&queue->pread) % queue->size;
		evict_ref_cnt = pg_atomic_read_u64(&queue->prs.evict_ref_cnt);
	}

	values[0] = Int64GetDatum(free_up_cnt);
	values[1] = Int64GetDatum(recv_phys_io_cnt);
	values[2] = Int64GetDatum(total_written);
	values[3] = Int64GetDatum(total_read);
	values[4] = Int64GetDatum(evict_ref_cnt);

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

		LWLockAcquire(&queue->lock.lock, LW_SHARED);
		rbuf_occupied = queue->occupied;
		memcpy(slots_info, queue->slot, sizeof(polar_ringbuf_slot_t) * POLAR_RINGBUF_MAX_SLOT);
		LWLockRelease(&queue->lock.lock);

		/* report as physical positions in [0, size) */
		for (i = 0; i < POLAR_RINGBUF_MAX_SLOT; i++)
			slots_info[i].pread %= queue->size;

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
			values[2] = BoolGetDatum(slots_info[i].strong);
			values[3] = Int32GetDatum(slots_info[i].ref_num);
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

/* Get fullpage logindex snapshot used mem table size */
PG_FUNCTION_INFO_V1(polar_used_logindex_fullpage_snapshot_mem_tbl_size);

Datum
polar_used_logindex_fullpage_snapshot_mem_tbl_size(PG_FUNCTION_ARGS)
{
	log_idx_table_id_t mem_tbl_size = 0;

	if (polar_logindex_redo_instance && polar_logindex_redo_instance->fullpage_logindex_snapshot)
		mem_tbl_size = polar_logindex_used_mem_tbl_size(polar_logindex_redo_instance->fullpage_logindex_snapshot);

	return mem_tbl_size;
}

PG_FUNCTION_INFO_V1(polar_get_fullpage_stat);

Datum
polar_get_fullpage_stat(PG_FUNCTION_ARGS)
{
#define FULLPAGE_COLUMN_SIZE 9

	TupleDesc	tupdesc;
	Datum		values[FULLPAGE_COLUMN_SIZE];
	bool		nulls[FULLPAGE_COLUMN_SIZE];
	polar_fullpage_stat_t *stat;
	log_index_snapshot logindex_snapshot;
	uint64_t	read_fullpage_count;
	uint64_t	restore_oldpage_count;

	if (!POLAR_LOGINDEX_FULLPAGE_CTL_EXIST())
		PG_RETURN_NULL();

	tupdesc = CreateTemplateTupleDesc(FULLPAGE_COLUMN_SIZE);
	TupleDescInitEntry(tupdesc, (AttrNumber) 1, "write_fullpage_count", INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 2, "read_oldpage_count", INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 3, "insert_fullpage_lsn", LSNOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 4, "flush_fullpage_lsn", LSNOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 5, "read_fullpage_count", INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 6, "restore_oldpage_count", INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 7, "restore_oldpage_rate", FLOAT4OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 8, "fullpage_bgworker_replay_lsn", LSNOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 9, "load_fullpage_lsn", LSNOID, -1, 0);
	tupdesc = BlessTupleDesc(tupdesc);

	MemSet(nulls, 0, sizeof(nulls));

	stat = &polar_logindex_redo_instance->fullpage_ctl->stat;
	logindex_snapshot = polar_logindex_redo_instance->fullpage_logindex_snapshot;
	values[0] = UInt64GetDatum(pg_atomic_read_u64(&stat->write_fullpage_count));
	values[1] = UInt64GetDatum(pg_atomic_read_u64(&stat->read_oldpage_count));
	values[2] = LSNGetDatum(pg_atomic_read_u64(&stat->insert_fullpage_lsn));
	values[3] = LSNGetDatum(Max(pg_atomic_read_u64((pg_atomic_uint64 *) &logindex_snapshot->meta.max_lsn),
								pg_atomic_read_u64(&logindex_snapshot->flush_active_table_lsn)));
	read_fullpage_count = pg_atomic_read_u64(&stat->read_fullpage_count);
	restore_oldpage_count = pg_atomic_read_u64(&stat->restore_oldpage_count);
	values[4] = UInt64GetDatum(read_fullpage_count);
	values[5] = UInt64GetDatum(restore_oldpage_count);

	if (read_fullpage_count > 0)
		values[6] = Float4GetDatum(((float) restore_oldpage_count / (float) read_fullpage_count) * 100);
	else
		values[6] = Float4GetDatum(0);
	values[7] = LSNGetDatum(pg_atomic_read_u64(&stat->fullpage_bgworker_replay_lsn));
	values[8] = LSNGetDatum(pg_atomic_read_u64(&stat->load_fullpage_lsn));

	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}
