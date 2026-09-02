#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# Build standalone shared-library projects and route consumer fragments through
# the repository test harness that actually instantiates them.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

shared_dirs() {
  find "$REPO_ROOT/apps/shared_libs" -mindepth 2 -maxdepth 2 -name CMakeLists.txt -type f -print0 |
    sort -z |
    while IFS= read -r -d '' cmake_file; do dirname "$cmake_file"; done
}

is_standalone() {
  grep -Eq '^[[:space:]]*project[[:space:]]*\(' "$1/CMakeLists.txt"
}

resolve_libs() {
  local wanted="$1" dir
  while IFS= read -r dir; do
    if [ "$wanted" = all ] || [ "${dir##*/}" = "$wanted" ]; then
      printf '%s\n' "$dir"
    fi
  done < <(shared_dirs)
}

build_all() {
  local wanted="$1" dir found=0 fragments=0
  while IFS= read -r dir; do
    found=1
    if is_standalone "$dir"; then
      echo "==> Building standalone ${dir#"$REPO_ROOT/"}"
      "$SCRIPT_DIR/host_cmake.sh" "$dir" "$dir/build"
    else
      fragments=1
      echo "==> ${dir#"$REPO_ROOT/"} is a consumer CMake fragment"
    fi
  done < <(resolve_libs "$wanted")
  if [ "$found" -eq 0 ]; then
    echo "unknown shared library '$wanted'; run 'just apps::shared::list'" >&2
    return 2
  fi
  if [ "$fragments" -eq 1 ]; then
    echo "==> Building and testing consumer fragments through the canonical test harness"
    "${RA8_JUST:-just}" apps::shared::test "$wanted"
  fi
}

build_all "${1:-all}"
