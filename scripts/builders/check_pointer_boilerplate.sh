#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# scripts/builders/check_pointer_boilerplate.sh -- gate: reject the generated
# pointer-only definition comment in application and example sources.
#
# This is a minimal trusted launcher, not an implementation: the detector is
# the Zig host tool tools/check_pointer_boilerplate (#858), which replaced
# the Python scripts/checks/check_pointer_boilerplate.py  PATHREF-OK: the
# predecessor this names was deleted in the same change. All this does is
# resolve zig, build the tool once, and hand over argv and the exit status
# unchanged.
#
#     bash scripts/builders/check_pointer_boilerplate.sh --selftest
#     bash scripts/builders/check_pointer_boilerplate.sh
#
# Invoked by the format gate in scripts/ci/gates/hygiene.sh.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TOOL_DIR="${ROOT}/tools/check_pointer_boilerplate"

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
  echo "check_pointer_boilerplate.sh: FATAL -- zig not found. Set ZIG, or put zig on PATH." >&2
  exit 2
fi

"${ZIG}" build \
  --build-file "${TOOL_DIR}/build.zig" \
  --cache-dir "${TOOL_DIR}/build/cache" \
  --prefix "${TOOL_DIR}/build" \
  -Doptimize=ReleaseSafe >/dev/null

RA8_REPO_ROOT="${ROOT}" exec "${TOOL_DIR}/build/bin/check_pointer_boilerplate" "$@"
