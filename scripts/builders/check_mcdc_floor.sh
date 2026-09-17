#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# scripts/builders/check_mcdc_floor.sh -- the per-file MC/DC floor gate.
#
# This is a minimal trusted launcher, not an implementation: the gate is the
# Zig host tool tools/check_mcdc_floor (#858, #1205), which replaced the
# Python scripts/checks/check_mcdc_floor.py  PATHREF-OK: the predecessor this
# names was deleted in the same change.
#
# All this does is resolve zig, build the tool when it is missing or older than
# its sources, and hand over argv and the exit status unchanged.
#
#     bash scripts/builders/check_mcdc_floor.sh --selftest
#     bash scripts/builders/check_mcdc_floor.sh
#
# Exit status is the tool's: 0 every first-party file with a reachable decision
# is at or above the floor (or --selftest held), 1 an offender, a production
# root with no reachable decision, a missing or unreadable report, or a failing
# selftest case. A 2 can only come from this launcher, for an absent zig.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TOOL_DIR="${ROOT}/tools/check_mcdc_floor"
BINARY="${TOOL_DIR}/build/bin/check_mcdc_floor"

# A missing zig is FATAL rather than a silent fallback. A gate that cannot run
# must never look like a gate that passed: this floor is the only check that
# sees a single rotted file behind well-covered siblings, and the aggregate
# MC/DC rate beside it is neutralized in CI with RA8_MCDC_THRESHOLD=0.
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
    echo "check_mcdc_floor.sh: FATAL -- zig not found. Set ZIG, or put zig on PATH." >&2
    exit 2
  fi
  "${ZIG}" build \
    --build-file "${TOOL_DIR}/build.zig" \
    --cache-dir "${TOOL_DIR}/build/cache" \
    --prefix "${TOOL_DIR}/build" \
    -Doptimize=ReleaseSafe >/dev/null
fi

# The coverage document is resolved against the repository root, as the
# predecessor's REPO_ROOT did, not against the caller's directory.
RA8_REPO_ROOT="${ROOT}" exec "${BINARY}" "$@"
