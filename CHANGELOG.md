# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

### Performance

- Reduce contention on the flush list on RW node by splitting it into multiple
  partitions (currently 64), with each partition having its own own lock,
  control structure and statistics (XCOM-38)

[unreleased]: https://bitbucket.org/awydex/polardb/branches/compare/POLARDB_15_STABLE..v15.14.5.0
