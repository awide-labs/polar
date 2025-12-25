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

### Performance

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
