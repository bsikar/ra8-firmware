#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie

set -euo pipefail

if (( $# != 3 )); then
  echo "usage: integration.sh READER_VMEM WORKSPACE_TEST MAX_BUDGET" >&2
  exit 2
fi

tool=$1
workspace_test=$2
max_budget=$3
work=$(mktemp -d "${TMPDIR:-/tmp}/reader-vmem-test.XXXXXX")

cleanup() {
  chmod 0755 "$work/fault-parent" 2>/dev/null || true
  rm -rf -- "$work"
}
trap cleanup EXIT

fail() {
  echo "reader_vmem integration: $*" >&2
  exit 1
}

assert_no_temp() {
  if find "$work" -name '.reader_vmem.tmp.*' -print -quit | grep -q .; then
    fail "temporary trace leaked"
  fi
}

expected_hash=afe1d21e574813489acf7c53d37a79416eb1541b71eb2e14553f41090f18e066
"$workspace_test" || fail "two-instance workspace contract failed"

default_trace="$work/default.trace"
"$tool" "$default_trace" >"$work/default.out" 2>"$work/default.err"
read -r default_hash _ < <(sha256sum "$default_trace")
[[ $default_hash == "$expected_hash" ]] || fail "default trace differs from legacy golden"
[[ $(wc -l <"$default_trace") -eq 88480 ]] || fail "default reference count changed"
[[ $(wc -c <"$default_trace") -eq 521928 ]] || fail "default trace size changed"
grep -Fq 'reader_vmem: book=2050 frames (8396800 bytes), budget=256 frames' \
  "$work/default.err" || fail "default geometry report changed"
grep -Fq 'ra8_vmem SLRU: hits=30553 misses=57927 evictions=57671  hit_rate=34.53%' \
  "$work/default.err" || fail "default firmware-cache statistics changed"

budget_one="$work/budget-one.trace"
"$tool" "$budget_one" 1 >"$work/budget-one.out" 2>"$work/budget-one.err"
cmp -- "$default_trace" "$budget_one" || fail "cache budget changed reference semantics"
grep -Fq 'ra8_vmem SLRU: hits=0 misses=88480 evictions=88479  hit_rate=0.00%' \
  "$work/budget-one.err" || fail "one-frame firmware-cache statistics changed"

max_trace="$work/max-budget.trace"
"$tool" "$max_trace" "$max_budget" >"$work/max-budget.out" 2>"$work/max-budget.err"
cmp -- "$default_trace" "$max_trace" || fail "compiled maximum changed reference semantics"
grep -Fq "budget=$max_budget frames" "$work/max-budget.err" ||
  fail "compiled maximum was not exercised"

sentinel="$work/sentinel"
printf 'existing-trace-must-survive\n' >"$sentinel"
preserved="$work/preserved.trace"
cp "$sentinel" "$preserved"
if "$tool" "$preserved" 0 >/dev/null 2>&1; then
  fail "zero budget unexpectedly succeeded"
fi
cmp -- "$preserved" "$sentinel" || fail "argument failure changed existing trace"

over_budget=$((max_budget + 1))
if "$tool" "$preserved" "$over_budget" >"$work/over.out" 2>"$work/over.err"; then
  fail "over-cap budget unexpectedly succeeded"
fi
cmp -- "$preserved" "$sentinel" || fail "workspace failure changed existing trace"
grep -Fq "budget $over_budget requires " "$work/over.err" ||
  fail "workspace failure omitted exact required bytes"
grep -Fq "compiled max budget $max_budget provides " "$work/over.err" ||
  fail "workspace failure omitted compiled capacity"
assert_no_temp

mkdir "$work/real-parent"
cp "$sentinel" "$work/real-parent/preserved.trace"
ln -s real-parent "$work/symlink-parent"
if "$tool" "$work/symlink-parent/preserved.trace" >/dev/null 2>&1; then
  fail "symlink parent unexpectedly accepted"
fi
cmp -- "$work/real-parent/preserved.trace" "$sentinel" ||
  fail "symlink-parent failure changed output"
assert_no_temp

mkdir "$work/final-is-directory"
if "$tool" "$work/final-is-directory" >/dev/null 2>&1; then
  fail "rename over directory unexpectedly succeeded"
fi
[[ -d $work/final-is-directory ]] || fail "rename fault damaged destination"
assert_no_temp

printf 'symlink-target-must-not-change\n' >"$work/symlink-target"
ln -s symlink-target "$work/final-link.trace"
"$tool" "$work/final-link.trace" >/dev/null 2>&1
[[ -f $work/final-link.trace && ! -L $work/final-link.trace ]] ||
  fail "final symlink was followed"
printf 'symlink-target-must-not-change\n' | cmp -- "$work/symlink-target" - ||
  fail "final symlink target changed"
read -r link_hash _ < <(sha256sum "$work/final-link.trace")
[[ $link_hash == "$expected_hash" ]] || fail "replacement trace differs from golden"

mkdir "$work/fault-parent"
cp "$sentinel" "$work/fault-parent/preserved.trace"
chmod 0555 "$work/fault-parent"
if "$tool" "$work/fault-parent/preserved.trace" >/dev/null 2>&1; then
  fail "unwritable parent unexpectedly accepted"
fi
cmp -- "$work/fault-parent/preserved.trace" "$sentinel" ||
  fail "open fault changed existing trace"
chmod 0755 "$work/fault-parent"
assert_no_temp

echo "reader_vmem integration: golden/workspace/fault/publication tests passed"
