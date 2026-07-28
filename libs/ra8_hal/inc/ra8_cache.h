/**
 * @file ra8_cache.h
 * @brief Cortex-M85 L1 D-cache maintenance (clean / invalidate by address).
 * @ingroup grp_hal_system
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Provides the cache-maintenance primitives that the dual-core mailbox and every
 * DMA path must call before the Cortex-M85 L1 data cache can be safely enabled:
 *
 *   - **clean** -- write dirty lines back to memory before a peripheral or the
 *     secondary core reads the buffer (use before starting a TX DMA);
 *   - **invalidate** -- drop stale lines before the CPU reads a buffer that a
 *     peripheral or the secondary core wrote (use after an RX DMA completes);
 *   - **clean + invalidate** -- both, for a bidirectional buffer.
 *
 * Each by-address operation acts on the whole cache lines spanning the requested
 * byte range, with the line size discovered at run time from the Cortex-M85
 * Cache Type Register (CTR.DminLine). These are Arm v8-M architectural registers
 * in the System Control Block (PPB window 0xE000Exxx), NOT RA8D2 peripherals, so
 * they carry Arm-architecture references rather than Hardware-User's-Manual
 * citations.
 *
 * @warning This module does NOT enable the D-cache. Enabling it is a separate,
 *          strictly ordered step (issue #173 / the T4 chain) that must land only
 *          after every DMA path and the inter-core mailbox invoke these
 *          primitives -- otherwise a cached write the peer never sees, or a stale
 *          line the CPU reads, silently corrupts data.
 *
 * @note On a host build (`RA8_OFF_TARGET`) the SCB window is backed by the
 *       fake MMIO map, so the maintenance writes are observable to tests but
 *       have no real cache effect; the loop and line-size logic are exercised
 *       exactly as on silicon.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_err.h"

/**
 * @brief Smallest Cortex-M85 D-cache line size, in bytes.
 *
 * @details Reads CTR.DminLine (the log2 of the line size in words) from the Arm
 *          v8-M Cache Type Register and returns the line size in bytes
 *          (`4 << DminLine`). On the RA8D2's Cortex-M85 this is 32 bytes.
 *
 * @return Line size in bytes (a power of two, >= 4).
 * @retval 32 The Cortex-M85 L1 D-cache line size.
 *
 * @pre The Cortex-M85 core is running (CTR is always readable in any mode).
 * @pre No precondition on cache enable state -- CTR reports the geometry
 *      regardless of whether the cache is on.
 * @post No architectural state is modified (pure read).
 * @post The return value is a power-of-two byte count usable as a loop stride.
 *
 * @note Thread-safe; reads a read-only architectural register.
 * @since 0.1.0
 */
[[nodiscard]] uint32_t ra8_cache_dcache_line_bytes(void);

/**
 * @brief Clean (write back) the D-cache lines covering a byte range.
 *
 * @details Writes every cache line that overlaps `[addr, addr + size)` back to
 *          main memory via DCCMVAC, bracketed by data barriers. Call this on a
 *          buffer the CPU has filled, immediately before handing it to a TX DMA
 *          or to the secondary core, so the consumer sees the latest bytes.
 *
 * @param[in] addr Start of the range. Must not be NULL.
 * @param[in] size Range length in bytes; 0 is a no-op success.
 *
 * @return Error code.
 * @retval k_ra8_ok           Lines cleaned (or `size == 0`).
 * @retval k_ra8_err_null_ptr `addr` was NULL.
 *
 * @pre `addr` is non-NULL (unless `size == 0`).
 * @pre The range lies in cacheable memory (SRAM / SDRAM / OSPI XIP).
 * @post Every dirty line overlapping the range has been written to the point of
 *       coherency.
 * @post No line is invalidated -- the data remains resident and valid.
 *
 * @note Not interrupt-safe against concurrent maintenance on the same range.
 * @since 0.1.0
 */
ra8_err_t ra8_cache_dcache_clean_by_addr(const void* addr, uint32_t size);

/**
 * @brief Invalidate (drop) the D-cache lines covering a byte range.
 *
 * @details Discards every cache line overlapping `[addr, addr + size)` via
 *          DCIMVAC, bracketed by data barriers, so the next CPU read fetches
 *          from memory. Call this on a buffer after an RX DMA (or the secondary
 *          core) has written it, before the CPU reads it.
 *
 * @param[in,out] addr Start of the range. Must not be NULL.
 * @param[in]     size Range length in bytes; 0 is a no-op success.
 *
 * @return Error code.
 * @retval k_ra8_ok           Lines invalidated (or `size == 0`).
 * @retval k_ra8_err_null_ptr `addr` was NULL.
 *
 * @pre `addr` is non-NULL (unless `size == 0`).
 * @pre The buffer is cache-line aligned and a whole number of lines, OR the
 *      caller accepts that partially-overlapped lines are dropped (any dirty
 *      bytes in them are lost -- clean first if that matters).
 * @post Every line overlapping the range is marked invalid.
 * @post The next read of the range misses the cache and fetches from memory.
 *
 * @note Not interrupt-safe against concurrent maintenance on the same range.
 * @since 0.1.0
 */
ra8_err_t ra8_cache_dcache_invalidate_by_addr(void* addr, uint32_t size);

/**
 * @brief Clean and invalidate the D-cache lines covering a byte range.
 *
 * @details Writes back then drops every cache line overlapping
 *          `[addr, addr + size)` via DCCIMVAC, bracketed by data barriers. Use
 *          for a buffer that is both produced and re-read across a DMA or a
 *          cross-core handoff.
 *
 * @param[in] addr Start of the range. Must not be NULL.
 * @param[in] size Range length in bytes; 0 is a no-op success.
 *
 * @return Error code.
 * @retval k_ra8_ok           Lines cleaned and invalidated (or `size == 0`).
 * @retval k_ra8_err_null_ptr `addr` was NULL.
 *
 * @pre `addr` is non-NULL (unless `size == 0`).
 * @pre The range lies in cacheable memory.
 * @post Every dirty line is written to memory and then marked invalid.
 * @post The next read of the range fetches from memory.
 *
 * @note Not interrupt-safe against concurrent maintenance on the same range.
 * @since 0.1.0
 */
ra8_err_t ra8_cache_dcache_clean_invalidate_by_addr(const void* addr, uint32_t size);

/**
 * @brief Invalidate the entire D-cache by set/way.
 *
 * @details Walks the cache geometry from CCSIDR (selected via CSSELR) and
 *          invalidates every set and way via DCISW, bracketed by data barriers.
 *          This is the bulk invalidate a safe cache-enable sequence runs once at
 *          boot before setting CCR.DC, so the cache starts from a known-clean
 *          state. It does NOT clean -- any dirty lines are discarded, which is
 *          correct only when the cache has not yet been enabled.
 *
 * @return None.
 *
 * @pre The D-cache is currently DISABLED (this discards, it does not clean).
 * @pre Runs single-threaded with interrupts masked (boot context).
 * @post Every set/way of the L1 D-cache is invalid.
 * @post No architectural register other than CSSELR is left modified.
 *
 * @note Not thread-safe; boot / single-threaded use only.
 * @warning Calling this with the cache enabled and dirty lines present loses
 *          those writes; use ::ra8_cache_dcache_clean_invalidate_by_addr instead.
 * @since 0.1.0
 */
void ra8_cache_dcache_invalidate_all(void);

#ifdef __cplusplus
}
#endif
