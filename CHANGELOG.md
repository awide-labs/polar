# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

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

[unreleased]: https://bitbucket.org/awydex/polardb/branches/compare/POLARDB_15_STABLE..v15.14.5.0
