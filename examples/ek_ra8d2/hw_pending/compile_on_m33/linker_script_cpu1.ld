/*
 * GNU ld linker script for the CPU1 (Cortex-M33) image of compile_on_m33.
 *
 * MRAM_CPU1 0x020C0000 256 KiB  Code + rodata + vectors
 * SRAM_CPU1 0x22190000  64 KiB  .data, small .bss, M33 main stack
 * SDRAM     0x68000000  64 MiB  Working-set arenas (the 96 KiB ra8_epub pool)
 *
 * For the full EPUB->RABOOK1 compile the M33 links miniz + tinyxml2 + ra8_epub,
 * whose 96 KiB first-fit arena (ra8_epub_miniz_alloc / operator new) does not fit
 * the 64 KiB SRAM_CPU1; it is parked in external SDRAM via .sdram_bss below. The
 * M85 brings up the SDRAM controller before ra8_cpu1_release.
 *
 * Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

ENTRY(cpu1_reset_handler)

MEMORY
{
    MRAM_CPU1 (rx)  : ORIGIN = 0x020C0000, LENGTH = 256K
    SRAM_CPU1 (rwx) : ORIGIN = 0x22190000, LENGTH = 64K
    SDRAM     (rwx) : ORIGIN = 0x68000000, LENGTH = 64M
}

/* Deeper than the emitter-only PoC (tinyxml2 recursion + ra8_book walk frames),
 * so default to 24 KiB -- still well inside SRAM_CPU1 once the arena is in SDRAM. */
g_ra8_ls_cpu1_stack_size = DEFINED(g_ra8_ls_cpu1_stack_size) ? g_ra8_ls_cpu1_stack_size : 24K;
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
        *(EXCLUDE_FILE(*ra8_epub*) .bss*)
        *(COMMON)
        g_ra8_ls_cpu1_bss_end   = .;
    } > SRAM_CPU1

    /* newlib _sbrk references `end` as the heap base and must link. The ra8_epub
     * allocators are static pools, so malloc/sbrk are never actually called on
     * this image; `end` only resolves the symbol (heap grows toward the stack
     * if a stray malloc ever runs, which the static-pool design prevents). */
    PROVIDE(end  = .);
    PROVIDE(_end = .);

    /* The ra8_epub working-set arenas live in external SDRAM, kept OUT of the
     * .bss zero range above: the 96 KiB first-fit pool (ra8_epub_miniz_alloc
     * s_pool, shared by miniz + operator new) and the 48 KiB OPF/NCX scratch
     * (ra8_epub_open s_opf_buf) overflow the 64 KiB SRAM_CPU1. NOLOAD -- neither
     * needs startup zeroing (the pool installs its free-list head on first use;
     * the scratch is filled before each read), and both are unused until the M33
     * actually compiles (stage a1). The small first-party .bss stays in
     * SRAM_CPU1, zeroed by cpu1_reset_handler. */
    .sdram_bss (NOLOAD) : ALIGN(8)
    {
        g_ra8_ls_cpu1_sdram_start = .;
        *(.sdram_bss .sdram_bss.*)
        *ra8_epub*(.bss .bss.* COMMON)
        g_ra8_ls_cpu1_sdram_end = .;
    } > SDRAM

    .stack (NOLOAD) :
    {
        . = ORIGIN(SRAM_CPU1) + LENGTH(SRAM_CPU1) - g_ra8_ls_cpu1_stack_size;
        . = . + g_ra8_ls_cpu1_stack_size;
    } > SRAM_CPU1
}
