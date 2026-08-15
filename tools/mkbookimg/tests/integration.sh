#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie

set -euo pipefail

if (( $# != 2 )); then
  echo "usage: integration.sh MKBOOKIMG CMAKE" >&2
  exit 2
fi

tool=$1
cmake_cmd=$2
work=$(mktemp -d "${TMPDIR:-/tmp}/mkbookimg-test.XXXXXX")

cleanup() {
  rm -rf -- "$work"
}
trap cleanup EXIT

fail() {
  echo "mkbookimg integration: $*" >&2
  exit 1
}

assert_preserved() {
  local output=$1
  local expected=$2
  cmp -- "$output" "$expected" || fail "existing output changed after failure"
  if find "$work" -name '.mkbookimg.tmp.*' -print -quit | grep -q .; then
    fail "temporary image leaked after failure"
  fi
}

mkdir "$work/input"
printf 'RABOOK-GOLDEN-A\nline-two\n' >"$work/input/alpha.rabook"
dd if=/dev/zero bs=1 count=9000 2>/dev/null | tr '\000' 'Z' >"$work/input/Second Book.rabook"

golden="$work/golden.img"
"$tool" "$golden" "$work/input/alpha.rabook" "$work/input/Second Book.rabook" >/dev/null 2>&1
read -r actual_hash _ < <("$cmake_cmd" -E sha256sum "$golden")
expected_hash=0a028a558cac7ced26b928f1f9c9d8d8cd36dc364f970a387eb5c429e48e4f73
[[ $actual_hash == "$expected_hash" ]] || fail "image differs from legacy golden: $actual_hash"
[[ $(wc -c <"$golden") -eq 67108864 ]] || fail "image length is not exactly 64 MiB"

sentinel="$work/sentinel"
printf 'existing-output-must-survive\n' >"$sentinel"
preserved="$work/preserved.img"
cp "$sentinel" "$preserved"
if "$tool" "$preserved" "$work/input/missing.rabook" >/dev/null 2>&1; then
  fail "missing input unexpectedly succeeded"
fi
assert_preserved "$preserved" "$sentinel"

: >"$work/input/empty.rabook"
if "$tool" "$preserved" "$work/input/empty.rabook" >/dev/null 2>&1; then
  fail "empty input unexpectedly succeeded"
fi
assert_preserved "$preserved" "$sentinel"

ln -s alpha.rabook "$work/input/symlink.rabook"
if "$tool" "$preserved" "$work/input/symlink.rabook" >/dev/null 2>&1; then
  fail "symlink input unexpectedly succeeded"
fi
assert_preserved "$preserved" "$sentinel"

mkdir "$work/final-is-directory"
if "$tool" "$work/final-is-directory" "$work/input/alpha.rabook" >/dev/null 2>&1; then
  fail "rename over a directory unexpectedly succeeded"
fi
[[ -d $work/final-is-directory ]] || fail "failed publication damaged destination directory"
if find "$work" -name '.mkbookimg.tmp.*' -print -quit | grep -q .; then
  fail "temporary image leaked after rename failure"
fi

mutable="$work/input/mutable.rabook"
dd if=/dev/zero of="$mutable" bs=4096 count=1024 2>/dev/null
cp "$sentinel" "$preserved"
(
  while :; do
    printf X >>"$mutable"
    truncate -s 4194304 "$mutable"
  done
) &
mutator_pid=$!
mutation_status=0
"$tool" "$preserved" "$mutable" >/dev/null 2>&1 || mutation_status=$?
kill "$mutator_pid" 2>/dev/null || true
wait "$mutator_pid" 2>/dev/null || true
[[ $mutation_status -ne 0 ]] || fail "mutating input unexpectedly published"
assert_preserved "$preserved" "$sentinel"

echo "mkbookimg integration: golden/corruption/fault/publication tests passed"
