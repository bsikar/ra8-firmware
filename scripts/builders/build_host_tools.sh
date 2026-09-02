#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# Discovery-driven build/clean dispatcher for compiled host tools.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

tool_dirs() {
  find "$REPO_ROOT/tools" -mindepth 2 -maxdepth 2 -name CMakeLists.txt -type f -print0 |
    sort -z |
    while IFS= read -r -d '' cmake_file; do dirname "$cmake_file"; done
}

resolve_tools() {
  local wanted="$1" dir
  while IFS= read -r dir; do
    if [ "$wanted" = all ] || [ "${dir##*/}" = "$wanted" ]; then
      printf '%s\n' "$dir"
    fi
  done < <(tool_dirs)
}

list_tools() {
  local dir
  while IFS= read -r dir; do printf '%s\n' "${dir##*/}"; done < <(tool_dirs)
}

build_one() {
  local dir="$1" name="${1##*/}" mode=()
  [ "$name" != ra8_emulator ] || mode=(--emulator)
  echo "==> Building tools/$name"
  if ! "$SCRIPT_DIR/host_cmake.sh" "${mode[@]}" "$dir" "$dir/build"; then
    echo "tools/$name failed to build." >&2
    echo "Run 'just setup' and enter 'just dev-shell' for the pinned compiler/toolchain." >&2
    if [ "$name" = ra8_emulator ]; then
      echo "For a native macOS emulator build, also run 'just apps::emulator::setup'." >&2
    fi
    return 1
  fi
}

clean_one() {
  local dir="$1" name="${1##*/}"
  rm -rf -- "$dir/build"
  case "$name" in
    cache_bench) rm -f -- "$dir/cache_bench" "$dir/miniz_host.o" ;;
    reader_vmem) rm -f -- "$dir/reader_vmem" "$dir"/*.trace ;;
    glyph_bench) rm -f -- "$dir/glyph_bench" ;;
  esac
}

main() {
  local action="${1:-}" wanted="${2:-all}" dir found=0
  case "$action" in
    list)
      list_tools
      return
      ;;
    build | clean) ;;
    *)
      echo "Usage: scripts/builders/build_host_tools.sh {list|build|clean} [all|TOOL]" >&2
      return 2
      ;;
  esac
  while IFS= read -r dir; do
    found=1
    if [ "$action" = build ]; then
      build_one "$dir"
    else
      clean_one "$dir"
    fi
  done < <(resolve_tools "$wanted")
  if [ "$found" -eq 0 ]; then
    echo "unknown compiled host tool '$wanted'; run 'just tools::list'" >&2
    return 2
  fi
}

main "$@"
