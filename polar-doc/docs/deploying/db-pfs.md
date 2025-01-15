---
author: 棠羽
date: 2025/01/15
minute: 15
---

# 基于 PFS 文件系统部署

<ArticleInfo :frontmatter=$frontmatter></ArticleInfo>

本文将指导您在分布式文件系统 PolarDB File System（PFS）上编译部署 PolarDB-PG，适用于已经在共享存储上格式化并挂载 PFS 文件系统的计算节点。

[[toc]]

## Primary 节点部署

初始化 Primary 节点的本地数据目录 `~/primary/`：

```bash:no-line-numbers
initdb -D $HOME/primary
```

在共享存储的 `/nvme1n1/shared_data/` 路径上创建共享数据目录，然后使用 `polar-initdb.sh` 脚本初始化共享数据目录：

```bash:no-line-numbers
# 使用 pfs 创建共享数据目录
sudo pfs -C disk mkdir /nvme1n1/shared_data
# 初始化 Primary 节点的本地目录和共享目录
sudo polar-initdb.sh $HOME/primary/ /nvme1n1/shared_data/ primary
# 注入配置模板
cat /u01/polardb_pg/share/polardb.conf.sample >> $HOME/primary/postgresql.conf
```

编辑 Primary 节点的配置文件 `~/primary/postgresql.conf`，增加配置项：

```ini:no-line-numbers
port=5432
polar_hostid=1
polar_disk_name='nvme1n1'
polar_datadir='/nvme1n1/shared_data/'
polar_vfs.localfs_mode=off
polar_storage_cluster_name='disk'
```

编辑 Primary 节点的客户端认证文件 `~/primary/pg_hba.conf`，增加以下配置项，允许 Replica 节点进行物理复制：

```ini:no-line-numbers
host	replication	postgres	0.0.0.0/0	trust
```

最后，启动 Primary 节点：

```bash:no-line-numbers
pg_ctl start -D $HOME/primary
```

检查 Primary 节点能否正常运行：

```bash:no-line-numbers
psql -p 5432 -d postgres -c 'SELECT version();'
                                   version
----------------------------------------------------------------------
 PostgreSQL 15.x (PolarDB 15.x.x.x build xxxxxxxx) on {your_platform}
(1 row)
```

在 Primary 节点上，为对应的 Replica 节点创建相应的复制槽，用于 Replica 节点的物理复制：

```bash:no-line-numbers
psql -p 5432 -d postgres \
    -c "SELECT pg_create_physical_replication_slot('replica1');"
 pg_create_physical_replication_slot
-------------------------------------
 (replica1,)
(1 row)
```

## Replica 节点部署

在 Replica 节点本地磁盘的 `~/replica1` 路径下创建一个空目录，然后通过 `polar-initdb.sh` 脚本使用共享存储上的数据目录来初始化 Replica 节点的本地目录。初始化后的本地目录中没有默认配置文件，所以还需要使用 `initdb` 创建一个临时的本地目录模板，然后将所有的默认配置文件拷贝到 Replica 节点的本地目录下：

```shell:no-line-numbers
# 从共享存储初始化 Replica 节点的本地目录
mkdir -m 0700 $HOME/replica1
polar-initdb.sh $HOME/replica1/ /nvme1n1/shared_data/ replica

# 产生配置文件
initdb -D /tmp/replica1
cp /tmp/replica1/*.conf $HOME/replica1/

# 注入配置模板
cat /u01/polardb_pg/share/polardb.conf.sample >> $HOME/replica1/postgresql.conf
```

编辑 Replica 节点的配置文件 `~/replica1/postgresql.conf`，增加配置项：

```ini:no-line-numbers
port=5433
polar_hostid=2
polar_disk_name='nvme1n1'
polar_datadir='/nvme1n1/shared_data/'
polar_vfs.localfs_mode=off
polar_storage_cluster_name='disk'

# replication
primary_slot_name='replica1'
primary_conninfo='host=[Primary 节点所在IP] port=5432 user=postgres dbname=postgres application_name=replica1'
```

标识节点以 Replica 模式启动：

```shell:no-line-numbers
touch $HOME/replica1/replica.signal
```

最后，启动 Replica 节点：

```bash:no-line-numbers
pg_ctl start -D $HOME/replica1
```

检查 Replica 节点能否正常运行：

```bash:no-line-numbers
psql -p 5433 -d postgres -c 'SELECT version();'
                                   version
----------------------------------------------------------------------
 PostgreSQL 15.x (PolarDB 15.x.x.x build xxxxxxxx) on {your_platform}
(1 row)
```

## 集群检查和测试

部署完成后，需要进行实例检查和测试，确保 Primary 节点可正常写入数据，Replica 节点可以正常读取。

登录 Primary 节点，创建测试表并插入样例数据：

```bash:no-line-numbers
psql -q -p 5432 -d postgres \
    -c "CREATE TABLE t (t1 INT PRIMARY KEY, t2 INT); INSERT INTO t VALUES (1, 1),(2, 3),(3, 3);"
```

登录 Replica 节点，查询刚刚插入的样例数据：

```bash:no-line-numbers
psql -q -p 5433 -d postgres -c "SELECT * FROM t;"
 t1 | t2
----+----
  1 |  1
  2 |  3
  3 |  3
(3 rows)
```

在 Primary 节点上插入的数据对 Replica 节点可见，这意味着基于共享存储的 PolarDB-PG 计算节点集群搭建成功。

---

## 常见运维步骤

- [备份恢复](../operation/backup-and-restore.md)
- [共享存储在线扩容](../operation/grow-storage.md)
- [计算节点扩缩容](../operation/scale-out.md)
- [Replica 节点在线 Promote](../operation/ro-online-promote.md)
