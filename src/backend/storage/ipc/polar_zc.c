/*-------------------------------------------------------------------------
 *
 * polar_zc.c
 *	  Zero-copy ("zc") memfd-backed main shared-memory segment.
 *
 * When zero-copy is enabled the whole main shared-memory segment is backed by a
 * single memfd (honoring huge pages) registered with pfsd, so device IO streams
 * straight out of any shmem-resident buffer instead of bouncing through pfsd's
 * pool. This file owns that segment's create/register and teardown-on-restart.
 *
 * IDENTIFICATION
 *	  src/backend/storage/ipc/polar_zc.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "storage/polar_fd.h"
#include "storage/polar_zc.h"

bool		polar_enable_zero_copy = true;

bool
polar_zc_storage_ready(void)
{
#ifdef USE_PFSD
	/* only pfsd can consume a registered memfd */
	return polar_enable_shared_storage_mode &&
		polar_local_vfs_state != POLAR_VFS_UNKNOWN &&
		polar_datadir != NULL &&
		polar_vfs_type_by_path(polar_datadir) == POLAR_VFS_PFS;
#else
	return false;
#endif
}

bool
polar_zc_main_segment_active(void)
{
	return polar_enable_zero_copy && polar_zc_storage_ready();
}

#ifdef USE_PFSD

#include <sys/mman.h>
#include <unistd.h>

#include "storage/pg_shmem.h"	/* GetHugePageSize, huge_pages */
#include "storage/shmem.h"		/* add_size */

/*
 * Postmaster-private handle for the one registered segment. In postmaster
 * process memory (NOT shared memory) so it survives the reset_shared internal
 * restart that recreates the memfd, letting the stale registration be dropped
 * before the fresh one is installed. Inherited across fork but only mutated in
 * the postmaster.
 *
 * Teardown lives at recreate time (polar_zc_region_init), not in an
 * on_shmem_exit handler, because on_shmem_exit runs in every backend's exit:
 * munmap there is harmless (per-process), but polar_unregister_buffer() drops
 * the GLOBAL pfsd registration, so the first backend to exit would kill
 * zero-copy for the rest. (Clean postmaster shutdown can't unregister either --
 * ExitPostmaster() unmounts pfsd before shmem_exit runs.) So the teardown below
 * runs only on crash-recovery restart, where pfsd is still mounted and this reg
 * still holds the previous generation's fd and buf_id.
 */
typedef struct PolarZcRegion
{
	bool		valid;
	int			fd;
	int64		buf_id;			/* >=0 registered; <0 registration failed */
	char	   *base;			/* mmap base == the registered region start */
	Size		size;			/* mapped length */
} PolarZcRegion;

static PolarZcRegion MainSegReg =
{
	false, -1, -1, NULL, 0
};

/*
 * Create (or, on internal restart, recreate) a memfd region of at least
 * usable_size bytes, register it with pfsd, and record it in *reg for teardown
 * on the next restart. Returns the mmap base. Requests huge pages when the
 * cluster asks for them (huge_pages = on/try).
 */
static char *
polar_zc_region_init(const char *name, Size usable_size, PolarZcRegion *reg)
{
	int			fd;
	Size		map_size;
	Size		hpsz;
	unsigned	mfd_flags;
	bool		want_huge;
	char	   *raw;
	int64		buf_id;

	/*
	 * Tear down a prior generation (internal restart): drop the pfsd-specific
	 * resources only. The mapping is released by AnonymousShmemDetach at the
	 * preceding shmem_exit(1), so munmap'ing reg->base here would
	 * double-unmap a possibly-reused VA.
	 */
	if (reg->valid)
	{
		if (reg->buf_id >= 0)
			polar_unregister_buffer(reg->buf_id);
		if (reg->fd >= 0)
			close(reg->fd);
		MemSet(reg, 0, sizeof(*reg));
	}

	want_huge = false;
#ifdef MFD_HUGETLB
	if (huge_pages == HUGE_PAGES_ON || huge_pages == HUGE_PAGES_TRY)
		want_huge = true;
#endif

	/*
	 * A memfd selects the page size at creation via MFD_HUGETLB (not mmap
	 * flags), then maps MAP_SHARED. Under huge_pages = try, retry with
	 * regular pages on any huge-page failure (memfd_create or mmap).
	 */
retry:
	mfd_flags = MFD_CLOEXEC;
	hpsz = 0;
	map_size = usable_size;
#ifdef MFD_HUGETLB
	if (want_huge)
	{
		Size		hugepagesize;
		int			mmap_flags;

		GetHugePageSize(&hugepagesize, &mmap_flags);

		/*
		 * GetHugePageSize returns the size-class in mmap_flags with the same
		 * shift-26 encoding as MFD_HUGE_*, so masking off MAP_HUGETLB carries
		 * the bits straight into mfd_flags.
		 */
		mfd_flags |= MFD_HUGETLB | (unsigned) (mmap_flags & ~MAP_HUGETLB);
		hpsz = hugepagesize;

		/* hugetlbfs requires the file length to be a huge-page multiple */
		if (map_size % hpsz != 0)
			map_size = add_size(map_size, hpsz - (map_size % hpsz));
	}
#endif

	fd = memfd_create(name, mfd_flags);
	if (fd < 0)
	{
		if (want_huge && huge_pages == HUGE_PAGES_TRY)
		{
			elog(DEBUG1, "memfd_create(MFD_HUGETLB) for \"%s\" failed, "
				 "falling back to regular pages: %m", name);
			want_huge = false;
			goto retry;
		}
		ereport(FATAL,
				(errcode_for_file_access(),
				 errmsg("could not create memfd for \"%s\": %m", name)));
	}

	if (ftruncate(fd, map_size) != 0)
		ereport(FATAL,
				(errcode_for_file_access(),
				 errmsg("could not size memfd \"%s\" to %zu bytes: %m",
						name, map_size)));

	raw = mmap(NULL, map_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (raw == MAP_FAILED)
	{
		if (want_huge && huge_pages == HUGE_PAGES_TRY)
		{
			close(fd);
			elog(DEBUG1, "mmap of MFD_HUGETLB memfd \"%s\" failed, "
				 "falling back to regular pages: %m", name);
			want_huge = false;
			goto retry;
		}
		ereport(FATAL,
				(errcode_for_file_access(),
				 errmsg("could not mmap memfd \"%s\" of %zu bytes: %m",
						name, map_size)));
	}

	/*
	 * Register the whole region; the *_zc shim computes buf_off relative to
	 * the mmap base (raw), which pfsd resolves against its own mapping of the
	 * memfd.
	 */
	buf_id = polar_register_buffer(fd, map_size, raw);
	if (buf_id < 0)
		ereport(WARNING,
				(errmsg("could not register region \"%s\" with pfsd; "
						"its IO will use the copying path", name)));

	reg->valid = true;
	reg->fd = fd;
	reg->buf_id = buf_id;
	reg->base = raw;
	reg->size = map_size;

	ereport(LOG,
			(errmsg("zero-copy region \"%s\": %s (memfd %zu bytes, %s pages)",
					name,
					(buf_id >= 0) ? "registered" : "registration failed (copying path)",
					map_size, (hpsz != 0) ? "huge" : "regular")));

	return raw;
}

void *
polar_zc_main_segment_create(Size *size)
{
	char	   *raw;

	/* honor whatever huge_pages requests for the main segment */
	raw = polar_zc_region_init("pg-main-shmem", *size, &MainSegReg);

	/*
	 * report the actual (possibly huge-page-rounded) length back to the
	 * caller
	 */
	*size = MainSegReg.size;
	return raw;
}

#else							/* !USE_PFSD */

/*
 * Unreachable without pfsd: polar_zc_main_segment_active() is always false, so
 * sysv_shmem.c never calls this. Present only so callers need no USE_PFSD guard.
 */
void *
polar_zc_main_segment_create(Size *size)
{
	elog(FATAL, "zero-copy main segment requested without pfsd support");
	return NULL;				/* keep compiler quiet */
}

#endif							/* USE_PFSD */
