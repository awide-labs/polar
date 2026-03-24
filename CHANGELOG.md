# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

### Added

- Expose 69 PolarDB-specific GUCs in `pg_settings` and
  `postgres --describe-config` so that Patroni can enumerate,
  validate, and track `pending_restart` for them (XCOM-195)
- Add pg_bulkload v3.1.23, a high-speed bulk data loading utility,
  as an in-tree extension with full PolarDB shared storage support
  (XCOM-195)

### Removed

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
