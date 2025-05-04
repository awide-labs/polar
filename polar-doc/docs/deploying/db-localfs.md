---
author: 棠羽
date: 2024/08/30
minute: 15
---

# 基于单机文件系统部署

<ArticleInfo :frontmatter=$frontmatter></ArticleInfo>

本文将指导您在单机文件系统（如 ext4）上部署 PolarDB-PG。

[[toc]]

## 基于安装包部署

从 GitHub 的 [Release](https://github.com/ApsaraDB/PolarDB-for-PostgreSQL/releases) 页面下载符合 CPU 架构和 Linux 发行版的安装包，并安装到系统上。

创建一个空白的数据目录 `${primary_datadir}`，用于存放 PolarDB-PG 的数据文件：

```bash:no-line-numbers
mkdir ${primary_datadir}
```

并使用 `initdb` 程序在 `${primary_datadir}` 目录下初始化数据库：

```bash:no-line-numbers
/u01/polardb_pg/bin/initdb -k -A trust -D ${primary_datadir}
```

可以在 `${primary_datadir}/postgresql.conf` 中添加更多配置，然后跟随提示启动数据库：

```bash:no-line-numbers
/u01/polardb_pg/bin/pg_ctl -D ${primary_datadir} -l logfile start
```

## 基于 Docker 镜像部署

首先，拉取 PolarDB-PG 的 [二进制镜像](https://hub.docker.com/r/polardb/polardb_pg_binary/tags)，里面已包含 PolarDB-PG 的二进制文件：

::: code-tabs
@tab DockerHub

```bash:no-line-numbers
docker pull polardb/polardb_pg_binary:15
```

@tab 阿里云 ACR

```bash:no-line-numbers
docker pull registry.cn-hangzhou.aliyuncs.com/polardb_pg/polardb_pg_binary:15
```

:::

接下来，创建一个空白数据目录：

```bash:no-line-numbers
mkdir ${primary_datadir}
```

创建一个容器，将该目录作为容器的 volumn 挂载到容器内的 `/var/polardb` 目录下。此外，将该容器的 `5432` 端口映射到主机。以后台模式启动并进入容器，保持容器运行：

::: code-tabs
@tab DockerHub

```bash:no-line-numbers
docker run -d \
    --cap-add=SYS_PTRACE --privileged=true \
    --name polardb -p 5432:5432 \
    -v ${primary_datadir}:/var/polardb \
    polardb/polardb_pg_binary:15 \
    sleep infinity
docker exec -it polardb bash
```

@tab 阿里云 ACR

```bash:no-line-numbers
docker run -d \
    --cap-add=SYS_PTRACE --privileged=true \
    --name polardb -p 5432:5432 \
    -v ${primary_datadir}:/var/polardb \
    registry.cn-hangzhou.aliyuncs.com/polardb_pg/polardb_pg_binary:15 \
    sleep infinity
docker exec -it polardb bash
```

:::

在容器内初始化数据库：

```bash:no-line-numbers
/u01/polardb_pg/bin/initdb -k -A trust -D /var/polardb/
```

类似地，修改 `/var/polardb/postgresql.conf` 中必要的配置后，在容器内启动数据库：

```bash:no-line-numbers
/u01/polardb_pg/bin/pg_ctl -D /var/polardb/ -l logfile start
```

## 基于 Docker 镜像 + 守护脚本部署

首先，拉取 PolarDB-PG 的 [单机实例镜像](https://hub.docker.com/r/polardb/polardb_pg_local_instance/tags)，里面已包含 PolarDB-PG 单机实例的守护脚本。

::: code-tabs
@tab DockerHub

```bash:no-line-numbers
docker pull polardb/polardb_pg_local_instance:15
```

@tab 阿里云 ACR

```bash:no-line-numbers
docker pull registry.cn-hangzhou.aliyuncs.com/polardb_pg/polardb_pg_local_instance:15
```

:::

新建一个空白目录 `${your_data_dir}` 作为 PolarDB-PG 实例的数据目录。启动容器时，将该目录作为 volumn 挂载到容器内，对数据目录进行初始化。在初始化的过程中，可以传入环境变量覆盖默认值：

- `POLARDB_PORT`：PolarDB-PG 运行所需要使用的端口号，默认值为 `5432`；镜像将会使用三个连续的端口号（默认 `5432-5434`）
- `POLARDB_USER`：初始化数据库时创建默认的 superuser（默认 `postgres`）
- `POLARDB_PASSWORD`：默认 superuser 的密码

使用如下命令初始化数据库：

::: code-tabs
@tab DockerHub

```bash:no-line-numbers
docker run -it --rm \
    --cap-add=SYS_PTRACE --privileged=true \
    --env POLARDB_PORT=5432 \
    --env POLARDB_USER=u1 \
    --env POLARDB_PASSWORD=your_password \
    -v ${your_data_dir}:/var/polardb \
    polardb/polardb_pg_local_instance:15 \
    echo 'done'
```

@tab 阿里云 ACR

```bash:no-line-numbers
docker run -it --rm \
    --cap-add=SYS_PTRACE --privileged=true \
    --env POLARDB_PORT=5432 \
    --env POLARDB_USER=u1 \
    --env POLARDB_PASSWORD=your_password \
    -v ${your_data_dir}:/var/polardb \
    registry.cn-hangzhou.aliyuncs.com/polardb_pg/polardb_pg_local_instance:15 \
    echo 'done'
```

:::

数据库初始化完毕后，使用 `-d` 参数以后台模式创建容器，启动 PolarDB-PG 服务。通常 PolarDB-PG 的端口需要暴露给外界使用，使用 `-p` 参数将容器内的端口范围暴露到容器外。比如，初始化数据库时使用的是 `5432-5434` 端口，如下命令将会把这三个端口映射到容器外的 `54320-54322` 端口：

::: code-tabs
@tab DockerHub

```bash:no-line-numbers
docker run -d \
    --cap-add=SYS_PTRACE --privileged=true \
    -p 54320-54322:5432-5434 \
    -v ${your_data_dir}:/var/polardb \
    polardb/polardb_pg_local_instance:15
```

@tab 阿里云 ACR

```bash:no-line-numbers
docker run -d \
    --cap-add=SYS_PTRACE --privileged=true \
    -p 54320-54322:5432-5434 \
    -v ${your_data_dir}:/var/polardb \
    registry.cn-hangzhou.aliyuncs.com/polardb_pg/polardb_pg_local_instance:15
```

:::

或者也可以直接让容器与宿主机共享网络：

::: code-tabs
@tab DockerHub

```bash:no-line-numbers
docker run -d \
    --cap-add=SYS_PTRACE --privileged=true \
    --network=host \
    -v ${your_data_dir}:/var/polardb \
    polardb/polardb_pg_local_instance:15
```

@tab 阿里云 ACR

```bash:no-line-numbers
docker run -d \
    --cap-add=SYS_PTRACE --privileged=true \
    --network=host \
    -v ${your_data_dir}:/var/polardb \
    registry.cn-hangzhou.aliyuncs.com/polardb_pg/polardb_pg_local_instance:15
```

:::
