---
author: 棠羽
date: 2023/09/20
minute: 20
---

# ePQ 支持创建 B-Tree 索引并行加速

<Badge type="tip" text="V11 / v1.1.15-" vertical="top" />

<ArticleInfo :frontmatter=$frontmatter></ArticleInfo>

[[toc]]

## 背景

在使用 PostgreSQL 时，如果想要在一张表中查询符合某个条件的行，默认情况下需要扫描整张表的数据，然后对每一行数据依次判断过滤条件。如果符合条件的行数非常少，而表的数据总量非常大，这显然是一个非常低效的操作。与阅读书籍类似，想要阅读某个特定的章节时，读者通常会通过书籍开头处的索引查询到对应章节的页码，然后直接从指定的页码开始阅读；在数据库中，通常会对被频繁查找的列创建索引，以避免进行开销极大的全表扫描：通过索引可以精确定位到被查找的数据位于哪些数据页面上。

PostgreSQL 支持创建多种类型的索引，其中使用得最多的是 [B-Tree](https://www.postgresql.org/docs/current/indexes-types.html#INDEXES-TYPES-BTREE) 索引，也是 PostgreSQL 默认创建的索引类型。在一张数据量较大的表上创建索引是一件非常耗时的事，因为其中涉及到的工作包含：

1. 顺序扫描表中的每一行数据
2. 根据要创建索引的列值（Scan Key）顺序，对每行数据在表中的物理位置进行排序
3. 构建索引元组，按 B-Tree 的结构组织并写入索引页面

PostgreSQL 支持并行（多进程扫描/排序）和并发（不阻塞 DML）创建索引，但只能在创建索引的过程中使用单个计算节点的资源。

PolarDB-PG 的 ePQ 弹性跨机并行查询特性支持对 B-Tree 类型的索引创建进行加速。ePQ 能够利用多个计算节点的 I/O 带宽并行扫描全表数据，并利用多个计算节点的 CPU 和内存资源对每行数据在表中的物理位置按索引列值进行排序，构建索引元组。最终，将有序的索引元组归并到创建索引的进程中，写入索引页面，完成索引的创建。

## 使用方法

### 数据准备

创建一张包含三个列，数据量为 1000000 行的表：

```sql:no-line-numbers
CREATE TABLE t (id INT, age INT, msg TEXT);

INSERT INTO t
SELECT
    random() * 1000000,
    random() * 10000,
    md5(random()::text)
FROM generate_series(1, 1000000);
```

### 创建索引

使用 ePQ 创建索引需要以下三个步骤：

1. 设置参数 `polar_enable_px` 为 `ON`，打开 ePQ 的开关
2. 按需设置参数 `polar_px_dop_per_node` 调整查询并行度
3. 在创建索引时显式声明 `px_build` 属性为 `ON`

```sql:no-line-numbers
SET polar_enable_px TO ON;
SET polar_px_dop_per_node TO 8;
CREATE INDEX t_idx1 ON t(id, msg) WITH(px_build = ON);
```

在创建索引的过程中，数据库会对正在创建索引的表施加 [`ShareLock`](https://www.postgresql.org/docs/current/explicit-locking.html#LOCKING-TABLES) 锁。这个级别的锁将会阻塞其它进程对表的 DML 操作（`INSERT` / `UPDATE` / `DELETE`）。

### 并发创建索引

类似地，ePQ 支持并发创建索引，只需要在 `CREATE INDEX` 后加上 `CONCURRENTLY` 关键字即可：

```sql:no-line-numbers
SET polar_enable_px TO ON;
SET polar_px_dop_per_node TO 8;
CREATE INDEX CONCURRENTLY t_idx2 ON t(id, msg) WITH(px_build = ON);
```

在创建索引的过程中，数据库会对正在创建索引的表施加 [`ShareUpdateExclusiveLock`](https://www.postgresql.org/docs/current/explicit-locking.html#LOCKING-TABLES) 锁。这个级别的锁将不会阻塞其它进程对表的 DML 操作。

## 使用限制

ePQ 加速创建索引暂不支持以下场景：

- 创建 `UNIQUE` 索引
- 创建索引时附带 `INCLUDING` 列
- 创建索引时指定 `TABLESPACE`
- 创建索引时带有 `WHERE` 而成为部分索引（Partial Index）
