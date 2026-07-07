/*-------------------------------------------------------------------------
 *
 * polar_shmem_tune.h
 *	  Placement tuning for the main shared-memory segment.
 *
 * Helpers that place PolarDB's main shared-memory segment well on NUMA
 * hardware, applied regardless of how the segment is backed (anonymous mmap or
 * the zero-copy memfd, see polar_zc.h):
 *
 *	- polar_shmem_numa_interleave_range() installs an MPOL_INTERLEAVE policy on
 *	  the whole segment, so its pages are spread evenly across NUMA nodes.
 *
 * Gated by the polar_shmem_numa_interleave PGC_POSTMASTER GUC; Linux-only and
 * best-effort, a no-op elsewhere.
 *
 * IDENTIFICATION
 *	  src/include/storage/polar_shmem_tune.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef POLAR_SHMEM_TUNE_H
#define POLAR_SHMEM_TUNE_H

/*
 * Interleave the main shared-memory segment across NUMA nodes. PGC_POSTMASTER.
 * Applies to any segment backing; a no-op on single-node hosts. See
 * polar_shmem_numa_interleave_range().
 */
extern PGDLLIMPORT bool polar_shmem_numa_interleave;

/*
 * Install an MPOL_INTERLEAVE policy on [base, base+len) before it is faulted,
 * so its pages are spread across NUMA nodes with CPUs that the process may
 * allocate from. Gated by polar_shmem_numa_interleave; best-effort, a no-op
 * with fewer than two such nodes, and it stands aside (reporting why, at LOG)
 * when the postmaster already carries an explicit mempolicy. Returns true if
 * the policy was actually installed, false otherwise. Must run before the
 * range is touched (i.e. at
 * segment creation).
 */
extern void polar_shmem_numa_interleave_range(void *base, Size len);

#endif							/* POLAR_SHMEM_TUNE_H */
