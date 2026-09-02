#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# scripts/builders/select_host_compiler.sh -- shared C23-capable host-compiler
# selection (and the matching gcov tool) for the host-test, coverage, and
# ra8_emulator builds.
#
# C23 fixed-underlying-type enums ("typedef enum : uint8_t") require clang >= 17
# or gcc >= 13. CMake otherwise defaults to a bare "cc", which on the Debian 12
# dev box is gcc 12 and rejects the syntax outright -- breaking every host-test,
# coverage, or ra8_emulator configure that does not pin a compiler. This helper
# compile-tests the feature rather than parsing version strings, so the same
# probe works across gcc, clang, and future versions.
#
# Usage (source it first):
#   ra8_select_host_compiler [candidate ...]   -- sets+exports CC and CXX to the
#       first listed candidate that compiles a C23 typed enum (default order is
#       clang-first; the coverage and ra8_emulator builds pass a gcc-first list so
#       they match CI's gcov pipeline and only fall back to clang where gcc is
#       too old).
#   ra8_select_emulator_compiler               -- selects the normal gcc-first
#       host pair on Linux and requires Clang on macOS, whose CoreGraphics
#       headers use Clang block syntax that Homebrew GCC cannot parse.
#   ra8_cmake_reset_if_incompatible "$dir" ["$source"] -- wipe a CMake build
#       directory whose cached compiler differs from the selected $CC, or whose
#       source path came from a different host/container mount.
#   ra8_gcov_executable_for "$CC"              -- echoes the gcovr --gcov-executable
#       that reads coverage data produced by $CC (llvm-cov for clang, gcov for gcc).
#

# Return success if compiler $1 accepts a C23 fixed-underlying-type enum.
ra8_c23_compiler_ok() {
  printf 'typedef enum : int { k_x = 0 } e_t;\nint main(void){return (int)k_x;}\n' |
    "$1" -std=gnu2x -x c -fsyntax-only - >/dev/null 2>&1
}

# Echo the C++ driver that matches C compiler $1 (clang-19 -> clang++-19,
# gcc-14 -> g++-14, anything else -> c++). Absolute compiler paths retain
# their directory. Kept as a helper so the selection
# loop and the final CXX assignment derive the pairing identically.
ra8_cxx_for_cc() {
  local _dir="" _name="$1"
  if [[ "$_name" == */* ]]; then
    _dir="${_name%/*}/"
    _name="${_name##*/}"
  fi
  case "$_name" in
    clang*) echo "${_dir}${_name/clang/clang++}" ;;
    gcc*) echo "${_dir}${_name/gcc/g++}" ;;
    *) echo "c++" ;;
  esac
}

# Return success if compiler $1 is an executable C++ driver. The source probe
# catches a stale/broken explicitly pinned CXX before CMake caches it.
ra8_cxx_compiler_ok() {
  printf 'int main(){return 0;}\n' |
    "$1" -x c++ -fsyntax-only - >/dev/null 2>&1
}

# Set and export CC / CXX to a C23-capable host compiler. Honours a pre-set CC.
# Candidates may be passed as arguments (default: clang-first). Returns non-zero
# (with a message on stderr) if no listed candidate is C23-capable with a
# matching C++ driver.
ra8_select_host_compiler() {
  if [ "$#" -eq 0 ]; then
    set -- clang-19 clang gcc cc
  fi
  if [ -n "${CC:-}" ]; then
    if ! command -v "$CC" >/dev/null 2>&1 || ! ra8_c23_compiler_ok "$CC"; then
      echo "error: explicitly selected CC='$CC' is absent or cannot compile the repository's C23 typed enums" >&2
      return 1
    fi
  else
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
  if ! command -v "$CXX" >/dev/null 2>&1 || ! ra8_cxx_compiler_ok "$CXX"; then
    echo "error: explicitly selected or paired CXX='$CXX' is absent or cannot compile C++" >&2
    return 1
  fi
  export CC CXX
}

# Select a C23 pair suitable for the emulator's host UI backend. The macOS
# sources include system framework headers that use Clang extensions, so a
# C23-capable Homebrew GCC is still not a valid compiler for this target.
ra8_select_emulator_compiler() {
  if [ "$(uname -s)" != "Darwin" ]; then
    ra8_select_host_compiler gcc-14 gcc-13 gcc clang-19 clang cc
    return
  fi

  if [ -n "${CC:-}" ] && ! "$CC" --version 2>/dev/null | head -1 | grep -qi clang; then
    echo "error: the macOS emulator requires Clang because CoreGraphics uses Clang block syntax; unset CC='$CC' or select clang" >&2
    return 1
  fi
  ra8_select_host_compiler clang-19 clang /usr/bin/clang
}

# Reset CMake output that cannot be reused in this environment. CMake stores
# absolute source and compiler paths, so an in-source build configured on macOS
# is invalid at the devcontainer's /workspace mount even when both use the same
# repository bytes. The optional source argument checks that path; a selected
# CC/CXX check the enabled compilers. A compatible or unconfigured directory
# is untouched; C-only projects have no CXX cache entry, which is ignored.
ra8_cmake_reset_if_incompatible() {
  local _build_dir="$1"
  local _source_dir="${2:-}"
  local _cache="$_build_dir/CMakeCache.txt"
  [ -f "$_cache" ] || return 0

  local _must_reset=0
  if [ -n "$_source_dir" ]; then
    local _cached_source _expected_source
    _cached_source="$(sed -n 's/^CMAKE_HOME_DIRECTORY:[^=]*=//p' "$_cache")"
    _expected_source="$(cd "$_source_dir" && pwd -P)"
    [ "$_cached_source" = "$_expected_source" ] || _must_reset=1
  fi
  if [ -n "${CC:-}" ]; then
    local _cached_cc
    _cached_cc="$(sed -n 's/^CMAKE_C_COMPILER:[^=]*=//p' "$_cache")"
    [ "$_cached_cc" = "$(command -v "$CC")" ] || _must_reset=1
  fi
  if [ -n "${CXX:-}" ]; then
    local _cached_cxx
    _cached_cxx="$(sed -n 's/^CMAKE_CXX_COMPILER:[^=]*=//p' "$_cache")"
    if [ -n "$_cached_cxx" ]; then
      [ "$_cached_cxx" = "$(command -v "$CXX")" ] || _must_reset=1
    fi
  fi
  if [ "$_must_reset" -eq 1 ]; then
    rm -rf "$_build_dir"
  fi
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
