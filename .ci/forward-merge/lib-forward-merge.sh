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

# Whether the pull request label marks the whole change a null-merge.
# Usage: forward_label_null <next_branch>
forward_label_null() {
  grep -qiE "(^|,)forward:null-(all|${1:?})(,|$)" <<<"${FORWARD_LABELS:-}"
}

# Whether a change should be null-merged ("ours") into <next> rather than
# really merged. Authors signal "does not apply" two ways, both honored:
#   * pull request label  forward:null-<next>  (or forward:null-all),
#     passed in as the comma-separated $FORWARD_LABELS
#   * commit trailer      Forward-as-null: <next>  (or: all)
# Echoes "null" or "merge".
# Usage: forward_intent <next_branch> <range_base_ref> <range_head>
forward_intent() {
  local next="${1:?}" base="${2:?}" head="${3:?}"
  if forward_label_null "${next}"; then
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
# is used when they carry none. Each <unit> contributes the commits it
# brings over its first parent.
# Usage: forward_refs <unit>...
forward_refs() {
  local u refs
  refs="$(for u in "$@"; do
      git log --format='%(trailers:key=Refs,valueonly)' "${u}^..${u}" 2>/dev/null
    done | tr ',' '\n' | tr -d '[:blank:]' | grep -E '^[A-Za-z]+-[0-9]+$' | sort -uV \
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
# merge-carried, but the merge commit itself is validated, including the
# 72-character limit on the subject).
# Usage: forward_merge_message <source> <next> <intent> <unit>...
forward_merge_message() {
  local source="${1:?}" next="${2:?}" intent="${3:?}"
  shift 3
  local low high subject refs
  low="$(stable_branch_num "${source}")"
  high="$(stable_branch_num "${next}")"
  if [[ "${intent}" == "null" ]]; then
    subject="chore(merge): null-merge PolarDB ${low} commits already in PolarDB ${high}"
  else
    subject="chore(merge): forward PolarDB ${low} changes to PolarDB ${high}"
  fi
  refs="$(forward_refs "$@")" || return 1
  printf '%s\n\nRefs: %s\n' "${subject}" "${refs}"
}

# Merge one same-intent run of units (see forward_units) onto HEAD, with a
# single merge commit of the last one.
# Usage: _forward_merge_segment <source> <next> <intent> <unit>...
_forward_merge_segment() {
  local source="${1:?}" next="${2:?}" intent="${3:?}"
  local head="${!#}" msg
  msg="$(forward_merge_message "$@")" || return 1
  # --no-ff keeps intent boundaries: a fast-forwarded real run could be
  # rewound by a following null run.
  local opts=(--no-ff -m "${msg}")
  [[ "${intent}" == "null" ]] && opts+=(-s ours)
  git merge "${opts[@]}" "${head}" \
    || { [[ -z "$(git ls-files --unmerged)" ]] && git commit -m "${msg}"; }
}

# The units forward_steps merges for the first-parent steps of
# <base>..<tip>, oldest first, one "<commit> <intent>" per line. A step is
# one unit, unless it is a pull request merge whose commits mix intents:
# then each commit of the pull request is a unit, followed by the merge
# commit itself as a null unit, since its changes came with those commits.
# Changes made in such a merge commit itself (e.g. conflict resolutions)
# are not forwarded.
# Usage: forward_units <next_branch> <base> <tip>
forward_units() {
  local next="${1:?}" base="${2:?}" tip="${3:?}" c x intent kinds
  local -a subs
  while IFS= read -r c; do
    if git rev-parse -q --verify "${c}^2^{commit}" >/dev/null; then
      subs=()
      kinds=""
      while IFS= read -r x; do
        intent="$(forward_intent "${next}" "${x}^" "${x}")"
        subs+=("${x} ${intent}")
        [[ "${kinds}" == *"${intent}"* ]] || kinds+="${intent}"
      done < <(git rev-list --reverse --first-parent "${c}^1..${c}^2" --not "${base}")
      if [[ "${kinds}" == *null* && "${kinds}" == *merge* ]]; then
        printf '%s\n' "${subs[@]}"
        printf '%s null\n' "${c}"
        continue
      fi
    fi
    printf '%s %s\n' "${c}" "$(forward_intent "${next}" "${c}^" "${c}")"
  done < <(git rev-list --reverse --first-parent "${base}..${tip}")
}

# Forward <source_tip> onto HEAD (a checkout of <next>) one unit at a time
# (see forward_units), grouping consecutive units of the same intent, so a
# null marker on one landed change never applies to its neighbours. On
# conflict returns non-zero with the merge left in progress.
# Usage: forward_steps <source> <next> <source_tip>
forward_steps() {
  local source="${1:?}" next="${2:?}" tip="${3:?}"
  # No mapfile: --prepare must run on macOS bash 3.2.
  local -a units=() intents=()
  local u intent
  while read -r u intent; do
    units+=("${u}")
    intents+=("${intent}")
  done < <(forward_units "${next}" HEAD "${tip}")
  (( ${#units[@]} == 0 )) && return 0

  # i=$(( i + 1 )), not (( i++ )): the latter fails under set -e when i is 0.
  local i=0 seg_start seg_intent
  while (( i < ${#units[@]} )); do
    seg_start=${i}
    seg_intent="${intents[i]}"
    while (( i + 1 < ${#units[@]} )) && [[ "${intents[i+1]}" == "${seg_intent}" ]]; do
      i=$(( i + 1 ))
    done
    _forward_merge_segment "${source}" "${next}" "${seg_intent}" \
      "${units[@]:seg_start:i-seg_start+1}" || return 1
    i=$(( i + 1 ))
  done
}

# The merge commit <head> would land as on <source_sha>, so the pull request
# check and --prepare can forward it the way the bot will after landing.
# Prints nothing if <head> is already in <source_sha> or does not merge
# cleanly into it. Needs a committer identity.
# Usage: forward_landing <source_sha> <head_sha>
forward_landing() {
  local source_sha="${1:?}" head_sha="${2:?}" tree
  git merge-base --is-ancestor "${head_sha}" "${source_sha}" && return 0
  tree="$(git merge-tree --write-tree "${source_sha}" "${head_sha}" 2>/dev/null)" \
    || return 0
  git commit-tree "${tree}" -p "${source_sha}" -p "${head_sha}" \
    -m "Land ${head_sha}"
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
# With <source>, its backlog is forwarded first (forward_backlog), and then
# <head> as it would land on <source> (forward_landing), unit by unit like
# the bot, so commits marked null-merge are not trial-merged for real.
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

  local err land=""
  [[ -n "${source_sha}" ]] && land="$(forward_landing "${source_sha}" "${head_sha}")"
  if [[ -n "${land}" ]]; then
    err="$(cd "${wt}" && FORWARD_REFS="${FORWARD_REFS:-none}" \
      forward_steps "${source}" "${next}" "${land}" 2>&1 >/dev/null)"
  else
    err="$(git -C "${wt}" merge --no-ff --no-commit "${head_sha}" 2>&1 >/dev/null)"
  fi
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
