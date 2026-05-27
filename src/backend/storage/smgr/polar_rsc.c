/*-------------------------------------------------------------------------
 *
 * polar_rsc.c
 *	  Support for PolarDB-PG relation size cache.
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
 *	  src/backend/storage/smgr/polar_rsc.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "catalog/pg_tablespace_d.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/polar_rsc.h"
#include "storage/shmem.h"
#include "storage/smgr.h"
#include "storage/s_lock.h"
#include "utils/hsearch.h"
#include "utils/rel.h"

/* POLAR: GUC definition */
int			polar_rsc_shared_relations;
int			polar_rsc_pool_sweep_times;

bool		polar_enable_rel_size_cache;
bool		polar_enable_replica_rel_size_cache;
bool		polar_enable_standby_rel_size_cache;

typedef struct polar_rsc_shared_relation_mapping_t
{
	RelFileLocator rlocator;
	int			index;
} polar_rsc_shared_relation_mapping_t;

typedef struct polar_rsc_stat_t
{
	/* smgrnblocks call hit/miss rate */
	pg_atomic_uint64 nblocks_pointer_hit;
	pg_atomic_uint64 nblocks_mapping_hit;
	pg_atomic_uint64 nblocks_mapping_miss;

	/* mapping update hit/miss rate */
	pg_atomic_uint64 mapping_update_hit;
	pg_atomic_uint64 mapping_update_evict;
	pg_atomic_uint64 mapping_update_invalidate;

	/* dropping buffer counters */
	pg_atomic_uint64 drop_buffer_full_scan;
	pg_atomic_uint64 drop_buffer_hash_search;
} polar_rsc_stat_t;

#define NUM_POLAR_RSC_LOCK_PARTITIONS 128

typedef struct polar_rsc_shared_relation_pool_t
{
	pg_atomic_uint32 next_sweep;
	polar_rsc_stat_t global_stat;	/* statistics */
	LWLockPadded mapping_lock[NUM_POLAR_RSC_LOCK_PARTITIONS];
	polar_rsc_shared_relation_t entries[FLEXIBLE_ARRAY_MEMBER];
} polar_rsc_shared_relation_pool_t;

static polar_rsc_shared_relation_t *rsc_ref_held = NULL;
static polar_rsc_shared_relation_pool_t *rsc_pool = NULL;
static HTAB *rsc_mappings = NULL;

#define RSC_PARTITION_LOCK_BY_INDEX(index) \
	(&rsc_pool->mapping_lock[(index)].lock)
#define RSC_PARTITION_LOCK_BY_HASH(hash) \
	RSC_PARTITION_LOCK_BY_INDEX(hash % NUM_POLAR_RSC_LOCK_PARTITIONS)

#define RSC_LOCKED					0x01
#define RSC_VALID					0x02

/* Each forknum gets its own dirty bits. */
#define RSC_DIRTY(forknum)			(0x04 << (forknum))

/* Masks to test if any forknum is currently dirty. */
#define RSC_DIRTY_MASK				(((RSC_DIRTY(MAX_FORKNUM + 1) - 1) ^ (RSC_DIRTY(0) - 1)))

#define RSC_GLOBAL_STAT(field)	(&rsc_pool->global_stat.field)


Size
polar_rsc_shmem_size(void)
{
	Size		size = 0;

	if (polar_rsc_shared_relations <= 0)
		return size;

	size = add_size(size,
					offsetof(polar_rsc_shared_relation_pool_t, entries) +
					sizeof(polar_rsc_shared_relation_t) * polar_rsc_shared_relations);
	size = add_size(size,
					hash_estimate_size(polar_rsc_shared_relations,
									   sizeof(polar_rsc_shared_relation_mapping_t)));

	elog(LOG, "RSC total shared memory size is %lu", size);

	return size;
}

void
polar_rsc_shmem_init(void)
{
	HASHCTL		info;
	bool		found;
	uint32		i;

	if (polar_rsc_shared_relations <= 0)
		return;

	info.keysize = sizeof(RelFileLocator);
	info.entrysize = sizeof(polar_rsc_shared_relation_mapping_t);
	info.num_partitions = NUM_POLAR_RSC_LOCK_PARTITIONS;
	rsc_mappings = ShmemInitHash("PolarDB RSC shared relation mapping",
								 polar_rsc_shared_relations,
								 polar_rsc_shared_relations,
								 &info,
								 HASH_ELEM | HASH_BLOBS | HASH_PARTITION);

	rsc_pool = ShmemInitStruct("PolarDB RSC shared relation pool",
							   offsetof(polar_rsc_shared_relation_pool_t, entries) +
							   sizeof(polar_rsc_shared_relation_t) * polar_rsc_shared_relations,
							   &found);
	if (!found)
	{
		int			trancheid = LWLockNewTrancheId();

		pg_atomic_init_u32(&rsc_pool->next_sweep, 0);

		pg_atomic_init_u64(RSC_GLOBAL_STAT(nblocks_pointer_hit), 0);
		pg_atomic_init_u64(RSC_GLOBAL_STAT(nblocks_mapping_hit), 0);
		pg_atomic_init_u64(RSC_GLOBAL_STAT(nblocks_mapping_miss), 0);
		pg_atomic_init_u64(RSC_GLOBAL_STAT(mapping_update_hit), 0);
		pg_atomic_init_u64(RSC_GLOBAL_STAT(mapping_update_evict), 0);
		pg_atomic_init_u64(RSC_GLOBAL_STAT(mapping_update_invalidate), 0);
		pg_atomic_init_u64(RSC_GLOBAL_STAT(drop_buffer_full_scan), 0);
		pg_atomic_init_u64(RSC_GLOBAL_STAT(drop_buffer_hash_search), 0);

		LWLockRegisterTranche(trancheid, "PolarRSCMapping");
		for (i = 0; i < NUM_POLAR_RSC_LOCK_PARTITIONS; i++)
			LWLockInitialize(&rsc_pool->mapping_lock[i].lock, trancheid);

		for (i = 0; i < polar_rsc_shared_relations; i++)
		{
			pg_atomic_init_u32(&rsc_pool->entries[i].flags, 0);
			pg_atomic_init_u64(&rsc_pool->entries[i].generation, 0);
			rsc_pool->entries[i].usecount = 0;
		}
	}
}

static uint32
polar_rsc_lock_entry(polar_rsc_shared_relation_t *sr)
{
	SpinDelayStatus delayStatus;
	uint32		old_flags;

	START_CRIT_SECTION();
	init_local_spin_delay(&delayStatus);

	for (;;)
	{
		old_flags = pg_atomic_fetch_or_u32(&sr->flags, RSC_LOCKED);
		if (!(old_flags & RSC_LOCKED))
		{
			rsc_ref_held = sr;
			break;
		}
		perform_spin_delay(&delayStatus);
	}
	finish_spin_delay(&delayStatus);

	return old_flags | RSC_LOCKED;
}

static void
polar_rsc_unlock_entry(polar_rsc_shared_relation_t *sr, uint32 flags)
{
	pg_write_barrier();
	rsc_ref_held = NULL;
	pg_atomic_write_u32(&sr->flags, flags & ~RSC_LOCKED);

	END_CRIT_SECTION();
}

void
polar_rsc_release_holding_ref(void)
{
	if (rsc_ref_held)
		polar_rsc_unlock_entry(rsc_ref_held, 0);
}

static void
rsc_proc_exit_release(int code, Datum arg)
{
	polar_rsc_release_holding_ref();
}

void
polar_rsc_backend_init(void)
{
	on_shmem_exit(rsc_proc_exit_release, 0);
}

BlockNumber
polar_rsc_search_by_ref(SMgrRelation reln, ForkNumber forknum)
{
	polar_rsc_shared_relation_t *sr = reln->rsc_ref;

	if (sr)
	{
		BlockNumber result;

		pg_read_barrier();

		/*
		 * We can load int-sized values atomically without special measures.
		 */
		Assert(sizeof(sr->nblocks[forknum]) == sizeof(uint32));
		result = sr->nblocks[forknum];

		/*
		 * With a read barrier between the loads, we can check that the object
		 * still refers to the same rlocator before trusting the answer.
		 */
		pg_read_barrier();

		if (pg_atomic_read_u64(&sr->generation) == reln->rsc_generation)
		{
			/*
			 * No need to use atomic operation, usecount can be imprecise.
			 */
			if (sr->usecount < polar_rsc_pool_sweep_times)
				sr->usecount++;

			return result;
		}

		/*
		 * The generation doesn't match, the shared relation must have been
		 * evicted since we got a pointer to it.  We'll need to do more work.
		 */
		reln->rsc_ref = NULL;
	}

	return InvalidBlockNumber;
}

BlockNumber
polar_rsc_search_by_mapping(SMgrRelation reln, ForkNumber forknum)
{
	polar_rsc_shared_relation_mapping_t *mapping;
	polar_rsc_shared_relation_t *sr;
	uint32		hash;
	LWLock	   *mapping_lock;
	BlockNumber result = InvalidBlockNumber;

	hash = get_hash_value(rsc_mappings, &reln->smgr_rlocator.locator);
	mapping_lock = RSC_PARTITION_LOCK_BY_HASH(hash);

	LWLockAcquire(mapping_lock, LW_SHARED);
	mapping = hash_search_with_hash_value(rsc_mappings,
										  &reln->smgr_rlocator.locator,
										  hash,
										  HASH_FIND,
										  NULL);
	if (mapping)
	{
		sr = &rsc_pool->entries[mapping->index];
		result = sr->nblocks[forknum];

		/*
		 * No need to use atomic operation, usecount can be imprecise.
		 */
		if (sr->usecount < polar_rsc_pool_sweep_times)
			sr->usecount++;

		/*
		 * We can take the fast path until this entry is eventually evicted.
		 */
		reln->rsc_ref = sr;
		reln->rsc_generation = pg_atomic_read_u64(&sr->generation);
	}
	LWLockRelease(mapping_lock);

	return result;
}

static polar_rsc_shared_relation_t *
rsc_lru_pool_sweep(void)
{
	polar_rsc_shared_relation_t *sr;
	uint32		index;
	uint32		flags;
	int			sr_used_count = 0;

	for (;;)
	{
		/* Lock the next one in clock-hand order. */
		index = pg_atomic_fetch_add_u32(&rsc_pool->next_sweep, 1) % polar_rsc_shared_relations;
		sr = &rsc_pool->entries[index];

		flags = polar_rsc_lock_entry(sr);

		if (--sr->usecount <= 0)
			return sr;
		if (++sr_used_count >= polar_rsc_pool_sweep_times)
		{
			polar_rsc_unlock_entry(sr, flags);

			sr = &rsc_pool->entries[random() % polar_rsc_shared_relations];
			flags = polar_rsc_lock_entry(sr);
			return sr;
		}

		polar_rsc_unlock_entry(sr, flags);
	}
}

static polar_rsc_shared_relation_t *
rsc_alloc_entry(void)
{
	polar_rsc_shared_relation_mapping_t *mapping;
	polar_rsc_shared_relation_t *sr;
	uint32		index;
	LWLock	   *mapping_lock;
	uint32		flags;
	RelFileLocator rlocator;
	uint32		hash;

retry:
	sr = rsc_lru_pool_sweep();

	/*
	 * If it is not used, unlock and return this shared relation.
	 */
	flags = pg_atomic_read_u32(&sr->flags);
	if (!(flags & RSC_VALID))
	{
		polar_rsc_unlock_entry(sr, flags);
		return sr;
	}

	/*
	 * We need to get the partition LWLock first, or there will be spin stuck
	 * if we hold a locked shared relation and try to acquire the LWLock. so
	 * copy the rlocator and unlock the shared relation here.
	 */
	index = sr - rsc_pool->entries;
	rlocator = sr->rlocator;
	polar_rsc_unlock_entry(sr, flags);

	/*
	 * Get the partition LWLock of the shared relation to be replaced, and
	 * lock it exclusively.
	 */
	hash = get_hash_value(rsc_mappings, &rlocator);
	mapping_lock = RSC_PARTITION_LOCK_BY_HASH(hash);

	LWLockAcquire(mapping_lock, LW_EXCLUSIVE);

	/*
	 * Now we get the LWLock. Check if the shared relation picked earlier has
	 * gone from hash table. If so, we need to retry and pick another one.
	 */
	mapping = hash_search_with_hash_value(rsc_mappings,
										  &rlocator,
										  hash,
										  HASH_FIND,
										  NULL);
	if (!mapping || mapping->index != index)
	{
		LWLockRelease(mapping_lock);
		goto retry;
	}

	/*
	 * Lock up the shared relation, and invalidate it for other processes by
	 * bumping the generation.
	 */
	flags = polar_rsc_lock_entry(sr);
	Assert(flags & RSC_VALID);
	flags &= ~RSC_VALID;
	pg_atomic_add_fetch_u64(&sr->generation, 1);
	polar_rsc_unlock_entry(sr, flags);

	/*
	 * Evict the shared relation from the mapping table.
	 */
	hash_search_with_hash_value(rsc_mappings,
								&rlocator,
								hash,
								HASH_REMOVE,
								NULL);
	LWLockRelease(mapping_lock);

	return sr;
}

static bool
rsc_allocate_and_put(RelFileLocator *rlocator, BlockNumber *nblocks)
{
	polar_rsc_shared_relation_mapping_t *mapping;
	polar_rsc_shared_relation_t *sr;
	uint32		hash;
	LWLock	   *mapping_lock;
	uint32		flags;
	bool		found;

	hash = get_hash_value(rsc_mappings, rlocator);
	mapping_lock = RSC_PARTITION_LOCK_BY_HASH(hash);

rsc_evict:

	/*
	 * Find a new shared relation, choose an old one to evict if necessary. DO
	 * NOT lock up shared relation here because we need to hold LWLock first.
	 * Or else the process holding a spinlock may leave CPU and sleep, if the
	 * LWLock is really busy.
	 */
	sr = rsc_alloc_entry();

	/*
	 * Hold exclusive lock to create a mapping.
	 */
	LWLockAcquire(mapping_lock, LW_EXCLUSIVE);

	/*
	 * Lock up the shared relation again, and we expect it to be invalid. If
	 * it is valid, then it has been used by others, so we need to pick
	 * another one.
	 */
	flags = polar_rsc_lock_entry(sr);
	if (flags & RSC_VALID)
	{
		polar_rsc_unlock_entry(sr, flags);
		LWLockRelease(mapping_lock);
		goto rsc_evict;
	}

	/*
	 * The shared relation is locked up and safe to use. Try to enter shared
	 * relation into hash table, and unlock. If another process beats us and
	 * has already entered the hash table somewhere else, we should give up
	 * and pick that entry.
	 */
	mapping = hash_search_with_hash_value(rsc_mappings,
										  rlocator,
										  hash,
										  HASH_ENTER,
										  &found);
	if (found)
	{
		polar_rsc_unlock_entry(sr, 0);	/* = not valid */
		LWLockRelease(mapping_lock);
	}
	else
	{
		/*
		 * Success! Do some initialization for the evicted shared relation.
		 */
		mapping->index = sr - rsc_pool->entries;
		sr->usecount = 1;
		sr->rlocator = *rlocator;
		pg_atomic_add_fetch_u64(&sr->generation, 1);

		for (int i = 0; i <= MAX_FORKNUM; i++)
			sr->nblocks[i] = nblocks[i];

		polar_rsc_unlock_entry(sr, RSC_VALID);
		LWLockRelease(mapping_lock);

		pg_atomic_add_fetch_u64(RSC_GLOBAL_STAT(mapping_update_evict), 1);
	}

	/*
	 * Return true if we are the one who have inserted the entry.
	 */
	return !found;
}

static bool
_smgrexists_raw(SMgrRelation reln, ForkNumber forknum)
{
	struct stat fst;
	char		pathname[MAXPGPATH];
	char	   *path = relpath(reln->smgr_rlocator, forknum);

	snprintf(pathname, MAXPGPATH, "%s", path);
	pfree(path);

	/*
	 * Use stat instead of open to check existence, because open does more
	 * work.
	 */
	if (polar_stat(pathname, &fst) < 0)
	{
		if (errno == ENOENT)
			return false;
		else
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not stat file \"%s\": %m", pathname)));
	}

	return true;
}

BlockNumber
polar_rsc_update_entry(SMgrRelation reln,
					   ForkNumber forknum,
					   BlockNumber nblocks,
					   polar_rsc_search_mode_t mode)
{
	polar_rsc_shared_relation_mapping_t *mapping;
	polar_rsc_shared_relation_t *sr;
	uint32		hash;
	LWLock	   *mapping_lock;
	uint32		flags;

	hash = get_hash_value(rsc_mappings, &reln->smgr_rlocator.locator);
	mapping_lock = RSC_PARTITION_LOCK_BY_HASH(hash);

rsc_nblocks_update:
	sr = NULL;

	LWLockAcquire(mapping_lock, LW_SHARED);
	mapping = hash_search_with_hash_value(rsc_mappings,
										  &reln->smgr_rlocator.locator,
										  hash,
										  HASH_FIND,
										  NULL);
	if (mapping)
	{
		bool		is_extend = (nblocks != InvalidBlockNumber);

		/*
		 * If we don't have any knowledge about the relation size currently,
		 * call the internal routine to get.
		 */
		if (nblocks == InvalidBlockNumber)
		{
			switch (mode)
			{
				case POLAR_RSC_SEARCH_AND_EVICT:
					nblocks = smgrnblocks_real(reln, forknum);
					break;

				case POLAR_RSC_NOEXIST_SEARCH_AND_EVICT:
					if (_smgrexists_raw(reln, forknum))
						nblocks = smgrnblocks_real(reln, forknum);
					else
						nblocks = 0;
					break;

					/* unreachable */
				default:
					Assert(false);
					break;
			}
		}

		/*
		 * Concurrent update/lookup corner case: A backend calls smgrnblocks
		 * for lookup and misses the cache, so it calls smgrnblocks_real to
		 * get the real block size and is going to update the shared relation.
		 * Before it holds the partition lock, another backend calls
		 * smgrextend and extends the real block size and update the cache. If
		 * the first backend updates the shared relation, it may overwrite the
		 * new block size with its old block size. It is not expected.
		 *
		 * So we must mark the shared relation as dirty if it is updated by an
		 * extend. Further call of smgrnblocks (lookup) should not update the
		 * cached value. Only another extend can update the value.
		 */
		sr = &rsc_pool->entries[mapping->index];
		flags = polar_rsc_lock_entry(sr);

		if (is_extend)
		{
			/*
			 * Extend and truncate may update the value, and there are no
			 * races to worry about because they can have higher level
			 * exclusive locking on the relation.
			 */
			sr->nblocks[forknum] = nblocks;

			/*
			 * Mark the shared relation dirty, and make sure it stays dirty to
			 * avoid any shared relation lookup updating this entry, unless it
			 * is evicted and reloaded in clean state.
			 */
			flags |= RSC_DIRTY(forknum);
		}
		else if (!(flags & RSC_DIRTY(forknum)))
		{
			/*
			 * Non-dirty shared relation is allowed to update cleanly.
			 */
			sr->nblocks[forknum] = nblocks;
		}
		if (sr->usecount < polar_rsc_pool_sweep_times)
			sr->usecount++;
		polar_rsc_unlock_entry(sr, flags);
	}
	LWLockRelease(mapping_lock);

	/*
	 * We are done if the hash table mapping exists. Return with meaningful
	 * relation size.
	 */
	if (sr)
	{
		pg_atomic_add_fetch_u64(RSC_GLOBAL_STAT(mapping_update_hit), 1);
		return nblocks;
	}

	/*
	 * Do some more work if there is no mapping currently.
	 */
	{
		BlockNumber nblocks[MAX_FORKNUM + 1] = {InvalidBlockNumber, InvalidBlockNumber, InvalidBlockNumber, InvalidBlockNumber};

		rsc_allocate_and_put(&reln->smgr_rlocator.locator, nblocks);
	}

	/*
	 * There should be a valid shared relation in mapping. Get back to update
	 * nblocks of shared relation.
	 */
	goto rsc_nblocks_update;

	pg_unreachable();
	return nblocks;
}

/*
 * polar_rsc_search_cached_entry
 *
 * Pure shared-memory lookup: try the per-backend cached pointer first, then
 * the hash mapping.  Returns InvalidBlockNumber on miss without touching the
 * file system or evicting an entry.
 */
BlockNumber
polar_rsc_search_cached_entry(SMgrRelation reln, ForkNumber forknum)
{
	BlockNumber result;

	result = polar_rsc_search_by_ref(reln, forknum);
	if (result != InvalidBlockNumber)
	{
		pg_atomic_add_fetch_u64(RSC_GLOBAL_STAT(nblocks_pointer_hit), 1);
		return result;
	}

	result = polar_rsc_search_by_mapping(reln, forknum);
	if (result != InvalidBlockNumber)
	{
		pg_atomic_add_fetch_u64(RSC_GLOBAL_STAT(nblocks_mapping_hit), 1);
		return result;
	}

	pg_atomic_add_fetch_u64(RSC_GLOBAL_STAT(nblocks_mapping_miss), 1);

	return InvalidBlockNumber;
}

/*
 * polar_rsc_search_entry
 *
 * Search and update nblocks values inside RSC based on the mode.
 */
BlockNumber
polar_rsc_search_entry(SMgrRelation reln, ForkNumber forknum,
					   polar_rsc_search_mode_t mode)
{
	BlockNumber result;

	if (mode == POLAR_RSC_SEARCH_NEVER)
		return InvalidBlockNumber;

	result = polar_rsc_search_cached_entry(reln, forknum);
	if (result != InvalidBlockNumber)
		return result;

	switch (mode)
	{
		case POLAR_RSC_SEARCH_AND_EVICT:
		case POLAR_RSC_NOEXIST_SEARCH_AND_EVICT:
			result = polar_rsc_update_entry(reln, forknum, InvalidBlockNumber, mode);
			break;

		case POLAR_RSC_SEARCH_NO_EVICT:
			elog(WARNING, "RSC miss but no need to evict, fallback to real file system call");
			result = smgrnblocks_real(reln, forknum);
			break;

		case POLAR_RSC_SEARCH_MEMORY_ONLY:
			/* already InvalidBlockNumber */
			break;

		default:
			elog(PANIC, "unexpected RSC search mode: %d", mode);
	}

	return result;
}

/*
 * polar_rsc_update_if_exists
 *
 * Update the nblocks of the given relation fork if it exists in RSC and the
 * given nblocks value meets the condition, judged by the callback function.
 * Skip asking for real nblocks value if RSC miss, because we don't want to
 * involve I/O for startup process, which can be very slow under some
 * circumstances.
 *
 * NOTE: we can only override the value if we know the actual value in memory.
 * Relying on the value given by storage is not safe, because the file may
 * have been modified by primary. Trust the value from WAL record is more
 * accurate.
 */
void
polar_rsc_update_if_exists(RelFileLocator *rlocator,
						   ForkNumber forknum,
						   BlockNumber nblocks,
						   bool (*update_cond_cb) (BlockNumber newval, BlockNumber cached))
{
	polar_rsc_shared_relation_mapping_t *mapping;
	polar_rsc_shared_relation_t *sr;
	uint32		hash;
	uint32		flags;
	LWLock	   *mapping_lock;

	Assert(nblocks != InvalidBlockNumber);

	hash = get_hash_value(rsc_mappings, rlocator);
	mapping_lock = RSC_PARTITION_LOCK_BY_HASH(hash);

	LWLockAcquire(mapping_lock, LW_SHARED);
	mapping = hash_search_with_hash_value(rsc_mappings,
										  rlocator,
										  hash,
										  HASH_FIND,
										  NULL);
	if (mapping)
	{
		sr = &rsc_pool->entries[mapping->index];
		flags = polar_rsc_lock_entry(sr);

		/*
		 * We know the nblocks value in memory, update it.
		 */
		if (sr->nblocks[forknum] != InvalidBlockNumber)
		{
			if (update_cond_cb == NULL ||
				update_cond_cb(nblocks, sr->nblocks[forknum]))
			{
				sr->nblocks[forknum] = nblocks;
				flags |= RSC_DIRTY(forknum);
			}
		}

		polar_rsc_unlock_entry(sr, flags);
	}

	LWLockRelease(mapping_lock);
}

void
polar_rsc_init_empty_entry(RelFileLocator *rlocator, ForkNumber forknum)
{
	BlockNumber nblocks[MAX_FORKNUM + 1] = {0, 0, 0, InvalidBlockNumber};

	/*
	 * Initialize all fork length to 0 when MAIN fork is created. At that
	 * time, the length of VM / FSM fork must be 0 (not exist).
	 */
	Assert(forknum == MAIN_FORKNUM);

	/*
	 * Don't load relfilelocator under global tablespace into RSC because of
	 * uncontrolled flushing of relmapper.
	 */
	if (rlocator->spcOid != GLOBALTABLESPACE_OID &&
		rsc_allocate_and_put(rlocator, nblocks) == false)
	{
		elog(
#ifdef NBLOCKS_DEBUG
			 PANIC,
#else
			 WARNING,
#endif
			 "RelFileLocator %u/%u/%u already in RSC when replaying XLOG_SMGR_CREATE",
			 rlocator->spcOid, rlocator->dbOid, rlocator->relNumber);
	}
}

void
polar_rsc_drop_entry(RelFileLocator *rlocator)
{
	polar_rsc_shared_relation_mapping_t *mapping;
	polar_rsc_shared_relation_t *sr;
	uint32		hash;
	LWLock	   *mapping_lock;
	uint32		flags PG_USED_FOR_ASSERTS_ONLY;
	ForkNumber	forknum;

	hash = get_hash_value(rsc_mappings, rlocator);
	mapping_lock = RSC_PARTITION_LOCK_BY_HASH(hash);

	LWLockAcquire(mapping_lock, LW_EXCLUSIVE);
	mapping = hash_search_with_hash_value(rsc_mappings,
										  rlocator,
										  hash,
										  HASH_FIND,
										  NULL);
	if (mapping)
	{
		sr = &rsc_pool->entries[mapping->index];

		flags = polar_rsc_lock_entry(sr);
		Assert(flags & RSC_VALID);
		for (forknum = 0; forknum <= MAX_FORKNUM; forknum++)
			sr->nblocks[forknum] = InvalidBlockNumber;
		/* Mark it invalid and drop the mapping. */
		sr->usecount = 0;
		polar_rsc_unlock_entry(sr, flags & ~RSC_VALID);

		hash_search_with_hash_value(rsc_mappings,
									rlocator,
									hash,
									HASH_REMOVE,
									NULL);

		pg_atomic_add_fetch_u64(RSC_GLOBAL_STAT(mapping_update_invalidate), 1);
	}
	LWLockRelease(mapping_lock);
}

void
polar_rsc_drop_entries(Oid dbOid, Oid spcOid)
{
	int			i;
	uint32		flags;
	RelFileLocator rlocator;
	polar_rsc_shared_relation_t *sr;

	for (i = 0; i < polar_rsc_shared_relations; i++)
	{
		sr = &rsc_pool->entries[i];
		flags = polar_rsc_lock_entry(sr);

		if ((flags & RSC_VALID) &&
			((OidIsValid(dbOid) && sr->rlocator.dbOid == dbOid) ||
			 (OidIsValid(spcOid) && sr->rlocator.spcOid == spcOid)))
		{
			rlocator = sr->rlocator;
			polar_rsc_unlock_entry(sr, flags);
			polar_rsc_drop_entry(&rlocator);
		}
		else
			polar_rsc_unlock_entry(sr, flags);
	}
}

/*
 * polar_rsc_promote
 *
 * Clean up the RSC mapping table and clear the entries.
 */
void
polar_rsc_promote(void)
{
	polar_rsc_shared_relation_mapping_t *mapping;
	polar_rsc_shared_relation_t *sr;
	HASH_SEQ_STATUS status;
	int			i;
	int			j;

	for (i = 0; i < NUM_POLAR_RSC_LOCK_PARTITIONS; i++)
		LWLockAcquire(RSC_PARTITION_LOCK_BY_INDEX(i), LW_EXCLUSIVE);

	hash_seq_init(&status, rsc_mappings);
	while ((mapping = hash_seq_search(&status)))
		hash_search(rsc_mappings, &mapping->rlocator, HASH_REMOVE, NULL);

	for (i = 0; i < polar_rsc_shared_relations; i++)
	{
		sr = &rsc_pool->entries[i];

		polar_rsc_lock_entry(sr);

		pg_atomic_add_fetch_u64(&sr->generation, 1);
		sr->usecount = 0;
		for (j = 0; j <= MAX_FORKNUM; j++)
			sr->nblocks[j] = InvalidBlockNumber;

		polar_rsc_unlock_entry(sr, 0);
	}

	for (i = NUM_POLAR_RSC_LOCK_PARTITIONS; --i >= 0;)
		LWLockRelease(RSC_PARTITION_LOCK_BY_INDEX(i));
}

bool
polar_rsc_check_consistent(Relation rel, ForkNumber forknum)
{
	BlockNumber cached;
	BlockNumber actual;
	RelFileLocator rlocator;
	SMgrRelation reln = RelationGetSmgr(rel);
	bool		old_rel_size_cache_flag;

	/*
	 * Always consistent if RSC will not be used.
	 */
	if (!POLAR_RSC_AVAILABLE(reln, forknum))
	{
		elog(WARNING,
			 "relation cannot be appearing in RSC, "
			 "nblocks value will always be consistent");
		return true;
	}

	/*
	 * Always consistent if physical file does not exist. There can be no
	 * visibility map file or free space mapping file for a relation.
	 */
	if (!smgrexists(reln, forknum))
	{
		elog(WARNING,
			 "%s fork of relation %d does not exist, "
			 "nblocks will always be consistent",
			 forkNames[forknum], rel->rd_id);
		return true;
	}

	rlocator = reln->smgr_rlocator.locator;

	/*
	 * Get nblocks value of relation from RSC. If miss, just call the file
	 * system without eviction.
	 */
	cached = polar_rsc_search_entry(reln, forknum, POLAR_RSC_SEARCH_NO_EVICT);

	/*
	 * Disable RSC temporarily, and get the real nblocks by calling
	 * smgrnblocks through file system.
	 */
	old_rel_size_cache_flag = polar_enable_rel_size_cache;
	polar_enable_rel_size_cache = false;
	actual = smgrnblocks(reln, forknum);
	polar_enable_rel_size_cache = old_rel_size_cache_flag;

	RelationCloseSmgr(rel);

	if (cached != actual)
	{
		elog(WARNING,
			 "cached nblocks value of relation \"%s\" is not consistent with file system, "
			 "spcOid: %d, dbOid: %d, relNumber: %d; cached value: %d, actual value: %d",
			 RelationGetRelationName(rel),
			 rlocator.spcOid, rlocator.dbOid, rlocator.relNumber,
			 cached, actual);
		return false;
	}

	return true;
}

void
polar_rsc_stat_pool_entries(ReturnSetInfo *rsinfo, bool withlock)
{
#define RSC_ENTRY_MEM_COLS 16
	polar_rsc_shared_relation_t *sr;
	polar_rsc_shared_relation_mapping_t *mapping;
	RelFileLocator rlocator;
	uint32		hash;
	LWLock	   *mapping_lock;
	int			i;
	int			j;
	Datum		values[RSC_ENTRY_MEM_COLS];
	bool		isnull[RSC_ENTRY_MEM_COLS];

	for (i = 0; i < polar_rsc_shared_relations; i++)
	{
		uint32		flags;
		bool		found;
		bool		_rsc_enabled;
		SMgrRelation reln;
		bool		valid;

		j = 0;
		values[j++] = i;
		sr = &rsc_pool->entries[i];

		if (withlock)
			flags = polar_rsc_lock_entry(sr);
		rlocator = sr->rlocator;
		if (withlock)
			polar_rsc_unlock_entry(sr, flags);

		values[j++] = rlocator.spcOid;	/* spcOid */
		values[j++] = rlocator.dbOid;	/* dbOid */
		values[j++] = rlocator.relNumber;	/* relNumber */

		hash = get_hash_value(rsc_mappings, &rlocator);

		if (withlock)
		{
			mapping_lock = RSC_PARTITION_LOCK_BY_HASH(hash);
			LWLockAcquire(mapping_lock, LW_SHARED);
		}
		mapping = hash_search_with_hash_value(rsc_mappings,
											  &rlocator,
											  hash,
											  HASH_FIND,
											  &found);
		values[j++] = found && mapping->index == i; /* in_cache */

		if (withlock)
			LWLockRelease(mapping_lock);

		flags = pg_atomic_read_u32(&sr->flags);
		values[j++] = flags & RSC_LOCKED;	/* entry_locked */
		values[j++] = valid = flags & RSC_VALID;	/* entry_valid */
		values[j++] = flags & RSC_DIRTY_MASK;	/* entry_dirty */

		values[j++] = pg_atomic_read_u64(&sr->generation);	/* generation */
		values[j++] = sr->usecount; /* usecount */

		values[j++] = sr->nblocks[MAIN_FORKNUM];	/* main_cache */
		values[j++] = sr->nblocks[FSM_FORKNUM]; /* fsm_cache */
		values[j++] = sr->nblocks[VISIBILITYMAP_FORKNUM];	/* vm_cache */

		_rsc_enabled = polar_enable_rel_size_cache;
		polar_enable_rel_size_cache = false;

		if (RelFileNumberIsValid(rlocator.relNumber))
		{
			reln = smgropen(rlocator, INVALID_PROC_NUMBER);
			values[j++] = valid && smgrexists(reln, MAIN_FORKNUM) ?
				smgrnblocks_real(reln, MAIN_FORKNUM) : InvalidBlockNumber;	/* main_real */
			values[j++] = valid && smgrexists(reln, FSM_FORKNUM) ?
				smgrnblocks_real(reln, FSM_FORKNUM) : InvalidBlockNumber;	/* fsm_real */
			values[j++] = valid && smgrexists(reln, VISIBILITYMAP_FORKNUM) ?
				smgrnblocks_real(reln, VISIBILITYMAP_FORKNUM) : InvalidBlockNumber; /* vm_real */
			smgrclose(reln);
		}
		else
		{
			values[j++] = InvalidBlockNumber;	/* main_real */
			values[j++] = InvalidBlockNumber;	/* fsm_real */
			values[j++] = InvalidBlockNumber;	/* vm_real */
		}

		polar_enable_rel_size_cache = _rsc_enabled;

		MemSet(isnull, 0, sizeof(bool) * RSC_ENTRY_MEM_COLS);
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, isnull);
	}
#undef RSC_ENTRY_MEM_COLS
}

void
polar_rsc_stat_drop_buffer(bool full_scan)
{
	if (full_scan)
		pg_atomic_add_fetch_u64(RSC_GLOBAL_STAT(drop_buffer_full_scan), 1);
	else
		pg_atomic_add_fetch_u64(RSC_GLOBAL_STAT(drop_buffer_hash_search), 1);
}

void
polar_rsc_global_stat_fetch(Datum *values, int len,
							polar_rsc_stat_version_t version)
{
	int			i = 0;

	values[i++] = UInt64GetDatum(pg_atomic_read_u64(RSC_GLOBAL_STAT(nblocks_pointer_hit)));
	values[i++] = UInt64GetDatum(pg_atomic_read_u64(RSC_GLOBAL_STAT(nblocks_mapping_hit)));
	values[i++] = UInt64GetDatum(pg_atomic_read_u64(RSC_GLOBAL_STAT(nblocks_mapping_miss)));
	values[i++] = UInt64GetDatum(pg_atomic_read_u64(RSC_GLOBAL_STAT(mapping_update_hit)));
	values[i++] = UInt64GetDatum(pg_atomic_read_u64(RSC_GLOBAL_STAT(mapping_update_evict)));
	values[i++] = UInt64GetDatum(pg_atomic_read_u64(RSC_GLOBAL_STAT(mapping_update_invalidate)));

	if (version >= POLAR_RSC_STAT_V2)
	{
		values[i++] = UInt64GetDatum(pg_atomic_read_u64(RSC_GLOBAL_STAT(drop_buffer_full_scan)));
		values[i++] = UInt64GetDatum(pg_atomic_read_u64(RSC_GLOBAL_STAT(drop_buffer_hash_search)));
	}

	Assert(i == len);
}

void
polar_rsc_global_stat_reset(void)
{
	pg_atomic_write_u64(RSC_GLOBAL_STAT(nblocks_pointer_hit), 0LL);
	pg_atomic_write_u64(RSC_GLOBAL_STAT(nblocks_mapping_hit), 0LL);
	pg_atomic_write_u64(RSC_GLOBAL_STAT(nblocks_mapping_miss), 0LL);
	pg_atomic_write_u64(RSC_GLOBAL_STAT(mapping_update_hit), 0LL);
	pg_atomic_write_u64(RSC_GLOBAL_STAT(mapping_update_evict), 0LL);
	pg_atomic_write_u64(RSC_GLOBAL_STAT(mapping_update_invalidate), 0LL);
	pg_atomic_write_u64(RSC_GLOBAL_STAT(drop_buffer_full_scan), 0LL);
	pg_atomic_write_u64(RSC_GLOBAL_STAT(drop_buffer_hash_search), 0LL);
}

void
polar_rsc_nblocks_lookup_stat_reset(void)
{
	pg_atomic_write_u64(RSC_GLOBAL_STAT(nblocks_pointer_hit), 0);
	pg_atomic_write_u64(RSC_GLOBAL_STAT(nblocks_mapping_hit), 0);
	pg_atomic_write_u64(RSC_GLOBAL_STAT(nblocks_mapping_miss), 0);
}

uint64
polar_rsc_nblocks_lookup_stat_hits(void)
{
	return pg_atomic_read_u64(RSC_GLOBAL_STAT(nblocks_pointer_hit)) +
		pg_atomic_read_u64(RSC_GLOBAL_STAT(nblocks_mapping_hit));
}
