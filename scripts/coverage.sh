#!/bin/bash
# coverage.sh -- run the host unit tests with gcov instrumentation
# and produce an HTML + text coverage report.
#
# Usage:
#   ./scripts/coverage.sh
#   ./scripts/coverage.sh --gate   # exit non-zero if below 90% line / 80% branch
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FW_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$FW_DIR/build/coverage"

GATE=false
if [[ "${1:-}" == "--gate" ]]; then
  GATE=true
fi

GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m'

if ! command -v gcovr &>/dev/null; then
  echo -e "${RED}Error:${NC} gcovr not found. Install with:"
  echo "  pip3 install gcovr"
  exit 1
fi

# Pin a C23-capable host compiler. CMake otherwise defaults to a bare "cc",
# which on the Debian 12 dev box is gcc 12 and cannot parse this codebase's
# C23 typed enums -- the same selection the host-test build uses.
# shellcheck source=scripts/utils/select_host_compiler.sh
# Prefer gcc (matches CI's gcov pipeline); fall back to clang where the host
# gcc is too old for C23 (the dev box ships gcc 12).
. "$SCRIPT_DIR/utils/select_host_compiler.sh"
ra_select_host_compiler gcc-14 gcc-13 gcc clang-19 clang cc

# CMake refuses to change CMAKE_C_COMPILER on an existing cache; if a prior
# configure pinned a different compiler, wipe the tree so the new one applies.
if [[ -f "$BUILD_DIR/CMakeCache.txt" ]]; then
  _cached_cc="$(sed -n 's/^CMAKE_C_COMPILER:[^=]*=//p' "$BUILD_DIR/CMakeCache.txt")"
  if [[ "$_cached_cc" != "$(command -v "$CC")" ]]; then
    rm -rf "$BUILD_DIR"
  fi
fi

echo -e "${YELLOW}[1/4]${NC} Configuring coverage build (CC=$CC)..."
cmake -B "$BUILD_DIR" -S "$FW_DIR/tests" \
  -DCMAKE_BUILD_TYPE=Debug \
  -DRA_COVERAGE=ON \
  -DCMAKE_C_COMPILER="$CC" \
  -DCMAKE_CXX_COMPILER="$CXX" \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -Wno-dev >/dev/null

echo -e "${YELLOW}[2/4]${NC} Building..."
cmake --build "$BUILD_DIR" --parallel >/dev/null

echo -e "${YELLOW}[3/4]${NC} Running ctest..."
chmod +x "$BUILD_DIR"/test_* || true
ctest --test-dir "$BUILD_DIR" --output-on-failure --timeout 60 | tail -4

mkdir -p "$BUILD_DIR/coverage"
echo -e "${YELLOW}[4/4]${NC} Running gcovr..."

GCOVR_OPTS=(
  --gcov-executable "$(ra_gcov_executable_for "$CC")"
  --root "$FW_DIR"
  --object-directory "$BUILD_DIR"
  --filter "$FW_DIR/libs/"
  --filter "$FW_DIR/src/"
  --exclude "$FW_DIR/libs/third_party/"
  --exclude "$FW_DIR/tests/"
  --exclude-throw-branches
  --exclude-unreachable-branches
  --html-details "$BUILD_DIR/coverage/index.html"
  --txt "$BUILD_DIR/coverage/summary.txt"
  --json "$BUILD_DIR/coverage/coverage.json"
  --print-summary
)

if [[ "$GATE" == "true" ]]; then
  GCOVR_OPTS+=(
    --fail-under-line 90
    --fail-under-branch 80
  )
fi

if gcovr "${GCOVR_OPTS[@]}"; then
  echo ""
  echo -e "${GREEN}[PASS]${NC} Aggregate coverage report: $BUILD_DIR/coverage/index.html"
  cat "$BUILD_DIR/coverage/summary.txt" 2>/dev/null || true
  if [[ "$GATE" == "true" ]]; then
    # Per-file FLOOR: the aggregate line gate above can be met while an
    # individual file rots below 90%; enforce >= 90% line on EVERY first-party
    # file, with no allowlist ("no grandfather"). Reads the JSON just written.
    echo ""
    if ! python3 "$FW_DIR/scripts/utils/check_coverage_floor.py"; then
      echo -e "${RED}[FAIL]${NC} Per-file coverage floor failed"
      exit 1
    fi
  fi
  exit 0
else
  echo ""
  echo -e "${RED}[FAIL]${NC} Coverage gate failed"
  cat "$BUILD_DIR/coverage/summary.txt" 2>/dev/null || true
  exit 1
fi
