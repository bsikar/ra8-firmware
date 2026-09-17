#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# scripts/builders/check_no_unsafe_python_install.sh -- gate: reject
# first-party attempts to override PEP 668 package ownership.
#
# This is a minimal trusted launcher, not an implementation: the detector is
# the Zig host tool tools/check_no_unsafe_python_install (#858), which
# replaced scripts/checks/check_no_unsafe_python_install.py. PATHREF-OK: history, not a live path.
# The predecessor was deleted in the same change. All this does is resolve
# zig, build the tool once, and hand over argv and the exit status unchanged.
#
#     bash scripts/builders/check_no_unsafe_python_install.sh --selftest
#     bash scripts/builders/check_no_unsafe_python_install.sh
#
# Invoked by the checks gate in scripts/ci/gates/checks.sh.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TOOL_DIR="${ROOT}/tools/check_no_unsafe_python_install"

# A missing zig is FATAL rather than a skipped gate. A quality gate that
# cannot run must never exit 0: that reports a clean tree for exactly the
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
  echo "check_no_unsafe_python_install.sh: FATAL -- zig not found. Set ZIG, or put zig on PATH." >&2
  exit 2
fi

"${ZIG}" build \
  --build-file "${TOOL_DIR}/build.zig" \
  --cache-dir "${TOOL_DIR}/build/cache" \
  --prefix "${TOOL_DIR}/build" \
  -Doptimize=ReleaseSafe >/dev/null

RA8_REPO_ROOT="${ROOT}" exec "${TOOL_DIR}/build/bin/check_no_unsafe_python_install" "$@"
