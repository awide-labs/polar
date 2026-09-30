#!/usr/bin/env bash
#
# check_pgindent.sh - Check C code style compliance using pgindent
#
# This script runs pgindent on the codebase and fails if:
# - pgindent reports any errors (e.g., "Failure in ./file.c: Error@...")
# - pgindent makes any changes to the code (detected via git diff)
#
# Usage: .ci/check_pgindent.sh
#
# Requirements:
# - pg_bsd_indent must be installed and available in PATH
# - Must be run from the root of the repository
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

echo "=== Checking C code style with pgindent ==="

# Verify pg_bsd_indent is available
if ! command -v pg_bsd_indent &> /dev/null; then
    log_error "Error: pg_bsd_indent is not installed or not in PATH"
    exit 1
fi

echo "Using pg_bsd_indent version: $(pg_bsd_indent --version 2>&1 | head -1)"

# Create a temporary file for capturing stderr
STDERR_FILE=$(mktemp)
trap "rm -f '$STDERR_FILE'" EXIT

# Run pgindent, capturing stderr to check for errors
# pgindent returns 0 even when it makes changes or reports errors
echo "Running pgindent..."
if ! "$REPO_ROOT/src/tools/pgindent/pgindent" 2> "$STDERR_FILE"; then
    log_error "pgindent exited with non-zero status"
    cat "$STDERR_FILE"
    exit 1
fi

# Check if pgindent reported any errors
PGINDENT_ERRORS=0
if [ -s "$STDERR_FILE" ]; then
    # Check for "Failure" messages which indicate errors
    if grep -q "Failure" "$STDERR_FILE"; then
        PGINDENT_ERRORS=1
        log_error "=== pgindent reported errors ==="
        cat "$STDERR_FILE"
    else
        # Other stderr output (warnings, info) - just display it
        echo "pgindent output:"
        cat "$STDERR_FILE"
    fi
fi

# Check if any files were modified
CHANGED_FILES=$(git diff --name-only)
if [ -n "$CHANGED_FILES" ]; then
    log_error "=== pgindent found code style issues ==="
    echo
    echo "The following files need formatting:"
    echo "$CHANGED_FILES"
    echo
    echo "=== Diff of required changes ==="
    echo
    # Show colorized diff
    # Use --color=always to force color output even when not in a terminal
    git diff --color=always
    echo
    log_error "Please run 'src/tools/pgindent/pgindent' locally and commit."
    exit 1
fi

# Final status check
if [ "$PGINDENT_ERRORS" -eq 1 ]; then
    log_error "pgindent reported errors (see above)"
    exit 1
fi

log_success "=== C code style check passed ==="
exit 0
