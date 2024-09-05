# 版本规划

PolarDB PostgreSQL 将持续发布对用户有价值的功能。当前我们计划了 5 个阶段：

## PolarDB PostgreSQL 1.0 版本

1.0 版本基于 Shared-Storage 的存储计算分离架构，发布必备的最小功能集合，例如：PolarVFS、刷脏和 Buffer 管理、LogIndex、SyncDDL 等。

- PolarVFS：数据库内核中抽象出了一层 VFS 层，使得内核可以对接任意的存储，包括 bufferIO 和 directIO。
- 刷脏和 Buffer 管理：由原来的 N 份计算+N 份存储，转变成了 N 份计算+1 份存储，主节点在刷脏时需要做协调，避免只读节点读取到超前的“未来页面”。
- LogIndex: 由于只读节点不能刷脏，所需要的特定版本页面需要从 Shared-Storage 上读取一个老的版本页面，并通过在内存中回放来得到正确的版本。LogIndex 结构记录了每个 Page 所对应的 WAL 日志 Meta 信息，在需要回放时直接查找 LogIndex，从而加速回放过程。
- DDL 同步: 在存储计算分离后，主节点在执行 DDL 时需要兼顾只读节点对 Relation 等对象的引用，相关的 DDL 动作需要同步地在只读节点上上锁。
- 数据库监控：支持主机和数据库的监控，同时为 HA 切换提供了判断依据。

## PolarDB PostgreSQL 2.0 版本

除了在存储计算分离架构上改动之外，2.0 版本将在优化器上进行深度的优化，例如：

- UniqueKey：和 Plan 节点的有序性类似，UniqueKey 维护的是 Plan 节点数据的唯一性。数据的唯一性可以减少不必要的 DISTINCT、Group By，增加 Join 结果有序性判断等。

## PolarDB PostgreSQL 3.0 版本

3.0 版本主要在存储计算分离后在可用性上进行重大优化，例如：

- 并行回放：存储计算分离之后，PolarDB 通过 LogIndex 实现了 Lazy 的回放。实现原理为：仅标记一个 Page 应该回放哪些 WAL 日志，在读进程时再进行真正的回放过程。此时对读的性能是有影响的。在 3.0 版本中，我们在 Lazy 回放基础上实现了并行回放，从而加速 Page 的回放过程。
- OnlinePromote：在主节点崩溃后，切换到任意只读节点。该只读节点无需重启，继续并行回放完所有的 WAL 之后，Promote 成为新的主节点，从而进一步降低了不可用时间。

## PolarDB PostgreSQL 4.0 版本

为了满足日益增多的 HTAP 混合负载需求，4.0 版本将发布基于 Shared-Storage 架构的分布式并行执行引擎，充分发挥多个只读节点的 CPU/MEM/IO 资源。

经测试，在计算集群逐步扩展到 256 核时，性能仍然能够线性提升。

## PolarDB PostgreSQL 5.0 版本

基于存储计算分离的一写多读架构中，读能力能够弹性的扩展，但是写入能力仍然只能在单个节点上执行。

5.0 版本将发布 Shared-Nothing On Share-Everything 架构，结合 PolarDB 的分布式版本和 PolarDB 集中式版本的架构优势，使得多个节点都能够写入。
