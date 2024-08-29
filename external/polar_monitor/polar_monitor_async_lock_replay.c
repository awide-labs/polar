/*-------------------------------------------------------------------------
 *
 * polar_monitor_async_lock_replay.c
 *	  show infomation of async replay locks.
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
 *    external/polar_monitor/polar_monitor_async_lock_replay.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/xlog.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "postmaster/polar_async_lock_replay.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/pg_lsn.h"
#include "utils/timestamp.h"

PG_FUNCTION_INFO_V1(polar_async_replay_lock);
Datum
polar_async_replay_lock(PG_FUNCTION_ARGS)
{
#define ASYNC_LOCK_REPLAY_COUNT 7
	int			cols = 1;
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	TupleDesc	tupdesc;
	Tuplestorestate *tupstore;
	MemoryContext per_query_ctx;
	MemoryContext oldcontext;
	HASH_SEQ_STATUS status;
	polar_alr_lock *alr_lock;
	polar_alr_xact *alr_xact = NULL;

	if (!polar_allow_alr())
		elog(ERROR, "polar_enable_async_lock_replay is disabled or it's not in replica mode");

	/* Build a tuple descriptor for our result type */
	tupdesc = CreateTemplateTupleDesc(ASYNC_LOCK_REPLAY_COUNT);
	TupleDescInitEntry(tupdesc, (AttrNumber) cols++, "xid",
					   XIDOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) cols++, "dbOid",
					   OIDOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) cols++, "relOid",
					   OIDOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) cols++, "lsn",
					   LSNOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) cols++, "rtime",
					   TIMESTAMPTZOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) cols++, "state",
					   TEXTOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) cols++, "running",
					   BOOLOID, -1, 0);

	per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
	oldcontext = MemoryContextSwitchTo(per_query_ctx);

	tupstore = tuplestore_begin_heap(true, false, work_mem);
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = tupstore;
	rsinfo->setDesc = tupdesc;

	MemoryContextSwitchTo(oldcontext);
	LWLockAcquire(&polar_alr_ctl->lock, LW_SHARED);
	hash_seq_init(&status, polar_alr_ctl->lock_tbl);
	while ((alr_lock = hash_seq_search(&status)))
	{
		Datum		values[ASYNC_LOCK_REPLAY_COUNT];
		bool		nulls[ASYNC_LOCK_REPLAY_COUNT];

		if (alr_xact == NULL || alr_lock->lock.xid != alr_xact->xid)
			alr_xact = hash_search(polar_alr_ctl->xact_tbl, &alr_lock->lock.xid, HASH_FIND, NULL);

		MemSet(nulls, 0, sizeof(bool) * ASYNC_LOCK_REPLAY_COUNT);
		values[0] = ObjectIdGetDatum(alr_lock->lock.xid);
		values[1] = ObjectIdGetDatum(alr_lock->lock.dbOid);
		values[2] = ObjectIdGetDatum(alr_lock->lock.relOid);
		values[3] = LSNGetDatum(alr_lock->lsn);
		values[4] = TimestampTzGetDatum(alr_lock->rtime);
		values[5] = CStringGetTextDatum(polar_alr_state_names[alr_lock->state]);
		values[6] = BoolGetDatum(alr_xact->running);

		tuplestore_putvalues(tupstore, tupdesc, values, nulls);
	}
	LWLockRelease(&polar_alr_ctl->lock);
	/* clean up and return the tuplestore */
	tuplestore_donestoring(tupstore);

	PG_RETURN_VOID();
}
