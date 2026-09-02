#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# Configure and build one native CMake project with a proven C23 compiler pair.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

# shellcheck source=scripts/builders/select_host_compiler.sh
. "$SCRIPT_DIR/select_host_compiler.sh"
# shellcheck source=scripts/ci/lib/parallelism.sh
. "$REPO_ROOT/scripts/ci/lib/parallelism.sh"

usage() {
  echo "Usage: scripts/builders/host_cmake.sh [--emulator] [--target NAME] SOURCE BUILD [CMAKE_ARG ...]" >&2
}

_write_fake_compiler() {
  local path="$1" status="$2"
  printf '#!/usr/bin/env bash\nexit %s\n' "$status" >"$path"
  chmod 0755 "$path"
}

_cache_fixture() {
  local build="$1" source="$2" cc="$3" cxx="$4"
  mkdir -p "$build"
  printf 'CMAKE_HOME_DIRECTORY:INTERNAL=%s\nCMAKE_C_COMPILER:FILEPATH=%s\nCMAKE_CXX_COMPILER:FILEPATH=%s\n' \
    "$source" "$cc" "$cxx" >"$build/CMakeCache.txt"
}

_selftest_compilers() {
  local scratch="$1" good_cc="$2" good_cxx="$3" bad="$4"
  if (CC="$bad" CXX="$good_cxx" ra8_select_host_compiler) >/dev/null 2>&1; then
    echo "host_cmake.sh --selftest: incompatible explicit CC was accepted" >&2
    return 1
  fi
  if (CC="$good_cc" CXX="$bad" ra8_select_host_compiler) >/dev/null 2>&1; then
    echo "host_cmake.sh --selftest: incompatible explicit CXX was accepted" >&2
    return 1
  fi
  (CC="$good_cc" CXX="$good_cxx" ra8_select_host_compiler) || {
    echo "host_cmake.sh --selftest: compatible explicit compiler pair was rejected" >&2
    return 1
  }
  [ -d "$scratch" ]
}

_selftest_caches() {
  local scratch="$1" good_cc="$2" good_cxx="$3" source
  source="$scratch/source"
  mkdir -p "$source"
  CC="$good_cc" CXX="$good_cxx"
  _cache_fixture "$scratch/good" "$source" "$good_cc" "$good_cxx"
  ra8_cmake_reset_if_incompatible "$scratch/good" "$source"
  [ -f "$scratch/good/CMakeCache.txt" ] || return 1
  _cache_fixture "$scratch/stale-c" "$source" /wrong/cc "$good_cxx"
  ra8_cmake_reset_if_incompatible "$scratch/stale-c" "$source"
  [ ! -e "$scratch/stale-c" ] || return 1
  _cache_fixture "$scratch/stale-cxx" "$source" "$good_cc" /wrong/cxx
  ra8_cmake_reset_if_incompatible "$scratch/stale-cxx" "$source"
  [ ! -e "$scratch/stale-cxx" ] || return 1
}

selftest() {
  local scratch good_cc good_cxx bad
  scratch="$(mktemp -d "${TMPDIR:-/tmp}/ra8-host-cmake.XXXXXXXX")"
  HOST_CMAKE_SELFTEST_SCRATCH="$scratch"
  trap 'rm -rf -- "${HOST_CMAKE_SELFTEST_SCRATCH:?}"' EXIT
  good_cc="$scratch/clang-99"
  good_cxx="$scratch/clang++-99"
  bad="$scratch/gcc-12"
  _write_fake_compiler "$good_cc" 0
  _write_fake_compiler "$good_cxx" 0
  _write_fake_compiler "$bad" 1
  _selftest_compilers "$scratch" "$good_cc" "$good_cxx" "$bad"
  _selftest_caches "$scratch" "$good_cc" "$good_cxx"
  rm -rf -- "$scratch"
  trap - EXIT
  unset HOST_CMAKE_SELFTEST_SCRATCH
  echo "host_cmake.sh --selftest: PASS (explicit CC/CXX both ways; stale C/CXX caches reset)"
}

main() {
  local emulator=0 target=""
  if [ "${1:-}" = "--selftest" ]; then
    selftest
    return
  fi
  while [ "$#" -gt 0 ]; do
    case "$1" in
      --emulator)
        emulator=1
        shift
        ;;
      --target)
        [ "$#" -ge 2 ] || {
          usage
          return 2
        }
        target="$2"
        shift 2
        ;;
      --)
        shift
        break
        ;;
      *) break ;;
    esac
  done
  [ "$#" -ge 2 ] || {
    usage
    return 2
  }
  local source="$1" build="$2"
  shift 2
  [ -f "$source/CMakeLists.txt" ] || {
    echo "host_cmake: missing $source/CMakeLists.txt" >&2
    return 2
  }
  if [ "$emulator" -eq 1 ]; then
    ra8_select_emulator_compiler
  else
    ra8_select_host_compiler
  fi
  ra8_cmake_reset_if_incompatible "$build" "$source"
  cmake -S "$source" -B "$build" \
    -DCMAKE_C_COMPILER="$(command -v "$CC")" \
    -DCMAKE_CXX_COMPILER="$(command -v "$CXX")" "$@"
  local build_args=(--build "$build" --parallel "$(ra8_max_jobs)")
  [ -z "$target" ] || build_args+=(--target "$target")
  cmake "${build_args[@]}"
}

main "$@"
