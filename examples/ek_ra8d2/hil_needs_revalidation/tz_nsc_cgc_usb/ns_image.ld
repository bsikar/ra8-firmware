/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/*
 * GNU ld linker script for the tz_nsc_cgc_usb NON-SECURE image (#96).
 *
 * This is the Non-Secure half of a two-project TrustZone build: it is linked
 * SEPARATELY from the Secure ELF so it gets its own private copies of libc,
 * libgcc, ThreadX, USBX, ra8_usb and the HAL modules ra8_usb pulls in
 * (ra8_log / ra8_mstp / ra8_time / ra8_usbhs) -- the Secure side uses those same
 * modules, and a single ELF can only place each function once (NS calling a
 * Secure-resident helper faults INVTRAN). NS->Secure calls go ONLY through the
 * CMSE import library (the Secure side's SG veneers, linked in via --in-implib).
 *
 * Run-time VMA: SRAM2 Non-secure alias 0x3210_0000. Load LMA: Secure MRAM
 * 0x0208_0000 (the Secure boot copies LMA->VMA after SRAMSABAR2 marks SRAM2
 * Non-secure, then BLXNS-es to the reset vector).
 */

ENTRY(ns_reset_handler)

MEMORY
{
    /* Load home in Secure MRAM (flasher writes physical MRAM here). */
    NS_LOAD (rx)     : ORIGIN = 0x02080000, LENGTH = 256K
    /* Run-time home: SRAM2 via the bit[28] Non-secure alias. */
    NS_SRAM_RUN (rwx): ORIGIN = 0x32100000, LENGTH = 512K
}

/* NS main stack tops out at the top of NS_SRAM_RUN (0x3218_0000). Slot 0 of
 * the NS vector table holds this; the Secure boot loads it into MSP_NS. */
g_ra8_ls_ns_stack_top = ORIGIN(NS_SRAM_RUN) + LENGTH(NS_SRAM_RUN);

/* The Secure boot copies a fixed window from NS_LOAD; the ThreadX port wants
 * the NS vector-table base + a free-RAM sentinel. */
g_ra8_vector_table_start = ORIGIN(NS_SRAM_RUN);

SECTIONS
{
    .ns_vectors ORIGIN(NS_SRAM_RUN) : ALIGN(8)
    {
        g_ra8_ls_ns_run_start = .;
        KEEP(*(.ns_vectors))
        KEEP(*(.ns_vectors.*))
    } > NS_SRAM_RUN AT > NS_LOAD

    .text : ALIGN(8)
    {
        *(.ns_text)
        *(.ns_text.*)
        *(.text .text.* .glue_7 .glue_7t)
        *(.gnu.linkonce.t.*)
    } > NS_SRAM_RUN AT > NS_LOAD

    .rodata : ALIGN(8)
    {
        *(.ns_rodata)
        *(.ns_rodata.*)
        *(.rodata .rodata.*)
        *(.gnu.linkonce.r.*)
    } > NS_SRAM_RUN AT > NS_LOAD

    .ARM.exidx : ALIGN(4)
    {
        *(.ARM.exidx* .gnu.linkonce.armexidx.*)
    } > NS_SRAM_RUN AT > NS_LOAD

    .data : ALIGN(8)
    {
        *(.ns_data)
        *(.ns_data.*)
        *(.data .data.*)
        *(.gnu.linkonce.d.*)
        g_ra8_ls_ns_run_end = .;
    } > NS_SRAM_RUN AT > NS_LOAD

    /* LMA bounds of the loaded NS image (in MRAM) for the Secure copy. */
    g_ra8_ls_ns_load_start = LOADADDR(.ns_vectors);
    g_ra8_ls_ns_load_end   = LOADADDR(.data) + SIZEOF(.data);

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

ASSERT(g_ra8_ls_ns_load_end <= ORIGIN(NS_LOAD) + LENGTH(NS_LOAD),
       "FATAL: NS image overflows NS_LOAD (MRAM)")
ASSERT(g_ra8_threadx_unused_memory_start <= ORIGIN(NS_SRAM_RUN) + LENGTH(NS_SRAM_RUN),
       "FATAL: NS image overflows NS_SRAM_RUN (SRAM2)")
