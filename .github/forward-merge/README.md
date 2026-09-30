# Forward-merge branch sync

Keeps the stable branches (`POLARDB_15_STABLE` -> `POLARDB_17_STABLE` -> ...
higher versions later) in sync. The invariant is:

> for every adjacent pair, `git log <higher>..<lower>` is empty.

Changes flow **upward only**. You land a fix on the lowest branch it applies
to, and it is forward-merged into every higher branch as either a real merge
(with conflict resolution/adaptation) or a `git merge -s ours` null-merge
(when it does not apply). Both keep the lower branch an ancestor of the
higher one, so the invariant holds.

## Pieces

| File | Role |
| ---- | ---- |
| [`.ci/forward-merge/lib-stable-branches.sh`](../../.ci/forward-merge/lib-stable-branches.sh) | Discovers/orders `POLARDB_<N>_STABLE`, computes the next branch and drift. |
| [`.ci/forward-merge/lib-rerere-cache.sh`](../../.ci/forward-merge/lib-rerere-cache.sh) | Shared `git rerere` cache in `refs/forward-merge/rerere`. |
| [`.ci/forward-merge/lib-forward-merge.sh`](../../.ci/forward-merge/lib-forward-merge.sh) | Intent detection + trial merge. |
| [`.ci/forward-merge/check-can-forward-merge.sh`](../../.ci/forward-merge/check-can-forward-merge.sh) | Pull request check: blocks until the forward-merge is feasible. |
| [`.ci/forward-merge/forward-merge.sh`](../../.ci/forward-merge/forward-merge.sh) | Forward-merge core + author `--prepare`/`--record`. |
| [`forward-merge.sh`](forward-merge.sh) | GitHub glue: push, open the pull request, assign authors, arm auto-merge. |
| [`.ci/check_sync.sh`](../../.ci/check_sync.sh) | Drift alarm (scheduled + on stable pushes). |
| [`../workflows/forward-merge.yml`](../workflows/forward-merge.yml) | The `Can Forward Merge`, `Forward Merge` and `Sync Check` jobs. |

## How it flows

1. You open a pull request into a lower stable branch (e.g.
   `POLARDB_15_STABLE`).
2. **Can Forward Merge** trial-merges your change into the next branch
   (`POLARDB_17_STABLE`). It passes when the merge is clean, when you marked
   it a null-merge, or when a recorded resolution applies. Otherwise it fails
   with what to do; it is a required check, so the pull request cannot merge.
3. After your pull request lands, the push triggers **Forward Merge**: as
   `awide-polardb-bot` it builds the merge to the next branch, pushes a
   `forward/<source>-to-<next>` branch, opens a pull request assigned to you,
   and arms auto-merge (with a merge commit). A clean forward lands with no
   further action once its checks pass.
4. That landing triggers the same job on the next branch, cascading the change
   all the way up.
5. **Sync Check** runs daily, on demand and on stable pushes; it goes red when
   a branch pair stays out of sync past the age/count thresholds.

## Author workflow for a conflicting forward-merge

When Can Forward Merge fails, resolve the conflict once - the resolution is
cached (by hunk content, not SHA) and replayed automatically by the bot:

```bash
git checkout my-pr-branch           # the branch with your unmerged commits
# If `origin` is your personal fork, point the helper at the canonical remote:
#   export FORWARD_REMOTE=upstream
.ci/forward-merge/forward-merge.sh --prepare POLARDB_17_STABLE   # the higher branch to forward into
# follow the printed instructions: resolve in the worktree, `git add -A`
.ci/forward-merge/forward-merge.sh --record
```

`--record` pushes `refs/forward-merge/rerere`, so it needs write access to
the repository; contributors without it ask a maintainer to record the
resolution. Re-run the check; it now passes. For a change that does **not**
apply to the higher branch, record a null-merge instead (`git merge -s ours`
inside the prepare worktree), or skip the work entirely with either:

- pull request label `forward:null-<higher>` (or `forward:null-all`), or
- a `Forward-as-null: <higher>` commit footer (or `Forward-as-null: all`).

Footers must be in their own paragraph (a blank line before them).

## Secrets and variables

| Name | Kind | Required | Description |
| ---- | ---- | -------- | ----------- |
| `FORWARD_BOT_TOKEN` | secret | yes | Token of `awide-polardb-bot` (write access to the repository). Classic: `repo` + `workflow` scopes; fine-grained: Contents, Pull requests and Workflows read/write. `workflow` is needed because forwarded changes may touch `.github/workflows/`. |
| `FORWARD_REFS` | variable | no | `Refs:` of a forward-merge whose commits carry none. |
| `SYNC_MAX_AGE_HOURS` | variable | no | Drift age that fails Sync Check. Default: 24. |
| `SYNC_MAX_COUNT` | variable | no | Backlog size that fails regardless of age. Default: 20. |

The bot's pushes and pull requests are made with its own token, not
`GITHUB_TOKEN`, so they trigger the pull request checks.

## One-time GitHub configuration

- **Settings -> General -> Pull Requests**: allow merge commits (forward
  merges must land as merge commits) and allow auto-merge.
- **Rulesets** for `POLARDB_*_STABLE`: require pull requests and the status
  checks, including **Can Forward Merge**; auto-merge only arms when the
  target branch has required checks. Do not restrict `forward/*` branches:
  the bot force-pushes them.
- `refs/forward-merge/rerere` is outside `refs/heads/*`; keep it writable by
  the bot and maintainers.

## Notes and limitations

- Drift is unavoidable in the short window between a lower-branch merge and the
  bot's forward landing; that is why Sync Check uses thresholds rather than
  demanding instantaneous emptiness.
- rerere replay is heuristic. The forward pull request runs the full checks
  and only auto-merges when green, so a bad replay is caught by tests.
- If the bot hits a conflict with no cached resolution (e.g. the check was
  bypassed), the Forward Merge job fails and Sync Check stays red until the
  resolution is recorded with `--prepare`/`--record` and the job is re-run.
