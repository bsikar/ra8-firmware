#!/usr/bin/env bash
#
# scripts/utils/coverage_report.sh -- statement + branch coverage report.
#
# This is the canonical entry point for plain gcov/gcovr statement and
# branch coverage. It complements (but does NOT replace) the existing
# DO-178C Level B MC/DC pipeline (scripts/utils/mcdc_report.sh, wired to
# `make mcdc`). MC/DC subsumes branch coverage in theory, but for the
# IEC 61508 SIL 3 / DO-178C Level B audit trail we maintain a separate
# statement + branch baseline that can be ratcheted independently and
# diffed against per-PR.
#
# Pipeline:
#   1. Configure tests/build-cov/ with RA_COVERAGE=ON and RA_MCDC=OFF
#      (the two instrumentation modes are mutually exclusive --
#      tests/CMakeLists.txt forces RA_COVERAGE=OFF when RA_MCDC=ON,
#      so we explicitly pass both flags here).
#   2. Build all host tests.
#   3. Run them via ctest.
#   4. gcovr filtered to first-party libs/ra_* and src/, with HTML
#      details, Cobertura XML (consumed by check_coverage.py), and
#      a printed line/branch summary.
#
# On macOS the test executables segfault because the simulator uses
# MAP_FIXED below 4 GiB which arm64 macOS rejects, so we re-exec
# inside the project's Linux devcontainer just like
# scripts/coverage_report.sh and scripts/utils/mcdc_report.sh.
#
# Outputs (under build/coverage/):
#   index.html         -- gcovr --html-details report
#   coverage.xml       -- gcovr --xml (Cobertura format) for the gate
#   summary.txt        -- one-line gcovr summary
#
# Usage:
#   bash scripts/utils/coverage_report.sh
#   make coverage
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

BUILD_DIR_REL="tests/build-cov"
REPORT_DIR_REL="build/coverage"

# ---------------------------------------------------------------------------
# macOS shim: re-exec inside the Linux devcontainer.
# ---------------------------------------------------------------------------
if [[ "$(uname -s)" == "Darwin" && "${1:-}" != "--in-container" ]]; then
  if ! command -v docker >/dev/null 2>&1; then
    echo "error: docker not on PATH (need it on macOS for coverage)" >&2
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
    docker build -t "$IMAGE_TAG" \
      -f "$REPO_ROOT/.devcontainer/Dockerfile" \
      "$REPO_ROOT/.devcontainer"
  fi

  docker run --rm \
    -v "$REPO_ROOT":/work \
    -w /work \
    "$IMAGE_TAG" \
    bash -lc '
            set -e
            if ! command -v gcovr >/dev/null 2>&1; then
                echo "==> Installing gcovr in container"
                if [[ $EUID -eq 0 ]]; then SUDO=""; else SUDO="sudo"; fi
                $SUDO apt-get update -qq
                $SUDO env DEBIAN_FRONTEND=noninteractive \
                    apt-get install -y -qq --no-install-recommends gcovr
            fi
            bash scripts/utils/coverage_report.sh --in-container
        '
  exit $?
fi

# Drop the --in-container marker if present.
if [[ "${1:-}" == "--in-container" ]]; then
  shift
fi

if ! command -v gcovr >/dev/null 2>&1; then
  echo "error: gcovr not on PATH (pip3 install gcovr)" >&2
  exit 1
fi

BUILD_DIR="$REPO_ROOT/$BUILD_DIR_REL"
REPORT_DIR="$REPO_ROOT/$REPORT_DIR_REL"

mkdir -p "$REPORT_DIR"

# Pin a C23-capable host compiler. CMake otherwise defaults to a bare "cc",
# which on the Debian 12 dev box is gcc 12 and cannot parse this codebase's
# C23 typed enums -- the same selection the host-test build uses.
# shellcheck source=scripts/utils/select_host_compiler.sh
. "$SCRIPT_DIR/select_host_compiler.sh"
ra_select_host_compiler gcc-14 gcc-13 gcc clang-19 clang cc

# CMake refuses to change CMAKE_C_COMPILER on an existing cache; if a prior
# configure pinned a different compiler, wipe the tree so the new one applies.
if [[ -f "$BUILD_DIR/CMakeCache.txt" ]]; then
  _cached_cc="$(sed -n 's/^CMAKE_C_COMPILER:[^=]*=//p' "$BUILD_DIR/CMakeCache.txt")"
  if [[ "$_cached_cc" != "$(command -v "$CC")" ]]; then
    rm -rf "$BUILD_DIR"
  fi
fi

echo "==> [1/4] Configuring host tests with RA_COVERAGE=ON RA_MCDC=OFF (CC=$CC)"
cmake -B "$BUILD_DIR" -S "$REPO_ROOT/tests" \
  -DCMAKE_BUILD_TYPE=Debug \
  -DRA_COVERAGE=ON \
  -DRA_MCDC=OFF \
  -DCMAKE_C_COMPILER="$CC" \
  -DCMAKE_CXX_COMPILER="$CXX" \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -Wno-dev >/dev/null

echo "==> [2/4] Building"
cmake --build "$BUILD_DIR" --parallel >/dev/null

echo "==> [3/4] Running ctest"
(cd "$BUILD_DIR" && ctest --output-on-failure) | tail -10 || true

echo "==> [4/4] Running gcovr (HTML + Cobertura XML)"
# Wipe any stale gcov artifacts from prior MC/DC / lcov / fuzz runs so a
# mixed-compiler build dir cannot feed gcovr leftover data. The selected
# compiler's gcov tool is pinned via --gcov-executable below, so there is no
# longer a clang-data-read-by-gcc-gcov mismatch to suppress (the old
# --gcov-ignore-errors workaround is gone; it also breaks gcovr < 6).
find "$BUILD_DIR" -name '*.gcov.json.gz' -delete 2>/dev/null || true

gcovr \
  --gcov-executable "$(ra_gcov_executable_for "$CC")" \
  --root "$REPO_ROOT" \
  --filter "libs/ra_" \
  --filter "src/" \
  --exclude "libs/third_party/" \
  --exclude "tests/" \
  --exclude "build/" \
  --exclude-throw-branches \
  --exclude-unreachable-branches \
  --html-details "$REPORT_DIR/index.html" \
  --xml "$REPORT_DIR/coverage.xml" \
  --txt "$REPORT_DIR/summary.txt" \
  --print-summary \
  "$BUILD_DIR" |
  tee "$REPORT_DIR/print-summary.txt"

echo ""
echo "==> HTML report:   $REPORT_DIR/index.html"
echo "==> Cobertura XML: $REPORT_DIR/coverage.xml"
