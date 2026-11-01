#!/bin/bash
# Check for commits excluded from validation due to merge commits
#
# This script identifies commits that are brought in by merge commits and
# therefore excluded from validation in other CI checks. It warns about
# these commits to provide visibility.
#
# Usage: check_merges.sh BASE_REF [CURRENT_REF]
#   BASE_REF: Base commit/ref to compare against (required)
#   CURRENT_REF: Current commit/ref (optional, defaults to HEAD)

set -euo pipefail

# Source common functions
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=common.sh
source "${SCRIPT_DIR}/common.sh"

usage() {
  echo "Usage: $0 BASE_REF [CURRENT_REF]"
  echo ""
  echo "  BASE_REF: Base commit/ref to compare against (required)"
  echo "  CURRENT_REF: Current commit/ref (optional, defaults to HEAD)"
  exit 1
}

is_forward_merge_automation() {
  local base="${1:-}" current="${2:-}"
  local bot_name="${FORWARD_BOT_NAME:-${FORWARD_BOT_LOGIN:-awide-polardb-bot}}"
  local bot_email="${FORWARD_BOT_EMAIL:-}"

  local sha
  while IFS= read -r sha; do
    [ -n "${sha}" ] || continue
    is_merge_commit "${sha}" || continue
    [ "$(git log -1 --format='%an' "${sha}" 2>/dev/null)" = "${bot_name}" ] && return 0
    [ -n "${bot_email}" ] \
      && [ "$(git log -1 --format='%ae' "${sha}" 2>/dev/null)" = "${bot_email}" ] && return 0
  done < <(get_commits_in_range "${base}" "${current}" "%H")

  return 1
}

main() {
  local base_ref="${1:-}"
  local current_ref="${2:-HEAD}"

  if [ -z "${base_ref}" ]; then
    log_error "Error: BASE_REF is required"
    echo ""
    usage
  fi

  echo "Base ref: ${base_ref}"
  echo "Current ref: ${current_ref}"
  echo ""

  # Get the list of commits in the range
  local commits
  commits=$(get_commits_in_range "${base_ref}" "${current_ref}" "%H")

  if [ -z "${commits}" ]; then
    log_success "No commits found in range"
    exit 0
  fi

  # Build excluded commits array
  local excluded_commits=()
  local excluded_sha
  while IFS= read -r excluded_sha; do
    if [ -n "${excluded_sha}" ]; then
      excluded_commits+=("${excluded_sha}")
    fi
  done <<< "$(get_excluded_commits "${base_ref}" "${current_ref}")"

  local count=${#excluded_commits[@]}
  if [ "${count}" -eq 0 ]; then
    log_success "✓ No commits excluded from validation"
    exit 0
  fi

  # Build warning message with list of excluded commits
  local warning_msg="**Warning:** ${count} commit(s) excluded from validation:"
  local sha
  for sha in "${excluded_commits[@]}"; do
    local subject
    subject=$(git log -1 --format="%s" "${sha}" 2>/dev/null || echo "unknown")
    warning_msg+=$'\n'"- ${sha:0:8}: ${subject}"
  done

  # Post to the PR for human merges; keep it log-only for the forward-merge
  # bot, whose brought commits were already validated.
  if is_forward_merge_automation "${base_ref}" "${current_ref}"; then
    log_warning_local "${warning_msg}"
  else
    log_warning "${warning_msg}"
  fi

  exit 0
}

main "$@"
