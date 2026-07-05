/*-------------------------------------------------------------------------
 *
 * test_polar_shm_aset.c
 *
 *
 * Copyright (c) 2022, Alibaba Group Holding Limited
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
 *  src/test/modules/test_polar_shm_aset/test_polar_shm_aset.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "storage/ipc.h"
#include "storage/shmem.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/resowner.h"
#include "utils/timestamp.h"

#include <stdlib.h>
#include <unistd.h>

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(test_shm_aset_random);
PG_FUNCTION_INFO_V1(test_shm_aset_random_parallel);

#define	TEST_SHM_SIZE	(10*1024*1024)

static shmem_startup_hook_type prev_shmem_startup_hook = NULL;
static dsa_area *area = NULL;

void		_PG_init(void);
void		_PG_fini(void);

/* Which order to free objects in, within each loop. */
typedef enum
{
	/* Free in random order. */
	MODE_RANDOM,
	/* Free in the same order we allocated (FIFO). */
	MODE_FORWARDS,
	/* Free in reverse order of allocation (LIFO). */
	MODE_BACKWARDS
} test_mode;

/* Per-worker results. */
typedef struct
{
	pid_t		pid;
	int64		count;
	int64		elapsed_time_us;
}			test_result;

/* Parameters for a test run, passed to workers. */
typedef struct
{
	int			loops;
	int			num_allocs;
	int			min_alloc;
	int			max_alloc;
	test_mode	mode;
	test_result results[1];		/* indexed by worker number */
} test_parameters;

/* The startup message given to each worker. */
typedef struct
{
	/* Where to find the parameters. */
	dsa_pointer parameters;
	/* What index this worker should write results to. */
	Size		output_index;
} test_msg;


static test_mode
parse_test_mode(text *mode)
{
	test_mode	result = MODE_RANDOM;
	char	   *cstr = text_to_cstring(mode);

	if (strcmp(cstr, "random") == 0)
		result = MODE_RANDOM;
	else if (strcmp(cstr, "forwards") == 0)
		result = MODE_FORWARDS;
	else if (strcmp(cstr, "backwards") == 0)
		result = MODE_BACKWARDS;
	else
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("unknown mode")));
	return result;
}

static void
check_parameters(const test_parameters *parameters)
{
	if (parameters->min_alloc < 1 || parameters->min_alloc > parameters->max_alloc)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("min_alloc must be >= 1, and min_alloc must be <= max_alloc")));
}

static int
my_tranche_id(void)
{
	static int	tranche_id = 0;

	if (tranche_id == 0)
		tranche_id = LWLockNewTrancheId();

	return tranche_id;
}

static void
test_shm_aset_init()
{
	void	   *base;
	bool		found;
	Size		size = TEST_SHM_SIZE;
	ShmAsetCtl *ctl;
	MemoryContext old_mcxt;

	if (prev_shmem_startup_hook)
		prev_shmem_startup_hook();

	elog(LOG, "init area and pin it.");
	old_mcxt = MemoryContextSwitchTo(TopMemoryContext);
	base = ShmemInitStruct("test_shm_aset", size, &found);
	if (!found)
	{
		dsa_area   *area = dsa_create_in_place(base, size, my_tranche_id(), NULL);

		dsa_set_size_limit(area, size);

		/* To prevent the area from being released */
		dsa_pin(area);
	}
	else
	{
		/* Attach to an existing area */
		area = dsa_attach_in_place(base, NULL);
	}

	/*
	 * shmem_startup_hook is called before polar_shm_aset_ctl_init, so we
	 * should call it to init shm_aset_ctl first.
	 */
	if (shm_aset_ctl == NULL)
		polar_shm_aset_ctl_init();

	Assert(shm_aset_ctl != NULL);

	/* Init shm_aset_ctl for global plan cache */
	ctl = &shm_aset_ctl[SHM_ASET_TYPE_GPC];
	ctl->base = base;
	ctl->size = size;

	MemoryContextSwitchTo(old_mcxt);
	elog(LOG, "init done.");
}

static int64
timestamp_diff(TimestampTz start, TimestampTz end)
{
	long		secs = 0;
	int			microsecs = 0;

	TimestampDifference(end, start, &secs, &microsecs);
	return secs * 1000 + microsecs;
}

void
_PG_init(void)
{
	if (!process_shared_preload_libraries_in_progress)
		return;

	RequestAddinShmemSpace(TEST_SHM_SIZE);

	prev_shmem_startup_hook = shmem_startup_hook;
	shmem_startup_hook = test_shm_aset_init;
}

void
_PG_fini(void)
{
	shmem_startup_hook = prev_shmem_startup_hook;
}

static void
test_shm_aset_ctl(ShmAsetCtl *ctl)
{
	Assert(ctl != NULL);
	Assert(ctl->magic == POLAR_SHM_ASET_MAGIC);
	Assert(ctl->type == SHM_ASET_TYPE_GPC);
	Assert(ctl->size == TEST_SHM_SIZE);
}

static void
init_local_area()
{
	MemoryContext old_mcxt;
	ShmAsetCtl *ctl = &shm_aset_ctl[SHM_ASET_TYPE_GPC];

	test_shm_aset_ctl(ctl);

	if (area != NULL)
		return;

	elog(LOG, "init local area");

	old_mcxt = MemoryContextSwitchTo(TopMemoryContext);
	area = dsa_attach_in_place(ctl->base, NULL);
	polar_shm_aset_set_area(SHM_ASET_TYPE_GPC, area);

	MemoryContextSwitchTo(old_mcxt);
}

static void
do_random_test(MemoryContext mcxt, Size output_index, test_parameters *parameters)
{
	void	  **objects;
	int			min_alloc;
	int			extra_alloc;
	int32		i;
	int32		loop;
	TimestampTz start_time = GetCurrentTimestamp();
	int64		total_allocations = 0;
	MemoryContext old_mcxt;

	/*
	 * Make tests reproducible (on the same computer at least) by using the
	 * same random sequence every time.
	 */
	srand(42);

	min_alloc = parameters->min_alloc;
	extra_alloc = parameters->max_alloc - parameters->min_alloc;

	objects = (void **) palloc(sizeof(void *) * parameters->num_allocs);
	Assert(objects != NULL);

	/* swtich to ShmAllocSet, then test allocate and free */
	old_mcxt = MemoryContextSwitchTo(mcxt);
	for (loop = 0; loop < parameters->loops; ++loop)
	{
		int			num_actually_allocated = 0;

		for (i = 0; i < parameters->num_allocs; ++i)
		{
			Size		size;

			/* Adjust size randomly if needed. */
			size = min_alloc;
			if (extra_alloc > 0)
				size += rand() % extra_alloc;

			/* Allocate! */
			objects[i] = palloc(size);
			if (objects[i] == NULL)
			{
				elog(LOG, "dsa: loop %d: out of memory after allocating %d objects", loop, i + 1);
				break;
			}
			++num_actually_allocated;
			/* Pay the cost of accessing that memory */
			memset(objects[i], 42, size);
		}
		if (parameters->mode == MODE_RANDOM)
		{
			for (i = 0; i < num_actually_allocated; ++i)
			{
				Size		x = rand() % num_actually_allocated;
				Size		y = rand() % num_actually_allocated;
				void	   *temp = objects[x];

				objects[x] = objects[y];
				objects[y] = temp;
			}
		}
		if (parameters->mode == MODE_BACKWARDS)
		{
			for (i = num_actually_allocated - 1; i >= 0; --i)
				pfree(objects[i]);
		}
		else
		{
			for (i = 0; i < num_actually_allocated; ++i)
				pfree(objects[i]);
		}
		total_allocations += num_actually_allocated;
	}
	MemoryContextSwitchTo(old_mcxt);
	pfree(objects);

	parameters->results[output_index].elapsed_time_us =
		timestamp_diff(start_time, GetCurrentTimestamp());
	parameters->results[output_index].pid = getpid();
	parameters->results[output_index].count = total_allocations;
}

Datum
test_shm_aset_random(PG_FUNCTION_ARGS)
{
	test_parameters parameters;
	MemoryContext mcxt;

	parameters.loops = PG_GETARG_INT32(0);
	parameters.num_allocs = PG_GETARG_INT32(1);
	parameters.min_alloc = PG_GETARG_INT32(2);
	parameters.max_alloc = PG_GETARG_INT32(3);
	parameters.mode = parse_test_mode(PG_GETARG_TEXT_PP(4));
	check_parameters(&parameters);

	init_local_area();
	Assert(area != NULL);

	mcxt = ShmAllocSetContextCreate(NULL, "test_shmaset",
									parameters.min_alloc,
									parameters.min_alloc,
									parameters.max_alloc,
									&shm_aset_ctl[SHM_ASET_TYPE_GPC]);

	MemoryContextStats(mcxt);
	do_random_test(mcxt, 0, &parameters);
	MemoryContextStats(mcxt);
	MemoryContextDelete(mcxt);

	PG_RETURN_NULL();
}

Datum		test_shm_aset_random_worker_main(Datum arg);

Datum
test_shm_aset_random_worker_main(Datum arg)
{
	test_msg	msg;
	test_parameters *parameters;
	MemoryContext mcxt;

	CurrentResourceOwner = ResourceOwnerCreate(NULL, "test_dsa toplevel");

	/* Receive hello message and attach to shmem area. */
	memcpy(&msg, MyBgworkerEntry->bgw_extra, sizeof(msg));
	init_local_area();
	Assert(area != NULL);

	parameters = dsa_get_address(area, msg.parameters);
	Assert(parameters != NULL);

	mcxt = ShmAllocSetContextCreate(NULL, "test_shmaset",
									parameters->min_alloc,
									parameters->min_alloc,
									parameters->max_alloc,
									&shm_aset_ctl[SHM_ASET_TYPE_GPC]);

	do_random_test(mcxt, msg.output_index, parameters);
	MemoryContextStats(mcxt);
	MemoryContextDelete(mcxt);

	return (Datum) 0;
}

/* Parallel version: fork a bunch of background workers to do it. */
Datum
test_shm_aset_random_parallel(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	TupleDesc	tupdesc;
	Tuplestorestate *tupstore;
	MemoryContext per_query_ctx;
	MemoryContext oldcontext;

	test_msg	msg;
	test_parameters *parameters;
	int			workers;
	int			i;
	BackgroundWorkerHandle **handles;

	/* tuplestore boilerplate stuff... */
	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that cannot accept a set")));
	if (!(rsinfo->allowedModes & SFRM_Materialize))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("materialize mode required, but it is not " \
						"allowed in this context")));
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
	oldcontext = MemoryContextSwitchTo(per_query_ctx);
	tupstore = tuplestore_begin_heap(true, false, work_mem);
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = tupstore;
	rsinfo->setDesc = tupdesc;
	MemoryContextSwitchTo(oldcontext);

	/* Prepare to work! */
	workers = PG_GETARG_INT32(5);
	handles = palloc(sizeof(BackgroundWorkerHandle *) * workers);

	init_local_area();
	Assert(area != NULL);

	/* Allocate space for the parameters object. */
	msg.parameters = dsa_allocate(area, sizeof(test_parameters) +
								  sizeof(test_result) * workers);
	Assert(DsaPointerIsValid(msg.parameters));

	/* Set up the parameters object. */
	parameters = dsa_get_address(area, msg.parameters);
	parameters->loops = PG_GETARG_INT32(0);
	parameters->num_allocs = PG_GETARG_INT32(1);
	parameters->min_alloc = PG_GETARG_INT32(2);
	parameters->max_alloc = PG_GETARG_INT32(3);
	parameters->mode = parse_test_mode(PG_GETARG_TEXT_PP(4));
	check_parameters(parameters);

	/* Start the workers. */
	for (i = 0; i < workers; ++i)
	{
		BackgroundWorker bgw;

		snprintf(bgw.bgw_name, sizeof(bgw.bgw_name), "worker%d", i);
		bgw.bgw_flags = BGWORKER_SHMEM_ACCESS;
		bgw.bgw_start_time = BgWorkerStart_PostmasterStart;
		bgw.bgw_restart_time = BGW_NEVER_RESTART;
		snprintf(bgw.bgw_library_name, sizeof(bgw.bgw_library_name),
				 "test_polar_shm_aset");
		snprintf(bgw.bgw_function_name, sizeof(bgw.bgw_function_name),
				 "test_shm_aset_random_worker_main");
		Assert(sizeof(parameters) <= BGW_EXTRALEN);
		/* Each worker will write its output to a different slot. */
		msg.output_index = i;
		memcpy(bgw.bgw_extra, &msg, sizeof(msg));
		bgw.bgw_notify_pid = MyProcPid;

		if (!RegisterDynamicBackgroundWorker(&bgw, &handles[i]))
			elog(ERROR, "Can't start worker");
	}

	/* Wait for the workers to complete. */
	for (i = 0; i < workers; ++i)
		/* erm, should really check for BGWH_STOPPED */
		WaitForBackgroundWorkerShutdown(handles[i]);

	/* Generate result tuples. */
	for (i = 0; i < workers; ++i)
	{
		Datum		values[3];
		bool		nulls[] = {false, false, false};
		Interval   *interval = palloc(sizeof(Interval));

		interval->month = 0;
		interval->day = 0;
		interval->time = parameters->results[i].elapsed_time_us
#ifndef HAVE_INT64_TIMESTAMP
			/ 1000000.0
#endif
			;

		values[0] = Int32GetDatum(parameters->results[i].pid);
		values[1] = Int64GetDatum(parameters->results[i].count);
		values[2] = PointerGetDatum(interval);
		tuplestore_putvalues(tupstore, tupdesc, values, nulls);
	}
	tuplestore_donestoring(tupstore);

	pfree(handles);

	return (Datum) 0;
}
