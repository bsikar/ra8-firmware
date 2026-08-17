#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# ra8-firmware -- MISRA-C 2012 audit (advisory)
#
# Runs cppcheck + the bundled misra.py addon over every first-party C source
# root (libs/ + src/ + port/ + tools/ + apps/, excluding vendored/generated
# code) and emits:
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
# Out of scope: tests/, examples/, mocks/, and any BUILD TREE beneath a root
# that produces one (see the build-output section below -- the exclusion is
# derived from lint_targets.is_build_output, never from a */build/* glob).
# Host tools are first-party production code and deliberately remain in scope.
#
# Usage: misra_check_inner.sh [--selftest]
#
# Strategy: cppcheck's `--addon=misra` dispatcher silently drops
# misra.py findings when no MISRA rule-texts file is supplied (the
# rule texts are copyrighted by MISRA Ltd and cannot be redistributed).
# To keep this audit working out of the box we instead generate
# `--dump` files with cppcheck and run misra.py directly on them --
# the rule IDs (e.g. [misra-c2012-15.5]) are emitted regardless.
#

set -euo pipefail
set +H

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"

# ra8_max_jobs -- the ONE canonical bounded-parallelism width (#328); cppcheck's
# -j below derives from it instead of raw nproc.
# shellcheck source=scripts/ci/lib/parallelism.sh
. "$SCRIPT_DIR/../ci/lib/parallelism.sh"

cd "$ROOT_DIR"

OUT_DIR="$ROOT_DIR/build/misra"
mkdir -p "$OUT_DIR"
RAW="$OUT_DIR/raw.txt"
MISRA_RAW="$OUT_DIR/misra-raw.txt"
RESULTS="$OUT_DIR/results.txt"

# The first-party source roots this audit covers. Everything downstream --
# the header search path, the cppcheck scan list, the dump discovery and the
# cleanup trap -- is derived from this ONE array, so a root cannot be added
# to some of those four and silently omitted from the others.
#
# It already was. When apps/ joined the scan list the header path beneath it
# was still spelled as a hand-written glob naming only tools/*/inc, so every
# header under apps/ became unreachable: cppcheck charged each caller MISRA
# 17.3 for the resulting implicit declarations and each definition 8.4 for
# the declaration it could no longer see, while SILENTLY LOSING the 9.2,
# 11.x, 18.4, 20.1 and 21.15 findings it can only raise once the types in
# those headers are known. A net drop in findings is the dangerous direction
# of that failure -- it reads as a burn-down.
RA8_MISRA_ROOTS=(libs port tools apps)

# ---------------------------------------------------------------------------
# BUILD OUTPUT IS NOT SOURCE -- and `build/` does not mean the same thing
# everywhere in this tree.
#
# The scan used to hand cppcheck the bare root directories with no build
# exclusion at all, so whatever an untracked build tree happened to leave under
# a scanned root was audited as first-party source. Regenerating the baseline on
# a dirty checkout therefore FROZE that junk into the ratchet: 12 rows of CMake
# compiler-probe source (tools/*/build/CMakeFiles/.../CMakeCCompilerId.c) had to
# be purged from .github/misra-baseline.txt by hand. Junk in the baseline is
# worse than junk in a report -- the ratchet then measures the tree against a
# population that never existed, and a real finding can hide behind a probe row.
#
# A blanket `*/build/*` exclusion is the WRONG fix, and lint_targets.py already
# says why. `build` names build OUTPUT only beneath the roots that produce it
# (tools/<t>/build, apps/<cat>/<p>/build, examples/**/<app>/build, tests/build,
# docs/build, port/**). Under libs/ it is an ordinary directory
# name a module is entitled to use for real source, and `builders/` is not a
# build tree at all despite the prefix. A glob cannot tell those apart, and a
# glob that swallows source is the dangerous direction: the audit goes quieter
# and the burn-down looks like progress.
#
# So the verdict is not re-derived here. This asks lint_targets.is_build_output
# -- the ONE definition of "this is build output" in the tree, shared with
# .gitignore and thirteen other checkers -- and turns its answer into cppcheck
# -i flags plus a filter for the header path and the dump discovery. A second
# description of the same fact is exactly how `build/` came to mean two things.
#
# The alternative considered and rejected: scanning only `git ls-files`. It
# would have fixed this case, since build trees are gitignored, but by a rule
# that is blanket in a different disguise -- it would drop every ignored path,
# not just build output -- and it costs real recall. A brand-new .c that has not
# been `git add`ed yet would silently leave the audit, and a baseline
# regenerated in that state would be missing a whole file. It would also mean
# re-implementing cppcheck's own source-file selection in bash, so the scanned
# POPULATION could drift from what cppcheck picks up walking a directory.
# Excluding the OUTPUT keeps the population exactly as it was and changes only
# what was never source.
# ---------------------------------------------------------------------------

# Print every build-output directory beneath the roots named as arguments.
# Build trees are pruned rather than descended, so a nested one is reported once
# by its outermost directory.
ra8_misra_build_output_dirs() {
  RA8_MISRA_CHECKS_DIR="$SCRIPT_DIR" python3 - "$@" <<'PYEOF'
import os
import sys

sys.path.insert(0, os.environ["RA8_MISRA_CHECKS_DIR"])

from lint_targets import is_build_output

for root in sys.argv[1:]:
    for dirpath, dirnames, _filenames in os.walk(root):
        descend = []
        for name in sorted(dirnames):
            candidate = os.path.join(dirpath, name)
            # is_build_output classifies a FILE path -- its last component is
            # the file name and is never treated as a directory. Probe with a
            # dummy leaf so the directory itself is what gets judged.
            if is_build_output(candidate + "/probe"):
                print(candidate)
            else:
                descend.append(name)
        dirnames[:] = descend
PYEOF
}

# True when a path lies inside one of the enumerated build trees. Used to filter
# the header path and the dump discovery with the same verdict the scan uses.
ra8_misra_is_build_output() {
  local candidate="$1" excluded
  for excluded in ${RA8_MISRA_BUILD_DIRS[@]+"${RA8_MISRA_BUILD_DIRS[@]}"}; do
    if [[ "$candidate" == "$excluded" || "$candidate" == "$excluded"/* ]]; then
      return 0
    fi
  done
  return 1
}

# --- selftest --------------------------------------------------------------
# Both directions, against a throwaway tree, driving the SAME two functions the
# scan drives. One direction alone proves nothing here: an exclusion that
# matched everything would also produce a perfectly quiet, perfectly wrong
# audit, and that failure reads as a burn-down.
_ra8_misra_expect() {
  if [[ "$1" == "yes" ]]; then
    echo "  [ok] $2"
  else
    echo "  [FAIL] $2"
    RA8_MISRA_SELFTEST_FAILS=$((RA8_MISRA_SELFTEST_FAILS + 1))
  fi
}

_ra8_misra_expect_listed() {
  local listing="$1" wanted="$2"
  if grep -qxF "$wanted" "$listing"; then
    _ra8_misra_expect yes "excluded (build output): $wanted"
  else
    _ra8_misra_expect no "excluded (build output): $wanted"
  fi
}

_ra8_misra_expect_absent() {
  local listing="$1" wanted="$2"
  if grep -qxF "$wanted" "$listing"; then
    _ra8_misra_expect no "still scanned (source root): $wanted"
  else
    _ra8_misra_expect yes "still scanned (source root): $wanted"
  fi
}

# A dirty checkout: build output under every root that can produce one, plus
# source under the two paths a blanket */build/* glob would wrongly swallow.
_ra8_misra_selftest_fixture() {
  local tree="$1"
  mkdir -p \
    "$tree/tools/foo/build/CMakeFiles/3.28.3/CompilerIdC" \
    "$tree/apps/stand_alone/prod/build/CMakeFiles" \
    "$tree/examples/ek_ra8d2/hw_validated/smoke/blink/build" \
    "$tree/tests/build-cov" \
    "$tree/libs/ra8_x/build" \
    "$tree/libs/ra8_x/builders" \
    "$tree/libs/ra8_x/src" \
    "$tree/tools/foo/src"
  : >"$tree/tools/foo/build/CMakeFiles/3.28.3/CompilerIdC/CMakeCCompilerId.c"
  : >"$tree/libs/ra8_x/build/real.c"
  : >"$tree/libs/ra8_x/builders/real.c"
  : >"$tree/libs/ra8_x/src/real.c"
  : >"$tree/tools/foo/src/real.c"
}

_ra8_misra_selftest_roots() {
  (cd "$1" && ra8_misra_build_output_dirs libs port tools apps examples tests)
}

# Both directions over the dirty fixture, plus the non-vacuity floor: an
# enumerator that returned nothing would pass every must-stay-quiet assertion
# here on its own.
_ra8_misra_selftest_dirty() {
  local listing="$1"

  # MUST FIRE -- every root where build output can legitimately live.
  _ra8_misra_expect_listed "$listing" "tools/foo/build"
  _ra8_misra_expect_listed "$listing" "apps/stand_alone/prod/build"
  _ra8_misra_expect_listed "$listing" "examples/ek_ra8d2/hw_validated/smoke/blink/build"
  _ra8_misra_expect_listed "$listing" "tests/build-cov"

  # MUST STAY QUIET -- libs/ is not a build-tree root, so `build` there is an
  # ordinary directory that may hold real source; `builders/` is never one.
  _ra8_misra_expect_absent "$listing" "libs/ra8_x/build"
  _ra8_misra_expect_absent "$listing" "libs/ra8_x/builders"
  _ra8_misra_expect_absent "$listing" "libs/ra8_x/src"
  _ra8_misra_expect_absent "$listing" "tools/foo/src"

  if [[ "$(wc -l <"$listing")" -ge 4 ]]; then
    _ra8_misra_expect yes "the dirty fixture yields a non-empty exclusion set"
  else
    _ra8_misra_expect no "the dirty fixture yields a non-empty exclusion set"
  fi
}

# The other end of the same property: a CLEAN tree must exclude NOTHING, so an
# exclusion that had quietly become blanket cannot pass as a quiet audit.
_ra8_misra_selftest_clean() {
  local tree="$1" listing="$2"
  mkdir -p "$tree/libs/ra8_x/src" "$tree/tools/foo/src"
  : >"$tree/libs/ra8_x/src/real.c"
  _ra8_misra_selftest_roots "$tree" >"$listing"
  if [[ ! -s "$listing" ]]; then
    _ra8_misra_expect yes "a clean tree excludes nothing"
  else
    _ra8_misra_expect no "a clean tree excludes nothing"
  fi
}

# The membership predicate the header path and the dump discovery are filtered
# with must agree with the enumeration, in both directions.
_ra8_misra_selftest_predicate() {
  local listing="$1" probe
  probe="tools/foo/build/CMakeFiles/3.28.3/CompilerIdC/CMakeCCompilerId.c"
  mapfile -t RA8_MISRA_BUILD_DIRS <"$listing"
  if ra8_misra_is_build_output "$probe"; then
    _ra8_misra_expect yes "the membership predicate rejects a file inside a build tree"
  else
    _ra8_misra_expect no "the membership predicate rejects a file inside a build tree"
  fi
  if ra8_misra_is_build_output "libs/ra8_x/build/real.c"; then
    _ra8_misra_expect no "the membership predicate keeps source under a source root"
  else
    _ra8_misra_expect yes "the membership predicate keeps source under a source root"
  fi
  RA8_MISRA_BUILD_DIRS=()
}

ra8_misra_selftest() {
  local tmp listing
  RA8_MISRA_SELFTEST_FAILS=0
  echo "misra_check_inner.sh --selftest"
  tmp="$(mktemp -d)"
  listing="$tmp/excluded.txt"

  _ra8_misra_selftest_fixture "$tmp/tree"
  _ra8_misra_selftest_roots "$tmp/tree" >"$listing"
  _ra8_misra_selftest_dirty "$listing"
  _ra8_misra_selftest_clean "$tmp/clean" "$tmp/clean.txt"
  _ra8_misra_selftest_predicate "$listing"

  rm -rf "$tmp"
  if [[ "$RA8_MISRA_SELFTEST_FAILS" -ne 0 ]]; then
    echo "SELFTEST FAILED: $RA8_MISRA_SELFTEST_FAILS assertion(s)" >&2
    return 1
  fi
  echo "selftest: all assertions held (both directions)."
  return 0
}

case "${1:-}" in
  --selftest)
    ra8_misra_selftest
    exit $?
    ;;
  "") ;;
  *)
    echo "misra_check_inner.sh: unknown option: $1" >&2
    echo "usage: misra_check_inner.sh [--selftest]" >&2
    exit 2
    ;;
esac

# The build trees to keep out of this run. Empty on a clean checkout, which is
# why the selftest asserts that case too.
RA8_MISRA_BUILD_DIRS=()
mapfile -t RA8_MISRA_BUILD_DIRS < <(ra8_misra_build_output_dirs "${RA8_MISRA_ROOTS[@]}")
RA8_MISRA_EXCLUDE_ARGS=()
for _build_dir in ${RA8_MISRA_BUILD_DIRS[@]+"${RA8_MISRA_BUILD_DIRS[@]}"}; do
  RA8_MISRA_EXCLUDE_ARGS+=("-i$_build_dir")
done
if [[ ${#RA8_MISRA_BUILD_DIRS[@]} -gt 0 ]]; then
  echo "[INFO] excluding ${#RA8_MISRA_BUILD_DIRS[@]} build tree(s) from the scan:" >&2
  printf '         %s\n' "${RA8_MISRA_BUILD_DIRS[@]}" >&2
fi

# Delete dump artefacts on EVERY exit path, not just success. A stale dump
# left behind by an aborted run (disk-full, timeout, ^C) would be picked up
# by the next run's find and could resurrect findings for source that has
# since changed -- silent corruption of the ratchet comparison.
# shellcheck disable=SC2329  # invoked by `trap cleanup_dumps EXIT` below.
cleanup_dumps() {
  find "${RA8_MISRA_ROOTS[@]}" -name '*.dump' -not -path '*/third_party/*' -delete 2>/dev/null || true
  find "${RA8_MISRA_ROOTS[@]}" -name '*.ctu-info' -not -path '*/third_party/*' -delete 2>/dev/null || true
}
trap cleanup_dumps EXIT

if ! command -v cppcheck >/dev/null 2>&1; then
  echo "[ERROR] cppcheck not in PATH (brew install cppcheck)" >&2
  exit 2
fi

ADDON_DIR=""
for candidate in /opt/homebrew/share/Cppcheck/addons /usr/share/cppcheck/addons \
  /usr/local/share/Cppcheck/addons /usr/lib/*-linux-gnu/cppcheck/addons; do
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

JOBS="${JOBS:-$(ra8_max_jobs)}"

# Header roots, enumerated FROM the scan roots above rather than hand-picked,
# and at ANY depth beneath them.
#
# The list used to name five directories while the audit scanned libs/
# AND port/, so every header under port/*/inc was invisible: cppcheck saw
# the calls into those interfaces as implicitly-declared functions and
# charged the caller MISRA 17.3 for them. That is a defect in the audit's
# view of the tree, not in the tree -- the same failure the annotation
# gate had when its include path was a hand-picked list (see
# _include_args() in check_annotations.py).
#
# It recurred twice while apps/ was being added, which is why neither a
# hand-written list nor a fixed-depth glob is used here any more. First the
# glob still named only tools/*/inc, so the product headers under apps/ went
# missing. Then the products moved down a level, under a category directory,
# and a one-star glob missed them again. Finding every directory NAMED inc under
# the scanned roots has no depth to get wrong: the audit's header path is a
# consequence of the layout instead of a second description of it.
#
# ...and the same for `src` under apps/, where the PRIVATE header roots have to
# be on the path too. Elsewhere they do not: a libs/ or tools/ module is one
# directory pair, so a TU reaches its own `*_internal.h` by the same-directory
# rule and needs no -I. A product under apps/ spans two directories -- its
# portable core at apps/shared/<product> and each build form at
# apps/<form>/<product> -- so a form's TU includes core `*_internal.h` headers
# that same-directory resolution cannot see. Without this, cppcheck charged the
# caller 17.3 for the resulting implicit declarations AND silently stopped
# raising 9.2 on aggregates whose types it could no longer resolve. A net DROP
# in findings is the dangerous direction of that failure: it reads as a
# burn-down. Derived by the same find, so a product that grows a third
# directory is covered the day it lands.
INCLUDE_DIRS=()
while IFS= read -r _inc_dir; do
  [[ -n "$_inc_dir" ]] || continue
  # An `inc` directory inside a build tree is a copy of somebody else's
  # headers; filtered by the SAME verdict that keeps that tree out of the scan,
  # not by a second glob that could disagree with it.
  ra8_misra_is_build_output "$_inc_dir" && continue
  INCLUDE_DIRS+=("-I$_inc_dir")
done < <(
  {
    find "${RA8_MISRA_ROOTS[@]}" -type d -name inc \
      -not -path '*/third_party/*' 2>/dev/null
    find apps -type d -name src \
      -not -path '*/third_party/*' 2>/dev/null
  } | sort -u
)
if [[ ${#INCLUDE_DIRS[@]} -eq 0 ]]; then
  echo "[ERROR] no header roots found -- run from the repo root" >&2
  exit 2
fi

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
#
# -U__clang__ is load-bearing. ra8_attributes.h guards the annotation
# macros with `#if defined(__clang__) && !defined(__CPPCHECK__)` so the
# `[[clang::annotate("...")]]` form -- which cppcheck's C parser cannot
# represent, and which damages the parse of whatever declaration follows
# -- stays hidden from this audit. That guard is not sufficient on its
# own: cppcheck does not merely evaluate the condition, it ENUMERATES
# configurations for every macro named in it, and emits a
# `__GNUC__;__clang__` configuration in which __clang__ is defined and
# __CPPCHECK__ is not. In that configuration the attribute survives, and
# misra.py reports phantom findings against the annotated file -- 14.2,
# 16.2 and 17.3 where the source has no loop, no switch and no implicit
# declaration, plus inflated 15.5 exit-point counts and an extra 8.4 for
# every definition whose declaration the damaged parse swallowed.
# Mentioning __CPPCHECK__ in the guard is what makes cppcheck enumerate
# it, so the header alone can never close this.
#
# -U removes __clang__ from configuration enumeration entirely, so no
# such configuration is generated. This is also the configuration that
# ships: the firmware is built by arm-none-eabi-gcc, where these macros
# are already comments. Suppressing the four rules or absorbing the
# findings into the baseline would blind the ratchet to real defects in
# every annotated file.
echo "[INFO] generating cppcheck dumps under ${RA8_MISRA_ROOTS[*]} ..." >&2
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
  -itools/vela/generated \
  ${RA8_MISRA_EXCLUDE_ARGS[@]+"${RA8_MISRA_EXCLUDE_ARGS[@]}"} \
  -U__clang__ \
  --std=c11 \
  --platform=unix32 \
  --language=c \
  --quiet \
  --error-exitcode=0 \
  "${INCLUDE_DIRS[@]}" \
  "${RA8_MISRA_ROOTS[@]}" \
  2>"$RAW"
RC=$?
set -e

if [[ "$RC" -eq 124 ]]; then
  echo "[ERROR] cppcheck exceeded 10-minute budget" >&2
  exit 1
fi

# Run misra.py on every dump file produced under the first-party roots.
DUMPS=()
while IFS= read -r _dump; do
  [[ -n "$_dump" ]] || continue
  ra8_misra_is_build_output "$_dump" && continue
  DUMPS+=("$_dump")
done < <(
  find "${RA8_MISRA_ROOTS[@]}" -name '*.dump' \
    -not -path '*/third_party/*' -not -path '*/vela/generated/*' | sort
)
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
