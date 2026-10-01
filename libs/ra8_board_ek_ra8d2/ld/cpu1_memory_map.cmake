# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# The EK-RA8D2 CPU1 (Cortex-M33) image window: ONE definition of the two
# addresses that were repeated across nine hand-maintained linker scripts
# (RA8FW-309).
#
# These are the same numbers as k_ra8_board_cpu1_image_base and
# k_ra8_board_cpu1_image_size_bytes in ra8_board_ek_ra8d2_dualcore.h. The
# header is what C code reads; this is what the generated linker fragment is
# built from. Both describe the board, so both live in the board layer.

# No include_guard: this module DEFINES VARIABLES rather than functions, and a
# global guard would make every include after the first a silent no-op, leaving
# the caller's scope empty. Re-including is the point -- each app that composes
# a CPU1 image needs these in its own scope.

# Where the linked M33 image is flashed. The M33 vector table sits at this
# address, so ra8_cpu1_release() starts the core from here.
set(RA8_CPU1_IMAGE_ORIGIN 0x020C0000)
set(RA8_CPU1_IMAGE_LENGTH 256K)

# Where the M33's own data lives. Every linker_script_cpu1.ld in the tree
# declares exactly this window (ten of them, all identical), because it is the
# top 64 KiB of SRAM3, the last on-chip data-SRAM bank: the ECC syndrome
# aliases begin at 0x221A0000, so this is the highest real data SRAM there is.
set(RA8_CPU1_SRAM_ORIGIN 0x22190000)
set(RA8_CPU1_SRAM_LENGTH 64K)

# The M33's initial stack pointer, handed to ra8_cpu1_release() as its `sp`
# argument. It MUST equal the value the M33's own script computes for
# g_ra8_ls_cpu1_stack_top, which is ORIGIN(SRAM_CPU1) + LENGTH(SRAM_CPU1) --
# the top of the window above.
#
# This was previously "ORIGIN(SRAM) + LENGTH(SRAM) - 16K", which is a point in
# SRAM0 (0x220FBF00) and not where the M33 stack is at all. It reached that
# spelling honestly: it reads as "16K below the top of usable SRAM", and on the
# M85 side that sentence is true of SRAM0. But the M33 does not live in SRAM0,
# so the M85 was handing ra8_cpu1_release() an address in the wrong bank. It
# went unnoticed because ra8_cpu1_release() only null- and 8-byte-align-checks
# `sp` and then never uses it: the M33 latches its real MSP from the first word
# of its own vector table (Armv8-M reset). Both the old and new values pass
# those two checks, so this is a correctness fix, not a behaviour fix, and no
# HIL result changes. Derived from the window above so the two sides cannot
# drift apart again.
set(RA8_CPU1_STACK_TOP_EXPR "${RA8_CPU1_SRAM_ORIGIN} + ${RA8_CPU1_SRAM_LENGTH}")
