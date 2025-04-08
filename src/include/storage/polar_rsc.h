/*-------------------------------------------------------------------------
 *
 * polar_rsc.h
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
 *	  src/include/storage/polar_rsc.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef POLAR_RSC_H
#define POLAR_RSC_H

#ifndef FRONTEND

#include "access/xlogreader.h"
#include "common/relpath.h"
#include "miscadmin.h"
#include "nodes/execnodes.h"
#include "port/atomics.h"
#include "storage/block.h"
#include "storage/relfilenode.h"
#include "utils/relcache.h"

/* GUCs */
extern PGDLLIMPORT int polar_rsc_shared_relations;
extern PGDLLIMPORT int polar_rsc_pool_sweep_times;
extern PGDLLIMPORT bool polar_enable_rel_size_cache;
extern PGDLLIMPORT bool polar_enable_replica_rel_size_cache;
extern PGDLLIMPORT bool polar_enable_standby_rel_size_cache;

typedef struct polar_rsc_shared_relation_t
{
	RelFileNode rnode;
	BlockNumber nblocks[MAX_FORKNUM + 1];
	pg_atomic_uint32 flags;
	pg_atomic_uint64 generation;	/* mapping change */
	int64		usecount;		/* used for clock sweep */
} polar_rsc_shared_relation_t;

typedef struct polar_rsc_shared_relation_pool_t
{
	pg_atomic_uint32 next_sweep;
	polar_rsc_shared_relation_t entries[FLEXIBLE_ARRAY_MEMBER];
} polar_rsc_shared_relation_pool_t;

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
} polar_rsc_stat_t;

#define RSC_PARTITION_LOCK_BY_INDEX(index) \
	(&MainLWLockArray[POLAR_RSC_LWLOCK_OFFSET + (index)].lock)
#define RSC_PARTITION_LOCK_BY_HASH(hash) \
	RSC_PARTITION_LOCK_BY_INDEX(hash % NUM_POLAR_RSC_LOCK_PARTITIONS)

#define RSC_LOCKED					0x01
#define RSC_VALID					0x02

/* Each forknum gets its own dirty bits. */
#define RSC_DIRTY(forknum)			(0x04 << (forknum))

/* Masks to test if any forknum is currently dirty. */
#define RSC_DIRTY_MASK				(((RSC_DIRTY(MAX_FORKNUM + 1) - 1) ^ (RSC_DIRTY(0) - 1)))

/*
 * Check if RSC is enabled based on GUC and role.
 */
#define POLAR_RSC_PRIMARY_ENABLED() \
	(polar_enable_rel_size_cache && polar_is_primary())

#define POLAR_RSC_REPLICA_ENABLED() \
	(polar_enable_rel_size_cache && \
	 polar_enable_replica_rel_size_cache && polar_is_replica())

#define POLAR_RSC_STANDBY_ENABLED() \
	(polar_enable_rel_size_cache && \
	 polar_enable_standby_rel_size_cache && polar_is_standby())

/*
 * I don't care about condition duplication, because we can leave the
 * deduplication work to compiler after macro expansion.
 */
#define POLAR_RSC_ENABLED() \
	(POLAR_RSC_PRIMARY_ENABLED() || \
	 POLAR_RSC_REPLICA_ENABLED() || \
	 POLAR_RSC_STANDBY_ENABLED())

/*
 * Check if the accessing of nblocks value of current relation should
 * update the corresponding RSC entry.
 */
#define POLAR_RSC_SHOULD_UPDATE(reln, forknum) \
	(POLAR_RSC_ENABLED() && \
	 !reln->polar_flag_for_bulk_extend[forknum] && \
	 !IsBootstrapProcessingMode() && \
	 !RelFileNodeBackendIsTemp(reln->smgr_rnode))

extern polar_rsc_stat_t *polar_rsc_global_stat;

struct SMgrRelationData;

extern BlockNumber smgrnblocks_real(struct SMgrRelationData *reln, ForkNumber forknum);
extern BlockNumber polar_rsc_search_by_ref(struct SMgrRelationData *reln, ForkNumber forknum);
extern BlockNumber polar_rsc_search_by_mapping(struct SMgrRelationData *reln, ForkNumber forknum);
extern BlockNumber polar_rsc_search_entry(struct SMgrRelationData *reln, ForkNumber forknum, bool evict);
extern BlockNumber polar_rsc_update_entry(struct SMgrRelationData *reln, ForkNumber forknum, BlockNumber nblocks);
extern void polar_rsc_update_if_exists_and_greater_than(RelFileNode *rnode,
														ForkNumber forknum,
														BlockNumber nblocks);

extern uint32 polar_rsc_lock_entry(polar_rsc_shared_relation_t *sr);
extern void polar_rsc_unlock_entry(polar_rsc_shared_relation_t *sr, uint32 flags);
extern void polar_rsc_drop_entry(RelFileNode *rnode);
extern void polar_rsc_drop_entries(Oid dbnode, Oid spcnode);
extern void polar_rsc_release_holding_ref(void);

extern size_t polar_rsc_shmem_size(void);
extern void polar_rsc_shmem_init(void);
extern void polar_rsc_backend_init(void);

extern bool polar_rsc_check_consistent(Relation rel, ForkNumber forknum);
extern void polar_rsc_stat_pool_entries(ReturnSetInfo *rsinfo, bool withlock);

/*
 * RSC on replica.
 */
typedef struct polar_rsc_replica_redo_cb_t
{
	void		(*redo) (XLogReaderState *record);
} polar_rsc_replica_redo_cb_t;

extern const polar_rsc_replica_redo_cb_t polar_rsc_replica_redo_cb[RM_N_BUILTIN_IDS];

extern void polar_rsc_promote(void);

#endif							/* FRONTEND */

#endif							/* POLAR_RSC_H */
