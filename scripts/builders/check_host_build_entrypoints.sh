#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# scripts/builders/check_host_build_entrypoints.sh -- audit the native host
# build entry points.
#
# This is a minimal trusted launcher, not an implementation: the audit is
# performed by the Zig host tool tools/check_host_build_entrypoints (#858),
# which replaced the Python scripts/checks/check_host_build_entrypoints.py, now
# deleted. PATHREF-OK: history, not a live path. All this does is resolve zig,
# build the tool once, and hand over argv and the exit status unchanged.
#
#     bash scripts/builders/check_host_build_entrypoints.sh --selftest
#     bash scripts/builders/check_host_build_entrypoints.sh
#
# Invoked by the host-build authority block in scripts/ci/gates/checks.sh.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TOOL_DIR="${ROOT}/tools/check_host_build_entrypoints"

# A missing zig is FATAL rather than a skipped gate. A check that exits 0
# because it could not run is the vacuous green this gate exists to prevent.
ZIG="${ZIG:-}"
if [ -z "${ZIG}" ]; then
  if command -v zig >/dev/null 2>&1; then
    ZIG="$(command -v zig)"
  elif [ -x "${HOME}/.local/zig/zig" ]; then
    ZIG="${HOME}/.local/zig/zig"
  fi
fi
if [ -z "${ZIG}" ]; then
  echo "check_host_build_entrypoints.sh: FATAL -- zig not found. Set ZIG, or put zig on PATH." >&2
  exit 2
fi

"${ZIG}" build \
  --build-file "${TOOL_DIR}/build.zig" \
  --cache-dir "${TOOL_DIR}/build/cache" \
  --prefix "${TOOL_DIR}/build" \
  -Doptimize=ReleaseSafe >/dev/null

RA8_REPO_ROOT="${ROOT}" exec "${TOOL_DIR}/build/bin/check_host_build_entrypoints" "$@"
