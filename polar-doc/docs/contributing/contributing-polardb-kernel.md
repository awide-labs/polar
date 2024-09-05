# Code Contributing

PolarDB for PostgreSQL is an open source project based on PostgreSQL and other open source projects. Our main target is to create a larger community of PostgreSQL. Contributors are welcomed to submit their code and ideas. In a long run, we hope this project can be managed by developers from both inside and outside Alibaba Cloud.

## Branch Description and Management

- `POLARDB_15_STABLE` is the stable branch of PolarDB-PG

## Before Contributing

- Sign the [CLA](https://gist.github.com/alibaba-oss/151a13b0a72e44ba471119c7eb737d74) of PolarDB for PostgreSQL

## Contributing

Here is a checklist to prepare and submit your PR (pull request):

- Create your own Github repository copy by forking `ApsaraDB/PolarDB-for-PostgreSQL`.
- Checkout documentations [Advanced Deployment](../deploying/deploy.md) for how to hack PolarDB-PG.
- Run `make stylecheck` to format your code, and push changes to your personal fork.
- Edit detailed commit message, and create a PR to upstream.
- Wait for all CI checks to pass.
- Wait for review and address all feedbacks.
- Wait for merging.

## An Example of Submitting Code Change

Let's use an example to walk through the list.

### Fork Your Own Repository

On GitHub repository of [PolarDB for PostgreSQL](https://github.com/ApsaraDB/PolarDB-for-PostgreSQL), Click **fork** button to create your own PolarDB repository.

### Create Local Repository

```bash:no-line-numbers
git clone https://github.com/<your-github>/PolarDB-for-PostgreSQL.git
```

### Create a Local Development Branch

Check out a new development branch from the stable branch `POLARDB_15_STABLE`. Suppose your branch is named as `dev`:

```bash:no-line-numbers
git checkout POLARDB_15_STABLE
git checkout -b dev
```

### Make Changes and Commit Locally

```bash:no-line-numbers
git status
git add <files-to-change>
git commit -m "modification for dev"
```

### Rebase and Commit to Remote Repository

Click `Fetch upstream` on your own repository page to make sure your development branch is up do date with upstream. Then pull the latest commits on stable branch to your local repository.

```bash:no-line-numbers
git checkout POLARDB_15_STABLE
git pull
```

Then, rebase your development branch to the stable branch, and resolve conflicts:

```bash:no-line-numbers
git checkout dev
git rebase POLARDB_15_STABLE
-- resolve conflict --
git push -f dev
```

### Create a Pull Request

Click **New pull request** or **Compare & pull request** button, choose to compare branches `ApsaraDB/PolarDB-for-PostgreSQL:POLARDB_15_STABLE` and `<your-github>/PolarDB-for-PostgreSQL:dev`, and write PR description.

GitHub will automatically run regression test on your code. Your PR should pass all these checks.

### Address Reviewers' Comments

Resolve all problems raised by reviewers and update the PR.

### Merge

It is done by PolarDB-PG maintainers.
