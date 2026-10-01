/**
 * @file examples/ek_ra8d2/hw_pending/secure_boot_ns_hil/src/trustzone_init.c
 * @brief Single-core TrustZone bring-up for a RAM-resident NS image.
 *
 * @par Tag
 * [Ring 1 / Boot] {World: S}
 *
 * @details
 * The RA8 IDAU is FIXED by address bit[28] (HUM section 51.3.3.1, p3265):
 * bit[28]=0 is Secure/NSC and the SAU cannot downgrade it, so an "NS" image at
 * the 0x02.. / 0x22.. (bit[28]=0) aliases always executes Secure. Real NS lives
 * at the bit[28]=1 aliases (0x12.. code, 0x32.. SRAM, 0x5.. peripherals).
 *
 * Code MRAM's secure/NS split needs persistent (brick-risky) option bytes, but
 * SRAM's split is the RUNTIME ``SRAMSABARn`` register, so the NS image is
 * RAM-resident: flashed into Secure MRAM (the LMA) and copied by this code into
 * the SRAM Non-secure alias 0x3210_0000 (physical SRAM2) after ``SRAMSABAR2``
 * marks SRAM2 Non-secure. No option bytes, no brick.
 *
 * Boot sequence (this file, all in Secure state, called from SystemInit):
 *   1. Open ``PRCR_S.PRC4`` and program ``SRAMSABAR0..3`` so physical SRAM
 *      [0x10_0000, 0x18_0000) (SRAM2) is Non-secure; lower SRAM (the Secure
 *      stack + heap) stays Secure. Re-lock PRC4.
 *   2. Programme the SAU to the bit[28] model (HUM p3267): mark the IDAU-NS
 *      ranges 0x1000_0000-, 0x3000_0000-, 0x5000_0000- as NS. Enable the SAU
 *      with ALLNS = 0 (default-deny). No NSC region: this NS image makes no
 *      NS->Secure calls (no veneers).
 *   3. Copy the NS image from its MRAM LMA to the SRAM NS alias.
 *
 * It deliberately does NOT BLXNS: ``main()`` runs the root-of-trust verify (which
 * needs the crypto heap the C runtime sets up) and then jumps. On a host build
 * (``RA8_OFF_TARGET`` or no ``RA8_TRUSTZONE_ENABLE``) this is a no-op.
 *
 * @par TrustZone Safety:
 *  - **Validates:** SAU_TYPE.SREGION >= 3 before programming.
 *  - **Trusts:** the boot ROM left the SAU disabled and the IDAU in its
 *    documented reset state (fixed bit[28] split).
 *  - **Denies:** ``main()`` denies the BLXNS on a failed root-of-trust verify.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "trustzone_init.h"

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_sau.h"
#include "ra8_tz_partition.h"

#ifdef RA8_TRUSTZONE_ENABLE

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
 * @enum tz_region_t
 * @brief Region indices for the bit[28] SAU layout (no NSC region).
 */
typedef enum : uint8_t {
  k_tz_region_ns_code = 0U, /**< 0x1000_0000-0x1FFF_FFFF NS.            */
  k_tz_region_ns_sram = 1U, /**< 0x3000_0000-0x3FFF_FFFF NS (NS image). */
  k_tz_region_ns_per  = 2U, /**< 0x5000_0000-0xDFFF_FFFF NS.            */
} tz_region_t;

/**
 * @brief Apply this app's security attribution through the shared descriptor.
 *
 * @details Builds the bit[28] SAU layout (three IDAU-NS ranges, no NSC region:
 *          this app has no NS->Secure veneers) plus the SRAMSABAR0..3 boundary
 *          set, and hands both to ``ra8_tz_partition_apply``, which validates
 *          the geometry, programmes the SAU through ``ra8_sau_configure`` and
 *          then writes the four boundaries inside its own PRC4 window. The
 *          SAU_TYPE.SREGION check this file used to make by hand is the
 *          descriptor validation's ``k_ra8_err_not_supported``.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                SAU enabled and the four boundaries written.
 * @retval k_ra8_err_not_supported The SAU implements fewer regions than the
 *                                 layout needs.
 * @retval k_ra8_err_invalid_arg   Descriptor geometry rejected.
 *
 * @pre Caller is in Secure state with the SAU disabled.
 * @pre Do not wrap this call in ``RA8_PROTECTED_WRITE``: the SRAM half opens
 *      and closes its own PRC4 scope, and nesting would re-lock it early.
 * @post On success SAU_CTRL.ENABLE = 1 (ALLNS = 0) and SRAM2
 *       [0x10_0000, 0x18_0000) is Non-secure (alias 0x3210_0000).
 * @post On a validation failure no register is written.
 * @note Not thread-safe; secure-boot only.
 * @since 0.1.0
 */
static ra8_err_t tz_partition_apply(void)
{
  /* HUM Ch 51.3.3.3 "Secure Attribution Unit (SAU)" p 3266 -- the three
   * IDAU-NS ranges HUM p3267 mandates be Non-secure. Everything else stays
   * Secure because ALLNS is left clear. */
  static const ra8_sau_region_t k_regions[] = {
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
   * Register" p 3527 -- boundary = start of the NS region; below = Secure,
   * at/above = Non-secure. Reset-cleared, so no option-byte brick exposure. */
  static const uint32_t k_sram_boundary[k_ra8_tz_partition_sram_bank_count] = {
    (uint32_t)k_tz_sramsabar0_val,
    (uint32_t)k_tz_sramsabar1_val,
    (uint32_t)k_tz_sramsabar2_val,
    (uint32_t)k_tz_sramsabar3_val,
  };

  const ra8_tz_partition_t partition = {
    .sau_regions      = k_regions,
    .sram_boundary    = k_sram_boundary,
    .sau_region_count = (uint8_t)(sizeof(k_regions) / sizeof(k_regions[0])),
    .sau_all_ns       = false,
  };
  return ra8_tz_partition_apply(&partition);
}

/**
 * @brief Copy the NS image (body + RoT trailer) from its MRAM LMA to SRAM.
 *
 * @details Plain word copy of the fixed ::k_sbns_ns_copy_size window from
 *          ::k_sbns_ns_load_base (MRAM) to ::k_sbns_ns_run_base (0x3210_0000).
 *          Runs AFTER the SAU + SRAMSABAR marked the destination Non-secure, so
 *          the store is a permitted Secure-side Non-secure access. The window is
 *          large enough to carry the signed body AND the appended
 *          ``ra8_rot_trailer_t`` (the verifier reads the trailer at the SRAM run
 *          base + body_len).
 *
 * @pre ``tz_partition_apply`` has run.
 * @pre The NS image (body + trailer) fits within ::k_sbns_ns_copy_size.
 * @post The NS vector table + RoT header + body + trailer are live at
 *       ::k_sbns_ns_run_base.
 * @post The source MRAM image is unchanged.
 * @note Not thread-safe; secure-boot only.
 * @since 0.1.0
 */
static void tz_copy_ns_image(void)
{
  const uintptr_t src_start = (uintptr_t)k_sbns_ns_load_base;
  const uintptr_t src_end   = src_start + (uintptr_t)k_sbns_ns_copy_size;
  uintptr_t       dst       = (uintptr_t)k_sbns_ns_run_base;
  for (uintptr_t src = src_start; src < src_end; src += sizeof(uint32_t)) {
    *(volatile uint32_t*)dst = *(const volatile uint32_t*)src;
    dst += sizeof(uint32_t);
  }
}

#endif /* RA8_TRUSTZONE_ENABLE */

void ra8_trustzone_init(void)
{
#ifdef RA8_TRUSTZONE_ENABLE
  /* 1. Programme the bit[28] SAU (3 IDAU-NS ranges, default-deny) and carve
   *    the SRAM2 NS aperture via the runtime SRAMSABAR boundary. */
  if (tz_partition_apply() != k_ra8_ok) {
    return; /* Leave the SAU disabled; main() will find no NS liveness. */
  }

  /* 2. Copy the NS image (body + RoT trailer) into the now-Non-secure SRAM
   *    alias. main() authenticates it there and BLXNS-es after crypto is up. */
  tz_copy_ns_image();
#endif
}
