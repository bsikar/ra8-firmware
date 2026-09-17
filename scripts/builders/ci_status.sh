#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# scripts/builders/ci_status.sh -- the one reader of the ci-monitor status
# file.
#
# This is a minimal trusted launcher, not an implementation: the reader is the
# Zig host tool tools/ci_status (#858, #1144), which replaced the Python
# scripts/ci/ci_status.py  PATHREF-OK: the predecessor this names was deleted
# in the same change.
#
# All this does is resolve zig, build the tool when it is missing or older
# than its sources, and hand over argv and the exit status unchanged.
#
#     bash scripts/builders/ci_status.sh <state-file> <mode> [arg]
#
# Invoked by monitor.sh's `_status_read`, which calls it several times per
# `status` read, which is why the build is skipped when the binary is already
# current rather than re-run on every call.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TOOL_DIR="${ROOT}/tools/ci_status"
BINARY="${TOOL_DIR}/build/bin/ci_status"

# A missing zig is FATAL rather than a silent fallback. A reader that cannot
# run must never print a plausible-looking answer: every wrong verdict this
# tool has ever handed an agent came from a plausible line and a plausible
# exit status, so it fails loudly instead.
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
    echo "ci_status.sh: FATAL -- zig not found. Set ZIG, or put zig on PATH." >&2
    exit 2
  fi
  "${ZIG}" build \
    --build-file "${TOOL_DIR}/build.zig" \
    --cache-dir "${TOOL_DIR}/build/cache" \
    --prefix "${TOOL_DIR}/build" \
    -Doptimize=ReleaseSafe >/dev/null
fi

exec "${BINARY}" "$@"
