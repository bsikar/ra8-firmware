#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
# shellcheck shell=bash
#
# scripts/ci/gates/analysis.sh -- Link-time structure: CMSE, SG offsets, stack usage.
#
# SOURCED, NEVER EXECUTED. scripts/ci.sh sources every file in this directory
# and is the only entry point; RA8_GATE_REGISTRY -- the single list of what
# gates exist -- stays there too. These files hold gate BODIES only, so there
# is still exactly one home for a gate's definition and exactly one command
# for a workflow to call (`just quality::local::gate <name>`). Adding a second
# registry here would recreate the drift the single-definition rule exists to
# prevent.
#
# Gates in this file: nsc-cmse, sg-offsets, stack-usage

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
