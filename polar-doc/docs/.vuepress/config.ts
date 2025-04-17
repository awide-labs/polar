import { defineUserConfig } from "vuepress";
import { path } from "vuepress/utils";
import { defaultTheme } from "@vuepress/theme-default";
import { docsearchPlugin } from "@vuepress/plugin-docsearch";
import { markdownMathPlugin } from "@vuepress/plugin-markdown-math";
import { markdownExtPlugin } from "@vuepress/plugin-markdown-ext";
import { registerComponentsPlugin } from "@vuepress/plugin-register-components";
import { navbar, sidebar } from "./configs";
import { viteBundler } from "@vuepress/bundler-vite";

const base_path = "/PolarDB-for-PostgreSQL/";

export default defineUserConfig({
  base: base_path,

  bundler: viteBundler(),

  head: [["link", { rel: "icon", href: base_path + "favicon.ico" }]],

  locales: {
    "/": {
      lang: "en-US",
      title: "PolarDB for PostgreSQL",
      description: "A cloud-native database developed by Alibaba Cloud",
    },
    "/zh/": {
      lang: "zh-CN",
      title: "PolarDB for PostgreSQL",
      description: "阿里云自主研发的云原生数据库",
    },
  },

  theme: defaultTheme({
    logo: "/images/polardb.png",
    repo: "ApsaraDB/PolarDB-for-PostgreSQL",
    docsBranch: "POLARDB_15_STABLE",
    docsDir: "polar-doc/docs/",
    colorMode: "light",

    locales: {
      "/": {
        selectLanguageName: "English",

        // page meta
        editLinkText: "Edit this page on GitHub",

        // navbar
        navbar: navbar.en,

        // sidebar
        sidebarDepth: 1,
        sidebar: sidebar.en,
      },
      "/zh/": {
        selectLanguageName: "简体中文",
        selectLanguageText: "选择语言",
        selectLanguageAriaLabel: "选择语言",

        // page meta
        editLinkText: "在 GitHub 上编辑此页",
        lastUpdatedText: "上次更新",
        contributorsText: "贡献者",

        // custom containers
        tip: "提示",
        warning: "注意",
        danger: "警告",

        // navbar
        navbar: navbar.zh,

        // sidebar
        sidebarDepth: 3,
        sidebar: sidebar.zh,
      },
    },
  }),

  plugins: [
    docsearchPlugin({
      appId: "OYQ6LCESQG",
      apiKey: "748b096a5ca5958b2da16301f213d7b1",
      indexName: "polardb-for-postgresql",
      locales: {
        "/zh/": {
          placeholder: "搜索文档",
          translations: {
            button: {
              buttonText: "搜索文档",
              buttonAriaLabel: "搜索文档",
            },
            modal: {
              searchBox: {
                resetButtonTitle: "清除查询条件",
                resetButtonAriaLabel: "清除查询条件",
                cancelButtonText: "取消",
                cancelButtonAriaLabel: "取消",
              },
              startScreen: {
                recentSearchesTitle: "搜索历史",
                noRecentSearchesText: "没有搜索历史",
                saveRecentSearchButtonTitle: "保存至搜索历史",
                removeRecentSearchButtonTitle: "从搜索历史中移除",
                favoriteSearchesTitle: "收藏",
                removeFavoriteSearchButtonTitle: "从收藏中移除",
              },
              errorScreen: {
                titleText: "无法获取结果",
                helpText: "你可能需要检查你的网络连接",
              },
              footer: {
                selectText: "选择",
                navigateText: "切换",
                closeText: "关闭",
                searchByText: "搜索提供者",
              },
              noResultsScreen: {
                noResultsText: "无法找到相关结果",
                suggestedQueryText: "你可以尝试查询",
                openIssueText: "你认为该查询应该有结果？",
                openIssueLinkText: "点击反馈",
              },
            },
          },
        },
      },
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
