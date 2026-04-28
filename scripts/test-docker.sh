#!/usr/bin/env bash
#
# scripts/test-docker.sh -- run the host unit-test suite in a Linux container.
#
# Why this exists:
#   On Apple Silicon macOS the simulator backing in tests/mocks/ra_sim_mmap.c
#   uses `mmap(MAP_FIXED, 0x40000000, ...)` to install RAM at the same
#   virtual addresses the RA8D2 chip exposes its peripherals at. macOS
#   arm64 refuses MAP_FIXED below 4 GiB regardless of -pagezero_size, so
#   the constructor aborts and every test exits with SIGKILL before
#   `main()` runs. This wrapper sidesteps the issue by running the tests
#   inside the project's existing Ubuntu 24.04 devcontainer, where the
#   simulator's MAP_FIXED works as designed.
#
#   On Linux this wrapper is unnecessary -- just run `make test`.
#
# Usage:
#   ./scripts/test-docker.sh           # build + run all tests
#   ./scripts/test-docker.sh --rebuild # force-rebuild the image
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
IMAGE_TAG="ra8d2-firmware-test:latest"

REBUILD=false
for arg in "$@"; do
    case "$arg" in
        --rebuild) REBUILD=true ;;
        *) echo "Unknown flag: $arg" >&2; exit 1 ;;
    esac
done

if ! command -v docker >/dev/null 2>&1; then
    echo "error: docker not on PATH. Install Docker Desktop and try again." >&2
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
    "$IMAGE_TAG" \
    bash -lc "
        set -e
        cmake -B build/host-docker -S tests \
            -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
            -DRA_COVERAGE=OFF
        cmake --build build/host-docker --parallel
        cd build/host-docker
        ctest --output-on-failure
    "
