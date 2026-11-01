#!/bin/bash
# Ensure POLAR_VERSION (VERSION_* vars) matches configure.ac / configure PACKAGE_VERSION.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck disable=SC1091
source "${SCRIPT_DIR}/common.sh"

REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_ROOT"

# shellcheck source=/dev/null
. ./POLAR_VERSION
EXPECTED_PG="${VERSION_MAJOR}.${VERSION_MINOR}"

# POLAR_VERSION MAJOR.MINOR must match upstream PostgreSQL AC_INIT / PACKAGE_VERSION (maintain POLAR_VERSION when merging upstream).
AC_INIT_LINE=$(grep '^AC_INIT(\[PostgreSQL\]' configure.ac | head -1)
if [[ ! "${AC_INIT_LINE}" =~ \[PostgreSQL\],\ \[([0-9]+\.[0-9]+)\] ]]; then
	log_error "check_polar_version: could not parse AC_INIT version from configure.ac"
	exit 1
fi
UPSTREAM_PG="${BASH_REMATCH[1]}"
if [[ "${UPSTREAM_PG}" != "${EXPECTED_PG}" ]]; then
	log_error "check_polar_version: POLAR_VERSION MAJOR.MINOR (${EXPECTED_PG}) must match configure.ac AC_INIT (${UPSTREAM_PG})."
	exit 1
fi

if [[ "$(grep -E '^PACKAGE_VERSION=' configure | head -1)" != "PACKAGE_VERSION='${EXPECTED_PG}'" ]]; then
	log_error "check_polar_version: configure PACKAGE_VERSION must be '${EXPECTED_PG}' (same as POLAR_VERSION MAJOR.MINOR; run autoconf if stale)."
	exit 1
fi

log_success "check_polar_version: OK (upstream PG ${UPSTREAM_PG})"
