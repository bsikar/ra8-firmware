/**
 * @file ra8_arena.h
 * @brief Init-time bump arena -- carves per-tier slab backing, zero-heap.
 * @ingroup grp_ereader
 *
 * @par Tag
 * [Ring 2 / Core] {World: NS}
 *
 * @details
 * Layer 0 of the #147 memory hierarchy, paired with ::ra8_slab. An arena owns one
 * contiguous memory region of a single tier (DTCM, SRAM, or SDRAM) and hands out
 * aligned sub-blocks by bumping a high-water mark. It is an **init-time** API:
 * there is no free -- you carve all the slab backing (and any other fixed
 * buffers) once during bring-up, then never allocate again (NASA Power-of-10
 * Rule 3). The remaining-bytes query lets the bring-up code fail fast if the
 * budget is over-subscribed.
 *
 * @code
 * extern uint8_t __sdram_pool_start[];           // from the linker script
 * ra8_arena_t sdram = {};
 * (void)ra8_arena_init(&sdram, __sdram_pool_start, 40U * 1024U * 1024U);
 * void* frame_backing = nullptr;
 * (void)ra8_arena_carve(&sdram, 64U * 4096U, 8U, &frame_backing); // 64 x 4 KiB
 * ra8_slab_t frames = {};
 * (void)ra8_slab_init(&frames, frame_backing, 64U * 4096U, 4096U);
 * @endcode
 *
 * @note Not thread-safe; bring-up runs single-threaded.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 *
 *

 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

#include "ra8_err.h"

/**
 * @struct ra8_arena_t
 * @brief Caller-owned bump-arena state over one tier region.
 *
 * @details Zero-initialise and pass to ::ra8_arena_init. The backing region must
 *          out-live the arena and every block carved from it. Fields are private.
 *
 * @invariant `used <= size`.
 *
 * @since 0.1.0
 */
typedef struct {
  uint8_t* base; /**< First byte of the tier region.         */
  uint32_t size; /**< Region length in bytes.                */
  uint32_t used; /**< High-water mark (bytes carved so far). */
} ra8_arena_t;

/**
 * @brief Bind a bump arena over a caller-owned tier region.
 *
 * @param[out] arena Arena state to populate (zero-initialised by caller).
 * @param[in]  base  First byte of the region (out-lives the arena).
 * @param[in]  size  Region length in bytes.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               Arena ready (empty).
 * @retval k_ra8_err_null_ptr     `arena` or `base` was NULL.
 * @retval k_ra8_err_invalid_size `size` was zero.
 *
 * @pre `base` addresses at least `size` writable bytes.
 * @pre `arena` does not alias `base`.
 * @post On success `arena->used == 0` and the whole region is available.
 * @post On any non-ok return `arena` is left unbound.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_arena_init(ra8_arena_t* arena, void* base, uint32_t size);

/**
 * @brief Carve an aligned sub-block from the arena (bump; no free).
 *
 * @param[in]  arena   Initialised arena.
 * @param[in]  bytes   Block size to carve (> 0).
 * @param[in]  align   Required alignment in bytes (a power of two, >= 1).
 * @param[out] out_ptr Receives the aligned block pointer on success.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               Block carved; `*out_ptr` set.
 * @retval k_ra8_err_null_ptr     `arena` or `out_ptr` was NULL.
 * @retval k_ra8_err_invalid_size `bytes` was zero.
 * @retval k_ra8_err_invalid_arg  `align` was zero or not a power of two.
 * @retval k_ra8_err_no_mem       The aligned block does not fit the remainder.
 *
 * @pre `arena` was populated by ::ra8_arena_init.
 * @pre `align` is a power of two.
 * @post On success `*out_ptr` is `align`-aligned with `bytes` of storage, and
 *       `arena->used` advanced past it.
 * @post On any non-ok return the arena is unchanged.
 *
 * @note Not thread-safe; init-time use only (there is no matching free).
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_arena_carve(ra8_arena_t* arena, uint32_t bytes, uint32_t align, void** out_ptr);

/**
 * @brief Report the bytes still available in the arena.
 *
 * @param[in]  arena         Initialised arena.
 * @param[out] out_remaining Receives `size - used`.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok           Remaining bytes reported.
 * @retval k_ra8_err_null_ptr `arena` or `out_remaining` was NULL.
 *
 * @pre `arena` was populated by ::ra8_arena_init.
 * @pre `out_remaining` is writable.
 * @post `*out_remaining == arena->size - arena->used`.
 * @post No arena state is mutated.
 *
 * @note Thread-safe with respect to a quiescent arena (pure read).
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_arena_remaining(const ra8_arena_t* arena, uint32_t* out_remaining);

static inline void* ra8_arena_alloc(ra8_arena_t* arena, uint32_t bytes, uint32_t align)
{
  void* ptr = NULL;
  (void)ra8_arena_carve(arena, bytes, align, &ptr);
  return ptr;
}

static inline void* ra8_arena_calloc(ra8_arena_t* arena, uint32_t count, uint32_t size)
{
  const uint32_t total = (count) * (size);
  void*          ptr   = ra8_arena_alloc(arena, total, 8U);
  if (ptr != NULL) {
    /* MISRA 11.5: ra8_arena_carve guarantees the pointer is to a writable
     * uint8_t region, so the conversion from void* is safe. */
    uint8_t* dst = (uint8_t*)ptr; /* cppcheck-suppress misra-c2012-11.5 */
    for (uint32_t i = 0U; i < total; ++i) {
      dst[i] = 0U;
    }
  }
  return ptr;
}

static inline void ra8_arena_reset(ra8_arena_t* arena)
{
  if (arena != NULL) {
    arena->used = 0U;
  }
}

static inline uint32_t ra8_arena_save(ra8_arena_t* arena)
{
  uint32_t mark = 0U;
  if (arena != NULL) {
    mark = arena->used;
  }
  return mark;
}

static inline void ra8_arena_restore(ra8_arena_t* arena, uint32_t mark)
{
  if (arena != NULL) {
    arena->used = mark;
  }
}

#ifdef __cplusplus
}
#endif
