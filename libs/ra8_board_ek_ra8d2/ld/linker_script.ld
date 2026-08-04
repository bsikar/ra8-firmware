/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/*
 * GNU ld linker script for ra8-firmware
 *
 * Target: Renesas R7KA8D2KF (RA8D2) on EK-RA8D2
 * - Arm Cortex-M85 primary core
 * - Arm Cortex-M33 secondary core (not used yet)
 *
 * Memory map (from RA8D2 Hardware User's Manual R01UH1065EJ):
 *
 * Region Start Size Usage
 * ------------ ---------- --------- ----------------------------
 * MRAM 0x02000000 1 MiB Code + rodata + vectors + OFS
 * ITCM 0x00000000 64 KiB Instruction tightly-coupled mem
 * DTCM 0x20000000 64 KiB Data tightly-coupled mem
 * SRAM (ECC) 0x22000000 2 MiB .data, .bss, main stack (no heap)
 * SDRAM 0x68000000 64 MiB External SDRAM (EK board)
 *
 * =====================================================================
 * Option-setting addresses below are the real RA8D2 layout from HUM
 * R01UH1065EJ Ch 7 Figure 7.1 p 279 (secure aliases). scripts/hil/run.sh
 * strips every .option_setting section before flashing as a deliberate
 * brick-safety policy -- program them only through the explicit path
 * documented there.
 * =====================================================================
 */

ENTRY(Reset_Handler)

MEMORY
{
    MRAM (rx) : ORIGIN = 0x02000000, LENGTH = 1024K
    /* Option-setting memory lives in the extra MRAM in two disjoint blocks
     * (HUM Ch 7 Figure 7.1 p 279): the configuration words at 0x02C9_F0xx and
     * the OTP / anti-rollback area at 0x02E0_7600.. . Placed at their real
     * secure-alias addresses so the flashing tool targets the right cells. */
    OFS_CFG (r) : ORIGIN = 0x02C9F000, LENGTH = 2K
    OFS_OTP (r) : ORIGIN = 0x02E07000, LENGTH = 68K
    ITCM (rwx) : ORIGIN = 0x00000000, LENGTH = 64K
    DTCM (rwx) : ORIGIN = 0x20000000, LENGTH = 64K
    /* Top 256 bytes of SRAM are carved off into NOINIT (below) for the
     * warm-reset-surviving crash-log record, so SRAM proper is 256 bytes
     * short of 1 MiB. Keeping it a distinct region (rather than pinning a
     * section at the top of a full-length SRAM) keeps the linker's SRAM
     * usage report honest instead of reading ~100% for a top-pinned span. */
    SRAM (rwx) : ORIGIN = 0x22000000, LENGTH = 1024K - 256
    NOINIT (rw) : ORIGIN = 0x220FFF00, LENGTH = 256   /* 0x22000000 + 1024K - 256; == k_ra8_crashlog_reserve_bytes */
    SDRAM (rwx) : ORIGIN = 0x68000000, LENGTH = 64M

    /* Non-Secure partition placeholders. Lower halves of
     * MRAM and SRAM stay Secure (they hold the HAL + secure data);
     * the upper halves are exposed to the NS application
     * (lwIP / CherryUSB / src/main.c after the relocation lands).
     *
     * These regions overlap with MRAM / SRAM in the single-image
     * build because the linker treats them as views of the same
     * physical bytes; the SAU programmes the bit-28 split at boot
     * (see blink_hal/trustzone_init.c) so the CPU sees them as
     * Non-Secure aliases. With RA8_TRUSTZONE_ENABLE off the linker
     * is allowed to place sections in either region freely. */
    NS_MRAM (rx) : ORIGIN = 0x02080000, LENGTH = 512K
    NS_SRAM (rwx) : ORIGIN = 0x22100000, LENGTH = 640K   /* SRAM1 upper half; unused placeholder w/ TrustZone off */
}

/*
 * Linker symbols. Named with the g_ra8_ls_ prefix so they are NOT in
 * the ISO C reserved leading-underscore namespace. The boot code in
 * blink_hal/vector_table.c declares them with exactly these names.
 */
g_ra8_ls_stack_size = DEFINED(g_ra8_ls_stack_size) ? g_ra8_ls_stack_size : 8K;
/* SRAM already excludes the top-of-SRAM NOINIT region (see MEMORY above),
 * so the stack top is simply the end of SRAM proper -- it sits directly
 * below the crash-log record and the two can never overlap. */
g_ra8_ls_stack_top = ORIGIN(SRAM) + LENGTH(SRAM);
/* No heap: NASA Power of 10 Rule 3 -- zero dynamic allocation after
 * init. _sbrk() in libs/ra8_core/src/ra8_sbrk_trap.c traps any
 * accidental malloc() call from newlib. */

/*
 * Option-setting region fixed addresses (HUM Ch 7 "Option-Setting Memory" Figure 7.1 p 279).
 *
 * Every entry is PROVIDE()-d so it can be overridden with
 * `--defsym=OFS0_ADDR=0x...` from the cmake toolchain file or the
 * top-level build. If you need to change the MRAM base, override all
 * of these in lock-step.
 */
PROVIDE(OFS0_ADDR = 0x02C9F040);
PROVIDE(OFS1_ADDR = 0x02C9F4C0);
PROVIDE(OFS2_ADDR = 0x02C9F044);
PROVIDE(OFS3_ADDR = 0x02C9F4C4);
PROVIDE(SAS_ADDR = 0x02C9F074);
PROVIDE(OFS1_SEC_ADDR = 0x02C9F0C0);
PROVIDE(OFS1_SEL_ADDR = 0x02C9F120);
PROVIDE(OFS3_SEC_ADDR = 0x02C9F0C4);
PROVIDE(OFS3_SEL_ADDR = 0x02C9F124);
PROVIDE(BPS_ADDR = 0x02C9F600);
PROVIDE(BPS_SEC_ADDR = 0x02C9F200);
PROVIDE(OTP_FSBLCTRL0_ADDR = 0x02E07600);
PROVIDE(OTP_FSBLCTRL1_ADDR = 0x02E07604);
PROVIDE(OTP_FSBLCTRL2_ADDR = 0x02E07608);
PROVIDE(OTP_SAMR_ADDR = 0x02E07614);
PROVIDE(OTP_SACC00_ADDR = 0x02E07620);
PROVIDE(OTP_SACC10_ADDR = 0x02E07630);
PROVIDE(OTP_SACC01_ADDR = 0x02E07640);
PROVIDE(OTP_SACC11_ADDR = 0x02E07650);
PROVIDE(OTP_SACC02_ADDR = 0x02E07660);
PROVIDE(OTP_SACC12_ADDR = 0x02E07670);
PROVIDE(OTP_SACC03_ADDR = 0x02E07680);
PROVIDE(OTP_SACC13_ADDR = 0x02E07690);
PROVIDE(OTP_PBPS_ADDR = 0x02E17780);
PROVIDE(OTP_PBPS_SEC_ADDR = 0x02E17700);
PROVIDE(OTP_ZHUK_ADDR = 0x02E17920);

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
        g_ra8_ls_exidx_start = .;
        *(.ARM.exidx* .gnu.linkonce.armexidx.*)
        g_ra8_ls_exidx_end = .;
    } > MRAM

    /* ---- C++ constructors / destructors (for __libc_init_array) ---- */
    .preinit_array : ALIGN(4)
    {
        g_ra8_ls_preinit_array_start = .;
        KEEP(*(.preinit_array*))
        g_ra8_ls_preinit_array_end = .;
    } > MRAM

    .init_array : ALIGN(4)
    {
        g_ra8_ls_init_array_start = .;
        KEEP(*(SORT(.init_array.*)))
        KEEP(*(.init_array*))
        g_ra8_ls_init_array_end = .;
    } > MRAM

    .fini_array : ALIGN(4)
    {
        g_ra8_ls_fini_array_start = .;
        KEEP(*(SORT(.fini_array.*)))
        KEEP(*(.fini_array*))
        g_ra8_ls_fini_array_end = .;
    } > MRAM

    g_ra8_ls_etext = .;

    /* ---- TrustZone Non-Secure Callable veneer stubs ----
     *
     * `-mcmse` makes gcc emit a `.gnu.sgstubs` output section
     * containing the secure-gateway veneers. The linker needs an
     * explicit placement; without one the link aborts even on
     * builds that have no veneers because gcc cannot prove the
     * section is empty without resolving every NSC call site.
     *
     * We park the section at the end of .text in MRAM with 32-byte
     * alignment (ARMv8-M veneer alignment requirement). When the
     * SAU programming uses NSC region 3 at 0x10000000,
     * the linker MEMORY map will need a matching alias region;
     * for the scaffold we keep it inside MRAM so the
     * single-image build still loads cleanly. */
    .gnu.sgstubs : ALIGN(32)
    {
        g_ra8_ls_sgstubs_start = .;
        *(.gnu.sgstubs*)
        g_ra8_ls_sgstubs_end = .;
    } > MRAM

    /* ---- Option setting regions (fixed flash addresses) ----
     *
     * Every one of these lands at a fixed MRAM address. Values are
     * provided by libs/ra8_hal/src/ra8_ofs.c via BSP_PLACE_IN_SECTION
     * (which expands to __attribute__((section(".option_setting_*")))).
     * A project that wants to change any of them just overrides the
     * matching `BSP_CFG_OPTION_SETTING_*` macro and rebuilds.
     */
    .option_setting_ofs0 OFS0_ADDR : { KEEP(*(.option_setting_ofs0)) } > OFS_CFG
    .option_setting_ofs1 OFS1_ADDR : { KEEP(*(.option_setting_ofs1)) } > OFS_CFG
    .option_setting_ofs2 OFS2_ADDR : { KEEP(*(.option_setting_ofs2)) } > OFS_CFG
    .option_setting_ofs3 OFS3_ADDR : { KEEP(*(.option_setting_ofs3)) } > OFS_CFG
    .option_setting_sas SAS_ADDR : { KEEP(*(.option_setting_sas)) } > OFS_CFG
    .option_setting_ofs1_sec OFS1_SEC_ADDR : { KEEP(*(.option_setting_ofs1_sec)) } > OFS_CFG
    .option_setting_ofs1_sel OFS1_SEL_ADDR : { KEEP(*(.option_setting_ofs1_sel)) } > OFS_CFG
    .option_setting_ofs3_sec OFS3_SEC_ADDR : { KEEP(*(.option_setting_ofs3_sec)) } > OFS_CFG
    .option_setting_ofs3_sel OFS3_SEL_ADDR : { KEEP(*(.option_setting_ofs3_sel)) } > OFS_CFG
    .option_setting_bps BPS_ADDR : { KEEP(*(.option_setting_bps)) } > OFS_CFG
    .option_setting_bps_sec BPS_SEC_ADDR : { KEEP(*(.option_setting_bps_sec)) } > OFS_CFG
    .option_setting_otp_fsblctrl0 OTP_FSBLCTRL0_ADDR : { KEEP(*(.option_setting_otp_fsblctrl0)) } > OFS_OTP
    .option_setting_otp_fsblctrl1 OTP_FSBLCTRL1_ADDR : { KEEP(*(.option_setting_otp_fsblctrl1)) } > OFS_OTP
    .option_setting_otp_fsblctrl2 OTP_FSBLCTRL2_ADDR : { KEEP(*(.option_setting_otp_fsblctrl2)) } > OFS_OTP
    .option_setting_otp_samr OTP_SAMR_ADDR : { KEEP(*(.option_setting_otp_samr)) } > OFS_OTP
    .option_setting_otp_sacc00 OTP_SACC00_ADDR : { KEEP(*(.option_setting_otp_sacc00)) } > OFS_OTP
    .option_setting_otp_sacc10 OTP_SACC10_ADDR : { KEEP(*(.option_setting_otp_sacc10)) } > OFS_OTP
    .option_setting_otp_sacc01 OTP_SACC01_ADDR : { KEEP(*(.option_setting_otp_sacc01)) } > OFS_OTP
    .option_setting_otp_sacc11 OTP_SACC11_ADDR : { KEEP(*(.option_setting_otp_sacc11)) } > OFS_OTP
    .option_setting_otp_sacc02 OTP_SACC02_ADDR : { KEEP(*(.option_setting_otp_sacc02)) } > OFS_OTP
    .option_setting_otp_sacc12 OTP_SACC12_ADDR : { KEEP(*(.option_setting_otp_sacc12)) } > OFS_OTP
    .option_setting_otp_sacc03 OTP_SACC03_ADDR : { KEEP(*(.option_setting_otp_sacc03)) } > OFS_OTP
    .option_setting_otp_sacc13 OTP_SACC13_ADDR : { KEEP(*(.option_setting_otp_sacc13)) } > OFS_OTP
    .option_setting_otp_pbps OTP_PBPS_ADDR : { KEEP(*(.option_setting_otp_pbps)) } > OFS_OTP
    .option_setting_otp_pbps_sec OTP_PBPS_SEC_ADDR : { KEEP(*(.option_setting_otp_pbps_sec)) } > OFS_OTP
    .option_setting_otp_zhuk OTP_ZHUK_ADDR : { KEEP(*(.option_setting_otp_zhuk)) } > OFS_OTP

    /* ---- Initialised data: loaded from MRAM, lives in SRAM ---- */
    .data : ALIGN(4)
    {
        g_ra8_ls_sdata = .;
        *(.data)
        *(.data.*)
        g_ra8_ls_edata = .;
    } > SRAM AT > MRAM

    g_ra8_ls_sidata = LOADADDR(.data);

    /* ---- Zero-init data ---- */
    .bss (NOLOAD) : ALIGN(4)
    {
        g_ra8_ls_sbss = .;
        *(.bss)
        *(.bss.*)
        *(COMMON)
        g_ra8_ls_ebss = .;
    } > SRAM

    /* No .heap region -- see ra8_sbrk_trap.c. */

    /* ---- Stack canary (32 bytes just below the stack top -- filled
     * with a known pattern by ra8_stack_canary_init() at boot so
     * ra8_stack_canary_check() has a sentinel to compare against). */
    .stack_canary (NOLOAD) : ALIGN(8)
    {
        g_ra8_ls_stack_canary_start = .;
        . = . + 32;
        g_ra8_ls_stack_canary_end = .;
    } > SRAM

    /* ---- Field crash-log record (survives warm / watchdog reset) ----
     *
     * NOLOAD so Reset_Handler's .bss zero-fill never touches it, and placed
     * in the dedicated NOINIT region at the very top of SRAM -- ABOVE the
     * main stack -- so it (a) survives any reset that keeps SRAM powered
     * (warm / watchdog / pin) and (b) never shifts .data / .bss and
     * therefore never moves the TrustZone fixed windows. A cold power-on
     * randomises SRAM; the record's magic + CRC then fail validation and the
     * log reads empty (the intended fail-safe). See
     * libs/ra8_core/src/ra8_crashlog.c (ra8_crashlog_record_t). Unreferenced in
     * apps that never call the crash-log API -- --gc-sections drops the
     * record and this section stays empty. */
    .noinit (NOLOAD) : ALIGN(8)
    {
        g_ra8_ls_noinit_start = .;
        *(.noinit)
        *(.noinit.*)
        g_ra8_ls_noinit_end = .;
    } > NOINIT

    /* ---- ITCM / DTCM sections (drivers opt in via
     * __attribute__((section(".itcm_text")))). ---- */
    .itcm_text : ALIGN(4)
    {
        g_ra8_ls_sitcm = .;
        *(.itcm_text)
        *(.itcm_text.*)
        g_ra8_ls_eitcm = .;
    } > ITCM AT > MRAM

    .dtcm_data : ALIGN(4)
    {
        g_ra8_ls_sdtcm = .;
        *(.dtcm_data)
        *(.dtcm_data.*)
        g_ra8_ls_edtcm = .;
    } > DTCM AT > MRAM

    .dtcm_bss (NOLOAD) : ALIGN(4)
    {
        g_ra8_ls_sdtcm_bss = .;
        *(.dtcm_bss)
        *(.dtcm_bss.*)
        g_ra8_ls_edtcm_bss = .;
    } > DTCM

    /* ---- SDRAM sections for framebuffers, image buffers, etc. ---- */
    .sdram_data (NOLOAD) : ALIGN(8)
    {
        g_ra8_ls_ssdram = .;
        *(.sdram_data)
        *(.sdram_data.*)
        g_ra8_ls_esdram = .;
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
ASSERT(g_ra8_ls_etext <= ORIGIN(MRAM) + LENGTH(MRAM),
       "FATAL: MRAM overflow -- .text + .rodata too large")
ASSERT(g_ra8_ls_ebss + g_ra8_ls_stack_size <= g_ra8_ls_stack_top,
       "FATAL: SRAM overflow -- .bss + stack won't fit")
ASSERT(g_ra8_ls_noinit_end <= ORIGIN(NOINIT) + LENGTH(NOINIT),
       "FATAL: .noinit crash-log record overran its 256-byte NOINIT region")
