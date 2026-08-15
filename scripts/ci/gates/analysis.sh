#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# shellcheck shell=bash
#
# scripts/ci/gates/analysis.sh -- Static analysis and link-time structure: cppcheck, MISRA, clang-tidy, CMSE.
#
# SOURCED, NEVER EXECUTED. scripts/ci.sh sources every file in this directory
# and is the only entry point; RA8_GATE_REGISTRY -- the single list of what
# gates exist -- stays there too. These files hold gate BODIES only, so there
# is still exactly one home for a gate's definition and exactly one command
# for a workflow to call (bash scripts/ci.sh --gate <name>). Adding a second
# registry here would recreate the drift the single-definition rule exists to
# prevent.
#
# Gates in this file: cppcheck, misra, tidy, nsc-cmse, sg-offsets, stack-usage

# --- cppcheck -------------------------------------------------------------
# cppcheck 2.13 (Ubuntu 24.04) is finicky about the --suppressions-list
# parser; convert each non-comment, non-blank line into an explicit
# --suppress= flag, the syntax every version accepts. examples/host/* are
# macOS-only dev tools (C23 nullptr + AppKit), not cross-compiled firmware.
gate_cppcheck() (
  set -e
  require_cmd cppcheck
  # cppcheck findings drift by version (2.10 / 2.13 / 2.21 each differ); the
  # tree is clean only on the pinned 2.13, so a wrong build must fail loud (#333).
  require_tool_versions cppcheck
  local apps=() dir line
  if [[ -d examples ]]; then
    for dir in examples/*/*/ examples/*/*/*/ examples/*/*/*/*/; do
      dir="${dir%/}"
      case "$dir" in examples/host/*) continue ;; esac
      [[ -f "$dir/main.c" ]] && apps+=("$dir")
    done
  fi
  local suppress_args=()
  while IFS= read -r line; do
    line="${line%$'\r'}"
    line="${line## }"
    line="${line%% }"
    [[ -z "$line" ]] && continue
    case "$line" in \#*) continue ;; esac
    suppress_args+=("--suppress=$line")
  done <.cppcheck-suppressions
  # The annotation macros must be RESOLVABLE, or this scan stops parsing.
  # RA8_OWNS_RESOURCE(kind) / RA8_DI_SLOT(role) are FUNCTION-LIKE macros
  # written at file scope; this invocation passes no header search path, so
  # `#include "ra8_attributes.h"` never resolves and cppcheck cannot tell them
  # from a call -- it abandons the translation unit with unknownMacro. The
  # object-like spellings (RA8_INTERNAL, RA8_PRIV) had always parsed anyway,
  # which is why the gap survived until libs/ra8_fs became the first in-scope
  # library to use the function-like forms and took seven TUs red at once.
  #
  # Force-include the ONE header that defines them rather than restating
  # -DRA8_OWNS_RESOURCE(x)= here: a hand-list of names drifts silently the next
  # time an annotation is added, and ra8_attributes.h already carries a
  # `!defined(__CPPCHECK__)` arm so every macro collapses to nothing under this
  # scan. A full -I search path is deliberately NOT the fix: it lets cppcheck
  # see through every project header at once and surfaces a large batch of
  # previously-unanalysed findings, which is a scope change rather than a
  # green-up. The MISRA half never fired because misra_check_inner.sh derives
  # real -I roots from the repo layout and resolves the include for real.
  local attrs_header=libs/ra8_core/inc/ra8_attributes.h
  if [[ ! -f "$attrs_header" ]]; then
    echo "cppcheck: $attrs_header not found -- the RA8_* annotation macros" >&2
    echo "  would go unresolved and every TU using a function-like one would" >&2
    echo "  fail with unknownMacro. Update this path if the header moved." >&2
    return 1
  fi
  # tools/ is held to the same bar as the firmware (CLAUDE.md), so it is IN
  # scope here -- not just tools/ra8_emulator, and not behind the
  # --error-exitcode=0 escape where a finding cannot fail anything (#596). The
  # host tools are the only first-party code that opens files and calls malloc,
  # so they are the only code that trips a cppcheck 2.13 blind spot: it does
  # not model the C23 `nullptr` keyword as a null constant and reports a
  # false-positive resourceLeak/memleak on every resource guarded by an
  # `if (p == nullptr)` return. cppcheck_c23_compat.h maps the keyword to the
  # classic null constant for the analyser only, force-included exactly as the
  # annotation header is: a command-line `-Dnullptr=NULL` would disable the
  # configuration exploration that the ra8p1 examples rely on, and a guarded
  # `#ifdef __CPPCHECK__` would be explored both ways and re-surface the false
  # positives. See that header for the full rationale.
  local compat_header=scripts/checks/cppcheck_c23_compat.h
  if [[ ! -f "$compat_header" ]]; then
    echo "cppcheck: $compat_header not found -- the C23 nullptr keyword would" >&2
    echo "  read as a non-null symbol and every nullptr-guarded fopen/malloc" >&2
    echo "  in tools/ would fail with a false-positive resourceLeak. Update" >&2
    echo "  this path if the shim moved." >&2
    return 1
  fi
  cppcheck --enable=warning,style,performance,portability \
    --error-exitcode=1 \
    "${suppress_args[@]}" \
    --inline-suppr \
    --include="$attrs_header" \
    --include="$compat_header" \
    -i libs/third_party \
    --std=c23 \
    src libs tools "${apps[@]}"
)

# --- scan-build -----------------------------------------------------------
# The clang static analyzer over the host unit-test build and every CMake host
# tool: path-sensitive symbolic execution, so it finds the null-deref / leak /
# garbage-read PATHS cppcheck's pattern matching cannot.
#
# docs/STATIC_ANALYSIS.md claimed "CI runs bash scripts/checks/scan_build.sh
# --strict" for months while no workflow ran it and RA8_GATE_REGISTRY had no
# such gate (#532); scripts/git/pre-commit even carried a comment saying so, so
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

# --- misra ----------------------------------------------------------------
# misra_check.sh (cppcheck misra.py addon) over libs/ src/ port/ tools/, then
# misra_ratchet.py compares per-file-per-rule finding counts against
# .github/misra-baseline.txt. `make cppcheck` is NOT a substitute: different
# rule set, no addon, no baseline, so a new MISRA finding sails through it.
#
# check_misra_deviations.py holds the deviation register's derived numbers
# to the committed baseline + suppression list (#632: the register claimed
# 166 Rule-8.4 findings while the baseline held 1873, and nothing checked
# it). It reads only committed files, so it runs BEFORE the slow scan to
# fail fast, selftest first per the house rule.
gate_misra() (
  set -e
  require_cmd cppcheck
  # The MISRA baseline records the cppcheck version it was generated with;
  # a drifted cppcheck ratchets against the wrong findings (#333).
  require_tool_versions cppcheck
  python3 scripts/checks/misra_ratchet.py --selftest
  python3 scripts/checks/check_misra_deviations.py --selftest
  python3 scripts/checks/check_misra_deviations.py --check
  bash scripts/checks/misra_check_inner.sh
  python3 scripts/checks/misra_ratchet.py --check
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
  use_pinned_arm_toolchain
  require_arm_gcc_m85
  require_cmd cmake
  # clang_tidy.sh prefers clang-tidy-18 (the pinned clang-tools-18 major); assert
  # it resolves and is really major 18, so a box with only a newer bare
  # clang-tidy cannot lint under a different major and drift the ratchet (#333).
  require_tool_versions clang-tidy-18
  bash scripts/checks/clang_tidy.sh --selftest
  python3 scripts/checks/tidy_ratchet.py --selftest

  local log rc
  log="$(mktemp)"
  rc=0
  bash scripts/checks/clang_tidy.sh --check --verbose >"$log" 2>&1 || rc=$?
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
  bash scripts/checks/check_nsc_cmse.sh
)

# --- sg-offsets -----------------------------------------------------------
# The only automated guard that the NSC Secure-Gateway veneer slot offsets in
# the linked SECURE ELF still match the k_sg_off_* enum ns_main.c reaches them
# by (ld emits the 8-byte stubs in ascending symbol order, so a rename or
# reorder silently shifts the slots). Reads the build-cross output:
# tz_nsc_cgc_usb is the app that binds all three ra8_nsc_cgc_* veneers. Its NS
# image and every non-TZ app carry no veneers and the checker skips them.
gate_sg_offsets() (
  set -e
  local elf
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
