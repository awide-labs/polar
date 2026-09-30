#!/bin/bash
# Forward-merge the stable branches upward (POLARDB_15_STABLE ->
# POLARDB_17_STABLE -> ...).
#
# Modes:
#   --merge   [SOURCE]   build the forward-merge branch SOURCE -> next and
#                        print how to push it and open the pull request.
#                        SOURCE defaults to the current branch.
#   --prepare HIGHER     author side: reproduce the forward-merge of your
#                        current branch (HEAD) into HIGHER - the next-higher
#                        stable branch, e.g. POLARDB_17_STABLE - so its
#                        conflict can be resolved once. Run it from your
#                        pull request branch.
#   --record             store the resolution from --prepare into the
#                        shared rerere cache and push it.
#
# The forward-merge pull request must be merged with a merge commit (no
# squash or rebase), or the next forward-merge sees the same changes again.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib-forward-merge.sh
source "${SCRIPT_DIR}/lib-forward-merge.sh"

_prepare_worktree() { git rev-parse --git-path forward-merge-prepare; }

_default_source() {
  printf '%s' "${1:-${GITHUB_REF_NAME:-$(git rev-parse --abbrev-ref HEAD)}}"
}

# Build the local forward-merge branch. Reports outcome via globals:
#   FWD_NEXT    next-higher branch ("" if SOURCE is the top branch)
#   FWD_BRANCH  forward branch name (set only when work was done)
#   FWD_STATUS  top | insync | clean | conflict
# A "clean" branch is left checked out, committed, ready to push. On
# "conflict" the merge is aborted and nothing is left behind - the
# resolution must be recorded via --prepare first (the can-forward-merge
# check normally guarantees this before the source ever lands).
# Usage: forward_merge_build <source>
forward_merge_build() {
  local source="${1:?}"
  FWD_NEXT=""; FWD_BRANCH=""; FWD_STATUS=""
  fetch_stable_branches
  FWD_NEXT="$(next_higher_branch "${source}")"
  [[ -z "${FWD_NEXT}" ]] && { FWD_STATUS="top"; return 0; }

  rerere_enable
  rerere_load

  if [[ -z "$(branch_drift "${FWD_NEXT}" "${source}")" ]]; then
    FWD_STATUS="insync"; return 0
  fi

  local next_ref source_ref
  next_ref="$(stable_branch_ref "${FWD_NEXT}")"
  source_ref="$(stable_branch_ref "${source}")"

  FWD_BRANCH="forward/${source}-to-${FWD_NEXT}"
  git checkout -B "${FWD_BRANCH}" "${next_ref}"

  if ! forward_steps "${source}" "${FWD_NEXT}" "${source_ref}"; then
    git merge --abort 2>/dev/null || true
    FWD_STATUS="conflict"
    return 0
  fi

  rerere_save || true
  FWD_STATUS="clean"
}

cmd_merge() {
  local source; source="$(_default_source "${1:-}")"
  forward_merge_build "${source}"
  case "${FWD_STATUS}" in
    top)      log_success "forward-merge: '${source}' is the highest branch; nothing to do" ;;
    insync)   log_success "forward-merge: ${FWD_NEXT} already contains ${source}" ;;
    clean)
      log_success "forward-merge: built ${FWD_BRANCH} (clean)"
      cat <<EOF

Push it and open a pull request into ${FWD_NEXT}; merge it with a merge
commit (no squash or rebase):

  git push $(forward_remote) ${FWD_BRANCH}
  gh pr create --base ${FWD_NEXT} --head ${FWD_BRANCH} --fill

EOF
      ;;
    conflict) log_error  "forward-merge: ${source} -> ${FWD_NEXT} conflicts; resolution not in cache (run --prepare)"; return 2 ;;
  esac
}

cmd_prepare() {
  local higher head head_sha wt higher_ref
  higher="${1:-}"
  head="${2:-HEAD}"
  if [[ -z "$(stable_branch_num "${higher}")" ]]; then
    log_error "forward-merge: --prepare needs the higher stable branch to forward into, e.g. --prepare POLARDB_17_STABLE"
    return 1
  fi
  fetch_stable_branches

  rerere_enable
  rerere_load
  higher_ref="$(stable_branch_ref "${higher}")"
  git rev-parse --verify -q "${higher_ref}^{commit}" >/dev/null \
    || { log_error "forward-merge: cannot find ${higher} on $(forward_remote)"; return 1; }
  # Merge the pull request's own content (HEAD), not a stable-branch tip:
  # the commits are not on the target branch yet, so merging its tip would
  # find nothing to resolve. rerere keys on hunk content, so a resolution
  # recorded here replays when the landed commits are forward-merged.
  head_sha="$(git rev-parse --verify -q "${head}^{commit}")" \
    || { log_error "forward-merge: cannot resolve '${head}'"; return 1; }
  ensure_merge_base "${higher_ref}" "${head_sha}" || true

  wt="$(_prepare_worktree)"
  git worktree remove --force "${wt}" 2>/dev/null || true
  git worktree add -q --detach "${wt}" "${higher_ref}"

  local lower lower_sha
  lower="$(next_lower_branch "${higher}")"
  if [[ -n "${lower}" ]] \
     && lower_sha="$(git rev-parse --verify -q "$(stable_branch_ref "${lower}")^{commit}")" \
     && ! (cd "${wt}" && forward_backlog "${lower}" "${higher}" "${lower_sha}"); then
    git worktree remove --force "${wt}" 2>/dev/null || true
    log_error "forward-merge: ${lower} backlog conflicts with ${higher}; fix the pending forward-merge first"
    return 1
  fi

  if git -C "${wt}" merge --no-edit "${head_sha}" \
     || { [[ -z "$(git -C "${wt}" ls-files --unmerged)" ]] && git -C "${wt}" commit --no-edit; }; then
    log_success "forward-merge: this branch merges cleanly into ${higher}; nothing to record"
    git worktree remove --force "${wt}"
    return 0
  fi

  local root
  root="$(git rev-parse --show-toplevel 2>/dev/null || pwd)"
  cat <<EOF

Resolve the forward-merge to ${higher} once, then record it:

  cd "${wt}"
  # fix the conflicts, or for a null-merge:  git merge --abort && git merge -s ours ${head_sha}
  git add -A
  cd "${root}"
  .ci/forward-merge/forward-merge.sh --record

EOF
}

cmd_record() {
  local wt
  rerere_enable
  wt="$(_prepare_worktree)"
  [[ -d "${wt}" ]] || { log_error "forward-merge: no prepare worktree; run --prepare first"; return 1; }

  # Captures the resolved hunks into the cache, then concludes the merge so
  # rerere stores the postimage.
  git -C "${wt}" rerere 2>/dev/null || true
  git -C "${wt}" commit --no-edit >/dev/null 2>&1 || true
  rerere_save push
  git worktree remove --force "${wt}"
  log_success "forward-merge: resolution recorded and pushed; re-run the pull request checks"
}

usage() { sed -n '2,18p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 1; }

main() {
  case "${1:-}" in
    --merge)   shift; cmd_merge "${1:-}" ;;
    --prepare) shift; cmd_prepare "${1:-}" "${2:-}" ;;
    --record)  cmd_record ;;
    *)         usage ;;
  esac
}

# Run only when executed; stay quiet (and return 0) when sourced for reuse.
if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
  main "$@"
fi
