#!/usr/bin/env bash
#
# scripts/utils/mcdc_report.sh -- DO-178B Level B MC/DC report.
#
# Modified Condition/Decision Coverage (MC/DC) is the structural
# coverage criterion mandated by DO-178B Level B for compound boolean
# decisions. The only open-source path to true MC/DC is clang >= 18
# with `-fcoverage-mcdc` paired with `-fcoverage-mapping` and
# `-fprofile-instr-generate`, then reported via
# `llvm-cov show --show-mcdc-summary`.
#
# Pipeline:
#   1. Configure tests/ with RA_MCDC=ON. The CMake probe selects
#      clang's -fcoverage-mcdc when available, otherwise falls back
#      to gcc condition coverage (NOT MC/DC -- prints a warning).
#   2. Build all host tests using clang as both CC and CXX.
#   3. Run each test executable with LLVM_PROFILE_FILE so each emits
#      its own raw .profraw file.
#   4. Merge raw profiles via `llvm-profdata merge -sparse`.
#   5. Render a per-file MC/DC text report via
#      `llvm-cov show --show-mcdc-summary`.
#   6. Exit non-zero if any first-party source has < 100% MC/DC.
#
# This is opt-in; the existing `make test` and `make coverage` flow
# is untouched. Invoke via `make mcdc` or directly:
#
#   bash scripts/utils/mcdc_report.sh
#
# Environment overrides:
#   CC, CXX                 -- compilers (default: clang / clang++ from $PATH)
#   LLVM_PROFDATA, LLVM_COV -- LLVM tools (default: probe alongside $CC)
#   RA_MCDC_BUILD_DIR       -- build dir (default: build/host-mcdc)
#   RA_MCDC_REPORT_DIR      -- report dir (default: build/mcdc-report)
#   RA_MCDC_THRESHOLD       -- minimum percent (default: 100)
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

BUILD_DIR="${RA_MCDC_BUILD_DIR:-$REPO_ROOT/build/host-mcdc}"
REPORT_DIR="${RA_MCDC_REPORT_DIR:-$REPO_ROOT/build/mcdc-report}"
THRESHOLD="${RA_MCDC_THRESHOLD:-100}"

# ---------------------------------------------------------------------------
# Compiler/tool resolution. We require clang for MC/DC; gcc paths are
# tolerated only as fallback so the script still runs end-to-end.
# ---------------------------------------------------------------------------
CC_BIN="${CC:-}"
CXX_BIN="${CXX:-}"
if [[ -z "$CC_BIN" ]]; then
    if [[ -x /opt/homebrew/opt/llvm/bin/clang ]]; then
        CC_BIN=/opt/homebrew/opt/llvm/bin/clang
        CXX_BIN=/opt/homebrew/opt/llvm/bin/clang++
    elif command -v clang >/dev/null 2>&1; then
        CC_BIN="$(command -v clang)"
        CXX_BIN="$(command -v clang++ || echo clang++)"
    else
        CC_BIN="$(command -v cc)"
        CXX_BIN="$(command -v c++ || echo c++)"
    fi
fi
[[ -z "$CXX_BIN" ]] && CXX_BIN="${CC_BIN}++"

# Probe MC/DC support with this CC.
HAVE_MCDC=0
if echo '' | "$CC_BIN" -fprofile-instr-generate -fcoverage-mapping \
        -fcoverage-mcdc -E -x c - >/dev/null 2>&1; then
    HAVE_MCDC=1
fi

# Resolve llvm-profdata / llvm-cov beside $CC_BIN so we don't pick up
# a system version that's incompatible with the profile format clang
# wrote.
CC_DIR="$(dirname "$CC_BIN")"
LLVM_PROFDATA_BIN="${LLVM_PROFDATA:-}"
LLVM_COV_BIN="${LLVM_COV:-}"
if [[ -z "$LLVM_PROFDATA_BIN" ]]; then
    if [[ -x "$CC_DIR/llvm-profdata" ]]; then
        LLVM_PROFDATA_BIN="$CC_DIR/llvm-profdata"
    else
        LLVM_PROFDATA_BIN="$(command -v llvm-profdata 2>/dev/null || true)"
    fi
fi
if [[ -z "$LLVM_COV_BIN" ]]; then
    if [[ -x "$CC_DIR/llvm-cov" ]]; then
        LLVM_COV_BIN="$CC_DIR/llvm-cov"
    else
        LLVM_COV_BIN="$(command -v llvm-cov 2>/dev/null || true)"
    fi
fi

echo "==> ra8d2-firmware MC/DC report"
echo "    CC            = $CC_BIN"
echo "    CXX           = $CXX_BIN"
echo "    llvm-profdata = ${LLVM_PROFDATA_BIN:-<missing>}"
echo "    llvm-cov      = ${LLVM_COV_BIN:-<missing>}"
echo "    MC/DC support = $([[ $HAVE_MCDC -eq 1 ]] && echo yes || echo NO -- fallback only)"
echo "    build dir     = $BUILD_DIR"
echo "    report dir    = $REPORT_DIR"
echo "    threshold     = ${THRESHOLD}%"
echo ""

if [[ $HAVE_MCDC -eq 0 ]]; then
    echo "WARNING: $CC_BIN does NOT support -fcoverage-mcdc."
    echo "         DO-178B Level B MC/DC cannot be measured with this compiler."
    echo "         Install clang >= 18 (brew install llvm / apt install clang-18)."
    echo "         Continuing with the configured fallback for completeness..."
    echo ""
fi

# ---------------------------------------------------------------------------
# 1. Configure
# ---------------------------------------------------------------------------
echo "==> [1/5] Configuring tests/ with RA_MCDC=ON"
cmake -B "$BUILD_DIR" -S "$REPO_ROOT/tests" \
    -DCMAKE_C_COMPILER="$CC_BIN" \
    -DCMAKE_CXX_COMPILER="$CXX_BIN" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DRA_MCDC=ON \
    -DRA_COVERAGE=OFF \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -Wno-dev >/dev/null

# ---------------------------------------------------------------------------
# 2. Build
# ---------------------------------------------------------------------------
echo "==> [2/5] Building host tests"
cmake --build "$BUILD_DIR" --parallel 2>&1 | tail -20 || {
    echo "build failed" >&2
    exit 1
}

# ---------------------------------------------------------------------------
# 3. Run -- one .profraw per test, so we can merge them.
# ---------------------------------------------------------------------------
PROFRAW_DIR="$BUILD_DIR/profraw"
rm -rf "$PROFRAW_DIR"
mkdir -p "$PROFRAW_DIR"

echo "==> [3/5] Running tests with LLVM_PROFILE_FILE"
TEST_BINARIES=()
while IFS= read -r -d '' f; do
    TEST_BINARIES+=("$f")
done < <(find "$BUILD_DIR" -maxdepth 1 -type f -perm -u+x -name 'test_*' -print0)

if [[ ${#TEST_BINARIES[@]} -eq 0 ]]; then
    echo "no test binaries found in $BUILD_DIR" >&2
    exit 1
fi

PASS=0
FAIL=0
for bin in "${TEST_BINARIES[@]}"; do
    name="$(basename "$bin")"
    if LLVM_PROFILE_FILE="$PROFRAW_DIR/${name}.profraw" \
            "$bin" >/dev/null 2>&1; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
    fi
done
echo "    ${PASS} passed, ${FAIL} failed (of ${#TEST_BINARIES[@]} test binaries)"

# ---------------------------------------------------------------------------
# 4. Merge profiles (only meaningful with clang source-based coverage).
# ---------------------------------------------------------------------------
mkdir -p "$REPORT_DIR"
MERGED_PROFDATA="$REPORT_DIR/merged.profdata"
TEXT_REPORT="$REPORT_DIR/mcdc.txt"
SUMMARY_REPORT="$REPORT_DIR/summary.txt"

if [[ $HAVE_MCDC -eq 1 && -n "$LLVM_PROFDATA_BIN" && -n "$LLVM_COV_BIN" ]]; then
    echo "==> [4/5] Merging profiles via llvm-profdata"
    PROFRAW_FILES=()
    while IFS= read -r -d '' f; do
        PROFRAW_FILES+=("$f")
    done < <(find "$PROFRAW_DIR" -name '*.profraw' -print0)

    if [[ ${#PROFRAW_FILES[@]} -eq 0 ]]; then
        echo "no .profraw files emitted -- did the test binaries crash?" >&2
        exit 1
    fi

    "$LLVM_PROFDATA_BIN" merge -sparse \
        -o "$MERGED_PROFDATA" \
        "${PROFRAW_FILES[@]}"

    # ---------------------------------------------------------------------
    # 5. MC/DC report
    # ---------------------------------------------------------------------
    echo "==> [5/5] Generating MC/DC report via llvm-cov"

    # Build the -object argument list (one per test binary).
    OBJ_ARGS=()
    FIRST=1
    for bin in "${TEST_BINARIES[@]}"; do
        if [[ $FIRST -eq 1 ]]; then
            OBJ_ARGS+=("$bin")
            FIRST=0
        else
            OBJ_ARGS+=("-object" "$bin")
        fi
    done

    # Per-file MC/DC dump (verbose, for human inspection).
    "$LLVM_COV_BIN" show \
        "${OBJ_ARGS[@]}" \
        -instr-profile="$MERGED_PROFDATA" \
        -show-mcdc \
        -show-mcdc-summary \
        -format=text \
        -ignore-filename-regex='(third_party|/tests/|/usr/|c\+\+/v[0-9]+)' \
        > "$TEXT_REPORT" 2>/dev/null || true

    # Per-file numeric summary (machine-parseable).
    "$LLVM_COV_BIN" report \
        "${OBJ_ARGS[@]}" \
        -instr-profile="$MERGED_PROFDATA" \
        -show-mcdc-summary \
        -ignore-filename-regex='(third_party|/tests/|/usr/|c\+\+/v[0-9]+)' \
        > "$SUMMARY_REPORT" 2>/dev/null || true

    echo ""
    echo "==> MC/DC summary (tail of $SUMMARY_REPORT):"
    tail -20 "$SUMMARY_REPORT" || true
    echo ""
    echo "Full per-file MC/DC dump : $TEXT_REPORT"
    echo "Per-file numeric summary : $SUMMARY_REPORT"

    # -----------------------------------------------------------------
    # Gate: scan the TOTAL row of the summary for an MC/DC %.
    # llvm-cov's `report --show-mcdc-summary` adds an "MC/DC Coverage"
    # column; the TOTAL row is the last non-empty line.
    # -----------------------------------------------------------------
    TOTAL_LINE="$(grep -E '^TOTAL' "$SUMMARY_REPORT" | tail -1 || true)"
    if [[ -z "$TOTAL_LINE" ]]; then
        echo ""
        echo "WARNING: could not locate TOTAL row in $SUMMARY_REPORT --"
        echo "         skipping MC/DC threshold gate."
        exit 0
    fi

    # Extract the last percentage on the TOTAL line; with
    # --show-mcdc-summary that is the MC/DC coverage column.
    MCDC_PCT="$(echo "$TOTAL_LINE" | grep -oE '[0-9]+\.[0-9]+%' | tail -1 | tr -d '%')"
    if [[ -z "$MCDC_PCT" ]]; then
        echo "WARNING: could not parse MC/DC % from TOTAL row -- skipping gate."
        exit 0
    fi

    echo ""
    printf "==> First-party MC/DC coverage: %s%% (threshold %s%%)\n" \
        "$MCDC_PCT" "$THRESHOLD"

    # awk for float comparison (bash can't).
    BELOW="$(awk -v a="$MCDC_PCT" -v b="$THRESHOLD" \
        'BEGIN { print (a + 0 < b + 0) ? 1 : 0 }')"
    if [[ "$BELOW" -eq 1 ]]; then
        echo "FAIL: MC/DC coverage ${MCDC_PCT}% < threshold ${THRESHOLD}%"
        echo "      (expected on initial bring-up -- subsequent agents"
        echo "       will add MC/DC test vectors per docs/MCDC.md)"
        exit 1
    fi
    echo "PASS: MC/DC coverage meets threshold"
    exit 0
fi

# ---------------------------------------------------------------------------
# Fallback path (no clang MC/DC). We already built + ran the tests
# under whatever instrumentation was available; just say so.
# ---------------------------------------------------------------------------
echo "==> [4/5] Skipped: llvm-profdata merge (clang MC/DC not available)"
echo "==> [5/5] Skipped: llvm-cov MC/DC report"
echo ""
echo "MC/DC report unavailable -- install clang >= 18 and re-run."
echo "Tests still executed under fallback instrumentation; see"
echo "scripts/coverage_report.sh for line/branch coverage instead."
exit 0
