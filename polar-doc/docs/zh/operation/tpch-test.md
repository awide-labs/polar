---
author: 棠羽
date: 2023/04/12
minute: 20
---

# TPC-H 测试

<ArticleInfo :frontmatter=$frontmatter></ArticleInfo>

本文将引导您对 PolarDB for PostgreSQL 进行 TPC-H 测试。

[[toc]]

## 背景

[TPC-H](https://www.tpc.org/tpch/default5.asp) 是专门测试数据库分析型场景性能的数据集。

## 测试准备

### 部署 PolarDB-PG

使用 Docker 快速拉起一个单机的 PolarDB for PostgreSQL 集群：

::: code-tabs
@tab DockerHub

```shell:no-line-numbers
docker pull polardb/polardb_pg_local_instance:11
docker run -it \
    --cap-add=SYS_PTRACE \
    --privileged=true \
    --name polardb_pg_htap \
    --shm-size=512m \
    polardb/polardb_pg_local_instance:11 \
    bash
```

@tab 阿里云 ACR

```shell:no-line-numbers
docker pull registry.cn-hangzhou.aliyuncs.com/polardb_pg/polardb_pg_local_instance:11
docker run -it \
    --cap-add=SYS_PTRACE \
    --privileged=true \
    --name polardb_pg_htap \
    --shm-size=512m \
    registry.cn-hangzhou.aliyuncs.com/polardb_pg/polardb_pg_local_instance:11 \
    bash
```

:::

或者参考 [进阶部署](../deploying/deploy.md) 部署一个基于共享存储的 PolarDB for PostgreSQL 集群。

### 生成 TPC-H 测试数据集

通过 [tpch-dbgen](https://github.com/ApsaraDB/tpch-dbgen) 工具来生成测试数据。

```bash:no-line-numbers
$ git clone https://github.com/ApsaraDB/tpch-dbgen.git
$ cd tpch-dbgen
$ ./build.sh --help

  1) Use default configuration to build
  ./build.sh
  2) Use limited configuration to build
  ./build.sh --user=postgres --db=postgres --host=localhost --port=5432 --scale=1
  3) Run the test case
  ./build.sh --run
  4) Run the target test case
  ./build.sh --run=3. run the 3rd case.
  5) Run the target test case with option
  ./build.sh --run --option="set polar_enable_px = on;"
  6) Clean the test data. This step will drop the database or tables, remove csv
  and tbl files
  ./build.sh --clean
  7) Quick build TPC-H with 100MB scale of data
  ./build.sh --scale=0.1
```

通过设置不同的参数，可以定制化地创建不同规模的 TPC-H 数据集。`build.sh` 脚本中各个参数的含义如下：

- `--user`：数据库用户名
- `--db`：数据库名
- `--host`：数据库主机地址
- `--port`：数据库服务端口
- `--run`：执行所有 TPC-H 查询，或执行某条特定的 TPC-H 查询
- `--option`：额外指定 GUC 参数
- `--scale`：生成 TPC-H 数据集的规模，单位为 GB

该脚本没有提供输入数据库密码的参数，需要通过设置 `PGPASSWORD` 为数据库用户的数据库密码来完成认证：

```shell:no-line-numbers
export PGPASSWORD=<your password>
```

生成并导入 100MB 规模的 TPC-H 数据：

```shell:no-line-numbers
./build.sh --scale=0.1
```

生成并导入 1GB 规模的 TPC-H 数据：

```shell:no-line-numbers
./build.sh
```

## 执行 PostgreSQL 单机并行执行

以 TPC-H 的 Q18 为例，执行 PostgreSQL 的单机并行查询，并观测查询速度。

在 `tpch-dbgen/` 目录下通过 `psql` 连接到数据库：

```shell:no-line-numbers
cd tpch-dbgen
psql
```

```sql:no-line-numbers
-- 打开计时
\timing on

-- 设置单机并行度
SET max_parallel_workers_per_gather = 2;

-- 查看 Q18 的执行计划
\i finals/18.explain.sql
                                                                         QUERY PLAN
------------------------------------------------------------------------------------------------------------------------------------------------------------
 Sort  (cost=3450834.75..3450835.42 rows=268 width=81)
   Sort Key: orders.o_totalprice DESC, orders.o_orderdate
   ->  GroupAggregate  (cost=3450817.91..3450823.94 rows=268 width=81)
         Group Key: customer.c_custkey, orders.o_orderkey
         ->  Sort  (cost=3450817.91..3450818.58 rows=268 width=67)
               Sort Key: customer.c_custkey, orders.o_orderkey
               ->  Hash Join  (cost=1501454.20..3450807.10 rows=268 width=67)
                     Hash Cond: (lineitem.l_orderkey = orders.o_orderkey)
                     ->  Seq Scan on lineitem  (cost=0.00..1724402.52 rows=59986052 width=22)
                     ->  Hash  (cost=1501453.37..1501453.37 rows=67 width=53)
                           ->  Nested Loop  (cost=1500465.85..1501453.37 rows=67 width=53)
                                 ->  Nested Loop  (cost=1500465.43..1501084.65 rows=67 width=34)
                                       ->  Finalize GroupAggregate  (cost=1500464.99..1500517.66 rows=67 width=4)
                                             Group Key: lineitem_1.l_orderkey
                                             Filter: (sum(lineitem_1.l_quantity) > '314'::numeric)
                                             ->  Gather Merge  (cost=1500464.99..1500511.66 rows=400 width=36)
                                                   Workers Planned: 2
                                                   ->  Sort  (cost=1499464.97..1499465.47 rows=200 width=36)
                                                         Sort Key: lineitem_1.l_orderkey
                                                         ->  Partial HashAggregate  (cost=1499454.82..1499457.32 rows=200 width=36)
                                                               Group Key: lineitem_1.l_orderkey
                                                               ->  Parallel Seq Scan on lineitem lineitem_1  (cost=0.00..1374483.88 rows=24994188 width=22)
                                       ->  Index Scan using orders_pkey on orders  (cost=0.43..8.45 rows=1 width=30)
                                             Index Cond: (o_orderkey = lineitem_1.l_orderkey)
                                 ->  Index Scan using customer_pkey on customer  (cost=0.43..5.50 rows=1 width=23)
                                       Index Cond: (c_custkey = orders.o_custkey)
(26 rows)

Time: 3.965 ms

-- 执行 Q18
\i finals/18.sql
       c_name       | c_custkey | o_orderkey | o_orderdate | o_totalprice |  sum
--------------------+-----------+------------+-------------+--------------+--------
 Customer#001287812 |   1287812 |   42290181 | 1997-11-26  |    558289.17 | 318.00
 Customer#001172513 |   1172513 |   36667107 | 1997-06-06  |    550142.18 | 322.00
 ...
 Customer#001288183 |   1288183 |   48943904 | 1996-07-22  |    398081.59 | 325.00
 Customer#000114613 |    114613 |   59930883 | 1997-05-17  |    394335.49 | 319.00
(84 rows)

Time: 80150.449 ms (01:20.150)
```

## 执行 ePQ 单机并行执行

PolarDB for PostgreSQL 提供了弹性跨机并行查询（ePQ）的能力，非常适合进行分析型查询。下面的步骤将引导您可以在一台主机上使用 ePQ 并行执行 TPC-H 查询。

在 `tpch-dbgen/` 目录下通过 `psql` 连接到数据库：

```shell:no-line-numbers
cd tpch-dbgen
psql
```

首先需要对 TPC-H 产生的八张表设置 ePQ 的最大查询并行度：

```sql:no-line-numbers
ALTER TABLE nation SET (px_workers = 100);
ALTER TABLE region SET (px_workers = 100);
ALTER TABLE supplier SET (px_workers = 100);
ALTER TABLE part SET (px_workers = 100);
ALTER TABLE partsupp SET (px_workers = 100);
ALTER TABLE customer SET (px_workers = 100);
ALTER TABLE orders SET (px_workers = 100);
ALTER TABLE lineitem SET (px_workers = 100);
```

以 Q18 为例，执行查询：

```sql:no-line-numbers
-- 打开计时
\timing on

-- 打开 ePQ 功能的开关
SET polar_enable_px = ON;
-- 设置每个节点的 ePQ 并行度为 1
SET polar_px_dop_per_node = 1;

-- 查看 Q18 的执行计划
\i finals/18.explain.sql
                                                                          QUERY PLAN
---------------------------------------------------------------------------------------------------------------------------------------------------------------
 PX Coordinator 2:1  (slice1; segments: 2)  (cost=0.00..257526.21 rows=59986052 width=47)
   Merge Key: orders.o_totalprice, orders.o_orderdate
   ->  GroupAggregate  (cost=0.00..243457.68 rows=29993026 width=47)
         Group Key: orders.o_totalprice, orders.o_orderdate, customer.c_name, customer.c_custkey, orders.o_orderkey
         ->  Sort  (cost=0.00..241257.18 rows=29993026 width=47)
               Sort Key: orders.o_totalprice DESC, orders.o_orderdate, customer.c_name, customer.c_custkey, orders.o_orderkey
               ->  Hash Join  (cost=0.00..42729.99 rows=29993026 width=47)
                     Hash Cond: (orders.o_orderkey = lineitem_1.l_orderkey)
                     ->  PX Hash 2:2  (slice2; segments: 2)  (cost=0.00..15959.71 rows=7500000 width=39)
                           Hash Key: orders.o_orderkey
                           ->  Hash Join  (cost=0.00..15044.19 rows=7500000 width=39)
                                 Hash Cond: (orders.o_custkey = customer.c_custkey)
                                 ->  PX Hash 2:2  (slice3; segments: 2)  (cost=0.00..11561.51 rows=7500000 width=20)
                                       Hash Key: orders.o_custkey
                                       ->  Hash Semi Join  (cost=0.00..11092.01 rows=7500000 width=20)
                                             Hash Cond: (orders.o_orderkey = lineitem.l_orderkey)
                                             ->  Partial Seq Scan on orders  (cost=0.00..1132.25 rows=7500000 width=20)
                                             ->  Hash  (cost=7760.84..7760.84 rows=400 width=4)
                                                   ->  PX Broadcast 2:2  (slice4; segments: 2)  (cost=0.00..7760.84 rows=400 width=4)
                                                         ->  Result  (cost=0.00..7760.80 rows=200 width=4)
                                                               Filter: ((sum(lineitem.l_quantity)) > '314'::numeric)
                                                               ->  Finalize HashAggregate  (cost=0.00..7760.78 rows=500 width=12)
                                                                     Group Key: lineitem.l_orderkey
                                                                     ->  PX Hash 2:2  (slice5; segments: 2)  (cost=0.00..7760.72 rows=500 width=12)
                                                                           Hash Key: lineitem.l_orderkey
                                                                           ->  Partial HashAggregate  (cost=0.00..7760.70 rows=500 width=12)
                                                                                 Group Key: lineitem.l_orderkey
                                                                                 ->  Partial Seq Scan on lineitem  (cost=0.00..3350.82 rows=29993026 width=12)
                                 ->  Hash  (cost=597.51..597.51 rows=749979 width=23)
                                       ->  PX Hash 2:2  (slice6; segments: 2)  (cost=0.00..597.51 rows=749979 width=23)
                                             Hash Key: customer.c_custkey
                                             ->  Partial Seq Scan on customer  (cost=0.00..511.44 rows=749979 width=23)
                     ->  Hash  (cost=5146.80..5146.80 rows=29993026 width=12)
                           ->  PX Hash 2:2  (slice7; segments: 2)  (cost=0.00..5146.80 rows=29993026 width=12)
                                 Hash Key: lineitem_1.l_orderkey
                                 ->  Partial Seq Scan on lineitem lineitem_1  (cost=0.00..3350.82 rows=29993026 width=12)
 Optimizer: PolarDB PX Optimizer
(37 rows)

Time: 216.672 ms

-- 执行 Q18
       c_name       | c_custkey | o_orderkey | o_orderdate | o_totalprice |  sum
--------------------+-----------+------------+-------------+--------------+--------
 Customer#001287812 |   1287812 |   42290181 | 1997-11-26  |    558289.17 | 318.00
 Customer#001172513 |   1172513 |   36667107 | 1997-06-06  |    550142.18 | 322.00
 ...
 Customer#001288183 |   1288183 |   48943904 | 1996-07-22  |    398081.59 | 325.00
 Customer#000114613 |    114613 |   59930883 | 1997-05-17  |    394335.49 | 319.00
(84 rows)

Time: 59113.965 ms (00:59.114)
```

可以看到比 PostgreSQL 的单机并行执行的时间略短。加大 ePQ 功能的节点并行度，查询性能将会有更明显的提升：

```sql:no-line-numbers
SET polar_px_dop_per_node = 2;
\i finals/18.sql
       c_name       | c_custkey | o_orderkey | o_orderdate | o_totalprice |  sum
--------------------+-----------+------------+-------------+--------------+--------
 Customer#001287812 |   1287812 |   42290181 | 1997-11-26  |    558289.17 | 318.00
 Customer#001172513 |   1172513 |   36667107 | 1997-06-06  |    550142.18 | 322.00
 ...
 Customer#001288183 |   1288183 |   48943904 | 1996-07-22  |    398081.59 | 325.00
 Customer#000114613 |    114613 |   59930883 | 1997-05-17  |    394335.49 | 319.00
(84 rows)

Time: 42400.500 ms (00:42.401)

SET polar_px_dop_per_node = 4;
\i finals/18.sql

       c_name       | c_custkey | o_orderkey | o_orderdate | o_totalprice |  sum
--------------------+-----------+------------+-------------+--------------+--------
 Customer#001287812 |   1287812 |   42290181 | 1997-11-26  |    558289.17 | 318.00
 Customer#001172513 |   1172513 |   36667107 | 1997-06-06  |    550142.18 | 322.00
 ...
 Customer#001288183 |   1288183 |   48943904 | 1996-07-22  |    398081.59 | 325.00
 Customer#000114613 |    114613 |   59930883 | 1997-05-17  |    394335.49 | 319.00
(84 rows)

Time: 19892.603 ms (00:19.893)

SET polar_px_dop_per_node = 8;
\i finals/18.sql
       c_name       | c_custkey | o_orderkey | o_orderdate | o_totalprice |  sum
--------------------+-----------+------------+-------------+--------------+--------
 Customer#001287812 |   1287812 |   42290181 | 1997-11-26  |    558289.17 | 318.00
 Customer#001172513 |   1172513 |   36667107 | 1997-06-06  |    550142.18 | 322.00
 ...
 Customer#001288183 |   1288183 |   48943904 | 1996-07-22  |    398081.59 | 325.00
 Customer#000114613 |    114613 |   59930883 | 1997-05-17  |    394335.49 | 319.00
(84 rows)

Time: 10944.402 ms (00:10.944)
```

> 使用 ePQ 执行 Q17 和 Q18 时可能会出现 OOM。需要设置以下参数防止用尽内存：
>
> ```sql:no-line-numbers
> SET polar_px_optimizer_enable_hashagg = 0;
> ```

## 执行 ePQ 跨机并行执行

在上面的例子中，出于简单考虑，PolarDB for PostgreSQL 的多个计算节点被部署在同一台主机上。在这种场景下使用 ePQ 时，由于所有的计算节点都使用了同一台主机的 CPU、内存、I/O 带宽，因此本质上是基于单台主机的并行执行。实际上，PolarDB for PostgreSQL 的计算节点可以被部署在能够共享存储节点的多台机器上。此时使用 ePQ 功能将进行真正的跨机器分布式并行查询，能够充分利用多台机器上的计算资源。

参考 [进阶部署](../deploying/deploy.md) 可以搭建起不同形态的 PolarDB for PostgreSQL 集群。集群搭建成功后，使用 ePQ 的方式与单机 ePQ 完全相同。

> 如果遇到如下错误：
>
> ```shell:no-line-numbers
> psql:queries/q01.analyze.sq1:24: WARNING:  interconnect may encountered a network error, please check your network
> DETAIL:  Failed to send packet (seq 1) to 192.168.1.8:57871 (pid 17766 cid 0) after 100 retries.
> ```
>
> 可以尝试统一修改每台机器的 MTU 为 9000：
>
> ```shell:no-line-numbers
> ifconfig <网卡名> mtu 9000
> ```
