/*
 * GNU ld linker script for ra8d2-firmware
 *
 * Target: Renesas R7KA8D2KF (RA8D2) on EK-RA8D2
 *   - Arm Cortex-M85 primary core
 *   - Arm Cortex-M33 secondary core (not used yet)
 *
 * Memory map (from RA8D2 Hardware User's Manual R01UH1065EJ):
 *
 *   Region        Start       Size       Usage
 *   ------------  ----------  ---------  ----------------------------
 *   MRAM          0x02000000  1 MiB      Code + rodata + vectors
 *   SRAM (ECC)    0x22000000  2 MiB      .data, .bss, main stack, heap
 *   ITCM          0x00000000  64 KiB     (reserved -- not used yet)
 *   DTCM          0x20000000  64 KiB     (reserved -- not used yet)
 *
 * TODO: confirm ITCM / DTCM sizes against HUM section 5 once the
 * first real application hits them; sizes above are conservative
 * lower bounds that will never over-extend.
 *
 * The linker script also emits the option-setting sections used by
 * the RA boot ROM (OFS0, OFS1, OFS2, OFS3, SAS) at fixed flash
 * addresses -- see HUM section 4 for the full table. These default to
 * all-ones (erased) and are filled in by `bsp_linker.c` if a project
 * provides the `BSP_CFG_OPTION_SETTING_*` macros.
 *
 * Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

ENTRY(Reset_Handler)

MEMORY
{
    MRAM  (rx)  : ORIGIN = 0x02000000, LENGTH = 1024K
    SRAM  (rwx) : ORIGIN = 0x22000000, LENGTH = 2048K
}

/* Main stack at the top of SRAM. */
_stack_size = 8K;
_stack_top  = ORIGIN(SRAM) + LENGTH(SRAM);

SECTIONS
{
    /* ---- Vector table ---- */
    .vectors : ALIGN(4)
    {
        KEEP(*(.vectors))
        KEEP(*(.vectors.*))
    } > MRAM

    /* ---- Code + const data ---- */
    .text : ALIGN(4)
    {
        *(.text)
        *(.text.*)
        *(.glue_7)
        *(.glue_7t)
    } > MRAM

    .rodata : ALIGN(4)
    {
        *(.rodata)
        *(.rodata.*)
    } > MRAM

    .ARM.extab : ALIGN(4)
    {
        *(.ARM.extab* .gnu.linkonce.armextab.*)
    } > MRAM

    .ARM.exidx : ALIGN(4)
    {
        __exidx_start = .;
        *(.ARM.exidx* .gnu.linkonce.armexidx.*)
        __exidx_end = .;
    } > MRAM

    _etext = .;

    /* ---- Initialised data: loaded from MRAM, lives in SRAM ---- */
    .data : ALIGN(4)
    {
        _sdata  = .;
        *(.data)
        *(.data.*)
        _edata  = .;
    } > SRAM AT > MRAM

    _sidata = LOADADDR(.data);

    /* ---- Zero-init data ---- */
    .bss (NOLOAD) : ALIGN(4)
    {
        _sbss = .;
        *(.bss)
        *(.bss.*)
        *(COMMON)
        _ebss = .;
    } > SRAM

    /* ---- Heap (not used; keeps linker happy about newlib) ---- */
    .heap (NOLOAD) : ALIGN(8)
    {
        _sheap = .;
        . = . + 0x1000;
        _eheap = .;
    } > SRAM

    /* ---- Discarded ---- */
    /DISCARD/ :
    {
        *(.note.GNU-stack)
        *(.gnu*)
    }
}
