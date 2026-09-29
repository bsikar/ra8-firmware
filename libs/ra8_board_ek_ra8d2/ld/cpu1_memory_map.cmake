# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# The EK-RA8D2 CPU1 (Cortex-M33) image window: ONE definition of the two
# addresses that were repeated across nine hand-maintained linker scripts
# (#742).
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

# The M33 stack grows down from the top of usable SRAM. 16K below the top,
# leaving the region above it to the M85. Expressed against the board map's own
# SRAM region rather than as a literal, so it tracks a change to that region
# instead of going stale the way the nine forks did.
set(RA8_CPU1_STACK_TOP_EXPR "ORIGIN(SRAM) + LENGTH(SRAM) - 16K")
