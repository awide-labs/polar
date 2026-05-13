# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

### Added

- Add pg_bulkload v3.1.23, a high-speed bulk data loading utility,
  as an in-tree extension with full PolarDB shared storage support
  (XCOM-99)
- Expose 69 PolarDB-specific GUCs in `pg_settings` and
  `postgres --describe-config` so that Patroni can enumerate,
  validate, and track `pending_restart` for them (XCOM-124)

### Fixed

- Fixed primary hanging during shutdown when replicas were already stopped,
  repeatedly logging "Checkpoint blocked" warnings until killed (XCOM-153)
- Fixed deadlock when WAL exceeds xlog queue capacity by releasing WALInsertLock
  when the queue is full, allowing walwriter to flush WAL so logindex saver can
  consume the queue (XCOM-141)
- Fixed `pg_ctl logrotate` functionality by restoring the missing check for the
  `logrotate` file in the data directory upon receiving SIGUSR1 (XCOM-87)
- Fixed replica promotion failure (FATAL: "WAL segment has already been removed")
  when primary crashed while creating a new WAL segment file (XCOM-114)
- Fixed VFS crash (SIGSEGV in `strlen`) when replica attempts to unlink
  shared CSNLOG files after disabling CSN; also prevent replica from
  modifying shared storage CSNLOG files (XCOM-125)

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
- Replace spinlock-protected reads of `RedoRecPtr` and
  `curr_primary_consistent_lsn` with lock-free `pg_atomic_uint64` ops,
  eliminating two hot spinlocks (`info_lck`, `WalRcv->mutex`) from the
  per-buffer-read path on replicas (XCOM-128)
- Speed up crash recovery replay by reading multiple WAL pages per I/O
  (default 128, configurable via `polar_recovery_bulk_read_size`)
  (XCOM-138)
- Eliminate walsender spinlock contention on the primary, letting
  walsenders keep up with the write load generating WAL (XCOM-137)
- Reduce `insertpos_lck` cache-line contention by reserving xlog queue
  space optimistically outside the spinlock (XCOM-141)

[unreleased]: https://github.com/awide-labs/polar/compare/6fcfdc2993a..POLARDB_15_STABLE
