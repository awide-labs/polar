#!/usr/bin/env bash
set -xeuo pipefail

# Usage: ci-test.sh <container_image> <debug_mode> <rules> [cleanup]
# Example: ci-test.sh rocky9 on ci-quick
# Example: ci-test.sh rocky9 off ci-full true
#
# debug_mode=on builds with assertions, injection points and fault injection,
# but optimized like a release build (-O2 instead of the -O0 of build.sh).
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

IMAGE=$("$(dirname "$0")/docker/image.sh" ensure "$CONTAINER_IMAGE")

# Create and start the container
docker create                                                  \
  -t                                                           \
  --shm-size=1g                                                \
  --name polardb_${CONTAINER_IMAGE}                            \
  -v `pwd`:/home/postgres/polar                                \
  "$IMAGE"                                                     \
  bash
docker start polardb_${CONTAINER_IMAGE}

# Make files writable for postgres user
docker exec polardb_${CONTAINER_IMAGE} bash -c \
  "cd /home/postgres/polar && \
   sudo chown -R postgres:postgres . 2>/dev/null || true"

# Build and run defined checks in sequence. The build uses all CPU cores. The
# ci-* targets run as many test suites concurrently as there are CPU cores,
# each with prove jobs for three quarters of them: most TAP tests spend their
# time waiting for servers rather than computing.
docker exec polardb_${CONTAINER_IMAGE} bash -c \
  "set -x && \
   cd /home/postgres/polar && \
   if [ -f /etc/bashrc ]; then source /etc/bashrc; fi && \
   set -eu && \
   CPUS=\$(nproc) && \
   MAKE_JOBS=\$CPUS && \
   PROVE_JOBS=\$(( CPUS * 3 / 4 > 0 ? CPUS * 3 / 4 : 1 )) && \
   export CFLAGS=-O2 CXXFLAGS=-O2 && \
   eatmydata ./build.sh --noinstall --debug=${DEBUG_MODE} --ec='--enable-tap-tests' && \
   export PG_TEST_INITDB_EXTRA_OPTS=--wal-segsize=16 && \
   export PGCTLTIMEOUT=300 && \
   for rule in ${RULES}; do \
     echo \"=== Running \$rule ===\" && \
     eatmydata make \$rule -j\$MAKE_JOBS -Otarget \
       PROVE_FLAGS=\"--timer --jobs=\$PROVE_JOBS\" \
       PG_TEST_EXTRA='kerberos ssl ldap wal_consistency_checking xid_wraparound' || \
       (echo \"=== \$rule FAILED ===\" && exit 1); \
   done"

# Clean up container if requested (useful when multiple containers run in same VM)
if [ "$CLEANUP" = "true" ] || [ "$CLEANUP" = "1" ]; then
  docker stop polardb_${CONTAINER_IMAGE} 2>/dev/null || true
  docker rm polardb_${CONTAINER_IMAGE} 2>/dev/null || true
fi

