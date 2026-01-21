#!/bin/bash
# Check if commits comply with Conventional Commits specification
#
# This script verifies that all commits in a given range follow the
# Conventional Commits specification format.
#
# Usage: check_commit_format.sh BASE_REF [CURRENT_REF]
#   BASE_REF: Base commit/ref to compare against (required)
#   CURRENT_REF: Current commit/ref (optional, defaults to HEAD)

set -euo pipefail

# Source common functions
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=.ci/common.sh
source "${SCRIPT_DIR}/common.sh"

usage() {
  echo "Usage: $0 BASE_REF [CURRENT_REF]"
  echo ""
  echo "  BASE_REF: Base commit/ref to compare against (required)"
  echo "  CURRENT_REF: Current commit/ref (optional, defaults to HEAD)"
  exit 1
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
    log_warning "No commits found in range, skipping check"
    exit 0
  fi

  # Build set of commits to exclude (merge commits and their brought commits)
  local excluded_commits=()
  local excluded_sha
  while IFS= read -r excluded_sha; do
    if [ -n "${excluded_sha}" ]; then
      excluded_commits+=("${excluded_sha}")
    fi
  done <<< "$(get_excluded_commits "${base_ref}" "${current_ref}")"

  echo "Checking commits for Conventional Commits compliance..."
  echo "---"

  local invalid_commits=()
  local commit_count=0

  local commit_sha
  while IFS= read -r commit_sha; do
    if [ -z "${commit_sha}" ]; then
      continue
    fi

    # Skip excluded commits (merge commits and their brought commits)
    if is_commit_excluded "${commit_sha}" "excluded_commits"; then
      local commit_msg
      commit_msg=$(git log -1 --format="%s" "${commit_sha}" 2>/dev/null)
      if is_merge_commit "${commit_sha}"; then
        log_success "  ✓ ${commit_sha:0:8} - ${commit_msg} (merge commit)"
      else
        log_success "  ✓ ${commit_sha:0:8} - ${commit_msg} " \
                    "(brought by merge)"
      fi
      continue
    fi

    commit_count=$((commit_count + 1))
    local commit_msg
    commit_msg=$(git log -1 --format="%s" "${commit_sha}")
    local full_msg
    full_msg=$(get_commit_body "${commit_sha}")

    # Check if commit message follows Conventional Commits format
    # Format: <type>[optional scope]: <description>
    local is_valid=true
    local errors=()

    # Check header format
    if ! echo "${commit_msg}" | grep -qE "${CC_HEADER_PATTERN}"; then
      is_valid=false
      errors+=("Header doesn't match Conventional Commits format")
    fi

    # Check header length (should be <= 72 characters)
    local header_length=${#commit_msg}
    if [ "${header_length}" -gt "${CC_MAX_LINE_LENGTH}" ]; then
      is_valid=false
      errors+=("Header exceeds ${CC_MAX_LINE_LENGTH} characters " \
               "(${header_length} chars)")
    fi

    # Check body line lengths (if body exists)
    local body_lines
    body_lines=$(echo "${full_msg}" | tail -n +2)
    local line_num=2
    while IFS= read -r line; do
      local line_length=${#line}
      if [ "${line_length}" -gt "${CC_MAX_LINE_LENGTH}" ]; then
        is_valid=false
        errors+=("Line ${line_num} exceeds ${CC_MAX_LINE_LENGTH} characters " \
                 "(${line_length} chars)")
      fi
      line_num=$((line_num + 1))
    done <<< "${body_lines}"

    # Check for Refs: footer (mandatory)
    if ! echo "${full_msg}" | grep -qE "^Refs: .+"; then
      is_valid=false
      errors+=("Missing 'Refs:' footer (e.g., 'Refs: ISSUE-123')")
    fi

    if [ "${is_valid}" = "true" ]; then
      log_success "  ✓ ${commit_sha:0:8} - ${commit_msg}"
    else
      log_error "  ✗ ${commit_sha:0:8} - ${commit_msg}"
      local error
      for error in "${errors[@]}"; do
        log_error "      → ${error}"
      done
      invalid_commits+=("${commit_sha}")
    fi
  done <<< "${commits}"

  echo "---"

  if [ ${#invalid_commits[@]} -eq 0 ]; then
    log_success "✓ All ${commit_count} commit(s) follow Conventional " \
                "Commits specification"
    exit 0
  else
    echo ""
    log_error "✗ ${#invalid_commits[@]} commit(s) do not follow " \
              "Conventional Commits specification"
    echo ""
    echo "Commit message format:"
    echo "  <type>[optional scope]: <description>"
    echo ""
    echo "  [optional body]"
    echo ""
    echo "  Refs: <reference>"
    echo ""
    echo "Valid types: feat, fix, docs, style, refactor, perf, test, " \
         "build, ci, chore, revert"
    echo ""
    echo "See https://www.conventionalcommits.org/ for more information."
    exit 1
  fi
}

main "$@"
