#!/usr/bin/env sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# Golden and failure-semantics coverage for the portable JOF verify CLI.

set -eu

if [ "$#" -ne 2 ]; then
  echo "usage: verify_integration.sh <rabook_imagepack> <repo-root>" >&2
  exit 2
fi

tool=$1
repo=$2
tmp=$(mktemp -d "${TMPDIR:-/tmp}/ra8-fmt-verify.XXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

digest() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1" | awk '{print $1}'
  else
    shasum -a 256 "$1" | awk '{print $1}'
  fi
}

verify_golden() {
  input=$1
  output=$2
  expected_digest=$3
  geometry=$4
  "$tool" verify --format jof --in "$input" --out "$output" >"$output.stdout" 2>"$output.stderr"
  test "$(digest "$output")" = "$expected_digest"
  test ! -s "$output.stderr"
  printf 'verify: %s | reference 1 tile | banded 1 tiles of %s rows\n' "$geometry" "$5" >"$output.expected"
  printf '  wrote reassembled raster to %s (ok)\n' "$output" >>"$output.expected"
  printf 'verdict: ROUND-TRIP EXACT -- the produced file is correct (0 differing bytes)\n' >>"$output.expected"
  cmp "$output.expected" "$output.stdout"
}

png=$repo/tests/fixtures/rabook_fixed_layout/OEBPS/images/page1.png
lossless=$repo/tests/fixtures/webp/fixture_lossless.webp
lossy=$repo/tests/fixtures/webp/fixture_lossy.webp

verify_golden "$png" "$tmp/page1.ppm" \
  389eaafa9aca31e8a507ada88ca5b34c1207d66938d182331ddb516bb18af561 "24x32 bpp=1" 32
verify_golden "$lossless" "$tmp/lossless.ppm" \
  c99e353823ef3386d198ff92081f0b47b611cb18363542714ff1abf109b3ffeb "8x8 bpp=4" 8
verify_golden "$lossy" "$tmp/lossy.ppm" \
  23395ffde945313d5059d712b4dd130ec1eb954b23b6b1cceb5e52fe3ed51e1e "8x8 bpp=4" 8

ln -s "$png" "$tmp/input-link.png"
if "$tool" verify --format jof --in "$tmp/input-link.png" >"$tmp/input-link.stdout" \
  2>"$tmp/input-link.stderr"; then
  exit 1
fi
test ! -s "$tmp/input-link.stdout"
grep '^ra8_fmt: cannot open verify input (rc=' "$tmp/input-link.stderr" >/dev/null

mkdir "$tmp/input-directory"
if "$tool" verify --format jof --in "$tmp/input-directory" >"$tmp/directory.stdout" \
  2>"$tmp/directory.stderr"; then
  exit 1
fi
test ! -s "$tmp/directory.stdout"
grep '^ra8_fmt: cannot open verify input (rc=' "$tmp/directory.stderr" >/dev/null

printf 'preserve-output' >"$tmp/output-target"
ln -s output-target "$tmp/output-link.ppm"
"$tool" verify --format jof --in "$png" --out "$tmp/output-link.ppm" \
  >"$tmp/output-link.stdout" 2>"$tmp/output-link.stderr"
printf 'preserve-output' >"$tmp/output-target.expected"
cmp "$tmp/output-target.expected" "$tmp/output-target"
test ! -s "$tmp/output-link.stderr"
grep 'wrote reassembled raster to .*output-link.ppm (FAILED)$' "$tmp/output-link.stdout" >/dev/null
grep '^verdict: ROUND-TRIP EXACT -- the produced file is correct (0 differing bytes)$' \
  "$tmp/output-link.stdout" >/dev/null

dd if="$png" of="$tmp/truncated.png" bs=1 count=30 2>/dev/null
printf 'preserve-truncated' >"$tmp/truncated.ppm"
if "$tool" verify --format jof --in "$tmp/truncated.png" --out "$tmp/truncated.ppm" \
  >"$tmp/truncated.stdout" 2>"$tmp/truncated.stderr"; then
  exit 1
fi
printf 'preserve-truncated' >"$tmp/truncated.expected"
cmp "$tmp/truncated.expected" "$tmp/truncated.ppm"
grep '^verify: reference encode failed (rc=' "$tmp/truncated.stdout" >/dev/null

cp "$png" "$tmp/corrupt.png"
printf '\377' | dd of="$tmp/corrupt.png" bs=1 seek=45 count=1 conv=notrunc 2>/dev/null
printf 'preserve-corrupt' >"$tmp/corrupt.ppm"
if "$tool" verify --format jof --in "$tmp/corrupt.png" --out "$tmp/corrupt.ppm" \
  >"$tmp/corrupt.stdout" 2>"$tmp/corrupt.stderr"; then
  exit 1
fi
printf 'preserve-corrupt' >"$tmp/corrupt.expected"
cmp "$tmp/corrupt.expected" "$tmp/corrupt.ppm"
grep '^verify: reference encode failed (rc=' "$tmp/corrupt.stdout" >/dev/null

printf '\211PNG\r\n\032\n\000\000\000\015IHDR\000\000\040\000\000\000\040\000\010\006\000\000\000\000\000\000\000' \
  >"$tmp/oversize.png"
test "$(wc -c <"$tmp/oversize.png" | tr -d ' ')" -eq 33
printf 'preserve-oversize' >"$tmp/oversize.ppm"
if "$tool" verify --format jof --in "$tmp/oversize.png" --out "$tmp/oversize.ppm" \
  >"$tmp/oversize.stdout" 2>"$tmp/oversize.stderr"; then
  exit 1
fi
printf 'preserve-oversize' >"$tmp/oversize.expected"
cmp "$tmp/oversize.expected" "$tmp/oversize.ppm"
grep '^ra8_fmt: JOF verify workspace too small: required 27058688 supplied 10486016 ' \
  "$tmp/oversize.stderr" >/dev/null

test -z "$(find "$tmp" -maxdepth 1 \( -name '.ra8spool.*' -o -name '.*.ra8tmp.*' \) -print -quit)"
