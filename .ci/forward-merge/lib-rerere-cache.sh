#!/bin/bash
# Shared git rerere cache, persisted in a git ref.
#
# rerere ("reuse recorded resolution") keys on conflict-hunk content, not
# commit SHAs, so a resolution recorded once is replayed automatically on
# the real merge even after a rebase changes the SHAs. Sharing that cache
# between the pull request check and whoever runs the forward-merge is
# what lets a conflict be resolved once, by the author of the change.
#
# The cache directory ($GIT_DIR/rr-cache) is stored as the tree of a
# single commit at RERERE_CACHE_REF and shuttled in/out with tar so we
# avoid touching the working tree or index.
#
# Relies on forward_remote() from lib-stable-branches.sh (sourced together
# via lib-forward-merge.sh) so the canonical remote is not hardcoded.

readonly RERERE_CACHE_REF='refs/forward-merge/rerere'

_rr_dir() { git rev-parse --git-path rr-cache; }

# Turn on rerere with autostaging so replayed resolutions land in the
# index without a second manual step.
rerere_enable() {
  git config rerere.enabled true
  git config rerere.autoupdate true
}

# Pull the shared cache from the canonical remote into this repo. A missing
# ref (first ever run) is not an error.
rerere_load() {
  local rr_dir remote
  rr_dir="$(_rr_dir)"
  remote="$(forward_remote)"
  [[ -n "${remote}" ]] || return 0
  git fetch -q "${remote}" "+${RERERE_CACHE_REF}:${RERERE_CACHE_REF}" 2>/dev/null || return 0
  mkdir -p "${rr_dir}"
  git archive "${RERERE_CACHE_REF}" 2>/dev/null | tar -x -C "${rr_dir}" 2>/dev/null || true
}

# Snapshot the local cache into RERERE_CACHE_REF. With <push>, also push it
# to the canonical remote (needs write access). No-op when there is
# nothing to record.
# Usage: rerere_save [push]
rerere_save() {
  local rr_dir tree commit
  rr_dir="$(_rr_dir)"
  [[ -d "${rr_dir}" ]] && find "${rr_dir}" -mindepth 1 -print -quit | grep -q . || return 0

  # Build the tree from rr-cache via a throwaway index so the real index
  # and work tree are untouched.
  local idx
  idx="$(mktemp -u)"
  GIT_INDEX_FILE="${idx}" git --work-tree="${rr_dir}" add -A -f
  tree="$(GIT_INDEX_FILE="${idx}" git write-tree)"
  rm -f "${idx}"

  # Chain onto the loaded cache commit so the push fast-forwards; force
  # anyway since the cache is a heuristic store (last writer wins, no data
  # correctness impact).
  local parent=()
  git rev-parse -q --verify "${RERERE_CACHE_REF}" >/dev/null 2>&1 \
    && parent=(-p "${RERERE_CACHE_REF}")
  if [[ -n "${parent[*]:-}" ]] \
     && [[ "$(git rev-parse "${RERERE_CACHE_REF}^{tree}")" == "${tree}" ]]; then
    return 0
  fi
  commit="$(git commit-tree "${tree}" ${parent[@]+"${parent[@]}"} \
    -m "forward-merge rerere cache $(date -u +%FT%TZ)")"
  git update-ref "${RERERE_CACHE_REF}" "${commit}"
  [[ "${1:-}" == "push" ]] || return 0
  git push -q -f "$(forward_remote)" "${RERERE_CACHE_REF}:${RERERE_CACHE_REF}"
}
