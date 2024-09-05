---
author: 棠羽
date: 2023/02/08
minute: 10
---

# ePQ 支持创建/刷新物化视图并行加速和批量写入

<Badge type="tip" text="V11 / v1.1.30-" vertical="top" />

<ArticleInfo :frontmatter=$frontmatter></ArticleInfo>

[[toc]]

## 背景

[物化视图 (Materialized View)](https://en.wikipedia.org/wiki/Materialized_view) 是一个包含查询结果的数据库对象。与普通的视图不同，物化视图不仅保存视图的定义，还保存了 [创建物化视图](https://www.postgresql.org/docs/current/sql-creatematerializedview.html) 时的数据副本。当物化视图的数据与视图定义中的数据不一致时，可以进行 [物化视图刷新 (Refresh)](https://www.postgresql.org/docs/current/sql-refreshmaterializedview.html) 保持物化视图中的数据与视图定义一致。物化视图本质上是对视图定义中的查询做预计算，以便于在查询时复用。

[`CREATE TABLE AS`](https://www.postgresql.org/docs/current/sql-createtableas.html) 语法用于将一个查询所对应的数据构建为一个新的表，其表结构与查询的输出列完全相同。

[`SELECT INTO`](https://www.postgresql.org/docs/current/sql-selectinto.html) 语法用于建立一张新表，并将查询所对应的数据写入表中，而不是将查询到的数据返回给客户端。其表结构与查询的输出列完全相同。

## 功能原理介绍

对于物化视图的创建和刷新，以及 `CREATE TABLE AS` / `SELECT INTO` 语法，由于在数据库层面需要完成的工作步骤十分相似，因此 PostgreSQL 内核使用同一套代码逻辑来处理这几种语法。内核执行过程中的主要步骤包含：

1. 数据扫描：执行视图定义或 `CREATE TABLE AS` / `SELECT INTO` 语法中定义的查询，扫描符合查询条件的数据
2. 数据写入：将上述步骤中扫描到的数据写入到一个新的物化视图 / 表中

PolarDB for PostgreSQL 对上述两个步骤分别引入了 ePQ 并行扫描和批量数据写入的优化。在需要扫描或写入的数据量较大时，能够显著提升上述 DDL 语法的性能，缩短执行时间：

1. ePQ 并行扫描：通过 ePQ 功能，利用多个计算节点的 I/O 带宽和计算资源并行执行视图定义中的查询，提升计算资源和带宽的利用率
2. 批量写入：不再将扫描到的每一个元组依次写入表或物化视图，而是在内存中攒够一定数量的元组后，一次性批量写入表或物化视图中，减少记录 WAL 日志的开销，降低对页面的锁定频率

## 使用说明

### ePQ 并行扫描

将以下参数设置为 `ON` 即可启用 ePQ 并行扫描来加速上述语法中的查询过程，目前其默认值为 `ON`。该参数生效的前置条件是 ePQ 特性的总开关 `polar_enable_px` 被打开。

```sql:no-line-numbers
SET polar_px_enable_create_table_as = ON;
```

由于 ePQ 特性的限制，该优化不支持 `CREATE TABLE AS ... WITH OIDS` 语法。对于该语法的处理流程中将会回退使用 PostgreSQL 内置优化器为 DDL 定义中的查询生成执行计划，并通过 PostgreSQL 的单机执行器完成查询。

### 批量写入

将以下参数设置为 `ON` 即可启用批量写入来加速上述语法中的写入过程，目前其默认值为 `ON`。

```sql:no-line-numbers
SET polar_enable_create_table_as_bulk_insert = ON;
```
