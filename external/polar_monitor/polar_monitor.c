/*-------------------------------------------------------------------------
 *
 * polar_monitor.c
 *	  display some information of PolarDB.
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
 *	  external/polar_monitor/polar_monitor.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/xlog.h"
#include "fmgr.h"
#include "access/polar_logindex.h"
#include "access/polar_logindex_redo.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "postmaster/polar_wal_pipeliner.h"
#include "storage/polar_fd.h"
#include "utils/builtins.h"
#include "utils/pg_lsn.h"

/* POLAR */
#include "pgstat.h"
#include "replication/slot.h"
#include "replication/polar_cluster_info.h"
/* POLAR end */

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(polar_consistent_lsn);

Datum
polar_consistent_lsn(PG_FUNCTION_ARGS)
{
	PG_RETURN_LSN(polar_get_consistent_lsn());
}

PG_FUNCTION_INFO_V1(polar_oldest_apply_lsn);

Datum
polar_oldest_apply_lsn(PG_FUNCTION_ARGS)
{
	if (RecoveryInProgress())
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("recovery is in progress"),
				 errhint("WAL control functions cannot be executed during recovery.")));

	PG_RETURN_LSN(polar_get_oldest_apply_lsn());
}


PG_FUNCTION_INFO_V1(polar_oldest_lock_lsn);
Datum
polar_oldest_lock_lsn(PG_FUNCTION_ARGS)
{
	if (RecoveryInProgress())
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("recovery is in progress"),
				 errhint("WAL control functions cannot be executed during recovery.")));

	PG_RETURN_LSN(polar_get_oldest_lock_lsn());
}

PG_FUNCTION_INFO_V1(polar_node_type);

Datum
polar_node_type(PG_FUNCTION_ARGS)
{
	char	   *mode;
	PolarNodeType node_type = polar_get_node_type();

	switch (node_type)
	{
		case POLAR_PRIMARY:
			mode = "primary";
			break;
		case POLAR_REPLICA:
			mode = "replica";
			break;
		case POLAR_STANDBY:
			mode = "standby";
			break;
		default:
			mode = "unknown";
	}

	PG_RETURN_TEXT_P(cstring_to_text(mode));
}

PG_FUNCTION_INFO_V1(polar_set_available);

Datum
polar_set_available(PG_FUNCTION_ARGS)
{
	bool		available = PG_GETARG_BOOL(0);

	polar_set_available_state(available);
	PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(polar_is_available);

Datum
polar_is_available(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(polar_get_available_state());
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

/* bulk extend stats */
/* per table (or index) */
PG_FUNCTION_INFO_V1(polar_pg_stat_get_bulk_extend_times);
Datum
polar_pg_stat_get_bulk_extend_times(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	int64		result;
	PgStat_StatTabEntry *tabentry;

	if ((tabentry = pgstat_fetch_stat_tabentry(relid)) == NULL)
		result = 0;
	else
		result = (int64) (tabentry->polar_bulk_extend_times);

	PG_RETURN_INT64(result);
}

PG_FUNCTION_INFO_V1(polar_pg_stat_get_bulk_extend_blocks);
Datum
polar_pg_stat_get_bulk_extend_blocks(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	int64		result;
	PgStat_StatTabEntry *tabentry;

	if ((tabentry = pgstat_fetch_stat_tabentry(relid)) == NULL)
		result = 0;
	else
		result = (int64) (tabentry->polar_bulk_extend_blocks);

	PG_RETURN_INT64(result);
}

/* POLAR: Bulk Read Stats */
/* Per table (or index) */
PG_FUNCTION_INFO_V1(polar_pg_stat_get_bulk_read_calls);
Datum
polar_pg_stat_get_bulk_read_calls(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	int64		result;
	PgStat_StatTabEntry *tabentry;

	if ((tabentry = pgstat_fetch_stat_tabentry(relid)) == NULL)
		result = 0;
	else
		result = (int64) (tabentry->polar_bulk_read_calls);

	PG_RETURN_INT64(result);
}

PG_FUNCTION_INFO_V1(polar_pg_stat_get_bulk_read_calls_IO);
Datum
polar_pg_stat_get_bulk_read_calls_IO(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	int64		result;
	PgStat_StatTabEntry *tabentry;

	if ((tabentry = pgstat_fetch_stat_tabentry(relid)) == NULL)
		result = 0;
	else
		result = (int64) (tabentry->polar_bulk_read_calls_IO);

	PG_RETURN_INT64(result);
}

PG_FUNCTION_INFO_V1(polar_pg_stat_get_bulk_read_blocks_IO);
Datum
polar_pg_stat_get_bulk_read_blocks_IO(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	int64		result;
	PgStat_StatTabEntry *tabentry;

	if ((tabentry = pgstat_fetch_stat_tabentry(relid)) == NULL)
		result = 0;
	else
		result = (int64) (tabentry->polar_bulk_read_blocks_IO);

	PG_RETURN_INT64(result);
}

PG_FUNCTION_INFO_V1(polar_get_slot_node_type);
Datum
polar_get_slot_node_type(PG_FUNCTION_ARGS)
{
	int			i;
	char	   *slot_name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	PolarNodeType slot_node_type = POLAR_UNKNOWN;

	if (max_replication_slots <= 0)
		PG_RETURN_TEXT_P(cstring_to_text(POLAR_UNKNOWN_STRING));

	LWLockAcquire(ReplicationSlotControlLock, LW_SHARED);

	for (i = 0; i < max_replication_slots; i++)
	{
		ReplicationSlot *s;

		s = &ReplicationSlotCtl->replication_slots[i];

		if (!s->in_use)
			continue;

		if (strcmp(NameStr(s->data.name), slot_name) == 0)
		{
			slot_node_type = s->polar_slot_node_type;
			break;
		}
	}

	LWLockRelease(ReplicationSlotControlLock);

	pfree(slot_name);

	switch (slot_node_type)
	{
		case POLAR_PRIMARY:
			PG_RETURN_TEXT_P(cstring_to_text(POLAR_PRIMARY_STRING));

		case POLAR_REPLICA:
			PG_RETURN_TEXT_P(cstring_to_text(POLAR_REPLICA_STRING));

		case POLAR_STANDBY:
			PG_RETURN_TEXT_P(cstring_to_text(POLAR_STANDBY_STRING));

		case POLAR_STANDALONE_DATAMAX:
			PG_RETURN_TEXT_P(cstring_to_text(POLAR_STANDALONE_DATAMAX_STRING));

		case POLAR_UNKNOWN:
		default:
			PG_RETURN_TEXT_P(cstring_to_text(POLAR_UNKNOWN_STRING));
	}
}

extern polar_wal_pipeline_stats_t *polar_wal_pipeline_get_stats();
extern polar_wait_object_t * polar_wal_pipeline_get_worker_wait_obj(int thread_no);
extern void polar_wal_pipeline_stats_reset(void);

PG_FUNCTION_INFO_V1(polar_wal_pipeline_info);
Datum
polar_wal_pipeline_info(PG_FUNCTION_ARGS)
{
	TupleDesc	tupdesc;
	Datum		values[10];
	bool		nulls[10];
	HeapTuple	tuple;
	Datum		result;
	int			i = 0;
	int			j;

	/* Build a tuple descriptor for our result type */
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	memset(nulls, 0, sizeof(nulls));

	values[i++] = UInt64GetDatum(polar_wal_pipeline_get_current_insert_lsn());
	values[i++] = UInt64GetDatum(polar_wal_pipeline_get_continuous_insert_lsn());
	values[i++] = UInt64GetDatum(polar_wal_pipeline_get_write_lsn());
	values[i++] = UInt64GetDatum(polar_wal_pipeline_get_flush_lsn());
	values[i++] = UInt64GetDatum(polar_wal_pipeline_get_unflushed_xlog_add_slot_no());
	values[i++] = UInt64GetDatum(polar_wal_pipeline_get_unflushed_xlog_del_slot_no());
	for (j = 0; j < POLAR_WAL_PIPELINE_NOTIFY_WORKER_NUM_MAX; j++)
		values[i++] = UInt64GetDatum(polar_wal_pipeline_get_last_notify_lsn(j));

	tuple = heap_form_tuple(BlessTupleDesc(tupdesc), values, nulls);
	result = HeapTupleGetDatum(tuple);

	PG_RETURN_DATUM(result);
}

PG_FUNCTION_INFO_V1(polar_wal_pipeline_stats);
Datum
polar_wal_pipeline_stats(PG_FUNCTION_ARGS)
{
	TupleDesc	tupdesc;
	Datum		values[31];
	bool		nulls[31];
	HeapTuple	tuple;
	Datum		result;
	int			i = 0;
	int			j;
	polar_wal_pipeline_stats_t *stats = polar_wal_pipeline_get_stats();
	uint64		user_stats[6];

	/* Build a tuple descriptor for our result type */
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	memset(nulls, 0, sizeof(nulls));

	for (j = 0; j < POLAR_WAL_PIPELINE_MAX_THREAD_NUM; j++)
	{
		if (POLAR_WAL_PIPELINER_ENABLE())
		{
			polar_wait_object_t *wait_obj = polar_wal_pipeline_get_worker_wait_obj(j);

			values[i++] = UInt64GetDatum(pg_atomic_read_u64(&wait_obj->stats.timeout_waits));
			values[i++] = UInt64GetDatum(pg_atomic_read_u64(&wait_obj->stats.wakeup_waits));
		}
		else
		{
			values[i++] = UInt64GetDatum(0);
			values[i++] = UInt64GetDatum(0);
		}
	}

	MemSet(user_stats, 0, sizeof(user_stats));
	for (int k = 0; k < POLAR_WAL_PIPELINE_STAT_PARTITIONS; k++)
	{
		user_stats[0] += pg_atomic_read_u64(&stats->user_stats[k].total_user_group_commits);
		user_stats[1] += pg_atomic_read_u64(&stats->user_stats[k].total_user_spin_commits);
		user_stats[2] += pg_atomic_read_u64(&stats->user_stats[k].total_user_timeout_commits);
		user_stats[3] += pg_atomic_read_u64(&stats->user_stats[k].total_user_wakeup_commits);
		user_stats[4] += pg_atomic_read_u64(&stats->user_stats[k].total_user_miss_timeouts);
		user_stats[5] += pg_atomic_read_u64(&stats->user_stats[k].total_user_miss_wakeups);
	}
	values[i++] = UInt64GetDatum(user_stats[0]);
	values[i++] = UInt64GetDatum(user_stats[1]);
	values[i++] = UInt64GetDatum(user_stats[2]);
	values[i++] = UInt64GetDatum(user_stats[3]);
	values[i++] = UInt64GetDatum(user_stats[4]);
	values[i++] = UInt64GetDatum(user_stats[5]);
	values[i++] = UInt64GetDatum(pg_atomic_read_u64(&stats->total_advance_callups));
	values[i++] = UInt64GetDatum(pg_atomic_read_u64(&stats->total_advances));
	values[i++] = UInt64GetDatum(pg_atomic_read_u64(&stats->total_write_callups));
	values[i++] = UInt64GetDatum(pg_atomic_read_u64(&stats->total_writes));
	values[i++] = UInt64GetDatum(pg_atomic_read_u64(&stats->unflushed_xlog_slot_waits));
	values[i++] = UInt64GetDatum(pg_atomic_read_u64(&stats->total_flush_callups));
	values[i++] = UInt64GetDatum(pg_atomic_read_u64(&stats->total_flushes));
	values[i++] = UInt64GetDatum(pg_atomic_read_u64(&stats->total_flush_merges));
	values[i++] = UInt64GetDatum(pg_atomic_read_u64(&stats->total_notify_callups));
	values[i++] = UInt64GetDatum(pg_atomic_read_u64(&stats->total_notifies));
	values[i++] = UInt64GetDatum(pg_atomic_read_u64(&stats->total_notified_users));

	tuple = heap_form_tuple(BlessTupleDesc(tupdesc), values, nulls);
	result = HeapTupleGetDatum(tuple);

	PG_RETURN_DATUM(result);
}

PG_FUNCTION_INFO_V1(polar_wal_pipeline_reset_stats);

Datum
polar_wal_pipeline_reset_stats(PG_FUNCTION_ARGS)
{
	polar_wal_pipeline_stats_reset();

	PG_RETURN_DATUM(true);
}

PG_FUNCTION_INFO_V1(polar_cluster_info);
Datum
polar_cluster_info(PG_FUNCTION_ARGS)
{
#define CLUSTER_INFO_COUNT 18
	int			i,
				count,
				cols = 1;
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	TupleDesc	tupdesc;
	Tuplestorestate *tupstore;
	MemoryContext per_query_ctx;
	MemoryContext oldcontext;
	polar_cluster_info_item *items = polar_cluster_info_ctl->items;
	polar_cluster_info_item *item;

	/* Build a tuple descriptor for our result type */
	tupdesc = CreateTemplateTupleDesc(CLUSTER_INFO_COUNT);
	TupleDescInitEntry(tupdesc, (AttrNumber) cols++, "name",
					   TEXTOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) cols++, "host",
					   TEXTOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) cols++, "port",
					   INT4OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) cols++, "release_date",
					   TEXTOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) cols++, "version",
					   TEXTOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) cols++, "slot_name",
					   TEXTOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) cols++, "type",
					   TEXTOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) cols++, "state",
					   TEXTOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) cols++, "cpu",
					   INT4OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) cols++, "cpu_quota",
					   INT4OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) cols++, "memory",
					   INT4OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) cols++, "memory_quota",
					   INT4OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) cols++, "iops",
					   INT4OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) cols++, "iops_quota",
					   INT4OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) cols++, "connection",
					   INT4OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) cols++, "connection_quota",
					   INT4OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) cols++, "px_connection",
					   INT4OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) cols++, "px_connection_quota",
					   INT4OID, -1, 0);

	per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
	oldcontext = MemoryContextSwitchTo(per_query_ctx);
	tupstore = tuplestore_begin_heap(true, false, work_mem);
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = tupstore;
	rsinfo->setDesc = tupdesc;
	MemoryContextSwitchTo(oldcontext);

	LWLockAcquire(&polar_cluster_info_ctl->lock, LW_EXCLUSIVE);
	count = polar_cluster_info_ctl->count;
	for (i = 0; i < count; ++i)
	{
		Datum		values[CLUSTER_INFO_COUNT];
		bool		nulls[CLUSTER_INFO_COUNT];

		MemSet(nulls, 0, sizeof(bool) * CLUSTER_INFO_COUNT);

		item = &items[i];
		values[0] = CStringGetTextDatum(item->name);
		values[1] = CStringGetTextDatum(item->host);
		values[2] = Int32GetDatum(item->port);
		values[3] = CStringGetTextDatum(item->release_date);
		values[4] = CStringGetTextDatum(item->version);
		values[5] = CStringGetTextDatum(item->slot_name);
		values[6] = CStringGetTextDatum(polar_node_type_string(item->type, DEBUG5));
		/* TODO: for master node print out NULL instead of `Disabled` state */
		values[7] = CStringGetTextDatum(polar_standby_state_string(item->state, DEBUG5));
		values[8] = Int32GetDatum(item->cpu);
		values[9] = Int32GetDatum(item->cpu_quota);
		values[10] = Int32GetDatum(item->memory);
		values[11] = Int32GetDatum(item->memory_quota);
		values[12] = Int32GetDatum(item->iops);
		values[13] = Int32GetDatum(item->iops_quota);
		values[14] = Int32GetDatum(item->connection);
		values[15] = Int32GetDatum(item->connection_quota);
		values[16] = Int32GetDatum(item->px_connection);
		values[17] = Int32GetDatum(item->px_connection_quota);
		tuplestore_putvalues(tupstore, tupdesc, values, nulls);
	}
	LWLockRelease(&polar_cluster_info_ctl->lock);

	/* clean up and return the tuplestore */
	tuplestore_donestoring(tupstore);

	return (Datum) 0;
}
