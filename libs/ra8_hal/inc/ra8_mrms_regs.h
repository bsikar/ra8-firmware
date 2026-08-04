/**
 * @file ra8_mrms_regs.h
 * @brief MRAM Memory System (MRMS) wait-state register layout for the RA8D2
 * @ingroup grp_hal_system
 *
 * @details
 * The Renesas RA8 Gen2 family does not expose the legacy MEMWAIT /
 * FLDWAITR / FLWT registers (`bsp_feature.h` set
 * `BSP_FEATURE_CGC_HAS_MEMWAIT = 0`, `HAS_FLDWAITR = 0`,
 * `HAS_FLWT = 0`). Wait states for the on-chip MRAM are programmed
 * through a dedicated **MRMS** (MRAM Memory System) register block at
 * `0x4013C000`.
 *
 * The CGC bring-up sequence (HUM Ch 54.4.3 "Frequency Change Procedure
 * for MRAM" / FSP `bsp_clocks.c`) requires:
 *
 * 1. Clear the prefetch buffer (write `MRCPFB = 0`, then 3 dummy reads)
 *    *before* changing any clock source or divider.
 * 2. Programme `MRCFREQ` with the new MRICLK frequency in MHz, with the
 *    key byte `0x1E` in the upper byte. The hardware refuses any write
 *    whose key byte is not `0x1E`, so the write is wrapped in a
 *    poll-until-readback loop.
 * 3. Programme `MREFREQ` with the new MRPCLK frequency in MHz, with key
 *    byte `0xE1` (note: not the same key as MRCFREQ).
 * 4. After the new clock tree is selected, re-enable the prefetch
 *    buffer (`MRCPFB = 1`) iff the new MRICLK is greater than or equal
 *    to 100 MHz.
 *
 * ## Register map
 *
 * | Offset | Reg     | Width | Notes                                       |
 * |-------:|---------|------:|---------------------------------------------|
 * | 0x000  | MRCPFB  | 32    | Prefetch buffer enable (0 or 1).            |
 * | 0x004  | MRCFREQ | 32    | MRICLK frequency in MHz, upper byte = 0x1E. |
 * | 0x008  | MREFREQ | 32    | MRPCLK frequency in MHz, upper byte = 0xE1. |
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* =============================================================================
 * Base address
 * =============================================================================
 */

/**
 * @enum ra8_mrms_addr_t
 * @brief Base address for the MRMS register block.
 */
typedef enum : uintptr_t {
  k_ra8_mrms_base_addr = 0x4013C000UL, /**< R_MRMS base. */
} ra8_mrms_addr_t;

/* =============================================================================
 * Register offsets within the MRMS block
 * =============================================================================
 */

/**
 * @enum ra8_mrms_offset_t
 * @brief Per-register offsets relative to ::k_ra8_mrms_base_addr.
 */
typedef enum : uint16_t {
  k_ra8_mrms_off_mrcpfb  = 0x000U, /**< MRCPFB  prefetch buffer enable.            */
  k_ra8_mrms_off_mrcfreq = 0x004U, /**< MRCFREQ MRICLK frequency latch (key 0x1E). */
  k_ra8_mrms_off_mrefreq = 0x008U, /**< MREFREQ MRPCLK frequency latch (key 0xE1). */
} ra8_mrms_offset_t;

/* =============================================================================
 * Field encodings
 * =============================================================================
 */

/**
 * @enum ra8_mrms_key_t
 * @brief Mandatory key bytes that must occupy the upper byte of MRCFREQ /
 *        MREFREQ for hardware to accept the write.
 *
 * @details
 * Cited from FSP `bsp_clocks.c` (`BSP_PRV_MRCFREQ_KEY` /
 * `BSP_PRV_MREFREQ_KEY`). MRCFREQ uses 0x1E in the upper byte; MREFREQ
 * uses 0xE1. Writes whose key byte is anything else are silently
 * dropped (the register continues to read back its previous value).
 */
typedef enum : uint32_t {
  k_ra8_mrcfreq_key = 0x1E000000UL, /**< MRCFREQ unlock key, upper byte = 0x1E. */
  k_ra8_mrefreq_key = 0xE1000000UL, /**< MREFREQ unlock key, upper byte = 0xE1. */
} ra8_mrms_key_t;

/**
 * @enum ra8_mrms_pfb_t
 * @brief Values written to MRCPFB to enable / disable the prefetch buffer.
 *
 * @details
 * The prefetch buffer must be **disabled** before any frequency change
 * (HUM Ch 54.4.3) and **re-enabled** only after the new MRICLK has
 * settled and only if MRICLK >= 100 MHz.
 */
typedef enum : uint32_t {
  k_ra8_mrcpfb_disable = 0x00000000UL, /**< Prefetch buffer off. */
  k_ra8_mrcpfb_enable  = 0x00000001UL, /**< Prefetch buffer on.  */
} ra8_mrms_pfb_t;

/**
 * @enum ra8_mrms_pfb_threshold_t
 * @brief MRICLK frequency threshold below which the prefetch buffer must
 *        remain disabled.
 *
 * @details
 * FSP `bsp_clocks.c` (`BSP_PRV_MRCPFB_LIMIT == 100`): only enable
 * the prefetch buffer when MRCFREQ reads back a value >= 100 MHz.
 */
typedef enum : uint32_t {
  k_ra8_mrcpfb_threshold_mhz = 100UL, /**< Minimum MRICLK MHz to enable PFB. */
} ra8_mrms_pfb_threshold_t;

/* =============================================================================
 * Inline accessors
 * =============================================================================
 */

/**
 * @brief Get pointer to the 32-bit MRCPFB register.
 *
 * @return Volatile pointer to MRCPFB at `0x4013C000`.
 *
 * @pre   MRMS clock domain is active (always-on out of reset).
 * @post  Caller may read or write the register.
 *
 * @note Not thread-safe. Caller must serialise with any other CGC
 *       bring-up code touching MRMS.
 *
 * @since 0.1.0
 */
static inline volatile uint32_t* ra8_mrms_mrcpfb(void)
{
  return (volatile uint32_t*)((uintptr_t)k_ra8_mrms_base_addr + (uintptr_t)k_ra8_mrms_off_mrcpfb);
}

/**
 * @brief Get pointer to the 32-bit MRCFREQ register (MRICLK frequency).
 *
 * @return Volatile pointer to MRCFREQ at `0x4013C004`.
 *
 * @pre  MRMS clock domain is active.
 * @post Caller may read or write the register; writes must include the
 *       `0x1E` key in the upper byte.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
static inline volatile uint32_t* ra8_mrms_mrcfreq(void)
{
  return (volatile uint32_t*)((uintptr_t)k_ra8_mrms_base_addr + (uintptr_t)k_ra8_mrms_off_mrcfreq);
}

/**
 * @brief Get pointer to the 32-bit MREFREQ register (MRPCLK frequency).
 *
 * @return Volatile pointer to MREFREQ at `0x4013C008`.
 *
 * @pre  MRMS clock domain is active.
 * @post Caller may read or write the register; writes must include the
 *       `0xE1` key in the upper byte.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
static inline volatile uint32_t* ra8_mrms_mrefreq(void)
{
  return (volatile uint32_t*)((uintptr_t)k_ra8_mrms_base_addr + (uintptr_t)k_ra8_mrms_off_mrefreq);
}

#ifdef __cplusplus
}
#endif
