---
author: 棠羽
date: 2023/03/06
minute: 20
---

# CPU 使用率高的排查方法

<ArticleInfo :frontmatter=$frontmatter></ArticleInfo>

在 PolarDB for PostgreSQL 的使用过程中，可能会出现 CPU 使用率异常升高甚至达到满载的情况。本文将介绍造成这种情况的常见原因和排查方法，以及相应的解决方案。

[[toc]]

## 业务量上涨

当 CPU 使用率上升时，最有可能的情况是业务量的上涨导致数据库使用的计算资源增多。所以首先需要排查目前数据库的活跃连接数是否比平时高很多。如果数据库配备了监控系统，那么活跃连接数的变化情况可以通过图表的形式观察到；否则可以直接连接到数据库，执行如下 SQL 来获取当前活跃连接数：

```sql:no-line-numbers
SELECT COUNT(*) FROM pg_stat_activity WHERE state NOT LIKE 'idle';
```

`pg_stat_activity` 是 PostgreSQL 的内置系统视图，该视图返回的每一行都是一个正在运行中的 PostgreSQL 进程，`state` 列表示进程当前的状态。该列可能的取值为：

- `active`：进程正在执行查询
- `idle`：进程空闲，正在等待新的客户端命令
- `idle in transaction`：进程处于事务中，但目前暂未执行查询
- `idle in transaction (aborted)`：进程处于事务中，且有一条语句发生过错误
- `fastpath function call`：进程正在执行一个 fast-path 函数
- `disabled`：进程的状态采集功能被关闭

上述 SQL 能够查询到所有非空闲状态的进程数，即可能占用 CPU 的活跃连接数。如果活跃连接数较平时更多，则 CPU 使用率的上升是符合预期的。

## 慢查询

如果 CPU 使用率上升，而活跃连接数的变化范围处在正常范围内，那么有可能出现了较多性能较差的慢查询。这些慢查询可能在很长一段时间里占用了较多的 CPU，导致 CPU 使用率上升。PostgreSQL 提供了慢查询日志的功能，执行时间高于 `log_min_duration_statement` 的 SQL 将会被记录到慢查询日志中。然而当 CPU 占用率接近满载时，将会导致整个系统的停滞，所有 SQL 的执行可能都会慢下来，所以慢查询日志中记录的信息可能非常多，并不容易排查。

### 定位执行时间较长的慢查询

[`pg_stat_statements`](https://www.postgresql.org/docs/15/pgstatstatements.html) 插件能够记录数据库服务器上所有 SQL 语句在优化和执行阶段的统计信息。由于该插件需要使用共享内存，因此插件名需要被配置在 `shared_preload_libraries` 参数中。

如果没有在当前数据库中创建过 `pg_stat_statements` 插件的话，首先需要创建这个插件。该过程将会注册好插件提供的函数及视图：

```sql:no-line-numbers
CREATE EXTENSION IF NOT EXISTS pg_stat_statements;
```

该插件和数据库系统本身都会不断累积统计信息。为了排查 CPU 异常升高后这段时间内的问题，需要把数据库和插件中留存的统计信息做一次清空，然后开始收集从当前时刻开始的统计信息：

```sql:no-line-numbers
-- 清空当前数据库的统计信息
SELECT pg_stat_reset();
-- 清空 pg_stat_statements 插件截止目前收集的统计信息
SELECT pg_stat_statements_reset();
```

接下来需要等待一段时间（1-2 分钟），使数据库和插件充分采集这段时间内的统计信息。

统计信息收集完毕后，参考使用如下 SQL 查询执行时间最长的 5 条 SQL：

```sql:no-line-numbers
-- < PostgreSQL 13
SELECT * FROM pg_stat_statements ORDER BY total_time DESC LIMIT 5;
-- >= PostgreSQL 13
SELECT * FROM pg_stat_statements ORDER BY total_exec_time DESC LIMIT 5;
```

### 定位读取 Buffer 数量较多的慢查询

当一张表缺少索引，而对该表的查询基本上都是点查时，数据库将不得不使用全表扫描，并在内存中进行过滤条件的判断，处理掉大量的无效记录，导致 CPU 使用率大幅提升。利用 `pg_stat_statements` 插件的统计信息，参考如下 SQL，可以列出截止目前读取 Buffer 数量最多的 5 条 SQL：

```sql:no-line-numbers
SELECT * FROM pg_stat_statements
ORDER BY shared_blks_hit + shared_blks_read DESC
LIMIT 5;
```

借助 PostgreSQL 内置系统视图 [`pg_stat_user_tables`](https://www.postgresql.org/docs/15/monitoring-stats.html#MONITORING-PG-STAT-ALL-TABLES-VIEW) 中的统计信息，也可以统计出使用全表扫描的次数最多的表。参考如下 SQL，可以获取具备一定规模数据量（元组约为 10 万个）且使用全表扫描获取到的元组数量最多的 5 张表：

```sql:no-line-numbers
SELECT * FROM pg_stat_user_tables
WHERE n_live_tup > 100000 AND seq_scan > 0
ORDER BY seq_tup_read DESC
LIMIT 5;
```

### 定位长时间执行不结束的慢查询

通过系统内置视图 `pg_stat_activity`，可以查询出长时间执行不结束的 SQL，这些 SQL 有极大可能造成 CPU 使用率过高。参考以下 SQL 获取查询执行时间最长，且目前还未退出的 5 条 SQL：

```sql:no-line-numbers
SELECT
    *,
    extract(epoch FROM (NOW() - xact_start)) AS xact_stay,
    extract(epoch FROM (NOW() - query_start)) AS query_stay
FROM pg_stat_activity
WHERE state NOT LIKE 'idle%'
ORDER BY query_stay DESC
LIMIT 5;
```

结合前一步中排查到的 **使用全表扫描最多的表**，参考如下 SQL 获取 **在该表上** 执行时间超过一定阈值（比如 10s）的慢查询：

```sql:no-line-numbers
SELECT * FROM pg_stat_activity
WHERE
    state NOT LIKE 'idle%' AND
    query ILIKE '%表名%' AND
    NOW() - query_start > interval '10s';
```

### 解决方法与优化思路

对于异常占用 CPU 较高的 SQL，如果仅有个别非预期 SQL，则可以通过给后端进程发送信号的方式，先让 SQL 执行中断，使 CPU 使用率恢复正常。参考如下 SQL，以慢查询执行所使用的进程 pid（`pg_stat_activity` 视图的 `pid` 列）作为参数，中止相应的进程的执行：

```sql:no-line-numbers
SELECT pg_cancel_backend(pid);
SELECT pg_terminate_backend(pid);
```

如果执行较慢的 SQL 是业务上必要的 SQL，那么需要对它进行调优。

首先可以对 SQL 涉及到的表进行采样，更新其统计信息，使优化器能够产生更加准确的执行计划。采样需要占用一定的 CPU，最好在业务低谷期运行：

```sql:no-line-numbers
ANALYZE 表名;
```

对于全表扫描较多的表，可以在常用的过滤列上创建索引，以尽量使用索引扫描，减少全表扫描在内存中过滤不符合条件的记录所造成的 CPU 浪费。
