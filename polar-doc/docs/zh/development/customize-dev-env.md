---
author: 棠羽
date: 2024/08/30
minute: 5
---

# 定制开发环境

<ArticleInfo :frontmatter=$frontmatter></ArticleInfo>

PolarDB-PG 提供已经构建完毕的开发镜像 [`polardb/polardb_pg_devel`](https://hub.docker.com/r/polardb/polardb_pg_devel/tags) 可供直接使用，镜像支持的 CPU 架构包含：

- `linux/amd64`
- `linux/arm64`

支持的 Linux 发行版包含：

- CentOS 7
- Anolis 8
- Rocky 8
- Rocky 9
- Ubuntu 20.04
- Ubuntu 22.04
- Ubuntu 24.04

通过如下方式即可拉取相应发行版的镜像：

::: code-tabs
@tab DockerHub

```bash:no-line-numbers
docker pull polardb/polardb_pg_devel:centos7
docker pull polardb/polardb_pg_devel:anolis8
docker pull polardb/polardb_pg_devel:rocky8
docker pull polardb/polardb_pg_devel:rocky9
docker pull polardb/polardb_pg_devel:ubuntu20.04
docker pull polardb/polardb_pg_devel:ubuntu22.04
docker pull polardb/polardb_pg_devel:ubuntu24.04
```

@tab 阿里云 ACR

```bash:no-line-numbers
docker pull registry.cn-hangzhou.aliyuncs.com/polardb_pg/polardb_pg_devel:centos7
docker pull registry.cn-hangzhou.aliyuncs.com/polardb_pg/polardb_pg_devel:anolis8
docker pull registry.cn-hangzhou.aliyuncs.com/polardb_pg/polardb_pg_devel:rocky8
docker pull registry.cn-hangzhou.aliyuncs.com/polardb_pg/polardb_pg_devel:rocky9
docker pull registry.cn-hangzhou.aliyuncs.com/polardb_pg/polardb_pg_devel:ubuntu20.04
docker pull registry.cn-hangzhou.aliyuncs.com/polardb_pg/polardb_pg_devel:ubuntu22.04
docker pull registry.cn-hangzhou.aliyuncs.com/polardb_pg/polardb_pg_devel:ubuntu24.04
```

:::

另外，也提供了构建上述开发镜像的 [Dockerfile](https://github.com/ApsaraDB/polardb-pg-docker-images)，您可以根据自己的需要在 Dockerfile 中添加更多依赖，然后构建自己的开发镜像。
