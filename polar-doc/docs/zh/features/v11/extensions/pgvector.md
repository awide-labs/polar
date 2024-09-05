---
author: 山现
date: 2023/12/25
minute: 10
---

# pgvector

<Badge type="tip" text="V11 / v1.1.35-" vertical="top" />

<ArticleInfo :frontmatter=$frontmatter></ArticleInfo>

[[toc]]

## 背景

[`pgvector`](https://github.com/pgvector/pgvector) 作为一款高效的向量数据库插件，基于 PostgreSQL 的扩展机制，利用 C 语言实现了多种向量数据类型和运算算法，同时还能够高效存储与查询以向量表示的 AI Embedding。

`pgvector` 支持 IVFFlat 索引。IVFFlat 索引能够将向量空间分为若干个划分区域，每个区域都包含一些向量，并创建倒排索引，用于快速地查找与给定向量相似的向量。IVFFlat 是 IVFADC 索引的简化版本，适用于召回精度要求高，但对查询耗时要求不严格（100ms 级别）的场景。相比其他索引类型，IVFFlat 索引具有高召回率、高精度、算法和参数简单、空间占用小的优势。

`pgvector` 插件算法的具体流程如下：

1. 高维空间中的点基于隐形的聚类属性，按照 K-Means 等聚类算法对向量进行聚类处理，使得每个类簇有一个中心点
2. 检索向量时首先遍历计算所有类簇的中心点，找到与目标向量最近的 n 个类簇中心
3. 遍历计算 n 个类簇中心所在聚类中的所有元素，经过全局排序得到距离最近的 k 个向量

## 使用方法

`pgvector` 可以顺序检索或索引检索高维向量，关于索引类型和更多参数介绍可以参考插件源代码的 [README](https://github.com/pgvector/pgvector/blob/master/README.md)。

### 安装插件

```sql:no-line-numbers
CREATE EXTENSION vector;
```

### 向量操作

执行如下命令，创建一个含有向量字段的表：

```sql:no-line-numbers
CREATE TABLE t (val vector(3));
```

执行如下命令，可以插入向量数据：

```sql:no-line-numbers
INSERT INTO t (val) VALUES ('[0,0,0]'), ('[1,2,3]'), ('[1,1,1]'), (NULL);
```

创建 IVFFlat 类型的索引：

1. `val vector_ip_ops` 表示需要创建索引的列名为 `val`，并且使用向量操作符 `vector_ip_ops` 来计算向量之间的相似度。该操作符支持向量之间的点积、余弦相似度、欧几里得距离等计算方式
2. `WITH (lists = 1)` 表示使用的划分区域数量为 1，这意味着所有向量都将被分配到同一个区域中。在实际应用中，划分区域数量需要根据数据规模和查询性能进行调整

```sql:no-line-numbers
CREATE INDEX ON t USING ivfflat (val vector_ip_ops) WITH (lists = 1);
```

计算近似向量：

```sql:no-line-numbers
=> SELECT * FROM t ORDER BY val <#> '[3,3,3]';
   val
---------
 [1,2,3]
 [1,1,1]
 [0,0,0]

(4 rows)
```

### 卸载插件

```sql:no-line-numbers
DROP EXTENSION vector;
```

## 注意事项

- [ePQ](../epq/README.md) 支持通过排序遍历高维向量，不支持通过索引查询向量类型
