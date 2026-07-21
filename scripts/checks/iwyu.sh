#!/usr/bin/env bash
#
# iwyu.sh -- Run include-what-you-use over the host unit-test build.
#
# Background
# ----------
# include-what-you-use (IWYU) is an LLVM-based tool that reports
# include-graph hygiene violations on a per-translation-unit basis:
#
#   * Direct uses with no direct include    -> "should add"
#   * Direct includes with no direct uses   -> "should remove"
#   * Pulled-in declarations only used by   -> "forward-declare"
#     pointer / reference
#
# Cleaning these up shrinks compile times, surfaces accidental coupling
# between modules, and helps SOLID-S (Single Responsibility) by making
# the public surface of each header explicit.
#
# We run IWYU against the host test build (tests/build-iwyu/) for the
# same reason as scan-build: clang has no working sysroot for
# arm-none-eabi, but the host build covers the same first-party TUs.
#
# Findings under libs/third_party/ are filtered out (SOUP -- see
# CLAUDE.md, docs/SOUP/). The script always exits 0 today (warn-only);
# CI may flip it to strict in a follow-up commit once the project-wide
# baseline is groomed.
#
# Usage:
#     scripts/checks/iwyu.sh                # full pass + summary
#     scripts/checks/iwyu.sh --check        # exit non-zero if any
#                                          # first-party TU has IWYU
#                                          # findings (CI gate)
#
# Environment overrides:
#     CMAKE -- cmake binary (default: cmake on PATH)
#     IWYU  -- IWYU binary  (default: include-what-you-use on PATH)
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

CMAKE="${CMAKE:-cmake}"
IWYU="${IWYU:-include-what-you-use}"

CHECK_MODE=0
if [[ "${1:-}" == "--check" ]]; then
  CHECK_MODE=1
  shift
fi

if ! command -v "$IWYU" >/dev/null 2>&1; then
  echo "iwyu.sh: SKIP -- $IWYU not on PATH."
  echo "  Install: sudo apt install iwyu  (or brew install include-what-you-use)"
  exit 0
fi

BUILD_DIR="$REPO_ROOT/tests/build-iwyu"
REPORT_DIR="$REPO_ROOT/build/iwyu-reports"

mkdir -p "$BUILD_DIR" "$REPORT_DIR"

# Parallelism (portable across linux/macos).
if command -v nproc >/dev/null 2>&1; then
  JOBS="$(nproc)"
elif command -v sysctl >/dev/null 2>&1; then
  JOBS="$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"
else
  JOBS=4
fi

# IWYU is wired via CMAKE_C_INCLUDE_WHAT_YOU_USE / CMAKE_CXX_INCLUDE_
# WHAT_YOU_USE -- cmake invokes the tool alongside every compile.
echo "==> iwyu: configuring host test build at $BUILD_DIR"
"$CMAKE" -B "$BUILD_DIR" -S "$REPO_ROOT/tests" \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DCMAKE_C_INCLUDE_WHAT_YOU_USE="$IWYU" \
  -DCMAKE_CXX_INCLUDE_WHAT_YOU_USE="$IWYU" \
  -Wno-dev >/dev/null

echo "==> iwyu: analyzing (this may take several minutes)..."
LOG="$REPORT_DIR/iwyu.log"
"$CMAKE" --build "$BUILD_DIR" --parallel "$JOBS" >"$LOG" 2>&1 || true

# IWYU writes "should add"/"should remove" lines to stderr, prefixed
# by the source path. Bin findings into first-party vs suppressed.
FIRST_PARTY=$(grep -cE '^/.+\.(c|h|cpp|hpp).* should (add|remove) these lines' "$LOG" 2>/dev/null |
  head -1 || true)
FIRST_PARTY=${FIRST_PARTY:-0}

# Strip third_party / tests entries, then recount per-TU findings.
FP=0
TP=0
TS=0
while IFS= read -r line; do
  # IWYU prints a header line of the form:
  #   /abs/path/foo.c should add these lines:
  src="$(echo "$line" | sed -E 's# should (add|remove) these lines.*##')"
  case "$src" in
    */libs/third_party/*) TP=$((TP + 1)) ;;
    */tests/*) TS=$((TS + 1)) ;;
    /*) FP=$((FP + 1)) ;;
  esac
done < <(grep -E ' should (add|remove) these lines' "$LOG" 2>/dev/null || true)

echo ""
echo "==> iwyu summary"
echo "    log file              : $LOG"
echo "    first-party TUs flagged: $FP"
echo "    third-party TUs        : $TP  (suppressed -- SOUP)"
echo "    test-scaffold TUs      : $TS  (suppressed -- exempt)"
echo ""
echo "    Each TU header above has the full add/remove list in $LOG."
echo "    To inspect a single TU: grep -A 50 '<filename> should add' $LOG"

if [[ "$CHECK_MODE" -eq 1 && "$FP" -gt 0 ]]; then
  echo "iwyu.sh: --check FAILED ($FP first-party TUs have IWYU findings)"
  exit 1
fi

exit 0
