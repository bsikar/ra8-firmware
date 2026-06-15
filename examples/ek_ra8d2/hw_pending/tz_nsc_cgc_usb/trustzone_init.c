/**
 * @file examples/ek_ra8d2/hw_pending/tz_nsc_cgc_usb/trustzone_init.c
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
 *   4. BLXNS into the NS reset vector via ``ra_tz_secure_boot_jump_ns``.
 *
 * This file does NOT use ``ra_tz_secure_boot_sau_init`` -- that function's
 * region table is tuned for cpu1_pingpong_ipc (CPU1 is the NS master) and
 * is shared; the bit[28] model here is app-local so that validated app is
 * untouched. Only the generic ``ra_tz_secure_boot_jump_ns`` primitive is
 * reused.
 *
 * On a host build (``RA_SIMULATOR_MODE``) this function is a no-op.
 *
 * @par TrustZone Safety:
 *  - **Validates:** SAU_TYPE.SREGION >= 4 before programming.
 *  - **Validates:** ``g_ra_ns_vector_table`` non-NULL + word-aligned
 *    (checked by ``ra_tz_secure_boot_jump_ns``).
 *  - **Trusts:** the boot ROM left the SAU disabled and the IDAU in its
 *    documented reset state (fixed bit[28] split).
 *  - **Denies:** any return path from ``ra_tz_secure_boot_jump_ns`` on
 *    hardware -- BLXNS leaves Secure thread mode.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "trustzone_init.h"

#include <stdint.h>

#include "ra_err.h"
#include "ra_tz_secure_boot.h"

/**
 * @var g_ra_ns_vector_table
 * @brief Defined in ``ns_main.c``; lives in ``.ns_vectors`` whose VMA the
 *        linker pins at ``ORIGIN(NS_SRAM_RUN)`` = 0x3210_0000. Forward-
 *        declared here so we can pass its address to the secure-boot
 *        library after the NS image has been copied into place.
 */
extern const uint32_t g_ra_ns_vector_table[];

/* Linker-provided bounds of the NS load image (LMA in Secure MRAM) and the
 * run-time destination (VMA = SRAM NS alias 0x3210_0000). */
extern uint32_t g_ra_ls_ns_load_start; /**< LMA start of NS image (MRAM).   */
extern uint32_t g_ra_ls_ns_load_end;   /**< LMA end of NS image (MRAM).     */
extern uint32_t g_ra_ls_ns_run_start;  /**< VMA start of NS image (SRAM NS).*/
/* Bounds of the NSC veneer stubs (.gnu.sgstubs) in Secure MRAM. */
extern uint32_t g_ra_ls_sgstubs_start; /**< Veneer-region start (NSC).      */
extern uint32_t g_ra_ls_sgstubs_end;   /**< Veneer-region end (NSC).        */

#ifdef RA_TRUSTZONE_ENABLE

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
  k_tz_sau_ctrl_addr   = 0xE000EDD0U, /**< SAU Control Register.            */
  k_tz_sau_type_addr   = 0xE000EDD4U, /**< SAU Type (implemented regions).  */
  k_tz_sau_rnr_addr    = 0xE000EDD8U, /**< SAU Region Number.               */
  k_tz_sau_rbar_addr   = 0xE000EDDCU, /**< SAU Region Base Address.         */
  k_tz_sau_rlar_addr   = 0xE000EDE0U, /**< SAU Region Limit Address.        */
  k_tz_sramsabar0_addr = 0x40008400U, /**< CPSCU SRAMSABAR0 (+4*n for n>0). */
  k_tz_prcr_s_addr     = 0x4001E3FAU, /**< SYSC PRCR_S (16-bit).            */
} tz_reg_addr_t;

/**
 * @enum tz_field_t
 * @brief Bit fields / magic values for the SAU and PRCR_S writes.
 *
 * @invariant ``k_tz_sau_rlar_*`` occupy bits [1:0]; the limit address
 *            occupies bits [31:5] (ARMv8-M 32-byte region quantum).
 */
typedef enum : uint32_t {
  k_tz_sau_ctrl_enable = 0x00000001U, /**< SAU_CTRL.ENABLE, ALLNS = 0.      */
  k_tz_sau_rlar_enable = 0x00000001U, /**< SAU_RLAR.ENABLE.                 */
  k_tz_sau_rlar_nsc    = 0x00000002U, /**< SAU_RLAR.NSC (Non-secure call).  */
  k_tz_sau_limit_mask  = 0xFFFFFFE0U, /**< 32-byte-aligned limit mask.      */
  k_tz_sau_type_mask   = 0x000000FFU, /**< SAU_TYPE.SREGION field mask.     */
} tz_field_t;

/**
 * @enum tz_prcr_t
 * @brief PRCR_S unlock/lock key values for the PRC4 (CPSCU) write gate.
 *
 * @invariant The top byte is the 0xA5 key; bit 4 is PRC4.
 */
typedef enum : uint16_t {
  k_tz_prcr_s_open  = 0xA510U, /**< Key | PRC4 set (unlock CPSCU writes).   */
  k_tz_prcr_s_close = 0xA500U, /**< Key | PRC4 clear (re-lock).             */
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
  k_tz_region_nsc     = 0U, /**< NSC veneers (.gnu.sgstubs in MRAM).       */
  k_tz_region_ns_code = 1U, /**< 0x1000_0000-0x1FFF_FFFF NS.              */
  k_tz_region_ns_sram = 2U, /**< 0x3000_0000-0x3FFF_FFFF NS (NS image).   */
  k_tz_region_ns_per  = 3U, /**< 0x5000_0000-0xDFFF_FFFF NS.              */
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
 * @return ra_err_t Error code.
 * @retval k_ra_ok                SAU programmed and enabled.
 * @retval k_ra_err_not_supported SAU_TYPE.SREGION < 4.
 *
 * @pre Caller is in Secure state with the SAU disabled.
 * @pre The veneer linker symbols bound a non-empty 32-byte-aligned range.
 * @post On success SAU_CTRL.ENABLE = 1 with the four regions above.
 * @post On failure the SAU stays disabled (default-allow Secure).
 * @note Not thread-safe; secure-boot only.
 * @since 0.1.0
 */
static ra_err_t tz_sau_program(void)
{
  /* HUM Ch 51.3.3.3 "Secure Attribution Unit (SAU)" p 3266 -- need at
   * least 4 implemented regions for this layout. */
  const uint32_t sau_type = tz_read32(k_tz_sau_type_addr);
  if ((sau_type & (uint32_t)k_tz_sau_type_mask) < (uint32_t)k_tz_sau_min_regions) {
    return k_ra_err_not_supported;
  }

  /* RLAR limit is the LAST byte of the veneer block (end - 1); the helper
   * masks it to the 32-byte SAU region quantum. */
  const uint32_t nsc_base  = (uint32_t)(uintptr_t)&g_ra_ls_sgstubs_start;
  const uint32_t nsc_limit = (uint32_t)(uintptr_t)&g_ra_ls_sgstubs_end - 1U;
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
  return k_ra_ok;
}

/**
 * @brief Copy the NS image from its MRAM LMA into the SRAM NS alias.
 *
 * @details Plain word copy of [g_ra_ls_ns_load_start, g_ra_ls_ns_load_end)
 *          to g_ra_ls_ns_run_start (0x3210_0000). Runs AFTER the SAU and
 *          SRAMSABAR have marked the destination Non-secure, so the store
 *          is a (permitted) Secure-master Non-secure access.
 *
 * @pre ``tz_sram_ns_boundary`` and ``tz_sau_program`` have run.
 * @pre The load and run ranges are word-aligned and equal in length.
 * @post The NS vector table + text + rodata are live at 0x3210_0000.
 * @post The source MRAM image is unchanged.
 * @note Not thread-safe; secure-boot only.
 * @since 0.1.0
 */
static void tz_copy_ns_image(void)
{
  const uintptr_t src_start = (uintptr_t)&g_ra_ls_ns_load_start;
  const uintptr_t src_end   = (uintptr_t)&g_ra_ls_ns_load_end;
  uintptr_t       dst       = (uintptr_t)&g_ra_ls_ns_run_start;
  for (uintptr_t src = src_start; src < src_end; src += sizeof(uint32_t)) {
    *(volatile uint32_t*)dst = *(const volatile uint32_t*)src;
    dst += sizeof(uint32_t);
  }
}

#endif /* RA_TRUSTZONE_ENABLE */

void ra_trustzone_init(void)
{
#ifdef RA_TRUSTZONE_ENABLE
  /* 1. Carve the SRAM2 NS aperture via the runtime SRAMSABAR boundary. */
  tz_sram_ns_boundary();

  /* 2. Programme the bit[28] SAU (NSC veneers + IDAU-NS ranges), enable. */
  if (tz_sau_program() != k_ra_ok) {
    return; /* Fall through to the S-side main() fallback. */
  }

  /* 3. Copy the NS image into the now-Non-secure SRAM alias. */
  tz_copy_ns_image();

  /* 4. BLXNS into the NS reset vector (slot 1 of g_ra_ns_vector_table at
   *    VMA 0x3210_0000). Does not return on hardware. */
  (void)ra_tz_secure_boot_jump_ns(g_ra_ns_vector_table);

  /* On host (RA_SIMULATOR_MODE) the library stubs BLXNS and returns; on
   * target this is unreachable. */
#endif
}
