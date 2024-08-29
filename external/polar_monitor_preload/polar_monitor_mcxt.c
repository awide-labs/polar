/*-------------------------------------------------------------------------
 *
 * polar_monitor_mcxt.c
 *	  show memory context monitor
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
 *	  external/polar_monitor_preload/polar_monitor_mcxt.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "catalog/pg_type.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/ipc.h"
#include "storage/procarray.h"
#include "storage/procsignal.h"
#include "utils/builtins.h"
#include "utils/guc.h"

/* POLAR */
#include "polar_monitor_preload.h"

bool		polar_mcxt_view = true;
int			polar_mcxt_timeout = 30;	/* default 3 seconds */

BackendMemoryStat *memstats = NULL;

/* POLAR: memory context status */
static void
getMemoryContextStat(MemoryContext context, MemoryContextCounters *stat)
{
	AssertArg(MemoryContextIsValid(context));

	/* Examine the context itself */
	memset(stat, 0, sizeof(*stat));
	(*context->methods->stats) (context, NULL, NULL, stat, false);
}

static void
iterateMemoryContext(MemoryContextIteratorState *state)
{
	MemoryContext context = state->context;

	AssertArg(MemoryContextIsValid(context));

	if (context->firstchild)
	{
		/* perfor first-depth search */
		state->context = context->firstchild;
		state->level++;
	}
	else if (context->nextchild)
	{
		/* goto next child if current context doesn't have a child */
		state->context = context->nextchild;
	}
	else if (context->parent)
	{
		/*
		 * walk up on tree to first parent which has a next child, that parent
		 * context was already visited
		 */
		while (context)
		{
			context = context->parent;
			state->level--;

			if (context == NULL)
			{
				/* we visited the whole context's tree */
				state->context = NULL;
				break;
			}
			else if (context->nextchild)
			{
				state->context = context->nextchild;
				break;
			}
		}
	}
}

Size
getMemstatSize(void)
{
	return BMSSIZE;
}

static void
copyBackendMemoryStat(InstanceState *state, pid_t targetBackendId)
{
	LWLockAcquire(memstats->lock, LW_SHARED);
	if (memstats->pid == targetBackendId)
	{
		memcpy(state->stat, memstats, BMSSIZE);
		state->stat->lock = NULL;	/* just to be sure */
		state->iContext = 0;
	}
	else
		elog(ERROR, "the target backend is not the expected one");
	LWLockRelease(memstats->lock);
}

/*
 * Get target backend list of used memory in whole instance in bytes.
 */
PG_FUNCTION_INFO_V1(polar_get_memory_stats);
Datum
polar_get_memory_stats(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	InstanceState *state;
	Datum		values[7];
	bool		nulls[7];
	HeapTuple	tuple;
	MemoryContextStat *ContextStat;
	int			wait_times = polar_mcxt_timeout;
	pid_t		targetBackendId = PG_GETARG_INT32(0);

	if (!polar_mcxt_view || memstats == NULL)
		elog(ERROR, "no support polar_get_memory_stats");

	if (targetBackendId == MyProc->pid)
		elog(ERROR, "please use polar_get_local_mcxt to get current backend memory context");

	if (SRF_IS_FIRSTCALL())
	{
		TupleDesc	tupdesc;
		MemoryContext oldcontext;

		funcctx = SRF_FIRSTCALL_INIT();
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		/* Build a tuple descriptor for our result type */
		if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("function returning record called in context "
							"that cannot accept type record")));

		funcctx->tuple_desc = BlessTupleDesc(tupdesc);

		state = palloc0(sizeof(*state));

		/*
		 * we make a copy of backend stat struct to prevent lossing stat on
		 * the fly if that backend will exit while we are printing it
		 */
		state->stat = palloc(BMSSIZE);
		funcctx->user_fctx = state;

		MemoryContextSwitchTo(oldcontext);

		/* initialize the data_ready flag */
		pg_atomic_write_u32(&memstats->data_ready, 0);

		/* send signal to target backend to write the memory context */
		SendProcSignal(targetBackendId, POLAR_PROCSIG_BACKEND_MEMORY_CONTEXT, InvalidBackendId);

		/*
		 * try wait_times, if it can not get the memstat, the target backend
		 * may not exist
		 */
		while (wait_times--)
		{
			if (pg_atomic_read_u32(&memstats->data_ready) == 1)
			{
				copyBackendMemoryStat(state, targetBackendId);
				pg_atomic_write_u32(&memstats->data_ready, 0);
				break;
			}
			else
				pg_usleep(1000L);
		}

		if (wait_times <= 0)
			elog(ERROR, "target backend may not exists");

		Assert(state->stat->nContext <= N_MC_STAT);
	}

	funcctx = SRF_PERCALL_SETUP();
	state = (InstanceState *) funcctx->user_fctx;
	if (state->iContext < state->stat->nContext)
	{
		ContextStat = state->stat->stats + state->iContext;
		memset(nulls, 0, sizeof(nulls));

		/* Fill data */
		values[0] = Int32GetDatum(state->stat->pid);
		values[1] = PointerGetDatum(cstring_to_text(ContextStat->name.data));
		values[2] = Int32GetDatum(ContextStat->level);
		values[3] = Int64GetDatum(ContextStat->stat.nblocks);
		values[4] = Int64GetDatum(ContextStat->stat.freechunks);
		values[5] = Int64GetDatum(ContextStat->stat.totalspace);
		values[6] = Int64GetDatum(ContextStat->stat.freespace);

		/* Data are ready */
		tuple = heap_form_tuple(funcctx->tuple_desc, values, nulls);

		/* go next context */
		state->iContext++;

		SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tuple));
	}
	else
	{
		SRF_RETURN_DONE(funcctx);
	}
}

PG_FUNCTION_INFO_V1(polar_get_local_memory_stats);
Datum
polar_get_local_memory_stats(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	MemoryContextIteratorState *state;

	if (SRF_IS_FIRSTCALL())
	{
		TupleDesc	tupdesc;
		MemoryContext oldcontext;

		funcctx = SRF_FIRSTCALL_INIT();
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		/* Build a tuple descriptor for our result type */
		if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("function returning record called in context "
							"that cannot accept type record")));

		funcctx->tuple_desc = BlessTupleDesc(tupdesc);

		state = palloc0(sizeof(*state));
		state->context = TopMemoryContext;
		funcctx->user_fctx = state;

		MemoryContextSwitchTo(oldcontext);
	}

	funcctx = SRF_PERCALL_SETUP();
	state = (MemoryContextIteratorState *) funcctx->user_fctx;
	if (state && state->context)
	{
		Datum		values[7];
		bool		nulls[7];
		HeapTuple	tuple;
		MemoryContextCounters stat;

		getMemoryContextStat(state->context, &stat);
		memset(nulls, 0, sizeof(nulls));

		/* Fill data */
		values[0] = Int32GetDatum(MyProc->pid);
		values[1] = PointerGetDatum(cstring_to_text(state->context->name));
		values[2] = Int32GetDatum(state->level);
		values[3] = Int64GetDatum(stat.nblocks);
		values[4] = Int64GetDatum(stat.freechunks);
		values[5] = Int64GetDatum(stat.totalspace);
		values[6] = Int64GetDatum(stat.freespace);

		/* Data are ready */
		tuple = heap_form_tuple(funcctx->tuple_desc, values, nulls);

		/* go next context */
		iterateMemoryContext(state);

		SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tuple));
	}
	else
	{
		SRF_RETURN_DONE(funcctx);
	}
}

void
polar_set_signal_mctx(void)
{
	if (memstats == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("memory stat collection isn't worked ")));

	if (polar_check_proc_signal(POLAR_PROCSIG_BACKEND_MEMORY_CONTEXT))
	{
		pg_atomic_write_u32(&memstats->signal_ready, 1);
		InterruptPending = true;
		MemoryContextDumpPending = true;
	}
}

void
polar_check_signal_mctx(void)
{
	MemoryContextIteratorState state;

	if (pg_atomic_read_u32(&memstats->signal_ready) == 1)
	{
		/*
		 * wait if reader currently locks our slot
		 */
		LWLockAcquire(memstats->lock, LW_EXCLUSIVE);

		memstats->pid = MyProc->pid;
		memstats->nContext = 0;
		state.context = TopMemoryContext;
		state.level = 0;

		/*
		 * walk through all memory context and fill stat table in shared
		 * memory
		 */
		do
		{
			MemoryContextStat *mcs = memstats->stats + memstats->nContext;
			int			namelen = strlen(state.context->name);

			if (namelen > NAMEDATALEN - 1)
				namelen = NAMEDATALEN - 1;
			memcpy(mcs->name.data, state.context->name, namelen);
			mcs->name.data[namelen] = '\0';

			mcs->level = state.level;

			getMemoryContextStat(state.context, &mcs->stat);
			memstats->nContext++;

			iterateMemoryContext(&state);
		} while (state.context && memstats->nContext < N_MC_STAT);
		pg_atomic_write_u32(&memstats->signal_ready, 0);
		pg_atomic_write_u32(&memstats->data_ready, 1);
		LWLockRelease(memstats->lock);
	}
}
