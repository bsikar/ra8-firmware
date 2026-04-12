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
 *   MRAM          0x02000000  1 MiB      Code + rodata + vectors + OFS
 *   DATA_FLASH    0x27000000  16 KiB     Data flash (EEPROM emulation)
 *   ITCM          0x00000000  64 KiB     Instruction tightly-coupled mem
 *   DTCM          0x20000000  64 KiB     Data tightly-coupled mem
 *   SRAM (ECC)    0x22000000  2 MiB      .data, .bss, main stack, heap
 *   SDRAM         0x68000000  64 MiB     External SDRAM (EK board)
 *
 * The linker script emits the RA boot-ROM option setting regions
 * (OFS0, OFS1, OFS2, OFS3, SAS) at the fixed addresses described in
 * HUM section 4 ("Option Function Select Registers"). Each sits at
 * its canonical offset inside the MRAM window and is filled in by
 * `libs/ra_hal/src/ra_ofs.c` (or by a vendor programming tool).
 * When no value is supplied, the region is left as all-ones (erased).
 *
 * Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

ENTRY(Reset_Handler)

MEMORY
{
    MRAM       (rx)  : ORIGIN = 0x02000000, LENGTH = 1024K
    DATA_FLASH (rw)  : ORIGIN = 0x27000000, LENGTH = 16K
    ITCM       (rwx) : ORIGIN = 0x00000000, LENGTH = 64K
    DTCM       (rwx) : ORIGIN = 0x20000000, LENGTH = 64K
    SRAM       (rwx) : ORIGIN = 0x22000000, LENGTH = 2048K
    SDRAM      (rwx) : ORIGIN = 0x68000000, LENGTH = 64M
}

/*
 * Linker symbols. Named with the g_ra_ls_ prefix so they are NOT in
 * the ISO C reserved leading-underscore namespace. The boot code in
 * src/boot/vector_table.c declares them with exactly these names.
 */
g_ra_ls_stack_size = 8K;
g_ra_ls_heap_size  = 4K;
g_ra_ls_stack_top  = ORIGIN(SRAM) + LENGTH(SRAM);

/*
 * Option setting region fixed addresses (HUM section 4).
 *
 * These sit in MRAM at well-known offsets. The boot ROM reads them
 * directly, without going through the Cortex-M85 reset vector. Each
 * section is individually KEEP()-ed so the linker never strips them,
 * even if no .c file references them.
 */
OFS0_ADDR     = 0x0300A100;
OFS1_ADDR     = 0x0300A104;
OFS2_ADDR     = 0x0300A108;
OFS3_ADDR     = 0x0300A10C;
SAS_ADDR      = 0x0300A110;
OFS1_SEC_ADDR = 0x0300A200;
OFS1_SEL_ADDR = 0x0300A204;

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
        g_ra_ls_exidx_start = .;
        *(.ARM.exidx* .gnu.linkonce.armexidx.*)
        g_ra_ls_exidx_end = .;
    } > MRAM

    /* ---- C++ constructors / destructors (for __libc_init_array) ---- */
    .preinit_array : ALIGN(4)
    {
        g_ra_ls_preinit_array_start = .;
        KEEP(*(.preinit_array*))
        g_ra_ls_preinit_array_end = .;
    } > MRAM

    .init_array : ALIGN(4)
    {
        g_ra_ls_init_array_start = .;
        KEEP(*(SORT(.init_array.*)))
        KEEP(*(.init_array*))
        g_ra_ls_init_array_end = .;
    } > MRAM

    .fini_array : ALIGN(4)
    {
        g_ra_ls_fini_array_start = .;
        KEEP(*(SORT(.fini_array.*)))
        KEEP(*(.fini_array*))
        g_ra_ls_fini_array_end = .;
    } > MRAM

    g_ra_ls_etext = .;

    /* ---- Option setting regions (fixed flash addresses) ---- */
    .option_setting_ofs0 OFS0_ADDR : { KEEP(*(.option_setting_ofs0)) } > MRAM
    .option_setting_ofs1 OFS1_ADDR : { KEEP(*(.option_setting_ofs1)) } > MRAM
    .option_setting_ofs2 OFS2_ADDR : { KEEP(*(.option_setting_ofs2)) } > MRAM
    .option_setting_ofs3 OFS3_ADDR : { KEEP(*(.option_setting_ofs3)) } > MRAM
    .option_setting_sas  SAS_ADDR  : { KEEP(*(.option_setting_sas))  } > MRAM
    .option_setting_ofs1_sec OFS1_SEC_ADDR : { KEEP(*(.option_setting_ofs1_sec)) } > MRAM
    .option_setting_ofs1_sel OFS1_SEL_ADDR : { KEEP(*(.option_setting_ofs1_sel)) } > MRAM

    /* ---- Initialised data: loaded from MRAM, lives in SRAM ---- */
    .data : ALIGN(4)
    {
        g_ra_ls_sdata = .;
        *(.data)
        *(.data.*)
        g_ra_ls_edata = .;
    } > SRAM AT > MRAM

    g_ra_ls_sidata = LOADADDR(.data);

    /* ---- Zero-init data ---- */
    .bss (NOLOAD) : ALIGN(4)
    {
        g_ra_ls_sbss = .;
        *(.bss)
        *(.bss.*)
        *(COMMON)
        g_ra_ls_ebss = .;
    } > SRAM

    /* ---- Heap (newlib requires it) ---- */
    .heap (NOLOAD) : ALIGN(8)
    {
        g_ra_ls_sheap = .;
        . = . + g_ra_ls_heap_size;
        g_ra_ls_eheap = .;
    } > SRAM

    /* ---- Stack canary (fill 32 bytes just below the stack top
     *      with a known pattern so stack-overflow checks have a
     *      sentinel to compare against) ---- */
    .stack_canary (NOLOAD) : ALIGN(8)
    {
        g_ra_ls_stack_canary = .;
        . = . + 32;
    } > SRAM

    /* ---- ITCM / DTCM sections (used when a driver opts in by
     *      placing hot code / state via
     *      __attribute__((section(".itcm_text"))) etc.) ---- */
    .itcm_text : ALIGN(4)
    {
        g_ra_ls_sitcm = .;
        *(.itcm_text)
        *(.itcm_text.*)
        g_ra_ls_eitcm = .;
    } > ITCM AT > MRAM

    .dtcm_data : ALIGN(4)
    {
        g_ra_ls_sdtcm = .;
        *(.dtcm_data)
        *(.dtcm_data.*)
        g_ra_ls_edtcm = .;
    } > DTCM AT > MRAM

    .dtcm_bss (NOLOAD) : ALIGN(4)
    {
        g_ra_ls_sdtcm_bss = .;
        *(.dtcm_bss)
        *(.dtcm_bss.*)
        g_ra_ls_edtcm_bss = .;
    } > DTCM

    /* ---- SDRAM sections for framebuffers, image buffers, etc. ---- */
    .sdram_data (NOLOAD) : ALIGN(8)
    {
        g_ra_ls_ssdram = .;
        *(.sdram_data)
        *(.sdram_data.*)
        g_ra_ls_esdram = .;
    } > SDRAM

    /* ---- Discarded ---- */
    /DISCARD/ :
    {
        *(.note.GNU-stack)
        *(.gnu*)
    }
}

/*
 * Assert that we did not overrun MRAM or SRAM at link time. If these
 * fire, resize the sections above or move the offending module to
 * SDRAM / ITCM / DTCM.
 */
ASSERT(g_ra_ls_etext <= ORIGIN(MRAM) + LENGTH(MRAM),
       "FATAL: MRAM overflow -- .text + .rodata too large")
ASSERT(g_ra_ls_ebss + g_ra_ls_heap_size + g_ra_ls_stack_size <= g_ra_ls_stack_top,
       "FATAL: SRAM overflow -- .bss + heap + stack won't fit")
