import { defineUserConfig } from "vuepress";
import { path } from "vuepress/utils";
import { defaultTheme } from "@vuepress/theme-default";
import { docsearchPlugin } from "@vuepress/plugin-docsearch";
import { markdownMathPlugin } from "@vuepress/plugin-markdown-math";
import { markdownExtPlugin } from "@vuepress/plugin-markdown-ext";
import { registerComponentsPlugin } from "@vuepress/plugin-register-components";
import { en as navbar } from "./configs/navbar/en";
import { en as sidebar } from "./configs/sidebar/en";
import { viteBundler } from "@vuepress/bundler-vite";

const base_path = "/PolarDB-for-PostgreSQL/";

export default defineUserConfig({
  base: base_path,
  lang: "en-US",
  title: "PolarDB for PostgreSQL",
  description: "A cloud-native database developed by Alibaba Cloud",

  bundler: viteBundler(),

  head: [["link", { rel: "icon", href: base_path + "favicon.ico" }]],

  theme: defaultTheme({
    logo: "/images/polardb.png",
    repo: "polardb/PolarDB-for-PostgreSQL",
    docsBranch: "POLARDB_15_STABLE",
    docsDir: "polar-doc/docs/",
    colorMode: "light",
    editLinkText: "Edit this page on GitHub",
    navbar,
    sidebarDepth: 1,
    sidebar,
  }),

  plugins: [
    docsearchPlugin({
      appId: "OYQ6LCESQG",
      apiKey: "748b096a5ca5958b2da16301f213d7b1",
      indexName: "polardb-for-postgresql",
    }),
    markdownMathPlugin({
      type: "katex",
    }),
    markdownExtPlugin({
      footnote: true,
    }),
    registerComponentsPlugin({
      componentsDir: path.resolve(__dirname, "./components"),
    }),
  ],
});
