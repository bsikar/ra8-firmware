#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# shellcheck shell=bash
#
# scripts/ci/gates/analysis.sh -- Static analysis and link-time structure: scan-build, clang-tidy, CMSE.
#
# SOURCED, NEVER EXECUTED. scripts/ci.sh sources every file in this directory
# and is the only entry point; RA8_GATE_REGISTRY -- the single list of what
# gates exist -- stays there too. These files hold gate BODIES only, so there
# is still exactly one home for a gate's definition and exactly one command
# for a workflow to call (`just quality::local::gate <name>`). Adding a second
# registry here would recreate the drift the single-definition rule exists to
# prevent.
#
# Gates in this file: scan-build, tidy, nsc-cmse, sg-offsets, stack-usage

# --- scan-build -----------------------------------------------------------
# The clang static analyzer over the host unit-test build and every CMake host
# tool: path-sensitive symbolic execution, so it finds the null-deref / leak /
# garbage-read PATHS cppcheck's pattern matching cannot.
#
# docs/STATIC_ANALYSIS.md claimed "CI runs bash scripts/checks/scan_build.sh
# --strict" for months while no workflow ran it and RA8_GATE_REGISTRY had no
# such gate (#532); the removed pre-commit hook even carried a comment saying so, so
# the tree contradicted itself. This row is what makes the sentence true.
#
# require_cmd on the PINNED major, not a bare `scan-build`: the CI image
# installs clang-tools-18, which provides scan-build-18 and no unversioned
# symlink, and analysing under a different clang major would be a different
# checker set from the one the baseline was measured with.
#
# --selftest FIRST. This script decides what counts as an actionable finding
# and what counts as an analysis at all, and it has already shipped a clean
# verdict over an analysis that never happened. The selftest drives the real
# classifier over synthetic reports in both directions (a first-party finding
# and an off-partition fixed-address finding must be REPORTED; SOUP, test
# scaffolding and the documented MMIO partitions must be SUPPRESSED), the
# vacuity floor either side of its boundary, and the fail-loud path.
# Remove only a workspace created by gate_scan_build. scan_build.sh itself
# must build outside the checkout so gcovr cannot ingest clang profile data;
# the gate therefore owns that external lifetime explicitly.
scan_build_gate_cleanup() {
  local dir="$1" tmp_root="${TMPDIR:-/tmp}" name parent
  name="$(basename -- "$dir")"
  parent="$(dirname -- "$dir")"
  if [[ "$parent" != "$tmp_root" || "$name" != ra8-scan-gate.* ]]; then
    echo "ERROR: refusing to remove non-gate scan-build path: $dir" >&2
    return 1
  fi
  rm -rf -- "${dir:?}"
}

# Assert both directions of the external-workspace ownership rule: the exact
# gate-shaped directory is reclaimed, and an unrelated directory is refused.
scan_build_gate_cleanup_selftest() {
  local probe keep
  probe="$(mktemp -d "${TMPDIR:-/tmp}/ra8-scan-gate.XXXXXXXX")"
  keep="$(mktemp -d "${TMPDIR:-/tmp}/ra8-scan-keep.XXXXXXXX")"
  scan_build_gate_cleanup "$probe"
  if [[ -e "$probe" || ! -d "$keep" ]]; then
    echo "ERROR: scan-build workspace cleanup self-test failed." >&2
    rm -rf -- "$probe" "$keep"
    return 1
  fi
  if scan_build_gate_cleanup "$keep" >/dev/null 2>&1; then
    echo "ERROR: scan-build cleanup accepted a non-gate path." >&2
    return 1
  fi
  rm -rf -- "$keep"
  echo "scan-build gate cleanup self-test: PASS"
}

gate_scan_build() (
  set -e
  require_cmd scan-build-18 \
    "CI installs clang-tools-18; add it to .devcontainer/Dockerfile too."
  require_cmd cmake
  scan_build_gate_cleanup_selftest
  local scan_out
  scan_out="$(mktemp -d "${TMPDIR:-/tmp}/ra8-scan-gate.XXXXXXXX")"
  trap 'scan_build_gate_cleanup "$scan_out"' EXIT
  bash scripts/checks/scan_build.sh --selftest
  RA8_SCAN_BUILD_OUT_DIR="$scan_out" bash scripts/checks/scan_build.sh --strict
)

# --- tidy -----------------------------------------------------------------
# clang-tidy over every first-party C, C++ and Objective-C file.
#
# Needs the cross-compiler as well as clang-tidy: since #369 the firmware pass
# parses examples/ and port/ against a CROSS-COMPILE compile database
# that scripts/builders/build_cross_compile_db.py produces by really
# configuring the RA8D2 / RA8P1 builds. require_arm_gcc_m85 makes an absent or
# too-old toolchain a hard failure -- if this degraded to skipping the firmware
# pass, the gate would go green having analysed barely half the tree, which is
# the exact failure #369 existed to describe.
#
# The verdict comes from tidy_ratchet.py, not from clang-tidy's exit status.
# #369 and #370 brought a large, never-before-analysed surface into scope, and
# it arrived carrying pre-existing findings. The ratchet freezes exactly those
# in a committed baseline and fails on any INCREASE -- so the new surface is
# genuinely gated, and code with no baseline entry (all of libs/, src/, tools/)
# still hard-fails on its first finding exactly as before.
#
# Exit code 2 from clang_tidy.sh means the script could not do its job (no
# compile database, a failed configure, a scope regression). That must fail
# immediately and must NEVER reach the ratchet: an infrastructure failure that
# produced no findings would otherwise read as a clean run.
gate_tidy() (
  set -e
  local pinned_tidy
  pinned_tidy="$(python3 scripts/checks/check_tool_versions.py --print-binary clang-tidy)"
  use_pinned_arm_toolchain
  require_arm_gcc_m85
  require_cmd cmake
  # The Dockerfile-derived registry selects the exact binary and major. Assert
  # that selection resolves and reports its registered major, so a newer bare
  # clang-tidy cannot silently drift the ratchet (#333).
  require_tool_versions "$pinned_tidy"
  CLANG_TIDY="$pinned_tidy" bash scripts/checks/clang_tidy.sh --selftest
  python3 scripts/checks/tidy_ratchet.py --selftest
  # #712: prove the committed baseline is the canonical file --update writes
  # before trusting it. A whole-file hand sort once bypassed the ratchet's own
  # refusal and survived ten days because "parseable" was the only bar.
  python3 scripts/checks/tidy_ratchet.py --attest

  local log rc
  log="$(mktemp)"
  rc=0
  CLANG_TIDY="$pinned_tidy" bash scripts/checks/clang_tidy.sh --check --verbose >"$log" 2>&1 || rc=$?
  cat "$log"
  if [ "$rc" -ge 2 ]; then
    echo "ERROR: clang_tidy.sh could not run (exit $rc); not ratcheting." >&2
    rm -f "$log"
    return 1
  fi
  python3 scripts/checks/tidy_ratchet.py --check "$log"
  rc=$?
  rm -f "$log"
  return "$rc"
)

# --- nsc-cmse -------------------------------------------------------------
# Compiles every libs/ra8_nsc TU under -mcmse with -Wall -Wextra -Werror. The
# warning flags are load-bearing: a bare -fsyntax-only run is what let a
# veneer attribute clash go unnoticed. No app links the comms/eth veneers, so
# only this gate would catch an over-4-arg cmse_nonsecure_entry regression.
gate_nsc_cmse() (
  set -e
  use_pinned_arm_toolchain
  require_arm_gcc_m85
  bash scripts/checks/check_nsc_cmse.sh --selftest
  bash scripts/checks/check_nsc_cmse.sh
)

# --- sg-offsets -----------------------------------------------------------
# Guards the NSC Secure-Gateway veneer slot offsets in the linked SECURE ELF:
# the CMSE import library binds the NS image to those byte offsets, so a rename
# or reorder silently shifts every NS->Secure call (ld's stub order is not
# ascending symbol-name order, so it cannot be predicted from the names).
# Reads the build-cross output: tz_nsc_cgc_usb is the app whose veneer set the
# pinned EXPECTED_OFFSETS table was derived from. The structural one-veneer-per
# -slot rule runs on every secure image with an NSC region, and ra8d2-ereader /
# tz_threadx_demo now run the checker POST_BUILD themselves. Its NS image and
# every non-TZ app carry no veneers and the checker skips them.
gate_sg_offsets() (
  set -e
  local elf
  python3 scripts/checks/check_sg_offsets.py --selftest
  elf="$(find examples -type f -name 'tz_nsc_cgc_usb.elf' | head -n 1)"
  if [[ -z "$elf" ]]; then
    echo "check_sg_offsets: tz_nsc_cgc_usb secure ELF not found -- run the" >&2
    echo "                  build-cross gate first (this gate reads its output)." >&2
    return 1
  fi
  echo "check_sg_offsets: inspecting $elf"
  python3 scripts/checks/check_sg_offsets.py "$elf"
)

# --- stack-usage ----------------------------------------------------------
# Every app is compiled with -fstack-usage (cmake/ra8_warnings.cmake), so
# build-cross left a per-object .su file next to each object. Aggregate them
# project-wide and fail on any first-party frame over 2048 B, any `dynamic`
# (VLA/alloca) frame -- NASA P10 Rule 3 -- or any critical-path module
# (ra8_isr/ra8_check/ra8_err/ra8_mpu/ra8_cgc/ra8_pfs) over 256 B.
#
# The aggregator runs WITHOUT --allow-empty, so a sweep that finds no .su files
# or collapses below its function floor FAILS rather than passing vacuously
# (#386) -- a stack budget that went unmeasured must never read as clean. The
# --selftest runs first and asserts that empty/collapsed detection still fires,
# so a detector that quietly stopped matching cannot pass as a clean gate.
gate_stack_usage() (
  set -e
  python3 scripts/checks/stack_usage_check.py --selftest
  python3 scripts/checks/stack_usage_check.py --strict
)
