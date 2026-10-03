# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# The EK-RA8D2 in-image Non-Secure window: where the .ns_* sections of a
# SINGLE-IMAGE TrustZone app land (RA8FW-309).
#
# Not to be confused with libs/ra8_board_ek_ra8d2/ld/ns_memory_map.cmake, which
# sizes the SEPARATE NS ELF that ra8_add_ns_image() links. The two schemes are
# genuinely different images: there, the NS world is its own file with its own
# vector table and load address; here, the NS code rides inside the Secure ELF
# in sections the SAU later reclassifies. An app uses one or the other, never
# both, and the addresses differ because the inline one has to tile around the
# CPU1 image in the same flash.

# No include_guard: this module DEFINES VARIABLES rather than functions, and a
# global guard would make every include after the first a silent no-op, leaving
# the caller's scope empty. Same reasoning as cpu1_memory_map.cmake.

# NS code + rodata. 256K, not the 512K the board map's NS_MRAM placeholder
# carries, because a single-image dual-core app also flashes the CPU1 image at
# 0x020C0000: 0x02080000 + 512K would run straight through it. Tiling is
# 512K Secure | 256K NS | 256K CPU1.
# The window RUNS at the bit-28 Non-secure alias and LOADS at the physical
# address: the RA8 IDAU keeps every bit-28-clear address Secure and the SAU
# cannot lower it (RA8FW-510), while a flasher writes physical MRAM and the
# tiling above is physical. The two name the same bytes.
set(RA8_NS_INLINE_MRAM_ORIGIN 0x12080000)
set(RA8_NS_INLINE_MRAM_LOAD 0x02080000)
set(RA8_NS_INLINE_MRAM_LENGTH 256K)

# NS data. Capped so it ends at 0x22190000, which is ON-chip (the physical ECC
# SRAM array is 0x22000000..0x221A0000, 1.6 MB) and below the CPU1/M33 bank
# that begins at 0x22190000.
#
# This bound is a SILICON FIX, not a tidy-up, and it is the reason this window
# is pinned here rather than inherited from the board map's 640K NS_SRAM
# placeholder. A 1024K length put the top of the window -- and therefore the NS
# initial MSP in g_ra8_ls_ns_stack_top -- at 0x22200000, 384K past the end of
# the array. On silicon the BLXNS BusFaulted on the very first NS stack push,
# before the .ns_bss zero loop had run. ra8_emulator did not reproduce it: its
# flat SRAM window makes 0x22200000 a perfectly valid address. Do not widen
# this to fill the placeholder.
# Named at the bit-28 alias for the same reason: 0x32100000..0x32190000 is
# 0x22100000..0x22190000, the bound below still applies to it.
set(RA8_NS_INLINE_SRAM_ORIGIN 0x32100000)
set(RA8_NS_INLINE_SRAM_LENGTH 576K)
