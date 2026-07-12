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
 * DATA_FLASH 0x27000000 16 KiB Data flash (EEPROM emulation)
 * ITCM 0x00000000 64 KiB Instruction tightly-coupled mem
 * DTCM 0x20000000 64 KiB Data tightly-coupled mem
 * SRAM (ECC) 0x22000000 2 MiB .data, .bss, main stack (no heap)
 * SDRAM 0x68000000 64 MiB External SDRAM (EK board)
 *
 * =====================================================================
 * WARNING -- OFS addresses below are a best-effort derivation of the
 * RA8 MRAM option-setting layout. Before flashing to a real board,
 * confirm against HUM R01UH1065EJ section 4 ("Option Function Select
 * Registers") that 0x0300_A100 is the correct OFS0 base. If it is not,
 * override via the build with:
 *
 * -Wl,--defsym=OFS0_ADDR=0xNNNNNNNN
 *
 * Getting this wrong cannot brick the chip if the data is all-ones
 * (the boot ROM interprets 0xFFFFFFFF as "default / permissive"), but
 * any non-erased value at the wrong address will lead to undefined
 * behaviour on first boot.
 * =====================================================================
 *
 * Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

ENTRY(Reset_Handler)

MEMORY
{
    MRAM (rx) : ORIGIN = 0x02000000, LENGTH = 768K
    MRAM_CPU1 (rx) : ORIGIN = 0x020C0000, LENGTH = 256K
    /* Option Setting / OTP block lives in a dedicated config flash
     * area outside main MRAM. Sized to cover 0x0300A000..0x0300B000
     * so every OFS*, SAS, BPS, OTP_* section has a home. */
    OFS (r) : ORIGIN = 0x0300A000, LENGTH = 4K
    DATA_FLASH (rw) : ORIGIN = 0x27000000, LENGTH = 16K
    ITCM (rwx) : ORIGIN = 0x00000000, LENGTH = 64K
    DTCM (rwx) : ORIGIN = 0x20000000, LENGTH = 64K
    SRAM (rwx) : ORIGIN = 0x22000000, LENGTH = 1024K
    SDRAM (rwx) : ORIGIN = 0x68000000, LENGTH = 64M

    /* Non-Secure partition placeholders. Lower halves of
     * MRAM and SRAM stay Secure (they hold the HAL + secure data);
     * the upper halves are exposed to the NS application
     * (lwIP / CherryUSB / src/main.c after the relocation lands).
     *
     * These regions overlap with MRAM / SRAM in the single-image
     * build because the linker treats them as views of the same
     * physical bytes; the SAU programmes the bit-28 split at boot
     * (see blink/trustzone_init.c) so the CPU sees them as
     * Non-Secure aliases. With RA8_TRUSTZONE_ENABLE off the linker
     * is allowed to place sections in either region freely. */
    NS_MRAM (rx) : ORIGIN = 0x02080000, LENGTH = 512K
    NS_SRAM (rwx) : ORIGIN = 0x22100000, LENGTH = 1024K
}

/*
 * Linker symbols. Named with the g_ra8_ls_ prefix so they are NOT in
 * the ISO C reserved leading-underscore namespace. The boot code in
 * blink/vector_table.c declares them with exactly these names.
 */
g_ra8_ls_stack_size = DEFINED(g_ra8_ls_stack_size) ? g_ra8_ls_stack_size : 8K;
g_ra8_ls_stack_top = ORIGIN(SRAM) + LENGTH(SRAM);
/* No heap: NASA Power of 10 Rule 3 -- zero dynamic allocation after
 * init. _sbrk() in libs/ra8_core/src/ra8_sbrk_trap.c traps any
 * accidental malloc() call from newlib. */

/*
 * Option-setting region fixed addresses (HUM section 4).
 *
 * Every entry is PROVIDE()-d so it can be overridden with
 * `--defsym=OFS0_ADDR=0x...` from the cmake toolchain file or the
 * top-level build. If you need to change the MRAM base, override all
 * of these in lock-step.
 */
PROVIDE(OFS0_ADDR = 0x0300A100);
PROVIDE(OFS1_ADDR = 0x0300A104);
PROVIDE(OFS2_ADDR = 0x0300A108);
PROVIDE(OFS3_ADDR = 0x0300A10C);
PROVIDE(SAS_ADDR = 0x0300A110);
PROVIDE(OFS1_SEC_ADDR = 0x0300A200);
PROVIDE(OFS1_SEL_ADDR = 0x0300A204);
PROVIDE(OFS3_SEC_ADDR = 0x0300A20C);
PROVIDE(OFS3_SEL_ADDR = 0x0300A210);
PROVIDE(BPS_ADDR = 0x0300A300);
PROVIDE(BPS_SEC_ADDR = 0x0300A400);
PROVIDE(OTP_FSBLCTRL0_ADDR = 0x0300A500);
PROVIDE(OTP_FSBLCTRL1_ADDR = 0x0300A504);
PROVIDE(OTP_FSBLCTRL2_ADDR = 0x0300A508);
PROVIDE(OTP_SAMR_ADDR = 0x0300A600);
PROVIDE(OTP_SACC00_ADDR = 0x0300A700);
PROVIDE(OTP_SACC10_ADDR = 0x0300A704);
PROVIDE(OTP_SACC01_ADDR = 0x0300A708);
PROVIDE(OTP_SACC11_ADDR = 0x0300A70C);
PROVIDE(OTP_SACC02_ADDR = 0x0300A710);
PROVIDE(OTP_SACC12_ADDR = 0x0300A714);
PROVIDE(OTP_SACC03_ADDR = 0x0300A718);
PROVIDE(OTP_SACC13_ADDR = 0x0300A71C);
PROVIDE(OTP_PBPS_ADDR = 0x0300A800);
PROVIDE(OTP_PBPS_SEC_ADDR = 0x0300A804);
PROVIDE(OTP_ZHUK_ADDR = 0x0300A900);

SECTIONS
{
    /* ---- Vector table ---- */
    .vectors : ALIGN(4)
    {
        KEEP(*(.vectors))
        KEEP(*(.vectors.*))
    } > MRAM

    /* ---- CPU1 (Cortex-M33) firmware image ----
     *
     * The dualcore_mailbox app builds two separate ELFs: this one for the
     * Cortex-M85 primary core, and ``dualcore_mailbox_cpu1.elf`` for the
     * Cortex-M33 secondary core. Per HUM Ch 2.6.1.1 "Boot CPU Selection"
     * p 117 the secondary CPU sits in power-gating at reset and is
     * released only when CPU0 writes CPU1ACTCSR; it then fetches its
     * reset vector from CPU1INITVTOR (HUM Ch 2.9.1.7 p 129).
     *
     * The CPU1 binary is converted to a relocatable ``.o`` by the
     * CMakeLists.txt post-build step, which renames its single ``.data``
     * section to ``.cpu1_image``. We pin that section here at the
     * MRAM_CPU1 origin (0x020C0000) so a single SWD ``loadfile`` of the
     * M85 .hex deposits both cores' code into MRAM. Without this
     * placement the cpu1.hex was built but never flashed, CPU1 fetched
     * its reset vector from blank MRAM (0xFFFFFFFF), and the ping-pong
     * counter stayed at 0 across the entire HIL probe window.
     */
    .cpu1_image ORIGIN(MRAM_CPU1) : ALIGN(4)
    {
        KEEP(*(.cpu1_image))
        KEEP(*(.cpu1_image.*))
    } > MRAM_CPU1

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
    .option_setting_ofs0 OFS0_ADDR : { KEEP(*(.option_setting_ofs0)) } > OFS
    .option_setting_ofs1 OFS1_ADDR : { KEEP(*(.option_setting_ofs1)) } > OFS
    .option_setting_ofs2 OFS2_ADDR : { KEEP(*(.option_setting_ofs2)) } > OFS
    .option_setting_ofs3 OFS3_ADDR : { KEEP(*(.option_setting_ofs3)) } > OFS
    .option_setting_sas SAS_ADDR : { KEEP(*(.option_setting_sas)) } > OFS
    .option_setting_ofs1_sec OFS1_SEC_ADDR : { KEEP(*(.option_setting_ofs1_sec)) } > OFS
    .option_setting_ofs1_sel OFS1_SEL_ADDR : { KEEP(*(.option_setting_ofs1_sel)) } > OFS
    .option_setting_ofs3_sec OFS3_SEC_ADDR : { KEEP(*(.option_setting_ofs3_sec)) } > OFS
    .option_setting_ofs3_sel OFS3_SEL_ADDR : { KEEP(*(.option_setting_ofs3_sel)) } > OFS
    .option_setting_bps BPS_ADDR : { KEEP(*(.option_setting_bps)) } > OFS
    .option_setting_bps_sec BPS_SEC_ADDR : { KEEP(*(.option_setting_bps_sec)) } > OFS
    .option_setting_otp_fsblctrl0 OTP_FSBLCTRL0_ADDR : { KEEP(*(.option_setting_otp_fsblctrl0)) } > OFS
    .option_setting_otp_fsblctrl1 OTP_FSBLCTRL1_ADDR : { KEEP(*(.option_setting_otp_fsblctrl1)) } > OFS
    .option_setting_otp_fsblctrl2 OTP_FSBLCTRL2_ADDR : { KEEP(*(.option_setting_otp_fsblctrl2)) } > OFS
    .option_setting_otp_samr OTP_SAMR_ADDR : { KEEP(*(.option_setting_otp_samr)) } > OFS
    .option_setting_otp_sacc00 OTP_SACC00_ADDR : { KEEP(*(.option_setting_otp_sacc00)) } > OFS
    .option_setting_otp_sacc10 OTP_SACC10_ADDR : { KEEP(*(.option_setting_otp_sacc10)) } > OFS
    .option_setting_otp_sacc01 OTP_SACC01_ADDR : { KEEP(*(.option_setting_otp_sacc01)) } > OFS
    .option_setting_otp_sacc11 OTP_SACC11_ADDR : { KEEP(*(.option_setting_otp_sacc11)) } > OFS
    .option_setting_otp_sacc02 OTP_SACC02_ADDR : { KEEP(*(.option_setting_otp_sacc02)) } > OFS
    .option_setting_otp_sacc12 OTP_SACC12_ADDR : { KEEP(*(.option_setting_otp_sacc12)) } > OFS
    .option_setting_otp_sacc03 OTP_SACC03_ADDR : { KEEP(*(.option_setting_otp_sacc03)) } > OFS
    .option_setting_otp_sacc13 OTP_SACC13_ADDR : { KEEP(*(.option_setting_otp_sacc13)) } > OFS
    .option_setting_otp_pbps OTP_PBPS_ADDR : { KEEP(*(.option_setting_otp_pbps)) } > OFS
    .option_setting_otp_pbps_sec OTP_PBPS_SEC_ADDR : { KEEP(*(.option_setting_otp_pbps_sec)) } > OFS
    .option_setting_otp_zhuk OTP_ZHUK_ADDR : { KEEP(*(.option_setting_otp_zhuk)) } > OFS

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

g_ra8_ls_cpu1_mram_start = ORIGIN(MRAM_CPU1);
g_ra8_ls_cpu1_stack_top  = ORIGIN(SRAM) + LENGTH(SRAM) - 16K;
