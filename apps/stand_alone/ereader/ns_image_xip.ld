/*
 * GNU ld linker script for the ra8d2-ereader NON-SECURE image -- OSPI XIP variant.
 *
 * Selected by the RA8_EREADER_NS_XIP build option (default OFF, which uses the
 * SRAM-run ns_image.ld). Unlike the SRAM-run layout -- where the Secure boot
 * copies the whole NS image MRAM->SRAM before BLXNS -- this variant runs the NS
 * code EXECUTE-IN-PLACE from Octo-SPI flash:
 *
 *   - .ns_vectors / .text / .rodata / .ARM.exidx live in OSPI at VMA == LMA
 *     (0x9000_0000, the TrustZone Non-secure alias of the 0x8000_0000 physical
 *     XIP window, IDAU bit[28] = 1). The M85 fetches NS instructions straight
 *     from flash after the Secure boot arms XIP via ra8_xspi_xip_enter -- there
 *     is no copy of .text to RAM.
 *   - .data has its VMA in SRAM2 (writable) but its LMA in OSPI; the NS reset
 *     handler copies it OSPI->SRAM at startup (the Secure boot no longer copies
 *     the image, so .data initialisation is the NS image's own job here).
 *   - .bss + the ThreadX heap + the NS main stack live in SRAM2.
 *
 * The flasher writes this image's bytes to OSPI physical 0x8000_0000; the NS
 * world addresses the same array through the 0x9000_0000 Non-secure alias.
 *
 * Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

ENTRY(ns_reset_handler)

MEMORY
{
    /* Execute-in-place home: OSPI flash Non-secure alias (bit[28] = 1). */
    NS_XIP (rx)       : ORIGIN = 0x90000000, LENGTH = 256K
    /* Writable run-time home: SRAM2 via the bit[28] Non-secure alias. */
    NS_SRAM_RUN (rwx) : ORIGIN = 0x32100000, LENGTH = 512K
}

/* NS main stack tops out at the top of NS_SRAM_RUN (0x3218_0000). Slot 0 of the
 * NS vector table (in OSPI) holds this; the Secure boot loads it into MSP_NS. */
g_ra8_ls_ns_stack_top = ORIGIN(NS_SRAM_RUN) + LENGTH(NS_SRAM_RUN);

/* The NS vector table base -- in OSPI for XIP. ns_reset_handler points VTOR
 * here so exceptions vector to the NS handlers fetched execute-in-place. */
g_ra8_vector_table_start = ORIGIN(NS_XIP);

SECTIONS
{
    /* --- Execute-in-place: vectors + code + read-only data live in OSPI. --- */
    .ns_vectors ORIGIN(NS_XIP) : ALIGN(8)
    {
        g_ra8_ls_ns_run_start = .;
        KEEP(*(.ns_vectors))
        KEEP(*(.ns_vectors.*))
    } > NS_XIP

    /* Root-of-trust header at ns_base + 0x40 (see the SRAM-run ns_image.ld for
     * the full rationale). For XIP the whole signed body lives in OSPI (VMA ==
     * LMA), so body_len spans vectors..data-init: LMA end - OSPI origin. */
    .ns_rot_header : ALIGN(4)
    {
        g_ra8_ls_ns_rot_header = .;
        LONG(0x3152534E);
        LONG(LOADADDR(.data) + SIZEOF(.data) - ORIGIN(NS_XIP));
    } > NS_XIP

    .text : ALIGN(8)
    {
        *(.ns_text)
        *(.ns_text.*)
        *(.text .text.* .glue_7 .glue_7t)
        *(.gnu.linkonce.t.*)
    } > NS_XIP

    .rodata : ALIGN(8)
    {
        *(.ns_rodata)
        *(.ns_rodata.*)
        *(.rodata .rodata.*)
        *(.gnu.linkonce.r.*)
    } > NS_XIP

    .ARM.exidx : ALIGN(4)
    {
        *(.ARM.exidx* .gnu.linkonce.armexidx.*)
    } > NS_XIP

    /* --- Writable initialised data: VMA in SRAM, LMA (init image) in OSPI. --- */
    .data : ALIGN(8)
    {
        g_ra8_ls_ns_data_start = .;
        *(.ns_data)
        *(.ns_data.*)
        *(.data .data.*)
        *(.gnu.linkonce.d.*)
        . = ALIGN(8);
        g_ra8_ls_ns_data_end = .;
    } > NS_SRAM_RUN AT > NS_XIP

    /* LMA of .data in OSPI -- ns_reset_handler copies [load .. load+size) to
     * [g_ra8_ls_ns_data_start .. g_ra8_ls_ns_data_end). */
    g_ra8_ls_ns_data_load_start = LOADADDR(.data);

    /* End of the RoT-signed body in OSPI (== data LMA end); rot_sign.py appends
     * the trailer here, at ns_base + body_len. Mirrors .ns_rot_header body_len. */
    g_ra8_ls_ns_signed_body_end = LOADADDR(.data) + SIZEOF(.data);

    .bss (NOLOAD) : ALIGN(8)
    {
        g_ra8_ls_ns_bss_start = .;
        *(.ns_bss)
        *(.ns_bss.*)
        *(.bss .bss.*)
        *(COMMON)
        g_ra8_ls_ns_bss_end = .;
        . = ALIGN(8);
        g_ra8_threadx_unused_memory_start = .;
    } > NS_SRAM_RUN

    /DISCARD/ :
    {
        *(.note.GNU-stack)
        *(.ARM.attributes)
        *(.comment)
    }
}

ASSERT((g_ra8_ls_ns_data_load_start + (g_ra8_ls_ns_data_end - g_ra8_ls_ns_data_start))
       <= ORIGIN(NS_XIP) + LENGTH(NS_XIP),
       "FATAL: NS XIP image (text+rodata+data-init) overflows NS_XIP (OSPI)")
ASSERT(g_ra8_threadx_unused_memory_start <= ORIGIN(NS_SRAM_RUN) + LENGTH(NS_SRAM_RUN),
       "FATAL: NS data+bss overflows NS_SRAM_RUN (SRAM2)")
/* The RoT header MUST land at ns_base + 0x40 (== k_ra8_tz_ns_rot_header_offset);
 * holds only while the NS vector table is exactly 16 entries. */
ASSERT(g_ra8_ls_ns_rot_header - g_ra8_ls_ns_run_start == 0x40,
       "FATAL: .ns_rot_header not at ns_base + 0x40 (NS vector table must be 16 entries)")
