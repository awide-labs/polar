---
home: true
title: Documentation
heroImage: /images/polardb.png
footer: Apache 2.0 Licensed | Copyright © Alibaba Group, Inc.
---

---

### Quick Start with Docker

Pull the [local instance image](https://hub.docker.com/r/polardb/polardb_pg_local_instance/tags) of PolarDB for PostgreSQL based on local storage. Create and run the container, and try PolarDB-PG instance directly:

::: code-tabs
@tab DockerHub

```bash:no-line-numbers
# pull the instance image and run the container
docker pull polardb/polardb_pg_local_instance:15
docker run -it --cap-add=SYS_PTRACE --privileged=true --rm polardb/polardb_pg_local_instance:15 psql
# check
postgres=# SELECT version();
                                   version
----------------------------------------------------------------------
 PostgreSQL 15.x (PolarDB 15.x.x.x build xxxxxxxx) on {your_platform}
(1 row)
```

@tab Alibaba Cloud ACR

```bash:no-line-numbers
# pull the instance image and run the container
docker pull registry.cn-hangzhou.aliyuncs.com/polardb_pg/polardb_pg_local_instance:15
docker run -it --cap-add=SYS_PTRACE --privileged=true --rm registry.cn-hangzhou.aliyuncs.com/polardb_pg/polardb_pg_local_instance:15 psql
# check
postgres=# SELECT version();
                                   version
----------------------------------------------------------------------
 PostgreSQL 15.x (PolarDB 15.x.x.x build xxxxxxxx) on {your_platform}
(1 row)
```

:::
