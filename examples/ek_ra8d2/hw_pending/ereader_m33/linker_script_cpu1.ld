/*
 * GNU ld linker script for the CPU1 (Cortex-M33) image of ereader_m33.
 *
 * MRAM_CPU1 0x020C0000 256 KiB  Code + rodata + vectors
 * SRAM_CPU1 0x22190000  64 KiB  .data, .bss, M33 main stack
 *
 * SRAM_CPU1 sits in the top 64 KiB of SRAM3, the last on-chip data-SRAM bank.
 * Per HUM Ch 58.1 "SRAM" Table 58.1 p 3527 the data SRAM banks are SRAM0..SRAM2
 * (512 KiB each, 0x22000000..0x22180000) and SRAM3 (128 KiB, 0x22180000..
 * 0x221A0000); the ECC syndrome aliases begin at 0x221A0000. The Cortex-M33
 * reads its initial main stack pointer from the first word of its vector table
 * (Armv8-M reset behaviour), so SRAM_CPU1's top -- 0x221A0000 -- is that SP. It
 * must therefore land inside real data SRAM (0x22000000..0x221A0000), which the
 * top of SRAM3 satisfies while staying clear of the M85 image (SRAM0+SRAM1,
 * 0x22000000..0x22100000) and the shared mailbox at 0x22100000.
 *
 * Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

ENTRY(cpu1_reset_handler)

MEMORY
{
    MRAM_CPU1 (rx)  : ORIGIN = 0x020C0000, LENGTH = 256K
    /* HUM Ch 58.1 "SRAM" Table 58.1 p 3527 -- top 64 KiB of SRAM3, the last
     * on-chip data-SRAM bank (0x22180000..0x221A0000). */
    SRAM_CPU1 (rwx) : ORIGIN = 0x22190000, LENGTH = 64K
}

g_ra_ls_cpu1_stack_size = DEFINED(g_ra_ls_cpu1_stack_size) ? g_ra_ls_cpu1_stack_size : 8K;
g_ra_ls_cpu1_stack_top  = ORIGIN(SRAM_CPU1) + LENGTH(SRAM_CPU1);

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
        g_ra_ls_cpu1_data_start = .;
        *(.data*)
        g_ra_ls_cpu1_data_end   = .;
    } > SRAM_CPU1 AT > MRAM_CPU1
    g_ra_ls_cpu1_data_load = LOADADDR(.data);

    .bss : ALIGN(4)
    {
        g_ra_ls_cpu1_bss_start = .;
        *(.bss*)
        *(COMMON)
        g_ra_ls_cpu1_bss_end   = .;
    } > SRAM_CPU1

    .stack (NOLOAD) :
    {
        . = ORIGIN(SRAM_CPU1) + LENGTH(SRAM_CPU1) - g_ra_ls_cpu1_stack_size;
        . = . + g_ra_ls_cpu1_stack_size;
    } > SRAM_CPU1
}
