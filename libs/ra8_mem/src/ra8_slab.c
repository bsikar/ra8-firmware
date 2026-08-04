/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_slab.c
 * @brief Fixed-cell slab allocator implementation (Layer 0, #147).
 *
 * @par Tag
 * [Ring 2 / Core] {World: NS}
 *
 * @details
 * The freelist is intrusive: each free cell stores, in its first 4 bytes, the
 * index of the next free cell (::k_ra8_slab_nil terminates). `memcpy` is used to
 * read/write that index so the access is alignment- and aliasing-safe regardless
 * of the cell's payload type.
 */

#include "ra8_slab.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"

/** @brief Module log tag. */
static const char* const s_tag = "ra8_slab";

/**
 * @brief Read the freelist next-index stored in free cell @p idx.
 *
 * @details Loads the 4-byte next-index threaded through the free cell at @p idx
 *          via `memcpy` (alignment-safe).
 *
 * @param[in] slab Initialised slab.
 * @param[in] idx  Index of a free cell (< `slab->cell_count`).
 *
 * @return The stored next-cell index, or ::k_ra8_slab_nil at the list tail.
 * @retval k_ra8_slab_nil Cell @p idx is the tail of the freelist.
 *
 * @pre `slab` and `slab->base` are non-NULL.
 * @pre `idx` addresses a cell currently on the freelist.
 * @post No state is modified.
 * @post The returned value is a valid index or ::k_ra8_slab_nil.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static uint32_t priv_slab_next(const ra8_slab_t* slab, uint32_t idx)
{
  uint32_t next = 0U;
  (void)memcpy(&next, &slab->base[(size_t)idx * (size_t)slab->cell_bytes], sizeof(next));
  return next;
}

/**
 * @brief Write the freelist next-index into cell @p idx.
 *
 * @details Stores the 4-byte @p next index into the cell at @p idx via `memcpy`
 *          (alignment-safe), threading it onto the freelist.
 *
 * @param[in,out] slab Initialised slab.
 * @param[in]     idx  Index of the cell to update (< `slab->cell_count`).
 * @param[in]     next Next-cell index to store (or ::k_ra8_slab_nil).
 *
 * @return Nothing.
 *
 * @pre `slab` and `slab->base` are non-NULL.
 * @pre `idx` addresses a cell inside the slab.
 * @post Cell @p idx holds @p next as its freelist link.
 * @post No other state is modified.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static void priv_slab_set_next(ra8_slab_t* slab, uint32_t idx, uint32_t next)
{
  (void)memcpy(&slab->base[(size_t)idx * (size_t)slab->cell_bytes], &next, sizeof(next));
}

ra8_err_t ra8_slab_init(ra8_slab_t* slab, void* buffer, uint32_t buffer_bytes, uint32_t cell_bytes)
{
  RA8_CHECK_NULL_PTR(slab, s_tag, "slab must not be nullptr");
  RA8_CHECK_NULL_PTR(buffer, s_tag, "buffer must not be nullptr");
  if (cell_bytes < (uint32_t)k_ra8_slab_min_cell_bytes) {
    return k_ra8_err_invalid_size;
  }
  if ((cell_bytes % (uint32_t)k_ra8_slab_align_bytes) != 0U) {
    return k_ra8_err_invalid_size;
  }
  const uint32_t count = buffer_bytes / cell_bytes;
  if (count == 0U) {
    return k_ra8_err_invalid_size;
  }
  slab->base       = (uint8_t*)buffer;
  slab->cell_bytes = cell_bytes;
  slab->cell_count = count;
  for (uint32_t i = 0U; i < count; ++i) {
    const uint32_t next = ((i + 1U) < count) ? (i + 1U) : (uint32_t)k_ra8_slab_nil;
    priv_slab_set_next(slab, i, next);
  }
  slab->free_head  = 0U;
  slab->free_count = count;
  return k_ra8_ok;
}

ra8_err_t ra8_slab_alloc(ra8_slab_t* slab, void** out_cell)
{
  RA8_CHECK_NULL_PTR(slab, s_tag, "slab must not be nullptr");
  RA8_CHECK_NULL_PTR(out_cell, s_tag, "out_cell must not be nullptr");
  if (slab->free_head == (uint32_t)k_ra8_slab_nil) {
    return k_ra8_err_no_mem;
  }
  const uint32_t idx = slab->free_head;
  slab->free_head    = priv_slab_next(slab, idx);
  slab->free_count--;
  *out_cell = (void*)&slab->base[(size_t)idx * (size_t)slab->cell_bytes];
  return k_ra8_ok;
}

ra8_err_t ra8_slab_free(ra8_slab_t* slab, void* cell)
{
  RA8_CHECK_NULL_PTR(slab, s_tag, "slab must not be nullptr");
  RA8_CHECK_NULL_PTR(cell, s_tag, "cell must not be nullptr");
  const uintptr_t cell_addr = (uintptr_t)cell;
  const uintptr_t base_addr = (uintptr_t)slab->base;
  if (cell_addr < base_addr) {
    return k_ra8_err_invalid_arg;
  }
  const uintptr_t off  = cell_addr - base_addr;
  const uintptr_t span = (uintptr_t)slab->cell_count * (uintptr_t)slab->cell_bytes;
  if (off >= span) {
    return k_ra8_err_invalid_arg;
  }
  if ((off % (uintptr_t)slab->cell_bytes) != 0U) {
    return k_ra8_err_invalid_arg;
  }
  const uint32_t idx = (uint32_t)(off / (uintptr_t)slab->cell_bytes);
  priv_slab_set_next(slab, idx, slab->free_head);
  slab->free_head = idx;
  slab->free_count++;
  return k_ra8_ok;
}

ra8_err_t ra8_slab_stats(const ra8_slab_t* slab, uint32_t* out_free, uint32_t* out_total)
{
  RA8_CHECK_NULL_PTR(slab, s_tag, "slab must not be nullptr");
  if (slab->base == nullptr) {
    return k_ra8_err_invalid_state;
  }
  if (out_free != nullptr) {
    *out_free = slab->free_count;
  }
  if (out_total != nullptr) {
    *out_total = slab->cell_count;
  }
  return k_ra8_ok;
}
