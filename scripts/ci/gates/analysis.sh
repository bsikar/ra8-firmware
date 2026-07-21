# shellcheck shell=bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
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
  cppcheck --enable=warning,style,performance,portability \
    --error-exitcode=1 \
    "${suppress_args[@]}" \
    --inline-suppr \
    -i libs/third_party \
    --std=c23 \
    src libs "${apps[@]}"
)

# --- misra ----------------------------------------------------------------
# misra_check.sh (cppcheck misra.py addon) over libs/ src/ port/, then
# misra_ratchet.py compares per-file-per-rule finding counts against
# .github/misra-baseline.txt. `make cppcheck` is NOT a substitute: different
# rule set, no addon, no baseline, so a new MISRA finding sails through it.
gate_misra() (
  set -e
  require_cmd cppcheck
  bash scripts/checks/misra_check_inner.sh
  python3 scripts/checks/misra_ratchet.py --check
)

# --- tidy -----------------------------------------------------------------
gate_tidy() {
  bash scripts/checks/clang_tidy.sh --check --verbose
}

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
gate_stack_usage() (
  set -e
  if [[ -z "$(find . -name '*.su' -print -quit)" ]]; then
    echo "stack_usage_check: no .su files found -- run the build-cross gate" >&2
    echo "                   first (this gate reads its output)." >&2
    return 1
  fi
  python3 scripts/checks/stack_usage_check.py --strict
)
