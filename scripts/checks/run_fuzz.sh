#!/usr/bin/env bash
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
#                 before the wall budget instead of idling.
#
# Crash artefacts are written to tests/build-fuzz/crashes/<target>/.
# The script exits non-zero if any requested harness reports a crash;
# with --all every harness still runs so one crash cannot mask another.
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

build_dir="${ROOT}/tests/build-fuzz"
registry="${ROOT}/tests/fuzz/CMakeLists.txt"

usage() {
  echo "Usage: $0 <target> [seconds]   # run one harness (default 60 s)" >&2
  echo "       $0 --all [seconds]      # run every registered harness" >&2
  echo "       $0 --list               # print the harness registry" >&2
  echo "  (registry: RA8_FUZZ_TARGETS in tests/fuzz/CMakeLists.txt)" >&2
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
# Parallelism mirrors tests/build_tests.sh: nproc (Linux), sysctl (macOS),
# fall back to 4 -- an explicit bound, not make's unlimited -j.
build_fuzz_target() {
  local cmake_target="$1" jobs
  if command -v nproc >/dev/null 2>&1; then
    jobs="$(nproc)"
  elif command -v sysctl >/dev/null 2>&1; then
    jobs="$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"
  else
    jobs=4
  fi
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

# Run one built harness for $2 seconds. Returns libFuzzer's exit status.
run_one() {
  local target="$1" seconds="$2"
  local bin="${build_dir}/fuzz/${target}"
  local crash_dir="${build_dir}/crashes/${target}"
  local corpus_dir="${ROOT}/tests/fuzz/corpus/${target}"
  local extra_args=()

  mkdir -p "${crash_dir}"
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
  "${bin}" \
    "${corpus_dir}" \
    -max_total_time="${seconds}" \
    -print_final_stats=1 \
    -artifact_prefix="${crash_dir}/" \
    "${extra_args[@]+"${extra_args[@]}"}"
}

if [[ $# -lt 1 ]]; then
  usage
  exit 2
fi

if [[ "$1" == "--list" ]]; then
  list_targets
  exit 0
fi

if [[ "$1" == "--all" ]]; then
  seconds="${2:-60}"
  validate_seconds "${seconds}"
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
    if ! run_one "${target}" "${seconds}"; then
      echo "FUZZ FAIL: ${target} (artefacts in ${build_dir}/crashes/${target}/)" >&2
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
