# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

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

- Fixed non-working log rotation via pg_ctl logrotate (XCOM-195)

### Performance

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
