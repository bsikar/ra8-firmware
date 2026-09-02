#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# scripts/report/mcdc_report.sh -- DO-178C Level B MC/DC report.
#
# Modified Condition/Decision Coverage (MC/DC) is the structural
# coverage criterion mandated by DO-178C Level B for compound boolean
# decisions. The only open-source path to true MC/DC is clang >= 18
# with `-fcoverage-mcdc` paired with `-fcoverage-mapping` and
# `-fprofile-instr-generate`, then reported via
# `llvm-cov show --show-mcdc-summary`.
#
# Pipeline:
#   1. Configure tests/ with RA8_MCDC=ON. The CMake probe selects
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
# This is opt-in; the existing `just quality::local::test` and `just quality::local::mcdc` flow
# is untouched. Invoke via `just quality::local::mcdc` or directly:
#
#   bash scripts/report/mcdc_report.sh
#
# Environment overrides:
#   CC, CXX                 -- compilers (default: clang / clang++ from $PATH)
#   LLVM_PROFDATA, LLVM_COV -- LLVM tools (default: probe alongside $CC)
#   RA8_MCDC_BUILD_DIR       -- build dir (default: build/host-mcdc)
#   RA8_MCDC_REPORT_DIR      -- report dir (default: build/mcdc-report)
#   RA8_MCDC_THRESHOLD       -- minimum percent (default: 100)
#   RA8_MCDC_BUILD_JOBS      -- build concurrency, 1 or 2 (default: 2)
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

BUILD_DIR="${RA8_MCDC_BUILD_DIR:-$REPO_ROOT/tests/build-cov}"
REPORT_DIR="${RA8_MCDC_REPORT_DIR:-$REPO_ROOT/build/mcdc-report}"
THRESHOLD="${RA8_MCDC_THRESHOLD:-100}"
BUILD_JOBS="${RA8_MCDC_BUILD_JOBS:-2}"
MCDC_CONTAINER_ENV=(
  "RA8_MCDC_THRESHOLD=$THRESHOLD"
  "RA8_MCDC_BUILD_JOBS=$BUILD_JOBS"
)

mcdc_valid_build_jobs() {
  [[ "$1" =~ ^[12]$ ]]
}

mcdc_all_tests_passed() {
  local passed="$1" failed="$2" total="$3"
  [[ "$passed" =~ ^[0-9]+$ && "$failed" =~ ^[0-9]+$ && "$total" =~ ^[0-9]+$ ]] ||
    return 1
  ((total > 0 && passed + failed == total && failed == 0))
}

mcdc_llvm_tools_available() {
  [[ -n "$1" && -x "$1" && -n "$2" && -x "$2" ]]
}

mcdc_read_reachable_decision_pct() {
  python3 - "$1" <<'PY'
import json
import math
import sys
from pathlib import Path

path = Path(sys.argv[1])
try:
    data = json.loads(path.read_text(encoding="utf-8"))
    value = float(data["reachable_decision_complete_rate"])
except (OSError, ValueError, TypeError, KeyError, json.JSONDecodeError) as exc:
    raise SystemExit(
        f"cannot read reachable_decision_complete_rate from {path}: {exc}"
    ) from exc
if not math.isfinite(value) or not 0.0 <= value <= 100.0:
    raise SystemExit(
        f"reachable_decision_complete_rate out of range in {path}: {value}"
    )
print(f"{value:.4f}")
PY
}

# Preserve the last diagnostic lines while returning the producer's real
# status. A plain `producer | tail` under pipefail was previously followed by
# `|| continue`, which let a partial test-binary universe reach llvm-cov.
mcdc_run_and_tail() {
  local -a pipeline_status=()
  set +e
  "$@" 2>&1 | tail -40
  pipeline_status=("${PIPESTATUS[@]}")
  set -e
  if [[ "${pipeline_status[0]}" -ne 0 ]]; then
    echo "FAIL: command exited ${pipeline_status[0]} before the MC/DC matrix completed." >&2
    return 1
  fi
  if [[ "${pipeline_status[1]}" -ne 0 ]]; then
    echo "FAIL: diagnostic tail exited ${pipeline_status[1]}." >&2
    return 1
  fi
}

if ! mcdc_valid_build_jobs "$BUILD_JOBS"; then
  echo "ERROR: RA8_MCDC_BUILD_JOBS must be 1 or 2 (got '$BUILD_JOBS')." >&2
  exit 2
fi

# ---------------------------------------------------------------------------
# The #346 guard, as a function so --selftest can drive the REAL thing rather
# than a lookalike of it.
#
# Wipes $1 when it was configured by a compiler other than $2. See the call
# site at step [1/5] for why a stale CMake cache is not merely inefficient
# here but silently produces an uninstrumented build.
# ---------------------------------------------------------------------------
mcdc_purge_stale_build_dir() {
  local build_dir="$1" want_cc="$2" cached_cc
  [[ -f "$build_dir/CMakeCache.txt" ]] || return 0
  cached_cc="$(sed -n 's/^CMAKE_C_COMPILER:[^=]*=//p' "$build_dir/CMakeCache.txt")"
  # Compare RESOLVED paths on both sides. CMake always caches a full path,
  # while callers pass whatever was in $CC -- gate_mcdc passes the bare name
  # "clang-18". A raw string compare therefore reports "compiler changed" on
  # every single run and wipes a perfectly good tree each time, which is the
  # over-firing half of this guard and just as wrong as never firing.
  want_cc="$(command -v "$want_cc" 2>/dev/null || printf '%s' "$want_cc")"
  cached_cc="$(command -v "$cached_cc" 2>/dev/null || printf '%s' "$cached_cc")"
  if [[ "$cached_cc" != "$want_cc" ]]; then
    echo "==> build dir was configured with '$cached_cc', now using"
    echo "    '$want_cc' -- wiping $build_dir so the cached"
    echo "    -fcoverage-mcdc probe cannot survive the compiler change."
    rm -rf "$build_dir"
  fi
}

# ---------------------------------------------------------------------------
# --selftest: prove the guard above still works, in BOTH directions, plus a
# dependency probe. Wired into gate_mcdc AHEAD of the real run.
#
# #346 was a guard that did not exist; the class of bug is a guard that stops
# matching and keeps printing green. Asserting the property beats remembering
# it, so this runs every time the gate does.
# ---------------------------------------------------------------------------
if [[ "${1:-}" == "--selftest" ]]; then
  echo "==> mcdc_report.sh self-test"

  # Dependency probe. A gate whose tool is absent must FAIL, never skip.
  _st_fail=0
  for _tool in awk cmake du python3 sed; do
    command -v "$_tool" >/dev/null 2>&1 || {
      echo "ERROR: required tool '$_tool' is not on PATH." >&2
      _st_fail=1
    }
  done
  _st_cc="${CC:-$(command -v clang-18 2>/dev/null || command -v clang 2>/dev/null || true)}"
  if [[ -z "$_st_cc" ]]; then
    echo "ERROR: no clang on PATH; MC/DC cannot be measured at all." >&2
    _st_fail=1
  elif ! echo 'int main(void){return 0;}' |
    "$_st_cc" -x c - -fprofile-instr-generate -fcoverage-mapping \
      -fcoverage-mcdc -o /dev/null 2>/dev/null; then
    echo "ERROR: $_st_cc does not accept the -fcoverage-mcdc flag trio." >&2
    _st_fail=1
  fi
  [[ "$_st_fail" -eq 0 ]] || exit 1
  echo "    dependency probe OK ($_st_cc accepts -fcoverage-mcdc)"

  if ! mcdc_valid_build_jobs 1 || ! mcdc_valid_build_jobs 2 ||
    mcdc_valid_build_jobs 0 || mcdc_valid_build_jobs 3; then
    echo "ERROR: MC/DC build-job ceiling self-test FAILED." >&2
    exit 1
  fi
  echo "    build-job ceiling OK (1/2 accepted; 0/3 refused)"

  if [[ "${MCDC_CONTAINER_ENV[0]}" != "RA8_MCDC_THRESHOLD=$THRESHOLD" ||
    "${MCDC_CONTAINER_ENV[1]}" != "RA8_MCDC_BUILD_JOBS=$BUILD_JOBS" ]]; then
    echo "ERROR: devcontainer MC/DC environment forwarding self-test FAILED." >&2
    exit 1
  fi
  echo "    devcontainer environment OK (threshold and build jobs forwarded)"

  if ! mcdc_run_and_tail bash -c 'printf "complete-build\n"; exit 0' >/dev/null; then
    echo "ERROR: successful build pipeline self-test FAILED." >&2
    exit 1
  fi
  if mcdc_run_and_tail bash -c 'printf "partial-build\n"; exit 7' \
    >/dev/null 2>&1; then
    echo "ERROR: failed build pipeline was allowed to continue." >&2
    exit 1
  fi
  echo "    build pipeline OK (complete accepted; partial refused)"

  if ! mcdc_all_tests_passed 4 0 4 || mcdc_all_tests_passed 3 1 4 ||
    mcdc_all_tests_passed 3 0 4 || mcdc_all_tests_passed 0 0 0; then
    echo "ERROR: MC/DC test-result verdict self-test FAILED." >&2
    exit 1
  fi
  echo "    test verdict OK (complete pass accepted; failure/mismatch/zero refused)"

  _st_tmp="$(mktemp -d "${TMPDIR:-/tmp}/ra8-mcdc-selftest.XXXXXXXX")"
  trap 'rm -rf -- "$_st_tmp"' EXIT

  printf '{"reachable_decision_complete_rate":89.72}\n' >"$_st_tmp/gate.json"
  if [[ "$(mcdc_read_reachable_decision_pct "$_st_tmp/gate.json")" != "89.7200" ]]; then
    echo "ERROR: valid reachable-rate artifact was rejected." >&2
    exit 1
  fi
  printf '{"not_reachable_decision_complete_rate":89.72}\n' >"$_st_tmp/bad-gate.json"
  if mcdc_read_reachable_decision_pct "$_st_tmp/bad-gate.json" >/dev/null 2>&1 ||
    mcdc_read_reachable_decision_pct "$_st_tmp/missing-gate.json" >/dev/null 2>&1; then
    echo "ERROR: invalid/missing reachable-rate artifact was accepted." >&2
    exit 1
  fi
  echo "    reachable artifact OK (valid accepted; invalid/missing refused)"

  if ! mcdc_llvm_tools_available /bin/sh /bin/sh ||
    mcdc_llvm_tools_available "$_st_tmp/missing-profdata" /bin/sh ||
    mcdc_llvm_tools_available /bin/sh "$_st_tmp/missing-cov"; then
    echo "ERROR: LLVM coverage-tool requirement self-test FAILED." >&2
    exit 1
  fi
  echo "    LLVM tool requirement OK (present accepted; either missing refused)"

  # Direction 1 -- FIRES: a cache written by a different compiler is wiped.
  # This is exactly the suite's shape: coverage configures with gcc, mcdc
  # then wants clang.
  mkdir -p "$_st_tmp/stale"
  printf 'CMAKE_C_COMPILER:FILEPATH=/usr/bin/gcc-13\n' \
    >"$_st_tmp/stale/CMakeCache.txt"
  mcdc_purge_stale_build_dir "$_st_tmp/stale" /usr/bin/clang-18 >/dev/null
  if [[ -f "$_st_tmp/stale/CMakeCache.txt" ]]; then
    echo "ERROR: mcdc self-test FAILED (direction 1)." >&2
    echo "       A build dir cached by gcc-13 SURVIVED a reconfigure with" >&2
    echo "       clang-18. The #346 guard has stopped firing: the cached" >&2
    echo "       RA8_HAVE_CLANG_MCDC=no would be reused, the build would" >&2
    echo "       come out uninstrumented, and the gate would die at the" >&2
    echo "       merge step blaming the tests." >&2
    exit 1
  fi
  echo "    direction 1 OK (stale gcc-13 cache wiped for clang-18)"

  # Direction 2 -- STAYS SILENT: same compiler, cache preserved. The
  # legal-but-tricky case. A guard that wipes unconditionally would pass
  # direction 1 while throwing away every incremental build, so assert the
  # negative too.
  mkdir -p "$_st_tmp/fresh"
  printf 'CMAKE_C_COMPILER:FILEPATH=/usr/bin/clang-18\n' \
    >"$_st_tmp/fresh/CMakeCache.txt"
  printf 'artifact\n' >"$_st_tmp/fresh/keepme"
  mcdc_purge_stale_build_dir "$_st_tmp/fresh" /usr/bin/clang-18 >/dev/null
  if [[ ! -f "$_st_tmp/fresh/keepme" ]]; then
    echo "ERROR: mcdc self-test FAILED (direction 2)." >&2
    echo "       A build dir cached by the SAME compiler was wiped. The" >&2
    echo "       guard is over-firing and discards every incremental build." >&2
    exit 1
  fi
  echo "    direction 2 OK (matching clang-18 cache preserved)"

  # Direction 2b -- the SAME compiler spelled two different ways. CMake caches
  # a resolved full path; gate_mcdc passes the bare name "clang-18". A string
  # compare calls those different and wipes the tree on EVERY run. This case
  # is not hypothetical -- the first cut of this guard did exactly that, and
  # direction 2 above missed it because both sides were full paths.
  _st_bare="$(basename "$_st_cc")"
  _st_resolved="$(command -v "$_st_bare" 2>/dev/null || printf '%s' "$_st_bare")"
  mkdir -p "$_st_tmp/spelling"
  printf 'CMAKE_C_COMPILER:FILEPATH=%s\n' "$_st_resolved" \
    >"$_st_tmp/spelling/CMakeCache.txt"
  printf 'artifact\n' >"$_st_tmp/spelling/keepme"
  mcdc_purge_stale_build_dir "$_st_tmp/spelling" "$_st_bare" >/dev/null
  if [[ ! -f "$_st_tmp/spelling/keepme" ]]; then
    echo "ERROR: mcdc self-test FAILED (direction 2b)." >&2
    echo "       Cache holds '$_st_resolved' and the caller passed the" >&2
    echo "       equivalent bare name '$_st_bare', yet the tree was wiped." >&2
    echo "       The guard compares unresolved strings, so it fires on every" >&2
    echo "       run and no incremental build ever survives." >&2
    exit 1
  fi
  echo "    direction 2b OK ('$_st_bare' == '$_st_resolved', cache preserved)"

  # Direction 3 -- the CMake layer: RA8_MCDC=ON with a compiler that has no
  # -fcoverage-mcdc must FAIL AT CONFIGURE, not degrade to plain gcov. This
  # is the half of #346 that turned a configure problem into a merge-step
  # red herring. Only assert it when a suitable gcc is actually present.
  _st_gcc="$(command -v gcc-13 2>/dev/null || command -v gcc 2>/dev/null || true)"
  if [[ -n "$_st_gcc" ]] && ! echo 'int main(void){return 0;}' |
    "$_st_gcc" -x c - -fcoverage-mcdc -o /dev/null 2>/dev/null; then
    if cmake -B "$_st_tmp/cfg" -S "$REPO_ROOT/tests" \
      -DCMAKE_C_COMPILER="$_st_gcc" \
      -DRA8_MCDC=ON >"$_st_tmp/cfg.log" 2>&1; then
      echo "ERROR: mcdc self-test FAILED (direction 3)." >&2
      echo "       tests/ configured RA8_MCDC=ON with $_st_gcc, which has" >&2
      echo "       no -fcoverage-mcdc, and SUCCEEDED. It must fail at" >&2
      echo "       configure instead of silently building uninstrumented." >&2
      exit 1
    fi
    echo "    direction 3 OK (RA8_MCDC=ON + non-MC/DC compiler fails configure)"
  fi

  echo "==> mcdc_report.sh self-test PASSED"
  exit 0
fi

# ---------------------------------------------------------------------------
# macOS shim: the host test harness uses MAP_FIXED below 4 GiB for the
# fake MMIO region; macOS arm64 SIGKILLs the process. Re-exec
# inside the project's Linux devcontainer just like
# scripts/report/tree_coverage.sh does. Pass --in-container to skip this.
# ---------------------------------------------------------------------------
if [[ "$(uname -s)" == "Darwin" && "${1:-}" != "--in-container" ]]; then
  exec bash "$REPO_ROOT/scripts/ci/devcontainer_run.sh" -- \
    env "${MCDC_CONTAINER_ENV[@]}" CC=clang-18 CXX=clang++-18 \
    bash scripts/report/mcdc_report.sh --in-container
fi

# Strip the in-container marker so it never reaches argument parsing
# below.
if [[ "${1:-}" == "--in-container" ]]; then
  shift
fi

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
# If $CC_BIN looks like clang-18 / clang-20 etc., look for the
# matching llvm-profdata-NN / llvm-cov-NN that Debian-style packaging
# installs.
CC_BASENAME="$(basename "$CC_BIN")"
CC_VERSUFFIX=""
if [[ "$CC_BASENAME" =~ -([0-9]+)$ ]]; then
  CC_VERSUFFIX="-${BASH_REMATCH[1]}"
fi
if [[ -z "$LLVM_PROFDATA_BIN" ]]; then
  for cand in "$CC_DIR/llvm-profdata" "$CC_DIR/llvm-profdata${CC_VERSUFFIX}" \
    "/usr/bin/llvm-profdata${CC_VERSUFFIX}" "/usr/bin/llvm-profdata"; do
    if [[ -x "$cand" ]]; then
      LLVM_PROFDATA_BIN="$cand"
      break
    fi
  done
  [[ -z "$LLVM_PROFDATA_BIN" ]] &&
    LLVM_PROFDATA_BIN="$(command -v "llvm-profdata${CC_VERSUFFIX}" 2>/dev/null ||
      command -v llvm-profdata 2>/dev/null || true)"
fi
if [[ -z "$LLVM_COV_BIN" ]]; then
  for cand in "$CC_DIR/llvm-cov" "$CC_DIR/llvm-cov${CC_VERSUFFIX}" \
    "/usr/bin/llvm-cov${CC_VERSUFFIX}" "/usr/bin/llvm-cov"; do
    if [[ -x "$cand" ]]; then
      LLVM_COV_BIN="$cand"
      break
    fi
  done
  [[ -z "$LLVM_COV_BIN" ]] &&
    LLVM_COV_BIN="$(command -v "llvm-cov${CC_VERSUFFIX}" 2>/dev/null ||
      command -v llvm-cov 2>/dev/null || true)"
fi

echo "==> ra8-firmware MC/DC report"
echo "    CC            = $CC_BIN"
echo "    CXX           = $CXX_BIN"
echo "    llvm-profdata = ${LLVM_PROFDATA_BIN:-<missing>}"
echo "    llvm-cov      = ${LLVM_COV_BIN:-<missing>}"
echo "    MC/DC support = $([[ $HAVE_MCDC -eq 1 ]] && echo yes || echo NO -- fallback only)"
echo "    build dir     = $BUILD_DIR"
echo "    report dir    = $REPORT_DIR"
echo "    threshold     = ${THRESHOLD}%"
echo "    build jobs    = $BUILD_JOBS (hard maximum: 2)"
echo ""

if [[ $HAVE_MCDC -eq 0 ]]; then
  # Stop here rather than configure a build that cannot measure MC/DC. The
  # configure step would now FATAL_ERROR anyway (tests/CMakeLists.txt), but
  # saying it plainly at the point the compiler was chosen beats a CMake
  # backtrace, and this script's own probe is what already knows the answer.
  echo "ERROR: $CC_BIN does NOT support -fcoverage-mcdc." >&2
  echo "       DO-178C Level B MC/DC cannot be measured with this compiler," >&2
  echo "       so this run stops instead of producing a report that looks" >&2
  echo "       like MC/DC and is not." >&2
  echo "       Install clang >= 18 (brew install llvm / apt install clang-18)." >&2
  echo "       To explore plain decision/condition coverage on a gcc-only" >&2
  echo "       box, configure tests/ by hand with -DRA8_MCDC_ALLOW_FALLBACK=ON." >&2
  exit 1
fi

if ! mcdc_llvm_tools_available "$LLVM_PROFDATA_BIN" "$LLVM_COV_BIN"; then
  echo "ERROR: matching llvm-profdata and llvm-cov executables are required." >&2
  echo "       llvm-profdata = ${LLVM_PROFDATA_BIN:-<missing>}" >&2
  echo "       llvm-cov      = ${LLVM_COV_BIN:-<missing>}" >&2
  exit 1
fi

# ---------------------------------------------------------------------------
# 1. Configure
#
# Wipe the tree when a PREVIOUS configure pinned a different compiler (#346).
#
# tests/CMakeLists.txt probes -fcoverage-mcdc with check_c_compiler_flag, whose
# answer lands in the CACHED variable RA8_HAVE_CLANG_MCDC. The default build
# dir is tests/build-cov, which the coverage gate configures FIRST in the same
# suite -- with gcc-13, which has no -fcoverage-mcdc, so the probe caches "no".
# Re-configuring here with clang-18 makes CMake notice the compiler changed and
# re-run, but the cached probe answer is REUSED: the fallback branch is taken,
# the build gets plain --coverage instead of the clang MC/DC trio, and no
# .profraw can ever be emitted. The gate then died four steps later at the
# merge, blaming the tests for crashing when they had all passed.
#
# scripts/report/tree_coverage.sh carries this exact guard for the same reason;
# this script simply never got it.
# ---------------------------------------------------------------------------
mcdc_purge_stale_build_dir "$BUILD_DIR" "$CC_BIN"

echo "==> [1/5] Configuring tests/ with RA8_MCDC=ON"
cmake -B "$BUILD_DIR" -S "$REPO_ROOT/tests" \
  -DCMAKE_C_COMPILER="$CC_BIN" \
  -DCMAKE_CXX_COMPILER="$CXX_BIN" \
  -DCMAKE_BUILD_TYPE=Debug \
  -DRA8_MCDC=ON \
  -DRA8_COVERAGE=OFF \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  >/dev/null

# ---------------------------------------------------------------------------
# 2. Build
# ---------------------------------------------------------------------------
echo "==> [2/5] Building host tests"
# MC/DC deliberately compiles with -g0: LLVM coverage maps, not DWARF, carry
# the source/decision locations consumed below. Keeping DWARF in every test
# executable previously pushed this complete build past 8 GiB without adding
# any report evidence. The job ceiling also bounds peak linker memory. Neither
# measure removes a source, target, test, profile, or llvm-cov object.
# --keep-going diagnoses all link failures in one run; the captured producer
# status still fails closed, before any tests or coverage tools can observe a
# partial binary universe.
if ! mcdc_run_and_tail cmake --build "$BUILD_DIR" -j"$BUILD_JOBS" -- --keep-going; then
  echo "FAIL: the complete MC/DC test-binary matrix did not build." >&2
  exit 1
fi

BUILD_KIB="$(du -sk "$BUILD_DIR" | awk '{print $1}')"
printf '    build footprint = %s KiB (complete instrumented matrix)\n' "$BUILD_KIB"

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
if ! mcdc_all_tests_passed "$PASS" "$FAIL" "${#TEST_BINARIES[@]}"; then
  echo "FAIL: the complete MC/DC test matrix did not pass; refusing a partial report." >&2
  exit 1
fi

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
    # Do NOT blame the tests here. When they all passed (the usual case) the
    # binaries plainly ran, and a passing binary that emits no .profraw was
    # never instrumented in the first place -- the failure is at CONFIGURE
    # time, several steps back. The original wording ("did the test binaries
    # crash?") sent #346 four steps away from its cause.
    echo "ERROR: no .profraw files were emitted." >&2
    echo "       The tests are not the problem: an instrumented binary" >&2
    echo "       writes its profile on exit, so zero .profraw means the" >&2
    echo "       build was never instrumented with -fprofile-instr-generate." >&2
    echo "       Check the [1/5] configure step above -- if it reported" >&2
    echo "       'Falling back to plain gcov', the cached -fcoverage-mcdc" >&2
    echo "       probe in $BUILD_DIR was answered by a" >&2
    echo "       different compiler (#346). Remove that directory and re-run." >&2
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

  # Files excluded from the MC/DC denominator: vendored trees, the host
  # test harness, system / libc++ headers, and the macOS-only host display
  # backend (ra8_display_pal_host_macos*) -- a desktop dev-preview tool, not
  # airborne firmware, so DO-178C MC/DC does not apply to it.
  #
  # ...and mdl -- both its shared core (apps/shared_libs/mdl) and its host
  # form (apps/host/mdl) -- whose per-file line/branch standing is
  # held by the whole-tree `coverage-tree` gate over its own CTest suite, the
  # same gate that holds every other first-party translation unit.
  # Its production sources appear here only as LINK dependencies of the
  # portable-contract tests under tests/ -- each of those drives one contract
  # through the same code the tool build compiles, so the sources cannot be
  # trimmed from those targets (verified: of 62 mdl sources across the
  # five mdl units, exactly one is unreferenced). Instrumenting them from here
  # put 1094 MC/DC conditions into this denominator with 961 never executed,
  # because the suite that does exercise them is a different build this report
  # never runs. origin/dev's report contains zero mdl files; this keeps
  # the report measuring what it is the gate for, and leaves mdl measured
  # where it is actually tested. Unifying the two regimes -- measuring MC/DC
  # from each tool's own suite -- is the real answer and is filed separately.
  mcdc_ignore_re='(third_party|/tests/|/usr/|c\+\+/v[0-9]+|ra8_display_pal_host_macos'
  # Both current build-form directories are named explicitly:
  # the product's core moved to apps/shared_libs/ so a second build form could
  # consume it, and a pattern naming only the form would have put the whole
  # core back into this denominator with its coverage measured elsewhere.
  mcdc_ignore_re+='|apps/(host|shared_libs)/mdl/)'

  # Per-file MC/DC dump (verbose, for human inspection).
  if ! "$LLVM_COV_BIN" show \
    "${OBJ_ARGS[@]}" \
    -instr-profile="$MERGED_PROFDATA" \
    -show-mcdc \
    -show-mcdc-summary \
    -format=text \
    -ignore-filename-regex="$mcdc_ignore_re" \
    >"$TEXT_REPORT" 2>/dev/null; then
    echo "FAIL: llvm-cov could not produce the MC/DC detail report." >&2
    exit 1
  fi

  # Per-file numeric summary (machine-parseable).
  if ! "$LLVM_COV_BIN" report \
    "${OBJ_ARGS[@]}" \
    -instr-profile="$MERGED_PROFDATA" \
    -show-mcdc-summary \
    -ignore-filename-regex="$mcdc_ignore_re" \
    >"$SUMMARY_REPORT" 2>/dev/null; then
    echo "FAIL: llvm-cov could not produce the MC/DC summary report." >&2
    exit 1
  fi

  echo ""
  echo "==> MC/DC summary (tail of $SUMMARY_REPORT):"
  tail -20 "$SUMMARY_REPORT"
  echo ""
  echo "Full per-file MC/DC dump : $TEXT_REPORT"
  echo "Per-file numeric summary : $SUMMARY_REPORT"

  # -----------------------------------------------------------------
  # Gate: scan the TOTAL row of the summary for an MC/DC %.
  # llvm-cov's `report --show-mcdc-summary` adds an "MC/DC Coverage"
  # column; the TOTAL row is the last non-empty line.
  # -----------------------------------------------------------------
  if ! TOTAL_LINE="$(grep -E '^TOTAL' "$SUMMARY_REPORT" | tail -1)" || [[ -z "$TOTAL_LINE" ]]; then
    echo "FAIL: could not locate the TOTAL row in $SUMMARY_REPORT." >&2
    exit 1
  fi

  # Extract the last percentage on the TOTAL line; with
  # --show-mcdc-summary that is the MC/DC coverage column.
  if ! MCDC_PCT="$(echo "$TOTAL_LINE" | grep -oE '[0-9]+\.[0-9]+%' | tail -1 | tr -d '%')" ||
    [[ -z "$MCDC_PCT" ]]; then
    echo "FAIL: could not parse MC/DC % from the TOTAL row." >&2
    exit 1
  fi

  echo ""
  printf "==> First-party absolute MC/DC coverage: %s%% (informational)\n" \
    "$MCDC_PCT"

  # ------------------------------------------------------------
  # Reachable-only gate (DO-178C 6.4.4.3 -- deactivated code).
  # Regenerate the gap classifier and read the reachable
  # decision-complete rate. The gate fails when that rate is below
  # THRESHOLD; documented-deactivated decision regions are excluded
  # from the denominator.
  # ------------------------------------------------------------
  python3 "$REPO_ROOT/scripts/fix/regen_mcdc_gaps.py"

  # ------------------------------------------------------------
  # NON-VACUITY FLOOR on the report itself, before any verdict is
  # drawn from it. Every line above that narrows the denominator --
  # the ignore regex most of all -- is one edit away from narrowing
  # it to nothing, and a report over almost no files produces a
  # SMALLER uncovered count and a HIGHER percentage, i.e. it reads
  # as an improvement. dev measures 513 files and this branch 542,
  # so 300 clears both with margin while a collapsed scan trips it.
  # ------------------------------------------------------------
  measured_files="$(awk "NF>=16 && \$1 ~ /\// {n++} END{print n+0}" "$SUMMARY_REPORT")"
  if [[ "$measured_files" -lt 300 ]]; then
    echo "FAIL: the MC/DC report covers only $measured_files file(s) (floor 300)." >&2
    echo "      The denominator collapsed -- check -ignore-filename-regex and the" >&2
    echo "      test-binary list before believing any percentage from this run." >&2
    exit 1
  fi
  echo "==> MC/DC report covers $measured_files first-party file(s) (floor 300)"

  # ------------------------------------------------------------
  # Per-file MC/DC FLOOR (no allowlist). The reachable-rate gate
  # below is an aggregate that the CI job neutralizes with
  # RA8_MCDC_THRESHOLD=0; this floor runs UNCONDITIONALLY and fails
  # the report if ANY first-party file drops below the per-file
  # reachable-MC/DC bar -- a single rotted file can no longer hide
  # behind well-covered siblings. Mirrors the per-file wiring in
  # scripts/checks/check_tree_coverage.py. Reads the mcdc_per_file.json the
  # regenerator just wrote.
  # ------------------------------------------------------------
  if command -v python3 >/dev/null 2>&1; then
    echo ""
    if ! python3 "$REPO_ROOT/scripts/checks/check_mcdc_floor.py" --selftest; then
      echo "FAIL: per-file MC/DC floor self-test failed."
      exit 1
    fi
    if ! python3 "$REPO_ROOT/scripts/checks/check_mcdc_floor.py"; then
      echo "FAIL: per-file MC/DC floor failed (offenders above)."
      exit 1
    fi
  fi

  GATE_JSON="$REPORT_DIR/gate.json"
  if ! REACHABLE_PCT="$(mcdc_read_reachable_decision_pct "$GATE_JSON")"; then
    echo "FAIL: reachable decision-complete MC/DC classification is" >&2
    echo "      missing or unreadable; refusing to substitute LLVM's" >&2
    echo "      condition-level percentage." >&2
    exit 1
  fi

  printf "==> Reachable decision-complete MC/DC rate: %s%% (threshold %s%%)\n" \
    "$REACHABLE_PCT" "$THRESHOLD"
  echo "    (Deactivated decision regions excluded per DO-178C 6.4.4.3;"
  echo "     see docs/MCDC_DEACTIVATIONS.md.)"

  BELOW="$(awk -v a="$REACHABLE_PCT" -v b="$THRESHOLD" \
    'BEGIN { print (a + 0 < b + 0) ? 1 : 0 }')"
  if [[ "$BELOW" -eq 1 ]]; then
    echo "FAIL: reachable decision-complete MC/DC ${REACHABLE_PCT}% < threshold ${THRESHOLD}%"
    echo "      Add MC/DC vectors per docs/MCDC.md, or document the"
    echo "      gap as deactivated in docs/MCDC_DEACTIVATIONS.md."
    exit 1
  fi
  echo "PASS: reachable decision-complete MC/DC meets threshold"
  exit 0
fi
