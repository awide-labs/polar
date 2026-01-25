#!/bin/bash
# Bitbucket-specific PR comment integration
#
# This script provides Bitbucket-specific functionality to post CI warnings
# as comments on Pull Requests. It implements the on_warning_hook that
# is called by the CI-agnostic log_warning function in .ci/common.sh.
#
# Usage: Source this file in Bitbucket Pipelines jobs to enable PR comment posting
#   source .bitbucket/pr-comments.sh
#
# Requirements:
#   - BITBUCKET_PR_COMMENT_TOKEN: Bitbucket OAuth access token (repository variable)
#   - BITBUCKET_PR_ID: PR ID (automatically set by Bitbucket for PR pipelines)
#   - BITBUCKET_WORKSPACE: Workspace slug (automatically set by Bitbucket)
#   - BITBUCKET_REPO_SLUG: Repository slug (automatically set by Bitbucket)

# Implementation of the warning hook for Bitbucket
# This function is called by log_warning() in .ci/common.sh
on_warning_hook() {
  local warning_msg="${1:-}"

  # Only post to PR if all required Bitbucket variables are present
  if [ -n "${BITBUCKET_PR_COMMENT_TOKEN:-}" ] && \
     [ -n "${BITBUCKET_PR_ID:-}" ] && \
     [ -n "${BITBUCKET_WORKSPACE:-}" ] && \
     [ -n "${BITBUCKET_REPO_SLUG:-}" ]; then
    _bitbucket_post_warning_to_pr "${warning_msg}"
  fi
}

# Post a warning comment to the Bitbucket PR
# Usage: _bitbucket_post_warning_to_pr MESSAGE
_bitbucket_post_warning_to_pr() {
  local message="${1:-}"

  if [ -z "${message}" ]; then
    return 1
  fi

  # Get script name for context (look up the call stack to find the actual script)
  local script_name
  script_name=$(basename "${BASH_SOURCE[3]}" 2>/dev/null || echo "CI Script")

  # Construct the pipeline URL (Bitbucket doesn't provide this directly)
  local pipeline_url="https://bitbucket.org/${BITBUCKET_WORKSPACE}/${BITBUCKET_REPO_SLUG}/pipelines/results/${BITBUCKET_BUILD_NUMBER:-0}"

  # Format message: wrap single-line in blockquote, use as-is for multi-line
  local formatted_message
  if [[ "${message}" == *$'\n'* ]]; then
    # Multi-line message: use as-is (likely already formatted with Markdown)
    formatted_message="${message}"
  else
    # Single-line message: wrap in blockquote with bold
    formatted_message="> **${message}**"
  fi

  # Format the comment body with Markdown
  local comment_body
  comment_body=$(cat <<EOF
### ⚠️ Friendly warning from CI bot 🤖

Hi! I was checking the CI and noticed something worth your attention:

${formatted_message}

**Source:** \`${script_name}\`    
**Pipeline:** [#${BITBUCKET_BUILD_NUMBER:-unknown}](${pipeline_url})

*I'm just a helpful bot doing my job. This warning won't break your pipeline, but you might want to take a look!*
EOF
)

  # Encode the comment body for JSON (Bitbucket uses content.raw format)
  local json_body
  if command -v jq &> /dev/null; then
    json_body=$(jq -n --arg body "${comment_body}" '{content: {raw: $body}}')
  else
    # Fallback: use python for JSON encoding if jq is not available
    json_body=$(python3 -c "import json, sys; print(json.dumps({'content': {'raw': sys.argv[1]}}))" "${comment_body}" 2>/dev/null || \
                python -c "import json, sys; print(json.dumps({'content': {'raw': sys.argv[1]}}))" "${comment_body}" 2>/dev/null)

    # If both fail, skip posting
    if [ -z "${json_body}" ]; then
      echo -e "\033[2m(Note: Neither jq nor python available for JSON encoding, skipping PR comment)\033[0m" >&2
      return 1
    fi
  fi

  # Post comment to PR using Bitbucket API
  local api_url="https://api.bitbucket.org/2.0/repositories/${BITBUCKET_WORKSPACE}/${BITBUCKET_REPO_SLUG}/pullrequests/${BITBUCKET_PR_ID}/comments"

  local response curl_exit_code=0 curl_stderr
  curl_stderr=$(mktemp)
  response=$(curl -s -w "\n%{http_code}" \
    --request POST \
    --header "Authorization: Bearer ${BITBUCKET_PR_COMMENT_TOKEN}" \
    --header "Content-Type: application/json" \
    --data "${json_body}" \
    "${api_url}" 2>"${curl_stderr}") || curl_exit_code=$?

  local http_code
  if [ "${curl_exit_code}" -ne 0 ]; then
    http_code="000"
  else
    http_code=$(echo "${response}" | tail -1)
  fi

  # Silently continue on API failure - don't want to break CI over this
  if [ "${http_code}" != "201" ]; then
    echo -e "\033[2m(Note: Failed to post warning to PR, HTTP ${http_code})\033[0m" >&2
    if [ "${curl_exit_code}" -ne 0 ]; then
      echo -e "\033[2m(curl exit code: ${curl_exit_code})\033[0m" >&2
      if [ -s "${curl_stderr}" ]; then
        echo -e "\033[2m(curl error: $(cat "${curl_stderr}"))\033[0m" >&2
      fi
    fi
  fi
  rm -f "${curl_stderr}"
}
