/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/*
 * GNU ld linker script for the CPU1 (Cortex-M33) image of cpu1_pingpong.
 *
 * MRAM_CPU1 0x020C0000 256 KiB  Code + rodata + vectors
 * SRAM_CPU1 0x22190000  64 KiB  .data, .bss, M33 main stack
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
