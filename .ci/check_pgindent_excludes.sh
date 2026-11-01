#!/usr/bin/env bash
#
# check_pgindent_excludes.sh - Warn if exclude_file_patterns is modified
#
# This script checks if the PR modifies exclude_file_patterns and posts a
# warning if so. This helps reviewers pay attention to changes that exclude
# files from pgindent checks.
#
# Usage: .ci/check_pgindent_excludes.sh BASE_REF CURRENT_REF
#
# Arguments:
#   BASE_REF: Base commit/ref to compare against (required)
#   CURRENT_REF: Current commit/ref (optional, defaults to HEAD)
#

set -euo pipefail

# Source common functions
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=.ci/common.sh
source "${SCRIPT_DIR}/common.sh"

EXCLUDES_FILE="src/tools/pgindent/exclude_file_patterns"

main() {
    local base_ref="${1:-}"
    local current_ref="${2:-HEAD}"

    if [ -z "${base_ref}" ]; then
        log_error "Error: BASE_REF is required"
        echo "Usage: $0 BASE_REF [CURRENT_REF]"
        exit 1
    fi

    echo "Checking for changes to ${EXCLUDES_FILE}..."
    echo "Base ref: ${base_ref}"
    echo "Current ref: ${current_ref}"

    # Check if the file was modified in this PR
    if git diff --name-only "${base_ref}..${current_ref}" | grep -q "^${EXCLUDES_FILE}$"; then
        # Build warning message with details
        local warning_msg="**exclude_file_patterns was modified**"
        warning_msg+=$'\n\n'
        warning_msg+="This PR modifies \`${EXCLUDES_FILE}\`, which excludes "
        warning_msg+="files from pgindent checks."
        warning_msg+=$'\n\n'
        warning_msg+="**Please verify that:**"$'\n'
        warning_msg+="- The excluded files are appropriate (e.g. imported code, generated files)"$'\n'
        warning_msg+="- The exclusions are necessary to avoid merge conflicts or false failures"$'\n'
        warning_msg+=$'\n'
        warning_msg+="**Changes:**"$'\n'
        warning_msg+="\`\`\`diff"$'\n'
        warning_msg+="$(git diff "${base_ref}..${current_ref}" -- "${EXCLUDES_FILE}")"
        warning_msg+=$'\n'"\`\`\`"

        log_warning "${warning_msg}"
    fi

    log_success "=== pgindent excludes check passed ==="
    exit 0
}

main "$@"
