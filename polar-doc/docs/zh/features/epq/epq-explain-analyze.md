---
author: 渊云、秦疏
date: 2023/09/06
minute: 30
---

# ePQ 执行计划查看与分析

<Badge type="tip" text="V11 / v1.1.20-" vertical="top" />

<ArticleInfo :frontmatter=$frontmatter></ArticleInfo>

[[toc]]

## 背景

PostgreSQL 提供了 `EXPLAIN` 命令用于 SQL 语句的性能分析。它能够输出 SQL 对应的查询计划，以及在执行过程中的具体耗时、资源消耗等信息，可用于排查 SQL 的性能瓶颈。

`EXPLAIN` 命令原先只适用于单机执行的 SQL 性能分析。PolarDB-PG 的 ePQ 弹性跨机并行查询扩展了 `EXPLAIN` 的功能，使其可以打印 ePQ 的跨机并行执行计划，还能够统计 ePQ 执行计划在各个算子上的执行时间、数据扫描量、内存使用量等信息，并以统一的视角返回给客户端。

## 功能介绍

### 执行计划查看

ePQ 的执行计划是分片的。每个计划分片（Slice）由计算节点上的虚拟执行单元（Segment）启动的一组进程（Gang）负责执行，完成 SQL 的一部分计算。ePQ 在执行计划中引入了 Motion 算子，用于在执行不同计划分片的进程组之间进行数据传递。因此，Motion 算子就是计划分片的边界。

ePQ 中总共引入了三种 Motion 算子：

- `PX Coordinator`：源端数据发送到同一个目标端（汇聚）
- `PX Broadcast`：源端数据发送到每一个目标端（广播）
- `PX Hash`：源端数据经过哈希计算后发送到某一个目标端（重分布）

以一个简单查询作为例子：

```sql:no-line-numbers
=> CREATE TABLE t (id INT);
=> SET polar_enable_px TO ON;
=> EXPLAIN (COSTS OFF) SELECT * FROM t LIMIT 1;
                   QUERY PLAN
-------------------------------------------------
 Limit
   ->  PX Coordinator 6:1  (slice1; segments: 6)
         ->  Partial Seq Scan on t
 Optimizer: PolarDB PX Optimizer
(4 rows)
```

以上执行计划以 Motion 算子为界，被分为了两个分片：一个是接收最终结果的分片 `slice0`，一个是扫描数据的分片`slice1`。对于 `slice1` 这个计划分片，ePQ 将使用六个执行单元（`segments: 6`）分别启动一个进程来执行，这六个进程各自负责扫描表的一部分数据（`Partial Seq Scan`），通过 Motion 算子将六个进程的数据汇聚到一个目标端（`PX Coordinator 6:1`），传递给 `Limit` 算子。

如果查询逐渐复杂，则执行计划中的计划分片和 Motion 算子会越来越多：

```sql:no-line-numbers
=> CREATE TABLE t1 (a INT, b INT, c INT);
=> SET polar_enable_px TO ON;
=> EXPLAIN (COSTS OFF) SELECT SUM(b) FROM t1 GROUP BY a LIMIT 1;
                         QUERY PLAN
------------------------------------------------------------
 Limit
   ->  PX Coordinator 6:1  (slice1; segments: 6)
         ->  GroupAggregate
               Group Key: a
               ->  Sort
                     Sort Key: a
                     ->  PX Hash 6:6  (slice2; segments: 6)
                           Hash Key: a
                           ->  Partial Seq Scan on t1
 Optimizer: PolarDB PX Optimizer
(10 rows)
```

以上执行计划中总共有三个计划分片。将会有六个进程（`segments: 6`）负责执行 `slice2` 分片，分别扫描表的一部分数据，然后通过 Motion 算子（`PX Hash 6:6`）将数据重分布到另外六个（`segments: 6`）负责执行 `slice1` 分片的进程上，各自完成排序（`Sort`）和聚合（`GroupAggregate`），最终通过 Motion 算子（`PX Coordinator 6:1`）将数据汇聚到结果分片 `slice0`。
