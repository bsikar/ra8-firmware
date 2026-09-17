#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# scripts/builders/roadmap_stats.sh -- refresh the generated Summary block in
# docs/ROADMAP.md from the checkboxes in the rest of that file, or with
# --check refuse a stale one.
#
# This is a minimal trusted launcher, not an implementation: the parser, the
# renderer and the marker substitution are the Zig host tool
# tools/roadmap_stats (#858), which replaced the Python
# scripts/report/roadmap_stats.py.  PATHREF-OK: deleted in the same change.
# All this does is resolve zig, build the tool once, and hand over argv and
# the exit status unchanged.
#
#     bash scripts/builders/roadmap_stats.sh --check
#     bash scripts/builders/roadmap_stats.sh
#
# Invoked by gate_roadmap_stats in scripts/ci/gates/build.sh and by the
# `record_stats` recipe in just/docs.just.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TOOL_DIR="${ROOT}/tools/roadmap_stats"

# A missing zig is FATAL rather than a skipped gate. Exiting 0 here would
# report a current summary because nothing recomputed it, which is the same
# vacuous green the gate-honesty epic (#190) exists to prevent.
ZIG="${ZIG:-}"
if [ -z "${ZIG}" ]; then
  if command -v zig >/dev/null 2>&1; then
    ZIG="$(command -v zig)"
  elif [ -x "${HOME}/.local/zig/zig" ]; then
    ZIG="${HOME}/.local/zig/zig"
  fi
fi
if [ -z "${ZIG}" ]; then
  echo "roadmap_stats.sh: FATAL -- zig not found. Set ZIG, or put zig on PATH." >&2
  exit 2
fi

"${ZIG}" build \
  --build-file "${TOOL_DIR}/build.zig" \
  --cache-dir "${TOOL_DIR}/build/cache" \
  --prefix "${TOOL_DIR}/build" \
  -Doptimize=ReleaseSafe >/dev/null

RA8_REPO_ROOT="${ROOT}" exec "${TOOL_DIR}/build/bin/roadmap_stats" "$@"
