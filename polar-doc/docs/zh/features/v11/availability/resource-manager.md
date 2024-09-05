---
author: 学有
date: 2022/11/25
minute: 20
---

# Resource Manager

<Badge type="tip" text="V11 / v1.1.1-" vertical="top" />

<ArticleInfo :frontmatter=$frontmatter></ArticleInfo>

[[toc]]

## 背景

PolarDB for PostgreSQL 的内存可以分为以下三部分：

- 共享内存
- 进程间动态共享内存
- 进程私有内存

进程间动态共享内存和进程私有内存是 **动态分配** 的，其使用量随着实例承载的业务运行情况而不断变化。过多使用动态内存，可能会导致内存使用量超过操作系统限制，触发内核内存限制机制，造成实例进程异常退出，实例重启，引发实例不可用的问题。

进程私有内存 MemoryContext 管理的内存可以分为两部分：

- 工作计算区域内存：业务运行所需的内存，此部分内存会影响业务的正常运行；
- Cache 内存：数据库会把部分内部元数据存放在进程内，此部分内存只会影响数据库性能；

## 目标

为了解决以上问题，PolarDB for PostgreSQL 增加了 **Resource Manager** 资源限制机制，能够在实例运行期间，周期性检测资源使用情况。对于超过资源限制阈值的进程，强制进行资源限制，降低实例不可用的风险。

Resource Manager 主要的限制资源有：

- 内存
- CPU
- I/O

当前仅支持对内存资源进行限制。

## 内存限制原理

内存限制依赖 Cgroup，如果不存在 Cgroup，则无法有效进行资源限制。Resource Manager 作为 PolarDB for PostgreSQL 一个后台辅助进程，周期性读取 Cgroup 的内存使用数据作为内存限制的依据。当发现存在进程超过内存限制阈值后，会读取内核的用户进程内存记账，按照内存大小排序，依次对内存使用量超过阈值的进程发送中断进程信号（SIGTERM）或取消操作信号（SIGINT）。

### 内存限制方式

Resource Manager 守护进程会随着实例启动而建立，同时对 RW、RO 以及 Standby 节点起作用。可以通过修改参数改变 Resource Manager 的行为。

- `enable_resource_manager`：是否启动 Resource Manager，取值为 `on` / `off`，默认值为 `on`
- `stat_interval`：资源使用量周期检测的间隔，单位为毫秒，取值范围为 `10`-`10000`，默认值为 `500`
- `total_mem_limit_rate`：限制实例内存使用的百分比，当实例内存使用超过该百分比后，开始强制对内存资源进行限制，默认值为 `95`
- `total_mem_limit_remain_size`：实例内存预留值，当实例空闲内存小于预留值后，开始强制对内存资源进行限制，单位为 kB，取值范围为 `131072`-`MAX_KILOBYTES`（整型数值最大值），默认值为 `524288`
- `mem_release_policy`：内存资源限制的策略
  - `none`：无动作
  - `default`：缺省策略（默认值），优先中断空闲进程，然后中断活跃进程
  - `cancel_query`：中断活跃进程
  - `terminate_idle_backend`：中断空闲进程
  - `terminate_any_backend`：中断所有进程
  - `terminate_random_backend`：中断随机进程

### 内存限制效果

```log:no-line-numbers
2022-11-28 14:07:56.929 UTC [18179] LOG:  [polar_resource_manager] terminate process 13461 release memory 65434123 bytes
2022-11-28 14:08:17.143 UTC [35472] FATAL:  terminating connection due to out of memory
2022-11-28 14:08:17.143 UTC [35472] BACKTRACE:
        postgres: primary: postgres postgres [local] idle(ProcessInterrupts+0x34c) [0xae5fda]
        postgres: primary: postgres postgres [local] idle(ProcessClientReadInterrupt+0x3a) [0xae1ad6]
        postgres: primary: postgres postgres [local] idle(secure_read+0x209) [0x8c9070]
        postgres: primary: postgres postgres [local] idle() [0x8d4565]
        postgres: primary: postgres postgres [local] idle(pq_getbyte+0x30) [0x8d4613]
        postgres: primary: postgres postgres [local] idle() [0xae1861]
        postgres: primary: postgres postgres [local] idle() [0xae1a83]
        postgres: primary: postgres postgres [local] idle(PostgresMain+0x8df) [0xae7949]
        postgres: primary: postgres postgres [local] idle() [0x9f4c4c]
        postgres: primary: postgres postgres [local] idle() [0x9f440c]
        postgres: primary: postgres postgres [local] idle() [0x9ef963]
        postgres: primary: postgres postgres [local] idle(PostmasterMain+0x1321) [0x9ef18a]
        postgres: primary: postgres postgres [local] idle() [0x8dc1f6]
        /lib64/libc.so.6(__libc_start_main+0xf5) [0x7f888afff445]
        postgres: primary: postgres postgres [local] idle() [0x49d209]
```
