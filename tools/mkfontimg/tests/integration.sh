#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie

set -euo pipefail

if (($# != 2)); then
  echo "usage: integration.sh MKFONTIMG CMAKE" >&2
  exit 2
fi

tool=$1
cmake_cmd=$2
work=$(mktemp -d "${TMPDIR:-/tmp}/mkfontimg-test.XXXXXX")

cleanup() {
  rm -rf -- "$work"
}
trap cleanup EXIT

fail() {
  echo "mkfontimg integration: $*" >&2
  exit 1
}

assert_preserved() {
  local output=$1
  local expected=$2
  cmp -- "$output" "$expected" || fail "existing output changed after failure"
  if find "$work" -name '.mkfontimg.tmp.*' -print -quit | grep -q .; then
    fail "temporary image leaked after failure"
  fi
}

mkdir "$work/input"
dd if=/dev/zero bs=1 count=9000 2>/dev/null | tr '\000' 'F' >"$work/input/font.bin"

font_image="$work/font.img"
"$tool" "$work/input/font.bin" "$font_image" FONT.OTF >/dev/null 2>&1
read -r font_hash _ < <("$cmake_cmd" -E sha256sum "$font_image")
expected_font_hash=c293f3bff8508dcc81d74e1d6b427f372d89baac9fc4ea4aac8af75a2b876567
[[ $font_hash == "$expected_font_hash" ]] || fail "font image differs from legacy golden"
[[ $(wc -c <"$font_image") -eq 4194304 ]] || fail "font image is not exactly 4 MiB"

blank_image="$work/blank.img"
"$tool" --blank "$blank_image" >/dev/null 2>&1
read -r blank_hash _ < <("$cmake_cmd" -E sha256sum "$blank_image")
expected_blank_hash=1f5720358302c8a9d329b85f698fa1e86aae6d29042e294f48ebfd5b253107c1
[[ $blank_hash == "$expected_blank_hash" ]] || fail "blank image differs from legacy golden"

sentinel="$work/sentinel"
printf 'existing-output-must-survive\n' >"$sentinel"
preserved="$work/preserved.img"
cp "$sentinel" "$preserved"
if "$tool" "$work/input/missing.bin" "$preserved" >/dev/null 2>&1; then
  fail "missing input unexpectedly succeeded"
fi
assert_preserved "$preserved" "$sentinel"

printf 'too-small' >"$work/input/small.bin"
if "$tool" "$work/input/small.bin" "$preserved" >/dev/null 2>&1; then
  fail "undersized font unexpectedly succeeded"
fi
assert_preserved "$preserved" "$sentinel"

dd if=/dev/zero of="$work/input/oversize.bin" bs=1 count=0 seek=4194305 2>/dev/null
if "$tool" "$work/input/oversize.bin" "$preserved" >/dev/null 2>&1; then
  fail "oversized font unexpectedly succeeded"
fi
assert_preserved "$preserved" "$sentinel"

ln -s font.bin "$work/input/symlink.bin"
if "$tool" "$work/input/symlink.bin" "$preserved" >/dev/null 2>&1; then
  fail "symlink font unexpectedly succeeded"
fi
assert_preserved "$preserved" "$sentinel"

if "$tool" "$work/input/font.bin" "$preserved" 'NOT/A/NAME' >/dev/null 2>&1; then
  fail "invalid card name unexpectedly succeeded"
fi
assert_preserved "$preserved" "$sentinel"

mkdir "$work/final-is-directory"
if "$tool" --blank "$work/final-is-directory" >/dev/null 2>&1; then
  fail "rename over a directory unexpectedly succeeded"
fi
[[ -d $work/final-is-directory ]] || fail "publication fault damaged destination directory"
if find "$work" -name '.mkfontimg.tmp.*' -print -quit | grep -q .; then
  fail "temporary image leaked after rename failure"
fi

mutable="$work/input/mutable.bin"
dd if=/dev/zero of="$mutable" bs=4096 count=256 2>/dev/null
cp "$sentinel" "$preserved"
(
  while :; do
    printf X >>"$mutable"
    truncate -s 1048576 "$mutable"
  done
) &
mutator_pid=$!
mutation_status=0
"$tool" "$mutable" "$preserved" FONT.OTF >/dev/null 2>&1 || mutation_status=$?
kill "$mutator_pid" 2>/dev/null || true
wait "$mutator_pid" 2>/dev/null || true
[[ $mutation_status -ne 0 ]] || fail "mutating font unexpectedly published"
assert_preserved "$preserved" "$sentinel"

echo "mkfontimg integration: golden/corruption/fault/publication tests passed"
