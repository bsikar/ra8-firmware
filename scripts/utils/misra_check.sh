#!/bin/bash
# ra8-firmware -- MISRA-C 2012 audit (advisory)
#
# Runs cppcheck + the bundled misra.py addon over first-party source
# (libs/ + src/ + port/, excluding libs/third_party/) and emits:
#
#   build/misra/results.txt  -- one violation per line, parsed
#   build/misra/raw.txt      -- raw cppcheck stderr
#   build/misra/misra-raw.txt-- raw misra.py stdout
#   stdout                   -- per-rule tally
#
# This is the audit hook for IEC 61508 SIL 3 / DO-178C MISRA-C 2012
# compliance. cppcheck-MISRA covers roughly two thirds of the
# mandatory + required rules; see docs/MISRA.md for the gap plan.
#
# Out of scope: tests/, examples/, build/, mocks/.
#
# Strategy: cppcheck's `--addon=misra` dispatcher silently drops
# misra.py findings when no MISRA rule-texts file is supplied (the
# rule texts are copyrighted by MISRA Ltd and cannot be redistributed).
# To keep this audit working out of the box we instead generate
# `--dump` files with cppcheck and run misra.py directly on them --
# the rule IDs (e.g. [misra-c2012-15.5]) are emitted regardless.
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT

set -euo pipefail
set +H

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"

cd "$ROOT_DIR"

OUT_DIR="$ROOT_DIR/build/misra"
mkdir -p "$OUT_DIR"
RAW="$OUT_DIR/raw.txt"
MISRA_RAW="$OUT_DIR/misra-raw.txt"
RESULTS="$OUT_DIR/results.txt"

# Delete dump artefacts on EVERY exit path, not just success. A stale dump
# left behind by an aborted run (disk-full, timeout, ^C) would be picked up
# by the next run's find and could resurrect findings for source that has
# since changed -- silent corruption of the ratchet comparison.
cleanup_dumps() {
  find libs src port -name '*.dump' -not -path '*/third_party/*' -delete 2>/dev/null || true
  find libs src port -name '*.ctu-info' -not -path '*/third_party/*' -delete 2>/dev/null || true
}
trap cleanup_dumps EXIT

if ! command -v cppcheck >/dev/null 2>&1; then
  echo "[ERROR] cppcheck not in PATH (brew install cppcheck)" >&2
  exit 2
fi

ADDON_DIR=""
for candidate in /opt/homebrew/share/Cppcheck/addons /usr/share/cppcheck/addons /usr/local/share/Cppcheck/addons /usr/lib/x86_64-linux-gnu/cppcheck/addons; do
  if [[ -d "$candidate" && -f "$candidate/misra.py" ]]; then
    ADDON_DIR="$candidate"
    break
  fi
done
if [[ -z "$ADDON_DIR" ]]; then
  echo "[ERROR] cppcheck misra.py addon not found" >&2
  exit 2
fi
MISRA_PY="$ADDON_DIR/misra.py"

JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}"

INCLUDE_DIRS=(
  -Ilibs/ra8_core/inc
  -Ilibs/ra8_hal/inc
  -Ilibs/ra8_nsc/inc
  -Isrc/inc
  -Itools/board_sim/inc
)

echo "[INFO] cppcheck MISRA-C 2012 audit -- $(cppcheck --version)" >&2
echo "[INFO] jobs=$JOBS  output=$RESULTS" >&2

if command -v gtimeout >/dev/null 2>&1; then
  TIMEOUT_CMD=(gtimeout 600)
elif command -v timeout >/dev/null 2>&1; then
  TIMEOUT_CMD=(timeout 600)
else
  TIMEOUT_CMD=()
fi

# cppcheck 2.13 (Ubuntu 24.04 / Debian 12 -- the CI + dev-box version) is
# finicky about the --suppressions-list parser ("Failed to add suppression.
# No id."); convert each non-comment, non-blank line into an explicit
# --suppress= flag instead, exactly like the cppcheck CI job does. This
# syntax is accepted by every cppcheck version.
SUPPRESS_ARGS=()
while IFS= read -r line; do
  line="${line%$'\r'}"
  line="${line## }"
  line="${line%% }"
  [[ -z "$line" ]] && continue
  case "$line" in
    \#*) continue ;;
  esac
  SUPPRESS_ARGS+=("--suppress=$line")
done <"$ROOT_DIR/.cppcheck-suppressions"

# cppcheck 2.20 does not support --std=c23. The codebase uses C23
# typed enums (`enum : uint8_t`) and `[[nodiscard]]`-style attributes;
# both raise syntaxError on the affected line. cppcheck recovers and
# continues parsing the rest of the translation unit, so MISRA
# coverage on the remaining ~95% of code is still useful.
echo "[INFO] generating cppcheck dumps under libs/, src/, port/ ..." >&2
set +e
"${TIMEOUT_CMD[@]}" cppcheck \
  -j "$JOBS" \
  --dump \
  --enable=warning \
  --inline-suppr \
  "${SUPPRESS_ARGS[@]}" \
  --suppress=missingIncludeSystem \
  --suppress=unmatchedSuppression \
  --suppress=syntaxError \
  --suppress=internalError \
  --suppress=*:libs/third_party/* \
  -ilibs/third_party \
  -ilibs/ra8_epub/src/ra8_epub_xml_shim.cpp \
  --std=c11 \
  --platform=unix32 \
  --language=c \
  --quiet \
  --error-exitcode=0 \
  "${INCLUDE_DIRS[@]}" \
  libs src port \
  2>"$RAW"
RC=$?
set -e

if [[ "$RC" -eq 124 ]]; then
  echo "[ERROR] cppcheck exceeded 10-minute budget" >&2
  exit 1
fi

# Run misra.py on every dump file produced under libs/ src/ port/.
mapfile -t DUMPS < <(find libs src port -name '*.dump' -not -path '*/third_party/*' | sort)
echo "[INFO] running misra.py on ${#DUMPS[@]} dump file(s) ..." >&2
if [[ ${#DUMPS[@]} -eq 0 ]]; then
  echo "[ERROR] cppcheck produced zero dump files -- the audit did not run" >&2
  echo "        (see $RAW for the cppcheck invocation error)" >&2
  exit 1
fi

: >"$MISRA_RAW"
set +e
python3 "$MISRA_PY" --quiet "${DUMPS[@]}" >>"$MISRA_RAW" 2>&1
MISRA_PY_RC=$?
set -e
# misra.py exits non-zero when it finds violations (expected) but ALSO
# crashes outright on a corrupt/truncated dump (seen in practice when the
# box runs out of disk mid-dump: cppcheck leaves a partial XML file and
# misra.py dies with an ElementTree ParseError partway through the list).
# Swallowing that produced a silently truncated results.txt, which the
# ratchet would misread as a massive burn-down. A Python traceback is
# never a legitimate outcome -- fail loudly on it.
if grep -q "^Traceback (most recent call last):" "$MISRA_RAW"; then
  echo "[ERROR] misra.py crashed (exit $MISRA_PY_RC) -- results.txt would be truncated" >&2
  echo "        traceback follows (from $MISRA_RAW):" >&2
  grep -A 12 "^Traceback (most recent call last):" "$MISRA_RAW" | tail -13 >&2
  exit 1
fi

# misra.py emits one diagnostic per line in the format:
#   [path/file.c:LINE] (severity) <message> [misra-c2012-X.Y]
# Parse into TSV: rule \t severity \t file \t line \t message
grep -E '\[misra-c2012-[0-9]+\.[0-9]+\] *$' "$MISRA_RAW" | awk '
{
    n = match($0, /\[misra-[A-Za-z0-9.\-]+\] *$/);
    if (n == 0) next;
    rule = substr($0, RSTART+1, RLENGTH-2);
    sub(/ *$/, "", rule);
    head = substr($0, 1, RSTART-1);
    sub(/ *$/, "", head);
    if (match(head, /^\[[^]]+\]/) == 0) next;
    locator = substr(head, 2, RLENGTH-2);
    rest = substr(head, RLENGTH+1);
    sub(/^ */, "", rest);
    sev = "style";
    if (match(rest, /^\([a-z]+\)/)) {
        sev = substr(rest, 2, RLENGTH-2);
        rest = substr(rest, RLENGTH+1);
        sub(/^ */, "", rest);
    }
    n2 = split(locator, lp, ":");
    file = lp[1];
    line = (n2 >= 2) ? lp[2] : "";
    msg = rest;
    gsub(/\t/, " ", msg);
    printf("%s\t%s\t%s\t%s\t%s\n", rule, sev, file, line, msg);
}' | sort -u >"$RESULTS"

# Dump artefacts are removed by the EXIT trap (cleanup_dumps above).

TOTAL=$(wc -l <"$RESULTS" | tr -d ' ')

echo
echo "MISRA-C 2012 audit complete"
echo "  results:    $RESULTS"
echo "  cppcheck:   $RAW"
echo "  misra.py:   $MISRA_RAW"
echo "  total unique violations: $TOTAL"
echo
echo "Top 10 most-violated rules:"
awk -F'\t' '{print $1}' "$RESULTS" | sort | uniq -c | sort -rn | head -10

exit 0
