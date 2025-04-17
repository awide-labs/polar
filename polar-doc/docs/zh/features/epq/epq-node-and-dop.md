---
author: 渊云
date: 2023/09/06
minute: 20
---

# ePQ 计算节点范围选择与并行度控制

<Badge type="tip" text="V11 / v1.1.20-" vertical="top" />

<ArticleInfo :frontmatter=$frontmatter></ArticleInfo>

[[toc]]

## 背景介绍

PolarDB-PG 的 ePQ 弹性跨机并行查询特性提供了精细的粒度控制方法，可以合理使用集群内的计算资源。在最大程度利用闲置计算资源进行并行查询，提升资源利用率的同时，避免了对其它业务负载产生影响：

1. ePQ 可以动态调整集群中参与并行查询的计算节点范围，避免使用负载较高的计算节点
2. ePQ 支持为每条查询动态调整在计算节点上的并行度，避免 ePQ 并行查询进程对计算资源的消耗影响到相同节点上的其它进程

## 计算节点范围选择

参数 `polar_px_nodes` 指定了参与 ePQ 的计算节点范围，默认值为空，表示所有只读节点都参与 ePQ 并行查询：

```sql:no-line-numbers
=> SHOW polar_px_nodes;
 polar_px_nodes
----------------

(1 row)
```

如果希望读写节点也参与 ePQ 并行，则可以设置如下参数：

```sql:no-line-numbers
SET polar_px_use_primary TO ON;
```

如果部分只读节点负载较高，则可以通过修改 `polar_px_nodes` 参数设置仅特定几个而非所有只读节点参与 ePQ 并行查询。参数 `polar_px_nodes` 的合法格式是一个以英文逗号分隔的节点名称列表。获取节点名称需要安装 `polar_monitor` 插件：

```sql:no-line-numbers
CREATE EXTENSION IF NOT EXISTS polar_monitor;
```

通过 `polar_monitor` 插件提供的集群拓扑视图，可以查询到集群中所有计算节点的名称：

```sql:no-line-numbers
=> SELECT name,slot_name,type FROM polar_cluster_info;
 name  | slot_name |  type
-------+-----------+---------
 node0 |           | Primary
 node1 | standby1  | Standby
 node2 | replica1  | Replica
 node3 | replica2  | Replica
(4 rows)
```

其中：

- `Primary` 表示读写节点
- `Replica` 表示只读节点
- `Standby` 表示备库节点

通用的最佳实践是使用负载较低的只读节点参与 ePQ 并行查询：

```sql:no-line-numbers
=> SET polar_px_nodes = 'node2,node3';
=> SHOW polar_px_nodes;
 polar_px_nodes
----------------
 node2,node3
(1 row)
```

## 并行度控制

参数 `polar_px_dop_per_node` 用于设置当前会话中的 ePQ 查询在每个计算节点上的执行单元（Segment）数量，每个执行单元会为其需要执行的每一个计划分片（Slice）启动一个进程。

该参数默认值为 `3`，通用最佳实践值为当前计算节点 CPU 核心数的一半。如果计算节点的 CPU 负载较高，可以酌情递减该参数，控制计算节点的 CPU 占用率至 80% 以下；如果查询性能不佳时，可以酌情递增该参数，也需要保持计算节点的 CPU 水位不高于 80%。否则可能会拖慢其它的后台进程。

## 并行度计算方法示例

创建一张表：

```sql:no-line-numbers
CREATE TABLE test(id INT);
```

假设集群内有两个只读节点，`polar_px_nodes` 为空，此时 ePQ 将使用集群内的所有只读节点参与并行查询；参数 `polar_px_dop_per_node` 的值为 `3`，表示每个计算节点上将会有三个执行单元。执行计划如下：

```sql:no-line-numbers
=> SHOW polar_px_nodes;
 polar_px_nodes
----------------

(1 row)

=> SHOW polar_px_dop_per_node;
 polar_px_dop_per_node
-----------------------
 3
(1 row)

=> EXPLAIN SELECT * FROM test;
                                  QUERY PLAN
-------------------------------------------------------------------------------
 PX Coordinator 6:1  (slice1; segments: 6)  (cost=0.00..431.00 rows=1 width=4)
   ->  Partial Seq Scan on test  (cost=0.00..431.00 rows=1 width=4)
 Optimizer: PolarDB PX Optimizer
(3 rows)
```

从执行计划中可以看出，两个只读节点上总计有六个执行单元（`segments: 6`）将会执行这个计划中唯一的计划分片 `slice1`。这意味着总计会有六个进程并行执行当前查询。

此时，调整 `polar_px_dop_per_node` 为 `4`，再次执行查询，两个只读节点上总计会有八个执行单元参与当前查询。由于执行计划中只有一个计划分片 `slice1`，这意味着总计会有八个进程并行执行当前查询：

```sql:no-line-numbers
=> SET polar_px_dop_per_node TO 4;
SET
=> EXPLAIN SELECT * FROM test;
                                  QUERY PLAN
-------------------------------------------------------------------------------
 PX Coordinator 8:1  (slice1; segments: 8)  (cost=0.00..431.00 rows=1 width=4)
   ->  Partial Seq Scan on test  (cost=0.00..431.00 rows=1 width=4)
 Optimizer: PolarDB PX Optimizer
(3 rows)
```

此时，如果设置 `polar_px_use_primary` 参数，让读写节点也参与查询，那么读写节点上也将会有四个执行单元参与 ePQ 并行执行，集群内总计 12 个进程参与并行执行：

```sql:no-line-numbers
=> SET polar_px_use_primary TO ON;
SET
=> EXPLAIN SELECT * FROM test;
                                   QUERY PLAN
---------------------------------------------------------------------------------
 PX Coordinator 12:1  (slice1; segments: 12)  (cost=0.00..431.00 rows=1 width=4)
   ->  Partial Seq Scan on test  (cost=0.00..431.00 rows=1 width=4)
 Optimizer: PolarDB PX Optimizer
(3 rows)
```
