import type { NavbarConfig } from "@vuepress/theme-default";

export const en: NavbarConfig = [
  {
    text: "Deployment",
    children: [
      "/deploying/introduction.html",
      "/deploying/quick-start.html",
      "/deploying/deploy.html",
      {
        text: "Preparation of Shared-Storage Device",
        children: ["/deploying/storage-aliyun-essd.html"],
      },
      {
        text: "Preparation of File System",
        children: ["/deploying/fs-pfs.html"],
      },
      {
        text: "Deploying PolarDB",
        children: ["/deploying/db-localfs.html", "/deploying/db-pfs.html"],
      },
      {
        text: "More about Deployment",
        children: ["/deploying/deploy-official.html"],
      },
    ],
  },
  {
    text: "Ops",
    link: "/operation/",
    children: [
      {
        text: "Daily Ops",
        children: [
          "/operation/backup-and-restore.html",
          "/operation/grow-storage.html",
          "/operation/scale-out.html",
          "/operation/ro-online-promote.html",
        ],
      },
      {
        text: "Benchmarks",
        children: ["/operation/tpcc-test.html", "/operation/tpch-test.html"],
      },
    ],
  },
  {
    text: "Kernel Features",
    link: "/features/",
  },
  {
    text: "Theory",
    link: "/theory/",
    children: [
      {
        text: "PolarDB for PostgreSQL",
        children: [
          "/theory/arch-overview.html",
          "/theory/arch-htap.html",
          "/theory/buffer-management.html",
          "/theory/ddl-synchronization.html",
          "/theory/logindex.html",
        ],
      },
      {
        text: "PostgreSQL",
        children: ["/theory/analyze.html", "/theory/polar-sequence-tech.html"],
      },
    ],
  },
  {
    text: "Dev",
    link: "/development/",
    children: [
      "/development/dev-on-docker.html",
      "/development/customize-dev-env.html",
    ],
  },
  {
    text: "Contributing",
    link: "/contributing/",
    children: [
      "/contributing/contributing-polardb-kernel.html",
      "/contributing/contributing-polardb-docs.html",
      "/contributing/coding-style.html",
      "/contributing/trouble-issuing.html",
    ],
  },
];
