#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# scripts/ci/test-docker.sh -- run the host unit-test suite in a Linux container.
#
# Why this exists:
#   On Apple Silicon macOS the fake backing in tests/mocks/ra8_fake_mmap.c
#   uses `mmap(MAP_FIXED, 0x40000000, ...)` to install RAM at the same
#   virtual addresses the RA8D2 chip exposes its peripherals at. macOS
#   arm64 refuses MAP_FIXED below 4 GiB regardless of -pagezero_size, so
#   the constructor aborts and every test exits with SIGKILL before
#   `main()` runs. This wrapper sidesteps the issue by running the tests
#   inside the project's existing Ubuntu 24.04 devcontainer, where the
#   fake's MAP_FIXED works as designed.
#
#   On Linux this wrapper is unnecessary -- just run `make test`.
#
# Container engine: colima on macOS (no Docker Desktop license). The
# script auto-starts a colima VM if one isn't running. On Linux the
# colima check is skipped and we use whatever docker daemon is up.
#
# Usage:
#   ./scripts/ci/test-docker.sh           # build + run all tests
#   ./scripts/ci/test-docker.sh --rebuild # force-rebuild the image
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
IMAGE_TAG="ra8-firmware-test:latest"

# ra8_max_jobs -- the ONE canonical bounded-parallelism width (#328). Forwarded
# into the container as CMAKE_BUILD_PARALLEL_LEVEL so the in-container
# `cmake --build --parallel` honours the same bound as every other gate.
# shellcheck source=scripts/ci/lib/parallelism.sh
. "$SCRIPT_DIR/lib/parallelism.sh"

REBUILD=false
for arg in "$@"; do
  case "$arg" in
    --rebuild) REBUILD=true ;;
    *)
      echo "Unknown flag: $arg" >&2
      exit 1
      ;;
  esac
done

if ! command -v docker >/dev/null 2>&1; then
  echo "error: docker not on PATH." >&2
  echo "  install: brew install colima docker" >&2
  exit 1
fi

# On macOS, prefer colima (no Docker Desktop license required).
if [[ "$(uname -s)" == "Darwin" ]]; then
  if ! command -v colima >/dev/null 2>&1; then
    echo "error: colima not on PATH. Install with: brew install colima" >&2
    exit 1
  fi
  if ! colima status >/dev/null 2>&1; then
    echo "==> Starting colima VM (4 CPU, 6 GiB)"
    colima start --cpu 4 --memory 6
  fi
fi

if ! docker info >/dev/null 2>&1; then
  echo "error: docker daemon not reachable." >&2
  if [[ "$(uname -s)" == "Darwin" ]]; then
    echo "  try: colima start" >&2
  fi
  exit 1
fi

if [[ "$REBUILD" == "true" ]] || ! docker image inspect "$IMAGE_TAG" >/dev/null 2>&1; then
  echo "==> Building $IMAGE_TAG (one-time, ~2 GB image)"
  docker build -t "$IMAGE_TAG" -f "$REPO_ROOT/.devcontainer/Dockerfile" "$REPO_ROOT/.devcontainer"
fi

# Run host tests inside the container. Mount the repo read-write so
# the build artefacts go under /work/build/tidy-docker (kept separate
# from the macOS host build dir so neither overwrites the other).
echo "==> Running host tests inside container"
docker run --rm \
  -v "$REPO_ROOT":/work \
  -w /work \
  -e CMAKE_BUILD_PARALLEL_LEVEL="$(ra8_max_jobs)" \
  "$IMAGE_TAG" \
  bash -lc "
        set -e
        cmake -B build/host-docker -S tests \
            -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
            -DRA8_COVERAGE=OFF
        cmake --build build/host-docker --parallel
        cd build/host-docker
        ctest --output-on-failure
    "
