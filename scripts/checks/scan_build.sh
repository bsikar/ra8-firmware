#!/usr/bin/env bash
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
# We point it at the host test build (tests/build-scan/) rather than
# the cross-compile firmware build because clang cannot consume the
# arm-none-eabi sysroot reliably. The host build covers the same first-
# party libs/, src/, port/ TUs.
#
# Findings under libs/third_party/ are filtered out (SOUP -- see
# CLAUDE.md, docs/SOUP/). First-party findings are recorded in
# docs/STATIC_ANALYSIS.md as the project's expected baseline.
#
# Usage:
#     scripts/checks/scan_build.sh             # full analyze + summary
#     scripts/checks/scan_build.sh --check     # exit non-zero on any
#                                             #   first-party finding
#                                             #   (CI gate; warn-only
#                                             #   today, see pre-commit)
#     scripts/checks/scan_build.sh --strict    # alias for --check (CI naming);
#                                             #   exit non-zero on any
#                                             #   actionable first-party
#                                             #   finding after suppressions
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
# Environment overrides:
#     CMAKE       -- cmake binary (default: cmake on PATH)
#     SCAN_BUILD  -- scan-build binary (default: scan-build on PATH)
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

# ra8_max_jobs -- the ONE canonical bounded-parallelism width (#328).
# shellcheck source=scripts/ci/lib/parallelism.sh
. "$SCRIPT_DIR/../ci/lib/parallelism.sh"

CMAKE="${CMAKE:-cmake}"
SCAN_BUILD="${SCAN_BUILD:-scan-build}"

CHECK_MODE=0
if [[ "${1:-}" == "--check" || "${1:-}" == "--strict" ]]; then
  CHECK_MODE=1
  shift
fi

if ! command -v "$SCAN_BUILD" >/dev/null 2>&1; then
  echo "scan_build.sh: SKIP -- $SCAN_BUILD not on PATH."
  echo "  Install: sudo apt install clang-tools-18  (or brew install llvm)"
  exit 0
fi

BUILD_DIR="$REPO_ROOT/tests/build-scan"
REPORT_DIR="$REPO_ROOT/build/scan-build-reports"

mkdir -p "$BUILD_DIR" "$REPORT_DIR"

# Parallelism: the bounded canonical width, not a raw nproc (#328).
JOBS="$(ra8_max_jobs)"

echo "==> scan-build: configuring host test build at $BUILD_DIR"
"$SCAN_BUILD" --use-cc=clang --use-c++=clang++ \
  "$CMAKE" -B "$BUILD_DIR" -S "$REPO_ROOT/tests" \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -Wno-dev >/dev/null

echo "==> scan-build: analyzing (this may take several minutes)..."
# -o : per-run html report dir
# --status-bugs : exit non-zero if bugs found (we re-implement filtering
#   below since scan-build's exit code does not understand path filters).
# Disable a couple of high-noise checks that fire mostly on test
# scaffolding -- enabling them would drown out real first-party signal.
"$SCAN_BUILD" --use-cc=clang --use-c++=clang++ \
  -o "$REPORT_DIR" \
  -disable-checker deadcode.DeadStores \
  -disable-checker security.insecureAPI.DeprecatedOrUnsafeBufferHandling \
  "$CMAKE" --build "$BUILD_DIR" --parallel "$JOBS" \
  >"$REPORT_DIR/scan-build.log" 2>&1 || true

# Locate the freshest report directory scan-build produced.
LATEST_REPORT="$(find "$REPORT_DIR" -maxdepth 1 -mindepth 1 -type d |
  sort | tail -n 1 || true)"

# Count findings, partitioning first-party vs suppressed (third_party
# SOUP + host-only test scaffolding + documented HAL MMIO suppression).
FIRST_PARTY_COUNT=0
THIRD_PARTY_COUNT=0
TEST_COUNT=0
MMIO_SUPPRESSED_COUNT=0
if [[ -n "$LATEST_REPORT" && -d "$LATEST_REPORT" ]]; then
  while IFS= read -r html; do
    # Each report html embeds a BUGFILE marker in an HTML comment
    # (e.g. `<!-- BUGFILE /abs/path/to/file.c -->`) pointing at the
    # source file the bug was reported against. Use that to bin the
    # finding.
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
  done < <(find "$LATEST_REPORT" -name 'report-*.html' 2>/dev/null)
fi

echo ""
echo "==> scan-build summary"
echo "    report dir       : ${LATEST_REPORT:-<none>}"
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
