#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie

set -eu

tool=$1
fixture_builder=$2
temp_dir=$(mktemp -d)
trap 'rm -rf "$temp_dir"' EXIT HUP INT TERM
book=$temp_dir/fixture.rabook

"$fixture_builder" --emit "$book"
"$tool" inspect "$book" >"$temp_dir/auto.txt"
"$tool" inspect --format rabook --in "$book" >"$temp_dir/explicit.txt"
cmp "$temp_dir/auto.txt" "$temp_dir/explicit.txt"
grep -Fx 'verdict: VALID (chunk table monotonic and complete)' "$temp_dir/auto.txt" >/dev/null
grep -Fx '  chunk_bytes    : 128' "$temp_dir/auto.txt" >/dev/null

"$tool" inspect "$book" --verbose >"$temp_dir/verbose.txt"
grep -Fx '  entry     start        end       length' "$temp_dir/verbose.txt" >/dev/null
