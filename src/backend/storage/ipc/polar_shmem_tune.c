/*-------------------------------------------------------------------------
 *
 * polar_shmem_tune.c
 *	  Placement tuning for the main shared-memory segment.
 *
 * Placement helpers applied to PolarDB's main shared-memory segment regardless
 * of its backing (anonymous mmap or the zero-copy memfd): a whole-segment
 * MPOL_INTERLEAVE policy, and (see polar_shmem_prefault_main) prefaulting
 * of the whole segment. Both are Linux-only and best-effort.
 *
 * IDENTIFICATION
 *	  src/backend/storage/ipc/polar_shmem_tune.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#ifdef __linux__
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

#include "miscadmin.h"
#include "port/pg_bitutils.h"
#include "portability/instr_time.h"
#include "postmaster/bgworker.h"
#include "storage/pg_shmem.h"	/* huge_pages, huge_pages_status */
#include "storage/polar_shmem_tune.h"
#include "utils/guc.h"

bool		polar_shmem_numa_interleave = true;
bool		polar_shmem_prefault = true;

/*
 * MADV_POPULATE_WRITE advice (kernel >= 5.14). Defined by hand rather
 * than testing for the glibc macro (only since 2.36), so builds on older
 * toolchains still prefault on modern kernels; the kernel's own EINVAL at
 * runtime is the real availability signal.
 */
#if defined(__linux__) && !defined(MADV_POPULATE_WRITE)
#define MADV_POPULATE_WRITE 23
#endif

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
 * faulted through this mapping (see polar_shmem_prefault_main()).
 *
 * Do nothing with fewer than two allowed NUMA nodes with CPUs or when the
 * postmaster has an explicit memory policy. Failures are nonfatal and leave
 * first-touch placement in effect.
 */
bool
polar_shmem_numa_interleave_range(void *base, Size len)
{
	if (!polar_shmem_numa_interleave)
		return false;

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
			return false;
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
			return false;
		}

		memset(nodemask, 0, sizeof(nodemask));

		/* the set of nodes this process is allowed to allocate from */
		if (syscall(SYS_get_mempolicy, NULL, nodemask,
					(unsigned long) POLAR_NUMA_NMASK_BITS, NULL,
					(unsigned long) MPOL_F_MEMS_ALLOWED) != 0)
		{
			elog(WARNING, "shared-memory NUMA interleave: get_mempolicy(MEMS_ALLOWED) failed: %m");
			return false;
		}

		before = pg_popcount((char *) nodemask, POLAR_NUMA_NMASK_BITS / 8);
		polar_numa_keep_cpu_nodes(nodemask);
		nnodes = pg_popcount((char *) nodemask, POLAR_NUMA_NMASK_BITS / 8);

		if (nnodes < before)
			ereport(LOG,
					(errmsg("excluding %d memory-only NUMA node(s) from shared-memory interleave",
							before - nnodes)));

		if (nnodes < 2)
			return false;		/* one usable node: nothing to interleave */

		if (syscall(SYS_mbind, base, (unsigned long) len, MPOL_INTERLEAVE,
					nodemask, (unsigned long) POLAR_NUMA_NMASK_BITS, 0u) != 0)
		{
			elog(WARNING, "shared-memory NUMA interleave: mbind(MPOL_INTERLEAVE) failed: %m");
			return false;
		}

		ereport(LOG,
				(errmsg("interleaved %zu-byte shared-memory segment across %d NUMA nodes",
						len, nnodes)));
		return true;
	}
#endif
	return false;
}

#define POLAR_PREFAULT_CHUNK_SIZE	((Size) 256 * 1024 * 1024)
#define POLAR_PREFAULT_DELAY_US		10000L

/*
 * Register a one-shot worker to prefault the huge-page segment under the NUMA
 * policy installed during segment creation. The worker inherits the mapping and
 * its VMA policy across fork, so postmaster startup and recovery do not wait
 * for page allocation and zeroing.
 *
 * Segments not backed by huge pages are skipped.
 */
void
polar_shmem_prefault_start(void *base, Size len)
{
	static bool reported_no_huge = false;

	if (!polar_shmem_prefault)
		return;

	/* prefault only when the segment really got huge pages (see above) */
	if (huge_pages_status != HUGE_PAGES_ON)
	{
		if (!reported_no_huge && huge_pages != HUGE_PAGES_OFF)
		{
			reported_no_huge = true;
			ereport(LOG,
					(errmsg("not prefaulting shared memory: huge pages are "
							"not in use (huge_pages_status = \"%s\")",
							GetConfigOption("huge_pages_status", false, false)),
					 errhint("Configure a hugetlb pool so the shared-memory "
							 "segment can use huge pages, or set "
							 "polar_shmem_prefault = off.")));
		}
		return;
	}

#if defined(__linux__) && defined(MADV_POPULATE_WRITE)
	{
		BackgroundWorker worker;

		MemSet(&worker, 0, sizeof(worker));
		worker.bgw_flags = BGWORKER_SHMEM_ACCESS;
		worker.bgw_start_time = BgWorkerStart_PostmasterStart;
		worker.bgw_restart_time = BGW_NEVER_RESTART;
		snprintf(worker.bgw_library_name, BGW_MAXLEN, "postgres");
		snprintf(worker.bgw_function_name, BGW_MAXLEN,
				 "polar_shmem_prefault_main");
		snprintf(worker.bgw_name, BGW_MAXLEN, "shared-memory prefault");
		snprintf(worker.bgw_type, BGW_MAXLEN, "shared-memory prefault");
		worker.bgw_main_arg = PointerGetDatum(base);
		memcpy(worker.bgw_extra, &len, sizeof(len));
		RegisterBackgroundWorker(&worker);
	}
#endif
}

/*
 * Prefault in bounded chunks so shutdown interrupts are handled promptly and
 * the postmaster and backends can proceed concurrently. Pages already faulted
 * by another process are reused without being reallocated or zeroed again,
 * although the worker still populates its own page tables for them.
 */
void
polar_shmem_prefault_main(Datum main_arg)
{
#if defined(__linux__) && defined(MADV_POPULATE_WRITE)
	char	   *base = (char *) DatumGetPointer(main_arg);
	Size		len;
	Size		offset;
	instr_time	start_time;
	instr_time	end_time;

	memcpy(&len, MyBgworkerEntry->bgw_extra, sizeof(len));
	BackgroundWorkerUnblockSignals();
	INSTR_TIME_SET_CURRENT(start_time);
	ereport(LOG,
			(errmsg("starting shared-memory prefault: %zu bytes in %zu-byte chunks",
					len, POLAR_PREFAULT_CHUNK_SIZE)));

	/*
	 * base is an mmap() result, so it is page-aligned as madvise() requires;
	 * the kernel rounds len up.
	 */
	for (offset = 0; offset < len;)
	{
		Size		chunk = Min(len - offset, POLAR_PREFAULT_CHUNK_SIZE);

		CHECK_FOR_INTERRUPTS();
		if (madvise(base + offset, chunk, MADV_POPULATE_WRITE) != 0)
		{
			if (errno == EINVAL)
				ereport(WARNING,
						(errmsg("MADV_POPULATE_WRITE unsupported by this kernel "
								"(needs 5.14+); remaining shared memory left lazily faulted")));
			else
				ereport(WARNING,
						(errmsg("could not prefault %zu-byte shared-memory range "
								"at offset %zu, leaving remaining pages lazily "
								"faulted: %m", chunk, offset)));
			return;
		}

		offset += chunk;
		ereport(DEBUG1,
				(errmsg("shared-memory prefault progress: %zu of %zu bytes",
						offset, len)));
		if (offset < len)
			pg_usleep(POLAR_PREFAULT_DELAY_US);
	}

	CHECK_FOR_INTERRUPTS();
	INSTR_TIME_SET_CURRENT(end_time);
	INSTR_TIME_SUBTRACT(end_time, start_time);
	ereport(LOG,
			(errmsg("prefaulted %zu-byte shared-memory segment in %.3f s",
					len, INSTR_TIME_GET_DOUBLE(end_time))));
#endif
}
