/*-------------------------------------------------------------------------
 *
 * ipci.c
 *	  POSTGRES inter-process communication initialization code.
 *
 * Portions Copyright (c) 2024, Alibaba Group Holding Limited
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/storage/ipc/ipci.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/clog.h"
#include "access/commit_ts.h"
#include "access/heapam.h"
#include "access/multixact.h"
#include "access/nbtree.h"
#include "access/subtrans.h"
#include "access/syncscan.h"
#include "access/twophase.h"
#include "access/xlogprefetcher.h"
#include "access/xlogrecovery.h"
#include "commands/async.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "postmaster/autovacuum.h"
#include "postmaster/bgworker_internals.h"
#include "postmaster/bgwriter.h"
#include "postmaster/postmaster.h"
#include "replication/logicallauncher.h"
#include "replication/origin.h"
#include "replication/slot.h"
#include "replication/walreceiver.h"
#include "replication/walsender.h"
#include "storage/bufmgr.h"
#include "storage/dsm.h"
#include "storage/ipc.h"
#include "storage/pg_shmem.h"
#include "storage/pmsignal.h"
#include "storage/predicate.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "storage/procsignal.h"
#include "storage/sinvaladt.h"
#include "storage/spin.h"
#include "utils/snapmgr.h"
#include "utils/polar_features.h"

/* POLAR */
#include "access/polar_logindex_redo.h"
#include "postmaster/polar_async_lock_replay.h"
#include "storage/polar_rsc.h"
#include "storage/polar_xlogbuf.h"
/* POLAR end */

/* GUCs */
int			shared_memory_type = DEFAULT_SHARED_MEMORY_TYPE;

/* POLAR */
#include "common/file_perm.h"
#include "pg_config.h"
#include "postmaster/polar_parallel_bgwriter.h"
#include <sys/stat.h>
#include "utils/faultinjector.h"
#include <unistd.h>
/* POLAR end */

shmem_startup_hook_type shmem_startup_hook = NULL;

/* POLAR: used for polar_monitor hook */
polar_monitor_hook_type polar_monitor_hook = NULL;

static Size total_addin_request = 0;

/* POLAR */
static char *polar_shmem_stat_file = "polar_shmem_stat_file";
static void polar_output_shmem_stat(Size size);

/* POLAR end */

/*
 * RequestAddinShmemSpace
 *		Request that extra shmem space be allocated for use by
 *		a loadable module.
 *
 * This may only be called via the shmem_request_hook of a library that is
 * loaded into the postmaster via shared_preload_libraries.  Calls from
 * elsewhere will fail.
 */
void
RequestAddinShmemSpace(Size size)
{
	if (!process_shmem_requests_in_progress)
		elog(FATAL, "cannot request additional shared memory outside shmem_request_hook");
	total_addin_request = add_size(total_addin_request, size);
}

/*
 * CalculateShmemSize
 *		Calculates the amount of shared memory and number of semaphores needed.
 *
 * If num_semaphores is not NULL, it will be set to the number of semaphores
 * required.
 */
Size
CalculateShmemSize(int *num_semaphores)
{
	Size		size;
	int			numSemas;

	/* Compute number of semaphores we'll need */
	numSemas = ProcGlobalSemas();
	numSemas += SpinlockSemas();

	/* Return the number of semaphores if requested by the caller */
	if (num_semaphores)
		*num_semaphores = numSemas;

	/*
	 * Size of the Postgres shared-memory block is estimated via moderately-
	 * accurate estimates for the big hogs, plus 100K for the stuff that's too
	 * small to bother with estimating.
	 *
	 * We take some care to ensure that the total size request doesn't
	 * overflow size_t.  If this gets through, we don't need to be so careful
	 * during the actual allocation phase.
	 */
	size = 100000;
	size = add_size(size, PGSemaphoreShmemSize(numSemas));
	size = add_size(size, SpinlockSemaSize());
	size = add_size(size, hash_estimate_size(SHMEM_INDEX_SIZE,
											 sizeof(ShmemIndexEnt)));
	size = add_size(size, dsm_estimate_size());
	size = add_size(size, BufferShmemSize());
	size = add_size(size, LockShmemSize());
	size = add_size(size, PredicateLockShmemSize());
	size = add_size(size, ProcGlobalShmemSize());
	size = add_size(size, XLogPrefetchShmemSize());
	size = add_size(size, XLOGShmemSize());
	size = add_size(size, XLogRecoveryShmemSize());
	size = add_size(size, CLOGShmemSize());
	size = add_size(size, CommitTsShmemSize());
	size = add_size(size, SUBTRANSShmemSize());
	size = add_size(size, TwoPhaseShmemSize());
	size = add_size(size, BackgroundWorkerShmemSize());
	size = add_size(size, MultiXactShmemSize());
	size = add_size(size, LWLockShmemSize());
	size = add_size(size, ProcArrayShmemSize());
	size = add_size(size, BackendStatusShmemSize());
	size = add_size(size, SInvalShmemSize());
	size = add_size(size, PMSignalShmemSize());
	size = add_size(size, ProcSignalShmemSize());
	size = add_size(size, CheckpointerShmemSize());
	size = add_size(size, AutoVacuumShmemSize());
	size = add_size(size, ReplicationSlotsShmemSize());
	size = add_size(size, ReplicationOriginShmemSize());
	size = add_size(size, WalSndShmemSize());
	size = add_size(size, WalRcvShmemSize());
	size = add_size(size, PgArchShmemSize());
	size = add_size(size, ApplyLauncherShmemSize());
	size = add_size(size, SnapMgrShmemSize());
	size = add_size(size, BTreeShmemSize());
	size = add_size(size, SyncScanShmemSize());
	size = add_size(size, AsyncShmemSize());
	size = add_size(size, StatsShmemSize());
#ifdef FAULT_INJECTOR
	size = add_size(size, FaultInjector_ShmemSize());
#endif

#ifdef EXEC_BACKEND
	size = add_size(size, ShmemBackendArraySize());
#endif

	/* POLAR: add parallel background writer shared memory size */
	size = add_size(size, polar_parallel_bgwriter_shmem_size());

	/* POLAR: Add logindex share memory size */
	size = add_size(size, polar_logindex_redo_shmem_size());

	/* POLAR: add PolarDB xlog buffer share memory size */
	size = add_size(size, polar_xlog_buffer_shmem_size());

	/* POLAR: add async lock replay related share memory size */
	size = add_size(size, polar_alr_shmem_size());

	/* include additional requested shmem from preload libraries */
	size = add_size(size, total_addin_request);

	size = add_size(size, (Size) polar_shm_unused * BLCKSZ);

	/* POLAR: add RSC shared memory size */
	size = add_size(size, polar_rsc_shmem_size());

	/*
	 * NOTE NOTE NOTE: DO NOT ADD YOUR ADD_SIZE FUNCTION BELOW ME !!!
	 *
	 * ALL new add_size functions should be placed before the final round up
	 * operation. We add an assert to gurantee this. No one can stop you to
	 * put it behind the assert, of course ...
	 */

	/* might as well round it off to a multiple of a typical page size */
	size = add_size(size, 8192 - (size % 8192));
	Assert(size % 8192 == 0);

	return size;
}

static Size
polar_get_shared_mem_total_size(int *numSemas)
{
	int			low;
	int			high;
	Size		size;
	int			tmp_huge_page_total;

	/*
	 * POLAR: we control the total hugepage size by a binary-search of
	 * NBuffers. We use binary search to get last element in the range [0,
	 * polar_shm_limit) which CalculateShmemSize(numSemas) / BLCKSZ does not
	 * compare more than polar_shm_limit. We use upper_bound and minus 1 to
	 * get final target.
	 */
	tmp_huge_page_total = polar_shm_limit - polar_shm_reserved;
	if (tmp_huge_page_total <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("reserved huge pages cannot exceed total huge pages"),
				 errdetail("polar_shm_reserved should be less than polar_shm_limit")));

	/* POLAR: we must ensure polar_shm_limit >= NBuffers */
	NBuffers = tmp_huge_page_total;
	low = 0;
	high = NBuffers;

	/*
	 * POLAR: we must call CalculateShmemSize() here, because wal_buffers is
	 * based on NBuffers. And xlog_buffer is set in
	 * CalculateShmemSize()->XLOGShmemSize()->XLOGChooseNumBuffers().
	 */
	size = CalculateShmemSize(numSemas);
	/* POLAR: upper_bound */
	while (low < high)
	{
		int			num;

		NBuffers = low + ((high - low) >> 1);
		size = CalculateShmemSize(numSemas);
		num = size / BLCKSZ;

		/*
		 * POLAR: we can break with the first matched element, which is
		 * different with upper_bound.
		 */
		if (num == tmp_huge_page_total)
		{
			low = NBuffers + 1;
			break;
		}
		else if (num < tmp_huge_page_total)
			low = NBuffers + 1;
		else
			high = NBuffers;
	}
	NBuffers = low;

	/* ----------------
	 * POLAR: to ensure that NBuffers is precise.
	 * 1. CalculateShmemSize(numSemas) will be more than
	 * 	  polar_shm_limit with NBuffers.
	 * 2. CalculateShmemSize(numSemas) will be not more than
	 * 	  polar_shm_limit with NBuffers--.
	 * ----------------
	 */
	size = CalculateShmemSize(numSemas);
	Assert(size / BLCKSZ > tmp_huge_page_total);
	NBuffers--;

	if (NBuffers < 16)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("Too less shared buffers to start up PolarDB"),
				 errhint("Consider increasing polar_shm_limit or decreasing polar_shm_reserved, "
						 "or decreasing memory usage of other components")));
	size = CalculateShmemSize(numSemas);
	Assert(size / BLCKSZ <= tmp_huge_page_total);

	size = add_size(size, polar_feature_shmem_size());

	return size;
}

/*
 * CreateSharedMemoryAndSemaphores
 *		Creates and initializes shared memory and semaphores.
 *
 * This is called by the postmaster or by a standalone backend.
 * It is also called by a backend forked from the postmaster in the
 * EXEC_BACKEND case.  In the latter case, the shared memory segment
 * already exists and has been physically attached to, but we have to
 * initialize pointers in local memory that reference the shared structures,
 * because we didn't inherit the correct pointer values from the postmaster
 * as we do in the fork() scenario.  The easiest way to do that is to run
 * through the same code as before.  (Note that the called routines mostly
 * check IsUnderPostmaster, rather than EXEC_BACKEND, to detect this case.
 * This is a bit code-wasteful and could be cleaned up.)
 */
void
CreateSharedMemoryAndSemaphores(void)
{
	PGShmemHeader *shim = NULL;

	if (!IsUnderPostmaster)
	{
		PGShmemHeader *seghdr;
		Size		size;
		int			numSemas;

		/* Compute the size of the shared-memory block */

		if (polar_shm_limit)
			size = polar_get_shared_mem_total_size(&numSemas);
		else
			size = CalculateShmemSize(&numSemas);

		elog(DEBUG3, "invoking IpcMemoryCreate(size=%zu)", size);

		/*
		 * Create the shmem segment
		 */
		seghdr = PGSharedMemoryCreate(size, &shim);

		InitShmemAccess(seghdr);

		/* POLAR: output shared memory size to a world readable file */
		polar_output_shmem_stat(size);
		/* POLAR end */

		/*
		 * Create semaphores
		 */
		PGReserveSemaphores(numSemas);

		/*
		 * If spinlocks are disabled, initialize emulation layer (which
		 * depends on semaphores, so the order is important here).
		 */
#ifndef HAVE_SPINLOCKS
		SpinlockSemaInit();
#endif
	}
	else
	{
		/*
		 * We are reattaching to an existing shared memory segment. This
		 * should only be reached in the EXEC_BACKEND case.
		 */
#ifndef EXEC_BACKEND
		elog(PANIC, "should be attached to shared memory already");
#endif
	}

	/*
	 * Set up shared memory allocation mechanism
	 */
	if (!IsUnderPostmaster)
		InitShmemAllocation();

	/*
	 * Now initialize LWLocks, which do shared memory allocation and are
	 * needed for InitShmemIndex.
	 */
	CreateLWLocks();

	/*
	 * Set up shmem.c index hashtable
	 */
	InitShmemIndex();

	dsm_shmem_init();

	/*
	 * Set up xlog, clog, and buffers
	 */
	XLOGShmemInit();
	XLogPrefetchShmemInit();
	XLogRecoveryShmemInit();
	CLOGShmemInit();
	CommitTsShmemInit();
	SUBTRANSShmemInit();
	MultiXactShmemInit();
	InitBufferPool();

#ifdef FAULT_INJECTOR
	FaultInjector_ShmemInit();
#endif

	/* POLAR: init parallel background writer */
	polar_init_parallel_bgwriter();

	/* POLAR: Setup logindex */
	polar_logindex_redo_shmem_init();

	/* POLAR: init xlog buffer share memory */
	polar_init_xlog_buffer("XLog Block Buffer");

	/* POLAR: init RSC shared memory */
	polar_rsc_shmem_init();

	/* POLAR: init async lock replay share memory */
	polar_alr_shmem_init();

	/*
	 * Set up lock manager
	 */
	InitLocks();

	/*
	 * Set up predicate lock manager
	 */
	InitPredicateLocks();

	/*
	 * Set up process table
	 */
	if (!IsUnderPostmaster)
		InitProcGlobal();
	CreateSharedProcArray();
	CreateSharedBackendStatus();
	TwoPhaseShmemInit();
	BackgroundWorkerShmemInit();

	/*
	 * Set up shared-inval messaging
	 */
	CreateSharedInvalidationState();

	/*
	 * Set up interprocess signaling mechanisms
	 */
	PMSignalShmemInit();
	ProcSignalShmemInit();
	CheckpointerShmemInit();
	AutoVacuumShmemInit();
	ReplicationSlotsShmemInit();
	ReplicationOriginShmemInit();
	WalSndShmemInit();
	WalRcvShmemInit();
	PgArchShmemInit();
	ApplyLauncherShmemInit();

	/*
	 * Set up other modules that need some shared memory space
	 */
	SnapMgrInit();
	BTreeShmemInit();
	SyncScanShmemInit();
	AsyncShmemInit();
	StatsShmemInit();

	polar_feature_shmem_init();

#ifdef EXEC_BACKEND

	/*
	 * Alloc the win32 shared backend array
	 */
	if (!IsUnderPostmaster)
		ShmemBackendArrayAllocation();
#endif

	/* Initialize dynamic shared memory facilities. */
	if (!IsUnderPostmaster)
		dsm_postmaster_startup(shim);

	/*
	 * Now give loadable modules a chance to set up their shmem allocations
	 */
	if (shmem_startup_hook)
		shmem_startup_hook();
}

/*
 * InitializeShmemGUCs
 *
 * This function initializes runtime-computed GUCs related to the amount of
 * shared memory required for the current configuration.
 */
void
InitializeShmemGUCs(void)
{
	char		buf[64];
	Size		size_b;
	Size		size_mb;
	Size		hp_size;

	/*
	 * Calculate the shared memory size and round up to the nearest megabyte.
	 */
	size_b = CalculateShmemSize(NULL);
	size_mb = add_size(size_b, (1024 * 1024) - 1) / (1024 * 1024);
	sprintf(buf, "%zu", size_mb);
	SetConfigOption("shared_memory_size", buf,
					PGC_INTERNAL, PGC_S_DYNAMIC_DEFAULT);

	/*
	 * Calculate the number of huge pages required.
	 */
	GetHugePageSize(&hp_size, NULL);
	if (hp_size != 0)
	{
		Size		hp_required;

		hp_required = add_size(size_b / hp_size, 1);
		sprintf(buf, "%zu", hp_required);
		SetConfigOption("shared_memory_size_in_huge_pages", buf,
						PGC_INTERNAL, PGC_S_DYNAMIC_DEFAULT);
	}
}

/*
 * POLAR: output shared memory size to file
 */
static void
polar_output_shmem_stat(Size size)
{
	FILE	   *fshmemfile = fopen(polar_shmem_stat_file, "w");
	int			save_errno = errno;

	if (fshmemfile)
	{
		fprintf(fshmemfile, "%zu", size);
		fclose(fshmemfile);
		/* Make file world readable with mode as same as other files */
		if (chmod(polar_shmem_stat_file, pg_file_create_mode) != 0)
		{
			save_errno = errno;
			elog(ERROR, "could not change permissions of shmem_total_size file: %s", strerror(save_errno));
		}
	}
	else
		elog(ERROR, "could not write shmem_total_size file: %s", strerror(save_errno));
}

/*
 * POLAR: on_proc_exit callback to delete shared memory stat file
 */
void
polar_unlink_shmem_stat_file(int status, Datum arg)
{
	unlink(polar_shmem_stat_file);
}
