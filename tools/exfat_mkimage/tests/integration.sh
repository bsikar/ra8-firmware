#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie

set -euo pipefail

if (($# != 2)); then
  echo "usage: integration.sh EXFAT_MKIMAGE CMAKE" >&2
  exit 2
fi

tool=$1
cmake_cmd=$2
work=$(mktemp -d "${TMPDIR:-/tmp}/exfat-mkimage-test.XXXXXX")

cleanup() {
  rm -rf -- "$work"
}
trap cleanup EXIT

fail() {
  echo "exfat_mkimage integration: $*" >&2
  exit 1
}

assert_no_temp() {
  if find "$work" -name '.exfat_mkimage.tmp.*' -print -quit | grep -q .; then
    fail "temporary image leaked"
  fi
}

image="$work/showcase.img"
"$tool" "$image" >/dev/null 2>&1
read -r actual_hash _ < <("$cmake_cmd" -E sha256sum "$image")
expected_hash=d3068af77071871baeaf77748519aaba2d12cdd236b9cd32fd22338aa477c771
[[ $actual_hash == "$expected_hash" ]] || fail "image differs from legacy golden"
[[ $(wc -c <"$image") -eq 67108864 ]] || fail "image is not exactly 64 MiB"

if command -v fsck.exfat >/dev/null 2>&1; then
  fsck.exfat -n "$image" >/dev/null 2>&1 || fail "fsck.exfat rejected generated image"
fi

sentinel="$work/sentinel"
printf 'existing-output-must-survive\n' >"$sentinel"
mkdir "$work/real-parent"
cp "$sentinel" "$work/real-parent/preserved.img"
ln -s real-parent "$work/symlink-parent"
if "$tool" "$work/symlink-parent/preserved.img" >/dev/null 2>&1; then
  fail "symlink parent unexpectedly accepted"
fi
cmp -- "$work/real-parent/preserved.img" "$sentinel" || fail "symlink-parent failure changed output"
assert_no_temp

mkdir "$work/final-is-directory"
if "$tool" "$work/final-is-directory" >/dev/null 2>&1; then
  fail "rename over directory unexpectedly succeeded"
fi
[[ -d $work/final-is-directory ]] || fail "rename fault damaged destination directory"
assert_no_temp

printf 'symlink-target-must-not-change\n' >"$work/symlink-target"
ln -s symlink-target "$work/final-link.img"
"$tool" "$work/final-link.img" >/dev/null 2>&1
[[ -f $work/final-link.img && ! -L $work/final-link.img ]] || fail "final symlink was followed"
printf 'symlink-target-must-not-change\n' | cmp -- "$work/symlink-target" - ||
  fail "final symlink target was modified"
read -r link_hash _ < <("$cmake_cmd" -E sha256sum "$work/final-link.img")
[[ $link_hash == "$expected_hash" ]] || fail "replacement image differs from golden"
assert_no_temp

if "$tool" "$work/unwritten.img" extra >/dev/null 2>&1; then
  fail "extra command-line argument unexpectedly succeeded"
fi
[[ ! -e $work/unwritten.img ]] || fail "usage error touched destination"

echo "exfat_mkimage integration: golden/fsck/fault/publication tests passed"
