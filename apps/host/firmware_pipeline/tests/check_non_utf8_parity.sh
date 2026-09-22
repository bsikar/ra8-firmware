#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie

set -euo pipefail

if [[ $# -ne 2 ]]; then
  echo "usage: check_non_utf8_parity.sh <rust-executable> <zig-executable>" >&2
  exit 2
fi

fixture_dir="$(mktemp -d "${TMPDIR:-/tmp}/firmware-pipeline-non-utf8.XXXXXX")"
trap 'rm -rf -- "$fixture_dir"' EXIT
fixture_path="${fixture_dir}/firmware-"$'\xff'".bin"
printf 'hello' >"$fixture_path"

rust_output="$($1 "$fixture_path")"
zig_output="$($2 "$fixture_path")"

if [[ "$rust_output" != "$zig_output" ]]; then
  echo "Rust and Zig mains disagree for a non-UTF-8 input path" >&2
  exit 1
fi
