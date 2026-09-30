#!/bin/bash
# Shared forward-merge logic used by both the pull request check and
# forward-merge.sh.
#
# Sourcing this pulls in the branch-topology, rerere-cache, and common
# log helpers so callers get one entry point.

_FM_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# common.sh lives one level up in .ci/; the rest are siblings here.
# shellcheck source=../common.sh
source "${_FM_DIR}/../common.sh"
# shellcheck source=lib-stable-branches.sh
source "${_FM_DIR}/lib-stable-branches.sh"
# shellcheck source=lib-rerere-cache.sh
source "${_FM_DIR}/lib-rerere-cache.sh"
unset _FM_DIR

# Whether a change should be null-merged ("ours") into <next> rather than
# really merged. Authors signal "does not apply" two ways, both honored:
#   * pull request label  forward:null-<next>  (or forward:null-all),
#     passed in as the comma-separated $FORWARD_LABELS
#   * commit trailer      Forward-as-null: <next>  (or: all)
# Echoes "null" or "merge".
# Usage: forward_intent <next_branch> <range_base_ref> <range_head>
forward_intent() {
  local next="${1:?}" base="${2:?}" head="${3:?}"
  if grep -qiE "(^|,)forward:null-(all|${next})(,|$)" \
       <<<"${FORWARD_LABELS:-}"; then
    printf 'null'; return 0
  fi
  # Collect first, match after: piping into `grep -q` makes grep exit on the
  # first match while git log is still traversing, and the resulting SIGPIPE
  # turns a *found* trailer into a non-zero pipeline under `set -o pipefail`.
  local trailers
  trailers="$(git log --format='%(trailers:key=Forward-as-null,valueonly)' \
    "${base}..${head}" 2>/dev/null)" || trailers=""
  if grep -qiwE "all|${next}" <<<"${trailers}"; then
    printf 'null'; return 0
  fi
  printf 'merge'
}

# Refs: footer for the merge commit: the Jira references of the forwarded
# commits, so the merge stays traceable to the original work. $FORWARD_REFS
# is used when they carry none.
# Usage: forward_refs <range_base_ref> <range_head>
forward_refs() {
  local base="${1:?}" head="${2:?}" refs
  refs="$(git log --format='%(trailers:key=Refs,valueonly)' "${base}..${head}" 2>/dev/null \
    | tr ',' '\n' | tr -d '[:blank:]' | grep -E '^[A-Za-z]+-[0-9]+$' | sort -uV \
    | paste -sd, - | sed 's/,/, /g')"
  if [[ -n "${refs}" ]]; then
    printf '%s' "${refs}"
  elif [[ -n "${FORWARD_REFS:-}" ]]; then
    printf '%s' "${FORWARD_REFS}"
  else
    log_error "forward-merge: the forwarded commits have no Refs:; set FORWARD_REFS" >&2
    return 1
  fi
}

# Conventional-Commits-compliant message for the forward-merge commit, so it
# passes .ci/check_commit_format.sh (the brought commits are exempt as
# merge-carried, but the merge commit itself is validated).
# Usage: forward_merge_message <source> <next> <intent> <base_ref> <head>
forward_merge_message() {
  local source="${1:?}" next="${2:?}" intent="${3:?}" base="${4:?}" head="${5:?}"
  local low high subject refs
  low="$(stable_branch_num "${source}")"
  high="$(stable_branch_num "${next}")"
  if [[ "${intent}" == "null" ]]; then
    subject="chore(merge): null-merge PolarDB ${low} commits already present in PolarDB ${high}"
  else
    subject="chore(merge): forward PolarDB ${low} changes to PolarDB ${high}"
  fi
  refs="$(forward_refs "${base}" "${head}")" || return 1
  printf '%s\n\nRefs: %s\n' "${subject}" "${refs}"
}

# Merge one same-intent run of landed changes, ending at <head>, onto HEAD.
# Usage: _forward_merge_segment <source> <next> <intent> <range_base> <head>
_forward_merge_segment() {
  local source="${1:?}" next="${2:?}" intent="${3:?}" base="${4:?}" head="${5:?}" msg
  msg="$(forward_merge_message "${source}" "${next}" "${intent}" "${base}" "${head}")" \
    || return 1
  # --no-ff keeps intent boundaries: a fast-forwarded real run could be
  # rewound by a following null run.
  local opts=(--no-ff -m "${msg}")
  [[ "${intent}" == "null" ]] && opts+=(-s ours)
  git merge "${opts[@]}" "${head}" \
    || { [[ -z "$(git ls-files --unmerged)" ]] && git commit -m "${msg}"; }
}

# Forward <source_tip> onto HEAD (a checkout of <next>) one first-parent step
# at a time, grouping consecutive steps of the same intent, so a null marker
# on one landed change never applies to its neighbours. On conflict returns
# non-zero with the merge left in progress.
# Usage: forward_steps <source> <next> <source_tip>
forward_steps() {
  local source="${1:?}" next="${2:?}" tip="${3:?}"
  # No mapfile: --prepare must run on macOS bash 3.2.
  local -a steps=() intents=()
  local c
  while IFS= read -r c; do
    steps+=("${c}")
    intents+=("$(forward_intent "${next}" "${c}^" "${c}")")
  done < <(git rev-list --reverse --first-parent "HEAD..${tip}")
  (( ${#steps[@]} == 0 )) && return 0

  # i=$(( i + 1 )), not (( i++ )): the latter fails under set -e when i is 0.
  local i=0 seg_start seg_intent
  while (( i < ${#steps[@]} )); do
    seg_start=${i}
    seg_intent="${intents[i]}"
    while (( i + 1 < ${#steps[@]} )) && [[ "${intents[i+1]}" == "${seg_intent}" ]]; do
      i=$(( i + 1 ))
    done
    _forward_merge_segment "${source}" "${next}" "${seg_intent}" \
      "${steps[seg_start]}^" "${steps[i]}" || return 1
    i=$(( i + 1 ))
  done
}

# Forward the not-yet-forwarded backlog of <source> onto HEAD the way
# forward-merge.sh --merge will, so a pending null-merge is not trial-merged
# for real. Pull request labels are ignored: they describe the pull
# request, not the backlog. <source_tip> must be a SHA, since
# forward_remote() can differ inside a detached worktree.
# Usage: forward_backlog <source> <next> <source_tip>
forward_backlog() {
  local source="${1:?}" next="${2:?}" tip="${3:?}"
  if ! FORWARD_LABELS="" FORWARD_REFS="${FORWARD_REFS:-none}" \
       forward_steps "${source}" "${next}" "${tip}" >/dev/null 2>&1; then
    git merge --abort >/dev/null 2>&1 || true
    return 1
  fi
}

# Dry-run a merge of <head> into <next> inside a throwaway worktree, with
# the shared rerere cache already loaded so recorded resolutions apply.
# With <source>, its backlog is forwarded first (forward_backlog).
# Exit status (fail-closed: never claim "clean" unless a merge actually ran
# to a conflict-free result):
#   0  clean    - merge succeeded, or rerere replayed every conflict
#   1  conflict - real conflicts remain (conflicting paths printed)
#   2  error    - could not evaluate (unresolved refs, worktree/merge never
#                 started); the caller must treat this as "not provably safe"
#   3  backlog  - <source>'s backlog itself conflicts with <next>
# Leaves no worktree or in-progress merge behind.
# Usage: trial_merge <next_branch> <head> [<source>]
trial_merge() {
  local next="${1:?}" head="${2:?}" source="${3:-}" base base_sha head_sha wt rc merge_rc
  base="$(stable_branch_ref "${next}")"
  # Resolve both ends to concrete SHAs in THIS repo. Critical for head:
  # we merge inside a separate worktree below, and a symbolic name like
  # "HEAD" would otherwise resolve to that worktree's own HEAD (the base),
  # silently turning the trial into a no-op that always looks clean.
  base_sha="$(git rev-parse --verify -q "${base}^{commit}")" || true
  head_sha="$(git rev-parse --verify -q "${head}^{commit}")" || true
  if [[ -z "${base_sha}" || -z "${head_sha}" ]]; then
    log_error "trial_merge: cannot resolve ${next} (${base}) or head '${head}'" >&2
    return 2
  fi
  # A shallow CI clone may lack the merge base, which would make the merge
  # below abort as "unrelated histories"; deepen just enough to expose it.
  ensure_merge_base "${base_sha}" "${head_sha}" || true

  wt="$(mktemp -d)"
  if ! git worktree add -q --detach "${wt}" "${base_sha}" 2>/dev/null; then
    log_error "trial_merge: could not create worktree at ${base}" >&2
    rm -rf "${wt}"
    return 2
  fi

  # --no-ff validates committer identity up front (even with --no-commit),
  # and CI runners have none configured; supply a throwaway one.
  local -x GIT_AUTHOR_NAME="can-forward-merge" GIT_AUTHOR_EMAIL="can-forward-merge@localhost"
  local -x GIT_COMMITTER_NAME="${GIT_AUTHOR_NAME}" GIT_COMMITTER_EMAIL="${GIT_AUTHOR_EMAIL}"

  local source_sha=""
  if [[ -n "${source}" ]]; then
    source_sha="$(git rev-parse --verify -q "$(stable_branch_ref "${source}")^{commit}")" || true
    [[ -n "${source_sha}" ]] && { ensure_merge_base "${base_sha}" "${source_sha}" || true; }
  fi
  if [[ -n "${source_sha}" ]] \
     && ! (cd "${wt}" && forward_backlog "${source}" "${next}" "${source_sha}"); then
    git worktree remove --force "${wt}" >/dev/null 2>&1 || true
    rm -rf "${wt}"
    return 3
  fi

  local err
  err="$(git -C "${wt}" merge --no-ff --no-commit "${head_sha}" 2>&1 >/dev/null)"
  merge_rc=$?
  rc=0
  if [[ "${merge_rc}" -ne 0 ]]; then
    if [[ -n "$(git -C "${wt}" ls-files --unmerged)" ]]; then
      rc=1
      git -C "${wt}" diff --name-only --diff-filter=U
    elif [[ ! -e "$(git -C "${wt}" rev-parse --git-path MERGE_HEAD)" ]]; then
      # Merge never started (e.g. unrelated histories - usually a shallow
      # clone missing the merge base) - not a clean result.
      log_error "trial_merge: merge of '${head}' into ${next} did not run: ${err:-unknown error}" >&2
      rc=2
    fi
    # else: nonzero with no unmerged entries but a merge in progress means
    # rerere replayed every conflict from the cache - a clean result (rc=0).
  fi

  git -C "${wt}" merge --abort >/dev/null 2>&1 || true
  git worktree remove --force "${wt}" >/dev/null 2>&1 || true
  rm -rf "${wt}"
  return "${rc}"
}
