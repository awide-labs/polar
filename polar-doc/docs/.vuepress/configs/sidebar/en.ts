import type { SidebarConfig } from "@vuepress/theme-default";

export const en: SidebarConfig = {
  "/theory/": [
    {
      text: "Theory",
      children: [
        "/theory/arch-overview.md",
        "/theory/buffer-management.md",
        "/theory/ddl-synchronization.md",
        "/theory/logindex.md",
      ],
    },
  ],
  "/roadmap/": [
    {
      text: "Roadmap",
      children: ["/roadmap/README.md"],
    },
  ],
  "/contributing": [
    {
      text: "Contributing",
      children: [
        "/contributing/contributing-polardb-kernel.md",
        "/contributing/coding-style.md",
      ],
    },
  ],
};
