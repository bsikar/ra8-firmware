#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie

set -euo pipefail

binary=${1:?usage: integration.sh BINARY}
scratch=$(mktemp -d "${TMPDIR:-/tmp}/ra8-glyph-bench.XXXXXX")
trap 'rm -rf "$scratch"' EXIT HUP INT TERM

"$binary" >"$scratch/out" 2>"$scratch/err"
test ! -s "$scratch/err"
test "$(wc -c <"$scratch/out" | tr -d ' ')" = 842

if command -v sha256sum >/dev/null 2>&1; then
  digest=$(sha256sum "$scratch/out" | awk '{print $1}')
else
  digest=$(shasum -a 256 "$scratch/out" | awk '{print $1}')
fi
test "$digest" = cc36cad1bf0a5d628a040d4856742eac4d17607e39c5c3522639010ac5c6baa9

set +e
"$binary" 2>"$scratch/pipe.err" | head -c 1 >/dev/null
pipe_status=$?
set -e
test "$pipe_status" -ne 0
grep -q "workload or output failed" "$scratch/pipe.err"

printf '%s\n' "glyph_bench integration: PASS"
