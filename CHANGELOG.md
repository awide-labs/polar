# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

### Added

- Wrapped the `pgbulkload` extension call with the native `COPY FROM`
  command (XCOM-195):
  - Added the following new `COPY FROM` options:
    - **DIRECT**: Enables the `pgbulkload` extension for data appends. This
	option is restricted to the **CSV** format and is an analogue of
	`WRITTER=DIRECT` with `MULTI_PROCESS=YES` `pgbulkload` options.
	- **WAL_LOGGED**: Enables routing data through shared buffers with WAL
    logging for standard PostgreSQL crash recovery, can be used only with
	**DIRECT** option. This option maps to `WRITTER=BUFFERED` `pgbulkload`
	option.
    - **BULKLOAD**: Allows passing a string of comma-separated "key=value"
	pairs to `pgbulkload`. This requires the **DIRECT** option and enables
	access to specific extension features not available in native `COPY`.
    Multi-process mode can be disabled using `BULKLOAD="MULTI_PROCESS=NO"`.
  - Added support for the **WHERE** clause and progress update.
  - Compatible **CSV** options (`DELIMITER`, `NULL`, `QUOTE`, `ESCAPE`,
    `FORCE_NOT_NULL`, `ENCODING`, `HEADER` as `SKIP=1`) are forwarded to
	`pgbulkload` extension; `FREEZE`, `FORCE_NULL`, `FORCE_QUOTE`, `BINARY`,
	`CONVERT_SELECTIVELY`, and `HEADER MATCH` are rejected with clear errors.
  - **`COPY FROM stdin` with `DIRECT`** is supported for normal client
    connections (psql, libpq): input uses the standard copy-in protocol and
    pg_bulkload `INPUT=stdin`. Not supported when the server reads from its
    own standard input (no client).
- The `pgbulkload` extension now utilizes PostgreSQL’s lock group mechanism to
  prevent table lock conflicts between Reader and Writer processes in
  multi-process mode. This eliminates the gap between the Reader releasing the
  lock and the Writer acquiring it, preventing DDL statements from modifying
  the relation during that window (XCOM-195).
- **Postmaster** now performs `pg_bulkload` recovery on startup. After acquiring
  the data directory lock, it searches for Load Status Files (`*.loadstatus`). 
  If any are found and the `pg_bulkload` extension is loaded, the corresponding
  recovery function is invoked. If the extension is missing or an older version
  is used that lacks the recovery function, the postmaster process will
  terminate with an error message in the log (XCOM-195).
- Expose 69 PolarDB-specific GUCs in `pg_settings` and
  `postgres --describe-config` so that Patroni can enumerate,
  validate, and track `pending_restart` for them (XCOM-195)
- Add pg_bulkload v3.1.23, a high-speed bulk data loading utility,
  as an in-tree extension with full PolarDB shared storage support
  (XCOM-195)

### Changed

- Added exponential backoff for the `Failed to get the instance memory
  usage` warning that previously flooded the logs when memory statistics
  were temporarily unavailable (XCOM-195)

### Removed

- The following third-party extensions have been removed (XCOM-195):
  - pldebugger
  - prefix
- The pg_partman extension has been temporarily disabled due to broken
  regression tests (XCOM-195)
- The pg_repack extension has been temporarily disabled due to broken
  regression tests (XCOM-195)
- The pgtap extension has been temporarily disabled due to broken regression
  tests (XCOM-195)
- The following third-party extensions have been removed (XCOM-195):
  - hll
  - ip4r
  - log_fdw
  - pase
  - pg_bigm
  - pg_jieba
  - roaringbitmap

### Fixed

- Fixed a hang on fast or immediate shutdown of a standby or replica
  that was stopped right after being promoted: the logindex background
  worker could exit before the online promote finished, leaving the
  shutdown checkpoint unable to advance the consistent LSN to the
  checkpoint redo and looping on "Checkpoint blocked" until pg_ctl
  timed out (XCOM-195)
- Fixed desync between MyProc->xmin and TransactionXmin in SnapshotResetXmin
  when polar_csn_enable is on. This could lead to incorrect CSNLOG requests.
  (XCOM-195)
- Fix (enable) parsing of CSN snapshots from their text representation.
  (XCOM-195)
- pg_visible_in_snapshot() may now raise "snapshot too old for CSN visibility
  check" when asked about a transaction in an old CSN snapshot whose
  commit-order (CSN) data has been truncated. Previously it returned
  unreliable transaction status from CLOG. (XCOM-195)
- Fix inverted/erroneous logic in pg_visible_in_snapshot() for CSN snapshots.
  It was producing inverted results for transactions in the [xmin, xmax] range.
  (XCOM-195)
- Fix 32-bit truncation of 64-bit CSN value in pg_current_snapshot()
  function. (XCOM-195)
- Stopped spurious `could not open directory "polar_cache_trash"` and
  `Failed to empty trash dir for local cache` WARNINGs that flooded the
  log during initdb and startup when the local cache trash directory did
  not yet exist (XCOM-195)
- Fixed a race condition in consistent LSN calculation across flush-list
  partitions that caused the primary to crash with "Current consistent
  lsn X is great than next consistent lsn Y" PANIC under concurrent
  buffer writes (XCOM-195)
- Fixed primary hanging during shutdown when replicas were already stopped,
  repeatedly logging "Checkpoint blocked" warnings until killed (XCOM-195)
- Fixed `pg_bulkload` crash recovery so that disk space allocated by an
  interrupted bulk load is reclaimed: after both automatic (postmaster)
  and offline (`pg_bulkload -r`) recovery, the target relation returns to
  its pre-load size. Previously the partially loaded blocks remained on
  disk — the rows were invisible, but the space was never freed (XCOM-195)
- Prevented replica startup with disabled logindex (`polar_logindex_mem_size=0`
  or `polar_xlog_queue_buffers=0`) which would lead to crash during WAL replay
  (XCOM-195)
- Introduced support for cgroup v2 to fetch memory statistics, removing the
  `Failed to get instance memory` error that previously spammed the logs (XCOM-195)
- VACUUM FULL/CLUSTER/REINDEX/TRUNCATE of mapped system catalogs recreates
  relfilenodes and commits the switch by updating pg_filenode.map. The relmap
  update is WAL-logged and flushed before the updated map file is guaranteed to
  be visible on shared storage. A fast replica can therefore replay the relmap
  WAL record and start resolving mapped OIDs while still reading an old
  pg_filenode.map from shared storage, producing stale filenodes that may have
  already been removed and leading to intermittent catalog/relcache failures
  (XCOM-195)
- Fixed replica promotion failure (FATAL: "WAL segment has already been removed")
  when primary crashed while creating a new WAL segment file (XCOM-195)
- Fixed non-working log rotation via pg_ctl logrotate (XCOM-195)

### Performance

- Speed up statements that must wait for WAL to reach disk (synchronous
  commits, DDL, and similar durable writes) when `polar_wal_pipeline_mode`
  is 3 or 5, cutting idle-system latency from ~100ms to single-digit
  milliseconds (XCOM-195)
- Avoid unconditional full scan of shared_buffers in the buffer-pool
  invalidation path when RSC is enabled.  This speeds up every
  command that drops or truncates relation storage, including
  DROP TABLE/INDEX/MATERIALIZED VIEW, TRUNCATE, VACUUM/autovacuum
  tail truncation, CLUSTER, VACUUM FULL, REFRESH MATERIALIZED VIEW,
  REINDEX, and rewriting forms of ALTER TABLE, as well as replay of
  smgr truncate records on replicas (XCOM-195)
- Replace spinlock-protected reads of `RedoRecPtr` and
  `curr_primary_consistent_lsn` with lock-free `pg_atomic_uint64` ops,
  eliminating two hot spinlocks (`info_lck`, `WalRcv->mutex`) from the
  per-buffer-read path on replicas (XCOM-195)
- Port CSN (Commit Sequence Number) feature from PolarDB 11 to improve MVCC
  scalability (XCOM-195)
- Increase the number of WAL insertion locks (NUM_XLOGINSERT_LOCKS) from 8 to 64
  to improve WAL insertion scalability (XCOM-195)
- Port WAL pipeline feature from PolarDB 11 to improve write throughput
  (XCOM-195)
- Increase the number of buffer partitions and the number of logindex hash locks
  (XCOM-195)
- Bump the maximum number of parallel bgwriters from 16 to 64 (XCOM-195)
- Avoid mini transaction overhead by the startup process when replayed WAL record
  updates a single page (XCOM-195)
- Optimize asynchronous DDL processing by the startup process when
  `polar_enable_async_ddl_lock_replay` is enabled (XCOM-195)
- Reduce replica lag by avoiding a contended spinlock and publishing replay LSN
  updates with optimized locking primitives (XCOM-195)
- Improve RW performance with logindex enabled by optimizing the XLOG queue
  space reservation process, eliminating a major bottleneck where packets were
  marked as free by writing into the queue while holding a global spinlock
  (XCOM-195)
- Reduce contention on the flush list on RW node by splitting it into multiple
  partitions (currently 64), with each partition having its own lock, control
  structure and statistics (XCOM-195)

[unreleased]: https://github.com/awide-labs/polar/compare/248cd221718..POLARDB_17_STABLE
