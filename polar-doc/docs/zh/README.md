---
home: true
title: 文档
heroImage: /images/polardb.png
footer: Apache 2.0 Licensed | Copyright © Alibaba Group, Inc.
---

---

### 通过 Docker 快速使用

拉取 PolarDB for PostgreSQL 的 [单机实例镜像](https://hub.docker.com/r/polardb/polardb_pg_local_instance/tags)，运行容器并试用 PolarDB-PG：

::: code-tabs
@tab DockerHub

```bash:no-line-numbers
# 拉取镜像并运行容器
docker pull polardb/polardb_pg_local_instance:15
docker run -it --cap-add=SYS_PTRACE --privileged=true --rm polardb/polardb_pg_local_instance:15 psql
# 测试可用性
postgres=# SELECT version();
                                   version
----------------------------------------------------------------------
 PostgreSQL 15.x (PolarDB 15.x.x.x build xxxxxxxx) on {your_platform}
(1 row)
```

@tab 阿里云 ACR

```bash:no-line-numbers
# 拉取镜像并运行容器
docker pull registry.cn-hangzhou.aliyuncs.com/polardb_pg/polardb_pg_local_instance:15
docker run -it --cap-add=SYS_PTRACE --privileged=true --rm registry.cn-hangzhou.aliyuncs.com/polardb_pg/polardb_pg_local_instance:15 psql
# 测试可用性
postgres=# SELECT version();
                                   version
----------------------------------------------------------------------
 PostgreSQL 15.x (PolarDB 15.x.x.x build xxxxxxxx) on {your_platform}
(1 row)
```

:::
