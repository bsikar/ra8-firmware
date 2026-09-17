#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# scripts/builders/check_since_version.sh -- enforce Doxygen `@since` tags on
# the public library contract, and check every `@since` value in the tree
# against the single top-level VERSION string.
#
# This is a minimal trusted launcher, not an implementation: both halves of
# the gate are the Zig host tool tools/check_since_version (#858), which
# replaced the Python scripts/checks/check-since-version.py. All this does is
# resolve zig, build the tool once, and hand over argv and the exit status
# unchanged.
#
#     bash scripts/builders/check_since_version.sh --selftest
#     bash scripts/builders/check_since_version.sh --all
#     bash scripts/builders/check_since_version.sh path/to/file.h ...
#
# Invoked by gate_since in scripts/ci/gates/hygiene.sh and by the `version`
# recipe in just/ci_local.just.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TOOL_DIR="${ROOT}/tools/check_since_version"

# A missing zig is FATAL rather than a skipped gate. Exiting 0 here would
# report a clean tree because nothing looked at it, which is the same failure
# the tool's own tracked-path floor exists to make visible.
ZIG="${ZIG:-}"
if [ -z "${ZIG}" ]; then
  if command -v zig >/dev/null 2>&1; then
    ZIG="$(command -v zig)"
  elif [ -x "${HOME}/.local/zig/zig" ]; then
    ZIG="${HOME}/.local/zig/zig"
  fi
fi
if [ -z "${ZIG}" ]; then
  echo "check_since_version.sh: FATAL -- zig not found. Set ZIG, or put zig on PATH." >&2
  exit 2
fi

"${ZIG}" build \
  --build-file "${TOOL_DIR}/build.zig" \
  --cache-dir "${TOOL_DIR}/build/cache" \
  --prefix "${TOOL_DIR}/build" \
  -Doptimize=ReleaseSafe >/dev/null

RA8_REPO_ROOT="${ROOT}" exec "${TOOL_DIR}/build/bin/check_since_version" "$@"
