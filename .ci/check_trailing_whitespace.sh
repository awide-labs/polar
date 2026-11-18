#!/bin/bash
# Check that PR commits do not add lines ending in spaces or tabs, in code,
# documentation and tests alike. Trailing whitespace is invisible in the
# pull request diff view, so review does not catch it.
#
# Files whose whitespace attribute in .gitattributes is unset (-whitespace)
# or excludes blank-at-eol / trailing-space (e.g. expected test outputs
# *.out, vendored sources) are exempt, the same files `git diff --check`
# leaves alone. In *.md files, exactly two trailing spaces after text are
# allowed: that is a Markdown hard line break.
#
# Pre-existing violations are grandfathered: only lines added by direct
# PR commits are checked. Merge commits are checked for the lines they add
# themselves (conflict resolutions and the like, as shown by
# `git diff-tree --cc`); commits brought by merges are skipped. A commit
# that has to add trailing whitespace verbatim (e.g. importing third-party
# code) can opt out with a "Skip-trailing-whitespace: true" footer.
#
# Usage: check_trailing_whitespace.sh BASE_REF [CURRENT_REF]
#   BASE_REF: Base commit/ref to compare against (required)
#   CURRENT_REF: Current commit/ref (optional, defaults to HEAD)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=.ci/common.sh
source "${SCRIPT_DIR}/common.sh"

readonly SKIP_FOOTER='Skip-trailing-whitespace'

usage() {
  echo "Usage: $0 BASE_REF [CURRENT_REF]"
  echo ""
  echo "  BASE_REF: Base commit/ref to compare against (required)"
  echo "  CURRENT_REF: Current commit/ref (optional, defaults to HEAD)"
  exit 1
}

# Emit one "path<TAB>lineno<TAB>text" line per line added by COMMIT_SHA
# that ends in a space or a tab (optionally followed by CR), empty output
# if none. Regular commits are diffed against their parent, merge commits
# with --cc, so only lines differing from every parent count as added.
# Usage: scan_commit_lines COMMIT_SHA
scan_commit_lines() {
  local commit_sha="${1:-}"
  local -a diff_opts

  if is_merge_commit "${commit_sha}"; then
    diff_opts=(--cc)
  else
    # -M: rename detection, so a renamed file's pre-existing (grandfathered)
    # content is not re-reported as additions.
    diff_opts=(-M -r)
  fi

  # core.quotePath=false: keep non-ASCII paths unquoted, as check-attr
  # expects them.
  git -c core.quotePath=false diff-tree --no-color --no-ext-diff -p --unified=0 \
    "${diff_opts[@]}" "${commit_sha}" |
    awk '
      # File header: track the current path; /dev/null (deletion) resets it.
      # Header lines only appear between "diff " and the first hunk, so an
      # added line that happens to start with "++ b/" is not mistaken for one.
      /^diff / { in_header = 1; path = ""; next }
      # Git appends a tab to the header when the path contains a space.
      in_header && /^\+\+\+ b\// {
        path = substr($0, 7)
        sub(/\t$/, "", path)
        next
      }
      in_header && !/^@@/ { next }

      # Hunk header: "@@ -a +c @@" for one parent, "@@@ -a -b +c @@@" for
      # two, and so on. The new-file range is field nparents + 2.
      /^@@/ {
        in_header = 0
        match($0, /^@+/)
        nparents = RLENGTH - 1
        added = ""
        for (i = 0; i < nparents; i++)
          added = added "+"
        lineno = substr($(nparents + 2), 2) + 0
        next
      }

      # "\ No newline at end of file"
      /^\\/ { next }

      {
        prefix = substr($0, 1, nparents)
        # Removed lines do not exist in the new file.
        if (index(prefix, "-"))
          next
        line = substr($0, nparents + 1)
        # Exactly two spaces after text are a Markdown hard line break.
        if (path != "" && prefix == added && line ~ /[ \t]\r?$/ &&
            !(path ~ /\.md$/ && line ~ /[^ \t]  \r?$/))
          print path "\t" lineno "\t" line
        lineno++
      }
    '
}

# Read scan_commit_lines output on stdin and print the lines of files whose
# whitespace attribute at COMMIT_SHA enables the trailing-space check.
# Usage: filter_by_attributes COMMIT_SHA
filter_by_attributes() {
  local commit_sha="${1:-}"
  local -a lines=()
  local line

  while IFS= read -r line || [ -n "${line}" ]; do
    if [ -n "${line}" ]; then
      lines+=("${line}")
    fi
  done

  if [ "${#lines[@]}" -eq 0 ]; then
    return 0
  fi

  # Newline-separated paths whose whitespace attribute disables the check.
  local exempt=""
  local path value
  while IFS= read -r -d '' path && IFS= read -r -d '' _ &&
        IFS= read -r -d '' value; do
    case ",${value}," in
      ,unset,|*,-blank-at-eol,*|*,-trailing-space,*)
        exempt+="${path}"$'\n'
        ;;
    esac
  done < <(printf '%s\n' "${lines[@]}" | cut -f1 | sort -u | tr '\n' '\0' |
           git check-attr -z --stdin --source="${commit_sha}" whitespace)

  local lineno text
  for line in "${lines[@]}"; do
    # Not "IFS=$'\t' read": it would strip the trailing tabs off the text.
    path="${line%%$'\t'*}"
    text="${line#*$'\t'}"
    lineno="${text%%$'\t'*}"
    text="${text#*$'\t'}"
    if ! printf '%s' "${exempt}" | grep -qxF -- "${path}"; then
      echo "${path}:${lineno}: ${text}"
    fi
  done
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
  commits=$(get_commits_in_range "${base_ref}" "${current_ref}")

  if [ -z "${commits}" ]; then
    log_warning "No commits found in range, skipping trailing whitespace check"
    exit 0
  fi

  local excluded_commits=()
  local excluded_sha
  while IFS= read -r excluded_sha; do
    if [ -n "${excluded_sha}" ]; then
      excluded_commits+=("${excluded_sha}")
    fi
  done <<< "$(get_excluded_commits "${base_ref}" "${current_ref}")"

  echo "Analyzing commits for added trailing whitespace..."
  echo "---"

  local failed_commits=0
  local commits_skipped=()
  local line violation

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

    if has_true_footer "${commit_sha}" "${SKIP_FOOTER}"; then
      commits_skipped+=("${commit_sha:0:8} - ${commit_msg}")
      log_success "  ✓ ${commit_sha:0:8} - ${commit_msg} " \
                  "(skip-trailing-whitespace)"
      continue
    fi

    local violations=()
    while IFS= read -r violation || [ -n "${violation}" ]; do
      if [ -n "${violation}" ]; then
        violations+=("${violation}")
      fi
    done < <(scan_commit_lines "${commit_sha}" |
             filter_by_attributes "${commit_sha}")

    if [ "${#violations[@]}" -eq 0 ]; then
      log_success "  ✓ ${commit_sha:0:8} - ${commit_msg}"
      continue
    fi

    failed_commits=$((failed_commits + 1))
    log_error "  ✗ ${commit_sha:0:8} - ${commit_msg}" \
              "(${#violations[@]} lines with trailing whitespace)"
    for violation in "${violations[@]}"; do
      # Make the invisible whitespace visible: tab as "→", space as "·".
      local text="${violation#*: }"
      local stripped="${text%"${text##*[! $'\t\r']}"}"
      local tail="${text:${#stripped}}"
      tail="${tail//$'\t'/→}"
      tail="${tail// /·}"
      tail="${tail//$'\r'/}"
      echo "      ${violation%%: *}: ${stripped}${LOG_ERROR_COLOR}${tail}${LOG_RESET_COLOR}"
    done
  done <<< "${commits}"

  if [ "${#commits_skipped[@]}" -gt 0 ]; then
    local combined_msg
    combined_msg="The following commits skip the trailing whitespace check (Skip-trailing-whitespace: true):"
    local entry
    for entry in "${commits_skipped[@]}"; do
      combined_msg+=$'\n'"  - ${entry}"
    done
    log_warning "${combined_msg}"
  fi

  echo "---"

  if [ "${failed_commits}" -gt 0 ]; then
    log_error "✗ ${failed_commits} commit(s) add lines with trailing whitespace."
    echo ""
    echo "Remove the trailing spaces and tabs in the respective commits, e.g."
    echo "with 'git rebase --whitespace=fix ${base_ref}' (it does not fix merge"
    echo "commits). To see them locally: git diff --check ${base_ref}"
    echo "Commits importing third-party code verbatim may add a"
    echo "'Skip-trailing-whitespace: true' footer instead."
    exit 1
  fi

  log_success "✓ No trailing whitespace added"
  exit 0
}

main "$@"
