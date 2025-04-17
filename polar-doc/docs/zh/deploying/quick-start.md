---
author: 棠羽
date: 2024/08/30
minute: 5
---

# 快速部署

<ArticleInfo :frontmatter=$frontmatter></ArticleInfo>

::: danger
为简化使用，容器内的 `postgres` 用户没有设置密码，仅供体验。如果在生产环境等高安全性需求场合，请务必修改健壮的密码！
:::

仅需单台机器，同时满足以下要求，就可以快速开启您的 PolarDB 之旅：

- CPU 架构为 AMD64 / ARM64
- 可用内存 4GB 以上
- 已安装 [Docker](https://www.docker.com/)
  - Ubuntu：[在 Ubuntu 上安装 Docker Engine](https://docs.docker.com/engine/install/ubuntu/)
  - Debian：[在 Debian 上安装 Docker Engine](https://docs.docker.com/engine/install/debian/)
  - CentOS：[在 CentOS 上安装 Docker Engine](https://docs.docker.com/engine/install/centos/)
  - RHEL：[在 RHEL 上安装 Docker Engine](https://docs.docker.com/engine/install/rhel/)
  - Fedora：[在 Fedora 上安装 Docker Engine](https://docs.docker.com/engine/install/fedora/)
  - macOS：[在 Mac 上安装 Docker Desktop](https://docs.docker.com/desktop/mac/install/)，并建议将内存调整为 4GB 以上
  - Windows：[在 Windows 上安装 Docker Desktop](https://docs.docker.com/desktop/windows/install/)，并建议将内存调整为 4GB 以上

拉取 PolarDB for PostgreSQL 的 [单机实例镜像](https://hub.docker.com/r/polardb/polardb_pg_local_instance/tags)，运行容器并试用 PolarDB-PG：

::: code-tabs
@tab DockerHub

```bash:no-line-numbers
# 拉取镜像并运行容器
docker pull polardb/polardb_pg_local_instance:15
docker run -it --rm polardb/polardb_pg_local_instance:15 psql
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
docker run -it --rm registry.cn-hangzhou.aliyuncs.com/polardb_pg/polardb_pg_local_instance:15 psql
# 测试可用性
postgres=# SELECT version();
                                   version
----------------------------------------------------------------------
 PostgreSQL 15.x (PolarDB 15.x.x.x build xxxxxxxx) on {your_platform}
(1 row)
```

:::
