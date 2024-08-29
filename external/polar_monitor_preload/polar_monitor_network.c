/*-------------------------------------------------------------------------
 *
 * polar_monitor_network.c
 *	  show network monitor
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
 *	  external/polar_monitor_preload/polar_monitor_network.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "catalog/pg_type.h"
#include "funcapi.h"
#include "libpq/polar_network_stats.h"
#include "miscadmin.h"
#include "polar_monitor_network.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "tcop/utility.h"
#include "utils/backend_status.h"
#include "utils/wait_event.h"

/* POLAR: max procs for lock stat */
#define POLAR_MAX_NETWORK_STAT_SLOTS (MaxBackends + NUM_AUXPROCTYPES)

#define CHECK_BACKENDID_VALID(backendid) (backendid >= 1 && backendid <= POLAR_MAX_NETWORK_STAT_SLOTS)

#define IS_MYBACKEND_ID_VALID() \
	(MyBackendId != InvalidBackendId && MyBackendId >= 1 && MyBackendId <= MaxBackends)

#define IS_MYAUX_PROC_TYPE_VALID() (MyAuxProcType != NotAnAuxProcess)

#define POLAR_NET_STAT_BACKEND_INDEX() \
	(IS_MYBACKEND_ID_VALID() ? MyBackendId - 1 : \
		(IS_MYAUX_PROC_TYPE_VALID() ? MaxBackends + MyAuxProcType : -1))

static polar_net_stat *total_network_stat = NULL;

static void polar_network_stat_shmem_shutdown(int code, Datum arg);
static void polar_network_stat_accum(polar_net_stat *stat);
static void polar_network_stat_add(polar_net_stat *dest, polar_net_stat *src);

Size
network_stat_shmem_size(void)
{
	/* all backends + aux proc + one total stats for all dead proc */
	return sizeof(polar_net_stat) * (POLAR_MAX_NETWORK_STAT_SLOTS + 1);
}

/*
 * shmem_startup hook: allocate or attach to shared memory,
 */
void
polar_network_stat_shmem_startup(void)
{
	bool		found = false;
	Size		total_size = 0;

	/*
	 * Create or attach to the shared memory state
	 */
	LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);

	total_size = network_stat_shmem_size();
	polar_network_stat_array = ShmemInitStruct("polar_network_stat_array",
											   total_size, &found);
	elog(LOG, "polar_network_stat_array share memory total size is %d", (int) total_size);
	if (!found)
	{
		/* First time through ... */
		memset(polar_network_stat_array, 0, total_size);
	}

	/* POLAR: leaves the first slot to 'total_network_stat' */
	total_network_stat = polar_network_stat_array;
	polar_network_stat_array++;

	LWLockRelease(AddinShmemInitLock);

	if (!IsUnderPostmaster)
	{
		on_shmem_exit(polar_network_stat_shmem_shutdown, (Datum) 0);
	}
}

static void
polar_network_stat_shmem_shutdown(int code, Datum arg)
{
	int			index;

	elog(INFO, "shutting down");
	/* Safety check ... shouldn't get here unless shmem is set up. */
	if (!polar_network_stat_array)
		return;

	index = POLAR_NET_STAT_BACKEND_INDEX();
	elog(INFO, "network stat index is %d", index);

	if (index > 0)
	{
		polar_net_stat *stat = &polar_network_stat_array[index];

		polar_network_stat_add(total_network_stat, stat);
		MemSet(stat, 0, sizeof(polar_net_stat));
	}
}

/*
 * POLAR: return the network stat
 */
PG_FUNCTION_INFO_V1(polar_stat_network);
Datum
polar_stat_network(PG_FUNCTION_ARGS)
{
#define POLAR_NETWORK_STAT_COLS 7
	int			index = 1;
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	TupleDesc	tupdesc;
	Tuplestorestate *tupstore;
	MemoryContext per_query_ctx;
	MemoryContext oldcontext;
	polar_net_stat stat;

	memset(&stat, 0, sizeof(polar_net_stat));

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

	tupdesc = CreateTemplateTupleDesc(POLAR_NETWORK_STAT_COLS);
	TupleDescInitEntry(tupdesc, (AttrNumber) index++, "send_count",
					   INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) index++, "send_bytes",
					   INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) index++, "send_block_time",
					   INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) index++, "recv_count",
					   INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) index++, "recv_bytes",
					   INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) index++, "recv_block_time",
					   INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) index++, "total_retrans",
					   INT8OID, -1, 0);
	per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
	oldcontext = MemoryContextSwitchTo(per_query_ctx);

	tupstore = tuplestore_begin_heap(true, false, work_mem);
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = tupstore;
	rsinfo->setDesc = tupdesc;

	MemoryContextSwitchTo(oldcontext);

	if (!polar_network_stat_array)
	{
		tuplestore_donestoring(tupstore);
		return (Datum) 0;
	}

	polar_local_network_stat();
	polar_network_stat_accum(&stat);

	/* just one line now */
	{
		/* for each row */
		int			col = 0;
		Datum		values[POLAR_NETWORK_STAT_COLS];
		bool		nulls[POLAR_NETWORK_STAT_COLS];

		MemSet(values, 0, sizeof(values));
		MemSet(nulls, 0, sizeof(nulls));

		for (index = 0; index < POLAR_NETWORK_STAT_MAX; index++)
		{
			values[col++] = Int64GetDatumFast(stat.opstat[index].count);
			values[col++] = Int64GetDatumFast(stat.opstat[index].bytes);
			values[col++] = Int64GetDatumFast(stat.opstat[index].block_time);
		}

		values[col++] = Int64GetDatumFast(stat.total_retrans);
		tuplestore_putvalues(tupstore, tupdesc, values, nulls);
	}

	/* clean up and return the tuplestore */
	tuplestore_donestoring(tupstore);

	return (Datum) 0;
}

/*
 * POLAR: return the network stat for every process
 *        we do not reuse polar_stat_network codes now.
 */
PG_FUNCTION_INFO_V1(polar_proc_stat_network);
Datum
polar_proc_stat_network(PG_FUNCTION_ARGS)
{
#define POLAR_PROC_NETWORK_STAT_COLS 14
	int			num_backends = pgstat_fetch_stat_numbackends();
	int			curr;
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

	tupdesc = CreateTemplateTupleDesc(POLAR_PROC_NETWORK_STAT_COLS);
	TupleDescInitEntry(tupdesc, (AttrNumber) index++, "pid",
					   INT4OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) index++, "send_count",
					   INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) index++, "send_bytes",
					   INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) index++, "send_block_time",
					   INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) index++, "recv_count",
					   INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) index++, "recv_bytes",
					   INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) index++, "recv_block_time",
					   INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) index++, "sendq",
					   INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) index++, "recvq",
					   INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) index++, "cwnd",
					   INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) index++, "rtt",
					   INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) index++, "rttvar",
					   INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) index++, "total_retrans",
					   INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) index++, "tcpinfo_update_time",
					   INT8OID, -1, 0);
	per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
	oldcontext = MemoryContextSwitchTo(per_query_ctx);

	tupstore = tuplestore_begin_heap(true, false, work_mem);
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = tupstore;
	rsinfo->setDesc = tupdesc;

	MemoryContextSwitchTo(oldcontext);

	if (!polar_network_stat_array)
	{
		tuplestore_donestoring(tupstore);
		return (Datum) 0;
	}

	polar_local_network_stat();

	for (curr = 1; curr <= num_backends; curr++)
	{
		Datum		values[POLAR_PROC_NETWORK_STAT_COLS];
		bool		nulls[POLAR_PROC_NETWORK_STAT_COLS];
		int			i = 0;
		int			col = 0;

		PgBackendStatus *beentry;
		polar_net_stat *procstat;
		LocalPgBackendStatus *local_beentry = pgstat_fetch_stat_local_beentry(curr);

		if (!local_beentry)
			continue;

		beentry = &local_beentry->backendStatus;
		if (!CHECK_BACKENDID_VALID(beentry->backendid))
			continue;

		MemSet(values, 0, sizeof(values));
		MemSet(nulls, 0, sizeof(nulls));

		values[col++] = Int32GetDatum(beentry->st_procpid);

		procstat = &polar_network_stat_array[beentry->backendid - 1];

		/* pid */
		for (i = 0; i < POLAR_NETWORK_STAT_MAX; i++)
		{
			values[col++] = Int64GetDatumFast(procstat->opstat[i].count);
			values[col++] = Int64GetDatumFast(procstat->opstat[i].bytes);
			values[col++] = Int64GetDatumFast(polar_network_real_block_time(&procstat->opstat[i]));
		}

		values[col++] = Int64GetDatumFast(procstat->sendq);
		values[col++] = Int64GetDatumFast(procstat->recvq);
		values[col++] = Int64GetDatumFast(procstat->cwnd);
		values[col++] = Int64GetDatumFast(procstat->rtt);
		values[col++] = Int64GetDatumFast(procstat->rttvar);
		values[col++] = Int64GetDatumFast(procstat->total_retrans);
		values[col++] = Int64GetDatumFast(procstat->tcpinfo_update_time);
		tuplestore_putvalues(tupstore, tupdesc, values, nulls);
	}
	/* clean up and return the tuplestore */
	tuplestore_donestoring(tupstore);

	return (Datum) 0;
}

/*
 *  accumulate network stat for all backends
 */
static void
polar_network_stat_accum(polar_net_stat *stat)
{
	int			i = 0;

	*stat = *total_network_stat;

	for (i = 0; i < POLAR_MAX_NETWORK_STAT_SLOTS; i++)
		polar_network_stat_add(stat, &polar_network_stat_array[i]);
}

static void
polar_network_stat_add(polar_net_stat *dest, polar_net_stat *src)
{
	int			i = 0;

	for (i = 0; i < POLAR_NETWORK_STAT_MAX; i++)
	{
		dest->opstat[i].bytes += src->opstat[i].bytes;
		dest->opstat[i].count += src->opstat[i].count;
		dest->opstat[i].block_time += polar_network_real_block_time(&src->opstat[i]);
	}

	dest->total_retrans += src->total_retrans;
}
