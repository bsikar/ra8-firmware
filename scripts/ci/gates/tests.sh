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
# for a workflow to call (`just quality::local::gate <name>`). Adding a second
# registry here would recreate the drift the single-definition rule exists to
# prevent.
#
# Gates in this file: work-harness, unit-tests, ubsan, coverage-tree, mcdc,
# artefact-freshness, cache-bench

# --- work-harness ---------------------------------------------------------
gate_work_harness() (
  set -e
  require_cmd python3 "the workflow harness is a Python command"
  require_cmd bash "the canonical workspace lifecycle is a Bash script"
  require_cmd sh "the emitted operator script is executed by POSIX sh"
  require_cmd git "workspace binding tests use isolated local git repositories"
  require_cmd jq "the emitted GitHub script discovers board metadata with jq"
  require_cmd shellcheck "the generated operator script must be valid POSIX shell"
  RA8_WORK_HARNESS_REGISTERED_GATE=1 python3 -I tools/work/src/work.py --selftest
  /bin/bash -p -n scripts/dev/agent_workspace.sh
  bash scripts/dev/agent_workspace_selftest.sh
  local generated
  generated="$(mktemp)"
  trap 'rm -f "$generated"' EXIT
  python3 -I tools/work/src/work.py plan tools/work/tests/fixtures/injection_notes.md \
    --emit-commands >"$generated"
  shellcheck -s sh "$generated"
)

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
# native toolchain pin in the Dockerfile and dev-box Ansible role, the dev box has it built from
# source at /usr/local/bin/gcc-14 (docs/TOOLCHAIN.md, "CONVERGED"), and every
# other host-compiler probe in this tree already prefers it first
# (scripts/report/tree_coverage.sh, scripts/emu/eil_all.sh, scripts/emu/smoke.sh,
# scripts/emu/matrix.sh all run `ra8_select_host_compiler gcc-14 gcc-13 ...`).
# Pinning ubsan to the one compiler every environment actually guarantees
# turns "gate fails loudly on a missing tool" into "gate does not need the
# missing tool", which is the stronger fix per CLAUDE.md's gate-honesty rule.
gate_ubsan() (
  set -e
  require_cmd gcc-14 "the UBSan gate pins gcc-14 to match the provisioned toolchain"
  CC=gcc-14 CXX=g++-14 /bin/bash -p scripts/dev/run_just.sh quality::local::test 1
)

# --- coverage-tree --------------------------------------------------------
# THE per-file line/branch policy, for every first-party translation unit in
# libs/, src/, port/, tools/, apps/ and examples/ -- one census, one baseline,
# one bar. It replaces three overlapping gates (an aggregate 90/80 plus a
# libs+src-only line floor, a SECOND coverage build ratcheted against a
# two-number baseline, and a legacy product-named ratchet), which between
# them built the same translation units twice and still left most of the tree
# unmentioned by any policy.
#
# tree_coverage.sh MEASURES and check_tree_coverage.py JUDGES; the split is why
# there is one policy surface rather than a policy per build.
#
# CC is pinned rather than selected. Line and branch counts are
# compiler-version specific, so a baseline frozen under gcc-14 and re-measured
# under gcc-13 reports regressions that are really a different compiler -- the
# gate would be reporting on the runner image, not on the tree.
gate_coverage_tree() (
  set -e
  require_cmd cmake
  require_cmd ctest
  require_cmd gcc-14 "the coverage gate pins gcc-14; counts are compiler-specific"
  require_cmd gcov-14
  require_tool_versions gcc-14
  require_cmd gcovr
  # gcovr's report model is version-sensitive. Version 8.4 began retaining
  # multiple coverage records per source line, which changes counts for the
  # white-box test variants that include production C files under renamed
  # symbols. Assert the exact 7.0 pin used to freeze the baseline (#333).
  require_tool_versions gcovr
  # mdl's listfile find_program()s both as REQUIRED for its real-libcurl
  # HTTPS integration test; without them cmake dies 700 lines before the gate
  # says anything, and the openssl CLI reaches the runner image only as a
  # transitive apt dependency of ca-certificates.
  require_cmd openssl "mdl's HTTPS integration test mints its server cert with openssl"
  require_cmd python3 "mdl's HTTPS integration test serves fixtures from python3 http.server"

  # Prove the policy still fires and stays quiet BEFORE spending the build on
  # a verdict it might not be able to reach.
  python3 scripts/checks/check_tree_coverage.py --selftest

  CC=gcc-14 CXX=g++-14 bash scripts/report/tree_coverage.sh
  python3 scripts/checks/check_tree_coverage.py
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
# this week. `just quality::local::mcdc` for a developer stays incremental (mcdc_report.sh keeps
# a matching-compiler cache) -- only the CI gate wipes. artefact-freshness runs
# AFTER this gate and re-reads the report mcdc_report.sh regenerates, so the wipe
# is safe.
read_mcdc_baseline() {
  local baseline_file="$1" baseline
  local -a baseline_rows=()
  mapfile -t baseline_rows < <(
    sed -e '/^[[:space:]]*#/d' -e '/^[[:space:]]*$/d' "$baseline_file"
  )
  if [[ "${#baseline_rows[@]}" -ne 1 ]]; then
    echo "FAIL: $baseline_file must contain exactly one non-comment percentage row" >&2
    return 1
  fi
  baseline="$(tr -d '[:space:]' <<<"${baseline_rows[0]}")"
  if [[ ! "$baseline" =~ ^(100([.]0+)?|[0-9]{1,2}([.][0-9]+)?)$ ]]; then
    echo "FAIL: invalid MC/DC baseline percentage: $baseline" >&2
    return 1
  fi
  printf '%s\n' "$baseline"
}

assert_mcdc_baseline_parser() {
  local scratch valid
  scratch="$(mktemp -d "${TMPDIR:-/tmp}/ra8-mcdc-baseline.XXXXXXXX")"
  printf '# provenance\n89.72\n' >"$scratch/valid.txt"
  valid="$(read_mcdc_baseline "$scratch/valid.txt")"
  [[ "$valid" == "89.72" ]] || return 1
  printf '89.72\n90.00\n' >"$scratch/multiple.txt"
  ! read_mcdc_baseline "$scratch/multiple.txt" >/dev/null 2>&1 || return 1
  printf 'not-a-percentage\n' >"$scratch/invalid.txt"
  ! read_mcdc_baseline "$scratch/invalid.txt" >/dev/null 2>&1 || return 1
  rm -rf -- "$scratch"
}

assert_mcdc_transports() {
  assert_mcdc_baseline_parser
  bash scripts/ci/devcontainer_run.sh --selftest
  CC=clang-18 CXX=clang++-18 bash scripts/report/mcdc_report.sh --selftest
}

gate_mcdc() (
  set -e
  set -o pipefail
  require_cmd clang-18 "the MC/DC gate pins clang-18 to match CI"

  # Prove both transport and build-dir freshness before measuring (see header).
  assert_mcdc_transports

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
  baseline="$(read_mcdc_baseline "$baseline_file")" || return
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
# Builds and runs cache_bench (the SLRU decision record), reader_vmem
# (drives the real ra8_vmem with a reader workload and emits a
# cache_bench-consumable trace) and glyph_bench (sweeps the real glyph atlas),
# re-confirming SLRU on the captured reader trace on every push. Both pinned
# host compilers are required: the rest of tools-build already proves clang-18
# and gcc-14 independently, and a benchmark is not a reason to accept a weaker
# warning/compiler bar. Each arm starts from clean outputs so Just cannot reuse
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
  require_cmd just "the cache benchmark builds through authoritative tool recipes"
  require_cmd clang-18 "the cache-bench gate pins clang-18 to match CI"
  require_cmd gcc-14 "the cache-bench gate pins gcc-14 as its second warning arm"
  require_tool_versions gcc-14
  local cc
  for cc in clang-18 gcc-14; do
    /bin/bash -p scripts/dev/run_just.sh tools::clean
    CC="$cc" /bin/bash -p scripts/dev/run_just.sh tools::bench_cache
  done
)
