#!/bin/bash
# Stable-branch topology helpers.
#
# Changes flow upward through the POLARDB_<N>_STABLE line (15 -> 17 -> ...).
# Nothing here hardcodes a version: the ordered branch list is discovered
# at runtime and sorted numerically by <N>, so a new major branch is
# picked up the moment it exists.
#
# Sourced by lib-forward-merge.sh. Defines functions only; sets no shell
# options (the caller owns those).

readonly STABLE_BRANCH_RE='^POLARDB_([0-9]+)_STABLE$'
readonly STABLE_BRANCH_GLOB='POLARDB_*_STABLE'

# Remote that hosts the canonical stable branches. "origin" is not assumed:
# in a clone where origin points at a personal fork, the canonical remote
# is a different one (e.g. "upstream"). Resolution order:
#   1. $FORWARD_REMOTE, if set;
#   2. the current branch's configured upstream remote;
#   3. "origin" if it exists;
#   4. the first remote configured.
forward_remote() {
  if [[ -n "${FORWARD_REMOTE:-}" ]]; then
    printf '%s' "${FORWARD_REMOTE}"
    return 0
  fi
  local branch upstream
  branch="$(git symbolic-ref -q --short HEAD 2>/dev/null || true)"
  if [[ -n "${branch}" ]]; then
    upstream="$(git config "branch.${branch}.remote" 2>/dev/null || true)"
    [[ -n "${upstream}" && "${upstream}" != "." ]] \
      && { printf '%s' "${upstream}"; return 0; }
  fi
  if git remote get-url origin >/dev/null 2>&1; then
    printf 'origin'
    return 0
  fi
  git remote | head -n1
}

# Where the canonical branch tips live. CI clones have remote-tracking refs;
# a local rehearsal may only have local heads. Pick whichever resolves.
_stable_ref_space() {
  local remote
  remote="$(forward_remote)"
  if [[ -n "${remote}" ]] \
     && git for-each-ref --count=1 "refs/remotes/${remote}/${STABLE_BRANCH_GLOB}" \
          --format='%(refname)' | grep -q .; then
    printf 'refs/remotes/%s' "${remote}"
  else
    printf 'refs/heads'
  fi
}

_stable_refspec() {
  printf '+refs/heads/%s:refs/remotes/%s/%s' \
    "${STABLE_BRANCH_GLOB}" "${1:?}" "${STABLE_BRANCH_GLOB}"
}

# Populate refs/remotes/<remote>/POLARDB_*_STABLE. CI clones may fetch only
# the pull request ref, so the topology helpers cannot rely on these
# existing - fetch them explicitly before any branch comparison.
#
# On a shallow clone the tips come in at depth 1; ensure_merge_base below
# deepens afterwards only as far as an actual comparison needs.
fetch_stable_branches() {
  local remote
  remote="$(forward_remote)"
  [[ -n "${remote}" ]] || return 0
  local args=(-q)
  if [[ -f "$(git rev-parse --git-path shallow 2>/dev/null)" ]]; then
    # --update-shallow: the new tips rewrite .git/shallow.
    args+=(--depth=1 --update-shallow)
  fi
  git fetch "${args[@]}" "${remote}" "$(_stable_refspec "${remote}")" \
    2>/dev/null || true
}

# Ensure the merge base of two commits is present, incrementally deepening a
# shallow clone. Stops the moment a base exists, so an in-sync pair - whose
# base is recent - stays cheap, and only a deep base pays for more history.
# No-op on a complete clone; returns non-zero when no base can be found
# (genuinely unrelated histories).
#
# --deepen is relative to *every* boundary in .git/shallow, not just the
# refs named here, so the pull request head's own history is extended along
# with them. Never deepen a raw SHA: upload-pack answers such a want with
# the complete history, undoing the shallow clone.
ensure_merge_base() {
  local a="${1:?}" b="${2:?}" remote step=64
  git merge-base "${a}" "${b}" >/dev/null 2>&1 && return 0
  [[ -f "$(git rev-parse --git-path shallow 2>/dev/null)" ]] || return 1
  remote="$(forward_remote)"
  [[ -n "${remote}" ]] || return 1
  while [[ -f "$(git rev-parse --git-path shallow 2>/dev/null)" ]]; do
    git fetch -q --deepen="${step}" --update-shallow "${remote}" \
      "$(_stable_refspec "${remote}")" 2>/dev/null || break
    git merge-base "${a}" "${b}" >/dev/null 2>&1 && return 0
    step=$(( step >= 1048576 ? step : step * 4 ))
  done
  git merge-base "${a}" "${b}" >/dev/null 2>&1
}

# Numeric <N> of a POLARDB_<N>_STABLE name (empty if it does not match).
stable_branch_num() {
  [[ "${1:-}" =~ ${STABLE_BRANCH_RE} ]] && printf '%s' "${BASH_REMATCH[1]}"
}

# Ordered (ascending <N>) list of stable branch short names, one per line.
stable_branches() {
  local space ref name num
  space="$(_stable_ref_space)"
  while IFS= read -r ref; do
    name="${ref##*/}"
    num="$(stable_branch_num "${name}")"
    [[ -n "${num}" ]] && printf '%s %s\n' "${num}" "${name}"
  done < <(git for-each-ref "${space}/${STABLE_BRANCH_GLOB}" --format='%(refname)') \
    | sort -n | awk '{print $2}'
}

# Short name of the next branch strictly above the given one, or empty if
# the given branch is already the highest (the top of the line).
next_higher_branch() {
  local current="${1:?next_higher_branch: branch required}"
  local cur_num branch num
  cur_num="$(stable_branch_num "${current}")"
  [[ -z "${cur_num}" ]] && return 0
  while IFS= read -r branch; do
    num="$(stable_branch_num "${branch}")"
    if (( num > cur_num )); then
      printf '%s' "${branch}"
      return 0
    fi
  done < <(stable_branches)
}

# Short name of the next branch below the given one, or empty.
next_lower_branch() {
  local current="${1:?next_lower_branch: branch required}"
  local cur_num branch num lower=""
  cur_num="$(stable_branch_num "${current}")"
  [[ -z "${cur_num}" ]] && return 0
  while IFS= read -r branch; do
    num="$(stable_branch_num "${branch}")"
    (( num < cur_num )) && lower="${branch}"
  done < <(stable_branches)
  printf '%s' "${lower}"
}

# Fully-qualified ref for a branch in the active ref space.
stable_branch_ref() {
  printf '%s/%s' "$(_stable_ref_space)" "${1:?stable_branch_ref: branch required}"
}

# Commits present in <lower> but not reachable from <higher> - the changes
# still waiting to be forwarded. Emits "<short-sha> <subject>" per line;
# empty output means <higher> contains all of <lower>.
branch_drift() {
  local higher="${1:?}" lower="${2:?}"
  git log --format='%h %s' \
    "$(stable_branch_ref "${higher}")..$(stable_branch_ref "${lower}")"
}
