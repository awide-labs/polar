# 贡献代码

PolarDB for PostgreSQL 基于 PostgreSQL 和其它开源项目进行开发，我们的主要目标是为 PostgreSQL 建立一个更大的社区。我们欢迎来自社区的贡献者提交他们的代码或想法。在更远的未来，我们希望这个项目能够被来自阿里云内部和外部的开发者共同管理。

## 分支说明与管理方式

- `POLARDB_15_STABLE` 是 PolarDB-PG 的稳定分支

## 贡献代码之前

- 签署 PolarDB for PostgreSQL 的 [CLA](https://gist.github.com/alibaba-oss/151a13b0a72e44ba471119c7eb737d74)

## 贡献流程

- 在 `ApsaraDB/PolarDB-for-PostgreSQL` 仓库点击 `fork` 复制一个属于您自己的仓库
- 查阅 [进阶部署](../deploying/deploy.md) 了解如何从源码编译开发 PolarDB-PG
- 确保运行 `make stylecheck` 对代码进行格式化，然后向您的仓库推送代码
- 编译详细的提交信息，向上游仓库发起 Pull Request
- 等待所有的 CI 测试通过
- 等待维护者评审您的代码，讨论并解决所有的评审意见
- 等待维护者合并您的代码

## 代码提交实例说明

### 复制您自己的仓库

在 [PolarDB for PostgreSQL](https://github.com/ApsaraDB/PolarDB-for-PostgreSQL) 的代码仓库页面上，点击右上角的 **fork** 按钮复制您自己的 PolarDB 仓库。

### 克隆您的仓库到本地

```bash:no-line-numbers
git clone https://github.com/<your-github>/PolarDB-for-PostgreSQL.git
```

### 创建本地开发分支

从主干分支 `POLARDB_15_STABLE` 上检出一个新的开发分支，假设这个分支名为 `dev`：

```bash:no-line-numbers
git checkout POLARDB_15_STABLE
git checkout -b dev
```

### 在本地仓库修改代码并提交

```bash:no-line-numbers
git status
git add <files-to-change>
git commit -m "modification for dev"
```

### 变基并提交到远程仓库

首先点击您自己仓库页面上的 `Fetch upstream` 确保您的开发分支与 PolarDB-PG 上游仓库的稳定分支一致。然后将稳定分支上的最新修改拉取到本地：

```bash:no-line-numbers
git checkout POLARDB_15_STABLE
git pull
```

接下来将您的开发分支变基到目前的稳定分支，并解决冲突：

```bash:no-line-numbers
git checkout dev
git rebase POLARDB_15_STABLE
-- 解决冲突 --
git push -f dev
```

### 创建 Pull Request

点击 **New pull request** 或 **Compare & pull request** 按钮，选择对 `ApsaraDB/PolarDB-for-PostgreSQL:POLARDB_15_STABLE` 分支和 `<your-github>/PolarDB-for-PostgreSQL:dev` 分支进行比较，并撰写 PR 描述。

GitHub 会对您的 PR 进行自动化的回归测试，您的 PR 需要 100% 通过这些测试。

### 解决代码评审中的问题

您可以与维护者就代码中的问题进行讨论，并解决他们提出的评审意见。

### 代码合并

如果您的代码通过了测试和评审，PolarDB-PG 的维护者将会把您的 PR 合并到稳定分支上。
