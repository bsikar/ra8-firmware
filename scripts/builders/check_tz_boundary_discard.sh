#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# scripts/builders/check_tz_boundary_discard.sh -- minimal trusted launcher for
# the Zig TrustZone boot-boundary discard gate (#858, #1250).
#
# The gate itself is tools/check_tz_boundary_discard, a Zig program. This
# wrapper exists only to cross the OS boundary: resolve a zig, build the tool
# once in ReleaseSafe, then exec it with the caller's argv untouched so the
# exit status the gate chose is the exit status the caller sees (0 clean,
# 1 a discard or a failing selftest case, 2 usage or a collapsed sweep).
#
# An absent zig is FATAL exit 2, never a silent skip: a gate that cannot run
# has not passed.
#
# Usage:
#   scripts/builders/check_tz_boundary_discard.sh [--selftest] [file ...]

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
tool_dir="${repo_root}/tools/check_tz_boundary_discard"
binary="${tool_dir}/zig-out/bin/check_tz_boundary_discard"

zig_bin="${ZIG:-}"
if [[ -z "${zig_bin}" ]]; then
  if command -v zig >/dev/null 2>&1; then
    zig_bin="$(command -v zig)"
  elif [[ -x "${HOME}/.local/zig/zig" ]]; then
    zig_bin="${HOME}/.local/zig/zig"
  fi
fi

if [[ -z "${zig_bin}" ]]; then
  printf 'check_tz_boundary_discard.sh: FATAL -- no zig found (set $ZIG, put zig on PATH, or install ~/.local/zig/zig). The TrustZone boot-boundary gate cannot run, so it has not passed.\n' >&2
  exit 2
fi

if [[ ! -x "${binary}" ]]; then
  "${zig_bin}" build --build-file "${tool_dir}/build.zig" -Doptimize=ReleaseSafe >&2
fi

# The gate walks relative roots, so it must run from the repository root; that
# is also what its FILE_FLOOR trip-wire is there to catch.
cd "${repo_root}"
exec "${binary}" "$@"
