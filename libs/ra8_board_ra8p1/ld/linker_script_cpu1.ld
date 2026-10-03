/*
 * GNU ld linker script for the CPU1 (Cortex-M33) image of an RA8P1 app
 * (RA8FW-496).
 *
 * The RA8P1 has no fixed CPU1 code window. Renesas R01AN7880 Rev.1.01,
 * Figure 6 (p 11) "RA8 Dual-Core Map of the Code MRAM Memory in Linear Mode"
 * shows one 1 Mbyte code MRAM user area at 0x0200_0000..0x020F_FFFF (secure
 * alias), with 0x0210_0000 and up reserved (bus error), and section 6.3
 * (p 50) leaves the CPU0/CPU1 split to the developer. This file takes the
 * EK-RA8D2 split, since ra8_board_ra8p1/ld/linker_script.ld already shares
 * the RA8D2 map:
 *
 * MRAM_CPU1 0x020C0000 256 KiB  Code + rodata + vectors (ends at the last
 *                               byte of Figure 6's user area)
 * SRAM_CPU1 0x22190000  64 KiB  .data, .bss, M33 main stack: CPU1's private
 *                               bank inside system_init's non-cacheable
 *                               region 4 (0x22100000..0x2219FFFF)
 *
 * Everything from ENTRY down is byte-identical to the EK-RA8D2 CPU1 script;
 * tests/zig_build_graph/ra8p1_cpu1_ld_test.zig holds it that way.
 *
 * Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

ENTRY(cpu1_reset_handler)

MEMORY
{
    MRAM_CPU1 (rx)  : ORIGIN = 0x020C0000, LENGTH = 256K
    SRAM_CPU1 (rwx) : ORIGIN = 0x22190000, LENGTH = 64K
}

g_ra8_ls_cpu1_stack_size = DEFINED(g_ra8_ls_cpu1_stack_size) ? g_ra8_ls_cpu1_stack_size : 8K;
g_ra8_ls_cpu1_stack_top  = ORIGIN(SRAM_CPU1) + LENGTH(SRAM_CPU1);

SECTIONS
{
    .cpu1_vectors ORIGIN(MRAM_CPU1) :
    {
        KEEP(*(.cpu1_vectors))
    } > MRAM_CPU1

    .text :
    {
        *(.text*)
        *(.rodata*)
    } > MRAM_CPU1

    .data : ALIGN(4)
    {
        g_ra8_ls_cpu1_data_start = .;
        *(.data*)
        g_ra8_ls_cpu1_data_end   = .;
    } > SRAM_CPU1 AT > MRAM_CPU1
    g_ra8_ls_cpu1_data_load = LOADADDR(.data);

    .bss : ALIGN(4)
    {
        g_ra8_ls_cpu1_bss_start = .;
        *(.bss*)
        *(COMMON)
        g_ra8_ls_cpu1_bss_end   = .;
    } > SRAM_CPU1

    .stack (NOLOAD) :
    {
        . = ORIGIN(SRAM_CPU1) + LENGTH(SRAM_CPU1) - g_ra8_ls_cpu1_stack_size;
        . = . + g_ra8_ls_cpu1_stack_size;
    } > SRAM_CPU1
}
