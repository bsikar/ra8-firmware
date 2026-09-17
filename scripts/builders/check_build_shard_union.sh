#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# scripts/builders/check_build_shard_union.sh -- the cross-build shard-union
# gate.
#
# This is a minimal trusted launcher, not an implementation: the gate is the
# Zig host tool tools/check_build_shard_union (#858, #1159), which replaced
# the Python scripts/checks/check_build_shard_union.py  PATHREF-OK: the
# predecessor this names was deleted in the same change.
#
# All this does is resolve zig, build the tool when it is missing or older
# than its sources, and hand over argv and the exit status unchanged.
#
#     bash scripts/builders/check_build_shard_union.sh --selftest
#     bash scripts/builders/check_build_shard_union.sh --shards N
#
# Exit status is the tool's: 0 the shards covered the tree exactly once, 1 a
# discrepancy, 2 a usage error.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TOOL_DIR="${ROOT}/tools/check_build_shard_union"
BINARY="${TOOL_DIR}/build/bin/check_build_shard_union"

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
    echo "check_build_shard_union.sh: FATAL -- zig not found. Set ZIG, or put zig on PATH." >&2
    exit 2
  fi
  "${ZIG}" build \
    --build-file "${TOOL_DIR}/build.zig" \
    --cache-dir "${TOOL_DIR}/build/cache" \
    --prefix "${TOOL_DIR}/build" \
    -Doptimize=ReleaseSafe >/dev/null
fi

RA8_REPO_ROOT="${RA8_REPO_ROOT:-${ROOT}}" exec "${BINARY}" "$@"
