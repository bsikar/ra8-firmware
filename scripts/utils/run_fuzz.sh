#!/usr/bin/env bash
#
# scripts/utils/run_fuzz.sh -- run one libFuzzer harness for an
# arbitrary wall-clock budget. Configures the fuzz build directory on
# first use, builds the requested target, then invokes it with the
# given timeout.
#
# Usage:
#
#   bash scripts/utils/run_fuzz.sh <target>            # 60 second run
#   bash scripts/utils/run_fuzz.sh <target> <seconds>  # custom budget
#
# Examples:
#
#   bash scripts/utils/run_fuzz.sh fuzz_ra_jpeg_sw 600
#   bash scripts/utils/run_fuzz.sh fuzz_ra_modem_at 30
#
# Crash artefacts are written to tests/build-fuzz/crashes/<target>/.
# The script exits non-zero if libFuzzer reports a crash.
#
# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

if [[ $# -lt 1 ]]; then
  echo "Usage: $0 <target> [seconds]" >&2
  echo "  target:  fuzz_ra_jpeg_sw | fuzz_ra_epub | fuzz_ra_stb_image | fuzz_ra_reflow_xml | fuzz_ra_stbtt | ..." >&2
  echo "           (full list: RA_FUZZ_TARGETS in tests/fuzz/CMakeLists.txt)" >&2
  exit 2
fi

target="$1"
seconds="${2:-60}"

build_dir="${ROOT}/tests/build-fuzz"
crash_dir="${build_dir}/crashes/${target}"
mkdir -p "${crash_dir}"

CC="${CC:-clang}"
CXX="${CXX:-clang++}"

if ! command -v "${CC}" >/dev/null 2>&1; then
  echo "ERROR: ${CC} not found. libFuzzer requires clang." >&2
  exit 1
fi

if [[ ! -f "${build_dir}/CMakeCache.txt" ]]; then
  echo "Configuring fuzz build (${build_dir})..."
  cmake -S "${ROOT}/tests" -B "${build_dir}" \
    -DRA_FUZZ=ON -DRA_COVERAGE=OFF \
    -DCMAKE_C_COMPILER="${CC}" \
    -DCMAKE_CXX_COMPILER="${CXX}" \
    -DCMAKE_BUILD_TYPE=Debug \
    >"${build_dir}/configure.log" 2>&1
fi

echo "Building ${target}..."
cmake --build "${build_dir}" --target "${target}" -j

bin="${build_dir}/fuzz/${target}"
if [[ ! -x "${bin}" ]]; then
  echo "ERROR: ${bin} did not build." >&2
  exit 1
fi

# Seed the corpus directory the first time and on every run -- the
# init script is idempotent and only refreshes the known-good seeds,
# so any crash reproducers added later are left in place.
bash "${ROOT}/scripts/utils/init_fuzz_corpora.sh" >/dev/null
corpus_dir="${ROOT}/tests/fuzz/corpus/${target}"

echo "Running ${target} for ${seconds}s (corpus -> ${corpus_dir}, artefacts -> ${crash_dir})..."
# -max_total_time bounds wall-clock; -print_final_stats shows coverage.
# -artifact_prefix= writes any crash inputs into the crash dir. The
# positional corpus dir is consumed as the seed corpus and is also
# where libFuzzer writes new interesting inputs.
"${bin}" \
  "${corpus_dir}" \
  -max_total_time="${seconds}" \
  -print_final_stats=1 \
  -artifact_prefix="${crash_dir}/"
