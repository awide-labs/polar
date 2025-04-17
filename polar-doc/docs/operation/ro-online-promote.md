---
author: 棠羽
date: 2022/12/25
minute: 15
---

# Replica 节点在线 Promote

<ArticleInfo :frontmatter=$frontmatter></ArticleInfo>

PolarDB for PostgreSQL 是一款存储与计算分离的云原生数据库，所有计算节点共享一份存储，并且对存储的访问具有 **一写多读** 的限制：所有计算节点可以对存储进行读取，但只有一个计算节点可以对存储进行写入。这种限制会带来一个问题：当 Primary 节点因为宕机或网络故障而不可用时，集群中将没有能够可以写入存储的计算节点，应用业务中的增、删、改，以及 DDL 都将无法运行。

本文将指导您在 PolarDB for PostgreSQL 计算集群中的 Primary 节点停止服务时，将任意一个 Replica 节点在线提升为 Primary 节点，从而使集群恢复对于共享存储的写入能力。

[[toc]]

## 前置准备

为方便起见，本示例使用基于本地磁盘的实例来进行演示。拉取如下镜像并启动容器，可以得到带有一个 Primary 节点和一个 Replica 节点的共享存储集群。

::: code-tabs
@tab DockerHub

```shell:no-line-numbers
docker pull polardb/polardb_pg_local_instance:15
docker run -it \
    --cap-add=SYS_PTRACE \
    --privileged=true \
    --name polardb_pg_htap \
    --shm-size=512m \
    polardb/polardb_pg_local_instance:15 \
    bash
```

@tab 阿里云 ACR

```shell:no-line-numbers
docker pull registry.cn-hangzhou.aliyuncs.com/polardb_pg/polardb_pg_local_instance:15
docker run -it \
    --cap-add=SYS_PTRACE \
    --privileged=true \
    --name polardb_pg_htap \
    --shm-size=512m \
    registry.cn-hangzhou.aliyuncs.com/polardb_pg/polardb_pg_local_instance:15 \
    bash
```

:::

容器内的 `5432` 和 `5433` 端口分别运行着 Primary 节点 Replica 节点。两个节点共享数据，并通过物理复制保持内存状态同步。

## Replica 节点不可写

首先，连接到 Primary 节点，创建一张表并插入一些数据：

```shell:no-line-numbers
psql -p5432
```

```sql:no-line-numbers
CREATE TABLE t (id int);
INSERT INTO t SELECT generate_series(1,10);
```

连接到 Replica 节点，并同样试图对表插入数据，将会发现无法进行插入操作：

```shell:no-line-numbers
psql -p5433
```

```sql:no-line-numbers
=> INSERT INTO t SELECT generate_series(1,10);
ERROR:  cannot execute INSERT in a read-only transaction
```

## Primary 节点停止写入

此时，关闭 Primary 节点，模拟出 Primary 节点不可用的行为：

```shell:no-line-numbers
$ pg_ctl -D ~/tmp_polardb_pg_15_primary/ stop
waiting for server to shut down.... done
server stopped
```

此时，集群中没有任何节点可以写入存储了。这时，我们需要将 Replica 节点提升为 Primary 节点，恢复对存储的写入。

## Replica 节点 Promote

只有当 Primary 节点停止写入后，才可以将 Replica 节点提升为 Primary 节点，否则将会出现集群内两个节点同时写入存储的情况。当数据库检测到出现多节点写入时，将会导致运行异常。

将运行在 `5433` 端口的 Replica 节点提升为 Primary 节点：

```shell:no-line-numbers
$ pg_ctl -D ~/tmp_polardb_pg_15_replica1/ promote
waiting for server to promote.... done
server promoted
```

## 计算集群恢复写入

连接到已经完成 promote 的新 Primary 节点上，再次尝试之前的 `INSERT` 操作：

```shell:no-line-numbers
psql -p5433
```

```sql:no-line-numbers
=> INSERT INTO t SELECT generate_series(1,10);
INSERT 0 10
```

从上述结果中可以看到，新的 Primary 节点能够成功对存储进行写入。这说明原先的 Replica 节点已经被提升为 Primary 节点了。
