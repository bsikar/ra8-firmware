#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# shellcheck shell=bash
#
# scripts/ci/gates/tests.sh -- Host test execution and its coverage gates.
#
# SOURCED, NEVER EXECUTED. scripts/ci.sh sources every file in this directory
# and is the only entry point; RA8_GATE_REGISTRY -- the single list of what
# gates exist -- stays there too. These files hold gate BODIES only, so there
# is still exactly one home for a gate's definition and exactly one command
# for a workflow to call (bash scripts/ci.sh --gate <name>). Adding a second
# registry here would recreate the drift the single-definition rule exists to
# prevent.
#
# Gates in this file: unit-tests, ubsan, coverage, coverage-report, mcdc,
# artefact-freshness, cache-bench, tools-coverage

# --- unit-tests -----------------------------------------------------------
gate_unit_tests() (
  set -e
  # build_tests.sh defaults to `${CMAKE:-cmake}` and run_tests.sh drives ctest.
  # Without a guard an absent toolchain surfaces as a bare "command not found"
  # deep in a build log; name the missing dependency at the gate boundary
  # instead. A gate must never be able to report "nothing to run" as success.
  require_cmd cmake "apt-get install -y cmake"
  require_cmd ctest "ships with cmake; check the cmake install"
  bash tests/build_tests.sh
  bash tests/run_tests.sh
)

# --- ubsan ----------------------------------------------------------------
# The whole host suite rebuilt under -fsanitize=undefined in its own tree with
# UBSAN_OPTIONS=halt_on_error=1, so any undefined behaviour is a hard test
# failure. Pin the compiler like the coverage gate does: the ambient `gcc`
# changes with the runner image, and -Wconversion findings are
# compiler-version-specific.
#
# gcc-14, not gcc-13 (#489): gcc-13 was never a provisioned pin anywhere, only
# an assumption that Ubuntu 24.04's apt `gcc` metapackage happens to default
# to it -- true on the ra8-ci runner image, but the shared dev box runs Debian
# 12 (bookworm), which defaults to gcc-12 and has no gcc-13 package at all
# (backports included). gcc-14 is what this tree actually provisions
# everywhere that runs this gate: the runner image installs it by an explicit
# Dockerfile ARG pin (.devcontainer/Dockerfile), the dev box has it built from
# source at /usr/local/bin/gcc-14 (docs/TOOLCHAIN.md, "CONVERGED"), and every
# other host-compiler probe in this tree already prefers it first
# (scripts/checks/coverage.sh, scripts/emu/eil_all.sh, scripts/emu/smoke.sh,
# scripts/emu/matrix.sh all run `ra8_select_host_compiler gcc-14 gcc-13 ...`).
# Pinning ubsan to the one compiler every environment actually guarantees
# turns "gate fails loudly on a missing tool" into "gate does not need the
# missing tool", which is the stronger fix per CLAUDE.md's gate-honesty rule.
gate_ubsan() (
  set -e
  require_cmd gcc-14 "the UBSan gate pins gcc-14 to match the provisioned toolchain"
  CC=gcc-14 CXX=g++-14 make ubsan
)

# --- coverage -------------------------------------------------------------
gate_coverage() (
  set -e
  require_cmd gcovr
  # gcovr version-sensitivity is documented history: the 5.2 line has a
  # merge_function assertion crash newer lines do not. Assert a floor so the
  # crashing ancient build fails loud instead of skewing coverage (#333).
  require_tool_versions gcovr
  bash scripts/checks/coverage.sh --gate
)

# --- coverage-report ------------------------------------------------------
# The plain statement + branch flow from docs/COVERAGE.md, ratcheted against
# .github/coverage-baseline.txt. Independent of the MC/DC pipeline.
# --in-container is a no-op marker that skips the script's macOS branch.
gate_coverage_report() (
  set -e
  require_cmd gcovr
  require_tool_versions gcovr
  bash scripts/report/coverage_report.sh --in-container
  python3 scripts/checks/check_coverage.py
)

# --- mcdc -----------------------------------------------------------------
# clang-18 source-based coverage with -fcoverage-mcdc, gated against
# .github/mcdc-baseline.txt so coverage can never regress.
#
# RA8_MCDC_THRESHOLD=0 disables mcdc_report.sh's own per-file gate; the
# project-wide baseline comparison below is the actual quality bar.
#
# Freshness (#346/#326): the gate first proves mcdc_report.sh's build-dir
# freshness guard still fires (--selftest) BEFORE spending twenty minutes
# measuring nothing -- an inherited tests/build-cov configured by the coverage
# gate's gcc cached "-fcoverage-mcdc: no", so the build came out uninstrumented
# and the gate died at the merge step blaming the tests when all 540 had passed.
# It then WIPES both tests/build-cov and build/mcdc-report: a reused build
# carries that stale probe, and a leftover build/mcdc-report holds a stale
# mcdc_per_file.json / gate.json the per-file MC/DC floor would read and
# FALSE-PASS on numbers this run never produced. This exact reuse hid a real red
# this week. `make mcdc` for a developer stays incremental (mcdc_report.sh keeps
# a matching-compiler cache) -- only the CI gate wipes. artefact-freshness runs
# AFTER this gate and re-reads the report mcdc_report.sh regenerates, so the wipe
# is safe.
gate_mcdc() (
  set -e
  set -o pipefail
  require_cmd clang-18 "the MC/DC gate pins clang-18 to match CI"

  # Prove the build-dir freshness guard fires before measuring (see header).
  CC=clang-18 CXX=clang++-18 bash scripts/report/mcdc_report.sh --selftest

  # Wipe build AND report dir for a guaranteed-fresh MC/DC build (see header).
  rm -rf "${RA8_MCDC_BUILD_DIR:-tests/build-cov}" \
    "${RA8_MCDC_REPORT_DIR:-build/mcdc-report}" && mkdir -p "${RA8_MCDC_REPORT_DIR:-build/mcdc-report}"

  # Transcript under build/, not the repo root: at the root it survives as an
  # untracked file that the next `git add -A` commits, which is how it once
  # turned dev red on the lint-coverage gate.
  local report_dir="${RA8_MCDC_REPORT_DIR:-build/mcdc-report}"
  CC=clang-18 CXX=clang++-18 RA8_MCDC_THRESHOLD=0 \
    bash scripts/report/mcdc_report.sh --in-container | tee "$report_dir/mcdc-output.log"

  local summary="build/mcdc-report/summary.txt"
  local baseline_file="${MCDC_BASELINE_FILE:-.github/mcdc-baseline.txt}"
  if [[ ! -f "$summary" ]]; then
    echo "FAIL: MC/DC summary not produced at $summary" >&2
    return 1
  fi
  if [[ ! -f "$baseline_file" ]]; then
    echo "FAIL: baseline file $baseline_file missing" >&2
    return 1
  fi
  local baseline total_line measured drop
  baseline="$(tr -d '[:space:]' <"$baseline_file")"
  total_line="$(grep -E '^TOTAL' "$summary" | tail -1 || true)"
  if [[ -z "$total_line" ]]; then
    echo "FAIL: no TOTAL row in $summary" >&2
    tail -40 "$summary" || true
    return 1
  fi
  # Last percentage column on the TOTAL row is the MC/DC %.
  measured="$(echo "$total_line" | grep -oE '[0-9]+\.[0-9]+%' | tail -1 | tr -d '%')"
  if [[ -z "$measured" ]]; then
    echo "FAIL: could not parse MC/DC % from TOTAL row: $total_line" >&2
    return 1
  fi
  printf 'Measured MC/DC: %s%%   Baseline: %s%%\n' "$measured" "$baseline"
  drop="$(awk -v m="$measured" -v b="$baseline" 'BEGIN{print (m+0 < b+0) ? 1 : 0}')"
  if [[ "$drop" -eq 1 ]]; then
    echo "FAIL: MC/DC coverage ${measured}% dropped below baseline ${baseline}%"
    echo ""
    echo "      Either add MC/DC test vectors for the new compound decisions"
    echo "      OR reduce the decision count. Do NOT lower the baseline file"
    echo "      to make this pass."
    echo ""
    echo "      scripts/checks/check_new_compound_has_mcdc.py is supposed to"
    echo "      catch this locally; if a regression reached CI, either the hook"
    echo "      was bypassed or a citation in an existing test drifted out of"
    echo "      the +/- 25-line tolerance window. See docs/MCDC.md."
    return 1
  fi
  echo "PASS: MC/DC coverage holds the baseline."
)

# --- artefact-freshness ---------------------------------------------------
# #380: the committed MC/DC + doxygen gap docs (docs/MCDC_GAPS.csv, .md,
# docs/MCDC_DEACTIVATIONS.md, docs/DOXYGEN_GAPS.csv, .md) must equal what their
# generators produce from the current tree. Nothing used to notice when they
# drifted, so on a DO-178C Level B target the human-readable gap record quietly
# described a tree that no longer existed.
#
# The MC/DC half CONSUMES the mcdc gate's build/mcdc-report/mcdc.txt
# (regen_mcdc_gaps.py reads it), so this gate is scheduled immediately AFTER the
# `mcdc` gate in the same job / same snapshot and reuses that report rather than
# re-running the ~20-minute coverage build. With the report absent the checker
# FAILS LOUDLY naming the dependency instead of skipping. The doxygen half is a
# static source parse and needs no toolchain input. --selftest runs first, in
# both directions, so a checker that stopped comparing cannot pass as clean.
gate_artefact_freshness() (
  set -e
  require_cmd python3 "the artefact-freshness gate regenerates docs via python generators"
  python3 scripts/checks/check_generated_artefacts.py --selftest
  python3 scripts/checks/check_generated_artefacts.py
)

# --- cache-bench ----------------------------------------------------------
# #147/#160: builds + runs cache_bench (the SLRU decision record), reader_vmem
# (drives the real ra8_vmem with a reader workload and emits a
# cache_bench-consumable trace) and glyph_bench (sweeps the real glyph atlas),
# re-confirming SLRU on the captured reader trace on every push. Both pinned
# host compilers are required: the rest of tools-build already proves clang-18
# and gcc-14 independently, and a benchmark is not a reason to accept a weaker
# warning/compiler bar. Each arm starts from clean outputs so make cannot reuse
# the first compiler's binaries for the second arm.
#
# Despite the name this is NOT a wall-clock gate, so it belongs in the local
# suite and is stable under a loaded shared box (#326/#328). Every non-zero exit
# in cache_bench / reader_vmem / glyph_bench comes from a DETERMINISTIC failure
# -- an allocation or trace-build error, a get/put/verify data-integrity
# mismatch, or the SLRU policy losing on the fixed captured trace -- none of
# which depend on how busy the machine is. The wall_ns / MiB-s figures the tools
# print are informational only and gate nothing, so a load average of 156 (#328)
# changes the numbers on screen but never the PASS/FAIL verdict.
gate_cache_bench() (
  set -e
  require_cmd clang-18 "the cache-bench gate pins clang-18 to match CI"
  require_cmd gcc-14 "the cache-bench gate pins gcc-14 as its second warning arm"
  require_tool_versions gcc-14
  local cc
  for cc in clang-18 gcc-14; do
    make -C tools/cache_bench clean
    make -C tools/reader_vmem clean
    make -C tools/glyph_bench clean
    CC="$cc" make bench-cache
  done
)

# --- tools-coverage -------------------------------------------------------
# media_dl carries production orchestration code that is not linked into the
# firmware host suite, so the libs/src coverage build cannot measure it. Build
# and run its seven CTest binaries under the same pinned gcc/gcov pipeline,
# then apply a per-file ratchet: historical uncovered debt may only shrink and
# every new production file must enter at the firmware's 90/80 line/branch bar.
gate_tools_coverage() (
  set -e
  require_cmd cmake
  require_cmd ctest
  require_cmd gcc-14 "the tool coverage gate pins gcc-14 to match gcov-14"
  require_cmd gcov-14
  require_cmd gcovr
  require_tool_versions gcc-14

  local out="$PWD/build/tool-coverage/media_dl"
  rm -rf "$out"
  cmake -S "$PWD/tools/media_dl" -B "$out" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_C_COMPILER=gcc-14 \
    -DRA8_COVERAGE=ON
  cmake --build "$out" --parallel "$(ra8_max_jobs)"
  ctest --test-dir "$out" --output-on-failure --timeout 60

  gcovr \
    --gcov-executable gcov-14 \
    --gcov-ignore-parse-errors=all \
    --root "$PWD" \
    --object-directory "$out" \
    --filter "$PWD/tools/media_dl/src/" \
    --exclude-throw-branches \
    --exclude-unreachable-branches \
    --json-summary "$out/coverage.json" \
    --json-summary-pretty \
    --print-summary

  python3 scripts/checks/check_tool_coverage.py --selftest
  python3 scripts/checks/check_tool_coverage.py
)
