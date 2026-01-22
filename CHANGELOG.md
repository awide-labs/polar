# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

### Fixed

- Fixed deadlock when WAL exceeds xlog queue capacity by releasing WALInsertLock
  when the queue is full, allowing walwriter to flush WAL so logindex saver can
  consume the queue (XCOM-94)
- Fixed `pg_ctl logrotate` functionality by restoring the missing check for the
  `logrotate` file in the data directory upon receiving SIGUSR1 (XCOM-87)

### Removed

- The following third-party extensions have been removed (XCOM-61):
  - hll
  - log_fdw
  - pase
  - pg_bigm
  - pg_jieba
  - pgvector
  - roaringbitmap

### Performance

- Reduce contention on the flush list on RW node by splitting it into multiple
  partitions (currently 64), with each partition having its own own lock,
  control structure and statistics (XCOM-38)
- Improve RW performance with logindex enabled by optimizing the XLOG queue
  space reservation process, eliminating a major bottleneck where packets were
  marked as free by writing into the queue while holding a global spinlock
  (XCOM-39)
- Reduce contention on XLogCtl->info_lck by publishing replay LSN updates with
  atomic writes and a seqlock (XCOM-35)
- Optimize asynchronous DDL processing by the startup process when
  `polar_enable_async_ddl_lock_replay` is enabled (XCOM-35)
- Avoid mini transaction overhead by the startup process when replayed WAL record
  updates single page (XCOM-35)
- Bump the maximum number of parallel bgwriters from 16 to 64 (XCOM-35)
- Increase the number of buffer partitions and the number of logindex hash locks (XCOM-35)
- Port WAL pipeline feature from PolarDB 11 to improve write throughput (XCOM-59)
- Increase the number of WAL insertion locks (NUM_XLOGINSERT_LOCKS) from 8 to 64
  to improve WAL insertion scalability (XCOM-59)
- Port CSN (Commit Sequence Number) feature from PolarDB 11 to improve MVCC
  scalability (XCOM-100)

[unreleased]: https://bitbucket.org/awydex/polardb/branches/compare/POLARDB_15_STABLE..v15.14.5.0
