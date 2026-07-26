/**
 * @file examples/ek_ra8d2/hil_needs_revalidation/tz_nsc_cgc_usb/trustzone_init.c
 * @brief Single-core TrustZone bring-up for a RAM-resident NS image (#60)
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
 * On a host build (``RA8_SIMULATOR_MODE``) this function is a no-op.
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
#include "ra8_tz_secure_boot.h"

/* Bounds of the NSC veneer stubs (.gnu.sgstubs) in this (Secure) image. */
extern uint32_t g_ra8_ls_sgstubs_start; /**< Veneer-region start (NSC). */
extern uint32_t g_ra8_ls_sgstubs_end;   /**< Veneer-region end (NSC).   */

/**
 * @enum tz_ns_image_t
 * @brief Fixed NS-image addresses (two-project build, #96).
 *
 * @details The NS image is a SEPARATE ELF (tz_nsc_cgc_usb_ns.elf), so the
 *          Secure side has none of its linker symbols. Its load (MRAM) and run
 *          (SRAM2 NS alias) bases are fixed by ns_image.ld; the Secure boot
 *          copies a fixed window large enough for the NS image (ThreadX + USBX
 *          + ra8_usb fit well under 192 KB) and BLXNS-es to slot 1 of the NS
 *          vector table at the run base.
 *
 * @invariant Matches ORIGIN(NS_LOAD) / ORIGIN(NS_SRAM_RUN) in ns_image.ld.
 */
typedef enum : uintptr_t {
  k_tz_ns_load_base = 0x02080000U, /**< NS image LMA (Secure MRAM).     */
  k_tz_ns_run_base  = 0x32100000U, /**< NS image VMA (SRAM2 NS alias).  */
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
  k_tz_sau_ctrl_addr   = 0xE000EDD0U, /**< SAU Control Register.              */
  k_tz_sau_type_addr   = 0xE000EDD4U, /**< SAU Type (implemented regions).    */
  k_tz_sau_rnr_addr    = 0xE000EDD8U, /**< SAU Region Number.                 */
  k_tz_sau_rbar_addr   = 0xE000EDDCU, /**< SAU Region Base Address.           */
  k_tz_sau_rlar_addr   = 0xE000EDE0U, /**< SAU Region Limit Address.          */
  k_tz_sramsabar0_addr = 0x40008400U, /**< CPSCU SRAMSABAR0 (+4*n for n>0).   */
  k_tz_prcr_s_addr     = 0x4001E3FAU, /**< SYSC PRCR_S (16-bit).              */
  k_tz_psarb_addr      = 0x40204004U, /**< PSCU PSARB (peripheral S/NS attr). */
} tz_reg_addr_t;

/**
 * @enum tz_field_t
 * @brief Bit fields / magic values for the SAU and PRCR_S writes.
 *
 * @invariant ``k_tz_sau_rlar_*`` occupy bits [1:0]; the limit address
 *            occupies bits [31:5] (ARMv8-M 32-byte region quantum).
 */
typedef enum : uint32_t {
  k_tz_sau_ctrl_enable      = 0x00000001U, /**< SAU_CTRL.ENABLE, ALLNS = 0.     */
  k_tz_sau_rlar_enable      = 0x00000001U, /**< SAU_RLAR.ENABLE.                */
  k_tz_sau_rlar_nsc         = 0x00000002U, /**< SAU_RLAR.NSC (Non-secure call). */
  k_tz_sau_limit_mask       = 0xFFFFFFE0U, /**< 32-byte-aligned limit mask.     */
  k_tz_sau_type_mask        = 0x000000FFU, /**< SAU_TYPE.SREGION field mask.    */
  k_tz_psarb_usbfs_ns       = 0x00000800U, /**< PSARB11 = 1: USBFS0 Non-secure. */
  k_tz_psarb_usbhs_ns       = 0x00001000U, /**< PSARB12 = 1: USBHS Non-secure.  */
  k_tz_psarb_usb_ns         = 0x00001800U, /**< PSARB11|12: both USB ctrls NS.  */
  k_tz_psarb_readback_spins = 1000U,       /**< Bounded read-back confirm loop. */
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
 * @enum tz_prcr_t
 * @brief PRCR_S unlock/lock key values for the PRC4 (CPSCU) write gate.
 *
 * @invariant The top byte is the 0xA5 key; bit 4 is PRC4.
 */
typedef enum : uint16_t {
  k_tz_prcr_s_open  = 0xA510U, /**< Key | PRC4 set (unlock CPSCU writes). */
  k_tz_prcr_s_close = 0xA500U, /**< Key | PRC4 clear (re-lock).           */
} tz_prcr_t;

/**
 * @enum tz_partition_t
 * @brief SAU region count, IDAU-NS range bounds, and the SRAM boundary.
 *
 * @details The IDAU-NS ranges are mandated NS-in-SAU by HUM p3267. The
 *          SRAMSABAR offsets place the secure/NS boundary at physical
 *          0x10_0000 so SRAM2 [0x10_0000, 0x18_0000) is the NS aperture.
 *
 * @invariant Region count <= the value SAU_TYPE reports.
 */
typedef enum : uint32_t {
  k_tz_sau_min_regions = 4U,          /**< Regions this layout programs.    */
  k_tz_ns_code_base    = 0x10000000U, /**< IDAU-NS code alias base.         */
  k_tz_ns_code_limit   = 0x1FFFFFE0U, /**< IDAU-NS code alias limit.        */
  k_tz_ns_sram_base    = 0x30000000U, /**< IDAU-NS SRAM alias base.         */
  k_tz_ns_sram_limit   = 0x3FFFFFE0U, /**< IDAU-NS SRAM alias limit.        */
  k_tz_ns_per_base     = 0x50000000U, /**< IDAU-NS peripheral alias base.   */
  k_tz_ns_per_limit    = 0xDFFFFFE0U, /**< IDAU-NS peripheral alias limit.  */
  k_tz_sramsabar0_val  = 0x00080000U, /**< SRAM0 all Secure (>= bank end).  */
  k_tz_sramsabar1_val  = 0x00100000U, /**< SRAM1 all Secure (>= bank end).  */
  k_tz_sramsabar2_val  = 0x00100000U, /**< SRAM2 all NS (boundary at base). */
  k_tz_sramsabar3_val  = 0x001A0000U, /**< SRAM3 all Secure (>= bank end).  */
} tz_partition_t;

/**
 * @brief Region indices for the bit[28] SAU layout.
 */
typedef enum : uint8_t {
  k_tz_region_nsc     = 0U, /**< NSC veneers (.gnu.sgstubs in MRAM).    */
  k_tz_region_ns_code = 1U, /**< 0x1000_0000-0x1FFF_FFFF NS.            */
  k_tz_region_ns_sram = 2U, /**< 0x3000_0000-0x3FFF_FFFF NS (NS image). */
  k_tz_region_ns_per  = 3U, /**< 0x5000_0000-0xDFFF_FFFF NS.            */
} tz_region_t;

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
 * @brief Programme one SAU region (RNR -> RBAR -> RLAR).
 * @param[in] region Region index (0..SAU_TYPE.SREGION-1).
 * @param[in] base   32-byte-aligned region base address.
 * @param[in] limit  32-byte-aligned region limit (last byte, low 5 = 0).
 * @param[in] is_nsc true to set RLAR.NSC (Non-secure-callable).
 * @pre Caller is in Secure state and the SAU is currently disabled.
 * @pre ``base`` <= ``limit`` and both are 32-byte aligned.
 * @post The region's RBAR/RLAR hold the requested bounds with ENABLE = 1.
 * @post The NSC bit matches ``is_nsc``.
 * @note Not thread-safe; secure-boot only.
 * @since 0.1.0
 */
static void tz_sau_set_region(uint8_t region, uint32_t base, uint32_t limit, bool is_nsc)
{
  /* HUM Ch 51.3.3.3 "Secure Attribution Unit (SAU)" p 3266 */
  tz_write32(k_tz_sau_rnr_addr, (uint32_t)region);
  tz_write32(k_tz_sau_rbar_addr, base & (uint32_t)k_tz_sau_limit_mask);
  uint32_t rlar = (limit & (uint32_t)k_tz_sau_limit_mask) | (uint32_t)k_tz_sau_rlar_enable;
  if (is_nsc) {
    rlar |= (uint32_t)k_tz_sau_rlar_nsc;
  }
  tz_write32(k_tz_sau_rlar_addr, rlar);
}

/**
 * @brief Mark physical SRAM2 Non-secure via the runtime SRAMSABAR registers.
 *
 * @details Opens the PRCR_S.PRC4 gate, writes SRAMSABAR0..3 so the
 *          secure/NS boundary sits at physical offset 0x10_0000 (SRAM2 and
 *          above Non-secure, lower SRAM Secure), then re-locks PRC4. This
 *          is the RAM-resident NS aperture; it is reset-cleared, so there
 *          is no persistent option-byte / brick exposure.
 *
 * @pre Caller is in Secure state.
 * @pre The Secure stack lives below physical 0x10_0000 (SRAM0/SRAM1).
 * @post SRAM2 [0x10_0000, 0x18_0000) is Non-secure (alias 0x3210_0000).
 * @post PRCR_S.PRC4 is cleared (write-protect restored).
 * @note Not thread-safe; secure-boot only.
 * @since 0.1.0
 */
static void tz_sram_ns_boundary(void)
{
  /* Open PRC4 so the CPSCU SRAMSABAR writes below land. */
  /* HUM Ch 9.2.4 "PRCR_S" p 397 */
  *(volatile uint16_t*)k_tz_prcr_s_addr = (uint16_t)k_tz_prcr_s_open;

  /* HUM Ch 58.2 "SRAMSABARn : SRAM Security Attribute Boundary Address
   * Register" p 3527 -- boundary = start address of the NS region; below
   * = Secure, at/above = Non-secure. */
  tz_write32(k_tz_sramsabar0_addr + (0U * sizeof(uint32_t)), (uint32_t)k_tz_sramsabar0_val);
  tz_write32(k_tz_sramsabar0_addr + (1U * sizeof(uint32_t)), (uint32_t)k_tz_sramsabar1_val);
  tz_write32(k_tz_sramsabar0_addr + (2U * sizeof(uint32_t)), (uint32_t)k_tz_sramsabar2_val);
  tz_write32(k_tz_sramsabar0_addr + (3U * sizeof(uint32_t)), (uint32_t)k_tz_sramsabar3_val);

  /* Re-lock PRC4 (restore CPSCU write-protect). */
  /* HUM Ch 9.2.4 "PRCR_S" p 397 */
  *(volatile uint16_t*)k_tz_prcr_s_addr = (uint16_t)k_tz_prcr_s_close;
}

/**
 * @brief Programme the bit[28] SAU layout and enable the SAU.
 *
 * @details Region 0 = NSC over the ``.gnu.sgstubs`` veneers (in the
 *          bit[28]=0 Secure code region, so the IDAU permits NSC).
 *          Regions 1-3 mark the three IDAU-NS ranges Non-secure as HUM
 *          p3267 mandates. Everything else stays Secure (ALLNS = 0).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                SAU programmed and enabled.
 * @retval k_ra8_err_not_supported SAU_TYPE.SREGION < 4.
 *
 * @pre Caller is in Secure state with the SAU disabled.
 * @pre The veneer linker symbols bound a non-empty 32-byte-aligned range.
 * @post On success SAU_CTRL.ENABLE = 1 with the four regions above.
 * @post On failure the SAU stays disabled (default-allow Secure).
 * @note Not thread-safe; secure-boot only.
 * @since 0.1.0
 */
static ra8_err_t tz_sau_program(void)
{
  /* HUM Ch 51.3.3.3 "Secure Attribution Unit (SAU)" p 3266 -- need at
   * least 4 implemented regions for this layout. */
  const uint32_t sau_type = tz_read32(k_tz_sau_type_addr);
  if ((sau_type & (uint32_t)k_tz_sau_type_mask) < (uint32_t)k_tz_sau_min_regions) {
    return k_ra8_err_not_supported;
  }

  /* RLAR limit is the LAST byte of the veneer block (end - 1); the helper
   * masks it to the 32-byte SAU region quantum. */
  const uint32_t nsc_base  = (uint32_t)(uintptr_t)&g_ra8_ls_sgstubs_start;
  const uint32_t nsc_limit = (uint32_t)(uintptr_t)&g_ra8_ls_sgstubs_end - 1U;
  tz_sau_set_region((uint8_t)k_tz_region_nsc, nsc_base, nsc_limit, /*is_nsc=*/true);
  tz_sau_set_region((uint8_t)k_tz_region_ns_code,
                    (uint32_t)k_tz_ns_code_base,
                    (uint32_t)k_tz_ns_code_limit,
                    /*is_nsc=*/false);
  tz_sau_set_region((uint8_t)k_tz_region_ns_sram,
                    (uint32_t)k_tz_ns_sram_base,
                    (uint32_t)k_tz_ns_sram_limit,
                    /*is_nsc=*/false);
  tz_sau_set_region((uint8_t)k_tz_region_ns_per,
                    (uint32_t)k_tz_ns_per_base,
                    (uint32_t)k_tz_ns_per_limit,
                    /*is_nsc=*/false);

  __asm__ volatile("dsb 0xF" ::: "memory");
  /* Enable the SAU with ALLNS = 0 (default-deny). */
  /* HUM Ch 51.3.3.3 "Secure Attribution Unit (SAU)" p 3266 */
  tz_write32(k_tz_sau_ctrl_addr, (uint32_t)k_tz_sau_ctrl_enable);
  __asm__ volatile("dsb 0xF" ::: "memory");
  __asm__ volatile("isb" ::: "memory");
  return k_ra8_ok;
}

/**
 * @brief Copy the NS image from its MRAM LMA into the SRAM NS alias.
 *
 * @details Plain word copy of the fixed ::k_tz_ns_copy_size window from
 *          ::k_tz_ns_load_base (MRAM) to ::k_tz_ns_run_base (0x3210_0000).
 *          Runs AFTER the SAU and SRAMSABAR have marked the destination
 *          Non-secure, so the store is a (permitted) Secure-side Non-secure
 *          access. A fixed window is used because the NS image is a separate
 *          ELF (#96); copying more than the image is harmless.
 *
 * @pre ``tz_sram_ns_boundary`` and ``tz_sau_program`` have run.
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
 * @details Opens the PRCR_S.PRC4 gate, sets PSARB11 (USBFS0) and PSARB12
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
  /* HUM Ch 51.8.1 "PSARB : Peripheral Security Attribution Register B" p 3284
   * -- PSARB11 = USBFS0, PSARB12 = USBHS (+ their MSTPCRB.MSTPB11/12 bits);
   * 0 = Secure, 1 = Non-secure. PSARx share the PRCR_S.PRC4 write gate with
   * SRAMSABARn (HUM Ch 13.2.1 "Association between PRCR bits and use of
   * registers" p 521), so the gate must be open across the write; HUM "Security
   * or Privilege Bit Write Timing" p 3301 then requires reading back until the
   * value matches. */
  /* HUM Ch 9.2.4 "PRCR_S" p 397 -- open PRC4. */
  *(volatile uint16_t*)k_tz_prcr_s_addr = (uint16_t)k_tz_prcr_s_open;
  const uint32_t want                   = tz_read32(k_tz_psarb_addr) | (uint32_t)k_tz_psarb_usb_ns;
  tz_write32(k_tz_psarb_addr, want);
  uint32_t seen = 0U;
  for (uint32_t spin = 0U; spin < (uint32_t)k_tz_psarb_readback_spins; spin += 1U) {
    seen = tz_read32(k_tz_psarb_addr);
    if (seen == want) {
      break;
    }
  }
  /* HUM Ch 9.2.4 "PRCR_S" p 397 -- re-lock PRC4. */
  *(volatile uint16_t*)k_tz_prcr_s_addr = (uint16_t)k_tz_prcr_s_close;
  g_tz_usb_psarb_readback               = seen;
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

  /* 1. Carve the SRAM2 NS aperture via the runtime SRAMSABAR boundary. */
  tz_sram_ns_boundary();

  /* 2. Programme the bit[28] SAU (NSC veneers + IDAU-NS ranges), enable. */
  if (tz_sau_program() != k_ra8_ok) {
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

  /* On host (RA8_SIMULATOR_MODE) the library stubs BLXNS and returns
   * k_ra8_ok; on target this point is unreachable. */
#endif
}
