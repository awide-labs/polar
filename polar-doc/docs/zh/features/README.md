# 自研功能

## 功能分类

- [高性能](./performance/README.md)
- [高可用](./availability/README.md)
- [安全](./security/README.md)
- [弹性跨机并行查询（ePQ）](./epq/README.md)
- [第三方插件](./extensions/README.md)

## 功能导览

### 高可用

- [只读节点 Online Promote](./availability/avail-online-promote.md) <Badge type="tip" text="V11 / v1.1.1-" vertical="top" />
- [WAL 日志并行回放](./availability/avail-parallel-replay.md) <Badge type="tip" text="V11 / v1.1.17-" vertical="top" />
- [DataMax 日志节点](./availability/datamax.md) <Badge type="tip" text="V11 / v1.1.6-" vertical="top" />
- [闪回表和闪回日志](./availability/flashback-table.md) <Badge type="tip" text="V11 / v1.1.22-" vertical="top" />
- [Resource Manager](./availability/resource-manager.md) <Badge type="tip" text="V11 / v1.1.1-" vertical="top" />

### 弹性跨机并行查询（ePQ）

- [自适应扫描](./epq/adaptive-scan.md) <Badge type="tip" text="V11 / v1.1.17-" vertical="top" />
- [集群拓扑视图](./epq/cluster-info.md) <Badge type="tip" text="V11 / v1.1.20-" vertical="top" />
- [ePQ 支持创建 B-Tree 索引并行加速](./epq/epq-create-btree-index.md) <Badge type="tip" text="V11 / v1.1.15-" vertical="top" />
- [ePQ 支持创建/刷新物化视图并行加速和批量写入](./epq/epq-ctas-mtview-bulk-insert.md) <Badge type="tip" text="V11 / v1.1.30-" vertical="top" />
- [ePQ 执行计划查看与分析](./epq/epq-explain-analyze.md) <Badge type="tip" text="V11 / v1.1.20-" vertical="top" />
- [ePQ 计算节点范围选择与并行度控制](./epq/epq-node-and-dop.md) <Badge type="tip" text="V11 / v1.1.20-" vertical="top" />
- [ePQ 支持分区表查询](./epq/epq-partitioned-table.md) <Badge type="tip" text="V11 / v1.1.17-" vertical="top" />
- [并行 INSERT](./epq/parallel-dml.md) <Badge type="tip" text="V11 / v1.1.17-" vertical="top" />

### 第三方插件

- [pgvector](./extensions/pgvector.md) <Badge type="tip" text="V11 / v1.1.35-" vertical="top" />
- [smlar](./extensions/smlar.md) <Badge type="tip" text="V11 / v1.1.28-" vertical="top" />

### 高性能

- [预读 / 预扩展](./performance/bulk-read-and-extend.md) <Badge type="tip" text="V11 / v1.1.1-" vertical="top" />
- [表大小缓存](./performance/rel-size-cache.md) <Badge type="tip" text="V11 / v1.1.10-" vertical="top" />
- [Shared Server](./performance/shared-server.md) <Badge type="tip" text="V11 / v1.1.30-" vertical="top" />

### 安全

- [polar_login_history 会话访问历史](./security/polar_login_history.md) <Badge type="tip" text="V15 / v15.12.4.0-" vertical="top" />
- [TDE 透明数据加密](./security/tde.md) <Badge type="tip" text="V11 / v1.1.1-" vertical="top" />
