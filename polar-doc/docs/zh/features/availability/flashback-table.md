---
author: 恒亦
date: 2022/11/23
minute: 20
---

# 闪回表和闪回日志

<Badge type="tip" text="V11 / v1.1.22-" vertical="top" />

<ArticleInfo :frontmatter=$frontmatter></ArticleInfo>

[[toc]]

## 概述

目前文件系统并不能保证数据库页面级别的原子读写，在一次页面的 I/O 过程中，如果发生设备断电等情况，就会造成页面数据的错乱和丢失。在实现闪回表的过程中，我们发现通过定期保存旧版本数据页 + WAL 日志回放的方式可以得到任意时间点的数据页，这样就可以解决半写问题。这种方式和 PostgreSQL 原生的 Full Page Write 相比，由于不在事务提交的主路径上，因此性能有了约 30% ~ 100% 的提升。实例规格越大，负载压力越大，效果越明显。

**闪回日志 (Flashback Log)** 用于保存压缩后的旧版本数据页。其解决半写问题的方案如下：

1. 对 Shared Buffer 中的每个 buffer，在每次 **闪回点 (Flashback Point)** 后第一次修改页面期间，记录 Flashback Log，保存该版本的数据页面
2. Flashback Log 顺序落盘
3. 维护 Flashback Log 的日志索引，用于快速检索某个数据页与其对应的 Flashback Log 记录

当遭遇半写问题（数据页 checksum 不正确）时，通过日志索引快速找到该页对应的 Flashback Log 记录，通过 Flashback Log 记录可以得到旧版本的正确数据页，用于替换被损坏的页。在文件系统不能保证 8kB 级别原子读写的任何设备上，都可以使用这个功能。需要特别注意的是，启用这个功能会造成一定的性能下降。

**闪回表 (Flashback Table)** 功能通过定期保留数据页面快照到闪回日志中，保留事务信息到快速恢复区中，支持用户将某个时刻的表数据恢复到一个新的表中。

## 使用方法

### 语法

```sql:no-line-numbers
FLASHBACK TABLE
    [ schema. ]table
    TO TIMESTAMP expr;
```

### 示例

准备测试数据。创建表 `test`，并插入数据：

```sql:no-line-numbers
CREATE TABLE test(id int);
INSERT INTO test select * FROM generate_series(1, 10000);
```

查看已插入的数据：

```sql:no-line-numbers
polardb=# SELECT count(1) FROM test;
 count
-------
 10000
(1 row)

polardb=# SELECT sum(id) FROM test;
   sum
----------
 50005000
(1 row)
```

等待 10 秒并删除表数据：

```sql:no-line-numbers
SELECT pg_sleep(10);
DELETE FROM test;
```

表中已无数据：

```sql:no-line-numbers
polardb=# SELECT * FROM test;
 id
----
(0 rows)
```

闪回表到 10 秒之前的数据：

```sql:no-line-numbers
polardb=# FLASHBACK TABLE test TO TIMESTAMP now() - interval'10s';
NOTICE:  Flashback the relation test to new relation polar_flashback_65566, please check the data
FLASHBACK TABLE
```

检查闪回表数据：

```sql:no-line-numbers
polardb=# SELECT count(1) FROM polar_flashback_65566;
 count
-------
 10000
(1 row)

polardb=# SELECT sum(id) FROM polar_flashback_65566;
   sum
----------
 50005000
(1 row)
```

## 实践指南

闪回表功能依赖闪回日志和快速恢复区功能，需要设置 `polar_enable_flashback_log` 和 `polar_enable_fast_recovery_area` 参数并重启。其他的参数也需要按照需求来修改，建议一次性修改完成并在业务低峰期重启。打开闪回表功能将会增大内存、磁盘的占用量，并带来一定的性能损失，请谨慎评估后再使用。

### 内存占用

打开闪回日志功能需要增加的共享内存大小为以下三项之和：

- `polar_flashback_log_buffers` \* 8kB
- `polar_flashback_logindex_mem_size` MB
- `polar_flashback_logindex_queue_buffers` MB

打开快速恢复区需要增加大约 32kB 的共享内存大小，请评估当前实例状态后再调整参数。

### 磁盘占用

为了保证能够闪回到一定时间之前，需要保留该段时间的闪回日志和 WAL 日志，以及两者的 LogIndex 文件，这会增加磁盘空间的占用。理论上 `polar_fast_recovery_area_rotation` 设置得越大，磁盘占用越多。若 `polar_fast_recovery_area_rotation` 设置为 `300`，则将会保存 5 个小时的历史数据。

打开闪回日志之后，会定期去做 **闪回点（Flashback Point)**。闪回点是检查点的一种，当触发检查点后会检查 `polar_flashback_point_segments` 和 `polar_flashback_point_timeout` 参数来判断当前检查点是否为闪回点。所以建议：

- 设置 `polar_flashback_point_segments` 为 `max_wal_size` 的倍数
- 设置 `polar_flashback_point_timeout` 为 `checkpoint_timeout` 的倍数

假设 5 个小时共产生 20GB 的 WAL 日志，闪回日志与 WAL 日志的比例大约是 1:20，那么大约会产生 1GB 的闪回日志。闪回日志和 WAL 日志的比例大小和以下两个因素有关：

- 业务模型中，写业务越多，闪回日志越多
- `polar_flashback_point_segments`、`polar_flashback_point_timeout` 参数设定越大，闪回日志越少

### 性能影响

闪回日志特性增加了两个后台进程来消费闪回日志，这势必会增大 CPU 的开销。可以调整 `polar_flashback_log_bgwrite_delay` 和 `polar_flashback_log_insert_list_delay` 参数使得两个后台进程工作间隔周期更长，从而减少 CPU 消耗，但是这可能会造成一定性能的下降，建议使用默认值即可。

另外，由于闪回日志功能需要在该页面刷脏之前，先刷对应的闪回日志，来保证不丢失闪回日志，所以可能会造成一定的性能下降。目前测试在大多数场景下性能下降不超过 5%。

在表闪回的过程中，目标表涉及到的页面在共享内存池中换入换出，可能会造成其他数据库访问操作的性能抖动。

### 使用限制

目前闪回表功能会恢复目标表的数据到一个新表中，表名为 `polar_flashback_目标表 OID`。在执行 `FLASHBACK TABLE` 语法后会有如下 `NOTICE` 提示：

```sql:no-line-numbers
polardb=# flashback table test to timestamp now() - interval '1h';
NOTICE:  Flashback the relation test to new relation polar_flashback_54986, please check the data
FLASHBACK TABLE
```

其中的 `polar_flashback_54986` 就是闪回恢复出的临时表，只恢复表数据到目标时刻。目前只支持 **普通表** 的闪回，不支持以下数据库对象：

- 索引
- Toast 表
- 物化视图
- 分区表 / 分区子表
- 系统表
- 外表
- 含有 toast 子表的表

另外，如果在目标时间到当前时刻对表执行过某些 DDL，则无法闪回：

- `DROP TABLE`
- `ALTER TABLE SET WITH OIDS`
- `ALTER TABLE SET WITHOUT OIDS`
- `TRUNCATE TABLE`
- 修改列类型，修改前后的类型不可以直接隐式转换，且不是无需增加其他值安全强制转换的 USING 子句
- 修改表为 `UNLOGGED` 或者 `LOGGED`
- 增加 `IDENTITY` 的列
- 增加有约束限制的列
- 增加默认值表达式含有易变的函数的列

其中 `DROP TABLE` 的闪回可以使用 PolarDB for PostgreSQL/Oracle 的闪回删除功能来恢复。

### 使用建议

当出现人为误操作数据的情况时，建议先使用审计日志快速定位到误操作发生的时间，然后将目标表闪回到该时间之前。在表闪回过程中，会持有目标表的排他锁，因此仅可以对目标表进行查询操作。另外，在表闪回的过程中，目标表涉及到的页面在共享内存池中换入换出，可能会造成其他数据库访问操作的性能抖动。因此，建议在业务低峰期执行闪回操作。

闪回的速度和表的大小相关。当表比较大时，为节约时间，可以加大 `polar_workers_per_flashback_table` 参数，增加并行闪回的 worker 个数。

在表闪回结束后，可以根据 `NOTICE` 的提示，查询对应闪回表的数据，和原表的数据进行比对。闪回表上不会有任何索引，用户可以根据查询需要自行创建索引。在数据比对完成之后，可以将缺失的数据重新回流到原表。

## 详细参数列表

| 参数名                                  | 参数含义                                                               | 取值范围                    | 默认值        | 生效方法               |
| --------------------------------------- | ---------------------------------------------------------------------- | --------------------------- | ------------- | ---------------------- |
| `polar_enable_flashback_log`            | 是否打开闪回日志                                                       | `on` / `off`                | `off`         | 修改配置文件后重启生效 |
| `polar_enable_fast_recovery_area`       | 是否打开快速恢复区                                                     | `on` / `off`                | `off`         | 修改配置文件后重启生效 |
| `polar_flashback_log_keep_segments`     | 闪回日志保留的文件个数，可重用。每个文件 256MB                         | `[3, 2147483647]`           | `8`           | `SIGHUP` 生效          |
| `polar_fast_recovery_area_rotation`     | 快速恢复区保留的事务信息时长，单位为分钟，即最大可闪回表到几分钟之前。 | `[1, 14400]`                | `180`         | `SIGHUP` 生效          |
| `polar_flashback_point_segments`        | 两个闪回点之间的最小 WAL 日志个数，每个 WAL 日志 1GB                   | `[1, 2147483647]`           | `16`          | `SIGHUP` 生效          |
| `polar_flashback_point_timeout`         | 两个闪回点之间的最小时间间隔，单位为秒                                 | `[1, 86400]`                | `300`         | `SIGHUP` 生效          |
| `polar_flashback_log_buffers`           | 闪回日志共享内存大小，单位为 8kB                                       | `[4, 262144]`               | `2048` (16MB) | 修改配置文件后重启生效 |
| `polar_flashback_logindex_mem_size`     | 闪回日志索引共享内存大小，单位为 MB                                    | `[3, 1073741823]`           | `64`          | 修改配置文件后重启生效 |
| `polar_flashback_logindex_bloom_blocks` | 闪回日志索引的布隆过滤器页面个数                                       | `[8, 1073741823]`           | `512`         | 修改配置文件后重启生效 |
| `polar_flashback_log_insert_locks`      | 闪回日志插入锁的个数                                                   | `[1, 2147483647]`           | `8`           | 修改配置文件后重启生效 |
| `polar_workers_per_flashback_table`     | 闪回表并行 worker 的数量                                               | `[0, 1024]` (0 为关闭并行)  | `5`           | 即时生效               |
| `polar_flashback_log_bgwrite_delay`     | 闪回日志 bgwriter 进程的工作间隔周期，单位为 ms                        | `[1, 10000]`                | `100`         | `SIGHUP` 生效          |
| `polar_flashback_log_flush_max_size`    | 闪回日志 bgwriter 进程每次刷盘闪回日志的大小，单位为 kB                | `[0, 2097152]` (0 为不限制) | `5120`        | `SIGHUP` 生效          |
| `polar_flashback_log_insert_list_delay` | 闪回日志 bginserter 进程的工作间隔周期，单位为 ms                      | `[1, 10000]`                | `10`          | `SIGHUP` 生效          |
