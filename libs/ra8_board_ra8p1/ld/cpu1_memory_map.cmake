# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# The RA8P1 CPU1 (Cortex-M33) image window (RA8FW-496). The RA8P1 has no fixed
# CPU1 code window: Renesas R01AN7880 Rev.1.01 Figure 6 (p 11) shows one
# 1 Mbyte code MRAM user area at 0x0200_0000..0x020F_FFFF, and section 6.3
# (p 50) leaves the CPU0/CPU1 split to the developer. This takes the EK-RA8D2
# split, the same numbers as ld/linker_script_cpu1.ld here:
#
#   CPU1 code  0x020C0000  256 KiB  (ends at the last byte of the user area)
#   CPU1 data  0x22190000   64 KiB  (CPU1's bank in system_init region 4)
#
# The M85 side stops at 0x020C0000 through the app's MRAM_LENGTH 768K.
# No include_guard, for the reason the EK-RA8D2 module gives: it defines
# variables, and each app that composes a CPU1 image needs them in its scope.
# tests/zig_build_graph/ra8p1_cpu1_ld_test.zig holds every set() below equal to
# the EK-RA8D2 module's.

set(RA8_CPU1_IMAGE_ORIGIN 0x020C0000)
set(RA8_CPU1_IMAGE_LENGTH 256K)
set(RA8_CPU1_SRAM_ORIGIN 0x22190000)
set(RA8_CPU1_SRAM_LENGTH 64K)
set(RA8_CPU1_STACK_TOP_EXPR "${RA8_CPU1_SRAM_ORIGIN} + ${RA8_CPU1_SRAM_LENGTH}")
