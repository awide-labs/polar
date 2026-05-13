#!/usr/bin/env bash
set -xeuo pipefail

# Usage: ci-test.sh <container_image> <debug_mode> <rules> [cleanup]
# Example: ci-test.sh rocky9 on precheck-ci
# Example: ci-test.sh rocky9 on precheck-ci true
#
# cleanup: optional flag (true/1 to enable cleanup, false/0 or omitted to skip)
#          Useful when multiple containers run in the same VM/step

if [ $# -lt 3 ] || [ $# -gt 4 ]; then
  echo "Usage: $0 <container_image> <debug_mode> <rules> [cleanup]" >&2
  exit 1
fi

CONTAINER_IMAGE="$1"
DEBUG_MODE="$2"
RULES="$3"
CLEANUP="${4:-false}"

echo "Running with container_image=$CONTAINER_IMAGE, debug_mode=$DEBUG_MODE"
echo "Executing rules: $RULES"

# Create and start the container
docker create                                                  \
  -t                                                           \
  --name polardb_${CONTAINER_IMAGE}                 \
  -v `pwd`:/home/postgres/PolarDB-for-PostgreSQL               \
  polardb/polardb_pg_devel:${CONTAINER_IMAGE} \
  bash
docker start polardb_${CONTAINER_IMAGE}

# Make files writable for postgres user
docker exec polardb_${CONTAINER_IMAGE} bash -c \
  "cd /home/postgres/PolarDB-for-PostgreSQL && \
   sudo chown -R postgres:postgres . 2>/dev/null || true"

# Build and run defined checks in sequence
# Use all available CPU cores for faster builds and tests
docker exec polardb_${CONTAINER_IMAGE} bash -c \
  "set -x && \
   if ! command -v eatmydata >/dev/null 2>&1; then \
     if command -v yum >/dev/null 2>&1; then \
       sudo yum install -y eatmydata; \
     elif command -v apt-get >/dev/null 2>&1; then \
       sudo apt-get update -y && \
       sudo apt-get install -y eatmydata; \
     fi; \
   fi && \
   cd /home/postgres/PolarDB-for-PostgreSQL && \
   if [ -f /etc/bashrc ]; then source /etc/bashrc; fi && \
   set -eu && \
   JOBS=\$(nproc) && \
   eatmydata ./build.sh --noinstall --debug=${DEBUG_MODE} --ec='--enable-tap-tests' && \
   for rule in ${RULES}; do \
     echo \"=== Running \$rule ===\" && \
     export PG_TEST_INITDB_EXTRA_OPTS=--wal-segsize=16 &&
     eatmydata make \$rule -j\$JOBS -Otarget || (echo \"=== \$rule FAILED ===\" && exit 1); \
   done"

# Clean up container if requested (useful when multiple containers run in same VM)
if [ "$CLEANUP" = "true" ] || [ "$CLEANUP" = "1" ]; then
  docker stop polardb_${CONTAINER_IMAGE} 2>/dev/null || true
  docker rm polardb_${CONTAINER_IMAGE} 2>/dev/null || true
fi

