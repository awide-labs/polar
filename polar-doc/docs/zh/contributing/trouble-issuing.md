# 问题报告

如果在运行 PolarDB for PostgreSQL 的过程中出现问题，请提供数据库的日志与机器的配置信息以方便定位问题。

通过 `polar_stat_env` 插件可以轻松获取数据库所在主机的硬件配置：

```sql:no-line-numbers
=> CREATE EXTENSION polar_stat_env;
=> SELECT polar_stat_env();
                           polar_stat_env
--------------------------------------------------------------------
 {                                                                 +
   "Role": "Primary",                                              +
   "CPU": {                                                        +
     "Architecture": "x86_64",                                     +
     "Model Name": "Intel(R) Xeon(R) Platinum 8369B CPU @ 2.70GHz",+
     "CPU Cores": "8",                                             +
     "CPU Thread Per Cores": "2",                                  +
     "CPU Core Per Socket": "4",                                   +
     "NUMA Nodes": "1",                                            +
     "L1d cache": "192 KiB (4 instances)",                         +
     "L1i cache": "128 KiB (4 instances)",                         +
     "L2 cache": "5 MiB (4 instances)",                            +
     "L3 cache": "48 MiB (1 instance)"                             +
   },                                                              +
   "Memory": {                                                     +
     "Memory Total (GB)": "14",                                    +
     "HugePage Size (MB)": "2",                                    +
     "HugePage Total Size (GB)": "0"                               +
   },                                                              +
   "OS Params": {                                                  +
     "OS": "5.10.134-16.1.al8.x86_64",                             +
     "Swappiness(1-100)": "0",                                     +
     "Vfs Cache Pressure(0-1000)": "100",                          +
     "Min Free KBytes(KB)": "67584"                                +
   }                                                               +
 }
(1 row)
```

通过 ePQ 功能可以直接获取整个集群中所有计算节点的硬件配置信息：

```sql:no-line-numbers
=> CREATE EXTENSION polar_stat_env;
=> SET polar_enable_px TO ON;
=> SET polar_px_use_master TO ON;
=> SET polar_px_use_standby TO ON;
=> SELECT * FROM polar_global_function('polar_stat_env');
                           polar_stat_env
---------------------------------------------------------------------
 {                                                                  +
   "Role": "Standby",                                               +
   "CPU": {                                                         +
     "Architecture": "x86_64",                                      +
     "Model Name": "Intel(R) Xeon(R) Platinum 8269CY CPU @ 2.50GHz",+
     "CPU Cores": "104",                                            +
     "CPU Thread Per Cores": "2",                                   +
     "CPU Core Per Socket": "26",                                   +
     "NUMA Nodes": "2",                                             +
     "L1d cache": "32K",                                            +
     "L1i cache": "32K",                                            +
     "L2 cache": "1024K",                                           +
     "L3 cache": "36608K"                                           +
   },                                                               +
   "Memory": {                                                      +
     "Memory Total (GB)": "754",                                    +
     "HugePage Size (MB)": "2",                                     +
     "HugePage Total Size (GB)": "42"                               +
   },                                                               +
   "OS Params": {                                                   +
     "OS": "5.10.134-16.1.al8.x86_64",                              +
     "Swappiness(1-100)": "0",                                      +
     "Vfs Cache Pressure(0-1000)": "500",                           +
     "Min Free KBytes(KB)": "20971520"                              +
   }                                                                +
 }
 {                                                                  +
   "Role": "Replica",                                               +
   "CPU": {                                                         +
     "Architecture": "x86_64",                                      +
     "Model Name": "Intel(R) Xeon(R) Platinum 8269CY CPU @ 2.50GHz",+
     "CPU Cores": "104",                                            +
     "CPU Thread Per Cores": "2",                                   +
     "CPU Core Per Socket": "26",                                   +
     "NUMA Nodes": "2",                                             +
     "L1d cache": "32K",                                            +
     "L1i cache": "32K",                                            +
     "L2 cache": "1024K",                                           +
     "L3 cache": "36608K"                                           +
   },                                                               +
   "Memory": {                                                      +
     "Memory Total (GB)": "754",                                    +
     "HugePage Size (MB)": "2",                                     +
     "HugePage Total Size (GB)": "42"                               +
   },                                                               +
   "OS Params": {                                                   +
     "OS": "5.10.134-16.1.al8.x86_64",                              +
     "Swappiness(1-100)": "0",                                      +
     "Vfs Cache Pressure(0-1000)": "500",                           +
     "Min Free KBytes(KB)": "20971520"                              +
   }                                                                +
 }
 {                                                                  +
   "Role": "Primary",                                               +
   "CPU": {                                                         +
     "Architecture": "x86_64",                                      +
     "Model Name": "Intel(R) Xeon(R) Platinum 8269CY CPU @ 2.50GHz",+
     "CPU Cores": "104",                                            +
     "CPU Thread Per Cores": "2",                                   +
     "CPU Core Per Socket": "26",                                   +
     "NUMA Nodes": "2",                                             +
     "L1d cache": "32K",                                            +
     "L1i cache": "32K",                                            +
     "L2 cache": "1024K",                                           +
     "L3 cache": "36608K"                                           +
   },                                                               +
   "Memory": {                                                      +
     "Memory Total (GB)": "754",                                    +
     "HugePage Size (MB)": "2",                                     +
     "HugePage Total Size (GB)": "42"                               +
   },                                                               +
   "OS Params": {                                                   +
     "OS": "5.10.134-16.1.al8.x86_64",                              +
     "Swappiness(1-100)": "0",                                      +
     "Vfs Cache Pressure(0-1000)": "500",                           +
     "Min Free KBytes(KB)": "20971520"                              +
   }                                                                +
 }
 {                                                                  +
   "Role": "Replica",                                               +
   "CPU": {                                                         +
     "Architecture": "x86_64",                                      +
     "Model Name": "Intel(R) Xeon(R) Platinum 8269CY CPU @ 2.50GHz",+
     "CPU Cores": "104",                                            +
     "CPU Thread Per Cores": "2",                                   +
     "CPU Core Per Socket": "26",                                   +
     "NUMA Nodes": "2",                                             +
     "L1d cache": "32K",                                            +
     "L1i cache": "32K",                                            +
     "L2 cache": "1024K",                                           +
     "L3 cache": "36608K"                                           +
   },                                                               +
   "Memory": {                                                      +
     "Memory Total (GB)": "754",                                    +
     "HugePage Size (MB)": "2",                                     +
     "HugePage Total Size (GB)": "42"                               +
   },                                                               +
   "OS Params": {                                                   +
     "OS": "5.10.134-16.1.al8.x86_64",                              +
     "Swappiness(1-100)": "0",                                      +
     "Vfs Cache Pressure(0-1000)": "500",                           +
     "Min Free KBytes(KB)": "20971520"                              +
   }                                                                +
 }
(4 rows)
```
