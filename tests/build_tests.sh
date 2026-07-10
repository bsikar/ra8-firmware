#!/usr/bin/env bash
#
# tests/build_tests.sh -- configure + build the host unit-test suite.
#
# Two build modes are supported, each rooted in its own out-of-tree
# directory so toggling between them does not invalidate the other:
#
#   default     ->  tests/build/        (RA_MCDC=OFF, fast iteration)
#   --coverage  ->  tests/build-cov/    (RA_MCDC=ON,  llvm-cov MC/DC)
#
# Coverage rebuilds are expensive (every TU is recompiled with
# -fcoverage-mcdc / -fprofile-instr-generate). Keeping the two trees
# separate means a `make test` after `make mcdc` is still incremental.
#
# Usage:
#
#     tests/build_tests.sh                # fast unit-test build
#     tests/build_tests.sh --coverage     # MC/DC-instrumented build
#
# Environment overrides:
#     CMAKE   -- cmake binary (default: cmake on PATH)
#     CC, CXX -- host compilers. When unset, a C23-capable compiler is
#                auto-selected (see pick below); a bare `cc` is often an
#                older gcc that rejects `typedef enum : uint8_t`.
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

CMAKE="${CMAKE:-cmake}"

MODE="fast"
if [[ "${1:-}" == "--coverage" ]]; then
  MODE="coverage"
  shift
elif [[ "${1:-}" == "--ubsan" ]]; then
  MODE="ubsan"
  shift
fi

if [[ "$MODE" == "coverage" ]]; then
  BUILD_DIR="$SCRIPT_DIR/build-cov"
  CMAKE_ARGS=(-DRA_MCDC=ON -DRA_COVERAGE=OFF)
  LABEL="coverage (RA_MCDC=ON)"
elif [[ "$MODE" == "ubsan" ]]; then
  BUILD_DIR="$SCRIPT_DIR/build-ubsan"
  CMAKE_ARGS=(-DRA_COVERAGE=OFF -DRA_SANITIZE=undefined)
  LABEL="ubsan (RA_SANITIZE=undefined)"
else
  BUILD_DIR="$SCRIPT_DIR/build"
  CMAKE_ARGS=()
  LABEL="fast"
fi

# Auto-select a C23-capable host compiler (clang >= 17 or gcc >= 13) when the
# caller has not pinned one. Shared with the coverage builds via the helper so
# the whole host-test tooling agrees on the compiler.
# shellcheck source=scripts/utils/select_host_compiler.sh
. "$SCRIPT_DIR/../scripts/utils/select_host_compiler.sh"
ra_select_host_compiler || exit 1
echo "    using CC=$CC CXX=$CXX"

CMAKE_ARGS+=("-DCMAKE_C_COMPILER=$CC")
CMAKE_ARGS+=("-DCMAKE_CXX_COMPILER=$CXX")

# Determine parallelism without depending on bash-only or GNU-only
# coreutils. nproc is Linux; sysctl is macOS; fall back to 4.
if command -v nproc >/dev/null 2>&1; then
  JOBS="$(nproc)"
elif command -v sysctl >/dev/null 2>&1; then
  JOBS="$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"
else
  JOBS=4
fi

echo "==> ra8-firmware tests: building ($LABEL)"
echo "    build dir : $BUILD_DIR"
echo "    jobs      : $JOBS"

mkdir -p "$BUILD_DIR"

"$CMAKE" -B "$BUILD_DIR" -S "$SCRIPT_DIR" \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -Wno-dev \
  "${CMAKE_ARGS[@]}"

"$CMAKE" --build "$BUILD_DIR" --parallel "$JOBS"

echo ""
echo "==> Build complete. Run with:"
if [[ "$MODE" == "coverage" ]]; then
  echo "    tests/run_tests.sh --coverage"
else
  echo "    tests/run_tests.sh"
fi
