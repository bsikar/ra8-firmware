#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# scripts/checks/run_fuzz.sh -- run libFuzzer harnesses for an arbitrary
# wall-clock budget. Configures the fuzz build directory on first use,
# builds the requested target(s), then invokes each with the given
# per-target budget.
#
# Usage:
#
#   bash scripts/checks/run_fuzz.sh <target>            # 60 second run
#   bash scripts/checks/run_fuzz.sh <target> <seconds>  # custom budget
#   bash scripts/checks/run_fuzz.sh --all [seconds]     # every harness
#   bash scripts/checks/run_fuzz.sh --list              # print the registry
#   bash scripts/checks/run_fuzz.sh --selftest          # prove the budget check
#
# Examples:
#
#   bash scripts/checks/run_fuzz.sh fuzz_ra8_jpeg_sw 600
#   bash scripts/checks/run_fuzz.sh --all 300           # nightly-style sweep
#
# The harness registry is RA8_FUZZ_TARGETS in tests/fuzz/CMakeLists.txt --
# the single source of truth. --list parses it and cross-checks it against
# the tests/fuzz/fuzz_ra8_*.c sources, so an unregistered harness (or a
# registry entry whose source was deleted) fails loudly instead of being
# silently skipped. `make fuzz` and .github/workflows/fuzz-nightly.yml both
# consume the registry through this script.
#
# Environment:
#
#   CC / CXX   -- pin a compiler. When unset, the first clang that can link
#                 `-fsanitize=fuzzer` is auto-selected (`clang`, then
#                 versioned majors newest-first). gcc cannot be used:
#                 it has no libFuzzer.
#   FUZZ_RUNS  -- optional libFuzzer -runs=<N> cap, forwarded verbatim.
#                 The `make fuzz` smoke sets this so trivial targets finish
#                 before the wall budget instead of idling. Setting it is also
#                 what makes that early finish legitimate rather than a starved
#                 run -- see fuzz_run_verdict(). It does NOT switch off the
#                 clock-coherence check.
#
# Crash artefacts are written to tests/build-fuzz/crashes/<target>/.
# The script exits non-zero if any requested harness reports a crash;
# with --all every harness still runs so one crash cannot mask another.
#
# THE BUDGET IS VERIFIED, NOT ASSUMED (#509)
#
# libFuzzer enforces -max_total_time off the WALL clock. In the compiler-rt
# sources the clang we pin is built from, compiler-rt/lib/fuzzer/
# FuzzerInternal.h defines
#
#   size_t secondsSinceProcessStartUp() {
#     return duration_cast<seconds>(system_clock::now() - ProcessStartTime)
#         .count();
#   }
#
# -- system_clock, the steppable one, never steady_clock -- and TimedOut()
# compares that value, as an UNSIGNED size_t, against -max_total_time. Step the
# host clock backward mid-run and the subtraction goes negative, the conversion
# makes it enormous, libFuzzer concludes the budget is spent, stops after
# seconds instead of minutes -- and exits 0.
#
# That is not hypothetical. On the win-ci runners a 60 second sweep reported
# "Done 7363327 runs in 251 second(s)" on one harness and a NEGATIVE duration
# on two others, and the job was green with "All fuzz runs passed". A sweep
# that fuzzed for four seconds is otherwise indistinguishable from one that
# fuzzed for ten minutes.
#
# So this script times each harness itself, on a clock that cannot step, and
# refuses to call a starved run a pass. See fuzz_run_verdict(), which is a pure
# function precisely so --selftest can drive every one of its branches -- a
# check nobody has watched fail is a check nobody knows still works.
#
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

# ra8_max_jobs -- the ONE canonical bounded-parallelism width (#328).
# shellcheck source=scripts/ci/lib/parallelism.sh
. "${SCRIPT_DIR}/../ci/lib/parallelism.sh"

build_dir="${ROOT}/tests/build-fuzz"
registry="${ROOT}/tests/fuzz/CMakeLists.txt"

usage() {
  echo "Usage: $0 <target> [seconds]   # run one harness (default 60 s)" >&2
  echo "       $0 --all [seconds]      # run every registered harness" >&2
  echo "       $0 --list               # print the harness registry" >&2
  echo "       $0 --selftest           # prove the budget check, then exit" >&2
  echo "  (registry: RA8_FUZZ_TARGETS in tests/fuzz/CMakeLists.txt)" >&2
}

# How far SHORT of the requested budget a run may finish before it is called
# starved. libFuzzer evaluates TimedOut() between units and stops only once its
# integer second count EXCEEDS the budget, so a healthy run always meets or
# overshoots it. This slack therefore covers process startup and the final
# corpus write -- it is not an allowance for a shortfall.
readonly k_budget_slack_s=5

# Divergence permitted between libFuzzer's self-reported duration and the
# monotonic measurement of the same interval. The two measure the same thing,
# so on a host with a sane clock they agree to within a second; the allowance
# is a fixed floor plus a tenth of the budget so a ten-minute run is not held
# to a tighter absolute bound than a one-minute one. The fault this catches is
# minutes wide (~4 min on the observed hosts), not seconds.
readonly k_clock_tolerance_floor_s=10
readonly k_clock_tolerance_divisor=10

# Milliseconds on CLOCK_MONOTONIC.
#
# Bash's own SECONDS and EPOCHSECONDS are both derived from the wall clock and
# step with it, so neither can be used to detect a clock fault -- they exhibit
# it. /proc/uptime is monotonic but is CLOCK_BOOTTIME, which keeps advancing
# while the machine is suspended; on the very hosts this check exists for (a
# WSL2 VM under a Windows box that sleeps) that would inflate the measurement
# and mask the starvation it is meant to catch. CLOCK_MONOTONIC excludes
# suspended time, so it is the one that answers "how long did the fuzzer
# actually get to run".
monotonic_ms() {
  python3 -c 'import time; print(time.monotonic_ns() // 1000000)'
}

# Fail loudly if there is no monotonic clock to measure with. Not a skip and
# not a fallback to the wall clock: a budget check that silently disappears
# when its dependency is missing is worth less than no check at all, because
# it also removes the reason to look.
require_monotonic_clock() {
  local probe
  if ! command -v python3 >/dev/null 2>&1; then
    echo "ERROR: python3 is required to time the fuzz budget." >&2
    echo "       libFuzzer enforces -max_total_time off the steppable wall" >&2
    echo "       clock, so this script measures the run on CLOCK_MONOTONIC" >&2
    echo "       instead (see the header). Bash offers no monotonic clock." >&2
    return 1
  fi
  # errexit disabled around the CALL, not masked with `||`: masking would leave
  # monotonic_ms running outside a normal errexit context, and the empty probe
  # below is the failure signal anyway.
  set +e
  probe="$(monotonic_ms 2>/dev/null)"
  set -e
  case "${probe}" in
    '' | *[!0-9]*)
      echo "ERROR: python3 is present but time.monotonic_ns() produced" >&2
      echo "       '${probe}' rather than an integer millisecond count." >&2
      return 1
      ;;
  esac
}

# Permitted disagreement, in seconds, between libFuzzer's self-reported
# duration and the monotonic measurement of a $1-second budget. One place, so
# the rule and the message it prints cannot drift apart.
fuzz_clock_tolerance() {
  echo $((k_clock_tolerance_floor_s + $1 / k_clock_tolerance_divisor))
}

# Decide whether one completed harness run is a pass.
#
# Deliberately a pure function of the arguments -- no files, no clock of its
# own, nothing read back from the run -- so --selftest can drive every branch
# in milliseconds instead of by arranging a real clock fault. The diagnosis is
# fuzz_verdict_explain()'s job; this decides, and only decides.
#
#   $1 requested budget, seconds
#   $2 measured elapsed, seconds, from monotonic_ms()
#   $3 libFuzzer's exit status
#   $4 libFuzzer's self-reported duration in seconds, or "" if it printed none
#   $5 the -runs cap in force, or "" when the run was bounded only by time
#
# Prints a one-word verdict; returns 0 for a pass and 1 for a failure.
fuzz_run_verdict() {
  local budget="$1" elapsed="$2" status="$3" reported="$4" runs_cap="$5"
  local diff

  # A crash is a REAL FINDING and stays one. It is the whole point of the
  # sweep, and it legitimately arrives four seconds in -- so it is decided
  # first, before any timing rule could relabel it as a clock fault.
  if [[ "${status}" -ne 0 ]]; then
    echo "crash"
    return 1
  fi

  # -print_final_stats=1 is passed unconditionally and the "Done N runs in T
  # second(s)" line is printed on every non-crash exit path, so its absence
  # means the run did not end the way this script believes it does. Refuse to
  # certify a budget that cannot be read.
  if [[ -z "${reported}" ]]; then
    echo "no-stats"
    return 1
  fi

  # THE clock-fault detector, and it is decided before the early-exit cases
  # below so that neither a -runs cap nor a short run can excuse a stepping
  # clock. Two independent measurements of one interval that disagree by
  # minutes mean the host moved the wall clock underneath the run.
  diff=$((elapsed - reported))
  if [[ "${diff}" -lt 0 ]]; then
    diff=$((-diff))
  fi
  if [[ "${diff}" -gt "$(fuzz_clock_tolerance "${budget}")" ]]; then
    echo "clock-skew"
    return 1
  fi

  # The one documented legitimate early exit: FUZZ_RUNS caps the iteration
  # count, so a trivial target finishes before the wall budget instead of
  # idling. That is what `make fuzz` sets it for, and it is not a shortfall.
  if [[ -n "${runs_cap}" ]]; then
    echo "runs-capped"
    return 0
  fi

  if [[ "${elapsed}" -lt $((budget - k_budget_slack_s)) ]]; then
    echo "short-run"
    return 1
  fi

  echo "ok"
  return 0
}

# Say, on stderr, what a failing verdict means and what to do about it.
#
#   $1 verdict token   $2 target   $3 budget   $4 elapsed   $5 reported
fuzz_verdict_explain() {
  local verdict="$1" target="$2" budget="$3" elapsed="$4" reported="$5"
  case "${verdict}" in
    crash)
      echo "FUZZ FAIL: ${target} exited non-zero after ${elapsed}s -- libFuzzer reported a finding." >&2
      echo "           Reproducer in ${build_dir}/crashes/${target}/." >&2
      ;;
    no-stats)
      echo "FUZZ FAIL: ${target} exited 0 without printing a 'Done N runs in T second(s)'" >&2
      echo "           line, so its budget cannot be verified. Either libFuzzer's" >&2
      echo "           reporting changed or the harness exited through a path this" >&2
      echo "           script does not model." >&2
      ;;
    clock-skew)
      echo "FUZZ FAIL: ${target} ran ${elapsed}s by CLOCK_MONOTONIC but libFuzzer reported" >&2
      echo "           ${reported}s for the same interval (tolerance $(fuzz_clock_tolerance "${budget}")s)." >&2
      echo "           The HOST WALL CLOCK STEPPED during the run. libFuzzer derives" >&2
      echo "           -max_total_time from system_clock, so the ${budget}s budget was" >&2
      echo "           neither honoured nor measurable and this sweep proves nothing." >&2
      echo "           Fix the runner's clock discipline (slew, not step) -- see #509." >&2
      ;;
    short-run)
      echo "FUZZ FAIL: ${target} was given ${budget}s but ran only ${elapsed}s, with no" >&2
      echo "           -runs cap to explain it and no crash reported. The budget was" >&2
      echo "           not spent, so a clean result here means nothing." >&2
      ;;
    *) ;;
  esac
}

# Print the RA8_FUZZ_TARGETS registry, one target per line, after
# cross-checking it against the tests/fuzz/fuzz_ra8_*.c sources. Both
# directions of drift are hard errors: a source file missing from the
# registry would never be fuzzed; a registry entry without a source
# cannot build.
list_targets() {
  local registered globbed src base t
  registered="$(awk '
    /^set\(RA8_FUZZ_TARGETS$/ { grab = 1; next }
    grab && /^\)/ { exit }
    grab { gsub(/[[:space:]]+/, ""); if ($0 != "") print }
  ' "${registry}")"
  if [[ -z "${registered}" ]]; then
    echo "ERROR: could not parse RA8_FUZZ_TARGETS from ${registry}" >&2
    return 1
  fi
  globbed=""
  for src in "${ROOT}"/tests/fuzz/fuzz_ra8_*.c; do
    base="$(basename "${src}" .c)"
    globbed="${globbed}${base}"$'\n'
    if ! grep -qx "${base}" <<<"${registered}"; then
      echo "ERROR: ${src} is not listed in RA8_FUZZ_TARGETS (${registry})." >&2
      echo "       Register the harness so the sweep runs it." >&2
      return 1
    fi
  done
  while IFS= read -r t; do
    if ! grep -qx "${t}" <<<"${globbed}"; then
      echo "ERROR: RA8_FUZZ_TARGETS lists ${t} but tests/fuzz/${t}.c does not exist." >&2
      return 1
    fi
  done <<<"${registered}"
  echo "${registered}"
}

# Reject a non-numeric or zero per-target budget before any work is done
# (libFuzzer treats -max_total_time=0 as "no time limit", which would hang
# an unattended sweep forever).
validate_seconds() {
  case "$1" in
    '' | *[!0-9]*)
      echo "ERROR: seconds must be a positive integer (got '$1')." >&2
      exit 2
      ;;
  esac
  if [[ "$1" -eq 0 ]]; then
    echo "ERROR: seconds must be >= 1 (0 means 'no limit' to libFuzzer)." >&2
    exit 2
  fi
}

# Return success if compiler $1 exists and can link a trivial libFuzzer
# binary (proves both clang and the platform's fuzzer runtime are present).
fuzz_compiler_ok() {
  local cand="$1" probe_dir ok=1
  command -v "${cand}" >/dev/null 2>&1 || return 1
  probe_dir="$(mktemp -d)"
  if printf 'int LLVMFuzzerTestOneInput(const unsigned char* data, unsigned long size);\nint LLVMFuzzerTestOneInput(const unsigned char* data, unsigned long size) { (void)data; (void)size; return 0; }\n' |
    "${cand}" -x c -fsanitize=fuzzer -o "${probe_dir}/probe" - >/dev/null 2>&1; then
    ok=0
  fi
  rm -rf "${probe_dir}"
  return "${ok}"
}

# Set CC/CXX to a fuzz-capable clang. Honours a pre-set CC (but still
# verifies it), otherwise probes bare clang first, then versioned majors
# newest-first (the CI runner and dev boxes ship only versioned binaries).
select_fuzz_compiler() {
  local cand
  if [[ -n "${CC:-}" ]]; then
    if ! fuzz_compiler_ok "${CC}"; then
      echo "ERROR: CC=${CC} cannot link -fsanitize=fuzzer." >&2
      echo "       libFuzzer requires clang with its compiler-rt fuzzer runtime" >&2
      echo "       (Debian/Ubuntu: apt install clang-<N> libclang-rt-<N>-dev)." >&2
      return 1
    fi
  else
    for cand in clang clang-22 clang-21 clang-20 clang-19 clang-18 clang-17; do
      if fuzz_compiler_ok "${cand}"; then
        CC="${cand}"
        break
      fi
    done
    if [[ -z "${CC:-}" ]]; then
      echo "ERROR: no clang able to link -fsanitize=fuzzer found on PATH." >&2
      echo "       libFuzzer requires clang with its compiler-rt fuzzer runtime" >&2
      echo "       (Debian/Ubuntu: apt install clang-<N> libclang-rt-<N>-dev)." >&2
      return 1
    fi
  fi
  if [[ -z "${CXX:-}" ]]; then
    CXX="${CC/clang/clang++}"
  fi
  export CC CXX
  echo "Using CC=${CC} CXX=${CXX}"
}

# Configure (first use) + build the given cmake target in tests/build-fuzz.
# Parallelism is the bounded canonical width (ra8_max_jobs), matching
# tests/build_tests.sh -- an explicit bound, not make's unlimited -j (#328).
build_fuzz_target() {
  local cmake_target="$1" jobs
  jobs="$(ra8_max_jobs)"
  if [[ ! -f "${build_dir}/CMakeCache.txt" ]]; then
    echo "Configuring fuzz build (${build_dir})..."
    mkdir -p "${build_dir}"
    cmake -S "${ROOT}/tests" -B "${build_dir}" \
      -DRA8_FUZZ=ON -DRA8_COVERAGE=OFF \
      -DCMAKE_C_COMPILER="${CC}" \
      -DCMAKE_CXX_COMPILER="${CXX}" \
      -DCMAKE_BUILD_TYPE=Debug \
      >"${build_dir}/configure.log" 2>&1
  fi
  echo "Building ${cmake_target} (jobs: ${jobs})..."
  cmake --build "${build_dir}" --target "${cmake_target}" --parallel "${jobs}"
}

# Run one built harness for $2 seconds, then rule on whether it actually got
# the budget it asked for. Returns 0 only when both the run and the budget are
# sound; the diagnosis is printed by fuzz_verdict_explain().
run_one() {
  local target="$1" seconds="$2"
  local bin="${build_dir}/fuzz/${target}"
  local crash_dir="${build_dir}/crashes/${target}"
  local corpus_dir="${ROOT}/tests/fuzz/corpus/${target}"
  local log_dir="${build_dir}/logs"
  local log="${log_dir}/${target}.log"
  local extra_args=()
  local t0 t1 elapsed reported verdict status=0 rc=0

  mkdir -p "${crash_dir}" "${log_dir}"
  if [[ ! -x "${bin}" ]]; then
    echo "ERROR: ${bin} did not build." >&2
    return 1
  fi
  if [[ -n "${FUZZ_RUNS:-}" ]]; then
    extra_args+=("-runs=${FUZZ_RUNS}")
  fi

  echo "Running ${target} for ${seconds}s (corpus -> ${corpus_dir}, artefacts -> ${crash_dir})..."
  # -max_total_time bounds wall-clock; -print_final_stats shows coverage.
  # -artifact_prefix= writes any crash inputs into the crash dir. The
  # positional corpus dir is consumed as the seed corpus and is also
  # where libFuzzer writes new interesting inputs.
  #
  # The output is teed to a per-target log so the "Done N runs in T second(s)"
  # line can be read back and compared with the monotonic measurement. It is
  # still streamed, because a nightly harness that prints nothing for ten
  # minutes is indistinguishable from a hung one.
  t0="$(monotonic_ms)"
  if ! "${bin}" \
    "${corpus_dir}" \
    -max_total_time="${seconds}" \
    -print_final_stats=1 \
    -artifact_prefix="${crash_dir}/" \
    "${extra_args[@]+"${extra_args[@]}"}" 2>&1 | tee "${log}"; then
    status="${PIPESTATUS[0]}"
    if [[ "${status}" -eq 0 ]]; then
      # tee failed rather than the harness; still a failed run.
      status=1
    fi
  fi
  t1="$(monotonic_ms)"
  elapsed=$(((t1 - t0 + 500) / 1000))

  # Take the LAST such line, and accept a negative count: a negative duration
  # is precisely the signature being hunted, so a parser that rejected it
  # would drop the evidence on the floor.
  reported="$(awk '/^Done [0-9]+ runs in -?[0-9]+ second\(s\)$/ { v = $(NF - 1) }
                   END { if (v != "") print v }' "${log}")"

  verdict="$(fuzz_run_verdict "${seconds}" "${elapsed}" \
    "${status}" "${reported}" "${FUZZ_RUNS:-}")" || rc=1
  fuzz_verdict_explain "${verdict}" "${target}" "${seconds}" "${elapsed}" "${reported}"
  echo "${target}: budget ${seconds}s, ran ${elapsed}s (CLOCK_MONOTONIC), libFuzzer reported ${reported:-no}s -- ${verdict}"
  return "${rc}"
}

# Drive every branch of fuzz_run_verdict() with synthetic inputs, in both
# directions: each case asserts the verdict AND the exit status, and the suite
# fails if a rule stops matching OR starts matching something it should not.
# Cases 3 and 4 are the real numbers observed on win-ci in #509.
verdict_case() {
  local want_verdict="$1" want_rc="$2" what="$3"
  shift 3
  local got_verdict got_rc=0
  got_verdict="$(fuzz_run_verdict "$@")" || got_rc=1
  if [[ "${got_verdict}" == "${want_verdict}" && "${got_rc}" -eq "${want_rc}" ]]; then
    echo "  ok   ${what} -> ${got_verdict} (rc ${got_rc})"
    return 0
  fi
  echo "  FAIL ${what}: expected ${want_verdict} (rc ${want_rc}), got ${got_verdict} (rc ${got_rc})" >&2
  return 1
}

selftest() {
  local fails=0
  echo "run_fuzz.sh budget-check selftest:"
  #             verdict      rc  description                        budget elapsed status reported cap
  verdict_case ok 0 "full budget spent" 300 302 0 301 "" || fails=1
  verdict_case crash 1 "crash found four seconds in" 300 4 1 "" "" || fails=1
  verdict_case clock-skew 1 "clock stepped BACK mid-run (#509)" 300 4 0 -232 "" || fails=1
  verdict_case clock-skew 1 "clock stepped FORWARD mid-run (#509)" 60 61 0 251 "" || fails=1
  verdict_case runs-capped 0 "FUZZ_RUNS cap reached early" 30 2 0 2 4096 || fails=1
  verdict_case short-run 1 "starved with a coherent clock" 300 4 0 4 "" || fails=1
  verdict_case no-stats 1 "no final-stats line to check" 300 300 0 "" "" || fails=1
  verdict_case clock-skew 1 "a -runs cap does not excuse a clock step" 30 2 0 -240 4096 || fails=1
  if [[ "${fails}" -ne 0 ]]; then
    echo "SELFTEST FAILED: the fuzz budget check no longer behaves as documented." >&2
    return 1
  fi
  echo "  8/8 cases as documented."
}

if [[ $# -lt 1 ]]; then
  usage
  exit 2
fi

if [[ "$1" == "--list" ]]; then
  list_targets
  exit 0
fi

if [[ "$1" == "--selftest" ]]; then
  selftest
  exit 0
fi

if [[ "$1" == "--all" ]]; then
  seconds="${2:-60}"
  validate_seconds "${seconds}"
  require_monotonic_clock
  targets="$(list_targets)"
  select_fuzz_compiler
  build_fuzz_target ra8_fuzz_all
  # Seed the corpus directories -- the init script is idempotent and only
  # refreshes the known-good seeds, so any crash reproducers added later
  # are left in place.
  bash "${ROOT}/scripts/checks/init_fuzz_corpora.sh" >/dev/null
  failed=""
  while IFS= read -r target; do
    echo ""
    echo "==== ${target} (${seconds}s) ===="
    # run_one has already printed the specific diagnosis -- a crash, a stepped
    # clock or an unexplained short run are different faults and were once all
    # reported here as "artefacts in ...", which is true of exactly one of them.
    if ! run_one "${target}" "${seconds}"; then
      failed="${failed} ${target}"
    fi
  done <<<"${targets}"
  echo ""
  if [[ -n "${failed}" ]]; then
    echo "FUZZ FAILURES:${failed}" >&2
    exit 1
  fi
  echo "All fuzz runs passed."
  exit 0
fi

target="$1"
seconds="${2:-60}"
validate_seconds "${seconds}"
require_monotonic_clock

# Reject unregistered targets up front -- a typo would otherwise burn the
# whole budget on a cmake error message.
if ! list_targets | grep -qx "${target}"; then
  echo "ERROR: unknown fuzz target '${target}'." >&2
  usage
  exit 2
fi

select_fuzz_compiler
build_fuzz_target "${target}"
bash "${ROOT}/scripts/checks/init_fuzz_corpora.sh" >/dev/null
run_one "${target}" "${seconds}"
