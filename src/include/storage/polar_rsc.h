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
#include "storage/relfilelocator.h"
#include "utils/relcache.h"

/* GUCs */
extern PGDLLIMPORT int polar_rsc_shared_relations;
extern PGDLLIMPORT int polar_rsc_pool_sweep_times;
extern PGDLLIMPORT bool polar_enable_rel_size_cache;
extern PGDLLIMPORT bool polar_enable_replica_rel_size_cache;
extern PGDLLIMPORT bool polar_enable_standby_rel_size_cache;

typedef struct polar_rsc_shared_relation_t
{
	RelFileLocator rlocator;
	BlockNumber nblocks[MAX_FORKNUM + 1];
	pg_atomic_uint32 flags;
	pg_atomic_uint64 generation;	/* mapping change */
	int64		usecount;		/* used for clock sweep */
} polar_rsc_shared_relation_t;

/*
 * POLAR: RSC search mode definition.
 *
 * Sometimes people call smgrnblocks-related functions for a definitive nblocks
 * value, while sometimes not: if there is no valid value in memory, no big deal.
 * So I define several modes to improve flexibility.
 *
 * POLAR_RSC_SEARCH_NEVER: never search, return directly.
 *
 * POLAR_RSC_SEARCH_MEMORY_ONLY: just search in memory, return if found; if not
 * found, don't ask for file system. Used for trying to get value fast by LUCK.
 *
 * POLAR_RSC_SEARCH_NO_EVICT: search in memory, return if found; if not found,
 * ask file system for the real value, but do not put the value back into RSC.
 * Used for getting a definitive value as fast as possible, without future use.
 *
 * POLAR_RSC_SEARCH_AND_EVICT: search in memory, if not found, ask file system
 * and put the value into RSC for future use. Used for getting a definitive
 * value fast, and be sure about it will be used in the near future.
 *
 * POLAR_RSC_NOEXIST_SEARCH_AND_EVICT: normal RSC search is under the assumption
 * of the file exists, while under this mode the assumption is not meet, so
 * before asking for real nblocks value, the existence must be checked, and fill
 * in 0 if the file does not exist.
 */
typedef enum polar_rsc_search_mode_t
{
	POLAR_RSC_SEARCH_NEVER,
	POLAR_RSC_SEARCH_MEMORY_ONLY,
	POLAR_RSC_SEARCH_NO_EVICT,
	POLAR_RSC_SEARCH_AND_EVICT,
	POLAR_RSC_NOEXIST_SEARCH_AND_EVICT,
} polar_rsc_search_mode_t;

typedef enum polar_rsc_stat_version_t
{
	POLAR_RSC_STAT_V1 = 0,
	POLAR_RSC_STAT_V2,
} polar_rsc_stat_version_t;

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
#define POLAR_RSC_AVAILABLE(reln, forknum) \
	(POLAR_RSC_ENABLED() && \
	 !IsBootstrapProcessingMode() && \
	 !RelFileLocatorBackendIsTemp(reln->smgr_rlocator))

struct SMgrRelationData;

extern BlockNumber smgrnblocks_real(struct SMgrRelationData *reln, ForkNumber forknum);
extern BlockNumber polar_rsc_search_by_ref(struct SMgrRelationData *reln, ForkNumber forknum);
extern BlockNumber polar_rsc_search_by_mapping(struct SMgrRelationData *reln, ForkNumber forknum);
extern BlockNumber polar_rsc_search_cached_entry(struct SMgrRelationData *reln, ForkNumber forknum);
extern BlockNumber polar_rsc_search_entry(struct SMgrRelationData *reln, ForkNumber forknum,
										  polar_rsc_search_mode_t mode);
extern BlockNumber polar_rsc_update_entry(struct SMgrRelationData *reln,
										  ForkNumber forknum, BlockNumber nblocks,
										  polar_rsc_search_mode_t mode);
extern void polar_rsc_update_if_exists(RelFileLocator *rlocator,
									   ForkNumber forknum,
									   BlockNumber nblocks,
									   bool (*update_cond_cb) (BlockNumber newval, BlockNumber cached));
extern void polar_rsc_init_empty_entry(RelFileLocator *rlocator, ForkNumber forknum);

extern void polar_rsc_drop_entry(RelFileLocator *rlocator);
extern void polar_rsc_drop_entries(Oid dbOid, Oid spcOid);
extern void polar_rsc_release_holding_ref(void);

extern Size polar_rsc_shmem_size(void);
extern void polar_rsc_shmem_init(void);
extern void polar_rsc_backend_init(void);

extern bool polar_rsc_check_consistent(Relation rel, ForkNumber forknum);
extern void polar_rsc_stat_pool_entries(ReturnSetInfo *rsinfo, bool withlock);
extern void polar_rsc_stat_drop_buffer(bool full_scan);

extern void polar_rsc_global_stat_fetch(Datum *values, int len,
										polar_rsc_stat_version_t version);
extern void polar_rsc_global_stat_reset(void);
extern void polar_rsc_nblocks_lookup_stat_reset(void);
extern uint64 polar_rsc_nblocks_lookup_stat_hits(void);

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
