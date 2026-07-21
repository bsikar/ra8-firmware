#!/usr/bin/env bash
#
# scripts/report/coverage_report_host.sh -- generate an HTML coverage report for the
# host unit-test suite via lcov + genhtml.
#
# This is purely an additional reporting tool layered alongside the
# existing coverage gate (scripts/checks/coverage.sh, which uses gcovr). It
# does NOT replace the gate and does NOT change gate behaviour.
#
# Pipeline:
#   1. Configure tests/ with RA8_COVERAGE=ON (wires --coverage,
#      -fprofile-arcs, -ftest-coverage in tests/CMakeLists.txt).
#   2. Build all host tests.
#   3. Run them via ctest.
#   4. lcov --capture into build/host-cov/coverage.info.
#   5. lcov --remove third_party + tests + system headers.
#   6. genhtml into build/coverage-html/.
#   7. Print overall line/function coverage % to stdout.
#
# On macOS the suite must run inside the project's Linux devcontainer
# (the simulator's MAP_FIXED below 4 GiB is rejected by macOS arm64),
# so this script transparently shells out to docker just like
# scripts/ci/test-docker.sh. On Linux it runs natively.
#
# Usage:
#   ./scripts/report/coverage_report_host.sh
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

BUILD_DIR_REL="build/host-cov"
HTML_DIR_REL="build/coverage-html"

run_native() {
  local repo="$1"
  local build="$repo/$BUILD_DIR_REL"
  local html="$repo/$HTML_DIR_REL"
  local info="$build/coverage.info"
  local info_filtered="$build/coverage.filtered.info"

  echo "==> [1/6] Configuring host tests with RA8_COVERAGE=ON"
  cmake -B "$build" -S "$repo/tests" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DRA8_COVERAGE=ON \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -Wno-dev >/dev/null

  echo "==> [2/6] Building"
  cmake --build "$build" --parallel >/dev/null

  echo "==> [3/6] Running ctest"
  (cd "$build" && ctest --output-on-failure) | tail -20 || true

  echo "==> [4/6] Capturing coverage with lcov"
  # --ignore-errors keeps lcov tolerant across gcc/lcov version skew.
  lcov --capture \
    --directory "$build" \
    --output-file "$info" \
    --rc lcov_branch_coverage=1 \
    --ignore-errors mismatch,negative,source,gcov,unused,inconsistent,empty \
    --quiet 2>&1 | tail -10 || true

  echo "==> [5/6] Filtering third_party + tests + system headers"
  lcov --remove "$info" \
    '*/libs/third_party/*' \
    '*/tests/*' \
    '/usr/*' \
    '*/c++/*' \
    --output-file "$info_filtered" \
    --rc lcov_branch_coverage=1 \
    --ignore-errors unused,empty,inconsistent \
    --quiet 2>&1 | tail -10 || true

  echo "==> [6/6] Generating HTML report"
  rm -rf "$html"
  genhtml "$info_filtered" \
    --output-directory "$html" \
    --branch-coverage \
    --legend \
    --title "ra8-firmware host coverage" \
    --ignore-errors source,inconsistent,corrupt \
    --quiet

  echo ""
  echo "==> Coverage summary:"
  # lcov --summary writes to stderr; capture it and pretty-print.
  summary="$(lcov --summary "$info_filtered" \
    --rc lcov_branch_coverage=1 \
    --ignore-errors inconsistent,empty 2>&1 || true)"
  echo "$summary" | grep -E 'lines|functions|branches' || true

  echo ""
  echo "HTML report: $html/index.html"
}

if [[ "$(uname -s)" == "Darwin" ]]; then
  # Run inside the same image scripts/ci/test-docker.sh uses.
  if ! command -v docker >/dev/null 2>&1; then
    echo "error: docker not on PATH (need it on macOS)" >&2
    exit 1
  fi
  if command -v colima >/dev/null 2>&1; then
    if ! colima status >/dev/null 2>&1; then
      echo "==> Starting colima VM"
      colima start --cpu 4 --memory 6
    fi
  fi
  if ! docker info >/dev/null 2>&1; then
    echo "error: docker daemon not reachable" >&2
    exit 1
  fi

  IMAGE_TAG="ra8-firmware-test:latest"
  if ! docker image inspect "$IMAGE_TAG" >/dev/null 2>&1; then
    echo "==> Building $IMAGE_TAG"
    docker build -t "$IMAGE_TAG" -f "$REPO_ROOT/.devcontainer/Dockerfile" "$REPO_ROOT/.devcontainer"
  fi

  # Re-exec this script inside the container. If the image predates the
  # Dockerfile change that adds lcov, install it on the fly.
  docker run --rm \
    -v "$REPO_ROOT":/work \
    -w /work \
    "$IMAGE_TAG" \
    bash -lc '
            set -e
            if ! command -v lcov >/dev/null 2>&1; then
                echo "==> Installing lcov in container (image predates Dockerfile update)"
                if [[ $EUID -eq 0 ]]; then
                    SUDO=""
                else
                    SUDO="sudo"
                fi
                $SUDO apt-get update -qq
                $SUDO env DEBIAN_FRONTEND=noninteractive \
                    apt-get install -y -qq --no-install-recommends lcov
            fi
            bash scripts/report/coverage_report_host.sh --in-container
        '
  exit $?
fi

# --- Linux native path (or already inside the container) ---
if [[ "${1:-}" == "--in-container" ]]; then
  shift
fi

if ! command -v lcov >/dev/null 2>&1 || ! command -v genhtml >/dev/null 2>&1; then
  echo "error: lcov / genhtml not on PATH" >&2
  echo "  Debian/Ubuntu: sudo apt-get install lcov" >&2
  echo "  macOS:         brew install lcov" >&2
  exit 1
fi

run_native "$REPO_ROOT"
