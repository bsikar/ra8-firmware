#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# scripts/builders/check_inclusive_terminology_commits.sh -- gate: no commit
# message on a push or PR spells the banned legacy SPI/I2C terminology.
#
# This is a minimal trusted launcher, not an implementation: the detector is
# the Zig host tool tools/check_inclusive_terminology_commits (#858), which
# replaced the Python scripts/checks/check_inclusive_terminology_commits.py
# PATHREF-OK: the predecessor this names was deleted in the same change.
#
# All this does is resolve zig, build the tool once, and hand over argv, stdin
# and the exit status unchanged.
#
#     bash scripts/builders/check_inclusive_terminology_commits.sh --selftest
#     git log BASE..HEAD --format=%B |
#       bash scripts/builders/check_inclusive_terminology_commits.sh
#
# Invoked by the inclusive-terminology-commits gate in scripts/ci/gates/hygiene.sh.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TOOL_DIR="${ROOT}/tools/check_inclusive_terminology_commits"

# A missing zig is FATAL rather than a skipped gate. A quality gate that
# cannot run must never exit 0: that reports a clean history for exactly the
# wrong reason, which is the failure this gate exists to prevent.
ZIG="${ZIG:-}"
if [ -z "${ZIG}" ]; then
  if command -v zig >/dev/null 2>&1; then
    ZIG="$(command -v zig)"
  elif [ -x "${HOME}/.local/zig/zig" ]; then
    ZIG="${HOME}/.local/zig/zig"
  fi
fi
if [ -z "${ZIG}" ]; then
  echo "check_inclusive_terminology_commits.sh: FATAL -- zig not found. Set ZIG, or put zig on PATH." >&2
  exit 2
fi

# The build writes to stderr on failure and nothing on success, and must not
# reach stdout: the gate's report is parsed by eye beside the commit range.
"${ZIG}" build \
  --build-file "${TOOL_DIR}/build.zig" \
  --cache-dir "${TOOL_DIR}/build/cache" \
  --prefix "${TOOL_DIR}/build" \
  -Doptimize=ReleaseSafe >/dev/null

exec "${TOOL_DIR}/build/bin/check_inclusive_terminology_commits" "$@"
