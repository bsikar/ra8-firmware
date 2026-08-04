/**
 * @file ra8_slab.h
 * @brief Fixed-cell slab allocator -- O(1), zero-fragmentation, zero-heap.
 * @ingroup grp_ereader
 *
 * @par Tag
 * [Ring 2 / Core] {World: NS}
 *
 * @details
 * Layer 0 of the #147 memory hierarchy. A slab carves a caller-owned buffer into
 * a fixed number of equal-size cells and hands them out / takes them back in
 * O(1) from an intrusive freelist. Because every cell is the same size there is
 * no external fragmentation, and because the backing buffer is supplied by the
 * caller (carved once from a per-tier arena at init), nothing is ever malloc'd
 * after init -- NASA Power-of-10 Rule 3.
 *
 * The freelist is threaded through the free cells themselves: each free cell's
 * first 4 bytes hold the index of the next free cell (::k_ra8_slab_nil
 * terminates). So a slab needs zero side-table memory and `cell_bytes` must be
 * at least 4 and a multiple of 4; the backing buffer must be 4-byte aligned (and
 * aligned to whatever the cells' payload needs).
 *
 * Typical Layer-2 use: 4 KiB page frames in SDRAM. Typical Layer-3 use: glyph
 * bitmaps in SRAM. The slab is agnostic to what a cell holds.
 *
 * @code
[[gnu::aligned(8)]]  * static uint8_t  s_pool[64U * 4096U];
 * ra8_slab_t       frames = {};
 * (void)ra8_slab_init(&frames, s_pool, sizeof(s_pool), 4096U); // 64 x 4 KiB
 * void* f = nullptr;
 * if (ra8_slab_alloc(&frames, &f) == k_ra8_ok) { ... ra8_slab_free(&frames, f); }
 * @endcode
 *
 * @note Not thread-safe; callers serialise (single-threaded reader / IRQs masked).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_err.h"

/**
 * @enum ra8_slab_const_t
 * @brief Slab sizing limits and the freelist terminator.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ra8_slab_nil            = 0xFFFFFFFFU, /**< Freelist terminator (no next cell).    */
  k_ra8_slab_min_cell_bytes = 4U,          /**< Smallest cell (holds one index).       */
  k_ra8_slab_align_bytes    = 4U,          /**< Required cell-size + buffer alignment. */
} ra8_slab_const_t;

/**
 * @struct ra8_slab_t
 * @brief Caller-owned state for one fixed-cell pool.
 *
 * @details Zero-initialise and pass to ::ra8_slab_init. The backing buffer must
 *          out-live the slab. Treat the fields as private.
 *
 * @invariant `free_count <= cell_count`.
 * @invariant `free_head` is either ::k_ra8_slab_nil or a valid cell index.
 *
 * @since 0.1.0
 */
typedef struct {
  uint8_t* base;       /**< First byte of the cell array (caller buffer).      */
  uint32_t cell_bytes; /**< Bytes per cell (>= 4, multiple of 4).              */
  uint32_t cell_count; /**< Total cells the buffer was divided into.           */
  uint32_t free_head;  /**< Index of the first free cell, or ::k_ra8_slab_nil. */
  uint32_t free_count; /**< Cells currently free (observability).              */
} ra8_slab_t;

/**
 * @brief Initialise a slab over a caller-owned buffer.
 *
 * @details Divides @p buffer into `floor(buffer_bytes / cell_bytes)` cells and
 *          threads every cell onto the freelist so all cells start free.
 *
 * @param[out] slab         Slab state to populate (zero-initialised by caller).
 * @param[in]  buffer       Backing memory, 4-byte aligned, out-living the slab.
 * @param[in]  buffer_bytes Size of @p buffer in bytes.
 * @param[in]  cell_bytes   Bytes per cell (>= 4 and a multiple of 4).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               Slab ready; all cells free.
 * @retval k_ra8_err_null_ptr     `slab` or `buffer` was NULL.
 * @retval k_ra8_err_invalid_size `cell_bytes` < 4, not 4-aligned, or larger than
 *                               `buffer_bytes` (zero cells).
 *
 * @pre `buffer` is at least `buffer_bytes` and 4-byte aligned.
 * @pre `slab` does not alias `buffer`.
 * @post On success `slab->free_count == slab->cell_count >= 1`.
 * @post On any non-ok return `slab` is left unmodified-usable (not bound).
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_slab_init(ra8_slab_t* slab, void* buffer, uint32_t buffer_bytes, uint32_t cell_bytes);

/**
 * @brief Allocate one cell from the slab (O(1)).
 *
 * @param[in]  slab     Initialised slab.
 * @param[out] out_cell Receives the cell pointer on success.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok           A cell was handed out; `*out_cell` set.
 * @retval k_ra8_err_null_ptr `slab` or `out_cell` was NULL.
 * @retval k_ra8_err_no_mem   The slab is exhausted (no free cell).
 *
 * @pre `slab` was populated by ::ra8_slab_init.
 * @pre `out_cell` is writable.
 * @post On success `*out_cell` points at `cell_bytes` of usable storage.
 * @post On success `free_count` decreased by one.
 *
 * @note Not thread-safe. The returned cell's contents are unspecified.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_slab_alloc(ra8_slab_t* slab, void** out_cell);

/**
 * @brief Return a previously-allocated cell to the slab (O(1)).
 *
 * @param[in] slab Initialised slab.
 * @param[in] cell A cell pointer previously returned by ::ra8_slab_alloc.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                 Cell returned to the freelist.
 * @retval k_ra8_err_null_ptr       `slab` or `cell` was NULL.
 * @retval k_ra8_err_invalid_arg    `cell` is outside the slab or not on a cell
 *                                 boundary.
 *
 * @pre `cell` came from this slab and is not already free.
 * @pre `slab` was populated by ::ra8_slab_init.
 * @post On success `free_count` increased by one and `cell` is reusable.
 * @post On any non-ok return the slab is unchanged.
 *
 * @note Not thread-safe. Double-free of an in-bounds cell is not detected (it
 *       corrupts the freelist); callers must not double-free.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_slab_free(ra8_slab_t* slab, void* cell);

/**
 * @brief Report the slab's free / total cell counts.
 *
 * @param[in]  slab      Initialised slab.
 * @param[out] out_free  Free cell count (may be NULL).
 * @param[out] out_total Total cell count (may be NULL).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok           Counters reported.
 * @retval k_ra8_err_null_ptr `slab` was NULL.
 *
 * @pre `slab` was populated by ::ra8_slab_init.
 * @pre At least one output pointer is non-NULL to be useful.
 * @post On success the requested counters are written.
 * @post No slab state is mutated.
 *
 * @note Thread-safe with respect to a quiescent slab (pure read).
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_slab_stats(const ra8_slab_t* slab, uint32_t* out_free, uint32_t* out_total);

#ifdef __cplusplus
}
#endif
