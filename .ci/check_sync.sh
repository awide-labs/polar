#!/bin/bash
# Audit the stable-branch sync invariant: for each adjacent pair
# (lower, higher), git log higher..lower must be empty. Drift is normal
# for a short window right after a lower-branch merge (the forward-merge
# job forwards it within minutes), so a commit only counts as a violation
# once it is older than SYNC_MAX_AGE_HOURS, or when the backlog exceeds
# SYNC_MAX_COUNT regardless of age.
#
# Usage: check_sync.sh
# Env:
#   SYNC_MAX_AGE_HOURS  age past which un-forwarded commits fail (default 24)
#   SYNC_MAX_COUNT      backlog size that fails regardless of age (default 20)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=forward-merge/lib-forward-merge.sh
source "${SCRIPT_DIR}/forward-merge/lib-forward-merge.sh"

readonly MAX_AGE_HOURS="${SYNC_MAX_AGE_HOURS:-24}"
readonly MAX_COUNT="${SYNC_MAX_COUNT:-20}"

main() {
  # Fetch all tips first so the comparison reflects the remote, not a stale
  # clone.
  fetch_stable_branches

  local branches=() b
  while IFS= read -r b; do branches+=("${b}"); done < <(stable_branches)
  if (( ${#branches[@]} < 2 )); then
    log_success "sync: fewer than two stable branches; nothing to check"
    exit 0
  fi

  local now violations=0 i lower higher
  now="$(date -u +%s)"

  for (( i = 0; i + 1 < ${#branches[@]}; i++ )); do
    lower="${branches[i]}"
    higher="${branches[i+1]}"

    local lines count=0 oldest_age=0 sha subject ts age
    lines="$(git log --format='%H%x09%ct%x09%s' \
      "$(stable_branch_ref "${higher}")..$(stable_branch_ref "${lower}")")"

    if [[ -z "${lines}" ]]; then
      log_success "✓ ${lower} -> ${higher}: in sync"
      continue
    fi

    echo "Drift ${lower} -> ${higher}:"
    while IFS=$'\t' read -r sha ts subject; do
      [[ -z "${sha}" ]] && continue
      count=$((count + 1))
      age=$(( (now - ts) / 3600 ))
      (( age > oldest_age )) && oldest_age=${age}
      printf '  %s  %3dh  %s\n' "${sha:0:8}" "${age}" "${subject}"
    done <<<"${lines}"

    if (( oldest_age >= MAX_AGE_HOURS || count >= MAX_COUNT )); then
      log_error "✗ ${lower} -> ${higher}: ${count} un-forwarded commit(s), oldest ${oldest_age}h (limits: ${MAX_AGE_HOURS}h / ${MAX_COUNT})"
      violations=$((violations + 1))
    else
      log_warning "  ${count} commit(s) pending forward, oldest ${oldest_age}h (within limits)"
    fi
  done

  if (( violations > 0 )); then
    log_error "sync: ${violations} branch pair(s) overdue for forward-merge."
    log_error "      Check the forward-merge workflow runs and the open forward/* pull requests;"
    log_error "      for a conflict, run '.ci/forward-merge/forward-merge.sh --prepare <higher>', resolve"
    log_error "      and --record, then re-run the forward-merge workflow."
    exit 1
  fi
  log_success "sync: all branch pairs within forward-merge limits"
}

main "$@"
