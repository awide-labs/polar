/*-------------------------------------------------------------------------
 *
 * polar_zc.h
 *	  Zero-copy ("zc") memfd-backed main shared-memory segment.
 *
 * When zero-copy is enabled (polar_enable_zero_copy) on a real pfsd mount, the
 * whole main shared-memory segment is a single memfd (honoring huge pages)
 * instead of an anonymous mmap, mapped MAP_SHARED in the postmaster before fork
 * and registered with pfsd once. Device IO then streams straight out of any
 * shmem buffer (WAL, buffer pool, copy buffers, SLRU, checksum scratch) via
 * pfsd_pwrite_zc / pfsd_pread_zc rather than bouncing through pfsd's pool -- no
 * per-consumer region, every consumer just lives in the one segment.
 *
 * IDENTIFICATION
 *	  src/include/storage/polar_zc.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef POLAR_ZC_H
#define POLAR_ZC_H

/*
 * Master enable for zero-copy IO. PGC_POSTMASTER. Inert unless shared storage
 * is enabled and a real pfsd is mounted (see polar_zc_main_segment_active).
 */
extern PGDLLIMPORT bool polar_enable_zero_copy;

/*
 * Is the cluster in a state where zc can actually be used, i.e. shared storage
 * is enabled and a real pfsd is mounted? Stable for the life of the postmaster
 * from the pfsd mount onward. Returns false when built without USE_PFSD.
 */
extern bool polar_zc_storage_ready(void);

/*
 * Should the main shared-memory segment be backed by a registered memfd?
 * polar_enable_zero_copy AND polar_zc_storage_ready(). Used by sysv_shmem.c to
 * choose the segment backing.
 */
extern bool polar_zc_main_segment_active(void);

/*
 * Create (or, on internal restart, recreate) the main shared-memory segment as
 * a single memfd of at least *size bytes, honoring huge pages, register it with
 * pfsd, and return the mmap base. *size is updated to the actual (huge-page-
 * rounded) length. The registration handle is postmaster-private so it survives
 * reset_shared: a stale prior registration is dropped before the fresh one.
 *
 * memfd/ftruncate/mmap failure is FATAL. Registration failure is non-fatal --
 * the segment is still usable as plain memory, IO falling back to copying. Call
 * in the postmaster after the pfsd mount, before any backend forks.
 */
extern void *polar_zc_main_segment_create(Size *size);

#endif							/* POLAR_ZC_H */
