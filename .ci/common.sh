#!/bin/bash
# Common functions for CI check scripts
#
# This file contains shared utility functions used by multiple CI check
# scripts. Source this file in your script to use these functions.

_COMMON_SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [ -f "${_COMMON_SCRIPT_DIR}/../.bitbucket/pr-comments.sh" ]; then
  # shellcheck source=../.bitbucket/pr-comments.sh
  source "${_COMMON_SCRIPT_DIR}/../.bitbucket/pr-comments.sh"
fi
unset _COMMON_SCRIPT_DIR

# Conventional Commits regex patterns
readonly CC_OPTIONAL_SCOPE="(\\([^)]+\\))?"
readonly CC_HEADER_BASE="^[a-z]+${CC_OPTIONAL_SCOPE}(!)?:"
readonly CC_HEADER_WITH_DESC="${CC_HEADER_BASE}[[:space:]]+.+"
readonly CC_HEADER_BREAKING="^[a-z]+${CC_OPTIONAL_SCOPE}!:"

readonly CC_SED_HEADER="^([a-z]+)${CC_OPTIONAL_SCOPE}(!)?:"
readonly CC_SED_TYPE_EXTRACT="${CC_SED_HEADER}.*"
readonly CC_SED_DESC_EXTRACT="${CC_SED_HEADER}[[:space:]]+"

log_error() {
  echo -e "\033[0;31m$*\033[0m"
}

log_success() {
  echo -e "\033[0;32m$*\033[0m"
}

log_warning() {
  local warning_msg="$*"

  # Always print to console with colored output
  echo -e "\033[1;33m${warning_msg}\033[0m"

  # Call optional hook for CI-specific warning handlers if available
  # This allows CI-specific integrations (e.g., posting to MRs/PRs)
  # without coupling this CI-agnostic code to any specific platform
  if type on_warning_hook &>/dev/null; then
    on_warning_hook "${warning_msg}"
  fi
}

# Get list of commits in a range
# Usage: get_commits_in_range BASE_REF CURRENT_REF [FORMAT]
#   FORMAT: git log format string (default: "%H %s" for SHA and subject)
# Returns: commit list via stdout, empty if no commits
get_commits_in_range() {
  local base="${1:-}"
  local current="${2:-HEAD}"
  local format="${3:-%H %s}"

  if [ -z "${base}" ]; then
    return 1
  fi

  git log --format="${format}" "${base}..${current}" 2>/dev/null || true
}

# Get full commit message body
# Usage: get_commit_body COMMIT_SHA
# Returns: full commit message via stdout
get_commit_body() {
  local commit_sha="${1:-}"
  if [ -z "${commit_sha}" ]; then
    return 1
  fi
  git log -1 --format="%B" "${commit_sha}" 2>/dev/null
}

# Extract commit type from Conventional Commits format
# Usage: extract_commit_type COMMIT_MESSAGE
# Returns: commit type via stdout (empty if not found)
extract_commit_type() {
  local commit_msg="${1:-}"
  if [ -z "${commit_msg}" ]; then
    return 1
  fi

  if echo "${commit_msg}" | grep -qE "${CC_HEADER_BASE}"; then
    echo "${commit_msg}" | sed -E "s/${CC_SED_TYPE_EXTRACT}/\\1/"
  fi
}

# Check if a commit is a merge commit
# Usage: is_merge_commit COMMIT_SHA
# Returns: 0 if merge commit, 1 otherwise
is_merge_commit() {
  local commit_sha="${1:-}"
  if [ -z "${commit_sha}" ]; then
    return 1
  fi
  git log -1 --format="%P" "${commit_sha}" 2>/dev/null | \
    grep -qE "^[0-9a-f]+ " || return 1
}

# Get all commits brought in by a merge commit
# Usage: get_merge_commits COMMIT_SHA
# Returns: list of commit SHAs via stdout (empty if not a merge or no commits)
get_merge_commits() {
  local commit_sha="${1:-}"
  if [ -z "${commit_sha}" ]; then
    return 1
  fi

  # Check if it's a merge commit (has multiple parents)
  local parents
  parents=$(git log -1 --format="%P" "${commit_sha}" 2>/dev/null)
  if [ -z "${parents}" ]; then
    return 1
  fi

  # Count parents (space-separated)
  local parent_count
  parent_count=$(echo "${parents}" | wc -w)
  if [ "${parent_count}" -lt 2 ]; then
    return 1
  fi

  # Get first parent (the branch being merged into)
  local first_parent
  first_parent=$(echo "${parents}" | cut -d' ' -f1)

  # Get second parent (the branch being merged)
  local second_parent
  second_parent=$(echo "${parents}" | cut -d' ' -f2)

  # Get all commits in second parent that are not in first parent
  git log --format="%H" "${first_parent}..${second_parent}" \
    2>/dev/null || true
}

# Get list of commits to exclude (commits brought by top-level merges)
# Top-level merges are validated, but their brought commits (including nested
# merges) are excluded.
# Usage: get_excluded_commits BASE_REF CURRENT_REF
# Returns: newline-separated list of commit SHAs via stdout
get_excluded_commits() {
  local base_ref="${1:-}"
  local current_ref="${2:-HEAD}"

  if [ -z "${base_ref}" ]; then
    return 1
  fi

  # Get all commit SHAs in the range
  local commits
  commits=$(get_commits_in_range "${base_ref}" "${current_ref}" "%H")

  if [ -z "${commits}" ]; then
    return 0
  fi

  # Step 1: Collect all commits brought by any merge in the range
  local -a all_brought_commits=()
  local sha
  while IFS= read -r sha; do
    if [ -z "$sha" ]; then
      continue
    fi
    if is_merge_commit "$sha"; then
      local brought
      brought=$(get_merge_commits "$sha")
      if [ -n "$brought" ]; then
        local brought_sha
        while IFS= read -r brought_sha; do
          if [ -n "$brought_sha" ]; then
            all_brought_commits+=("$brought_sha")
          fi
        done <<< "$brought"
      fi
    fi
  done <<< "${commits}"

  # Step 2: Identify top-level merges (merges not brought by another merge)
  # and output commits to skip (brought by top-level merges + nested merges)
  while IFS= read -r sha; do
    if [ -z "$sha" ]; then
      continue
    fi
    if is_merge_commit "$sha"; then
      # Check if this merge is brought by another merge (nested merge)
      if is_commit_excluded "$sha" "all_brought_commits"; then
        # This is a nested merge - skip it, don't process its brought commits
        echo "$sha"
        continue
      fi
      # This is a top-level merge - output its brought commits to skip
      local brought
      brought=$(get_merge_commits "$sha")
      if [ -n "$brought" ]; then
        local brought_sha
        while IFS= read -r brought_sha; do
          if [ -n "$brought_sha" ]; then
            echo "$brought_sha"
          fi
        done <<< "$brought"
      fi
    fi
  done <<< "$commits"
}

# Check if a commit is in the excluded list
# Usage: is_commit_excluded COMMIT_SHA EXCLUDED_ARRAY
# Returns: 0 if excluded, 1 otherwise
is_commit_excluded() {
  local commit_sha="${1:-}"
  local excluded_array_name="${2:-}"

  if [ -z "${commit_sha}" ] || [ -z "${excluded_array_name}" ]; then
    return 1
  fi

  # Use indirect array reference
  local excluded_array
  eval "excluded_array=(\"\${${excluded_array_name}[@]}\")"

  local excluded
  for excluded in "${excluded_array[@]}"; do
    if [ "${commit_sha}" = "${excluded}" ]; then
      return 0
    fi
  done
  return 1
}
