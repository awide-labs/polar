#!/bin/bash
# GitHub Actions integration for CI warnings
#
# Implements the on_warning_hook called by log_warning() in .ci/common.sh,
# which sources this file when running under GitHub Actions. Every warning
# becomes a workflow annotation and an entry in the job summary. On pull
# requests it is also posted as a PR comment, once per distinct warning.
#
# Environment:
#   GH_TOKEN:  token allowed to comment on pull requests (optional; without
#              it, or with the read-only token of a fork PR, no comment is
#              posted)
#   PR_NUMBER: number of the pull request to comment on (optional)
#   GITHUB_*:  set by GitHub Actions

on_warning_hook() {
  local message="${1:-}"
  [ -n "${message}" ] || return 0

  local script_name
  script_name=$(basename "${BASH_SOURCE[-1]}" 2>/dev/null || echo "CI script")

  _github_annotate_warning "${script_name}" "${message}"

  if [ -n "${GITHUB_STEP_SUMMARY:-}" ]; then
    printf '### :warning: %s\n\n%s\n\n' "${script_name}" "${message}" \
      >> "${GITHUB_STEP_SUMMARY}"
  fi

  if [ -n "${GH_TOKEN:-}" ] && [ -n "${PR_NUMBER:-}" ] \
     && command -v gh &>/dev/null; then
    _github_post_warning_to_pr "${script_name}" "${message}"
  fi
}

# Usage: _github_annotate_warning TITLE MESSAGE
_github_annotate_warning() {
  local title="${1}" message="${2}"

  # Workflow command escaping: % first, then line breaks.
  message="${message//'%'/%25}"
  message="${message//$'\r'/%0D}"
  message="${message//$'\n'/%0A}"
  title="${title//'%'/%25}"
  title="${title//','/%2C}"
  title="${title//':'/%3A}"
  echo "::warning title=${title}::${message}"
}

# Usage: _github_post_warning_to_pr SCRIPT_NAME MESSAGE
_github_post_warning_to_pr() {
  local script_name="${1}" message="${2}"

  # Single-line messages read better as a bold quote.
  local formatted_message="${message}"
  if [[ "${message}" != *$'\n'* ]]; then
    formatted_message="> **${message}**"
  fi

  # The marker identifies the warning across runs, so a re-run or a
  # force-push does not post the same warning again.
  local marker
  marker="<!-- ci-warning:$(printf '%s\n%s' "${script_name}" "${message}" \
    | _github_warning_normalize | sha256sum | cut -c1-16) -->"

  local repo="${GITHUB_REPOSITORY:?}"
  if gh api --paginate "repos/${repo}/issues/${PR_NUMBER}/comments" \
       --jq '.[].body' 2>/dev/null | grep -qF "${marker}"; then
    echo -e "\033[2m(Note: duplicate warning, skipping PR comment)\033[0m" >&2
    return 0
  fi

  local run_url="${GITHUB_SERVER_URL:-https://github.com}/${repo}/actions/runs/${GITHUB_RUN_ID:-}"
  local body
  body=$(cat <<EOF
${marker}
### :warning: CI warning

${formatted_message}

**Source:** \`${script_name}\`
**Job:** [\`${GITHUB_JOB:-unknown}\`](${run_url})

*This warning does not fail the check, but please take a look.*
EOF
)

  if ! gh api "repos/${repo}/issues/${PR_NUMBER}/comments" \
       -f body="${body}" >/dev/null 2>&1; then
    echo -e "\033[2m(Note: failed to post the warning as a PR comment)\033[0m" >&2
  fi
}

# Collapse the volatile bits so the same logical warning dedupes across runs:
# commit SHAs change on every force-push.
_github_warning_normalize() {
  sed -E 's/[0-9a-f]{7,40}/HASH/g'
}
