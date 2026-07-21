#!/usr/bin/env bash
#
# scripts/build/select_host_compiler.sh -- shared C23-capable host-compiler
# selection (and the matching gcov tool) for the host-test and coverage builds.
#
# C23 fixed-underlying-type enums ("typedef enum : uint8_t") require clang >= 17
# or gcc >= 13. CMake otherwise defaults to a bare "cc", which on the Debian 12
# dev box is gcc 12 and rejects the syntax outright -- breaking every host-test
# or coverage configure that does not pin a compiler. This helper compile-tests
# the feature rather than parsing version strings, so the same probe works
# across gcc, clang, and future versions.
#
# Usage (source it first):
#   ra8_select_host_compiler [candidate ...]   -- sets+exports CC and CXX to the
#       first listed candidate that compiles a C23 typed enum (default order is
#       clang-first; the coverage builds pass a gcc-first list so they match
#       CI's gcov pipeline and only fall back to clang where gcc is too old).
#   ra8_gcov_executable_for "$CC"              -- echoes the gcovr --gcov-executable
#       that reads coverage data produced by $CC (llvm-cov for clang, gcov for gcc).
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT

# Return success if compiler $1 accepts a C23 fixed-underlying-type enum.
ra8_c23_compiler_ok() {
  printf 'typedef enum : int { k_x = 0 } e_t;\nint main(void){return (int)k_x;}\n' |
    "$1" -std=gnu2x -x c -fsyntax-only - >/dev/null 2>&1
}

# Set and export CC / CXX to a C23-capable host compiler. Honours a pre-set CC.
# Candidates may be passed as arguments (default: clang-first). Returns non-zero
# (with a message on stderr) if no listed candidate is C23-capable.
ra8_select_host_compiler() {
  if [ "$#" -eq 0 ]; then
    set -- clang-19 clang gcc cc
  fi
  if [ -z "${CC:-}" ]; then
    local _cand
    for _cand in "$@"; do
      if command -v "$_cand" >/dev/null 2>&1 && ra8_c23_compiler_ok "$_cand"; then
        CC="$_cand"
        break
      fi
    done
    if [ -z "${CC:-}" ]; then
      echo "error: no C23-capable host compiler found (need clang >= 17 or gcc >= 13)" >&2
      echo "       install clang or set CC=<compiler> explicitly" >&2
      return 1
    fi
  fi
  if [ -z "${CXX:-}" ]; then
    case "$CC" in
      clang*) CXX="${CC/clang/clang++}" ;;
      gcc*) CXX="${CC/gcc/g++}" ;;
      *) CXX="c++" ;;
    esac
  fi
  export CC CXX
}

# Echo the gcovr --gcov-executable matching compiler $1. clang's coverage data
# (.gcda) must be read by llvm-cov; gcc's by a gcov of the same major version.
ra8_gcov_executable_for() {
  case "$1" in
    clang-*)
      if command -v "llvm-cov-${1#clang-}" >/dev/null 2>&1; then
        echo "llvm-cov-${1#clang-} gcov"
      else
        echo "llvm-cov gcov"
      fi
      ;;
    clang) echo "llvm-cov gcov" ;;
    gcc-*) echo "gcov-${1#gcc-}" ;;
    *) echo "gcov" ;;
  esac
}
