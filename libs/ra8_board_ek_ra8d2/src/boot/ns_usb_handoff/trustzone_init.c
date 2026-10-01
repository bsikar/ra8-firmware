/**
 * @file libs/ra8_board_ek_ra8d2/src/boot/ns_usb_handoff/trustzone_init.c
 * @brief Single-core TrustZone bring-up for a RAM-resident NS image
 *
 * @par Tag
 * [Ring 1 / Boot] {World: S}
 *
 * @details
 * The RA8 IDAU is FIXED by address bit[28] (HUM section 51.3.3.1, p3265):
 * bit[28]=0 is Secure/NSC and the SAU cannot downgrade it, so an "NS"
 * image at the 0x02.. / 0x22.. (bit[28]=0) aliases always executes
 * Secure. Real NS lives at the bit[28]=1 aliases (0x12.. code, 0x32..
 * SRAM, 0x5.. peripherals).
 *
 * Code MRAM's secure/NS split needs persistent (brick-risky) option
 * bytes, but SRAM's split is the RUNTIME ``SRAMSABARn`` register, so the
 * NS image is RAM-resident: flashed into Secure MRAM (the LMA) and copied
 * by this code into the SRAM Non-secure alias 0x3210_0000 (physical SRAM2)
 * after ``SRAMSABAR2`` marks SRAM2 Non-secure. No option bytes, no brick.
 *
 * Boot sequence (this file, all in Secure state):
 *   1. Open ``PRCR_S.PRC4`` and program ``SRAMSABAR0..3`` so physical SRAM
 *      [0x10_0000, 0x18_0000) (SRAM2) is Non-secure, lower SRAM stays
 *      Secure (the Secure stack lives there). Re-lock PRC4.
 *   2. Programme the SAU to the bit[28] model (HUM p3267): mark the
 *      IDAU-NS ranges 0x1000_0000-, 0x3000_0000-, 0x5000_0000- as NS, and
 *      one NSC region over the ``.gnu.sgstubs`` veneers in Secure MRAM.
 *      Enable SAU with ALLNS = 0 (default-deny).
 *   3. Copy the NS image from its MRAM LMA to the SRAM NS alias.
 *   4. BLXNS into the NS reset vector via ``ra8_tz_secure_boot_jump_ns``.
 *
 * This file does NOT use ``ra8_tz_secure_boot_sau_init`` -- that function's
 * region table is tuned for cpu1_pingpong_ipc (CPU1 is the NS core) and
 * is shared; the bit[28] model here is app-local so that validated app is
 * untouched. Only the generic ``ra8_tz_secure_boot_jump_ns`` primitive is
 * reused.
 *
 * On a host build (``RA8_OFF_TARGET``) this function is a no-op.
 *
 * @par TrustZone Safety:
 *  - **Validates:** SAU_TYPE.SREGION >= 4 before programming.
 *  - **Validates:** ``g_ra8_ns_vector_table`` non-NULL + word-aligned
 *    (checked by ``ra8_tz_secure_boot_jump_ns``).
 *  - **Trusts:** the boot ROM left the SAU disabled and the IDAU in its
 *    documented reset state (fixed bit[28] split).
 *  - **Denies:** treating the NS world as live on any
 *    ``ra8_tz_secure_boot_jump_ns`` return. On hardware a successful BLXNS
 *    leaves Secure thread mode and never returns; a returned denial verdict
 *    is latched in ::g_tz_jump_ns_err and boot falls back to the S-side
 *    ``main()``.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "trustzone_init.h"

#include <stdint.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_gpio_constants.h"
#include "ra8_pin_validator.h"
#include "ra8_port_constants.h"
#include "ra8_port_utils.h"
#include "ra8_register_protection.h"
#include "ra8_tz_partition.h"
#include "ra8_tz_psar.h"
#include "ra8_tz_secure_boot.h"

/* Bounds of the NSC veneer stubs (.gnu.sgstubs) in this (Secure) image. */
extern uint32_t g_ra8_ls_sgstubs_start; /**< Veneer-region start (NSC). */
extern uint32_t g_ra8_ls_sgstubs_end;   /**< Veneer-region end (NSC).   */

/**
 * @enum tz_ns_image_t
 * @brief Fixed NS-image addresses (two-project build).
 *
 * @details The NS image is a SEPARATE ELF (tz_nsc_cgc_usb_ns.elf), so the
 *          Secure side has none of its linker symbols. Its load (MRAM) and run
 *          (SRAM2 NS alias) bases are fixed by the NS linker script; the Secure boot
 *          copies a fixed window large enough for the NS image (ThreadX + USBX
 *          + ra8_usb fit well under 192 KB) and BLXNS-es to slot 1 of the NS
 *          vector table at the run base.
 *
 * @invariant Matches RA8_NS_MRAM_ORIGIN / RA8_NS_SRAM_ORIGIN in
 *             libs/ra8_board_ek_ra8d2/ld/ns_memory_map.cmake.
 */
typedef enum : uintptr_t {
  k_tz_ns_load_base = RA8_NS_MRAM_BASE, /**< NS image LMA (Secure MRAM).    */
  k_tz_ns_run_base  = RA8_NS_SRAM_BASE, /**< NS image VMA (SRAM2 NS alias). */
  k_tz_ns_copy_size = 0x00030000U, /**< Bytes copied LMA->VMA (192 KB). */
} tz_ns_image_t;

#ifdef RA8_TRUSTZONE_ENABLE

/**
 * @enum tz_reg_addr_t
 * @brief Secure-only register addresses touched during NS bring-up.
 *
 * @details SAU lives in the System Control Space (Arm v8-M, mirrored in
 *          HUM section 51.3.3.3). ``SRAMSABARn`` and ``PRCR_S`` live in
 *          the CPSCU / SYSC windows (HUM sections 58.2 and 9.2.4).
 *
 * @invariant All values are secure-only MMIO addresses; writes require
 *            Secure state and (for SRAMSABAR) an open PRC4 gate.
 */
typedef enum : uintptr_t {
  k_tz_psarb_addr = 0x40204004U, /**< PSCU PSARB (peripheral S/NS attr). */
} tz_reg_addr_t;

/**
 * @enum tz_field_t
 * @brief Bit fields / magic values for the SAU and PRCR_S writes.
 *
 * @invariant ``k_tz_sau_rlar_*`` occupy bits [1:0]; the limit address
 *            occupies bits [31:5] (ARMv8-M 32-byte region quantum).
 */
typedef enum : uint32_t {
  k_tz_sau_granule_mask     = 0xFFFFFFE0U, /**< 32-byte SAU quantum mask.       */
  k_tz_psarb_usbfs_ns       = 0x00000800U, /**< PSARB11 = 1: USBFS0 Non-secure. */
  k_tz_psarb_usbhs_ns       = 0x00001000U, /**< PSARB12 = 1: USBHS Non-secure.  */
  k_tz_psarb_usb_ns         = 0x00001800U, /**< PSARB11|12: both USB ctrls NS.  */
} tz_field_t;

/**
 * @enum tz_usb_pin_t
 * @brief Packed ``ra8_port_pin_t`` codes for the four EK-RA8D2 USB-FS pins.
 *
 * @details Packing is ``(port << 8) | pin`` (matches ::ra8_port_pin_t). The
 *          Secure side routes these to the USBFS peripheral function before
 *          BLXNS so the Non-secure USB stack drives a live PHY; PFS ownership
 *          (PMSAR) stays Secure but the muxed signal still reaches the
 *          (Non-secure-attributed) USBFS controller.
 *
 * @invariant Matches the EK-RA8D2 v1 User's Manual USB-FS (J11) pin map.
 */
typedef enum : uint16_t {
  k_tz_usb_pin_vbus    = (uint16_t)k_ra8_board_usbfs_pin_vbus,   /**< P4_07 FS VBUS. */
  k_tz_usb_pin_vbusen  = (uint16_t)k_ra8_board_usbfs_pin_vbusen, /**< P5_00 FS role. */
  k_tz_usb_pin_dp      = (uint16_t)k_ra8_board_usbfs_pin_dp,     /**< P8_14 FS D+.   */
  k_tz_usb_pin_dm      = (uint16_t)k_ra8_board_usbfs_pin_dm,     /**< P8_15 FS D-.   */
  k_tz_usb_pin_hs_vbus = (uint16_t)k_ra8_board_usbhs_pin_vbus,   /**< P4_08 HS VBUS. */
  k_tz_usb_pin_hs_pwr  = (uint16_t)k_ra8_board_usbhs_pin_pwr,    /**< PD07 J7 pwr.   */
} tz_usb_pin_t;

/**
 * @var g_tz_usb_psarb_readback
 * @brief PSARB value read back after marking USBFS0 Non-secure (J-Link probe).
 * @details Secure-side .bss. HUM "Security Bit Write Timing" p3301 requires
 *          reading the attribution register until it matches the written value;
 *          this captures that confirmed value so a bench halt can verify the
 *          USBFS NS delegation landed even if the NS image later faults.
 * @note Written once by ::tz_usb_handoff_prepare; read externally by J-Link.
 * @since 0.1.0
 */
volatile uint32_t g_tz_usb_psarb_readback;

/**
 * @var g_tz_usb_psarb_err
 * @brief ra8_tz_psar_set_ns() result for the USB attribution (J-Link probe).
 */
volatile uint32_t g_tz_usb_psarb_err;

/**
 * @var g_tz_usb_pins_err
 * @brief First non-OK ``ra8_err_t`` from the deterministic USB pin + PLL setup
 *        (FS/HS pins, J7 VBUS GPIO, USBHS PLL; 0 = OK). The I/O-expander is
 *        tracked separately in ::g_tz_usb_expander_err.
 * @note Written once by ::tz_usb_handoff_prepare; read externally by J-Link.
 * @since 0.1.0
 */
volatile uint32_t g_tz_usb_pins_err;

/**
 * @var g_tz_usb_expander_err
 * @brief Last ``ra8_err_t`` from the U15 I/O-expander host-mode write after the
 *        bounded retry loop (0 = OK).
 * @details Decoupled from ::g_tz_usb_pins_err because the RIIC1 BBSY flag can
 *          survive a warm reset (SYSRESETREQ), making the first expander write
 *          report k_ra8_err_busy until the bus-recovery in a later retry (or a
 *          cold boot) clears it. The external PI4IOE latches its host-mode
 *          output, so the USBHS host role persists across the MCU warm reset.
 * @note Written once by ::tz_usb_handoff_prepare; read externally by J-Link.
 * @since 0.1.0
 */
volatile uint32_t g_tz_usb_expander_err;

/**
 * @var g_tz_jump_ns_err
 * @brief Denial verdict from ::ra8_tz_secure_boot_jump_ns (0 = never denied).
 * @details On hardware ::ra8_tz_secure_boot_jump_ns only returns when it
 *          REFUSED to branch: the NS vector table failed validation or the
 *          root-of-trust gate rejected the NS image. The verdict is latched
 *          here so a J-Link probe can tell "NS image denied" apart from "SAU
 *          programming failed" when the S-side fallback main() is reached
 *          instead of the NS world.
 * @note Written once by ::ra8_trustzone_init on the denial path; read
 *       externally by J-Link.
 * @since 0.1.0
 */
volatile uint32_t g_tz_jump_ns_err;

/**
 * @enum tz_partition_t
 * @brief IDAU-NS range geometry and the per-bank SRAM boundary.
 *
 * @details The IDAU-NS ranges are mandated NS-in-SAU by HUM p3267, expressed
 *          here as base + size because that is what ``ra8_sau_region_t``
 *          takes; the driver derives the ``base + size - 32`` RLAR limit once
 *          so a limit computed one region short cannot leave secure memory
 *          reachable. The SRAMSABAR offsets place the secure/NS boundary at
 *          physical 0x10_0000 so SRAM2 [0x10_0000, 0x18_0000) is the NS
 *          aperture.
 */
typedef enum : uint32_t {
  k_tz_ns_code_base   = 0x10000000U, /**< IDAU-NS code alias base.         */
  k_tz_ns_code_size   = 0x10000000U, /**< IDAU-NS code alias length.       */
  k_tz_ns_sram_base   = 0x30000000U, /**< IDAU-NS SRAM alias base.         */
  k_tz_ns_sram_size   = 0x10000000U, /**< IDAU-NS SRAM alias length.       */
  k_tz_ns_per_base    = 0x50000000U, /**< IDAU-NS peripheral alias base.   */
  k_tz_ns_per_size    = 0x90000000U, /**< IDAU-NS peripheral alias length. */
  k_tz_sramsabar0_val = 0x00080000U, /**< SRAM0 all Secure (>= bank end).  */
  k_tz_sramsabar1_val = 0x00100000U, /**< SRAM1 all Secure (>= bank end).  */
  k_tz_sramsabar2_val = 0x00100000U, /**< SRAM2 all NS (boundary at base). */
  k_tz_sramsabar3_val = 0x001A0000U, /**< SRAM3 all Secure (>= bank end).  */
} tz_partition_t;

/**
 * @brief Write a 32-bit secure MMIO register.
 * @details Generic store; each caller documents the target register page.
 * @param[in] addr  Secure-only MMIO address.
 * @param[in] value Value to store.
 * @pre Caller is in Secure state.
 * @pre ``addr`` is one of ::tz_reg_addr_t.
 * @post The 32-bit store has landed (verifiable via SWD read-back).
 * @post No other register is affected.
 * @note Not thread-safe; secure-boot only.
 * @since 0.1.0
 */
static inline void tz_write32(uintptr_t addr, uint32_t value)
{
  /* HUM Ch 51.3.3.3 "Secure Attribution Unit (SAU)" p 3266 -- generic
   * secure-MMIO store; SAU / CPSCU callers cite their own page below. */
  *(volatile uint32_t*)addr = value;
}

/**
 * @brief Read a 32-bit secure MMIO register.
 * @param[in] addr Secure-only MMIO address.
 * @return The register contents.
 * @retval 0 Possible for an unimplemented field.
 * @pre Caller is in Secure state.
 * @pre ``addr`` is one of ::tz_reg_addr_t.
 * @post No state change.
 * @post The returned value reflects the live register.
 * @note Not thread-safe; secure-boot only.
 * @since 0.1.0
 */
static inline uint32_t tz_read32(uintptr_t addr)
{
  /* HUM Ch 51.3.3.3 "Secure Attribution Unit (SAU)" p 3266 */
  return *(volatile uint32_t*)addr;
}

/**
 * @brief Build this app's attribution map and hand it to the shared applier.
 *
 * @details The layout is four SAU regions plus the four SRAMSABAR boundaries:
 *          region 0 is NSC over the ``.gnu.sgstubs`` veneers (in the
 *          bit[28]=0 Secure code region, so the IDAU permits NSC) and
 *          regions 1-3 mark the three IDAU-NS ranges Non-secure as HUM p3267
 *          mandates. Everything else stays Secure (ALLNS = 0). The veneer
 *          bounds are linker-placed, so the descriptor is built here rather
 *          than held as static data.
 *
 *          ``ra8_tz_partition_apply`` validates the whole descriptor before
 *          its first write, programmes the SAU through ``ra8_sau_configure``
 *          (which also clears every region above the count, so an enabled
 *          window cannot be inherited from the boot ROM), and then writes the
 *          SRAM boundaries inside its own PRCR_S.PRC4 scope. Do not wrap this
 *          call in ``RA8_PROTECTED_WRITE``: the applier owns that window and
 *          a nested scope would re-lock every group on the inner exit.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                 SAU enabled and the four SRAM boundaries
 *                                  written.
 * @retval k_ra8_err_invalid_state  The veneer linker symbols bound an empty
 *                                  range, so region 0 would have no extent.
 * @retval k_ra8_err_not_supported  SAU_TYPE.SREGION < 4.
 * @retval k_ra8_err_invalid_arg    A region or boundary failed the geometry
 *                                  checks; no register was written.
 *
 * @pre Caller is in Secure state with the SAU disabled.
 * @post On success SAU_CTRL.ENABLE = 1 and SRAM2 [0x10_0000, 0x18_0000) is
 *       Non-secure (alias 0x3210_0000).
 * @post On failure no attribution register was written.
 * @note Not thread-safe; secure-boot only.
 * @since 0.1.0
 */
static ra8_err_t tz_partition_apply(void)
{
  /* RBAR/RLAR are 32-byte quantised: round the base down and the last byte
   * of the block (end - 1) down to the same quantum, then express the pair
   * as the base + size the SAU driver takes. */
  const uint32_t nsc_base =
    (uint32_t)(uintptr_t)&g_ra8_ls_sgstubs_start & (uint32_t)k_tz_sau_granule_mask;
  const uint32_t nsc_end = (uint32_t)(uintptr_t)&g_ra8_ls_sgstubs_end;
  if (nsc_end <= nsc_base) {
    return k_ra8_err_invalid_state;
  }
  const uint32_t nsc_last = (nsc_end - 1U) & (uint32_t)k_tz_sau_granule_mask;

  const ra8_sau_region_t regions[] = {
    {.base = (uintptr_t)nsc_base,
     .size = (nsc_last - nsc_base) + (uint32_t)k_ra8_sau_region_granule,
     .attr = k_ra8_sau_attr_nsc},
    {.base = (uintptr_t)k_tz_ns_code_base,
     .size = (uint32_t)k_tz_ns_code_size,
     .attr = k_ra8_sau_attr_ns},
    {.base = (uintptr_t)k_tz_ns_sram_base,
     .size = (uint32_t)k_tz_ns_sram_size,
     .attr = k_ra8_sau_attr_ns},
    {.base = (uintptr_t)k_tz_ns_per_base,
     .size = (uint32_t)k_tz_ns_per_size,
     .attr = k_ra8_sau_attr_ns},
  };

  /* HUM Ch 58.2 "SRAMSABARn : SRAM Security Attribute Boundary Address
   * Register" p 3527 -- boundary = start address of the NS region; below is
   * Secure, at/above is Non-secure. */
  const uint32_t sram_boundary[k_ra8_tz_partition_sram_bank_count] = {
    (uint32_t)k_tz_sramsabar0_val,
    (uint32_t)k_tz_sramsabar1_val,
    (uint32_t)k_tz_sramsabar2_val,
    (uint32_t)k_tz_sramsabar3_val,
  };

  const ra8_tz_partition_t partition = {
    .sau_regions      = regions,
    .sram_boundary    = sram_boundary,
    .sau_region_count = (uint8_t)(sizeof(regions) / sizeof(regions[0])),
    .sau_all_ns       = false,
  };
  return ra8_tz_partition_apply(&partition);
}

/**
 * @brief Copy the NS image from its MRAM LMA into the SRAM NS alias.
 *
 * @details Plain word copy of the fixed ::k_tz_ns_copy_size window from
 *          ::k_tz_ns_load_base (MRAM) to ::k_tz_ns_run_base (0x3210_0000).
 *          Runs AFTER the SAU and SRAMSABAR have marked the destination
 *          Non-secure, so the store is a (permitted) Secure-side Non-secure
 *          access. A fixed window is used because the NS image is a separate
 *          ELF; copying more than the image is harmless.
 *
 * @pre ``tz_partition_apply`` has run and returned k_ra8_ok.
 * @pre The NS image fits within ::k_tz_ns_copy_size.
 * @post The NS vector table + text + rodata + data are live at 0x3210_0000.
 * @post The source MRAM image is unchanged.
 * @note Not thread-safe; secure-boot only.
 * @since 0.1.0
 */
static void tz_copy_ns_image(void)
{
  const uintptr_t src_start = (uintptr_t)k_tz_ns_load_base;
  const uintptr_t src_end   = src_start + (uintptr_t)k_tz_ns_copy_size;
  uintptr_t       dst       = (uintptr_t)k_tz_ns_run_base;
  for (uintptr_t src = src_start; src < src_end; src += sizeof(uint32_t)) {
    *(volatile uint32_t*)dst = *(const volatile uint32_t*)src;
    dst += sizeof(uint32_t);
  }
}

/**
 * @brief Route the USB-FS device pins + USBHS host pins and enable the HS PLL.
 *
 * @details Resets the pin validator (stale claims survive warm resets), routes
 *          the four USB-FS pins as the DEVICE (P5_00 LOW), the two USBHS host
 *          pins (PD07 HIGH for J7 VBUS, P4_08 USBHS_VBUS), then enables the
 *          USBHS UTMI PLL. A warm reset leaves the PLL running, so
 *          ``k_ra8_err_busy`` is treated as success. The first failing step's
 *          error is returned; later steps are short-circuited.
 *
 * @return ra8_err_t First non-OK error from the pin/PLL chain (0 = OK).
 * @retval k_ra8_ok All pins routed and the USBHS PLL is up.
 * @pre Caller is in Secure state with full peripheral access.
 * @pre ``ra8_cgc_init`` has run (PLL1 locked).
 * @post The USB pins are muxed and PD07 is HIGH on success.
 * @post The pin-validator bitmap reflects only this routine's claims.
 * @note Not thread-safe; secure-boot only.
 * @since 0.1.0
 */
static ra8_err_t tz_usb_route_pins(void)
{
  /* Establish the pin-validator baseline. The Secure boot never runs
   * ra8_infrastructure_init (its main() is dead -- BLXNS does not return), so
   * the validator bitmap is in an uninitialised-contract state and warm
   * resets leave stale claims (SRAM survives SYSRESETREQ; PFS does not), which
   * would make the USB-pin claims spuriously conflict. Reset it first. */
  ra8_pin_validator_reset();

  /* Route the USB-FS pins as the DEVICE (Secure owns PFS). P5_00 LOW = dev. */
  ra8_err_t err = ra8_pfs_route_peripheral((ra8_port_pin_t)k_tz_usb_pin_vbus,
                                           k_ra8_psel_usb_fs,
                                           "tz_usb.fs_vbus");
  if (err == k_ra8_ok) {
    err = ra8_gpio_output_init((ra8_port_pin_t)k_tz_usb_pin_vbusen, k_ra8_level_low);
  }
  if (err == k_ra8_ok) {
    err =
      ra8_pfs_route_peripheral((ra8_port_pin_t)k_tz_usb_pin_dp, k_ra8_psel_usb_fs, "tz_usb.fs_dp");
  }
  if (err == k_ra8_ok) {
    err =
      ra8_pfs_route_peripheral((ra8_port_pin_t)k_tz_usb_pin_dm, k_ra8_psel_usb_fs, "tz_usb.fs_dm");
  }

  /* USBHS host pins: PD07 HIGH (U18 supplies J7 VBUS), route P4_08
   * USBHS_VBUS. (The host-mode mux is the U15 expander -- handled later.) */
  if (err == k_ra8_ok) {
    err = ra8_gpio_output_init((ra8_port_pin_t)k_tz_usb_pin_hs_pwr, k_ra8_level_high);
  }
  if (err == k_ra8_ok) {
    err = ra8_pfs_route_peripheral((ra8_port_pin_t)k_tz_usb_pin_hs_vbus,
                                   k_ra8_psel_usb_hs,
                                   "tz_usb.hs_vbus");
  }

  /* Enable the USBHS UTMI PLL (Secure CGC; PLL1 already locked). A warm
   * reset leaves the CGC PLL domain running, so a re-enable reports
   * k_ra8_err_busy ("already locked"); the clock is up either way, so treat
   * busy as success. */
  if (err == k_ra8_ok) {
    const ra8_err_t pll_err = ra8_cgc_usbhs_pll_enable();
    if ((pll_err != k_ra8_ok) && (pll_err != k_ra8_err_busy)) {
      err = pll_err;
    }
  }
  return err;
}

/**
 * @brief Mark BOTH USB controllers Non-secure in PSARB (bits 11 + 12).
 *
 * @details Via ra8_tz_psar_set_ns(): opens the PRCR_S.PRC4 gate, sets PSARB11
 *          (USBHS) so the NS image reaches them through the 0x5025_0000 /
 *          0x5035_0000 aliases, spins (bounded) on the read-back until the
 *          value confirms, then re-locks PRC4. The confirmed value is stored
 *          in ::g_tz_usb_psarb_readback for a bench halt to verify.
 *
 * @return void.
 * @pre Caller is in Secure state.
 * @pre The USB pins/PLL setup has run.
 * @post PSARB.PSARB11|PSARB12 = 1 (both USB controllers Non-secure).
 * @post PRCR_S.PRC4 is cleared; ::g_tz_usb_psarb_readback holds the confirmed
 *       value.
 * @note Not thread-safe; secure-boot only.
 * @since 0.1.0
 */
static void tz_usb_mark_ns(void)
{
  /* PSARB11 = USBFS0, PSARB12 = USBHS (HUM Ch 51.8.1 p 3284); 0 = Secure,
   * 1 = Non-secure, so the NS image reaches them through the 0x5025_0000 /
   * 0x5035_0000 aliases. The PRC4 gate, the mandatory read-back and the
   * re-lock live in ra8_tz_psar_set_ns(); the confirmed value is kept for a
   * bench halt to verify. */
  uint32_t        seen = 0U;
  const ra8_err_t err =
      ra8_tz_psar_set_ns((uintptr_t)k_tz_psarb_addr, (uint32_t)k_tz_psarb_usb_ns, &seen);
  g_tz_usb_psarb_readback = seen;
  g_tz_usb_psarb_err      = (uint32_t)err;
}

/**
 * @brief Hand BOTH USB controllers to the Non-secure world before BLXNS.
 *
 * @details The NS image runs the USB CDC self-loop: USBFS (J11) is the CDC-ACM
 *          DEVICE, USBHS (J7) is the polled HOST, and the two jacks are cabled
 *          together so the chip enumerates + echoes against itself. This routine
 *          does the Secure-only bring-up the NS image cannot:
 *          1. Route the four USB-FS pins (device role: P5_00 LOW).
 *          2. Set USBHS to host mode: U15 I/O-expander SW4-8 -> Host, PD07 HIGH
 *             (U18 supplies J7 VBUS), and route P4_08 USBHS_VBUS.
 *          3. Enable the USBHS UTMI PLL (``ra8_cgc_usbhs_pll_enable`` -- CGC is
 *             Secure-only; the 48 MHz USBFS clock is enabled by the NS image via
 *             the NSC CGC veneer).
 *          4. Mark BOTH controllers Non-secure in PSARB (bits 11 + 12) under the
 *             PRC4 gate, so the NS image reaches USBFS/USBHS through the
 *             0x5025_0000 / 0x5035_0000 aliases and may clear their
 *             MSTPCRB.MSTPB11/12 module-stop bits itself.
 *
 * @return void.
 * @pre Caller is in Secure state with full peripheral access (pre-BLXNS).
 * @pre ``ra8_cgc_init`` has run (PLL1 locked -- USBHS PLL needs it).
 * @post The USB pins are muxed and PD07 is HIGH (or ::g_tz_usb_pins_err records
 *       the first failing step).
 * @post PSARB.PSARB11|PSARB12 = 1 (both USB controllers Non-secure);
 *       ::g_tz_usb_psarb_readback holds the confirmed value.
 * @note Not thread-safe; secure-boot only.
 * @since 0.1.0
 */
static void tz_usb_handoff_prepare(void)
{
  /* 1+2+3. Route USB pins (FS device + HS host) and enable the USBHS PLL. */
  const ra8_err_t pins_err = tz_usb_route_pins();
  g_tz_usb_pins_err        = (uint32_t)pins_err;

  /* 4. Set the U15 I/O-expander to USBHS host mode (SW4-8 -> Host). Decoupled
   *    from the deterministic setup above and best-effort: on a cold boot the
   *    single I2C write lands (probe -> success); after a warm reset RIIC1's
   *    BBSY can still be set, reporting k_ra8_err_busy. The external PI4IOE
   *    latches its host-mode output, so the USBHS host role persists across the
   *    MCU warm reset -- the self-loop still enumerates (the HIL gate proves
   *    it). A retry cannot help here: the expander claims the SCL1/SDA1 pins on
   *    the first try and a second try would fault the pin validator. */
  const ra8_err_t exp_err = ra8_board_io_expander_set_usbhs_host_mode();
  g_tz_usb_expander_err   = (uint32_t)exp_err;

  /* 5. Mark BOTH USB controllers Non-secure in PSARB (bits 11 + 12). */
  tz_usb_mark_ns();
}

#endif /* RA8_TRUSTZONE_ENABLE */

void ra8_trustzone_init(void)
{
#ifdef RA8_TRUSTZONE_ENABLE
  /* 0. Hand USB-FS (pins + PSARB NS attribution) to the NS world. */
  tz_usb_handoff_prepare();

  /* 1. Apply the attribution map: the bit[28] SAU layout (NSC veneers +
   *    IDAU-NS ranges) and then the SRAMSABAR boundary that carves the SRAM2
   *    NS aperture. The SAU goes first because it is the coarse map the SRAM
   *    boundary refines, and neither is observable until step 3 touches the
   *    NS alias. */
  if (tz_partition_apply() != k_ra8_ok) {
    return; /* Fall through to the S-side main() fallback. */
  }

  /* 3. Copy the NS image into the now-Non-secure SRAM alias. */
  tz_copy_ns_image();

  /* 4. Clear the Secure PRIMASK before handing off. SystemInit masked IRQs
   *    (CPSID i) for secure bring-up; a set PRIMASK_S boosts the execution
   *    priority and masks NON-secure exceptions too, so the NS ThreadX
   *    PendSV / SysTick would never fire. The NS side cannot clear PRIMASK_S
   *    (it is Secure-banked), so do it here, right before BLXNS. */
  __asm__ volatile("cpsie i" ::: "memory");

  /* 5. BLXNS into the NS reset vector (slot 1 of the NS vector table at the
   *    fixed run base 0x3210_0000). Does not return on hardware. */
  const ra8_err_t jump_err =
    ra8_tz_secure_boot_jump_ns((const uint32_t*)(uintptr_t)k_tz_ns_run_base);
  if (jump_err != k_ra8_ok) {
    /* Reached on hardware ONLY when the NS image was DENIED (vector-table
     * validation or the root-of-trust gate failed). Latch the verdict for a
     * J-Link probe and fall through to the S-side main() fallback -- never
     * treat the NS world as live. */
    g_tz_jump_ns_err = (uint32_t)jump_err;
    return;
  }

  /* On host (RA8_OFF_TARGET) the library stubs BLXNS and returns
   * k_ra8_ok; on target this point is unreachable. */
#endif
}
