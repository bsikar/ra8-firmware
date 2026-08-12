#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# scan_build.sh -- Run clang's scan-build static analyzer against the
# host unit-test build of ra8-firmware.
#
# Background
# ----------
# scan-build (a.k.a. the Clang Static Analyzer) is a path-sensitive
# analyzer that ships with clang. It complements cppcheck (style /
# pattern) by finding null-deref, use-after-free, dead-store and
# logic-error paths through symbolic execution.
#
# We point it at the host test build (built OUT of the source tree; see
# BUILD_DIR below) rather than the cross-compile firmware build because clang
# cannot consume the arm-none-eabi sysroot reliably. The host build covers the
# same first-party libs/, src/, port/ TUs.
#
# Findings under libs/third_party/ are filtered out (SOUP -- see
# CLAUDE.md, docs/SOUP/). First-party findings are recorded in
# docs/STATIC_ANALYSIS.md as the project's expected baseline.
#
# Usage:
#     scripts/checks/scan_build.sh             # full analyze + summary
#     scripts/checks/scan_build.sh --check     # exit non-zero on any
#                                             #   actionable first-party
#                                             #   finding after suppressions
#     scripts/checks/scan_build.sh --strict    # alias for --check (CI naming)
#
# --strict is what the `scan-build` CI gate runs (RA8_GATE_REGISTRY ->
# gate_scan_build in scripts/ci/gates/analysis.sh -> the scan-build job in
# firmware.yml). It is blocking; there is no warn-only mode. Every run
# reconfigures and rebuilds from scratch -- the analyzer only sees what the
# build compiles, so an incremental run analyses nothing and would otherwise
# report a clean tree.
#
# Exit codes:
#     0  analysed, no actionable first-party findings
#     1  analysed, findings (only reachable with --check / --strict)
#     2  COULD NOT RUN -- no analyzer, unwritable tree, failed configure,
#        failed build, or a scan too small to mean anything. Never 0: a
#        caller must be able to tell "found nothing" from "never ran".
#
# Suppressions (applied AFTER the analyzer runs, by post-processing the
# generated HTML reports):
#
#   * core.FixedAddressDereference in libs/ra8_hal/src/ and libs/ra8_hal/inc/
#     -- every memory-mapped register access in the HAL goes through an
#     inline accessor that casts a uintptr_t enum constant to a volatile
#     pointer (the documented project pattern, see CLAUDE.md "Hardware
#     Register Access"). The checker fires on every such write, which is
#     by definition the entire job of MMIO firmware. There is no way to
#     satisfy this checker without abandoning register-level access, so
#     the entire HAL source/include partition is suppressed for this one
#     check-id. See docs/STATIC_ANALYSIS.md for the rationale.
#
#     NOTE: clang-18 -- the project pin -- has no such checker (it arrives in
#     a later major), so this filter currently matches nothing. It stays
#     because moving the pin brings ~683 findings back at once.
#
# Environment overrides:
#     CMAKE                  -- cmake binary (default: cmake on PATH)
#     SCAN_BUILD             -- analyzer binary (default: scan-build-18, else
#                               scan-build)
#     USE_CC / USE_CXX       -- compiler scan-build drives (default: the clang
#                               major derived from $SCAN_BUILD, e.g. clang-18)
#     RA8_SCAN_BUILD_OUT_DIR -- out-of-tree dir holding the build + report trees
#                               (default: a per-checkout dir under $TMPDIR)
#     MIN_TRANSLATION_UNITS  -- vacuity floor on the analysed TU count
#

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

# ra8_max_jobs -- the ONE canonical bounded-parallelism width (#328).
# shellcheck source=scripts/ci/lib/parallelism.sh
. "$SCRIPT_DIR/../ci/lib/parallelism.sh"

CMAKE="${CMAKE:-cmake}"
# clang-tools-18 is the project pin (.devcontainer/Dockerfile) and installs
# scan-build-18; a bare `scan-build` symlink comes from the unversioned
# package and is absent on the CI image. Prefer the pinned major, so a run here
# and a run on a runner analyse with the same checkers.
if [[ -z "${SCAN_BUILD:-}" ]] && command -v scan-build-18 >/dev/null 2>&1; then
  SCAN_BUILD=scan-build-18
fi
SCAN_BUILD="${SCAN_BUILD:-scan-build}"

# Pin the COMPILER scan-build drives to the same clang major as the analyzer
# driver, DERIVED from $SCAN_BUILD so the pin cannot drift in halves.
#
# scan-build reasons about the language dialect its --use-cc compiler accepts;
# scan-build-18 paired with a clang that predates C23 typed enums is analysing
# a different language than the one being compiled. There is no unversioned
# `clang` on the dev box or the CI image (clang-tools-18 ships scan-build-18 and
# clang-18 but no bare symlink), and scan-build SILENTLY falls back to the
# system default compiler -- gcc-12 here -- when --use-cc names a binary that
# does not exist. gcc-12 cannot parse this tree's C23, so the analysed build died
# 1356 errors deep for a reason with nothing to do with the tree under test
# (#559). The existence of the derived compiler is asserted (fail-loud) below,
# once the analyzer itself is known to be present.
_sb_base="$(basename -- "$SCAN_BUILD")"
_sb_ver="${_sb_base##*-}"
if [[ "$_sb_ver" =~ ^[0-9]+$ ]]; then
  USE_CC="${USE_CC:-clang-$_sb_ver}"
  USE_CXX="${USE_CXX:-clang++-$_sb_ver}"
else
  USE_CC="${USE_CC:-clang}"
  USE_CXX="${USE_CXX:-clang++}"
fi
unset _sb_base _sb_ver

# Vacuity floor: the compile database must list, and the build must produce,
# at least this many translation units before a clean report means anything.
# Measured 1128 on the current tree; this is a trip-wire for a collapsed
# configure or a no-op incremental build, not a policy on suite size.
MIN_TRANSLATION_UNITS="${MIN_TRANSLATION_UNITS:-800}"

# ===========================================================================
# THE TWO DECISIONS, FACTORED OUT SO --selftest CAN DRIVE THEM
#
# Everything else in this script is orchestration: configure, build, print.
# These two are the judgement, and both can fail silently -- the classifier by
# binning a real finding as suppressed, the floor by accepting a scan too
# small to mean anything. Both are asserted in both directions below.
# ===========================================================================

#: Findings, by partition. Globals because `classify_reports` fills them and
#: the summary reads them; a subshell would lose the counts.
FIRST_PARTY_COUNT=0
THIRD_PARTY_COUNT=0
TEST_COUNT=0
MMIO_SUPPRESSED_COUNT=0

# Bin every report-*.html under $1 into the four counters.
#
# Each report embeds `<!-- BUGFILE /abs/path -->` and `<!-- BUGTYPE ... -->`
# HTML comments naming the source file and the checker. An absent or empty
# directory is legitimate: scan-build writes nothing when it finds nothing.
classify_reports() {
  local report_root="${1:-}" html bugfile bugtype
  FIRST_PARTY_COUNT=0
  THIRD_PARTY_COUNT=0
  TEST_COUNT=0
  MMIO_SUPPRESSED_COUNT=0
  [[ -n "$report_root" && -d "$report_root" ]] || return 0
  while IFS= read -r html; do
    bugfile="$(grep -oE '<!-- BUGFILE [^ ]+ -->' "$html" | head -1 |
      sed 's/<!-- BUGFILE //;s/ -->$//' || true)"
    if [[ -z "$bugfile" ]]; then
      continue
    fi
    bugtype="$(grep -oE '<!-- BUGTYPE [^>]*-->' "$html" | head -1 || true)"
    if [[ "$bugfile" == *"/libs/third_party/"* ]]; then
      THIRD_PARTY_COUNT=$((THIRD_PARTY_COUNT + 1))
    elif [[ "$bugfile" == *"/tests/"* ]]; then
      # Test scaffolding is exempt (CLAUDE.md / docs/MCDC.md).
      TEST_COUNT=$((TEST_COUNT + 1))
    elif [[ "$bugtype" == *"fixed address"* ]] &&
      { [[ "$bugfile" == *"/libs/ra8_hal/src/"* ]] ||
        [[ "$bugfile" == *"/libs/ra8_hal/inc/"* ]] ||
        [[ "$bugfile" == *"/libs/ra8_mpu/src/"* ]] ||
        [[ "$bugfile" == *"/libs/ra8_mpu/inc/"* ]] ||
        [[ "$bugfile" == *"/libs/ra8_core/src/ra8_log.c" ]] ||
        [[ "$bugfile" == *"/libs/ra8_core/src/ra8_exception.c" ]]; }; then
      # Hardware-register MMIO accessor pattern -- ra8_hal, ra8_mpu,
      # and the ITM/SCB accessors in ra8_core/{ra8_log,ra8_exception}
      # are all built on volatile pointers cast from uintptr_t enum
      # constants. See docs/STATIC_ANALYSIS.md for the policy.
      MMIO_SUPPRESSED_COUNT=$((MMIO_SUPPRESSED_COUNT + 1))
    else
      FIRST_PARTY_COUNT=$((FIRST_PARTY_COUNT + 1))
    fi
  done < <(find "$report_root" -name 'report-*.html' 2>/dev/null)
}

# Whether a scan of $1 translation units is large enough to be worth a verdict.
scan_is_big_enough() { [[ "$1" -ge "$MIN_TRANSLATION_UNITS" ]]; }

# --- selftest -------------------------------------------------------------
# Both directions for both decisions, because this script decides what counts
# as an actionable finding and what counts as an analysis at all. A classifier
# that binned everything as suppressed, or a floor that accepted an empty
# scan, would report a clean tree forever -- and this script HAS shipped a
# clean verdict over an analysis that never happened (#532).

_sb_fixture() {
  printf '<!-- BUGFILE %s -->\n<!-- BUGTYPE %s -->\n' "$2" "$3" >"$1"
}

_sb_expect() {
  local label="$1" want="$2" got="$3"
  if [[ "$want" == "$got" ]]; then
    echo "  [ok] $label"
  else
    echo "  [FAIL] $label (want $want, got $got)"
    SELFTEST_FAILURES=$((SELFTEST_FAILURES + 1))
  fi
}

_sb_selftest_classifier() {
  local tmp
  tmp="$(mktemp -d)"
  # MUST be reported: ordinary first-party code, and a fixed-address finding
  # OUTSIDE the documented MMIO partitions (the filter must not be a blanket).
  _sb_fixture "$tmp/report-1.html" /w/port/esp-hosted/src/x.c "Memory leak"
  _sb_fixture "$tmp/report-2.html" /w/libs/ra8_reflow/src/y.c "fixed address dereference"
  # MUST stay quiet: SOUP, test scaffolding, and the MMIO accessor pattern.
  _sb_fixture "$tmp/report-3.html" /w/libs/third_party/miniz/miniz.c "Memory leak"
  _sb_fixture "$tmp/report-4.html" /w/tests/test_thing.c "Memory leak"
  _sb_fixture "$tmp/report-5.html" /w/libs/ra8_hal/src/ra8_vin.c "fixed address dereference"
  _sb_fixture "$tmp/report-6.html" /w/libs/ra8_core/src/ra8_log.c "fixed address dereference"
  classify_reports "$tmp"
  _sb_expect "actionable first-party findings are reported" 2 "$FIRST_PARTY_COUNT"
  _sb_expect "vendored SOUP is suppressed" 1 "$THIRD_PARTY_COUNT"
  _sb_expect "test scaffolding is suppressed" 1 "$TEST_COUNT"
  _sb_expect "the MMIO accessor pattern is suppressed" 2 "$MMIO_SUPPRESSED_COUNT"
  classify_reports "$tmp/does-not-exist"
  _sb_expect "an absent report dir yields no findings" 0 "$FIRST_PARTY_COUNT"
  rm -rf "$tmp"
}

_sb_selftest_floor() {
  local rc
  scan_is_big_enough 0 && rc=yes || rc=no
  _sb_expect "a zero-TU scan is refused" no "$rc"
  scan_is_big_enough $((MIN_TRANSLATION_UNITS - 1)) && rc=yes || rc=no
  _sb_expect "a scan one TU short of the floor is refused" no "$rc"
  scan_is_big_enough "$MIN_TRANSLATION_UNITS" && rc=yes || rc=no
  _sb_expect "a scan at the floor is accepted" yes "$rc"
}

_sb_selftest_fail_loud() {
  local rc=0
  SCAN_BUILD=/nonexistent/scan-build bash "${BASH_SOURCE[0]}" --strict \
    >/dev/null 2>&1 || rc=$?
  _sb_expect "a missing analyzer exits 2, never 0" 2 "$rc"
}

run_selftest() {
  SELFTEST_FAILURES=0
  echo "scan_build.sh --selftest:"
  _sb_selftest_classifier
  _sb_selftest_floor
  _sb_selftest_fail_loud
  if [[ "$SELFTEST_FAILURES" -ne 0 ]]; then
    echo "scan_build.sh: SELFTEST FAILED ($SELFTEST_FAILURES assertion(s))" >&2
    exit 1
  fi
  echo "scan_build.sh: selftest OK (both directions, classifier + floor + fail-loud)."
  exit 0
}

CHECK_MODE=0
case "${1:-}" in
  --selftest)
    run_selftest
    ;;
  --check | --strict)
    CHECK_MODE=1
    shift
    ;;
esac

# A missing analyzer is a FAILURE, never a skip. This used to `exit 0` with a
# "SKIP" line, which is the shape a gate must never have: the caller cannot
# tell "analysed and found nothing" from "never ran". Same rule as
# check_nsc_cmse.sh and every require_cmd in scripts/ci.sh.
if ! command -v "$SCAN_BUILD" >/dev/null 2>&1; then
  echo "scan_build.sh: FATAL -- $SCAN_BUILD is not on PATH; nothing was analysed." >&2
  echo "  The project pin is clang-tools-18, which installs scan-build-18." >&2
  echo "  Ubuntu: sudo apt install clang-tools-18   macOS: brew install llvm" >&2
  echo "  Override the binary with SCAN_BUILD=<path>." >&2
  exit 2
fi

# The clang that scan-build must DRIVE has to exist too, or scan-build silently falls
# back to the system default (gcc) and "analyses" a build that cannot even parse
# C23 (#559). Turn that silent fallback into a loud failure -- the same contract
# as the missing-analyzer check above.
if ! command -v "$USE_CC" >/dev/null 2>&1; then
  echo "scan_build.sh: FATAL -- the analyzer $SCAN_BUILD is present but the" >&2
  echo "  clang it must drive ($USE_CC) is not on PATH. scan-build would fall" >&2
  echo "  back to the system default compiler (gcc), which cannot parse this" >&2
  echo "  tree's C23, so nothing meaningful would be analysed." >&2
  echo "  Install the matching clang, or point SCAN_BUILD at a driver whose" >&2
  echo "  clang major is installed (scan-build-18 pairs with clang-18)." >&2
  exit 2
fi

# scan-build writes NOTHING under the source tree. An in-tree build tree
# (tests/build-scan) left clang-instrumented `.gcno` files under the checkout,
# and the `coverage` gate that runs later in the same snapshot -- gcovr walks
# the FILESYSTEM, not the git index, so .gitignore does not hide them -- died
# parsing a clang profile version its gcc-14 gcov rejects (#558). Building out of
# tree removes the contamination at the source: gcovr cannot discover what is
# not in the tree it scans.
#
# The path is per-checkout (a cksum of REPO_ROOT) so concurrent worktrees on the
# shared box do not collide, and stable so the top-of-run wipe below reclaims the
# previous run's tree instead of leaking one temp dir per run.
_sb_tag="$(printf '%s' "$REPO_ROOT" | cksum | cut -d' ' -f1)"
SCAN_OUT_DIR="${RA8_SCAN_BUILD_OUT_DIR:-${TMPDIR:-/tmp}/ra8-scan-build-$(id -u)-${_sb_tag}}"
unset _sb_tag
BUILD_DIR="$SCAN_OUT_DIR/build"
REPORT_DIR="$SCAN_OUT_DIR/reports"

# BOTH directories are wiped first, and neither is an optimisation to reclaim.
#
# The BUILD tree: scan-build analyses what the build COMPILES, so an incremental
# build analyses only what changed. A second local run over an up-to-date tree
# compiles nothing, produces no report, and reads as a clean analysis.
#
# The REPORT tree: the summary below picks the newest directory under it, which
# on a second run is the PREVIOUS run's. Measured here -- an incremental rerun
# after the offending finding was fixed still reported it, because the numbers
# came from a directory the run had not written. That is a verdict computed
# from another run's evidence, and it is why both go.
rm -rf "$BUILD_DIR" "$REPORT_DIR"
if ! mkdir -p "$BUILD_DIR" "$REPORT_DIR"; then
  echo "scan_build.sh: FATAL -- cannot create $BUILD_DIR / $REPORT_DIR." >&2
  echo "  Nothing can be analysed from a tree this script cannot write to." >&2
  exit 2
fi

# Parallelism: the bounded canonical width, not a raw nproc (#328).
JOBS="$(ra8_max_jobs)"

echo "==> scan-build: configuring host test build at $BUILD_DIR"
# A failed configure is fatal. It used to fall through: cmake died, no TU was
# ever compiled, and the summary below still printed "first-party bugs : 0"
# and exited 0 -- a clean verdict over an analysis that never happened.
if ! "$SCAN_BUILD" --use-cc="$USE_CC" --use-c++="$USE_CXX" \
  "$CMAKE" -B "$BUILD_DIR" -S "$REPO_ROOT/tests" \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -Wno-dev >/dev/null; then
  echo "scan_build.sh: FATAL -- cmake configure failed; nothing was analysed." >&2
  exit 2
fi

# VACUITY FLOOR. scan-build emits no report directory at all when it finds
# nothing, so "no reports" cannot be told from "nothing was analysed" by
# looking at the output. The compile database can: it lists exactly the
# translation units the analyzer will be handed. A collapsed one means the
# clean verdict below would be a verdict over almost no code.
TU_COUNT="$(grep -c '"file"' "$BUILD_DIR/compile_commands.json" 2>/dev/null || echo 0)"
if ! scan_is_big_enough "$TU_COUNT"; then
  echo "scan_build.sh: FATAL -- the compile database lists $TU_COUNT translation" >&2
  echo "  unit(s); the floor is $MIN_TRANSLATION_UNITS. A clean report over a" >&2
  echo "  collapsed scope is not a clean tree, it is an analysis that did not run." >&2
  exit 2
fi
echo "==> scan-build: analyzing $TU_COUNT translation units (several minutes)..."
# -o : per-run html report dir
# Disable a couple of high-noise checks that fire mostly on test
# scaffolding -- enabling them would drown out real first-party signal.
#
# --status-bugs is deliberately NOT passed: the path/check-id filtering below
# is what decides the verdict, and scan-build's own exit status cannot express
# it. That makes this exit status the BUILD's, so a non-zero one means the
# compile failed and nothing downstream is trustworthy.
BUILD_RC=0
"$SCAN_BUILD" --use-cc="$USE_CC" --use-c++="$USE_CXX" \
  -o "$REPORT_DIR" \
  -disable-checker deadcode.DeadStores \
  -disable-checker security.insecureAPI.DeprecatedOrUnsafeBufferHandling \
  "$CMAKE" --build "$BUILD_DIR" --parallel "$JOBS" \
  >"$REPORT_DIR/scan-build.log" 2>&1 || BUILD_RC=$?
if [[ "$BUILD_RC" -ne 0 ]]; then
  echo "scan_build.sh: FATAL -- the analyzed build FAILED (exit $BUILD_RC)." >&2
  echo "  It compiled nothing analysable; no verdict downstream is trustworthy." >&2
  # Surface the actual COMPILER error, not the last screenful. On this build the
  # tail is trailing scan-build "N bugs found" chatter and vendored
  # tf-psa-crypto analyzer warnings that say nothing about why the build died
  # (#558). Real diagnostics carry an `error:`; show the first cluster of them
  # and fall back to a tail only when the failure produced none (a link or
  # generator error).
  if grep -nE 'error:' "$REPORT_DIR/scan-build.log" >/dev/null 2>&1; then
    echo "  First compile errors in $REPORT_DIR/scan-build.log:" >&2
    grep -nE 'error:' "$REPORT_DIR/scan-build.log" | head -n 20 >&2
  else
    echo "  No 'error:' diagnostic found (a link or generator failure?);" >&2
    echo "  tail of $REPORT_DIR/scan-build.log:" >&2
    tail -n 40 "$REPORT_DIR/scan-build.log" >&2 || true
  fi
  exit 2
fi

# SECOND VACUITY FLOOR, on the other side of the build. The compile database
# says what the analyzer was OFFERED; the object files say what it actually
# compiled. Zero findings is a legitimate outcome, so an empty report directory
# proves nothing on its own -- these do.
OBJ_COUNT="$(find "$BUILD_DIR" -name '*.o' -type f | wc -l | tr -d ' ')"
if ! scan_is_big_enough "$OBJ_COUNT"; then
  echo "scan_build.sh: FATAL -- the analyzed build produced $OBJ_COUNT object" >&2
  echo "  file(s) from $TU_COUNT translation units; the floor is" >&2
  echo "  $MIN_TRANSLATION_UNITS. Nothing was analysed, so there is no verdict." >&2
  exit 2
fi

# The report directory scan-build produced. It is unambiguous because
# REPORT_DIR was wiped above: any directory in here is this run's.
LATEST_REPORT="$(find "$REPORT_DIR" -maxdepth 1 -mindepth 1 -type d |
  sort | tail -n 1 || true)"

# Count findings, partitioning first-party vs suppressed (third_party
# SOUP + host-only test scaffolding + documented HAL MMIO suppression).
classify_reports "$LATEST_REPORT"

echo ""
echo "==> scan-build summary"
echo "    translation units: $TU_COUNT analysed, $OBJ_COUNT object(s) produced"
echo "    report dir       : ${LATEST_REPORT:-<none, no findings>}"
echo "    first-party bugs : $FIRST_PARTY_COUNT          (actionable)"
echo "    HAL MMIO bugs    : $MMIO_SUPPRESSED_COUNT  (suppressed -- register accessor pattern)"
echo "    third-party bugs : $THIRD_PARTY_COUNT  (suppressed -- SOUP)"
echo "    test-scaffold    : $TEST_COUNT          (suppressed -- exempt)"
echo "    full log         : $REPORT_DIR/scan-build.log"
echo ""
echo "    See docs/STATIC_ANALYSIS.md for the documented baseline."

if [[ "$CHECK_MODE" -eq 1 && "$FIRST_PARTY_COUNT" -gt 0 ]]; then
  echo "scan_build.sh: --check/--strict FAILED ($FIRST_PARTY_COUNT first-party findings)"
  exit 1
fi

exit 0
