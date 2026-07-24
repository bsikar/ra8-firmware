#!/usr/bin/env bash
#
# scripts/builders/select_host_compiler.sh -- shared C23-capable host-compiler
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

# Echo the C++ driver that matches C compiler $1 (clang-19 -> clang++-19,
# gcc-14 -> g++-14, anything else -> c++). Kept as a helper so the selection
# loop and the final CXX assignment derive the pairing identically.
ra8_cxx_for_cc() {
  case "$1" in
    clang*) echo "${1/clang/clang++}" ;;
    gcc*) echo "${1/gcc/g++}" ;;
    *) echo "c++" ;;
  esac
}

# Set and export CC / CXX to a C23-capable host compiler. Honours a pre-set CC.
# Candidates may be passed as arguments (default: clang-first). Returns non-zero
# (with a message on stderr) if no listed candidate is C23-capable with a
# matching C++ driver.
ra8_select_host_compiler() {
  if [ "$#" -eq 0 ]; then
    set -- clang-19 clang gcc cc
  fi
  if [ -z "${CC:-}" ]; then
    local _cand
    for _cand in "$@"; do
      command -v "$_cand" >/dev/null 2>&1 || continue
      ra8_c23_compiler_ok "$_cand" || continue
      # The host-test and coverage builds enable_language(CXX), so a C compiler
      # whose matching C++ driver is absent (e.g. gcc-14 installed without
      # g++-14, which then leaks onto a shared self-hosted runner) would be
      # selected here and only fail deep in CMake configure. Require the C++
      # half too -- unless the caller pinned CXX explicitly -- so selection
      # falls through to the next complete pair instead.
      if [ -n "${CXX:-}" ] || command -v "$(ra8_cxx_for_cc "$_cand")" >/dev/null 2>&1; then
        CC="$_cand"
        break
      fi
    done
    if [ -z "${CC:-}" ]; then
      echo "error: no C23-capable host compiler with a matching C++ driver found (need clang >= 17 or gcc >= 13, plus its C++ driver)" >&2
      echo "       install clang/g++ or set CC=<compiler> (and CXX) explicitly" >&2
      return 1
    fi
  fi
  if [ -z "${CXX:-}" ]; then
    CXX="$(ra8_cxx_for_cc "$CC")"
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
