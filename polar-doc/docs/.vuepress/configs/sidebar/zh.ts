import type { SidebarConfig } from "@vuepress/theme-default";

export const zh: SidebarConfig = {
  "/zh/deploying": [
    {
      text: "部署指南",
      children: [
        "/zh/deploying/introduction.md",
        "/zh/deploying/quick-start.md",
        {
          text: "进阶部署",
          link: "/zh/deploying/deploy.md",
          children: [
            {
              text: "共享存储设备的准备",
              children: ["/zh/deploying/storage-aliyun-essd.md"],
            },
            {
              text: "文件系统的准备",
              children: ["/zh/deploying/fs-pfs.md"],
            },
            {
              text: "部署 PolarDB-PG 数据库",
              children: [
                "/zh/deploying/db-localfs.md",
                "/zh/deploying/db-pfs.md",
              ],
            },
          ],
        },
        {
          text: "更多部署方式",
          children: ["/zh/deploying/deploy-official.md"],
        },
      ],
    },
  ],
  "/zh/operation/": [
    {
      text: "使用与运维",
      children: [
        {
          text: "日常运维",
          children: [
            "/zh/operation/backup-and-restore.md",
            "/zh/operation/grow-storage.md",
            "/zh/operation/scale-out.md",
            "/zh/operation/ro-online-promote.md",
          ],
        },
        {
          text: "问题诊断",
          children: ["/zh/operation/cpu-usage-high.md"],
        },
        {
          text: "性能测试",
          children: [
            "/zh/operation/tpcc-test.md",
            "/zh/operation/tpch-test.md",
          ],
        },
      ],
    },
  ],
  "/zh/features": [
    {
      text: "自研功能",
      link: "/zh/features/",
      children: [
        {
          text: "功能分类",
          children: [
            {
              text: "高性能",
              link: "/zh/features/performance/",
              children: [
                "/zh/features/performance/bulk-read-and-extend.md",
                "/zh/features/performance/rel-size-cache.md",
                "/zh/features/performance/shared-server.md",
              ],
            },
            {
              text: "高可用",
              link: "/zh/features/availability/",
              children: [
                "/zh/features/availability/avail-online-promote.md",
                "/zh/features/availability/avail-parallel-replay.md",
                "/zh/features/availability/datamax.md",
                "/zh/features/availability/resource-manager.md",
                "/zh/features/availability/flashback-table.md",
              ],
            },
            {
              text: "安全",
              link: "/zh/features/security/",
              children: ["/zh/features/security/tde.md"],
            },
            {
              text: "弹性跨机并行查询（ePQ）",
              link: "/zh/features/epq/",
              children: [
                "/zh/features/epq/epq-explain-analyze.md",
                "/zh/features/epq/epq-node-and-dop.md",
                "/zh/features/epq/epq-partitioned-table.md",
                "/zh/features/epq/epq-create-btree-index.md",
                "/zh/features/epq/cluster-info.md",
                "/zh/features/epq/adaptive-scan.md",
                "/zh/features/epq/parallel-dml.md",
                "/zh/features/epq/epq-ctas-mtview-bulk-insert.md",
              ],
            },
            {
              text: "第三方插件",
              link: "/zh/features/extensions/",
              children: [
                "/zh/features/extensions/pgvector.md",
                "/zh/features/extensions/smlar.md",
              ],
            },
          ],
        },
      ],
    },
  ],
  "/zh/theory/": [
    {
      text: "原理解读",
      children: [
        {
          text: "PolarDB for PostgreSQL",
          children: [
            "/zh/theory/arch-overview.md",
            "/zh/theory/arch-htap.md",
            "/zh/theory/buffer-management.md",
            "/zh/theory/ddl-synchronization.md",
            "/zh/theory/logindex.md",
          ],
        },
        {
          text: "PostgreSQL",
          children: [
            "/zh/theory/analyze.md",
            "/zh/theory/polar-sequence-tech.md",
          ],
        },
      ],
    },
  ],
  "/zh/development/": [
    {
      text: "上手开发",
      children: [
        "/zh/development/dev-on-docker.md",
        "/zh/development/customize-dev-env.md",
      ],
    },
  ],
  "/zh/contributing": [
    {
      text: "参与社区",
      children: [
        "/zh/contributing/contributing-polardb-kernel.md",
        "/zh/contributing/contributing-polardb-docs.md",
        "/zh/contributing/coding-style.md",
        "/zh/contributing/trouble-issuing.md",
      ],
    },
  ],
};
