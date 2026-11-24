/*
 * Copy-to-run confirm sample payload linker script.
 *
 * Links ONCE at the dfu_bootloader's SRAM run base (k_ra8_dfu_run_base =
 * 0x22020000). The bootloader copies the binary straight to SRAM, so
 * .vectors, .text, .sram_text, .rodata, and .data land directly at runtime
 * addresses and payload_reset only needs to zero .bss.
 *
 * Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

ENTRY(payload_reset)

MEMORY
{
    RUN (rwx) : ORIGIN = 0x22020000, LENGTH = 256K
}

g_ra8_ls_stack_top = ORIGIN(RUN) + LENGTH(RUN);

SECTIONS
{
    .vectors : ALIGN(4)
    {
        KEEP(*(.vectors))
    } > RUN

    .text : ALIGN(4)
    {
        *(.text)
        *(.text.*)
        *(.sram_text)
        *(.sram_text.*)
        *(.rodata)
        *(.rodata.*)
    } > RUN

    .data : ALIGN(4)
    {
        *(.data)
        *(.data.*)
    } > RUN

    .bss (NOLOAD) : ALIGN(4)
    {
        g_payload_sbss = .;
        *(.bss)
        *(.bss.*)
        *(COMMON)
        g_payload_ebss = .;
    } > RUN

    /DISCARD/ :
    {
        *(.ARM.*)
        *(.comment)
        *(.note*)
        *(.eh_frame*)
    }
}
