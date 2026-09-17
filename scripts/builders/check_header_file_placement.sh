#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# scripts/builders/check_header_file_placement.sh -- a header under a src/
# directory shall be module-private (*_internal.h).
#
# This is a minimal trusted launcher, not an implementation: the gate is the
# Zig host tool tools/check_header_file_placement (#858, #1219), which replaced
# the Python scripts/checks/check_header_file_placement.py  PATHREF-OK: the
# predecessor this names was deleted in the same change.
#
# All this does is resolve zig, build the tool when it is missing or older than
# its sources, and hand over argv and the exit status unchanged.
#
#     bash scripts/builders/check_header_file_placement.sh --selftest
#     bash scripts/builders/check_header_file_placement.sh
#     bash scripts/builders/check_header_file_placement.sh libs/ra8_ui/src/x.h
#
# Exit status is the tool's: 0 every src/ header is *_internal (or an explicit
# path list filtered to nothing), 1 a misplaced header or a whole-tree sweep
# whose private-header census fell below the floor, 2 an argv error or
# --selftest handed paths. A 2 can also come from this launcher, for an absent
# zig.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TOOL_DIR="${ROOT}/tools/check_header_file_placement"
BINARY="${TOOL_DIR}/build/bin/check_header_file_placement"

# A missing zig is FATAL rather than a silent fallback. A gate that cannot run
# must never look like a gate that passed: nothing else in the tree notices a
# public interface filed under a private src/ directory.
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
    echo "check_header_file_placement.sh: FATAL -- zig not found. Set ZIG, or put zig on PATH." >&2
    exit 2
  fi
  "${ZIG}" build \
    --build-file "${TOOL_DIR}/build.zig" \
    --cache-dir "${TOOL_DIR}/build/cache" \
    --prefix "${TOOL_DIR}/build" \
    -Doptimize=ReleaseSafe >/dev/null
fi

# Relative arguments and the scan roots resolve against the repository root, as
# the predecessor's REPO_ROOT did, not against the caller's directory.
RA8_REPO_ROOT="${ROOT}" exec "${BINARY}" "$@"
