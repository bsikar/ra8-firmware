#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# run_corpus.sh -- drive the ra8_viewer malformed-input security corpus (#298).
#
# Builds the corpus with gen_corpus.py, then runs the viewer headless over every
# fixture and asserts the outcome, so a regression in an untrusted-allocation
# guard is caught without a human opening a window:
#
#   * malicious fixtures MUST exit with a clean ra8_err_t (exit 1) -- never 0
#     (an unbounded allocation slipped through), never a crash (killed by a
#     signal, exit >= 128), never a hang (timeout, exit 124);
#   * legitimate fixtures MUST exit 0 and write a P6 PPM (a bound that also
#     refuses a valid file is not a fix).
#
# Usage: run_corpus.sh <viewer-binary> [work-dir]

set -euo pipefail

VIEWER="${1:?usage: run_corpus.sh <viewer-binary> [work-dir]}"
WORK="${2:-$(mktemp -d)}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CORPUS="$WORK/corpus"
PPM="$WORK/out.ppm"

command -v python3 >/dev/null || {
  echo "run_corpus: python3 is required to build the corpus" >&2
  exit 1
}

# A hang is a failure mode this gate must catch, so cap every run when the
# coreutils timeout is available (it is on every Linux runner).
TIMEOUT=()
if command -v timeout >/dev/null; then
  TIMEOUT=(timeout 60)
fi

mkdir -p "$WORK"
python3 "$HERE/gen_corpus.py" "$CORPUS"

rc=0
fail=0

run_one() {
  rm -f "$PPM"
  set +e
  if [[ "${#TIMEOUT[@]}" -gt 0 ]]; then
    "${TIMEOUT[@]}" "$VIEWER" "$CORPUS/$1" --headless --dump-ppm "$PPM" >/dev/null 2>&1
  else
    "$VIEWER" "$CORPUS/$1" --headless --dump-ppm "$PPM" >/dev/null 2>&1
  fi
  rc=$?
  set -e
}

# --- malicious: expect a clean rejection (exit 1), no crash, no hang ---------
malicious=(
  giant_decl.cbz zip_bomb.cbz giant_decl.cbt
  truncated.cbz truncated.cbt garbage.cbr
  giant_tiles.jof truncated.jof unwrap_bomb.cbt.gz
)
for f in "${malicious[@]}"; do
  run_one "$f"
  if [[ "$rc" -eq 1 ]]; then
    echo "PASS refused: $f (clean exit 1)"
  elif [[ "$rc" -eq 0 ]]; then
    echo "FAIL: $f was ACCEPTED (exit 0) -- an unbounded allocation slipped through" >&2
    fail=1
  elif [[ "$rc" -eq 124 ]]; then
    echo "FAIL: $f HUNG (timeout)" >&2
    fail=1
  elif [[ "$rc" -ge 128 ]]; then
    echo "FAIL: $f CRASHED (killed by signal $((rc - 128)))" >&2
    fail=1
  else
    echo "FAIL: $f exited $rc (expected a clean ra8_err_t, exit 1)" >&2
    fail=1
  fi
done

# --- legitimate: a valid atlas must still decode ----------------------------
legit=(legit.jof)
for f in "${legit[@]}"; do
  run_one "$f"
  if [[ "$rc" -ne 0 ]]; then
    echo "FAIL: legitimate $f was rejected (exit $rc)" >&2
    fail=1
  elif [[ "$(head -c 2 "$PPM" 2>/dev/null)" != "P6" ]]; then
    echo "FAIL: legitimate $f produced no P6 image" >&2
    fail=1
  else
    echo "PASS decoded: $f -> $(wc -c <"$PPM") bytes of P6"
  fi
done

if [[ "$fail" -ne 0 ]]; then
  echo "run_corpus: FAILURES above" >&2
  exit 1
fi
echo "run_corpus: all ${#malicious[@]} malicious refused cleanly, ${#legit[@]} legitimate decoded"
