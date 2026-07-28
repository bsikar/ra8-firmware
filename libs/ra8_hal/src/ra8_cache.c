/**
 * @file ra8_cache.c
 * @brief Cortex-M85 L1 D-cache maintenance implementation.
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Implements ::ra8_cache.h against the Arm v8-M System Control Block cache
 * registers (PPB window 0xE000Exxx). The by-address operations walk the cache
 * lines spanning a byte range, writing each line's address to the relevant
 * maintenance register (DCCMVAC / DCIMVAC / DCCIMVAC) with the line size read at
 * run time from CTR. ::ra8_cache_dcache_invalidate_all walks the geometry from
 * CCSIDR and invalidates every set/way via DCISW, mirroring the CMSIS
 * `SCB_InvalidateDCache` idiom.
 *
 * These are Arm-architecture registers, so the inline comments reference the
 * Arm v8-M Architecture Reference Manual (the "Arm v8-M ARM") rather than the
 * RA8D2 Hardware User's Manual. On a host build the SCB window is backed by the
 * fake MMIO map, so the writes are observable to unit tests.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "ra8_cache.h"

#include <stdint.h>

#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_hw_intrinsics.h"

/** @brief Module log tag. */
static const char* const s_tag = "ra8_cache";

/**
 * @enum ra8_cache_reg_addr_t
 * @brief Arm v8-M SCB cache-maintenance register addresses (PPB 0xE000Exxx).
 * @details Arm v8-M ARM B11 "System Control Block". Mapped into the fake
 *          core window (0xE0000000) on a host build, real PPB on silicon.
 */
typedef enum : uintptr_t {
  k_ra8_cache_ctr      = 0xE000ED7CUL, /**< Cache Type Register (CTR).       */
  k_ra8_cache_ccsidr   = 0xE000ED80UL, /**< Cache Size ID Register (CCSIDR). */
  k_ra8_cache_csselr   = 0xE000ED84UL, /**< Cache Size Selection (CSSELR).   */
  k_ra8_cache_dcimvac  = 0xE000EF5CUL, /**< D-cache invalidate by MVA (PoC). */
  k_ra8_cache_dcisw    = 0xE000EF60UL, /**< D-cache invalidate by set/way.   */
  k_ra8_cache_dccmvac  = 0xE000EF68UL, /**< D-cache clean by MVA (PoC).      */
  k_ra8_cache_dccimvac = 0xE000EF70UL, /**< D-cache clean+invalidate by MVA. */
} ra8_cache_reg_addr_t;

/**
 * @enum ra8_cache_field_t
 * @brief Bit positions, masks, and sizes for the CTR / CCSIDR / DCISW fields.
 * @details Arm v8-M ARM register descriptions for CTR, CCSIDR, and DCISW.
 */
typedef enum : uint32_t {
  k_ra8_cache_word_bytes      = 4U,      /**< Bytes per 32-bit cache word.        */
  k_ra8_cache_ctr_dmin_shift  = 16U,     /**< CTR.DminLine bit position.          */
  k_ra8_cache_ctr_dmin_mask   = 0xFU,    /**< CTR.DminLine field mask (4 bits).   */
  k_ra8_cache_ccsidr_sets_sh  = 13U,     /**< CCSIDR.NumSets bit position.        */
  k_ra8_cache_ccsidr_sets_msk = 0x7FFFU, /**< CCSIDR.NumSets field mask (15 bit). */
  k_ra8_cache_ccsidr_assoc_sh = 3U,      /**< CCSIDR.Associativity bit position.  */
  k_ra8_cache_ccsidr_assoc_mk = 0x3FFU,  /**< CCSIDR.Associativity mask (10 bit). */
  k_ra8_cache_dcisw_set_shift = 5U,      /**< DCISW.SET position (32-byte line).  */
  k_ra8_cache_dcisw_way_shift = 30U,     /**< DCISW.WAY position (assoc <= 4).    */
} ra8_cache_field_t;

/**
 * @brief Typed pointer to a 32-bit SCB cache register.
 * @param[in] addr One of ::ra8_cache_reg_addr_t.
 * @return Volatile pointer for a single read or write.
 * @pre @p addr is a valid SCB cache-register address.
 * @pre The PPB / fake core window is accessible.
 * @post No state changed by forming the pointer.
 * @post The returned pointer aliases the live register.
 * @note Arm v8-M ARM B11 "System Control Block".
 * @since 0.1.0
 */
static inline volatile uint32_t* ra8_cache_reg(ra8_cache_reg_addr_t addr)
{
  return (volatile uint32_t*)addr;
}

uint32_t ra8_cache_dcache_line_bytes(void)
{
  /* Arm v8-M ARM: CTR.DminLine = log2 of the smallest D-cache line in words. */
  const uint32_t ctr = *ra8_cache_reg(k_ra8_cache_ctr);
  const uint32_t dmin =
    (ctr >> (uint32_t)k_ra8_cache_ctr_dmin_shift) & (uint32_t)k_ra8_cache_ctr_dmin_mask;
  return (uint32_t)k_ra8_cache_word_bytes << dmin;
}

/**
 * @brief Number of cache lines spanning @p size bytes from @p addr.
 *
 * @details Rounds @p addr down to its line boundary and counts the lines up to
 *          and including the one holding the last byte. Pure address arithmetic
 *          so the by-address operations share one tested span calculation.
 *
 * @param[in]  addr      Range start.
 * @param[in]  size      Range length in bytes (must be non-zero).
 * @param[in]  line      Cache line size in bytes (a power of two, >= 4).
 * @param[out] out_start Receives the line-aligned start address.
 *
 * @return Number of lines (>= 1).
 * @retval count Line count covering `[addr, addr + size)`; always >= 1 since a
 *               non-zero range touches at least one line.
 *
 * @pre @p size is non-zero and @p line is a non-zero power of two.
 * @pre @p out_start is non-NULL.
 * @post @p *out_start <= @p addr and is line-aligned.
 * @post The returned count covers `[addr, addr + size)`.
 * @note Pure function; trivially thread-safe.
 * @since 0.1.0
 */
static uint32_t
ra8_cache_span_lines(uintptr_t addr, uint32_t size, uint32_t line, uintptr_t* out_start)
{
  const uintptr_t mask  = (uintptr_t)line - 1U;
  const uintptr_t start = addr & ~mask;
  const uintptr_t last  = (addr + (uintptr_t)size - 1U) & ~mask;
  *out_start            = start;
  return (uint32_t)((last - start) / (uintptr_t)line) + 1U;
}

/**
 * @brief Apply a by-MVA maintenance op to every line of a range.
 *
 * @details Shared body of the three public by-address operations: validate the
 *          range, compute its line span, and write each line address to @p reg,
 *          bracketed by data barriers.
 *
 * @param[in] addr Range start (must be non-NULL).
 * @param[in] size Range length in bytes (0 is a success no-op).
 * @param[in] reg  Maintenance register (DCCMVAC / DCIMVAC / DCCIMVAC).
 *
 * @return Error code.
 * @retval k_ra8_ok           Range maintained (or `size == 0`).
 * @retval k_ra8_err_null_ptr `addr` was NULL.
 *
 * @pre @p addr is non-NULL unless @p size is zero.
 * @pre @p reg is a by-MVA maintenance register address.
 * @post Each line overlapping the range has been written to @p reg.
 * @post Barriers bracket the maintenance writes.
 * @note Not interrupt-safe against concurrent maintenance on the same range.
 * @since 0.1.0
 */
static ra8_err_t ra8_cache_maintain_range(const void* addr, uint32_t size, ra8_cache_reg_addr_t reg)
{
  RA8_CHECK_NULL_PTR(addr, s_tag, "maintain: addr");
  if (size == 0U) {
    return k_ra8_ok;
  }
  const uint32_t line  = ra8_cache_dcache_line_bytes();
  uintptr_t      start = 0U;
  const uint32_t lines = ra8_cache_span_lines((uintptr_t)addr, size, line, &start);
  ra8_hw_dsb();
  for (uint32_t i = 0U; i < lines; ++i) {
    *ra8_cache_reg(reg) = (uint32_t)(start + ((uintptr_t)i * (uintptr_t)line));
  }
  ra8_hw_dsb();
  ra8_hw_isb();
  return k_ra8_ok;
}

ra8_err_t ra8_cache_dcache_clean_by_addr(const void* addr, uint32_t size)
{
  return ra8_cache_maintain_range(addr, size, k_ra8_cache_dccmvac);
}

ra8_err_t ra8_cache_dcache_invalidate_by_addr(void* addr, uint32_t size)
{
  return ra8_cache_maintain_range(addr, size, k_ra8_cache_dcimvac);
}

ra8_err_t ra8_cache_dcache_clean_invalidate_by_addr(const void* addr, uint32_t size)
{
  return ra8_cache_maintain_range(addr, size, k_ra8_cache_dccimvac);
}

void ra8_cache_dcache_invalidate_all(void)
{
  /* Arm v8-M ARM: select L1 data cache, read its geometry, then invalidate
   * every set/way. CSSELR=0 selects level 0, data. */
  *ra8_cache_reg(k_ra8_cache_csselr) = 0U;
  ra8_hw_dsb();
  const uint32_t ccsidr = *ra8_cache_reg(k_ra8_cache_ccsidr);
  if ((ccsidr == 0U) || (ccsidr == UINT32_MAX)) {
    return; /* geometry unavailable -- nothing safe to invalidate */
  }
  uint32_t sets =
    (ccsidr >> (uint32_t)k_ra8_cache_ccsidr_sets_sh) & (uint32_t)k_ra8_cache_ccsidr_sets_msk;
  do {
    uint32_t ways =
      (ccsidr >> (uint32_t)k_ra8_cache_ccsidr_assoc_sh) & (uint32_t)k_ra8_cache_ccsidr_assoc_mk;
    do {
      *ra8_cache_reg(k_ra8_cache_dcisw) = (sets << (uint32_t)k_ra8_cache_dcisw_set_shift) |
                                          (ways << (uint32_t)k_ra8_cache_dcisw_way_shift);
    } while (ways-- != 0U);
  } while (sets-- != 0U);
  ra8_hw_dsb();
  ra8_hw_isb();
}
