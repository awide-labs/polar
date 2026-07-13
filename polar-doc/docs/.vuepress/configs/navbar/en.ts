import type { NavbarConfig } from "@vuepress/theme-default";

export const en: NavbarConfig = [
  {
    text: "Theory",
    link: "/theory/",
    children: [
      "/theory/arch-overview.html",
      "/theory/buffer-management.html",
      "/theory/ddl-synchronization.html",
      "/theory/logindex.html",
    ],
  },
  {
    text: "Roadmap",
    link: "/roadmap/",
  },
  {
    text: "Contributing",
    link: "/contributing/",
    children: [
      "/contributing/contributing-polardb-kernel.html",
      "/contributing/coding-style.html",
    ],
  },
];
