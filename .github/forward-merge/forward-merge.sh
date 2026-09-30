#!/bin/bash
# GitHub glue for the forward-merge bot.
#
# Runs on every push to a POLARDB_*_STABLE branch. Builds the forward-merge
# to the next-higher branch (via .ci/forward-merge/forward-merge.sh), pushes
# it, opens a pull request assigned to the original author(s), and enables
# auto-merge so a clean forward lands without anyone touching it. When the
# merge needs manual resolution (rare - the Can Forward Merge check normally
# records it first) the job fails loudly so sync-check stays red.
#
# Required: GH_TOKEN, the bot's token (also used by the checkout, so pushes
# run as the bot and trigger the pull request checks). See README.md.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
cd "${REPO_ROOT}"

# shellcheck source=../../.ci/forward-merge/forward-merge.sh
source "${REPO_ROOT}/.ci/forward-merge/forward-merge.sh"

REPO="${GITHUB_REPOSITORY:?must run in GitHub Actions}"
BOT_LOGIN="${FORWARD_BOT_LOGIN:-awide-polardb-bot}"

configure_git_bot() {
  local id
  id="$(gh api "users/${BOT_LOGIN}" --jq .id)"
  git config user.name "${BOT_LOGIN}"
  git config user.email "${id}+${BOT_LOGIN}@users.noreply.github.com"
}

# GitHub logins for the pull request: the authors of the forwarded commits
# (as GitHub links them) plus the user who pushed the triggering change, so
# the pull request never lands unassigned. One per line.
assignees() {
  local source="${1}" next="${2}" sha login
  {
    while IFS= read -r sha; do
      login="$(gh api "repos/${REPO}/commits/${sha}" --jq '.author.login // empty' 2>/dev/null || true)"
      [[ -n "${login}" ]] && echo "${login}"
    done < <(git rev-list --no-merges \
      "$(stable_branch_ref "${next}")..$(stable_branch_ref "${source}")")
    [[ -n "${GITHUB_ACTOR:-}" ]] && echo "${GITHUB_ACTOR}"
  } | grep -vx -e "${BOT_LOGIN}" -e 'web-flow' | sort -u
}

open_or_update_pr() {
  local source="${1}" branch="${2}" next="${3}" number title body login
  number="$(gh pr list -R "${REPO}" --head "${branch}" --base "${next}" \
    --state open --json number --jq '.[0].number // empty')"
  if [[ -n "${number}" ]]; then
    log_warning "forward-merge: pull request #${number} already open; updated"
  else
    title="Forward-merge ${source} into ${next}"
    body=$(cat <<PR
Automated forward-merge created by \`.github/forward-merge/forward-merge.sh\`
to keep the stable branches in sync (\`git log ${next}..${source}\` must be
empty).

Auto-merges with a merge commit when the checks pass. If review or changes
are needed, the original author(s) are assigned.
PR
)
    gh pr create -R "${REPO}" --head "${branch}" --base "${next}" \
      --title "${title}" --body "${body}" >/dev/null
    number="$(gh pr list -R "${REPO}" --head "${branch}" --base "${next}" \
      --state open --json number --jq '.[0].number')"
    log_success "forward-merge: opened pull request #${number}"
  fi
  while IFS= read -r login; do
    [[ -z "${login}" ]] && continue
    gh pr edit -R "${REPO}" "${number}" --add-assignee "${login}" >/dev/null 2>&1 \
      || log_warning "forward-merge: could not assign ${login}"
  done < <(assignees "${source}" "${next}")
  PR_NUMBER="${number}"
}

# Arm auto-merge with a merge commit (squash or rebase would drop the merge
# and break the sync invariant). GitHub arms it only when the target branch
# has required checks; never merge before the checks ran.
enable_auto_merge() {
  local number="${1}" err
  if err="$(gh pr merge -R "${REPO}" "${number}" --auto --merge 2>&1)"; then
    log_success "forward-merge: auto-merge armed on pull request #${number}"
  else
    log_warning "forward-merge: could not arm auto-merge on pull request #${number} (${err}); merge it with a merge commit once green"
  fi
}

main() {
  local source="${GITHUB_REF_NAME:?must run on a branch push}"
  : "${GH_TOKEN:?the bot token is required}"
  configure_git_bot

  forward_merge_build "${source}"
  case "${FWD_STATUS}" in
    top|insync)
      log_success "forward-merge: nothing to forward from ${source}"
      return 0 ;;
    conflict)
      log_error "forward-merge: ${source} -> ${FWD_NEXT} needs manual resolution."
      log_error "               Run '.ci/forward-merge/forward-merge.sh --prepare ${FWD_NEXT}' to record it."
      return 1 ;;
  esac

  git push -q --force "$(forward_remote)" "${FWD_BRANCH}:${FWD_BRANCH}"

  PR_NUMBER=""
  open_or_update_pr "${source}" "${FWD_BRANCH}" "${FWD_NEXT}"
  [[ -n "${PR_NUMBER}" ]] && enable_auto_merge "${PR_NUMBER}"
}

main "$@"
