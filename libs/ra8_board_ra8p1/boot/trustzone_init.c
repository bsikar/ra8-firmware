/**
 * @file libs/ra8_board_ra8p1/boot/trustzone_init.c
 * @brief Cortex-M85 TrustZone-M Security Attribution Unit (SAU) bring-up
 *
 * @par Tag
 * [Ring 1 / Boot] {World: S}
 *
 * @note RA8P1 board layer (issue #226): this chip-boot TU is byte-identical to
 *       the EK-RA8D2 copy. The RA8P1 (R7KA8P1KFLCAC) shares the RA8D2 Cortex-M85
 *       SAU and the IDAU bit-28 security split (see libs/ra8_core/inc/ra8_device.h),
 *       so the SAU bring-up is common to both boards.
 *
 * @details
 * scaffold for the secure / non-secure address-space split.
 * Programs the SAU with four canonical regions and enables it. Called
 * from ``SystemInit`` after the cache + MPU are up but before any
 * non-secure code can run.
 *
 * The function is gated behind the ``RA8_TRUSTZONE_ENABLE`` build
 * symbol so the single-world build (the ..8 default) does not
 * pay any code-size cost. When the symbol is undefined,
 * ``ra8_trustzone_init`` is an empty inline.
 *
 * ## Partition layout (scaffold)
 *
 * The RA8D2 IDAU defines bit 28 of the address as the security
 * attribute by default (S = bit 28 clear, NS = bit 28 set). The
 * SAU overlays additional rules. The partition is:
 *
 * | Region | Range | Attribute |
 * |-------:|:----------------------------|:--------------------|
 * | 0 | 0x02080000..0x020FFFFF | NS (upper MRAM) |
 * | 1 | 0x22100000..0x221FFFFF | NS (upper SRAM) |
 * | 2 | 0x6A000000..0x6BFFFFFF | NS (upper SDRAM) |
 * | 3 | 0x10000000..0x100FFFFF | NSC veneer alias |
 *
 * - Lower MRAM (0x02000000..0x0207FFFF) stays secure -- holds the
 * secure world image.
 * - Lower SRAM (0x22000000..0x220FFFFF) stays secure -- holds the
 * secure-world data + key vault.
 * - Upper MRAM / SRAM / SDRAM are exposed to the NS world for
 * the application.
 * - The NSC veneer page lives in a 1 MB alias the linker maps via
 * the ``.gnu.sgstubs`` section.
 *
 * These addresses are illustrative -- the actual partition lands
 * once the linker script grows the matching memory
 * regions and the veneer section is wired up.
 *
 * @par TrustZone Safety:
 * - **Validates:** SAU_TYPE.SREGION reports >= 4 regions before
 * programming any of them (chip family safety check).
 * - **Trusts:** the Boot ROM left the SAU disabled and the IDAU
 * in its reset state.
 * - **Denies:** any access from NS code to the registers programmed
 * here -- the entire SAU register window lives in the secure
 * region by definition.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "trustzone_init.h"

#include <stdint.h>

#include "ra8_boot_intrinsics.h"

#ifdef RA8_TRUSTZONE_ENABLE

/* =============================================================================
 * SAU register addresses (System Control Space, secure alias)
 * =============================================================================
 */

/** @brief SAU_TYPE SREGION-count mask (low byte). */
typedef enum : uint32_t {
  k_sau_sregion_mask = 0xFFU, /**< SAU sregion mask. */
} sau_type_mask_t;

typedef enum : uintptr_t {
  k_ra8_sau_ctrl_addr = 0xE000EDD0UL, /**< SAU_CTRL Control Register.    */
  k_ra8_sau_type_addr = 0xE000EDD4UL, /**< SAU_TYPE Type Register.       */
  k_ra8_sau_rnr_addr  = 0xE000EDD8UL, /**< SAU_RNR Region Number.        */
  k_ra8_sau_rbar_addr = 0xE000EDDCUL, /**< SAU_RBAR Region Base Address. */
  k_ra8_sau_rlar_addr = 0xE000EDE0UL, /**< SAU_RLAR Region Limit + bits. */
  k_ra8_sfsr_addr     = 0xE000EDE4UL, /**< SecureFault Status Register.  */
} ra8_tz_sau_addr_t;

/**
 * @enum ra8_tz_sau_ctrl_bit_t
 * @brief SAU_CTRL bit positions.
 */
typedef enum : uint32_t {
  k_ra8_sau_ctrl_enable = 1UL << 0, /**< ENABLE: main SAU enable.        */
  k_ra8_sau_ctrl_allns  = 1UL << 1, /**< ALLNS: default-NS unprogrammed. */
} ra8_tz_sau_ctrl_bit_t;

/**
 * @enum ra8_tz_sau_rlar_bit_t
 * @brief SAU_RLAR bit positions.
 */
typedef enum : uint32_t {
  k_ra8_sau_rlar_enable = 1UL << 0, /**< ENABLE: region active.              */
  k_ra8_sau_rlar_nsc    = 1UL << 1, /**< NSC: region is Non-Secure Callable. */
} ra8_tz_sau_rlar_bit_t;

/**
 * @enum ra8_tz_partition_t
 * @brief canonical region addresses.
 *
 * @details
 * SAU regions are 32-byte aligned per ARMv8-M; RLAR holds the
 * upper bound minus 32 OR-ed with the enable bits at write time.
 */
typedef enum : uint32_t {
  k_ra8_tz_ns_mram_base    = 0x02080000UL, /**< RA8 TrustZone ns MRAM base.    */
  k_ra8_tz_ns_mram_limit   = 0x020FFFE0UL, /**< RA8 TrustZone ns MRAM limit.   */
  k_ra8_tz_ns_sram_base    = 0x22100000UL, /**< RA8 TrustZone ns SRAM base.    */
  k_ra8_tz_ns_sram_limit   = 0x221FFFE0UL, /**< RA8 TrustZone ns SRAM limit.   */
  k_ra8_tz_ns_sdram_base   = 0x6A000000UL, /**< RA8 TrustZone ns SDRAM base.   */
  k_ra8_tz_ns_sdram_limit  = 0x6BFFFFE0UL, /**< RA8 TrustZone ns SDRAM limit.  */
  k_ra8_tz_nsc_veneer_base = 0x10000000UL, /**< RA8 TrustZone NSC veneer base. */
  k_ra8_tz_nsc_veneer_lim  = 0x100FFFE0UL, /**< RA8 TrustZone NSC veneer lim.  */
} ra8_tz_partition_t;

/* =============================================================================
 * Internal helpers
 * =============================================================================
 */

/**
 * @brief Programme one SAU region via RNR/RBAR/RLAR.
 *
 * @details Selects the region (RNR), sets its base (RBAR) and limit
 *   (RLAR) with the enable bit, and marks it NSC when requested.
 * @param[in] region Region number 0..(SAU_TYPE.SREGION - 1).
 * @param[in] base Start address (32-byte aligned).
 * @param[in] limit Upper bound minus 32 (32-byte aligned).
 * @param[in] is_nsc ``true`` to mark the region as NSC.
 * @pre The SAU is disabled or being configured during boot.
 * @pre @p region is within the implemented SAU region count.
 * @post The region's RBAR/RLAR are programmed and enabled.
 * @post RLAR.NSC reflects @p is_nsc.
 * @note Not thread-safe; single-threaded secure boot only.
 * @since 0.1.0
 */
static void internal_sau_set_region(uint32_t region, uint32_t base, uint32_t limit, bool is_nsc)
{
  ra8_boot_write32(k_ra8_sau_rnr_addr, region);
  ra8_boot_write32(k_ra8_sau_rbar_addr, base);
  uint32_t rlar = limit | (uint32_t)k_ra8_sau_rlar_enable;
  if (is_nsc) {
    rlar |= (uint32_t)k_ra8_sau_rlar_nsc;
  }
  ra8_boot_write32(k_ra8_sau_rlar_addr, rlar);
}

/* =============================================================================
 * Public entry point
 * =============================================================================
 */

#endif /* RA8_TRUSTZONE_ENABLE */

void ra8_trustzone_init(void)
{
#ifdef RA8_TRUSTZONE_ENABLE
  /* Sanity check: SAU_TYPE.SREGION must report >= 4 implemented
   * regions for our partition to fit. The Cortex-M85 always has 8,
   * but a chip-specific override could trim the count. */
  const uint32_t sau_type = ra8_boot_read32(k_ra8_sau_type_addr);
  if ((sau_type & k_sau_sregion_mask) < 4U) {
    /* Refuse to bring up TrustZone on an SAU we cannot use. The
     * caller will see SAU_CTRL.ENABLE clear and fall back to the
     * single-world model. */
    return;
  }

  /* Region 0: NS upper MRAM */
  internal_sau_set_region(0U,
                          (uint32_t)k_ra8_tz_ns_mram_base,
                          (uint32_t)k_ra8_tz_ns_mram_limit,
                          /*is_nsc=*/false);

  /* Region 1: NS upper SRAM */
  internal_sau_set_region(1U,
                          (uint32_t)k_ra8_tz_ns_sram_base,
                          (uint32_t)k_ra8_tz_ns_sram_limit,
                          /*is_nsc=*/false);

  /* Region 2: NS upper SDRAM */
  internal_sau_set_region(2U,
                          (uint32_t)k_ra8_tz_ns_sdram_base,
                          (uint32_t)k_ra8_tz_ns_sdram_limit,
                          /*is_nsc=*/false);

  /* Region 3: NSC veneer alias ( will place .gnu.sgstubs
   * here via the linker script). */
  internal_sau_set_region(3U,
                          (uint32_t)k_ra8_tz_nsc_veneer_base,
                          (uint32_t)k_ra8_tz_nsc_veneer_lim,
                          /*is_nsc=*/true);

  /* Enable the SAU. Leave ALLNS clear: anything we have not
   * explicitly carved out stays secure (default-deny). */
  ra8_boot_dsb();
  ra8_boot_write32(k_ra8_sau_ctrl_addr, (uint32_t)k_ra8_sau_ctrl_enable);
  ra8_boot_dsb();
  ra8_boot_isb();
#endif
}
