#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# scripts/builders/gen_jlink_w4.sh -- emit a J-Link w4 programming script.
#
# This is a minimal trusted launcher, not an implementation: the generator is
# the Zig host tool tools/gen_jlink_w4 (#858), which replaced the Python
# scripts/gen/gen_jlink_w4.py  PATHREF-OK: the predecessor this names was
# deleted in the same change.
#
# All this does is resolve zig, build the tool when it is missing or older than
# its sources, and hand over argv and the exit status unchanged. It never runs
# J-Link: the script goes to stdout for the caller to pipe or save.
#
#     bash scripts/builders/gen_jlink_w4.sh firmware.bin 0x02000000
#     bash scripts/builders/gen_jlink_w4.sh firmware.bin 0x02000000 --device DEV
#
# Exit status is the tool's: 0 the script was emitted in full, 1 a usage error,
# an unrecognised argument, a base address int(text, 16) would have rejected,
# an unreadable image, or an image with no readable vector table. A 2 can only
# come from this launcher, for an absent zig.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TOOL_DIR="${ROOT}/tools/gen_jlink_w4"
BINARY="${TOOL_DIR}/build/bin/gen_jlink_w4"

# A missing zig is FATAL rather than a silent fallback: a generator that cannot
# run must never look like one that emitted an empty script, which J-Link would
# accept as a no-op flash.
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
    echo "gen_jlink_w4.sh: FATAL -- zig not found. Set ZIG, or put zig on PATH." >&2
    exit 2
  fi
  "${ZIG}" build \
    --build-file "${TOOL_DIR}/build.zig" \
    --cache-dir "${TOOL_DIR}/build/cache" \
    --prefix "${TOOL_DIR}/build" \
    -Doptimize=ReleaseSafe >/dev/null
fi

# Relative image paths resolve against the caller's working directory, as the
# predecessor's Path(bin_file) did, so this never changes directory.
exec "${BINARY}" "$@"
