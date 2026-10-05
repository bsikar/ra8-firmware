/*
 * GNU ld linker script for txm_reload_cpu1's CPU1 (Cortex-M33) image
 * (RA8FW-774, copied from txm_manager_cpu1).
 *
 * The board's shared M33 map (libs/ra8_board_ek_ra8d2/ld/linker_script_cpu1.ld)
 * plus one section: `.txm_module`, the packed txm_hello_m33 module the Module
 * Manager loads in place. It sits in MRAM after the code, 1 KiB aligned so the
 * module's code region meets any MPU region alignment, and
 * g_ra8_ls_cpu1_txm_module_start names its first byte (the preamble).
 *
 * MRAM_CPU1 0x020C0000 256 KiB  Code + rodata + vectors + the module
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

    .txm_module : ALIGN(1024)
    {
        g_ra8_ls_cpu1_txm_module_start = .;
        KEEP(*(.txm_module))
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
