#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# scripts/builders/list_tests.sh -- print the test targets belonging to one
# repository domain.
#
# This is a minimal trusted launcher, not an implementation: the listing is
# produced by the Zig host tool tools/list_tests (#858), which replaced the
# Python scripts/dev/list_tests.py, now deleted. PATHREF-OK: history, not a live path.
# All this does is resolve zig, build the tool once, and hand over argv and the
# exit status unchanged.
#
#     bash scripts/builders/list_tests.sh hal
#
# Invoked by every category recipe in just/tests.just.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TOOL_DIR="${ROOT}/tools/list_tests"

# A missing zig is FATAL rather than a skipped listing. `just tests::hal`
# printing nothing and exiting 0 would read as "this category has no tests",
# which is the same failure the tool's own FILE-count banner exists to make
# visible: a report that looked at nothing must never look clean.
ZIG="${ZIG:-}"
if [ -z "${ZIG}" ]; then
  if command -v zig >/dev/null 2>&1; then
    ZIG="$(command -v zig)"
  elif [ -x "${HOME}/.local/zig/zig" ]; then
    ZIG="${HOME}/.local/zig/zig"
  fi
fi
if [ -z "${ZIG}" ]; then
  echo "list_tests.sh: FATAL -- zig not found. Set ZIG, or put zig on PATH." >&2
  exit 2
fi

"${ZIG}" build \
  --build-file "${TOOL_DIR}/build.zig" \
  --cache-dir "${TOOL_DIR}/build/cache" \
  --prefix "${TOOL_DIR}/build" \
  -Doptimize=ReleaseSafe >/dev/null

RA8_REPO_ROOT="${ROOT}" exec "${TOOL_DIR}/build/bin/list_tests" "$@"
