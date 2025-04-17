---
author: 棠羽
date: 2025/01/15
minute: 15
---

# 格式化并挂载 PFS

<ArticleInfo :frontmatter=$frontmatter></ArticleInfo>

PolarDB File System，简称 PFS 或 PolarFS，是由阿里云自主研发的高性能类 POSIX 的用户态分布式文件系统，服务于阿里云数据库 PolarDB 产品。使用 PFS 对共享存储进行格式化并挂载后，能够保证一个计算节点对共享存储的写入能够立刻对另一个计算节点可见。

[[toc]]

## PFS 编译安装

推荐使用 PolarDB-PG 的 [可执行文件镜像](https://hub.docker.com/r/polardb/polardb_pg_binary/tags)，其中已经包含了编译完毕的 PFS 工具，无需手动编译安装。通过以下命令进入容器即可：

::: code-tabs
@tab DockerHub

```shell:no-line-numbers
docker pull polardb/polardb_pg_binary:15
docker run -it \
    --network=host \
    --cap-add=SYS_PTRACE \
    --privileged=true \
    --name polardb_pg \
    --shm-size=512m \
    polardb/polardb_pg_binary:15 \
    bash
```

@tab 阿里云 ACR

```shell:no-line-numbers
docker pull registry.cn-hangzhou.aliyuncs.com/polardb_pg/polardb_pg_binary:15
docker run -it \
    --cap-add=SYS_PTRACE \
    --privileged=true \
    --name polardb_pg \
    --shm-size=512m \
    registry.cn-hangzhou.aliyuncs.com/polardb_pg/polardb_pg_binary:15 \
    bash
```

:::

PFS 的手动编译安装方式请参考 PFS 的 [README](https://github.com/ApsaraDB/polardb-file-system/blob/master/Readme-CN.md)，此处不再赘述。

## 块设备重命名

PFS 仅支持访问 **以特定字符开头的块设备**（详情可见 [PolarDB File System](https://github.com/ApsaraDB/PolarDB-FileSystem) 源代码的 [`src/pfs_core/pfs_api.h`](https://github.com/ApsaraDB/PolarDB-FileSystem/blob/master/src/pfs_core/pfs_api.h) 文件）：

```c:no-line-numbers
#define PFS_PATH_ISVALID(path)                                  \
    (path != NULL &&                                            \
     ((path[0] == '/' && isdigit((path)[1])) || path[0] == '.'  \
      || strncmp(path, "/pangu-", 7) == 0                       \
      || strncmp(path, "/sd", 3) == 0                           \
      || strncmp(path, "/sf", 3) == 0                           \
      || strncmp(path, "/vd", 3) == 0                           \
      || strncmp(path, "/nvme", 5) == 0                         \
      || strncmp(path, "/loop", 5) == 0                         \
      || strncmp(path, "/mapper_", 8) ==0))
```

因此，为了保证能够顺畅完成后续流程，我们建议在所有访问块设备的节点上使用相同的软链接访问共享块设备。例如，在 NBD 服务端主机上，使用新的块设备名 `/dev/nvme1n1` 软链接到共享存储块设备的原有名称 `/dev/vdb` 上：

```shell:no-line-numbers
sudo ln -s /dev/vdb /dev/nvme1n1
```

在 NBD 客户端主机上，使用同样的块设备名 `/dev/nvme1n1` 软链到共享存储块设备的原有名称 `/dev/nbd0` 上：

```shell:no-line-numbers
sudo ln -s /dev/nbd0 /dev/nvme1n1
```

这样便可以在服务端和客户端两台主机上使用相同的块设备名 `/dev/nvme1n1` 访问同一个块设备。

## 块设备格式化

使用 **任意一台主机**，在共享存储块设备上格式化 PFS 分布式文件系统：

```shell:no-line-numbers
sudo pfs -C disk mkfs nvme1n1
```

## PFS 文件系统挂载

在能够访问共享存储的 **所有主机节点** 上分别启动 PFS 守护进程，挂载 PFS 文件系统：

```shell:no-line-numbers
sudo /usr/local/polarstore/pfsd/bin/start_pfsd.sh -p nvme1n1 -w 4
```

---

## 在 PFS 上编译部署 PolarDB

参阅 [PolarDB 编译部署：PFS 文件系统](./db-pfs.md)。
