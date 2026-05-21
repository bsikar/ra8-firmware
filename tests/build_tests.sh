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
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

CMAKE="${CMAKE:-cmake}"

MODE="fast"
if [[ "${1:-}" == "--coverage" ]]; then
    MODE="coverage"
    shift
fi

if [[ "$MODE" == "coverage" ]]; then
    BUILD_DIR="$SCRIPT_DIR/build-cov"
    CMAKE_ARGS=(-DRA_MCDC=ON -DRA_COVERAGE=OFF)
    LABEL="coverage (RA_MCDC=ON)"
else
    BUILD_DIR="$SCRIPT_DIR/build"
    CMAKE_ARGS=()
    LABEL="fast"
fi

# Auto-select a C23-capable host compiler when the caller has not
# pinned one. C23 fixed-underlying-type enums (`typedef enum : uint8_t`)
# require clang >= 17 or gcc >= 13; CMake otherwise defaults to a bare
# `cc`, which on this host is gcc 12 and rejects the syntax. The probe
# below compile-tests the feature rather than parsing version strings.
c23_compiler_ok() {
    printf 'typedef enum : int { k_x = 0 } e_t;\nint main(void){return (int)k_x;}\n' \
        | "$1" -std=gnu2x -x c -fsyntax-only - >/dev/null 2>&1
}

if [[ -z "${CC:-}" ]]; then
    for _cand in clang-19 clang gcc cc; do
        if command -v "$_cand" >/dev/null 2>&1 && c23_compiler_ok "$_cand"; then
            CC="$_cand"
            break
        fi
    done
    if [[ -z "${CC:-}" ]]; then
        echo "error: no C23-capable host compiler found (need clang >= 17 or gcc >= 13)" >&2
        echo "       install clang or set CC=<compiler> explicitly" >&2
        exit 1
    fi
    echo "    auto-selected CC=$CC"
fi
if [[ -z "${CXX:-}" ]]; then
    case "$CC" in
        clang*) CXX="${CC/clang/clang++}" ;;
        gcc*)   CXX="${CC/gcc/g++}" ;;
        *)      CXX="c++" ;;
    esac
fi

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

echo "==> ra8d2-firmware tests: building ($LABEL)"
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
