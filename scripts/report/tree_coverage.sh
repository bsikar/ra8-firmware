#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# scripts/report/tree_coverage.sh -- the ONE coverage measurement for the tree.
#
# This script MEASURES and never judges. Every policy question -- what is
# enrolled, what a unit owes, what an unmeasured row may say -- belongs to
# scripts/checks/check_tree_coverage.py and its committed baseline. Splitting
# them is what keeps the tree down to one policy surface: three overlapping
# gates used to each carry their own build AND their own opinion, so the same
# translation unit was compiled twice and answered to two different bars.
#
# The measurement projects come from check_tree_coverage.py --projects, so the
# producer and the checker cannot disagree about which builds exist. For each:
#
#   1. configure with RA8_COVERAGE=ON under a pinned C23-capable compiler,
#   2. build and run its ctest suite,
#   3. report it into its OWN gcovr trace and summary.
#
# The traces are then MERGED into one report. That is the point, not an
# optimisation: the media_dl core is compiled by BOTH the host test suite and
# the media_dl form, so a single sweep over one build tree reports whichever
# half it happened to see (mdl_config.c measures 77.3% from the host suite
# alone and 90.1% from the union). Merging tracefiles is also what avoids the
# gcovr function-merge assertion two coverage builds of one TU otherwise hit --
# --merge-mode-functions=merge-use-line-min resolves the differing line tables
# deterministically.
#
# Outputs, all under build/tree-coverage/:
#   <project>/                  the CMake build tree, incl. compile_commands.json
#   traces/<project>.json       gcovr trace, the merge input
#   summaries/<project>.json    per-project summary, for the non-vacuity floor
#   summary.json                the MERGED per-file summary the checker reads
#   summary.txt, html/          human-readable forms
#
# Usage:
#   bash scripts/report/tree_coverage.sh
#   make coverage

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

# ra8_max_jobs -- the ONE canonical bounded-parallelism width (#328); every
# build and ctest width below derives from it instead of an unbounded default.
# shellcheck source=scripts/ci/lib/parallelism.sh
. "$SCRIPT_DIR/../ci/lib/parallelism.sh"

# Pin a C23-capable host compiler. CMake otherwise defaults to a bare "cc",
# which on the Debian 12 dev box is gcc 12 and cannot parse this codebase's
# C23 typed enums.
# shellcheck source=scripts/builders/select_host_compiler.sh
. "$SCRIPT_DIR/../builders/select_host_compiler.sh"
ra8_select_host_compiler gcc-14 gcc-13 gcc clang-19 clang cc

if ! command -v gcovr >/dev/null 2>&1; then
  echo "error: gcovr not on PATH (pip3 install gcovr)" >&2
  exit 1
fi

OUT_DIR="$REPO_ROOT/build/tree-coverage"
CHECKER="$REPO_ROOT/scripts/checks/check_tree_coverage.py"
JOBS="$(ra8_max_jobs)"
GCOV="$(ra8_gcov_executable_for "$CC")"

# A stale tree is how a gate reads numbers this run never produced: a leftover
# trace from a project that failed to configure merges in and looks measured.
rm -rf "$OUT_DIR"
mkdir -p "$OUT_DIR/traces" "$OUT_DIR/summaries" "$OUT_DIR/html"

# The census roots, and the three subtractions every other checker in this tree
# already makes: vendored SOUP, generated tables, and test sources (the
# instrument, not the thing measured) wherever they live.
GCOVR_SCOPE=(
  --root "$REPO_ROOT"
  --filter "$REPO_ROOT/libs/"
  --filter "$REPO_ROOT/port/"
  --filter "$REPO_ROOT/tools/"
  --filter "$REPO_ROOT/apps/"
  --filter "$REPO_ROOT/examples/"
  --exclude "$REPO_ROOT/libs/third_party/"
  --exclude "$REPO_ROOT/libs/ra8_fonts/"
  --exclude "$REPO_ROOT/port/threadx/"
  --exclude "$REPO_ROOT/tools/vela/generated/"
  --exclude ".*/tests/.*"
)

# --merge-mode-functions: ra8_rsip_life_get (and peers) compile into two TUs at
#   different line numbers; gcovr's default strict function-merge aborts with
#   GcovrMergeAssertionError. merge-use-line-min merges them deterministically.
# --gcov-ignore-parse-errors=all: a bounded status-poll loop driven to its cap
#   trips gcov's "suspicious hits" heuristic (gcc PR 68080), which gcovr treats
#   as a hard parse error. Plain "all" is the only value every gcovr version in
#   this fleet accepts; the newer suspicious_hits.* choice is rejected outright
#   by the runner's older build.
GCOVR_COMMON=(
  --merge-mode-functions=merge-use-line-min
  --gcov-ignore-parse-errors=all
  --exclude-throw-branches
  --exclude-unreachable-branches
)

measure_project() {
  local name="$1" src_dir="$2"
  local build="$OUT_DIR/$name"
  local extra=()

  # RA8_MCDC is a tests/ option only. Passing it to a product listfile earns a
  # "manually-specified variables were not used" warning and buys nothing.
  if [[ "$src_dir" == "tests" ]]; then
    extra+=(-DRA8_MCDC=OFF)
  fi

  echo "==> [$name] configure ($src_dir, CC=$CC)"
  cmake -B "$build" -S "$REPO_ROOT/$src_dir" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DRA8_COVERAGE=ON \
    "${extra[@]}" \
    -DCMAKE_C_COMPILER="$CC" \
    -DCMAKE_CXX_COMPILER="$CXX" \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -Wno-dev >/dev/null

  echo "==> [$name] build"
  cmake --build "$build" --parallel "$JOBS" >/dev/null

  echo "==> [$name] ctest"
  ctest --test-dir "$build" --output-on-failure --timeout 120 -j "$JOBS" | tail -4

  echo "==> [$name] gcovr"
  gcovr \
    --gcov-executable "$GCOV" \
    "${GCOVR_COMMON[@]}" \
    "${GCOVR_SCOPE[@]}" \
    --object-directory "$build" \
    --json "$OUT_DIR/traces/$name.json" \
    --json-summary "$OUT_DIR/summaries/$name.json" \
    -j "$JOBS"
}

TRACE_ARGS=()
while read -r project_name project_dir; do
  [[ -z "$project_name" ]] && continue
  measure_project "$project_name" "$project_dir"
  TRACE_ARGS+=(--json-add-tracefile "$OUT_DIR/traces/$project_name.json")
done < <(python3 "$CHECKER" --projects)

if [[ ${#TRACE_ARGS[@]} -eq 0 ]]; then
  echo "error: no measurement projects; check_tree_coverage.py --projects is empty" >&2
  exit 1
fi

echo "==> merging $((${#TRACE_ARGS[@]} / 2)) project trace(s)"
gcovr \
  --root "$REPO_ROOT" \
  --merge-mode-functions=merge-use-line-min \
  "${TRACE_ARGS[@]}" \
  --json-summary "$OUT_DIR/summary.json" \
  --json-summary-pretty \
  --html-details "$OUT_DIR/html/index.html" \
  --txt "$OUT_DIR/summary.txt" \
  --print-summary

echo ""
echo "==> merged summary: $OUT_DIR/summary.json"
echo "==> HTML report:    $OUT_DIR/html/index.html"
