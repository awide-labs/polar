---
author: 棠羽
date: 2023/04/11
minute: 15
---

# TPC-C 测试

<ArticleInfo :frontmatter=$frontmatter></ArticleInfo>

本文将引导您对 PolarDB for PostgreSQL 进行 TPC-C 测试。

[[toc]]

## 背景

TPC 是一系列事务处理和数据库基准测试的规范。其中 [TPC-C](https://www.tpc.org/tpcc/) (Transaction Processing Performance Council) 是针对 OLTP 的基准测试模型。TPC-C 测试模型给基准测试提供了一种统一的测试标准，可以大体观察出数据库服务稳定性、性能以及系统性能等一系列问题。对数据库展开 TPC-C 基准性能测试，一方面可以衡量数据库的性能，另一方面可以衡量采用不同硬件软件系统的性价比，是被业内广泛应用并关注的一种测试模型。

## 测试步骤

### 部署 PolarDB-PG

参考如下教程部署 PolarDB for PostgreSQL：

- [快速部署](../deploying/quick-start.md)
- [进阶部署](../deploying/deploy.md)

### 安装测试工具 BenchmarkSQL

[BenchmarkSQL](https://github.com/pgsql-io/benchmarksql) 依赖 Java 运行环境与 Maven 包管理工具，需要预先安装。拉取 BenchmarkSQL 工具源码并进入目录后，通过 `mvn` 编译工程：

```bash:no-line-numbers
$ git clone https://github.com/pgsql-io/benchmarksql.git
$ cd benchmarksql
$ mvn
```

编译出的工具位于如下目录中：

```bash:no-line-numbers
$ cd target/run
```

### TPC-C 配置

在编译完毕的工具目录下，将会存在面向不同数据库产品的示例配置：

```bash:no-line-numbers
$ ls | grep sample
sample.firebird.properties
sample.mariadb.properties
sample.oracle.properties
sample.postgresql.properties
sample.transact-sql.properties
```

其中，`sample.postgresql.properties` 包含 PostgreSQL 系列数据库的模板参数，可以基于这个模板来修改并自定义配置。参考 BenchmarkSQL 工具的 [文档](https://github.com/pgsql-io/benchmarksql/blob/master/docs/PROPERTIES.md) 可以查看关于配置项的详细描述。

配置项包含的配置类型有：

- JDBC 驱动及连接信息：需要自行配置 PostgreSQL 数据库运行的连接串、用户名、密码等
- 测试规模参数
- 测试时间参数
- 吞吐量参数
- 事务类型参数

### 导入数据

使用 `runDatabaseBuild.sh` 脚本，以配置文件作为参数，产生和导入测试数据：

```bash:no-line-numbers
./runDatabaseBuild.sh sample.postgresql.properties
```

### 预热数据

通常，在正式测试前会进行一次数据预热：

```bash:no-line-numbers
./runBenchmark.sh sample.postgresql.properties
```

### 正式测试

预热完毕后，再次运行同样的命令进行正式测试：

```bash:no-line-numbers
./runBenchmark.sh sample.postgresql.properties
```

### 查看结果

```bash:no-line-numbers
                                          _____ latency (seconds) _____
  TransType              count |   mix % |    mean       max     90th% |    rbk%          errors
+--------------+---------------+---------+---------+---------+---------+---------+---------------+
| NEW_ORDER    |           635 |  44.593 |   0.006 |   0.012 |   0.008 |   1.102 |             0 |
| PAYMENT      |           628 |  44.101 |   0.001 |   0.006 |   0.002 |   0.000 |             0 |
| ORDER_STATUS |            58 |   4.073 |   0.093 |   0.168 |   0.132 |   0.000 |             0 |
| STOCK_LEVEL  |            52 |   3.652 |   0.035 |   0.044 |   0.041 |   0.000 |             0 |
| DELIVERY     |            51 |   3.581 |   0.000 |   0.001 |   0.001 |   0.000 |             0 |
| DELIVERY_BG  |            51 |   0.000 |   0.018 |   0.023 |   0.020 |   0.000 |             0 |
+--------------+---------------+---------+---------+---------+---------+---------+---------------+

Overall NOPM:          635 (98.76% of the theoretical maximum)
Overall TPM:         1,424
```

另外也有 CSV 形式的结果被保存，从输出日志中可以找到结果存放目录。
