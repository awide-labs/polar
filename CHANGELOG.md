# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

### Added

- Add DataMax standalone mode, a dedicated log-storage node that streams
  WAL from an upstream primary and durably retains and archives it
  without applying it, so it holds no primary data (XCOM-195)
- Add `'w'` back-pressure marker in ReadyForQuery and switch the
  proxy LSN from the global WAL tip to per-session
  `XactLastRecEnd`/`XactLastCommitEnd`, so the proxy never asks a
  replica to wait for WAL the primary has not yet flushed
  (XCOM-195)
- Add `polar_query_delay_us` and `polar_replay_min_lag_size` GUCs for
  end-to-end testing of the proxy session-consistency wait.
  (XCOM-195)
- Add `polar_stat_node_metrics()` view in `polar_monitor` (1.3 → 1.4)
  exposing DB-PSI signals, LogIndex applier stats, and cgroup-v2 CPU
  metrics for proxy routing (XCOM-195)
- Report `transaction_isolation` and `default_transaction_isolation`
  to clients via GUC_REPORT so connection proxies can read the
  current isolation level via the libpq ParameterStatus stream
  (XCOM-195)
- Add LSN-based session-consistency wait for connection proxies:
  replicas block at snapshot acquisition until WAL replay reaches a
  target LSN, with configurable timeout and best-effort/strict modes
  (XCOM-195)
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
  validate, and track `pending_restart` for them (XCOM-124)
- Add pg_bulkload v3.1.23, a high-speed bulk data loading utility,
  as an in-tree extension with full PolarDB shared storage support
  (XCOM-99)

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
- The following third-party extensions have been removed (XCOM-61):
  - hll
  - ip4r
  - log_fdw
  - pase
  - pg_bigm
  - pg_jieba
  - roaringbitmap

### Fixed

- Fixed `polar_ignore_coredump_functions` not matching functions that the
  compiler renamed (for example, static functions in LTO-enabled release
  builds), so the coredump was not ignored (XCOM-198)
- Fixed a race where a shared-storage replica, or a standby with parallel
  replay enabled (`polar_enable_parallel_replay_standby_mode`, on by
  default), could sporadically return stale data: a page replayed while its
  WAL record was still being parsed was left stale with no retry (XCOM-195)
- Fixed a crash in `polar_tools logindex-page` when the required
  `table_path` argument was omitted. The tool now prints usage help
  and exits with an error code. (XCOM-195)
- Fixed a crash in `polar_tools control-data-change` when the control
  data file was empty or unreadable. The tool now prints an error
  message and exits with an error code. (XCOM-195)
- Fixed pg_bulkload client errors reporting a bare "ERROR:" with no message text (XCOM-186)
- Fixed ignored fsync errors during WAL file sync on shared storage —
  fsync failures now trigger a PANIC. Added proper WAL fsync statistics
  collection (wait events, I/O timing, sync counter). (XCOM-195)
- Fixed logical decoding on a cascading standby failing with an error
  ("invalid record length at ..." / "requested WAL segment has already
  been removed") during the upstream standby's promotion. This was a
  race in WAL-page timeline selection: in the promotion window where
  `RecoveryInProgress()` still returned true but the old timeline's WAL
  segments had already been removed, logical decoding could pick the old
  timeline and fail to read. Now the WAL insertion timeline is used as
  soon as it is set. (XCOM-195)
- Fixed another case where a standby or replica could hang on shutdown
  when it was stopped right after being promoted to primary, failing to
  shut down and requiring a forced stop. It was timing-dependent and
  seen mainly on busy or slow systems. (XCOM-195)
- Fixed incorrect query results on a hot standby when CSN-based
  snapshots were enabled. Under some conditions a query on the standby
  could treat rows as visible when they should not yet have been,
  returning data that its snapshot should not have seen. (XCOM-195)
- Fixed a snapshot-isolation violation with CSN xid snapshots (both
  `polar_csn_enable` and `polar_csn_xid_snapshot` on) under high
  concurrency. When more transactions were running than the snapshot's
  xid list could hold, the list overflowed and dropped the
  higher-numbered running xids, but visibility checks kept trusting the
  truncated list instead of the CSN log. A transaction that was in
  flight when the snapshot was taken could then have its rows wrongly
  become visible once it committed, with no crash or error. (XCOM-195)
- Fix `CREATEENUM` proxy events mis-attributed to the `combocid`
  counter (XCOM-195)
- Fixed a race in pg_bulkload's asynchronous input reader (used in
  MULTI_PROCESS mode) that could make a load occasionally read zero rows
  from the input file, finishing with nothing loaded and no parse errors
  reported (XCOM-195)
- Fixed LISTEN sessions silently stalling on the async-notify queue when CSN
  snapshots are enabled. A transaction that aborted after enqueuing a
  notification was treated as still in progress, so any session reading the
  NOTIFY queue would pin its read position at that notification and never
  deliver notifications enqueued afterward on that channel. (XCOM-195)
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
  repeatedly logging "Checkpoint blocked" warnings until killed (XCOM-153)
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
  when primary crashed while creating a new WAL segment file (XCOM-114)
- Fixed non-working log rotation via pg_ctl logrotate (XCOM-87)

### Performance

- Release builds are now linked with link-time optimization (LTO) enabled,
  producing smaller and faster binaries (XCOM-198)
- Fixed occasional slow commits on lightly loaded instances,
  because of lost wakeups of pipeline workers (XCOM-162)
- Eliminate the per-I/O memory copy on shared storage: PolarDB now reads and
  writes directly from its shared-memory buffers instead of copying every page
  through the pfsdaemon's shared pool. Enabled by default via
  `polar_enable_zero_copy` (XCOM-195)
- Speed up statements that must wait for WAL to reach disk (synchronous
  commits, DDL, and similar durable writes) when `polar_wal_pipeline_mode`
  is 3 or 5, cutting idle-system latency from ~100ms to single-digit
  milliseconds (XCOM-160)
- Avoid unconditional full scan of shared_buffers in the buffer-pool
  invalidation path when RSC is enabled.  This speeds up every
  command that drops or truncates relation storage, including
  DROP TABLE/INDEX/MATERIALIZED VIEW, TRUNCATE, VACUUM/autovacuum
  tail truncation, CLUSTER, VACUUM FULL, REFRESH MATERIALIZED VIEW,
  REINDEX, and rewriting forms of ALTER TABLE, as well as replay of
  smgr truncate records on replicas (XCOM-159)
- Replace spinlock-protected reads of `RedoRecPtr` and
  `curr_primary_consistent_lsn` with lock-free `pg_atomic_uint64` ops,
  eliminating two hot spinlocks (`info_lck`, `WalRcv->mutex`) from the
  per-buffer-read path on replicas (XCOM-128)
- Port CSN (Commit Sequence Number) feature from PolarDB 11 to improve MVCC
  scalability (XCOM-100)
- Increase the number of WAL insertion locks (NUM_XLOGINSERT_LOCKS) from 8 to 64
  to improve WAL insertion scalability (XCOM-59)
- Port WAL pipeline feature from PolarDB 11 to improve write throughput
  (XCOM-59)
- Increase the number of buffer partitions and the number of logindex hash locks
  (XCOM-35)
- Bump the maximum number of parallel bgwriters from 16 to 64 (XCOM-35)
- Avoid mini transaction overhead by the startup process when replayed WAL record
  updates a single page (XCOM-35)
- Optimize asynchronous DDL processing by the startup process when
  `polar_enable_async_ddl_lock_replay` is enabled (XCOM-35)
- Reduce replica lag by avoiding a contended spinlock and publishing replay LSN
  updates with optimized locking primitives (XCOM-35)
- Improve RW performance with logindex enabled by optimizing the XLOG queue
  space reservation process, eliminating a major bottleneck where packets were
  marked as free by writing into the queue while holding a global spinlock
  (XCOM-39)
- Reduce contention on the flush list on RW node by splitting it into multiple
  partitions (currently 64), with each partition having its own lock, control
  structure and statistics (XCOM-38)

[unreleased]: https://github.com/awide-labs/polar/compare/248cd221718..POLARDB_17_STABLE
