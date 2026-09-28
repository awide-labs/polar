#!/bin/bash
# Check if CHANGELOG.md needs to be updated based on Conventional Commits
#
# This script analyzes commits in a range to determine if they contain
# user-visible changes (feat, fix, perf, or BREAKING CHANGE) and verifies
# that CHANGELOG.md has been updated accordingly. Changelog entries added in
# commits that do not require an update are still validated for format.
#
# Usage: check_changelog.sh BASE_REF [CURRENT_REF]
#   BASE_REF: Base commit/ref to compare against (required)
#   CURRENT_REF: Current commit/ref (optional, defaults to HEAD)

set -euo pipefail

# Source common functions
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck disable=SC1091
source "${SCRIPT_DIR}/common.sh"

# Regex for numbered release heading: ## [<version>] - YYYY-MM-DD
# Version is any non-empty string; date is ISO 8601 calendar date
RELEASE_HEADING_PATTERN='^## \[.+\] - [0-9]{4}-(0[1-9]|1[0-2])-(0[1-9]|[12][0-9]|3[01])$'

# Regex for URL definition line (Keep a Changelog link format): [version]: URL
URL_DEFINITION_PATTERN='^\[.+\]:\s*.+'

readonly CHANGELOG="CHANGELOG.md"

# Jira issue reference(s) in parentheses, comma-separated when multiple
readonly CHANGELOG_ISSUE_REFS='\([A-Z]+-[0-9]+(,[[:space:]]*[A-Z]+-[0-9]+)*\)'
readonly CHANGELOG_ISSUE_END_RE="${CHANGELOG_ISSUE_REFS}(\\.|\\:)?\$"
readonly CHANGELOG_ISSUE_SUBLIST_RE="${CHANGELOG_ISSUE_REFS}:\$"

# Check whether a commit modifies CHANGELOG.md
# Usage: commit_modifies_changelog COMMIT_SHA
# Returns: 0 if modified, 1 otherwise
commit_modifies_changelog() {
  local commit_sha="${1:-}"

  if [ -z "${commit_sha}" ]; then
    return 1
  fi

  # Scope `git show` to the changelog path so non-empty output means it was
  # modified. Avoids a SIGPIPE/pipefail bug when piping a long file list
  # into `grep -q`.
  [ -n "$(git show --name-only --format="" "${commit_sha}" -- \
          "${CHANGELOG}" 2>/dev/null)" ]
}

# Check changelog entry format for a specific commit
# Each paragraph starting with '- ' must end with a Jira issue reference on
# the first line or on a wrapped continuation line. Sub-items (indented '- ')
# must not carry issue references.
# Usage: check_changelog_entry_format COMMIT_SHA
# Returns: 0 if valid, 1 if invalid (with errors printed)
check_changelog_entry_format() {
  local commit_sha="${1:-}"

  if [ -z "${commit_sha}" ]; then
    return 1
  fi

  # Indented changelog sub-item: "  - item"
  is_changelog_sub_item() {
    [[ "${1}" =~ ^[[:space:]]+-[[:space:]] ]]
  }

  # Indented continuation of paragraph text (not a sub-item)
  is_changelog_continuation() {
    [ -n "${1}" ] && [[ "${1}" =~ ^[[:space:]]+.+ ]] && ! is_changelog_sub_item "${1}"
  }

  # Get the diff for the changelog file in this commit (added lines only)
  local diff_output
  diff_output=$(git show --format="" --no-color "${commit_sha}" -- \
                "${CHANGELOG}" 2>/dev/null | \
                grep '^+' | grep -v '^+++' | sed 's/^+//' || true)

  if [ -z "${diff_output}" ]; then
    # No additions to changelog
    return 0
  fi

  # Process the diff to find paragraphs starting with '- '
  # A paragraph is a block starting with '- ' and continuing with indented
  # continuation lines or sub-items (lines starting with '  - ')
  local in_paragraph=false
  local current_paragraph=""
  local paragraph_start_line=""
  local issue_candidate=""
  local has_issue=false
  local invalid_entries=()
  local subitem_issue_entries=()
  local missing_space_entries=()
  local line_num=0
  
  while IFS= read -r line || [ -n "${line}" ]; do
    line_num=$((line_num + 1))

    if [ -z "${line}" ] || is_changelog_continuation "${line}" || \
       is_changelog_sub_item "${line}"; then
      if [ "${in_paragraph}" = "true" ]; then
        current_paragraph+=$'\n'"${line}"

        if is_changelog_sub_item "${line}"; then
          if echo "${line}" | grep -qE "${CHANGELOG_ISSUE_REFS}"; then
            subitem_issue_entries+=("${line}")
          fi
        elif is_changelog_continuation "${line}"; then
          # Only wrapped continuation lines may carry the paragraph issue ref
          local trimmed
          trimmed="${line#"${line%%[![:space:]]*}"}"
          if [ -n "${trimmed}" ] && [ "${has_issue}" != "true" ]; then
            issue_candidate="${line}"
          fi
        fi
      fi
    else
      # If we were in a paragraph, validate it now
      if [ "${in_paragraph}" = "true" ] && [ "${has_issue}" != "true" ]; then
        if ! echo "${issue_candidate}" | grep -qE "${CHANGELOG_ISSUE_END_RE}"; then
          invalid_entries+=("${paragraph_start_line}")
        fi
      fi

      # Check if this line starts a new top-level entry (dash at column 0)
      if [[ "${line}" =~ ^-\ .+ ]]; then
        # Start new paragraph
        in_paragraph=true
        current_paragraph="${line}"
        paragraph_start_line="${line}"
        issue_candidate="${line}"
      else
        # End last paragraph
        in_paragraph=false
        current_paragraph=""
        paragraph_start_line=""
        issue_candidate=""
      fi

      has_issue=false
    fi

    # Paragraph issue ref on the first line or a continuation line, optionally
    # followed by ':' when sub-items follow.
    if [ "${in_paragraph}" = "true" ] && [ "${has_issue}" != "true" ]; then
      if echo "${issue_candidate}" | grep -qE "${CHANGELOG_ISSUE_SUBLIST_RE}"; then
        has_issue=true
        issue_candidate=""
      fi
    fi

    # Flag issue references glued to the preceding word (missing space)
    if echo "${line}" | grep -qE "[^ ]${CHANGELOG_ISSUE_REFS}"; then
      missing_space_entries+=("${line}")
    fi
  done <<< "${diff_output}"

  # Check the last paragraph if we're still in one
  if [ "${in_paragraph}" = "true" ] && [ "${has_issue}" != "true" ]; then
    if ! echo "${issue_candidate}" | grep -qE "${CHANGELOG_ISSUE_END_RE}"; then
      invalid_entries+=("${paragraph_start_line}")
    fi
  fi

  # Report missing-space entries
  if [ ${#missing_space_entries[@]} -gt 0 ]; then
    log_error "    Issue reference must be preceded by a space:"
    for entry in "${missing_space_entries[@]}"; do
      local display_entry="${entry}"
      if [ ${#entry} -gt 60 ]; then
        display_entry="${entry:0:57}..."
      fi
      log_error "      → ${display_entry}"
    done
  fi

  # Report issue references on sub-items
  if [ ${#subitem_issue_entries[@]} -gt 0 ]; then
    log_error "    Issue references are only allowed on the paragraph line,"
    log_error "    not on sub-items:"
    for entry in "${subitem_issue_entries[@]}"; do
      local display_entry="${entry}"
      if [ ${#entry} -gt 60 ]; then
        display_entry="${entry:0:57}..."
      fi
      log_error "      → ${display_entry}"
    done
  fi

  # Report invalid entries
  if [ ${#invalid_entries[@]} -gt 0 ]; then
    log_error "    Invalid changelog entries (must end with Jira reference):"
    for entry in "${invalid_entries[@]}"; do
      # Truncate long entries for display
      local display_entry="${entry}"
      if [ ${#entry} -gt 60 ]; then
        display_entry="${entry:0:57}..."
      fi
      log_error "      → ${display_entry}"
    done
  fi

  if [ ${#invalid_entries[@]} -gt 0 ] || \
     [ ${#missing_space_entries[@]} -gt 0 ] || \
     [ ${#subitem_issue_entries[@]} -gt 0 ]; then
    return 1
  fi

  return 0
}

# Check that each numbered release has a heading in format "## [<version>] - YYYY-MM-DD"
# [Unreleased] is allowed without a date; all other releases must have the date.
# Uses the currently checked out CHANGELOG file.
# Usage: check_release_headings
# Returns: 0 if all valid, 1 if invalid (with errors printed)
check_release_headings() {
  local invalid_headings=()
  local line

  if [ ! -f "${CHANGELOG}" ]; then
    return 0
  fi

  if ! grep -n '^## \[' "${CHANGELOG}" >/dev/null 2>&1; then
    return 0
  fi

  while IFS= read -r line; do
    [ -z "${line}" ] && continue
    # Skip [Unreleased] - it does not require a date
    if echo "${line}" | grep -qE '^## \[Unreleased\]( - .*)?$'; then
      continue
    fi
    if ! echo "${line}" | grep -qE "${RELEASE_HEADING_PATTERN}"; then
      invalid_headings+=("${line}")
    fi
  done < <(grep '^## \[' "${CHANGELOG}" || true)

  if [ ${#invalid_headings[@]} -gt 0 ]; then
    log_error "    Release headings must use format: ## [<version>] - YYYY-MM-DD"
    for heading in "${invalid_headings[@]}"; do
      log_error "      → ${heading}"
    done
    return 1
  fi
  return 0
}

# Check that all URL definitions ([version]: URL) are at the end of the file.
# Once a URL definition appears, only URL definitions and blank lines are allowed.
# Uses the currently checked out CHANGELOG file.
# Usage: check_url_definitions_at_end
# Returns: 0 if valid, 1 if invalid (with errors printed)
check_url_definitions_at_end() {
  local line_num=0
  local in_url_section=false
  local invalid_lines=()

  if [ ! -f "${CHANGELOG}" ]; then
    return 0
  fi

  while IFS= read -r line; do
    line_num=$((line_num + 1))
    if echo "${line}" | grep -qE "${URL_DEFINITION_PATTERN}"; then
      in_url_section=true
    elif [ "${in_url_section}" = "true" ] && [ -n "${line}" ]; then
      invalid_lines+=("${line_num}: ${line}")
    fi
  done < "${CHANGELOG}"

  if [ ${#invalid_lines[@]} -gt 0 ]; then
    log_error "    URL definitions must be at the end of ${CHANGELOG}; " \
              "no other content is allowed after them."
    for entry in "${invalid_lines[@]}"; do
      log_error "      → line ${entry}"
    done
    return 1
  fi
  return 0
}

# Check if a commit adds changelog entries (lines starting with "- ") under a
# past release section instead of [Unreleased]. Such additions are discouraged
# because they rewrite history of an already-released version.
# Usage: check_entries_added_to_past_release COMMIT_SHA
# Output: warning lines to stdout (one per offending entry), nothing if none
# Returns: 0 if no such entries, 1 if any (warnings still printed to stdout)
check_entries_added_to_past_release() {
  local commit_sha="${1:-}"
  local diff_output
  local content_after
  local current_section=""
  local line
  local added_list_file

  if [ -z "${commit_sha}" ]; then
    return 0
  fi

  # Get added lines in CHANGELOG in this commit
  diff_output=$(git show --format="" --no-color "${commit_sha}" -- \
                "${CHANGELOG}" 2>/dev/null | \
                grep '^+' | grep -v '^+++' | sed 's/^+//' || true)

  # Collect top-level bullet lines that were added
  added_list_file=$(mktemp)
  while IFS= read -r line || [ -n "${line}" ]; do
    if [[ "${line}" =~ ^-\ .+ ]]; then
      printf '%s\n' "${line}" >> "${added_list_file}"
    fi
  done <<< "${diff_output}"

  if [ ! -s "${added_list_file}" ]; then
    rm -f "${added_list_file}"
    return 0
  fi

  # Get CHANGELOG content at this commit (after the change)
  content_after=$(git show "${commit_sha}:${CHANGELOG}" 2>/dev/null) || {
    rm -f "${added_list_file}"
    return 0
  }

  # Walk content and emit a warning for each added "- " line under a past release
  current_section=""
  while IFS= read -r line || [ -n "${line}" ]; do
    if [[ "${line}" =~ ^##\ \[.+\] ]]; then
      current_section=$(echo "${line}" | sed -n 's/^## \[\([^]]*\)\].*/\1/p')
    elif [[ "${line}" =~ ^-\ .+ ]]; then
      if [ "${current_section}" != "Unreleased" ] && [ -n "${current_section}" ]; then
        if grep -qF -- "${line}" "${added_list_file}" 2>/dev/null; then
          echo "- ${commit_sha:0:8}: entry under \`[${current_section}]\`: ${line}"
        fi
      fi
    fi
  done <<< "${content_after}"
  rm -f "${added_list_file}"
  return 0
}

# Check that each release heading (## [version] or ## [Unreleased]) has a
# corresponding URL definition ([version]: URL or [Unreleased]: URL) in the file.
# Matching is case-insensitive. Uses the currently checked out CHANGELOG file.
# Usage: check_release_url_definitions
# Returns: 0 if all valid, 1 if invalid (with errors printed)
check_release_url_definitions() {
  local release
  local release_lower
  local defined_keys
  local missing=()

  if [ ! -f "${CHANGELOG}" ]; then
    return 0
  fi

  defined_keys=$(sed -n 's/^\[\([^]]*\)\]:.*/\1/p' "${CHANGELOG}" | \
                tr '[:upper:]' '[:lower:]')

  while IFS= read -r release; do
    [ -z "${release}" ] && continue
    release_lower=$(echo "${release}" | tr '[:upper:]' '[:lower:]')
    if ! echo "${defined_keys}" | grep -qFx "${release_lower}"; then
      missing+=("${release}")
    fi
  done < <(sed -n 's/^## \[\([^]]*\)\].*/\1/p' "${CHANGELOG}")

  if [ ${#missing[@]} -gt 0 ]; then
    log_error "    Each release heading must have a URL definition at the " \
              "end of ${CHANGELOG}."
    for release in "${missing[@]}"; do
      log_error "      → [${release}] is missing a \"[${release}]: <URL>\" definition"
    done
    return 1
  fi
  return 0
}

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
  echo "Changelog file: ${CHANGELOG}"
  echo ""

  # Get the list of commits in the range
  local commits
  commits=$(get_commits_in_range "${base_ref}" "${current_ref}")

  if [ -z "${commits}" ]; then
    log_warning "No commits found in range, skipping changelog check"
    exit 0
  fi

  local excluded_commits=()
  local excluded_sha
  while IFS= read -r excluded_sha; do
    if [ -n "${excluded_sha}" ]; then
      excluded_commits+=("${excluded_sha}")
    fi
  done <<< "$(get_excluded_commits "${base_ref}" "${current_ref}")"

  echo "Analyzing commits for user-visible changes..."
  echo "---"

  local needs_changelog=false
  local commits_missing_changelog=()
  local commits_invalid_format=()
  local commits_skip_changelog=()
  local entries_in_past_release=()

  local line
  while IFS= read -r line; do
    if [ -z "${line}" ]; then
      continue
    fi

    local commit_sha commit_msg
    commit_sha=$(echo "${line}" | cut -d' ' -f1)
    commit_msg=$(echo "${line}" | cut -d' ' -f2-)

    if is_commit_excluded "$commit_sha" "excluded_commits"; then
      log_success "  ✓ ${commit_sha:0:8} - $commit_msg (brought by merge)"
      continue
    fi

    # If this commit touches CHANGELOG, check for entries added under a past release
    if commit_modifies_changelog "${commit_sha}"; then
      local warn_line
      while IFS= read -r warn_line || [ -n "${warn_line}" ]; do
        if [ -n "${warn_line}" ]; then
          entries_in_past_release+=("${warn_line}")
        fi
      done < <(check_entries_added_to_past_release "${commit_sha}" 2>/dev/null || true)
    fi

    # Get full commit message body
    local full_msg
    full_msg=$(get_commit_body "${commit_sha}")

    # Detect breaking changes before skip-changelog: breaking changes always
    # require a changelog update, even for types like chore or docs.
    local is_breaking=false

    # Check for breaking change indicator in type (type! or type(scope)!:)
    if echo "${commit_msg}" | grep -qE "${CC_HEADER_BREAKING}"; then
      is_breaking=true
    fi

    # Check for BREAKING CHANGE in commit message body
    if echo "${full_msg}" | grep -qiE "^BREAKING CHANGE:|^BREAKING:"; then
      is_breaking=true
    fi

    # Check if commit explicitly skips changelog update
    # Uses Conventional Commits footer format: skip-changelog: true
    # Breaking changes cannot be skipped.
    if [ "${is_breaking}" = "false" ] && \
       echo "${full_msg}" | grep -qiE "^skip-changelog:\s*true"; then
      commits_skip_changelog+=("${commit_sha:0:8} - ${commit_msg}")
      log_success "  ✓ ${commit_sha:0:8} - ${commit_msg} " \
                  "(skip-changelog)"
      continue
    fi

    # Extract commit type from Conventional Commits format
    # Format: <type>[optional scope][optional !]: <description>
    local commit_type
    commit_type=$(extract_commit_type "${commit_msg}")

    # Check if this commit has user-visible changes requiring changelog
    # feat, fix, perf, or breaking changes require changelog updates
    local has_user_visible_changes=false

    if [ "${is_breaking}" = "true" ]; then
      has_user_visible_changes=true
    fi

    # Check for user-visible commit types
    case "${commit_type}" in
      feat|fix|perf)
        has_user_visible_changes=true
        ;;
    esac

    local changelog_in_commit=false
    if commit_modifies_changelog "${commit_sha}"; then
      changelog_in_commit=true
    fi

    # Validate format whenever this commit touches the changelog, even when
    # an update is not required for the commit type.
    local format_valid=true
    if [ "${changelog_in_commit}" = "true" ]; then
      if ! check_changelog_entry_format "${commit_sha}"; then
        format_valid=false
        local format_entry="${commit_type:-other}: ${commit_msg} "
        format_entry+="(${commit_sha:0:8})"
        commits_invalid_format+=("${format_entry}")
      fi
    fi

    # Determine display type for reporting
    local display_type="${commit_type}"
    if [ "${is_breaking}" = "true" ]; then
      display_type="BREAKING"
    fi

    if [ "${has_user_visible_changes}" = "true" ]; then
      needs_changelog=true

      if [ "${changelog_in_commit}" = "false" ]; then
        local missing_entry="${display_type}: ${commit_msg} "
        missing_entry+="(${commit_sha:0:8})"
        commits_missing_changelog+=("${missing_entry}")
        log_error "  ✗ ${commit_sha:0:8} - ${commit_msg} " \
                  "(missing ${CHANGELOG} update)"
      elif [ "${format_valid}" = "true" ]; then
        log_success "  ✓ ${commit_sha:0:8} - ${commit_msg} " \
                    "(${display_type}, ${CHANGELOG} updated)"
      else
        log_error "  ✗ ${commit_sha:0:8} - ${commit_msg} " \
                  "(${CHANGELOG} format error)"
      fi
    elif [ "${changelog_in_commit}" = "true" ]; then
      if [ "${format_valid}" = "true" ]; then
        log_success "  ✓ ${commit_sha:0:8} - ${commit_msg} " \
                    "(no changelog needed, ${CHANGELOG} format OK)"
      else
        log_error "  ✗ ${commit_sha:0:8} - ${commit_msg} " \
                  "(${CHANGELOG} format error)"
      fi
    else
      log_success "  ✓ ${commit_sha:0:8} - ${commit_msg} " \
                  "(no changelog needed)"
    fi
  done <<< "${commits}"

  # Post a single combined warning for all skip-changelog commits
  if [ ${#commits_skip_changelog[@]} -gt 0 ]; then
    local combined_msg
    combined_msg="The following commits skip changelog (Skip-changelog: true):"
    local entry
    for entry in "${commits_skip_changelog[@]}"; do
      combined_msg+=$'\n'"  - ${entry}"
    done
    log_warning "${combined_msg}"
  fi

  # Warn if any changelog entries were added under a past release instead of Unreleased
  if [ ${#entries_in_past_release[@]} -gt 0 ]; then
    local past_release_msg
    past_release_msg="Changelog entries should be added under **\[Unreleased\]**, not under a past release. The following entries were added under a released version:"
    past_release_msg+=$'\n\n'
    local entry
    for entry in "${entries_in_past_release[@]}"; do
      past_release_msg+=$'\n'"${entry}"
    done
    past_release_msg+=$'\n\n'"Consider moving these to the **\[Unreleased\]** section."
    log_warning "${past_release_msg}"
  fi

  echo "---"

  local has_errors=false

  # Check release heading format (## [<version>] - YYYY-MM-DD)
  if ! check_release_headings; then
    has_errors=true
    echo ""
    log_error "✗ ${CHANGELOG}: each numbered release must have a heading " \
              "in the format: ## [<version>] - YYYY-MM-DD"
  fi

  # Check that all URL definitions are at the end of the file
  if ! check_url_definitions_at_end; then
    has_errors=true
    echo ""
    log_error "✗ ${CHANGELOG}: all URL definitions must be at the end of " \
              "the file."
  fi

  # Check that each release heading has a corresponding URL definition
  if ! check_release_url_definitions; then
    has_errors=true
    echo ""
    log_error "✗ ${CHANGELOG}: each release must have a URL definition."
  fi

  if [ ${#commits_invalid_format[@]} -gt 0 ]; then
    has_errors=true
    echo ""
    log_error "✗ ${CHANGELOG} entries have invalid format in:"
    echo ""
    local commit
    for commit in "${commits_invalid_format[@]}"; do
      echo "  - ${commit}"
    done
    echo ""
    echo "Each changelog entry starting with '- ' must end with a Jira"
    echo "issue reference in format (PROJ-NNNN) or (PROJ-NNNN, PROJ-MMMM),"
    echo "optionally followed by . or :. Issue references belong on the"
    echo "paragraph line or on a wrapped continuation line, not on indented"
    echo "sub-items."
    echo "The issue reference must be preceded by a space."
    echo ""
    echo "Example: - Add new feature for parallel query (PROJ-1234)"
  fi

  # If no user-visible changes detected, no changelog update needed
  if [ "${needs_changelog}" = "false" ]; then
    if [ "${has_errors}" = "true" ]; then
      echo ""
      echo "Please fix the changelog format in ${CHANGELOG}."
      exit 1
    fi
    log_success "✓ No user-visible changes detected. " \
                "Changelog update not required."
    exit 0
  fi

  # Check if all commits with user-visible changes have valid changelog updates
  if [ ${#commits_missing_changelog[@]} -gt 0 ]; then
    has_errors=true
    echo ""
    log_error "✗ ${CHANGELOG} must be updated for the following commits:"
    echo ""
    local commit
    for commit in "${commits_missing_changelog[@]}"; do
      echo "  - ${commit}"
    done
  fi

  if [ "${has_errors}" = "true" ]; then
    echo ""
    echo "Please update ${CHANGELOG} in the respective commits to " \
         "document these changes."
    echo "See https://keepachangelog.com/en/1.1.0/ for the changelog format."
    exit 1
  fi

  log_success "✓ All commits with user-visible changes have valid " \
              "${CHANGELOG} updates"
  exit 0
}

main "$@"
