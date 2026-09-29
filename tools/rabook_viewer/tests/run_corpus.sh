#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# run_corpus.sh -- drive the ra8_viewer malformed-input security corpus (#298).
#
# Builds the corpus with gen_corpus.py, then runs the viewer headless over every
# fixture and asserts the outcome, so a regression in workspace bounds or
# archive decoding is caught without a human opening a window:
#
#   * malicious fixtures MUST exit with a clean ra8_err_t (exit 1) -- never 0
#     (an unsafe input slipped through), never a crash (killed by a
#     signal, exit >= 128), never a hang (timeout, exit 124);
#   * legitimate fixtures MUST exit 0 and write a P6 PPM (a bound that also
#     refuses a valid file is not a fix);
#   * recognised-but-unwired fixtures MUST exit 1 AND say why on stderr (#849).
#     A wrapped comic, an EPUB, a RABOOK and an unknown extension each have
#     their own honest reason, and this tier fails if the viewer ever accepts
#     one of them or refuses it with the wrong reason -- "not wired yet" must
#     never drift into a silent claim of support.
#
# Usage: run_corpus.sh <viewer-binary> [work-dir]

set -euo pipefail

VIEWER="${1:?usage: run_corpus.sh <viewer-binary> [work-dir]}"
WORK="${2:-$(mktemp -d)}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CORPUS="$WORK/corpus"
PPM="$WORK/out.ppm"
COMIC="$HERE/../fixtures/sample.cbz"
COMIC_PPM_SHA256="003c60d6ff5b4f1ea4671193c278817878223605aabcafc2dab7e47927a4665d"

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
python3 "$HERE/gen_corpus.py" "$CORPUS" "$COMIC"
cp "$COMIC" "$CORPUS/legit.cbz"

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

# Same run, but the diagnostic text is kept so the unwired tier can assert the
# viewer said WHY it refused, not merely that it refused.
ERRLOG="$WORK/stderr.txt"
run_one_logged() {
  rm -f "$PPM" "$ERRLOG"
  set +e
  if [[ "${#TIMEOUT[@]}" -gt 0 ]]; then
    "${TIMEOUT[@]}" "$VIEWER" "$CORPUS/$1" --headless --dump-ppm "$PPM" \
      >/dev/null 2>"$ERRLOG"
  else
    "$VIEWER" "$CORPUS/$1" --headless --dump-ppm "$PPM" >/dev/null 2>"$ERRLOG"
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
    echo "FAIL: $f was ACCEPTED (exit 0) -- an unsafe input slipped through" >&2
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
legit=(legit.jof legit_deflate.jof legit.cbz legit.cbt)
for f in "${legit[@]}"; do
  run_one "$f"
  if [[ "$rc" -ne 0 ]]; then
    echo "FAIL: legitimate $f was rejected (exit $rc)" >&2
    fail=1
  elif [[ "$(head -c 2 "$PPM" 2>/dev/null)" != "P6" ]]; then
    echo "FAIL: legitimate $f produced no P6 image" >&2
    fail=1
  elif [[ "$f" == "legit.cbz" || "$f" == "legit.cbt" ]] &&
    [[ "$(python3 -c 'import hashlib,sys; print(hashlib.sha256(open(sys.argv[1], "rb").read()).hexdigest())' "$PPM")" != "$COMIC_PPM_SHA256" ]]; then
    echo "FAIL: legitimate $f pixels differ from the committed RGB golden" >&2
    fail=1
  else
    echo "PASS decoded: $f -> $(wc -c <"$PPM") bytes of P6"
  fi
done

# --- recognised but unwired: refused, and honest about why (#849) -----------
# Each entry is "fixture|expected stderr fragment".
unwired=(
  "legit.cbt.gz|wrapped comics require"
  "sample.epub|reflow reader engine"
  "sample.rabook|reflow reader engine"
  "notes.pdf|unsupported file type"
)
for entry in "${unwired[@]}"; do
  f="${entry%%|*}"
  want="${entry#*|}"
  run_one_logged "$f"
  if [[ "$rc" -eq 0 ]]; then
    echo "FAIL: $f was ACCEPTED (exit 0) -- an unwired format claimed support" >&2
    fail=1
  elif [[ "$rc" -eq 124 ]]; then
    echo "FAIL: $f HUNG (timeout)" >&2
    fail=1
  elif [[ "$rc" -ge 128 ]]; then
    echo "FAIL: $f CRASHED (killed by signal $((rc - 128)))" >&2
    fail=1
  elif [[ "$rc" -ne 1 ]]; then
    echo "FAIL: $f exited $rc (expected a clean ra8_err_t, exit 1)" >&2
    fail=1
  elif ! grep -qF "$want" "$ERRLOG"; then
    echo "FAIL: $f was refused without the honest reason ('$want')" >&2
    fail=1
  else
    echo "PASS unwired: $f (clean exit 1, reason: $want)"
  fi
done

if [[ "$fail" -ne 0 ]]; then
  echo "run_corpus: FAILURES above" >&2
  exit 1
fi
echo "run_corpus: all ${#malicious[@]} malicious refused cleanly," \
  "${#legit[@]} legitimate decoded, ${#unwired[@]} unwired refused with a reason"
