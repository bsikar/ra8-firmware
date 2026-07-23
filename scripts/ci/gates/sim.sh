# shellcheck shell=bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# scripts/ci/gates/sim.sh -- board_sim: boot smoke, the io-fabric demos, and SIL/HIL parity runs.
#
# SOURCED, NEVER EXECUTED. scripts/ci.sh sources every file in this directory
# and is the only entry point; RA8_GATE_REGISTRY -- the single list of what
# gates exist -- stays there too. These files hold gate BODIES only, so there
# is still exactly one home for a gate's definition and exactly one command
# for a workflow to call (bash scripts/ci.sh --gate <name>). Adding a second
# registry here would recreate the drift the single-definition rule exists to
# prevent.
#
# Gates in this file: board-sim-smoke, board-sim-matrix, board-sim-io-fabric,
#                     sil-integration

# --- board-sim-smoke ------------------------------------------------------
# board_sim (tools/board_sim) boots the real cross-compiled .elf on an
# emulated Cortex-M with the RA8D2 peripheral space modelled, asserting each
# image reaches its main loop without faulting (no invalid opcode, no unmapped
# access) and is not parked in a panic/fault halt.
gate_board_sim_smoke() (
  set -e
  use_pinned_arm_toolchain
  # Refuse to run on an unpinned Unicorn -- the emulator's decode of Armv8.1-M
  # is version-specific, so a fossil libunicorn produces an unreproducible
  # verdict (#354). Fail loudly here rather than silently boot on the wrong one.
  require_pinned_unicorn
  # Prove the gate is WIRED before trusting a green run. Every app class is
  # dispatched through a table, so a dropped entry stops that class being
  # asserted at all -- the app still builds, still runs, and still prints OK
  # from the generic path while the check it exists for never happens.
  bash scripts/sim/smoke.sh --selftest
  bash scripts/sim/smoke.sh
)

# --- board-sim-matrix -----------------------------------------------------
# The BREADTH gate to board-sim-smoke's depth: every example under
# examples/ek_ra8d2/ is built and booted on the emulator, and the failing count
# is ratcheted DOWNWARD against a committed baseline. Growth fails; shrinking
# is free (and prints a re-baseline notice).
#
# This measures #67's own headline success criterion -- "every example runs in
# the simulator". matrix.sh has existed and been well-formed for months while
# being invoked by NOTHING: not ci.sh, not a workflow, not the Makefile. That
# is this repo's dominant defect class applied to the epic's definition of
# done, which is why the fix is an enforcing gate and not a report.
#
# It is a ratchet rather than "zero faults", because a zero-faults rule would
# have to stay unwired until the last known fault is burned down -- i.e. it
# would measure nothing in the meantime, which is the same hole in a new shape.
# It is not an allowlist: no baseline row is a permanent exemption and the end
# state is an empty baseline.
#
# SPEED. This boots ~200 images and is firmly in the `slow` class; see the
# runtime note in matrix.sh. --selftest runs FIRST and costs nothing, so a
# classifier that stopped distinguishing truncation from failure is caught
# before the expensive sweep rather than after it.
gate_board_sim_matrix() (
  set -e
  use_pinned_arm_toolchain
  require_pinned_unicorn
  bash scripts/sim/matrix.sh --selftest
  bash scripts/sim/matrix_triage.sh --selftest
  python3 scripts/checks/matrix_ratchet.py --selftest
  # matrix.sh exits non-zero whenever the sweep is not perfectly clean, which is
  # the right default for a human running it by hand but is NOT this gate's
  # verdict -- the ratchet is. Capture the report either way, then judge.
  bash scripts/sim/matrix.sh || true
  # Print the remaining failures grouped by CAUSE before the verdict, so every
  # run of the gate leaves a burn-down plan in the log rather than a bare count
  # someone has to go re-derive. Diagnostic: it must not decide the gate.
  bash scripts/sim/matrix_triage.sh || true
  python3 scripts/checks/matrix_ratchet.py --check
)

# --- board-sim-io-fabric --------------------------------------------------
# ra8_io fabric (#155) end-to-end: every storage backend driven through the
# same VFS API (block device -> ra8_fs FAT format/mount -> VFS mkdir + nested
# file round-trip), plus the format registry, the LRU sector cache, and the
# DEFLATE stream, asserted by each demo's PASS banner. Covers RAM/SRAM,
# external SDRAM, SD-over-SPI, native SDHI and OSPI NOR (erase-before-write).
#
# ra8_io_mram_demo is deliberately NOT in this list (#170): it targets a
# general-purpose data-flash at 0x2700_0000 that the RA8D2 does not have (HUM
# Ch 5 Figure 5.2 p 237 calls the region "Extra MRAM (option-setting memory)";
# HUM Ch 59.7.4.5 Table 59.15 p 3592 enumerates the legal MACI Program targets
# and they are all option-setting / OTP). The bench returns Error=516 with the
# sequencer command-locked, board_sim now reproduces that rejection, and a gate
# that asserted its PASS banner was claiming a storage backend the part lacks.
gate_board_sim_io_fabric() (
  set -e
  use_pinned_arm_toolchain
  require_pinned_unicorn
  bash scripts/sim/smoke.sh \
    ra8_io_demo ra8_io_sdram_demo ra8_io_compress_demo \
    ra8_io_sd_demo ra8_io_sdhi_demo ra8_io_xspi_demo \
    ra8_io_fsfmt_demo ra8_io_cache_demo
)

# --- sil-integration ------------------------------------------------------
# The hardware-free mirror of the bench HIL suite: every app under
# examples/ek_ra8d2/hw_validated/hil/ booted in board_sim headless and checked
# against the SAME per-app hil.conf the real board is checked against.
# ENFORCING -- a board_sim modelling gap or a firmware regression fails here
# rather than being logged and ignored.
gate_sil_integration() (
  set -e
  use_pinned_arm_toolchain
  require_pinned_unicorn
  # ereader_shelf compiles against a COMMITTED generated MRAM library header.
  # The bake is not reproducible across architectures (libjpeg SIMD decode
  # rounding differs x86_64 vs Apple silicon at the same Pillow version), so
  # the fixture is tracked; assert it is present so a future re-gitignore
  # fails loudly here instead of as a confusing app FAIL.
  test -s examples/ek_ra8d2/hw_validated/hil/ereader_shelf/library.h
  bash scripts/sim/sil_all.sh -j "$(ra8_max_jobs)"
)
