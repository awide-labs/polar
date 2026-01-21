/*-------------------------------------------------------------------------
 *
 * polar_csn_mvcc_vars.h
 *	  polar csn mvcc related share global vars
 *
 * Portions Copyright (c) 2021, Alibaba Group Holding Limited
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *
 * src/include/access/polar_mvccvars.h
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "storage/shmem.h"
#include "access/polar_csn_mvcc_vars.h"
#include "access/transam.h"

polar_csn_mvcc_var_cache *polar_shmem_csn_mvcc_var_cache = NULL;

void
polar_csn_mvcc_var_cache_shmem_init(void)
{
	/* TODO: should align? by guangang.gg */
	polar_shmem_csn_mvcc_var_cache = (polar_csn_mvcc_var_cache *)
		ShmemAlloc(sizeof(*polar_shmem_csn_mvcc_var_cache));
	memset(polar_shmem_csn_mvcc_var_cache, 0, sizeof(*polar_shmem_csn_mvcc_var_cache));

	pg_atomic_init_u32(&polar_shmem_csn_mvcc_var_cache->polar_oldest_active_xid, InvalidTransactionId);
	pg_atomic_init_u64(&polar_shmem_csn_mvcc_var_cache->polar_next_csn, InvalidCommitSeqNo);
	pg_atomic_init_u64(&polar_shmem_csn_mvcc_var_cache->polar_latest_completed_xid, U64FromFullTransactionId(InvalidFullTransactionId));
}

/* Only used for test */
void
polar_csn_mvcc_var_cache_set(TransactionId oldest_active_xid, CommitSeqNo next_csn, FullTransactionId latest_completed_xid)
{
	pg_atomic_write_u32(&polar_shmem_csn_mvcc_var_cache->polar_oldest_active_xid, oldest_active_xid);
	pg_atomic_write_u64(&polar_shmem_csn_mvcc_var_cache->polar_next_csn, next_csn);
	pg_atomic_write_u64(&polar_shmem_csn_mvcc_var_cache->polar_latest_completed_xid, U64FromFullTransactionId(latest_completed_xid));
}
