#!/bin/bash
# Pairing check: a pull request into a lower stable branch may not merge
# until its forward-merge to the next-higher branch is proven feasible.
#
# "Feasible" means one of: the change merges cleanly into the next branch,
# the author marked it as a null-merge (does not apply), or the author
# already recorded the conflict resolution into the shared rerere cache.
# In all three cases the forward-merge can be finished unattended after
# this pull request lands; otherwise the check fails and tells the author
# what to do.
#
# Usage: check-can-forward-merge.sh [TARGET_BRANCH] [PR_HEAD]
#   TARGET_BRANCH defaults to GITHUB_BASE_REF.
#   PR_HEAD       the pull request head commit; defaults to HEAD. In CI pass
#                 it explicitly: depending on the checkout, HEAD can be the
#                 target tip, which is already contained in the next branch,
#                 so the trial merge would be a no-op and the check would
#                 pass blindly.
# Null-merge labels are read from FORWARD_LABELS (comma-separated).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib-forward-merge.sh
source "${SCRIPT_DIR}/lib-forward-merge.sh"

main() {
  local target="${1:-${GITHUB_BASE_REF:-}}"
  local head="${2:-HEAD}"

  if [[ -z "${target}" ]]; then
    log_warning "can-forward-merge: no target branch in context; skipping"
    exit 0
  fi
  fetch_stable_branches
  if [[ -z "$(stable_branch_num "${target}")" ]]; then
    log_success "can-forward-merge: target '${target}' is not a stable branch; nothing to pair"
    exit 0
  fi

  local next
  next="$(next_higher_branch "${target}")"
  if [[ -z "${next}" ]]; then
    log_success "can-forward-merge: '${target}' is the highest stable branch; nothing to forward"
    exit 0
  fi

  echo "Target branch : ${target}"
  echo "Forwards to   : ${next}"
  echo

  rerere_enable
  rerere_load

  local base
  base="$(stable_branch_ref "${target}")"
  # Shallow CI clones may not yet connect the pull request head to the
  # target (or to the next branch). Deepen before reading trailers or
  # trial-merging; trial_merge also calls this for the next-branch pair.
  ensure_merge_base "${base}" "${head}" || true
  ensure_merge_base "$(stable_branch_ref "${next}")" "${head}" || true

  if [[ "$(forward_intent "${next}" "${base}" "${head}")" == "null" ]]; then
    log_success "can-forward-merge: change is marked null-merge for ${next}; pairing satisfied"
    exit 0
  fi

  local conflicts rc
  conflicts="$(trial_merge "${next}" "${head}" "${target}")" && rc=0 || rc=$?
  if [[ "${rc}" -eq 0 ]]; then
    log_success "✓ can-forward-merge: merges cleanly into ${next}"
    exit 0
  fi
  if [[ "${rc}" -eq 3 ]]; then
    log_error "✗ can-forward-merge: un-forwarded commits already on ${target} conflict with ${next}; fix the pending forward-merge first"
    exit 1
  fi
  if [[ "${rc}" -eq 2 ]]; then
    log_error "✗ can-forward-merge: could not evaluate the forward-merge to ${next} (see above); failing closed"
    exit 1
  fi

  local msg
  msg="Forward-merge to ${next} has conflicts. This pull request is"
  msg+=" blocked until the resolution is recorded, so the forward-merge"
  msg+=" can be finished after it lands. Conflicting paths:"$'\n'
  local path
  while IFS= read -r path; do
    [[ -n "${path}" ]] && msg+=$'\n'"    ${path}"
  done <<<"${conflicts}"
  msg+=$'\n\n'"Resolve once, locally, from this pull request branch:"$'\n'
  msg+=$'\n'"    .ci/forward-merge/forward-merge.sh --prepare ${next}"
  msg+=$'\n\n'"Resolve the conflicts (or run 'git merge -s ours' for a"
  msg+=" null-merge if the change does not apply to ${next}), then let"
  msg+=" the helper push the recorded resolution. Re-run this check and"
  msg+=" it will pass. To skip forwarding entirely, label the pull request"
  msg+=" forward:null-${next} or add a Forward-as-null: ${next}"
  msg+=" commit trailer."
  log_error "${msg}"

  log_error "✗ can-forward-merge: unresolved forward-merge to ${next}"
  exit 1
}

main "$@"
