#!/usr/bin/env bash
#
# image.sh - resolve or build the CI image for a distribution
#
# Usage: .ci/docker/image.sh ref <os>
#        .ci/docker/image.sh ensure <os>
#        .ci/docker/image.sh build <os> [tag]
#
# <os> is a distribution name followed by its version, e.g. rocky9 or
# ubuntu22.04. Its image is <repo>/<name>:<version>-<hash>, e.g.
# ghcr.io/awide-labs/polar/dev/rocky:9-<hash>, where <hash> is derived from
# the contents of .ci/docker/Dockerfile.<os>, of the pg_bsd_indent sources and
# of the PolarFS commit recorded for the .ci/docker/polarfs submodule, which
# are built into the image, so every revision of them has its own tag and a
# tag is never overwritten.
#
# "ref" prints the image reference. "ensure" also makes the image available
# locally: it pulls the published image and, if that tag has not been
# published yet (e.g. a pull request changes the Dockerfile), builds it.
# "build" builds the image and tags it as the image reference or <tag>. It
# initializes the polarfs submodule if needed, and fails if the submodule is
# checked out at a different commit than the one recorded.
#
# POLAR_CI_IMAGE_REPO overrides the repository, ghcr.io/awide-labs/polar/dev
# by default.
#

set -euo pipefail

usage() {
  echo "Usage: $0 ref|ensure <os>" >&2
  echo "       $0 build <os> [tag]" >&2
  exit 1
}

[ $# -ge 2 ] || usage
ACTION="$1"
OS="$2"
case "${ACTION}" in
  ref|ensure) [ $# -eq 2 ] || usage ;;
  build) [ $# -le 3 ] || usage ;;
  *) usage ;;
esac

DOCKER_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${DOCKER_DIR}/../.." && pwd)"
INDENT_DIR="${ROOT_DIR}/src/tools/pg_bsd_indent"
POLARFS_DIR="${DOCKER_DIR}/polarfs"
DOCKERFILE="${DOCKER_DIR}/Dockerfile.${OS}"
REPO="${POLAR_CI_IMAGE_REPO:-ghcr.io/awide-labs/polar/dev}"

if [ ! -f "${DOCKERFILE}" ]; then
  echo "No Dockerfile for '${OS}': ${DOCKERFILE}" >&2
  exit 1
fi

if [[ ! "${OS}" =~ ^([a-z]+)([0-9][0-9.]*)$ ]]; then
  echo "'${OS}' is not a distribution name followed by a version" >&2
  exit 1
fi
NAME="${BASH_REMATCH[1]}"
VERSION="${BASH_REMATCH[2]}"

POLARFS_COMMIT="$(git -C "${ROOT_DIR}" ls-files -s .ci/docker/polarfs | cut -d' ' -f2)"
if [ -z "${POLARFS_COMMIT}" ]; then
  echo "No polarfs submodule recorded at ${POLARFS_DIR}" >&2
  exit 1
fi

hash_inputs() {
  local LC_ALL=C f
  echo "Dockerfile $(git hash-object "${DOCKERFILE}")"
  for f in "${INDENT_DIR}"/*.[ch] "${INDENT_DIR}"/Makefile; do
    echo "pg_bsd_indent/$(basename "$f") $(git hash-object "$f")"
  done
  echo "polarfs ${POLARFS_COMMIT}"
}

build() {
  local checked_out
  if [ ! -e "${POLARFS_DIR}/.git" ]; then
    git -C "${ROOT_DIR}" submodule update --init --recursive .ci/docker/polarfs >&2
  fi
  checked_out="$(git -C "${POLARFS_DIR}" rev-parse HEAD)"
  if [ "${checked_out}" != "${POLARFS_COMMIT}" ]; then
    echo "${POLARFS_DIR} is checked out at ${checked_out}, but" \
         "${POLARFS_COMMIT} is recorded; run" \
         "'git submodule update --init --recursive .ci/docker/polarfs'" >&2
    exit 1
  fi
  git -C "${POLARFS_DIR}" submodule update --init --recursive >&2
  docker build \
    --build-context pg_bsd_indent="${INDENT_DIR}" \
    --build-context polarfs="${POLARFS_DIR}" \
    -f "${DOCKERFILE}" -t "$1" "${DOCKER_DIR}" >&2
}

HASH="$(hash_inputs | git hash-object --stdin | cut -c1-12)"
IMAGE="${REPO}/${NAME}:${VERSION}-${HASH}"

case "${ACTION}" in
  ensure)
    if docker image inspect "${IMAGE}" >/dev/null 2>&1; then
      :
    elif docker pull "${IMAGE}" >&2; then
      :
    else
      echo "${IMAGE} is not published, building it locally" >&2
      if [ -n "${GITHUB_ACTIONS:-}" ]; then
        echo "::warning::${IMAGE} is not published, building it locally" >&2
      fi
      build "${IMAGE}"
    fi
    ;;
  build)
    IMAGE="${3:-${IMAGE}}"
    build "${IMAGE}"
    ;;
esac

echo "${IMAGE}"
