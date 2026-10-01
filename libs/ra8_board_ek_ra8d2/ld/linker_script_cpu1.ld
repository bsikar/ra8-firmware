/*
 * GNU ld linker script for the CPU1 (Cortex-M33) image of an EK-RA8D2 app.
 *
 * THE shared M33 map. Eight apps carried a byte-identical copy of this file,
 * differing only in the app name on the line above (RA8FW-309); ra8_add_cpu1_image()
 * now falls back here when an app has no linker_script_cpu1.ld of its own, so a
 * window change touches one file instead of eight. The two apps whose M33 image
 * needs external SDRAM (compile_on_m33, ereader_m33) keep a local script and
 * override this one.
 *
 * The two windows below are the same numbers cpu1_memory_map.cmake gives the
 * M85 side (RA8_CPU1_IMAGE_ORIGIN/LENGTH, RA8_CPU1_SRAM_ORIGIN/LENGTH). They are
 * spelled here as well because ld cannot read a CMake variable; check_board_
 * memory_map.py is what keeps the two spellings honest.
 *
 * MRAM_CPU1 0x020C0000 256 KiB  Code + rodata + vectors
 * SRAM_CPU1 0x22190000  64 KiB  .data, .bss, M33 main stack
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
