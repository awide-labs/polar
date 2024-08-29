/*-------------------------------------------------------------------------
 *
 * polar_monitor_lock.c
 *	  show lwlock and regular lock monitor
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
 *	  external/polar_monitor_preload/polar_monitor_lock.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "catalog/pg_type.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "polar_monitor_lock.h"
#include "storage/lwlock.h"
#include "storage/ipc.h"
#include "storage/polar_lock_stats.h"
#include "storage/sinvaladt.h"
#include "tcop/utility.h"
#include "utils/builtins.h"
#include "utils/backend_status.h"
#include "utils/wait_event.h"

/* POLAR: max procs for lock stat */
#define POLAR_MAX_LOCK_STAT_SLOTS (MaxBackends + NUM_AUXPROCTYPES)

#define CHECK_BACKENDID_VALID(backendid) (backendid >= 1 && backendid <= POLAR_MAX_LOCK_STAT_SLOTS)

#define LW_LOCK_MASK				((uint32) ((1 << 25)-1))

#define LW_VAL_EXCLUSIVE			((uint32) 1 << 24)

static void polar_lwlock_stat_accum(polar_all_lwlocks_stat *stat);
static void polar_lock_stat_accum(polar_all_locks_stat *stat);

Size
lwlock_stat_shmem_size(void)
{
	/* all backends + aux proc */
	return sizeof(polar_all_lwlocks_stat) * POLAR_MAX_LOCK_STAT_SLOTS;
}

Size
lock_stat_shmem_size(void)
{
	/* all backends + aux proc */
	return sizeof(polar_all_locks_stat) * POLAR_MAX_LOCK_STAT_SLOTS;
}

/*
 * shmem_startup hook: allocate or attach to shared memory,
 */
void
polar_lock_stat_shmem_startup(void)
{
	bool		found = false;
	Size		total_size = 0;

	/*
	 * Create or attach to the shared memory state
	 */
	LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);

	total_size = lwlock_stat_shmem_size();
	polar_lwlocks_stat_array = ShmemInitStruct("polar_lwlocks_stat_array",
											   total_size, &found);
	elog(LOG, "polar_lwlocks_stat_array share memory total size is %d", (int) total_size);
	if (!found)
	{
		/* First time through ... */
		memset(polar_lwlocks_stat_array, 0, total_size);
	}

	total_size = lock_stat_shmem_size();
	polar_locks_stat_array = ShmemInitStruct("polar_locks_stat_array",
											 total_size, &found);

	elog(LOG, "polar_locks_stat_array share memory total size is %d", (int) total_size);
	if (!found)
	{
		/* First time through ... */
		memset(polar_locks_stat_array, 0, total_size);
	}

	LWLockRelease(AddinShmemInitLock);
}

/*
 * POLAR: return the lock stat
 */
PG_FUNCTION_INFO_V1(polar_stat_lock);
Datum
polar_stat_lock(PG_FUNCTION_ARGS)
{
#define POLAR_LOCK_STAT_COLS 7
	int			i = 1;
	int			j;
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	TupleDesc	tupdesc;
	Tuplestorestate *tupstore;
	MemoryContext per_query_ctx;
	MemoryContext oldcontext;
	polar_all_locks_stat global_stat;

	MemSet(&global_stat, 0, sizeof(polar_all_locks_stat));

	/* check to see if caller supports us returning a tuplestore */
	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that cannot accept a set")));
	if (!(rsinfo->allowedModes & SFRM_Materialize))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("materialize mode required, but it is not " \
						"allowed in this context")));

	tupdesc = CreateTemplateTupleDesc(POLAR_LOCK_STAT_COLS);
	TupleDescInitEntry(tupdesc, (AttrNumber) i++, "id",
					   INT4OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) i++, "type",
					   TEXTOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) i++, "mode",
					   TEXTOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) i++, "lock_count",
					   INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) i++, "block_count",
					   INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) i++, "fastpath_count",
					   INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) i++, "wait_time",
					   INT8OID, -1, 0);
	per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
	oldcontext = MemoryContextSwitchTo(per_query_ctx);

	tupstore = tuplestore_begin_heap(true, false, work_mem);
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = tupstore;
	rsinfo->setDesc = tupdesc;

	MemoryContextSwitchTo(oldcontext);

	if (!polar_locks_stat_array)
	{
		tuplestore_donestoring(tupstore);
		return (Datum) 0;
	}

	/* accum polar_locks_stat_array result into global_stat */
	polar_lock_stat_accum(&global_stat);

	for (i = 0; i <= LOCKTAG_LAST_TYPE; i++)
	{
		for (j = AccessShareLock; j <= AccessExclusiveLock; j++)
		{
			int			col = 0;
			polar_regular_lock_stat *stat = &global_stat.detail[i][j];
			Datum		values[POLAR_LOCK_STAT_COLS];
			bool		nulls[POLAR_LOCK_STAT_COLS];

			MemSet(values, 0, sizeof(values));
			MemSet(nulls, 0, sizeof(nulls));

			/* id for order */
			values[col++] = Int32GetDatum(i);

			/* name */
			if (i <= LOCKTAG_LAST_TYPE)
				values[col++] = CStringGetTextDatum(LockTagTypeNames[i]);
			else
				values[col++] = CStringGetTextDatum("others");

			/* mode */
			if (j <= AccessExclusiveLock)
				values[col++] = CStringGetTextDatum(GetLockmodeName(DEFAULT_LOCKMETHOD, j));
			else
				values[col++] = CStringGetTextDatum("others");

			values[col++] = Int64GetDatumFast(stat->lock_count);
			values[col++] = Int64GetDatumFast(stat->block_count);
			values[col++] = Int64GetDatumFast(stat->fastpath_count);
			values[col++] = Int64GetDatumFast(stat->wait_time);

			tuplestore_putvalues(tupstore, tupdesc, values, nulls);
		}
	}
	/* clean up and return the tuplestore */
	tuplestore_donestoring(tupstore);

	return (Datum) 0;
}

/*
 * POLAR: return the lock stat for every process.
 *        we do not reuse polar_stat_lwlock codes now.
 */
PG_FUNCTION_INFO_V1(polar_proc_stat_lock);
Datum
polar_proc_stat_lock(PG_FUNCTION_ARGS)
{
#define POLAR_PROC_LOCK_STAT_COLS 8
	int			num_backends = pgstat_fetch_stat_numbackends();
	int			curr;
	int			i = 1;
	int			j;
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	TupleDesc	tupdesc;
	Tuplestorestate *tupstore;
	MemoryContext per_query_ctx;
	MemoryContext oldcontext;

	/* check to see if caller supports us returning a tuplestore */
	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that cannot accept a set")));
	if (!(rsinfo->allowedModes & SFRM_Materialize))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("materialize mode required, but it is not " \
						"allowed in this context")));

	tupdesc = CreateTemplateTupleDesc(POLAR_PROC_LOCK_STAT_COLS);
	TupleDescInitEntry(tupdesc, (AttrNumber) i++, "pid",
					   INT4OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) i++, "id",
					   INT4OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) i++, "type",
					   TEXTOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) i++, "mode",
					   TEXTOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) i++, "lock_count",
					   INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) i++, "block_count",
					   INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) i++, "fastpath_count",
					   INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) i++, "wait_time",
					   INT8OID, -1, 0);
	per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
	oldcontext = MemoryContextSwitchTo(per_query_ctx);

	tupstore = tuplestore_begin_heap(true, false, work_mem);
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = tupstore;
	rsinfo->setDesc = tupdesc;

	MemoryContextSwitchTo(oldcontext);

	if (!polar_locks_stat_array)
	{
		tuplestore_donestoring(tupstore);
		return (Datum) 0;
	}

	for (curr = 1; curr <= num_backends; curr++)
	{
		PgBackendStatus *beentry;
		polar_all_locks_stat *procstat;
		LocalPgBackendStatus *local_beentry = pgstat_fetch_stat_local_beentry(curr);

		if (!local_beentry)
			continue;

		beentry = &local_beentry->backendStatus;
		if (!CHECK_BACKENDID_VALID(beentry->backendid))
			continue;

		procstat = &polar_locks_stat_array[beentry->backendid - 1];

		for (i = 0; i <= LOCKTAG_LAST_TYPE; i++)
		{
			for (j = AccessShareLock; j <= AccessExclusiveLock; j++)
			{
				int			col = 0;
				polar_regular_lock_stat *stat = &procstat->detail[i][j];
				Datum		values[POLAR_PROC_LOCK_STAT_COLS];
				bool		nulls[POLAR_PROC_LOCK_STAT_COLS];

				MemSet(values, 0, sizeof(values));
				MemSet(nulls, 0, sizeof(nulls));

				/* pid */
				values[col++] = Int32GetDatum(beentry->st_procpid);

				/* id for order */
				values[col++] = Int32GetDatum(i);

				/* name */
				if (i <= LOCKTAG_LAST_TYPE)
					values[col++] = CStringGetTextDatum(LockTagTypeNames[i]);
				else
					values[col++] = CStringGetTextDatum("others");

				/* mode */
				if (j <= AccessExclusiveLock)
					values[col++] = CStringGetTextDatum(GetLockmodeName(DEFAULT_LOCKMETHOD, j));
				else
					values[col++] = CStringGetTextDatum("others");

				values[col++] = Int64GetDatumFast(stat->lock_count);
				values[col++] = Int64GetDatumFast(stat->block_count);
				values[col++] = Int64GetDatumFast(stat->fastpath_count);
				values[col++] = Int64GetDatumFast(stat->wait_time);

				tuplestore_putvalues(tupstore, tupdesc, values, nulls);
			}
		}
	}
	/* clean up and return the tuplestore */
	tuplestore_donestoring(tupstore);

	return (Datum) 0;
}

/*
 * POLAR: return the lwlock stat
 */
PG_FUNCTION_INFO_V1(polar_stat_lwlock);
Datum
polar_stat_lwlock(PG_FUNCTION_ARGS)
{
#define POLAR_LWLOCK_STAT_COLS 6
	int			index = 1;
	int			lwlocks;
	int			total;
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	TupleDesc	tupdesc;
	Tuplestorestate *tupstore;
	MemoryContext per_query_ctx;
	MemoryContext oldcontext;
	polar_all_lwlocks_stat stat;

	memset(&stat, 0, sizeof(polar_all_lwlocks_stat));

	/* check to see if caller supports us returning a tuplestore */
	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that cannot accept a set")));
	if (!(rsinfo->allowedModes & SFRM_Materialize))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("materialize mode required, but it is not " \
						"allowed in this context")));

	tupdesc = CreateTemplateTupleDesc(POLAR_LWLOCK_STAT_COLS);
	TupleDescInitEntry(tupdesc, (AttrNumber) index++, "tranche",
					   INT2OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) index++, "name",
					   TEXTOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) index++, "sh_acquire_count",
					   INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) index++, "ex_acquire_count",
					   INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) index++, "block_count",
					   INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) index++, "wait_time",
					   INT8OID, -1, 0);
	per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
	oldcontext = MemoryContextSwitchTo(per_query_ctx);

	tupstore = tuplestore_begin_heap(true, false, work_mem);
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = tupstore;
	rsinfo->setDesc = tupdesc;

	MemoryContextSwitchTo(oldcontext);

	if (!polar_lwlocks_stat_array)
	{
		tuplestore_donestoring(tupstore);
		return (Datum) 0;
	}

	polar_lwlock_stat_accum(&stat);

	lwlocks = polar_get_lwlock_counter();
	total = (lwlocks > POLAR_MAX_LWLOCK_TRANCHE ? POLAR_MAX_LWLOCK_TRANCHE : lwlocks);

	for (index = 1; index < total; index++)
	{
		/* for each row */
		int			col = 0;
		Datum		values[POLAR_LWLOCK_STAT_COLS];
		bool		nulls[POLAR_LWLOCK_STAT_COLS];

		MemSet(values, 0, sizeof(values));
		MemSet(nulls, 0, sizeof(nulls));

		/* tranche */
		values[col++] = Int16GetDatum(index);
		/* name */
		values[col++] = CStringGetTextDatum(GetLWLockIdentifier(PG_WAIT_LWLOCK, index));
		values[col++] = Int64GetDatumFast(stat.lwlocks[index].sh_acquire_count);
		values[col++] = Int64GetDatumFast(stat.lwlocks[index].ex_acquire_count);
		values[col++] = Int64GetDatumFast(stat.lwlocks[index].block_count);
		values[col++] = Int64GetDatumFast(stat.lwlocks[index].wait_time);
		tuplestore_putvalues(tupstore, tupdesc, values, nulls);
	}
	/* clean up and return the tuplestore */
	tuplestore_donestoring(tupstore);

	return (Datum) 0;
}

/*
 * POLAR: return the lwlock stat for every process
 *        we do not reuse polar_stat_lwlock codes now.
 */
PG_FUNCTION_INFO_V1(polar_proc_stat_lwlock);
Datum
polar_proc_stat_lwlock(PG_FUNCTION_ARGS)
{
#define POLAR_PROC_LWLOCK_STAT_COLS 7
	int			num_backends = pgstat_fetch_stat_numbackends();
	int			curr;
	int			index = 1;
	int			lwlocks;
	int			total;
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	TupleDesc	tupdesc;
	Tuplestorestate *tupstore;
	MemoryContext per_query_ctx;
	MemoryContext oldcontext;

	/* check to see if caller supports us returning a tuplestore */
	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that cannot accept a set")));
	if (!(rsinfo->allowedModes & SFRM_Materialize))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("materialize mode required, but it is not " \
						"allowed in this context")));

	tupdesc = CreateTemplateTupleDesc(POLAR_PROC_LWLOCK_STAT_COLS);
	TupleDescInitEntry(tupdesc, (AttrNumber) index++, "pid",
					   INT4OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) index++, "tranche",
					   INT2OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) index++, "name",
					   TEXTOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) index++, "sh_acquire_count",
					   INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) index++, "ex_acquire_count",
					   INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) index++, "block_count",
					   INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) index++, "wait_time",
					   INT8OID, -1, 0);
	per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
	oldcontext = MemoryContextSwitchTo(per_query_ctx);

	tupstore = tuplestore_begin_heap(true, false, work_mem);
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = tupstore;
	rsinfo->setDesc = tupdesc;

	MemoryContextSwitchTo(oldcontext);

	if (!polar_lwlocks_stat_array)
	{
		tuplestore_donestoring(tupstore);
		return (Datum) 0;
	}

	lwlocks = polar_get_lwlock_counter();
	total = (lwlocks > POLAR_MAX_LWLOCK_TRANCHE ? POLAR_MAX_LWLOCK_TRANCHE : lwlocks);

	for (curr = 1; curr <= num_backends; curr++)
	{
		PgBackendStatus *beentry;
		polar_all_lwlocks_stat *procstat;
		LocalPgBackendStatus *local_beentry = pgstat_fetch_stat_local_beentry(curr);

		if (!local_beentry)
			continue;

		beentry = &local_beentry->backendStatus;
		if (!CHECK_BACKENDID_VALID(beentry->backendid))
			continue;

		procstat = &polar_lwlocks_stat_array[beentry->backendid - 1];

		for (index = 0; index < total; index++)
		{
			/* for each row */
			int			col = 0;
			Datum		values[POLAR_PROC_LWLOCK_STAT_COLS];
			bool		nulls[POLAR_PROC_LWLOCK_STAT_COLS];

			MemSet(values, 0, sizeof(values));
			MemSet(nulls, 0, sizeof(nulls));

			/* pid */
			values[col++] = Int32GetDatum(beentry->st_procpid);
			/* tranche */
			values[col++] = Int16GetDatum(index);
			/* name */
			values[col++] = CStringGetTextDatum(GetLWLockIdentifier(PG_WAIT_LWLOCK, index));
			values[col++] = Int64GetDatumFast(procstat->lwlocks[index].sh_acquire_count);
			values[col++] = Int64GetDatumFast(procstat->lwlocks[index].ex_acquire_count);
			values[col++] = Int64GetDatumFast(procstat->lwlocks[index].block_count);
			values[col++] = Int64GetDatumFast(procstat->lwlocks[index].wait_time);
			tuplestore_putvalues(tupstore, tupdesc, values, nulls);
		}
	}
	/* clean up and return the tuplestore */
	tuplestore_donestoring(tupstore);

	return (Datum) 0;
}

/*
 * POLAR: return the lwlock stat for every process
 *        we do not reuse polar_stat_lwlock codes now.
 */
PG_FUNCTION_INFO_V1(polar_lwlock_stat_waiters);
Datum
polar_lwlock_stat_waiters(PG_FUNCTION_ARGS)
{

#define POLAR_LWLOCK_WAIT_NUMS_COLS 4
	int			index = 1;
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	TupleDesc	tupdesc;
	Tuplestorestate *tupstore;
	MemoryContext per_query_ctx;
	MemoryContext oldcontext;

	/* check to see if caller supports us returning a tuplestore */
	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that cannot accept a set")));
	if (!(rsinfo->allowedModes & SFRM_Materialize))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("materialize mode required, but it is not " \
						"allowed in this context")));

	tupdesc = CreateTemplateTupleDesc(POLAR_LWLOCK_WAIT_NUMS_COLS);
	TupleDescInitEntry(tupdesc, (AttrNumber) index++, "tranche",
					   INT2OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) index++, "lock_waiters",
					   INT4OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) index++, "sh_nums",
					   BOOLOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) index++, "ex_nums",
					   BOOLOID, -1, 0);
	per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
	oldcontext = MemoryContextSwitchTo(per_query_ctx);

	tupstore = tuplestore_begin_heap(true, false, work_mem);
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = tupstore;
	rsinfo->setDesc = tupdesc;

	MemoryContextSwitchTo(oldcontext);

	if (!MainLWLockArray)
	{
		tuplestore_donestoring(tupstore);
		return (Datum) 0;
	}

	for (index = 0; index < NUM_FIXED_LWLOCKS; index++)
	{
		/* for each row */
		int			col = 0;
		uint32		old_state;
		Datum		values[POLAR_LWLOCK_WAIT_NUMS_COLS];
		bool		nulls[POLAR_LWLOCK_WAIT_NUMS_COLS];

		MemSet(values, 0, sizeof(values));
		MemSet(nulls, 0, sizeof(nulls));

		/* tranche */
		values[col++] = Int16GetDatum(MainLWLockArray[index].lock.tranche);
		/* lock_waiters */
		values[col++] = Int32GetDatum(pg_atomic_read_u32(&MainLWLockArray[index].lock.nwaiters));
		/* sh_nums */
		old_state = pg_atomic_read_u32(&MainLWLockArray[index].lock.state);
		if (old_state & LW_LOCK_MASK)
			values[col++] = Int16GetDatum(1);
		else
			values[col++] = Int16GetDatum(0);
		/* ex_nums */
		if (old_state & LW_VAL_EXCLUSIVE)
			values[col++] = BoolGetDatum(1);
		else
			values[col++] = BoolGetDatum(0);

		tuplestore_putvalues(tupstore, tupdesc, values, nulls);
	}
	/* clean up and return the tuplestore */
	tuplestore_donestoring(tupstore);

	return (Datum) 0;
}

/*
 *  accumulate lwlock stat for all backends
 */
static void
polar_lwlock_stat_accum(polar_all_lwlocks_stat *stat)
{
	int			i = 0;
	int			j = 0;

	for (i = 0; i < POLAR_MAX_LOCK_STAT_SLOTS; i++)
	{
		polar_lwlock_stat *tmp = polar_lwlocks_stat_array[i].lwlocks;

		for (j = 0; j < POLAR_MAX_LWLOCK_TRANCHE; j++)
		{
			stat->lwlocks[j].ex_acquire_count += tmp[j].ex_acquire_count;
			stat->lwlocks[j].sh_acquire_count += tmp[j].sh_acquire_count;
			stat->lwlocks[j].block_count += tmp[j].block_count;
			stat->lwlocks[j].wait_time += tmp[j].wait_time;
		}
	}
}

/*
 *  accumulate regular lock stat for all backends
 */
static void
polar_lock_stat_accum(polar_all_locks_stat *stat)
{
	int			i = 0;
	int			j = 0;
	int			k = 0;

	for (i = 0; i < POLAR_MAX_LOCK_STAT_SLOTS; i++)
	{
		for (j = 0; j <= LOCKTAG_LAST_TYPE; j++)
		{
			for (k = 0; k < MAX_LOCKMODES; k++)
			{
				polar_regular_lock_stat *tmp = &polar_locks_stat_array[i].detail[j][k];

				stat->detail[j][k].lock_count += tmp->lock_count;
				stat->detail[j][k].block_count += tmp->block_count;
				stat->detail[j][k].fastpath_count += tmp->fastpath_count;
				stat->detail[j][k].wait_time += tmp->wait_time;
			}
		}
	}
}
