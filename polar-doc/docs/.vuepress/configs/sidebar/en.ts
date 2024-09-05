import type { SidebarConfig } from "@vuepress/theme-default";

export const en: SidebarConfig = {
  "/deploying": [
    {
      text: "Deployment",
      children: [
        "/deploying/introduction.md",
        "/deploying/quick-start.md",
        {
          text: "Advanced Deployment",
          link: "/deploying/deploy.md",
          children: [
            {
              text: "Preparation of Shared-Storage Device",
              children: ["/deploying/storage-aliyun-essd.md"],
            },
            {
              text: "Preparation of File System",
              children: ["/deploying/fs-pfs.md"],
            },
            {
              text: "Deploying PolarDB",
              children: ["/deploying/db-localfs.md", "/deploying/db-pfs.md"],
            },
          ],
        },
        {
          text: "More about Deployment",
          children: ["/deploying/deploy-official.md"],
        },
      ],
    },
  ],
  "/operation/": [
    {
      text: "Ops",
      children: [
        {
          text: "Daily Ops",
          children: [
            "/operation/backup-and-restore.md",
            "/operation/grow-storage.md",
            "/operation/scale-out.md",
            "/operation/ro-online-promote.md",
          ],
        },
        {
          text: "Benchmarks",
          children: ["/operation/tpcc-test.md", "/operation/tpch-test.md"],
        },
      ],
    },
  ],
  "/features": [],
  "/theory/": [
    {
      text: "Theory",
      children: [
        {
          text: "PolarDB for PostgreSQL",
          children: [
            "/theory/arch-overview.md",
            "/theory/arch-htap.md",
            "/theory/buffer-management.md",
            "/theory/ddl-synchronization.md",
            "/theory/logindex.md",
          ],
        },
        {
          text: "PostgreSQL",
          children: ["/theory/analyze.md", "/theory/polar-sequence-tech.md"],
        },
      ],
    },
  ],
  "/development/": [
    {
      text: "Development",
      children: [
        "/development/dev-on-docker.md",
        "/development/customize-dev-env.md",
      ],
    },
  ],
  "/contributing": [
    {
      text: "Contributing",
      children: [
        "/contributing/contributing-polardb-kernel.md",
        "/contributing/contributing-polardb-docs.md",
        "/contributing/coding-style.md",
        "/contributing/trouble-issuing.md",
      ],
    },
  ],
};
