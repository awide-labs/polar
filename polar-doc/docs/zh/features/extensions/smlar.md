---
author: 棠羽
date: 2022/10/05
minute: 10
---

# smlar

<Badge type="tip" text="V11 / v1.1.28-" vertical="top" />

<ArticleInfo :frontmatter=$frontmatter></ArticleInfo>

[[toc]]

## 背景

对大规模的数据进行相似度计算在电商业务、搜索引擎中是一个很关键的技术问题。相对简易的相似度计算实现不仅运算速度慢，还十分消耗资源。[`smlar`](https://github.com/jirutka/smlar) 是 PostgreSQL 的一款开源第三方插件，提供了可以在数据库内高效计算数据相似度的函数，并提供了支持 GiST 和 GIN 索引的相似度运算符。目前该插件已经支持 PostgreSQL 所有的内置数据类型。

::: warning
由于 smlar 插件的 `%` 操作符与 RUM 插件的 `%` 操作符冲突，因此 smlar 与 RUM 两个插件无法同时创建在同一 schema 中。
:::

## 函数及运算符介绍

- **`float4 smlar(anyarray, anyarray)`**

  计算两个数组的相似度，数组的数据类型需要一致。

- **`float4 smlar(anyarray, anyarray, bool useIntersect)`**

  计算两个自定义复合类型数组的相似度，`useIntersect` 参数表示是否让仅重叠元素还是全部元素参与运算；复合类型可由以下方式定义：

  ```sql:no-line-numbers
  CREATE TYPE type_name AS (element_name anytype, weight_name FLOAT4);
  ```

- **`float4 smlar(anyarray a, anyarray b, text formula);`**

  使用参数给定的公式来计算两个数组的相似度，数组的数据类型需要一致；公式中可以使用的预定义变量有：

  - `N.i`：两个数组中的相同元素个数（交集）
  - `N.a`：第一个数组中的唯一元素个数
  - `N.b`：第二个数组中的唯一元素个数

  ```sql:no-line-numbers
  SELECT smlar('{1,4,6}'::int[], '{5,4,6}', 'N.i / sqrt(N.a * N.b)');
  ```

- **`anyarray % anyarray`**

  该运算符的含义为，当两个数组的的相似度超过阈值时返回 `TRUE`，否则返回 `FALSE`。

- **`text[] tsvector2textarray(tsvector)`**

  将 `tsvector` 类型转换为字符串数组。

- **`anyarray array_unique(anyarray)`**

  对数组进行排序、去重。

- **`float4 inarray(anyarray, anyelement)`**

  如果元素出现在数组中，则返回 `1.0`；否则返回 `0`。

- **`float4 inarray(anyarray, anyelement, float4, float4)`**

  如果元素出现在数组中，则返回第三个参数；否则返回第四个参数。

## 可配置参数说明

- **`smlar.threshold FLOAT`**

  相似度阈值，用于给 `%` 运算符判断两个数组是否相似。

- **`smlar.persistent_cache BOOL`**

  全局统计信息的缓存是否存放在与事务无关的内存中。

- **`smlar.type STRING`**：相似度计算公式，可选的相似度类型包含：

  - [cosine](https://en.wikipedia.org/wiki/Cosine_similarity)（默认）
  - [tfidf](https://zh.wikipedia.org/zh-cn/Tf-idf)
  - [overlap](https://en.wikipedia.org/wiki/Overlap_coefficient)

- **`smlar.stattable STRING`**

  存储集合范围统计信息的表名，表定义如下：

  ```sql:no-line-numbers
  CREATE TABLE table_name (
    value   data_type UNIQUE,
    ndoc    int4 (or bigint)  NOT NULL CHECK (ndoc>0)
  );
  ```

- **`smlar.tf_method STRING`**：计算词频（TF，Term Frequency）的方法，取值如下

  - `n`：简单计数（默认）
  - `log`：`1 + log(n)`
  - `const`：频率等于 `1`

- **`smlar.idf_plus_one BOOL`**：计算逆文本频率指数的方法（IDF，Inverse Document Frequency）的方法，取值如下

  - `FALSE`：`log(d / df)`（默认）
  - `TRUE`：`log(1 + d / df)`

## 基本使用方法

### 安装插件

```sql:no-line-numbers
CREATE EXTENSION smlar;
```

### 相似度计算

使用上述的函数计算两个数组的相似度：

```sql
SELECT smlar('{3,2}'::int[], '{3,2,1}');
  smlar
----------
 0.816497
(1 row)

SELECT smlar('{1,4,6}'::int[], '{5,4,6}', 'N.i / (N.a + N.b)' );
  smlar
----------
 0.333333
(1 row)
```

### 卸载插件

```sql:no-line-numbers
DROP EXTENSION smlar;
```

## 原理和设计

[GitHub - jirutka/smlar](https://github.com/jirutka/smlar)

[PGCon 2012 - Finding Similar: Effective similarity search in database](https://www.pgcon.org/2012/schedule/track/Hacking/443.en.html) ([slides](https://www.pgcon.org/2012/schedule/attachments/252_smlar-2012.pdf))
