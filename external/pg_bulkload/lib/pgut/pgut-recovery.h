/*-------------------------------------------------------------------------
 *
 * pgut-recovery.h
 *
 * Copyright (c) 2007-2026, NTT, Inc.
 *
 *-------------------------------------------------------------------------
 */

#ifndef PGUT_RECOVERY_H
#define PGUT_RECOVERY_H

/**
 * @brief Run pg_bulkload recovery over the given data directory.
 *
 * Scans for .loadstatus files(LSF) under data_dir/pg_bulkload/, and if the cluster
 * did not shut down cleanly, truncates the corresponding relation ranges off
 * the affected segments (unlinking any later segments the loader created)
 * and removes the load status files.
 *
 * Callable from both the pg_bulkload frontend (recovery process) and from
 * postmaster/backend when the pg_bulkload extension is loaded (e.g. at
 * startup).
 *
 * Errors are reported via ereport/elog.
 *
 * When the function is called from pg_bulkload frontend,
 * the folloing conditions must be satisfied:
 *      - postmaster/postgres process is not running.
 *      - other recovery process is not running.
 * So when this function is called, LoaderCreateLockFile() must have been called previously and
 * a lock file has already created. After that LoaderUnlinkLockFile() must be called
 * for lock file deletion process.
 *
 * In postmaster the function is called after CreateDataDirLockFile().
 *
 * @param data_dir Path to the database cluster data directory (e.g. DataDir).
 */
extern void StartLoaderRecovery(const char *data_dir);

#endif   /* PGUT_RECOVERY_H */
