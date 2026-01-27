#!/usr/bin/env bash
#
# check_extensions_order.sh - Check extensions order in external/Makefile
#
# This script runs polar_sort_subdir.pl on external/Makefile to verify that
# SUBDIRS entries are sorted in ASCII order. The script fails if the tool
# finds any ordering issues or returns a non-zero exit code.
#
# Usage: .ci/check_extensions_order.sh
#

set -euo pipefail

# Source common functions
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=.ci/common.sh
source "${SCRIPT_DIR}/common.sh"

# Script directory and repo root
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

cd "$REPO_ROOT"

# Fix git directory path when running in a container
# In Docker containers, git might not recognize .git, so we set GIT_DIR
if [ -d ".git" ] && ! git rev-parse --git-dir > /dev/null 2>&1; then
    export GIT_DIR="$REPO_ROOT/.git"
    export GIT_WORK_TREE="$REPO_ROOT"
fi

echo "=== Checking extensions order in Makefile(s) ==="

TOOL_SCRIPT="$REPO_ROOT/src/tools/polar_sort_subdir.pl"
MAKEFILE="$REPO_ROOT/external/Makefile"

# Run the tool to sort SUBDIRS
echo "Running polar_sort_subdir.pl on external/Makefile..."
if ! perl "$TOOL_SCRIPT" -f "$MAKEFILE"; then
    log_error "polar_sort_subdir.pl exited with non-zero status"
    exit 1
fi

# Check if any files were modified
CHANGED_FILES=$(git diff --name-only)
if [ -n "$CHANGED_FILES" ]; then
    log_error "=== polar_sort_subdir.pl found ordering issues ==="
    echo
    echo "The following files need reordering:"
    echo "$CHANGED_FILES"
    echo
    echo "=== Diff of required changes ==="
    echo
    git diff --color=always
    echo
    log_error "Please fix the ordering by running locally:"
    log_error "  src/tools/polar_sort_subdir.pl -f external/Makefile"
    exit 1
fi

log_success "=== Extensions order check passed ==="
exit 0
