---
author: 杨思南
date: 2025/04/21
minute: 60
---

# polar_login_history 会话访问历史

<Badge type="tip" text="V15 / v15.12.4.0-" vertical="top" />

<ArticleInfo :frontmatter=$frontmatter></ArticleInfo>

[[toc]]

## 背景

会话访问历史功能，在用户与数据库成功建立会话后，向用户显示其最近一次的访问历史记录，包括日期、时间、IP、应用程序、自上次成功访问以来的失败访问次数。通过这些信息帮助用户检测异常访问行为，比如来自陌生IP或应用程序的访问，有助于及时发现账户盗用或恶意访问等潜在的安全威胁。

## 使用

对于用户来说：

- 安装插件

  ```bash:no-line-numbers
  cd external/polar_login_history
  make
  make install
  ```

- 在数据库运行过程中，可以修改配置参数 `shared_preload_libraries` 来加载动态库 `polar_login_history` ，这在重新启动数据库后生效。

  ```sql:no-line-numbers
  alter system set shared_preload_libraries = polar_login_history;
  ```

- 在数据库运行过程中，可以修改配置参数 `polar_login_history.enable` 来启用会话访问历史功能，这在重新启动数据库后生效。

  ```sql:no-line-numbers
  alter system set polar_login_history.enable = on;
  ```

- 执行 `psql` 成功登录数据库后，根据用户是否为首次登录将分别展示如下信息：

  ```bash:no-line-numbers
  psql -h [Primary节点所在IP] -p [Primary节点所在端口号] -d postgres -U [用户名] -c "select 1;"
  INFO:
  First login: 2025-04-21 11:46:54.46897+08 from 127.0.0.1 using psql
   ?column?
  ----------
          1
  (1 row)
  ```

  ```bash:no-line-numbers
  psql -h [Primary节点所在IP] -p [Primary节点所在端口号] -d postgres -U [用户名] -c "select 1;"
  INFO:
  Last login: 2025-04-21 11:46:54.46897+08 from 127.0.0.1 using psql
  The number of failures since the last successful login is 0
   ?column?
  ----------
          1
  (1 row)
  ```

## 原理

### 内存结构

数据库为每个用户维护一条会话访问历史信息，数据结构如下：

```c
typedef struct polar_login_history_info
{
	Oid			useroid;		/* user oid */
	TimestampTz logintime;		/* last login time */
	char		ip[POLAR_LOGIN_HISTORY_STRING_LENGTH];	/* last login ip */
	char		application[POLAR_LOGIN_HISTORY_STRING_LENGTH]; /* the application used
																 * for the last login */
	uint8		failcount;		/* the number of failed login attempts since
								 * the last successful login */
}			polar_login_history_info;
```

所有用户的会话访问历史信息以数组的形式保存在共享内存中。对于给定用户，通过用户OID和数组下标的映射关系，能够查询到该用户的访问历史信息，这种映射关系以哈希表的形式保存在共享内存中。这两种数据结构的大小由配置参数 `polar_login_history.maxnum` 决定，结构如下：

```c
static polar_login_history_array_struct * polar_login_history_array = NULL;
static HTAB * polar_login_history_hash = NULL;
```

对于数组，使用自旋锁机制以保护共享数据，避免同一数组元素的并发访问。对于哈希表，使用分区锁机制以减少并发访问的锁冲突，减少对并发性能的影响。

### 持久化

会话访问历史信息保存在 `polar_login_history` 文件中。

在数据库启动过程中，将读取该文件内容并填充至数组 `polar_login_history_array` 和哈希表 `polar_login_history_hash` 中。
在数据库关闭过程中，将数组 `polar_login_history_array` 信息写入文件，此外 `polar_worker` 进程也会定期写入会话访问历史信息，以防数据库异常崩溃造成数据丢失。

### 更新时机

当数据库调用函数 `PostgresMain` 处理客户端连接请求时，认为用户开始尝试与数据库建立会话，直到 `for (;;)` 循环监听客户端请求，此时认为会话建立成功，将成功的会话信息更新至共享内存中，若该过程中出现错误，则在错误处理前将失败的会话信息更新至共享内存中。
