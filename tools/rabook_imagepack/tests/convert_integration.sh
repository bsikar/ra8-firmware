#!/bin/sh
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
# Golden and failure-semantics coverage for the portable JOF convert CLI.

set -eu

if [ "$#" -ne 2 ]; then
  echo "usage: convert_integration.sh <rabook_imagepack> <repo-root>" >&2
  exit 2
fi

tool=$1
repo=$2
tmp=$(mktemp -d "${TMPDIR:-/tmp}/ra8-fmt-convert.XXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

digest()
{
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1" | awk '{print $1}'
  else
    shasum -a 256 "$1" | awk '{print $1}'
  fi
}

png=$repo/tests/fixtures/rabook_fixed_layout/OEBPS/images/page1.png
webp=$repo/tests/fixtures/webp/fixture_lossless.webp

png_out=$tmp/page1.jof
"$tool" convert --format jof --in "$png" --out "$png_out" >"$tmp/png.stdout" 2>"$tmp/png.stderr"
test "$(digest "$png_out")" = 16ea2ba980f69670fc3400c1feedeb5431882166c008c029579172e77fae8a8e
test ! -s "$tmp/png.stderr"
printf 'convert: 24x32 bpp=1 band=32 tiles=1 -> %s (428 bytes)\n' "$png_out" >"$tmp/png.expected"
cmp "$tmp/png.expected" "$tmp/png.stdout"

webp_out=$tmp/lossless.jof
"$tool" convert --format jof --in "$webp" --out "$webp_out" >"$tmp/webp.stdout" 2>"$tmp/webp.stderr"
test "$(digest "$webp_out")" = f585b2157b1156805805d5ee01d5124b4ec1cccbc72d931b1ebfaf6b9eba9f14
test ! -s "$tmp/webp.stderr"
printf 'convert: 8x8 bpp=4 band=8 tiles=1 -> %s (202 bytes)\n' "$webp_out" >"$tmp/webp.expected"
cmp "$tmp/webp.expected" "$tmp/webp.stdout"

printf 'preserve-target' >"$tmp/target.jof"
ln -s target.jof "$tmp/link.jof"
if "$tool" convert --format jof --in "$png" --out "$tmp/link.jof" >"$tmp/link.stdout" 2>"$tmp/link.stderr"; then
  exit 1
fi
test -L "$tmp/link.jof"
printf 'preserve-target' >"$tmp/target.expected"
cmp "$tmp/target.expected" "$tmp/target.jof"

dd if="$png" of="$tmp/truncated.png" bs=1 count=30 2>/dev/null
printf 'preserve-truncated' >"$tmp/truncated.jof"
if "$tool" convert --format jof --in "$tmp/truncated.png" --out "$tmp/truncated.jof" >"$tmp/truncated.stdout" 2>"$tmp/truncated.stderr"; then
  exit 1
fi
printf 'preserve-truncated' >"$tmp/truncated.expected"
cmp "$tmp/truncated.expected" "$tmp/truncated.jof"

printf '\211PNG\r\n\032\n\000\000\000\015IHDR\000\000\040\000\000\000\040\000' >"$tmp/oversize.png"
test "$(wc -c <"$tmp/oversize.png" | tr -d ' ')" -eq 24
printf 'preserve-oversize' >"$tmp/oversize.jof"
if "$tool" convert --format jof --in "$tmp/oversize.png" --out "$tmp/oversize.jof" >"$tmp/oversize.stdout" 2>"$tmp/oversize.stderr"; then
  exit 1
fi
printf 'preserve-oversize' >"$tmp/oversize.expected"
cmp "$tmp/oversize.expected" "$tmp/oversize.jof"
grep -E '^ra8_fmt: JOF convert workspace too small: required [0-9]+ supplied 8388608 \(work [0-9]+, webp 0\)$' "$tmp/oversize.stderr" >/dev/null

test -z "$(find "$tmp" -maxdepth 1 -name '.*.ra8tmp.*' -print -quit)"
