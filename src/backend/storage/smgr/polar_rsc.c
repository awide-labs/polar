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

/* RSC statistics */
polar_rsc_stat_t *polar_rsc_global_stat = NULL;

typedef struct polar_rsc_shared_relation_mapping_t
{
	RelFileNode rnode;
	int			index;
} polar_rsc_shared_relation_mapping_t;

static polar_rsc_shared_relation_t *rsc_ref_held = NULL;
static polar_rsc_shared_relation_pool_t *rsc_pool = NULL;
static HTAB *rsc_mappings = NULL;

size_t
polar_rsc_shmem_size(void)
{
	size_t		size = 0;

	if (polar_rsc_shared_relations <= 0)
		return size;

	size = add_size(size,
					offsetof(polar_rsc_shared_relation_pool_t, entries) +
					sizeof(polar_rsc_shared_relation_t) * polar_rsc_shared_relations);
	size = add_size(size,
					hash_estimate_size(polar_rsc_shared_relations,
									   sizeof(polar_rsc_shared_relation_mapping_t)));
	size = add_size(size, MAXALIGN(sizeof(polar_rsc_stat_t)));

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

	info.keysize = sizeof(RelFileNode);
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
		pg_atomic_init_u32(&rsc_pool->next_sweep, 0);
		for (i = 0; i < polar_rsc_shared_relations; i++)
		{
			pg_atomic_init_u32(&rsc_pool->entries[i].flags, 0);
			pg_atomic_init_u64(&rsc_pool->entries[i].generation, 0);
			rsc_pool->entries[i].usecount = 0;
		}
	}

	polar_rsc_global_stat = ShmemInitStruct("Polar RSC statistics",
											sizeof(polar_rsc_stat_t), &found);
	if (!found)
	{
		pg_atomic_init_u64(&polar_rsc_global_stat->nblocks_pointer_hit, 0);
		pg_atomic_init_u64(&polar_rsc_global_stat->nblocks_mapping_hit, 0);
		pg_atomic_init_u64(&polar_rsc_global_stat->nblocks_mapping_miss, 0);
		pg_atomic_init_u64(&polar_rsc_global_stat->mapping_update_hit, 0);
		pg_atomic_init_u64(&polar_rsc_global_stat->mapping_update_evict, 0);
		pg_atomic_init_u64(&polar_rsc_global_stat->mapping_update_invalidate, 0);
	}
}

uint32
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

void
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

		/* We can load int-sized values atomically without special measures. */
		Assert(sizeof(sr->nblocks[forknum]) == sizeof(uint32));
		result = sr->nblocks[forknum];

		/*
		 * With a read barrier between the loads, we can check that the object
		 * still refers to the same rnode before trusting the answer.
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

	hash = get_hash_value(rsc_mappings, &reln->smgr_rnode.node);
	mapping_lock = RSC_PARTITION_LOCK_BY_HASH(hash);

	LWLockAcquire(mapping_lock, LW_SHARED);
	mapping = hash_search_with_hash_value(rsc_mappings,
										  &reln->smgr_rnode.node,
										  hash,
										  HASH_FIND,
										  NULL);
	if (mapping)
	{
		sr = &rsc_pool->entries[mapping->index];
		result = sr->nblocks[forknum];

		/* no necessary to use a atomic operation, usecount can be imprecisely */
		if (sr->usecount < polar_rsc_pool_sweep_times)
			sr->usecount++;

		/* We can take the fast path until this SR is eventually evicted. */
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
	RelFileNode rnode;
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
	 * copy the rnode and unlock the shared relation here.
	 */
	index = sr - rsc_pool->entries;
	rnode = sr->rnode;
	polar_rsc_unlock_entry(sr, flags);

	/*
	 * Get the partition LWLock of the shared relation to be replaced, and
	 * lock it exclusively.
	 */
	hash = get_hash_value(rsc_mappings, &rnode);
	mapping_lock = RSC_PARTITION_LOCK_BY_HASH(hash);

	LWLockAcquire(mapping_lock, LW_EXCLUSIVE);

	/*
	 * Now we get the LWLock. Check if the shared relation picked earlier has
	 * gone from hash table. If so, we need to retry and pick another one.
	 */
	mapping = hash_search_with_hash_value(rsc_mappings,
										  &rnode,
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
								&rnode,
								hash,
								HASH_REMOVE,
								NULL);
	LWLockRelease(mapping_lock);

	return sr;
}

BlockNumber
polar_rsc_update_entry(SMgrRelation reln,
					   ForkNumber forknum,
					   BlockNumber nblocks)
{
	polar_rsc_shared_relation_mapping_t *mapping;
	polar_rsc_shared_relation_t *sr = NULL;
	uint32		hash;
	LWLock	   *mapping_lock;
	uint32		flags;
	int			i;
	bool		found;

	hash = get_hash_value(rsc_mappings, &reln->smgr_rnode.node);
	mapping_lock = RSC_PARTITION_LOCK_BY_HASH(hash);

rsc_nblocks_update:
	LWLockAcquire(mapping_lock, LW_SHARED);
	mapping = hash_search_with_hash_value(rsc_mappings,
										  &reln->smgr_rnode.node,
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
			nblocks = smgrnblocks_real(reln, forknum);

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
		pg_atomic_add_fetch_u64(&polar_rsc_global_stat->mapping_update_hit, 1);
		return nblocks;
	}

	/*
	 * Do some more work if there is no mapping currently.
	 */
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
	 * has already entered the hash table somewhere else, we should go back
	 * and pick that entry.
	 */
	mapping = hash_search_with_hash_value(rsc_mappings,
										  &reln->smgr_rnode.node,
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
		sr->rnode = reln->smgr_rnode.node;
		pg_atomic_add_fetch_u64(&sr->generation, 1);
		for (i = 0; i <= MAX_FORKNUM; i++)
			sr->nblocks[i] = InvalidBlockNumber;
		polar_rsc_unlock_entry(sr, RSC_VALID);
		LWLockRelease(mapping_lock);

		pg_atomic_add_fetch_u64(&polar_rsc_global_stat->mapping_update_evict, 1);
	}

	/*
	 * There should be a valid shared relation in mapping. Get back to update
	 * nblocks of shared relation.
	 */
	sr = NULL;
	goto rsc_nblocks_update;

	pg_unreachable();
	return nblocks;
}

BlockNumber
polar_rsc_search_entry(SMgrRelation reln, ForkNumber forknum, bool evict)
{
	BlockNumber result;

	result = polar_rsc_search_by_ref(reln, forknum);
	if (result != InvalidBlockNumber)
	{
		pg_atomic_add_fetch_u64(&polar_rsc_global_stat->nblocks_pointer_hit, 1);
		return result;
	}

	result = polar_rsc_search_by_mapping(reln, forknum);
	if (result != InvalidBlockNumber)
	{
		pg_atomic_add_fetch_u64(&polar_rsc_global_stat->nblocks_mapping_hit, 1);
		return result;
	}

	if (evict)
		result = polar_rsc_update_entry(reln, forknum, InvalidBlockNumber);
	else
	{
		elog(WARNING, "RSC miss but no need to evict, fallback to real file system call");
		result = smgrnblocks_real(reln, forknum);
	}

	pg_atomic_add_fetch_u64(&polar_rsc_global_stat->nblocks_mapping_miss, 1);

	return result;
}

void
polar_rsc_update_if_exists_and_greater_than(RelFileNode *rnode,
											ForkNumber forknum,
											BlockNumber nblocks)
{
	polar_rsc_shared_relation_mapping_t *mapping;
	polar_rsc_shared_relation_t *sr;
	uint32		hash;
	uint32		flags;
	LWLock	   *mapping_lock;

	hash = get_hash_value(rsc_mappings, rnode);
	mapping_lock = RSC_PARTITION_LOCK_BY_HASH(hash);

	LWLockAcquire(mapping_lock, LW_SHARED);
	mapping = hash_search_with_hash_value(rsc_mappings,
										  rnode,
										  hash,
										  HASH_FIND,
										  NULL);
	if (mapping)
	{
		sr = &rsc_pool->entries[mapping->index];
		flags = polar_rsc_lock_entry(sr);

		if (nblocks > sr->nblocks[forknum])
		{
			sr->nblocks[forknum] = nblocks;
			flags |= RSC_DIRTY(forknum);
		}

		polar_rsc_unlock_entry(sr, flags);
	}
	LWLockRelease(mapping_lock);
}

void
polar_rsc_drop_entry(RelFileNode *rnode)
{
	polar_rsc_shared_relation_mapping_t *mapping;
	polar_rsc_shared_relation_t *sr;
	uint32		hash;
	LWLock	   *mapping_lock;
	uint32		flags PG_USED_FOR_ASSERTS_ONLY;
	ForkNumber	forknum;

	hash = get_hash_value(rsc_mappings, rnode);
	mapping_lock = RSC_PARTITION_LOCK_BY_HASH(hash);

	LWLockAcquire(mapping_lock, LW_EXCLUSIVE);
	mapping = hash_search_with_hash_value(rsc_mappings,
										  rnode,
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
									rnode,
									hash,
									HASH_REMOVE,
									NULL);

		pg_atomic_add_fetch_u64(&polar_rsc_global_stat->mapping_update_invalidate, 1);
	}
	LWLockRelease(mapping_lock);
}

void
polar_rsc_drop_entries(Oid dbnode, Oid spcnode)
{
	int			i;
	uint32		flags;
	RelFileNode rnode;
	polar_rsc_shared_relation_t *sr;

	for (i = 0; i < polar_rsc_shared_relations; i++)
	{
		sr = &rsc_pool->entries[i];
		flags = polar_rsc_lock_entry(sr);

		if ((flags & RSC_VALID) &&
			((OidIsValid(dbnode) && sr->rnode.dbNode == dbnode) ||
			 (OidIsValid(spcnode) && sr->rnode.spcNode == spcnode)))
		{
			rnode = sr->rnode;
			polar_rsc_unlock_entry(sr, flags);
			polar_rsc_drop_entry(&rnode);
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
		hash_search(rsc_mappings, &mapping->rnode, HASH_REMOVE, NULL);

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
	RelFileNode rnode;
	SMgrRelation reln = RelationGetSmgr(rel);
	bool		old_rel_size_cache_flag;

	/*
	 * Always consistent if RSC will not be used.
	 */
	if (!POLAR_RSC_SHOULD_UPDATE(reln, forknum))
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

	rnode = reln->smgr_rnode.node;

	/*
	 * Get nblocks value of relation from RSC. If miss, just call the file
	 * system without eviction.
	 */
	cached = polar_rsc_search_entry(reln, forknum, false);

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
			 "spcNode: %d, dbNode: %d, relNode: %d; cached value: %d, actual value: %d",
			 RelationGetRelationName(rel),
			 rnode.spcNode, rnode.dbNode, rnode.relNode,
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
	RelFileNode rnode;
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
		rnode = sr->rnode;
		if (withlock)
			polar_rsc_unlock_entry(sr, flags);

		values[j++] = rnode.spcNode;	/* spec_node */
		values[j++] = rnode.dbNode; /* db_node */
		values[j++] = rnode.relNode;	/* rel_node */

		hash = get_hash_value(rsc_mappings, &rnode);

		if (withlock)
		{
			mapping_lock = RSC_PARTITION_LOCK_BY_HASH(hash);
			LWLockAcquire(mapping_lock, LW_SHARED);
		}
		mapping = hash_search_with_hash_value(rsc_mappings,
											  &rnode,
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

		reln = smgropen(rnode, InvalidBackendId);
		values[j++] = valid && smgrexists(reln, MAIN_FORKNUM) ?
			smgrnblocks_real(reln, MAIN_FORKNUM) : InvalidBlockNumber;	/* main_real */
		values[j++] = valid && smgrexists(reln, FSM_FORKNUM) ?
			smgrnblocks_real(reln, FSM_FORKNUM) : InvalidBlockNumber;	/* fsm_real */
		values[j++] = valid && smgrexists(reln, VISIBILITYMAP_FORKNUM) ?
			smgrnblocks_real(reln, VISIBILITYMAP_FORKNUM) : InvalidBlockNumber; /* vm_real */
		smgrclose(reln);

		polar_enable_rel_size_cache = _rsc_enabled;

		MemSet(isnull, 0, sizeof(bool) * RSC_ENTRY_MEM_COLS);
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, isnull);
	}
#undef RSC_ENTRY_MEM_COLS
}
