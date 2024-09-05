# 自研功能

- [PolarDB for PostgreSQL 11](./v11/README.md)

## 功能 / 版本映射矩阵

<table>
<thead>
<tr>
<th>功能 / 版本</th>
<th style="text-align:center">PostgreSQL</th>
<th style="text-align:center">PolarDB for PostgreSQL 11</th>
</tr>
</thead>
<tbody>
<tr>
<td><strong>高性能</strong></td>
<td style="text-align:center">...</td>
<td style="text-align:center"><a href="./v11/performance/">...</a></td>
</tr>
<tr>
<td>预读 / 预扩展</td>
<td style="text-align:center">/</td>
<td style="text-align:center"><a href="./v11/performance/bulk-read-and-extend.html"><Badge type="tip" text="V11 / v1.1.1-" vertical="top" /></a></td>
</tr>
<tr>
<td>表大小缓存</td>
<td style="text-align:center">/</td>
<td style="text-align:center"><a href="./v11/performance/rel-size-cache.html"><Badge type="tip" text="V11 / v1.1.10-" vertical="top" /></a></td>
</tr>
<tr>
<td>Shared Server</td>
<td style="text-align:center">/</td>
<td style="text-align:center"><a href="./v11/performance/shared-server.html"><Badge type="tip" text="V11 / v1.1.30-" vertical="top" /></a></td>
</tr>
<tr>
<td><strong>高可用</strong></td>
<td style="text-align:center">...</td>
<td style="text-align:center"><a href="./v11/availability/">...</a></td>
</tr>
<tr>
<td>只读节点 Online Promote</td>
<td style="text-align:center">/</td>
<td style="text-align:center"><a href="./v11/availability/avail-online-promote.html"><Badge type="tip" text="V11 / v1.1.1-" vertical="top" /></a></td>
</tr>
<tr>
<td>WAL 日志并行回放</td>
<td style="text-align:center">/</td>
<td style="text-align:center"><a href="./v11/availability/avail-parallel-replay.html"><Badge type="tip" text="V11 / v1.1.17-" vertical="top" /></a></td>
</tr>
<tr>
<td>DataMax 日志节点</td>
<td style="text-align:center">/</td>
<td style="text-align:center"><a href="./v11/availability/datamax.html"><Badge type="tip" text="V11 / v1.1.6-" vertical="top" /></a></td>
</tr>
<tr>
<td>Resource Manager</td>
<td style="text-align:center">/</td>
<td style="text-align:center"><a href="./v11/availability/resource-manager.html"><Badge type="tip" text="V11 / v1.1.1-" vertical="top" /></a></td>
</tr>
<tr>
<td>闪回表和闪回日志</td>
<td style="text-align:center">/</td>
<td style="text-align:center"><a href="./v11/availability/flashback-table.html"><Badge type="tip" text="V11 / v1.1.22-" vertical="top" /></a></td>
</tr>
<tr>
<td><strong>安全</strong></td>
<td style="text-align:center">...</td>
<td style="text-align:center"><a href="./v11/security/">...</a></td>
</tr>
<tr>
<td>透明数据加密</td>
<td style="text-align:center">/</td>
<td style="text-align:center"><a href="./v11/security/tde.html"><Badge type="tip" text="V11 / v1.1.1-" vertical="top" /></a></td>
</tr>
<tr>
<td><strong>弹性跨机并行查询（ePQ）</strong></td>
<td style="text-align:center">...</td>
<td style="text-align:center"><a href="./v11/epq/">...</a></td>
</tr>
<tr>
<td>ePQ 执行计划查看与分析</td>
<td style="text-align:center">/</td>
<td style="text-align:center"><a href="./v11/epq/epq-explain-analyze.html"><Badge type="tip" text="V11 / v1.1.22-" vertical="top" /></a></td>
</tr>
<tr>
<td>ePQ 计算节点范围选择与并行度控制</td>
<td style="text-align:center">/</td>
<td style="text-align:center"><a href="./v11/epq/epq-node-and-dop.html"><Badge type="tip" text="V11 / v1.1.20-" vertical="top" /></a></td>
</tr>
<tr>
<td>ePQ 支持分区表查询</td>
<td style="text-align:center">/</td>
<td style="text-align:center"><a href="./v11/epq/epq-partitioned-table.html"><Badge type="tip" text="V11 / v1.1.17-" vertical="top" /></a></td>
</tr>
<tr>
<td>ePQ 支持创建 B-Tree 索引并行加速</td>
<td style="text-align:center">/</td>
<td style="text-align:center"><a href="./v11/epq/epq-create-btree-index.html"><Badge type="tip" text="V11 / v1.1.15-" vertical="top" /></a></td>
</tr>
<tr>
<td>集群拓扑视图</td>
<td style="text-align:center">/</td>
<td style="text-align:center"><a href="./v11/epq/cluster-info.html"><Badge type="tip" text="V11 / v1.1.20-" vertical="top" /></a></td>
</tr>
<tr>
<td>自适应扫描</td>
<td style="text-align:center">/</td>
<td style="text-align:center"><a href="./v11/epq/adaptive-scan.html"><Badge type="tip" text="V11 / v1.1.17-" vertical="top" /></a></td>
</tr>
<tr>
<td>并行 INSERT</td>
<td style="text-align:center">/</td>
<td style="text-align:center"><a href="./v11/epq/parallel-dml.html"><Badge type="tip" text="V11 / v1.1.17-" vertical="top" /></a></td>
</tr>
<tr>
<td>ePQ 支持创建/刷新物化视图并行加速和批量写入</td>
<td style="text-align:center">/</td>
<td style="text-align:center"><a href="./v11/epq/epq-ctas-mtview-bulk-insert.html"><Badge type="tip" text="V11 / v1.1.30-" vertical="top" /></a></td>
</tr>
<tr>
<td><strong>第三方插件</strong></td>
<td style="text-align:center">...</td>
<td style="text-align:center"><a href="./v11/extensions/">...</a></td>
</tr>
<tr>
<td>pgvector</td>
<td style="text-align:center">/</td>
<td style="text-align:center"><a href="./v11/extensions/pgvector.html"><Badge type="tip" text="V11 / v1.1.35-" vertical="top" /></a></td>
</tr>
<tr>
<td>smlar</td>
<td style="text-align:center">/</td>
<td style="text-align:center"><a href="./v11/extensions/smlar.html"><Badge type="tip" text="V11 / v1.1.35-" vertical="top" /></a></td>
</tr>
</tbody>
</table>
