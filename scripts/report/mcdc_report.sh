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
# This is opt-in; the existing `make test` and `make coverage` flow
# is untouched. Invoke via `make mcdc` or directly:
#
#   bash scripts/report/mcdc_report.sh
#
# Environment overrides:
#   CC, CXX                 -- compilers (default: clang / clang++ from $PATH)
#   LLVM_PROFDATA, LLVM_COV -- LLVM tools (default: probe alongside $CC)
#   RA8_MCDC_BUILD_DIR       -- build dir (default: build/host-mcdc)
#   RA8_MCDC_REPORT_DIR      -- report dir (default: build/mcdc-report)
#   RA8_MCDC_THRESHOLD       -- minimum percent (default: 100)
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

BUILD_DIR="${RA8_MCDC_BUILD_DIR:-$REPO_ROOT/tests/build-cov}"
REPORT_DIR="${RA8_MCDC_REPORT_DIR:-$REPO_ROOT/build/mcdc-report}"
THRESHOLD="${RA8_MCDC_THRESHOLD:-100}"

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
  for _tool in cmake sed; do
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

  _st_tmp="$(mktemp -d "${TMPDIR:-/tmp}/ra8-mcdc-selftest.XXXXXXXX")"
  # shellcheck disable=SC2064  # expand $_st_tmp now: the trap must not depend
  # on the variable still being set when it fires.
  trap "rm -rf '$_st_tmp'" EXIT

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
      -DRA8_MCDC=ON -Wno-dev >"$_st_tmp/cfg.log" 2>&1; then
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
# scripts/report/coverage_report_host.sh does. Pass --in-container to skip this.
# ---------------------------------------------------------------------------
if [[ "$(uname -s)" == "Darwin" && "${1:-}" != "--in-container" ]]; then
  if ! command -v docker >/dev/null 2>&1; then
    echo "error: docker not on PATH (need it on macOS for MC/DC)" >&2
    exit 1
  fi
  if command -v colima >/dev/null 2>&1; then
    if ! colima status >/dev/null 2>&1; then
      echo "==> Starting colima VM"
      colima start --cpu 4 --memory 6
    fi
  fi
  if ! docker info >/dev/null 2>&1; then
    echo "error: docker daemon not reachable" >&2
    exit 1
  fi

  IMAGE_TAG="ra8-firmware-test:latest"
  if ! docker image inspect "$IMAGE_TAG" >/dev/null 2>&1; then
    echo "==> Building $IMAGE_TAG"
    docker build -t "$IMAGE_TAG" \
      -f "$REPO_ROOT/.devcontainer/Dockerfile" \
      "$REPO_ROOT/.devcontainer"
  fi

  docker run --rm \
    -v "$REPO_ROOT":/work \
    -w /work \
    -e RA8_MCDC_THRESHOLD \
    "$IMAGE_TAG" \
    bash -lc '
            set -e
            if [[ $EUID -eq 0 ]]; then SUDO=""; else SUDO="sudo"; fi
            # Required pieces for clang MC/DC: the compiler itself,
            # llvm-profdata + llvm-cov for reporting, and compiler-rt
            # which ships libclang_rt.profile-<arch>.a (otherwise the
            # link step can not resolve __llvm_profile_*).
            need_install=0
            command -v clang-18 >/dev/null 2>&1   || need_install=1
            command -v llvm-profdata-18 >/dev/null 2>&1 \
                || command -v llvm-profdata >/dev/null 2>&1 || need_install=1
            command -v llvm-cov-18 >/dev/null 2>&1 \
                || command -v llvm-cov >/dev/null 2>&1 || need_install=1
            ARCH=$(uname -m)
            RT_LIB="/usr/lib/llvm-18/lib/clang/18/lib/linux/libclang_rt.profile-${ARCH}.a"
            [[ -f "$RT_LIB" ]] || need_install=1
            if [[ $need_install -eq 1 ]]; then
                echo "==> Installing clang-18 + llvm-18 + libclang-rt-18-dev"
                $SUDO apt-get update -qq
                $SUDO env DEBIAN_FRONTEND=noninteractive \
                    apt-get install -y -qq --no-install-recommends \
                    clang-18 llvm-18 libclang-rt-18-dev
            fi
            export CC="${CC:-$(command -v clang-18 || command -v clang)}"
            export CXX="${CXX:-$(command -v clang++-18 || command -v clang++)}"
            bash scripts/report/mcdc_report.sh --in-container
        '
  exit $?
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
# scripts/checks/coverage.sh has carried this exact guard for the same exact reason;
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
  -Wno-dev >/dev/null

# ---------------------------------------------------------------------------
# 2. Build
# ---------------------------------------------------------------------------
echo "==> [2/5] Building host tests"
# -j2 + --keep-going: full -j parallel link can OOM the devcontainer
# (libclang_rt.profile is heavy); --keep-going lets us still produce a
# report even if one or two targets fail to link.
cmake --build "$BUILD_DIR" -j2 -- --keep-going 2>&1 | tail -40 || {
  echo "(some targets failed; continuing with whatever built)" >&2
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
  mcdc_ignore_re='(third_party|/tests/|/usr/|c\+\+/v[0-9]+|ra8_display_pal_host_macos)'

  # Per-file MC/DC dump (verbose, for human inspection).
  "$LLVM_COV_BIN" show \
    "${OBJ_ARGS[@]}" \
    -instr-profile="$MERGED_PROFDATA" \
    -show-mcdc \
    -show-mcdc-summary \
    -format=text \
    -ignore-filename-regex="$mcdc_ignore_re" \
    >"$TEXT_REPORT" 2>/dev/null || true

  # Per-file numeric summary (machine-parseable).
  "$LLVM_COV_BIN" report \
    "${OBJ_ARGS[@]}" \
    -instr-profile="$MERGED_PROFDATA" \
    -show-mcdc-summary \
    -ignore-filename-regex="$mcdc_ignore_re" \
    >"$SUMMARY_REPORT" 2>/dev/null || true

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
  printf "==> First-party absolute MC/DC coverage: %s%% (informational)\n" \
    "$MCDC_PCT"

  # ------------------------------------------------------------
  # Reachable-only gate (DO-178C 6.4.4.3 -- deactivated code).
  # Regenerate the gap classifier and read the reachable rate.
  # The gate fails when reachable_rate < THRESHOLD; documented-
  # deactivated conditions are excluded from the denominator.
  # ------------------------------------------------------------
  if command -v python3 >/dev/null 2>&1; then
    python3 "$REPO_ROOT/scripts/fix/regen_mcdc_gaps.py" || true
  fi

  # ------------------------------------------------------------
  # Per-file MC/DC FLOOR (no allowlist). The reachable-rate gate
  # below is an aggregate that the CI job neutralizes with
  # RA8_MCDC_THRESHOLD=0; this floor runs UNCONDITIONALLY and fails
  # the report if ANY first-party file drops below the per-file
  # reachable-MC/DC bar -- a single rotted file can no longer hide
  # behind well-covered siblings. Mirrors scripts/checks/coverage.sh's
  # check_coverage_floor.py wiring. Reads the mcdc_per_file.json the
  # regenerator just wrote.
  # ------------------------------------------------------------
  if command -v python3 >/dev/null 2>&1; then
    echo ""
    if ! python3 "$REPO_ROOT/scripts/checks/check_mcdc_floor.py"; then
      echo "FAIL: per-file MC/DC floor failed (offenders above)."
      exit 1
    fi
  fi

  GATE_JSON="$REPORT_DIR/gate.json"
  REACHABLE_PCT=""
  if [[ -f "$GATE_JSON" ]] && command -v python3 >/dev/null 2>&1; then
    REACHABLE_PCT="$(python3 -c "
import json,sys
d=json.load(open('$GATE_JSON'))
print(f\"{d['reachable_rate']:.4f}\")
" 2>/dev/null || true)"
  fi

  if [[ -z "$REACHABLE_PCT" ]]; then
    echo "WARNING: gate.json missing or unreadable -- falling back to absolute %." >&2
    REACHABLE_PCT="$MCDC_PCT"
  fi

  printf "==> First-party reachable MC/DC coverage: %s%% (threshold %s%%)\n" \
    "$REACHABLE_PCT" "$THRESHOLD"
  echo "    (Deactivated conditions excluded per DO-178C 6.4.4.3;"
  echo "     see docs/MCDC_DEACTIVATIONS.md.)"

  BELOW="$(awk -v a="$REACHABLE_PCT" -v b="$THRESHOLD" \
    'BEGIN { print (a + 0 < b + 0) ? 1 : 0 }')"
  if [[ "$BELOW" -eq 1 ]]; then
    echo "FAIL: reachable MC/DC ${REACHABLE_PCT}% < threshold ${THRESHOLD}%"
    echo "      Add MC/DC vectors per docs/MCDC.md, or document the"
    echo "      gap as deactivated in docs/MCDC_DEACTIVATIONS.md."
    exit 1
  fi
  echo "PASS: reachable MC/DC meets threshold"
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
echo "scripts/report/coverage_report_host.sh for line/branch coverage instead."
exit 0
