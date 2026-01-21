#!/bin/bash
# Common functions and constants for CI scripts
#
# This file provides shared functionality for commit format checking and
# changelog validation scripts.

# Enable strict mode for sourcing
set -euo pipefail

# ============================================================================
# CONSTANTS
# ============================================================================

# Conventional Commits types (from specification)
# https://www.conventionalcommits.org/en/v1.0.0/
CC_TYPES="feat|fix|docs|style|refactor|perf|test|build|ci|chore|revert"

# Regex patterns for Conventional Commits
# Header format: <type>[optional scope][optional !]: <description>
CC_HEADER_PATTERN="^(${CC_TYPES})(\([a-zA-Z0-9_/-]+\))?(!)?: .+"
CC_HEADER_BREAKING="^(${CC_TYPES})(\([a-zA-Z0-9_/-]+\))?!: "

# Maximum line length for commit messages
CC_MAX_LINE_LENGTH=72

# ============================================================================
# LOGGING FUNCTIONS
# ============================================================================

# Print a message in red (error)
log_error() {
  echo -e "\033[0;31m$*\033[0m"
}

# Print a message in green (success)
log_success() {
  echo -e "\033[0;32m$*\033[0m"
}

# Print a message in yellow (warning)
log_warning() {
  echo -e "\033[0;33m$*\033[0m"
}

# ============================================================================
# GIT HELPER FUNCTIONS
# ============================================================================

# Get list of commits in a range
# Usage: get_commits_in_range BASE_REF CURRENT_REF [FORMAT]
# Returns: newline-separated list via stdout
get_commits_in_range() {
  local base_ref="${1:-}"
  local current_ref="${2:-HEAD}"
  local format="${3:-%H %s}"

  if [ -z "${base_ref}" ]; then
    return 1
  fi

  git log --format="${format}" --reverse "${base_ref}..${current_ref}" \
    2>/dev/null || true
}

# Get the full commit message body (including subject)
# Usage: get_commit_body COMMIT_SHA
# Returns: full commit message via stdout
get_commit_body() {
  local commit_sha="${1:-}"
  if [ -z "${commit_sha}" ]; then
    return 1
  fi
  git log -1 --format="%B" "${commit_sha}" 2>/dev/null || true
}

# Extract commit type from Conventional Commits header
# Usage: extract_commit_type COMMIT_MSG
# Returns: type string via stdout (empty if not matching)
extract_commit_type() {
  local commit_msg="${1:-}"
  if [ -z "${commit_msg}" ]; then
    return 1
  fi
  # Extract type from: type(scope): description or type: description
  echo "${commit_msg}" | sed -nE "s/^(${CC_TYPES})(\([^)]+\))?(!)?: .+/\1/p"
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

# Get list of commits to exclude (merge commits and their brought commits)
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

  # Build list of excluded commits
  local commit_sha
  while IFS= read -r commit_sha; do
    if [ -z "${commit_sha}" ]; then
      continue
    fi
    # Check if it's a merge commit
    if is_merge_commit "${commit_sha}"; then
      # Add the merge commit itself
      echo "${commit_sha}"
      # Get all commits brought in by this merge
      local merge_commits
      merge_commits=$(get_merge_commits "${commit_sha}")
      if [ -n "${merge_commits}" ]; then
        while IFS= read -r merge_commit; do
          if [ -n "${merge_commit}" ]; then
            echo "${merge_commit}"
          fi
        done <<< "${merge_commits}"
      fi
    fi
  done <<< "${commits}"
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
