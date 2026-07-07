/*-------------------------------------------------------------------------
 *
 * polar_shmem_tune.c
 *	  Placement tuning for the main shared-memory segment.
 *
 * Placement helpers applied to PolarDB's main shared-memory segment regardless
 * of its backing (anonymous mmap or the zero-copy memfd): a whole-segment
 * MPOL_INTERLEAVE policy. Linux-only and best-effort.
 *
 * IDENTIFICATION
 *	  src/backend/storage/ipc/polar_shmem_tune.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#ifdef __linux__
#include <sys/syscall.h>
#include <unistd.h>
#endif

#include "port/pg_bitutils.h"
#include "storage/pg_shmem.h"	/* huge_pages, huge_pages_status */
#include "storage/polar_shmem_tune.h"

bool		polar_shmem_numa_interleave = true;

/*
 * NUMA interleave constants. Use the syscalls directly rather than depend on
 * libnuma / <numaif.h>, which the build does not link; these are stable kernel
 * ABI values.
 */
#ifndef MPOL_DEFAULT
#define MPOL_DEFAULT 0
#endif
#ifndef MPOL_INTERLEAVE
#define MPOL_INTERLEAVE 3
#endif
#ifndef MPOL_F_MEMS_ALLOWED
#define MPOL_F_MEMS_ALLOWED (1 << 2)
#endif
#define POLAR_NUMA_NMASK_BITS	1024
#define POLAR_NUMA_NMASK_LONGS	(POLAR_NUMA_NMASK_BITS / (8 * (int) sizeof(unsigned long)))

#if defined(__linux__) && defined(SYS_mbind) && defined(SYS_get_mempolicy)
/*
 * Drop memory-only nodes (CXL, HBM, PMEM) from mask. Leave it as is if
 * has_cpu can't be read.
 */
static void
polar_numa_keep_cpu_nodes(unsigned long *mask)
{
	unsigned long cpu_nodes[POLAR_NUMA_NMASK_LONGS] = {0};
	char		buf[4096];
	char	   *p = buf;
	FILE	   *f;

	if ((f = fopen("/sys/devices/system/node/has_cpu", "r")) == NULL)
		return;
	p = fgets(buf, sizeof(buf), f);
	fclose(f);
	if (p == NULL)
		return;

	while (*p && *p != '\n')
	{
		char	   *end;
		long		lo = strtol(p, &end, 10);
		long		hi = lo;

		if (end == p)
			return;				/* unexpected format */
		if (*end == '-')
			hi = strtol(end + 1, &end, 10);
		for (; lo <= hi && lo < POLAR_NUMA_NMASK_BITS; lo++)
			cpu_nodes[lo / (8 * sizeof(long))] |= 1UL << (lo % (8 * sizeof(long)));
		p = (*end == ',') ? end + 1 : end;
	}

	for (int i = 0; i < POLAR_NUMA_NMASK_LONGS; i++)
		mask[i] &= cpu_nodes[i];
}
#endif

/*
 * Apply MPOL_INTERLEAVE before pages are faulted. Anonymous mappings carry the
 * policy into forked children; regular memfds store it on the shmem inode, so
 * all mappings use it. Hugetlb memfds keep it VMA-local, so their pages must be
 * faulted through this mapping (see polar_shmem_prefault_range()).
 *
 * Do nothing with fewer than two allowed NUMA nodes with CPUs or when the
 * postmaster has an explicit memory policy. Failures are nonfatal and leave
 * first-touch placement in effect.
 */
void
polar_shmem_numa_interleave_range(void *base, Size len)
{
	if (!polar_shmem_numa_interleave)
		return;

#if defined(__linux__) && defined(SYS_mbind) && defined(SYS_get_mempolicy)
	{
		unsigned long nodemask[POLAR_NUMA_NMASK_LONGS];
		int			mode = MPOL_DEFAULT;
		int			before = 0;
		int			nnodes = 0;

		/*
		 * Leave an operator's explicit placement alone. A process mempolicy
		 * (numactl --membind / --interleave / --preferred, i.e.
		 * set_mempolicy) shows up in the mode returned by get_mempolicy()
		 * with flags 0; it is *not* reflected in MPOL_F_MEMS_ALLOWED below,
		 * which reports the cpuset mems (cgroup cpuset.mems, docker
		 * --cpuset-mems) instead. Both have to be consulted: mbind()
		 * validates its nodemask against the cpuset only, so without this
		 * check we would happily override a numactl --membind the operator
		 * asked for.
		 */
		if (syscall(SYS_get_mempolicy, &mode, NULL, 0ul, NULL, 0ul) != 0)
		{
			elog(WARNING, "shared-memory NUMA interleave: get_mempolicy() failed: %m");
			return;
		}

		if (mode != MPOL_DEFAULT)
		{
			/*
			 * Do not fight it - but say so, because inheriting a mempolicy
			 * unintentionally (a numactl wrapper in a service unit, a
			 * container runtime) is otherwise invisible: the operator would
			 * just see the interleave silently not happen.
			 */
			ereport(LOG,
					(errmsg("skipping shared-memory NUMA interleave: the postmaster already has an explicit memory policy (MPOL mode %d)",
							mode),
					 errhint("Placement is left to that policy, typically inherited from numactl.")));
			return;
		}

		memset(nodemask, 0, sizeof(nodemask));

		/* the set of nodes this process is allowed to allocate from */
		if (syscall(SYS_get_mempolicy, NULL, nodemask,
					(unsigned long) POLAR_NUMA_NMASK_BITS, NULL,
					(unsigned long) MPOL_F_MEMS_ALLOWED) != 0)
		{
			elog(WARNING, "shared-memory NUMA interleave: get_mempolicy(MEMS_ALLOWED) failed: %m");
			return;
		}

		before = pg_popcount((char *) nodemask, POLAR_NUMA_NMASK_BITS / 8);
		polar_numa_keep_cpu_nodes(nodemask);
		nnodes = pg_popcount((char *) nodemask, POLAR_NUMA_NMASK_BITS / 8);

		if (nnodes < before)
			ereport(LOG,
					(errmsg("excluding %d memory-only NUMA node(s) from shared-memory interleave",
							before - nnodes)));

		if (nnodes < 2)
			return;				/* one usable node: nothing to interleave */

		if (syscall(SYS_mbind, base, (unsigned long) len, MPOL_INTERLEAVE,
					nodemask, (unsigned long) POLAR_NUMA_NMASK_BITS, 0u) != 0)
		{
			elog(WARNING, "shared-memory NUMA interleave: mbind(MPOL_INTERLEAVE) failed: %m");
			return;
		}

		ereport(LOG,
				(errmsg("interleaved %zu-byte shared-memory segment across %d NUMA nodes",
						len, nnodes)));
	}
#endif
}
