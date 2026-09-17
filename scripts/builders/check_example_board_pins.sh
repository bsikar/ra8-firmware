#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# scripts/builders/check_example_board_pins.sh -- the example board-pin gate.
#
# This is a minimal trusted launcher, not an implementation: the gate is the
# Zig host tool tools/check_example_board_pins (#858), which replaced the
# Python scripts/checks/check_example_board_pins.py  PATHREF-OK: the
# predecessor this names was deleted in the same change.
#
# All this does is resolve zig, build the tool when it is missing or older than
# its sources, and hand over argv and the exit status unchanged.
#
#     bash scripts/builders/check_example_board_pins.sh --selftest
#     bash scripts/builders/check_example_board_pins.sh path/to/main.c ...
#
# Exit status is the tool's: 0 no example hand-encodes a board pin, 1 at least
# one does, 2 the whole-tree sweep collapsed below its floor.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TOOL_DIR="${ROOT}/tools/check_example_board_pins"
BINARY="${TOOL_DIR}/build/bin/check_example_board_pins"

# A missing zig is FATAL rather than a silent fallback. A gate that cannot run
# must never look like a gate that passed: this tree's most-repeated defect is
# a check quietly checking less than it claims, so it fails loudly instead.
needs_build=0
if [ ! -x "${BINARY}" ]; then
  needs_build=1
elif [ -n "$(find "${TOOL_DIR}/src" "${TOOL_DIR}/build.zig" -newer "${BINARY}" -print -quit 2>/dev/null)" ]; then
  needs_build=1
fi

if [ "${needs_build}" -eq 1 ]; then
  ZIG="${ZIG:-}"
  if [ -z "${ZIG}" ]; then
    if command -v zig >/dev/null 2>&1; then
      ZIG="$(command -v zig)"
    elif [ -x "${HOME}/.local/zig/zig" ]; then
      ZIG="${HOME}/.local/zig/zig"
    fi
  fi
  if [ -z "${ZIG}" ]; then
    echo "check_example_board_pins.sh: FATAL -- zig not found. Set ZIG, or put zig on PATH." >&2
    exit 2
  fi
  "${ZIG}" build \
    --build-file "${TOOL_DIR}/build.zig" \
    --cache-dir "${TOOL_DIR}/build/cache" \
    --prefix "${TOOL_DIR}/build" \
    -Doptimize=ReleaseSafe >/dev/null
fi

RA8_REPO_ROOT="${RA8_REPO_ROOT:-${ROOT}}" exec "${BINARY}" "$@"
