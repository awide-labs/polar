/*-------------------------------------------------------------------------
 *
 * polar_lock_stats.c
 *
 * Copyright (c) 2021, Alibaba Group Holding Limited
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
 *	  src/backend/storage/lmgr/polar_lock_stats.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "miscadmin.h"
#include "storage/backendid.h"
#include "storage/polar_lock_stats.h"
#include "utils/guc.h"

/* lwlock stat summary for local backend */
polar_lwlock_stat polar_lwlock_stat_local_summary;

/* lwlock stat detail for all backends */
polar_all_lwlocks_stat *polar_lwlocks_stat_array = NULL;

/* regular lock stat summary for local backend */
polar_regular_lock_stat polar_lock_stat_local_summary[LOCKTAG_LAST_TYPE + 1];

/* regular lock detail for all backends */
polar_all_locks_stat *polar_locks_stat_array = NULL;

#define POLAR_GET_LWLOCK_TRANCHE_ID(tranche) \
	(tranche >= 0 && tranche < POLAR_MAX_LWLOCK_TRANCHE ? tranche : 0)

#define IS_MYBACKEND_ID_VALID() \
	(MyBackendId != InvalidBackendId && MyBackendId >= 1 && MyBackendId <= MaxBackends)

#define IS_MYAUX_PROC_TYPE_VALID() (MyAuxProcType != NotAnAuxProcess)

#define POLAR_LOCK_STAT_BACKEND_INDEX() \
	(IS_MYBACKEND_ID_VALID() ? MyBackendId - 1 : \
		(IS_MYAUX_PROC_TYPE_VALID() ? MaxBackends + MyAuxProcType : -1))

#define POLAR_CHECK_LOCK_TYPE_AND_MODE(type, mode) \
	(type <= LOCKTAG_LAST_TYPE && mode >= 0 && mode <= AccessExclusiveLock)

/*
 *  init lwlock local stat
 */
void
polar_init_lwlock_local_stats(void)
{
	memset(&polar_lwlock_stat_local_summary, 0, sizeof(polar_lwlock_stat));
}

/*
 *  init regular lock local stat
 */
void
polar_init_lock_local_stats(void)
{
	memset(&polar_lock_stat_local_summary, 0, sizeof(polar_lock_stat_local_summary));
}

/*
 * lwlock stat acquire count
 */
void
polar_lwlock_stat_acquire(LWLock *lock, LWLockMode mode)
{
	if (!polar_enable_track_lock_stat)
		return;

	if (mode == LW_EXCLUSIVE)
		polar_lwlock_stat_local_summary.ex_acquire_count++;
	else
		polar_lwlock_stat_local_summary.sh_acquire_count++;

	if (polar_lwlocks_stat_array)
	{
		int			trancheid = POLAR_GET_LWLOCK_TRANCHE_ID(lock->tranche);
		int			index = POLAR_LOCK_STAT_BACKEND_INDEX();

		if (index < 0)
			return;

		if (mode == LW_EXCLUSIVE)
			polar_lwlocks_stat_array[index].lwlocks[trancheid].ex_acquire_count++;
		else
			polar_lwlocks_stat_array[index].lwlocks[trancheid].sh_acquire_count++;
	}
}

/*
 * lwlock stat block count
 */
void
polar_lwlock_stat_block(LWLock *lock)
{
	if (!polar_enable_track_lock_stat)
		return;

	polar_lwlock_stat_local_summary.block_count++;

	if (polar_lwlocks_stat_array)
	{
		int			trancheid = POLAR_GET_LWLOCK_TRANCHE_ID(lock->tranche);
		int			index = POLAR_LOCK_STAT_BACKEND_INDEX();

		if (index < 0)
			return;

		polar_lwlocks_stat_array[index].lwlocks[trancheid].block_count++;
	}
}

/*
 * lwlock stat record time
 */
void
polar_lwlock_stat_record_time(LWLock *lock, instr_time *start)
{
	if (!(polar_enable_track_lock_stat && polar_enable_track_lock_timing))
		return;

	if (!INSTR_TIME_IS_ZERO(*start))
	{
		instr_time	end;
		uint64		diff;

		INSTR_TIME_SET_CURRENT(end);
		diff = INSTR_TIME_GET_MICROSEC(end) - INSTR_TIME_GET_MICROSEC(*start);

		polar_lwlock_stat_local_summary.wait_time += diff;

		if (polar_lwlocks_stat_array)
		{
			int			trancheid = POLAR_GET_LWLOCK_TRANCHE_ID(lock->tranche);
			int			index = POLAR_LOCK_STAT_BACKEND_INDEX();

			if (index < 0)
				return;

			polar_lwlocks_stat_array[index].lwlocks[trancheid].wait_time += diff;
		}
	}
}

/*
 * regular lock stat lock count
 */
void
polar_lock_stat_lock(uint8 type, LOCKMODE mode)
{
	if (!polar_enable_track_lock_stat)
		return;

	if (!POLAR_CHECK_LOCK_TYPE_AND_MODE(type, mode))
		return;

	polar_lock_stat_local_summary[type].lock_count++;

	if (polar_locks_stat_array)
	{
		int			index = POLAR_LOCK_STAT_BACKEND_INDEX();

		if (index < 0)
			return;

		polar_locks_stat_array[index].detail[type][mode].lock_count++;
	}
}

/*
 * regular lock stat block count
 */
void
polar_lock_stat_block(uint8 type, LOCKMODE mode)
{
	if (!polar_enable_track_lock_stat)
		return;

	if (!POLAR_CHECK_LOCK_TYPE_AND_MODE(type, mode))
		return;

	polar_lock_stat_local_summary[type].block_count++;

	if (polar_locks_stat_array)
	{
		int			index = POLAR_LOCK_STAT_BACKEND_INDEX();

		if (index < 0)
			return;

		polar_locks_stat_array[index].detail[type][mode].block_count++;
	}
}

/*
 * regular lock stat fastpath count
 */
void
polar_lock_stat_fastpath(uint8 type, LOCKMODE mode)
{
	if (!polar_enable_track_lock_stat)
		return;

	if (!POLAR_CHECK_LOCK_TYPE_AND_MODE(type, mode))
		return;

	polar_lock_stat_local_summary[type].fastpath_count++;

	if (polar_locks_stat_array)
	{
		int			index = POLAR_LOCK_STAT_BACKEND_INDEX();

		if (index < 0)
			return;

		polar_locks_stat_array[index].detail[type][mode].fastpath_count++;
	}
}

/*
 * regular lock stat record time
 */
void
polar_lock_stat_record_time(uint8 type, LOCKMODE mode, instr_time *start)
{
	if (!(polar_enable_track_lock_stat && polar_enable_track_lock_timing))
		return;

	if (!POLAR_CHECK_LOCK_TYPE_AND_MODE(type, mode))
		return;

	if (!INSTR_TIME_IS_ZERO(*start))
	{
		instr_time	end;
		uint64		diff;

		INSTR_TIME_SET_CURRENT(end);
		diff = INSTR_TIME_GET_MICROSEC(end) - INSTR_TIME_GET_MICROSEC(*start);

		polar_lock_stat_local_summary[type].wait_time += diff;

		if (polar_locks_stat_array)
		{
			int			index = POLAR_LOCK_STAT_BACKEND_INDEX();

			if (index < 0)
				return;

			polar_locks_stat_array[index].detail[type][mode].wait_time += diff;
		}
	}
}
