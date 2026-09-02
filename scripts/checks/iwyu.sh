#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
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
# We run IWYU against the host test build (tests/build-iwyu/) for the same
# reason as scan-build: clang has no working sysroot for arm-none-eabi. This
# covers the host-buildable first-party TUs enrolled by tests/CMakeLists.txt;
# it does not claim the cross-only firmware compositions.
#
# Findings under libs/third_party/ or apps/shared_libs/third_party/ are
# filtered out (SOUP -- see CLAUDE.md and docs/SOUP/). The default invocation
# is report-only; --check exits non-zero when a first-party TU has findings.
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

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

# ra8_max_jobs -- the ONE canonical bounded-parallelism width (#328).
# shellcheck source=scripts/ci/lib/parallelism.sh
. "$SCRIPT_DIR/../ci/lib/parallelism.sh"

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

# Parallelism: the bounded canonical width, not a raw nproc (#328).
JOBS="$(ra8_max_jobs)"

# IWYU is wired via CMAKE_C_INCLUDE_WHAT_YOU_USE / CMAKE_CXX_INCLUDE_
# WHAT_YOU_USE -- cmake invokes the tool alongside every compile.
echo "==> iwyu: configuring host test build at $BUILD_DIR"
"$CMAKE" -B "$BUILD_DIR" -S "$REPO_ROOT/tests" \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DCMAKE_C_INCLUDE_WHAT_YOU_USE="$IWYU" \
  -DCMAKE_CXX_INCLUDE_WHAT_YOU_USE="$IWYU" \
  >/dev/null

echo "==> iwyu: analyzing (this may take several minutes)..."
LOG="$REPORT_DIR/iwyu.log"
if ! "$CMAKE" --build "$BUILD_DIR" --parallel "$JOBS" >"$LOG" 2>&1; then
  echo "iwyu.sh: analysis build FAILED; tail of $LOG:" >&2
  tail -n 80 "$LOG" >&2
  exit 1
fi

# IWYU writes "should add"/"should remove" lines to stderr, prefixed
# by the source path. Bin findings into first-party vs suppressed.
# Separate both third-party roots and tests, then recount per-TU findings.
FP=0
TP=0
TS=0
while IFS= read -r line; do
  # IWYU prints a header line of the form:
  #   /abs/path/foo.c should add these lines:
  src="$(echo "$line" | sed -E 's# should (add|remove) these lines.*##')"
  case "$src" in
    */libs/third_party/* | */apps/shared_libs/third_party/*) TP=$((TP + 1)) ;;
    */tests/*) TS=$((TS + 1)) ;;
    /*) FP=$((FP + 1)) ;;
  esac
done < <(grep -E ' should (add|remove) these lines' "$LOG" 2>/dev/null)

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
