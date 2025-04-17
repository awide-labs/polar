---
author: 棠羽
date: 2024/08/30
minute: 10
---

# 基于 Docker 容器开发

<ArticleInfo :frontmatter=$frontmatter></ArticleInfo>

## 下载源代码

从 [GitHub](https://github.com/ApsaraDB/PolarDB-for-PostgreSQL) 上下载 PolarDB for PostgreSQL 的源代码，稳定分支为 `POLARDB_15_STABLE`：

::: code-tabs
@tab GitHub

```bash:no-line-numbers
git clone -b POLARDB_15_STABLE https://github.com/ApsaraDB/PolarDB-for-PostgreSQL.git
```

@tab Gitee

```bash:no-line-numbers
git clone -b POLARDB_15_STABLE https://gitee.com/mirrors/PolarDB-for-PostgreSQL
```

:::

代码克隆完毕后，进入源码目录：

```bash:no-line-numbers
cd PolarDB-for-PostgreSQL/
```

## 拉取开发镜像

拉取 PolarDB-PG 的 [开发镜像](https://hub.docker.com/r/polardb/polardb_pg_devel/tags)。

::: code-tabs
@tab DockerHub

```bash:no-line-numbers
docker pull polardb/polardb_pg_devel:ubuntu24.04
```

@tab 阿里云 ACR

```bash:no-line-numbers
docker pull registry.cn-hangzhou.aliyuncs.com/polardb_pg/polardb_pg_devel:ubuntu24.04
```

:::

## 创建并运行容器

此时我们已经在开发机器的源码目录中。从开发镜像上创建一个容器，将当前目录作为一个 volume 挂载到容器中，这样可以：

- 在容器内的环境中编译源码
- 在容器外（开发机器上）使用编辑器来查看或修改代码

::: code-tabs
@tab DockerHub

```bash:no-line-numbers
docker run -it \
    -v $PWD:/home/postgres/polardb_pg \
    --shm-size=512m --cap-add=SYS_PTRACE --privileged=true \
    --name polardb_pg_devel \
    polardb/polardb_pg_devel:ubuntu24.04 \
    bash
```

@tab 阿里云 ACR

```bash:no-line-numbers
docker run -it \
    -v $PWD:/home/postgres/polardb_pg \
    --shm-size=512m --cap-add=SYS_PTRACE --privileged=true \
    --name polardb_pg_devel \
    registry.cn-hangzhou.aliyuncs.com/polardb_pg/polardb_pg_devel:ubuntu24.04 \
    bash
```

:::

进入容器后，为容器内用户获取源码目录的权限，然后编译部署 PolarDB-PG 实例。

```bash:no-line-numbers
# 获取权限并编译部署
cd polardb_pg
sudo chmod -R a+wr ./
sudo chown -R postgres:postgres ./
./build.sh

# 验证
psql -c 'SELECT version();'
                                   version
----------------------------------------------------------------------
 PostgreSQL 15.x (PolarDB 15.x.x.x build xxxxxxxx) on {your_platform}
(1 row)
```

## 构建选项说明

以下表格列出了编译、初始化或测试 PolarDB-PG 集群所可能使用到的选项及说明。更多选项及其说明详见源码目录下的 `build.sh` 脚本。

| 选项                 | 描述                                        |
| -------------------- | ------------------------------------------- |
| `--wr=N`             | 初始化 Replica 节点数量                     |
| `--ws=N`             | 初始化 Standby 节点数量                     |
| `--ec='--with-pfsd'` | 是否编译 PolarDB File System (PFS) 相关功能 |
| `--ni`               | 只编译，不拉起示例集群                      |
| `--port=`            | 指定 primary 节点端口号                     |
| `--debug=[on/off]`   | 编译为 Debug / Release 模式                 |

如无定制的需求，则可以按照下面给出的选项编译部署不同形态的 PolarDB-PG 集群并进行测试。

## 各形态编译部署

本地搭建一个单 Primary 节点：

```bash:no-line-numbers
./build.sh
```

本地搭建一个 Primary 节点 + 两个 Replica 节点的共享存储集群：

```bash:no-line-numbers
./build.sh --wr=2
```

本地搭建一个 Primary 节点 + 一个 Replica 节点 + 一个 Standby 节点的集群：

```bash:no-line-numbers
./build.sh --wr=1 --ws=1
```

## 回归测试

可以根据机器负载自行调整并行度 `$jobs`。

Debug 模式：

```bash:no-line-numbers
./build.sh --ws=1 --wr=2 --debug=on --jobs=$jobs --ec="--enable-tap-tests"
make precheck -j$jobs
```

Release 模式：

```bash:no-line-numbers
./build.sh --ws=1 --wr=2 --debug=off --jobs=$jobs --ec="--enable-tap-tests"
make precheck -j$jobs
```
