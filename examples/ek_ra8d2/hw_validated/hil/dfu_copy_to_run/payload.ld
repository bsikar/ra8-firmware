/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/*
 * Copy-to-run demo payload linker script.
 *
 * Links at the dfu_bootloader's SRAM run base (k_ra8_dfu_run_base = 0x22020000,
 * see libs/ra8_dfu/inc/ra8_dfu.h). The bootloader copies a staged slot body here
 * and branches to it, so the IDENTICAL image is bootable from either slot -- no
 * per-slot link base. ORIGIN MUST equal k_ra8_dfu_run_base.
 */

ENTRY(payload_reset)

MEMORY
{
    /* 256 KiB run window inside the 2 MiB SRAM; well under k_ra8_dfu_img_max. */
    RUN (rwx) : ORIGIN = 0x22020000, LENGTH = 256K
}

/* Initial MSP for the payload: top of the run window (stack grows down). */
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
        *(.rodata)
        *(.rodata.*)
    } > RUN

    /DISCARD/ :
    {
        *(.ARM.*)
        *(.comment)
        *(.note*)
        *(.eh_frame*)
    }
}
