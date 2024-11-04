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
#include "storage/polar_fd.h"
#include "utils/builtins.h"
#include "utils/pg_lsn.h"

/* POLAR */
#include "pgstat.h"
#include "replication/slot.h"
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
