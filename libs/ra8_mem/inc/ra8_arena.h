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
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_err.h"

/**
 * @struct ra8_arena_t
 * @brief Caller-owned bump-arena state over one tier region.
 *
 * @details Zero-initialise and pass to ::ra8_arena_init. The backing region must
 *          out-live the arena and every block carved from it. Fields are private.
 *
 * @invariant `used <= high_water <= size`.
 *
 * @since 0.1.0
 */
typedef struct {
  uint8_t* base;       /**< First byte of the tier region.                  */
  uint32_t size;       /**< Region length in bytes.                         */
  uint32_t used;       /**< Live bump cursor (bytes carved so far).         */
  uint32_t high_water; /**< Largest `used` observed since ::ra8_arena_init. */
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
 * @post On success `arena->used` and `arena->high_water` are zero and the whole
 *       region is available.
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

/**
 * @struct ra8_arena_slot_t
 * @brief One named sub-block of a multi-slot workspace carve.
 *
 * @details Describes a single reservation for ::ra8_arena_carve_all: how many
 *          bytes it needs, what alignment it needs, and where the resulting
 *          pointer is written. A workspace struct is filled by declaring one
 *          slot per member instead of chaining byte offsets by hand.
 *
 * @since 0.1.0
 */
typedef struct {
  uint32_t bytes;   /**< Block size to carve (> 0).       */
  uint32_t align;   /**< Alignment: a power of two, >= 1. */
  void**   out_ptr; /**< Receives the aligned block.      */
} ra8_arena_slot_t;

/**
 * @enum ra8_arena_limits_t
 * @brief Bounds the arena enforces on a multi-slot carve.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ra8_arena_slot_cap = 16U, /**< Largest slot count ::ra8_arena_carve_all accepts. */
} ra8_arena_limits_t;

/**
 * @brief Carve a whole multi-slot workspace, or nothing at all.
 *
 * @details Validates every slot and proves the whole set fits before the first
 *          pointer is published, so a workspace is never half-filled: on any
 *          rejection the arena is unchanged and no `out_ptr` has been written.
 *          Slots are carved in array order, so the caller controls packing.
 *          This is the checked replacement for a hand-written offset chain and
 *          its per-slot capacity comparisons.
 *
 * @param[in,out] arena      Initialised arena.
 * @param[in]     slots      Array of @p slot_count slot descriptors.
 * @param[in]     slot_count Number of slots (1 .. ::k_ra8_arena_slot_cap).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               Every slot carved; every `out_ptr` set.
 * @retval k_ra8_err_null_ptr     `arena`, `slots`, or some `slots[i].out_ptr` was NULL.
 * @retval k_ra8_err_invalid_size Some `slots[i].bytes` was zero.
 * @retval k_ra8_err_invalid_arg  `slot_count` was zero or above the cap, or some
 *                                `slots[i].align` was zero or not a power of two.
 * @retval k_ra8_err_no_mem       The slots do not all fit the remainder.
 *
 * @pre `arena` was populated by ::ra8_arena_init.
 * @pre Each `slots[i].out_ptr` addresses writable pointer storage.
 * @post On success each `slots[i].out_ptr` holds an `align`-aligned block of
 *       `bytes` storage, and no two blocks overlap.
 * @post On any non-ok return the arena and every `out_ptr` are untouched.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_arena_carve_all(ra8_arena_t* arena, const ra8_arena_slot_t* slots, uint32_t slot_count);

/**
 * @brief Report the largest occupancy the arena has ever reached.
 *
 * @details Unlike ::ra8_arena_remaining this survives ::ra8_arena_reset, so a
 *          reusable scratch arena can report the peak a whole run needed and
 *          bring-up can size the region from a measurement rather than a guess.
 *
 * @param[in]  arena          Initialised arena.
 * @param[out] out_high_water Receives the peak `used` value since init.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok           Peak reported.
 * @retval k_ra8_err_null_ptr `arena` or `out_high_water` was NULL.
 *
 * @pre `arena` was populated by ::ra8_arena_init.
 * @pre `out_high_water` is writable.
 * @post `*out_high_water == arena->high_water`.
 * @post No arena state is mutated.
 *
 * @note Thread-safe with respect to a quiescent arena (pure read).
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_arena_high_water(const ra8_arena_t* arena, uint32_t* out_high_water);

/**
 * @brief Rewind the arena to empty, keeping the high-water record.
 *
 * @details For a reusable scratch arena whose blocks all die together at the
 *          end of one operation: the next operation carves the same region
 *          again. Slab backing carved once during bring-up must never be in a
 *          reset arena, because reset does not and cannot invalidate the
 *          pointers already handed out.
 *
 * @param[in,out] arena Initialised arena.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok           Arena rewound.
 * @retval k_ra8_err_null_ptr `arena` was NULL.
 *
 * @pre `arena` was populated by ::ra8_arena_init.
 * @pre Every block previously carved from @p arena is dead.
 * @post `arena->used == 0` and the whole region is available again.
 * @post `arena->high_water` is unchanged.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_arena_reset(ra8_arena_t* arena);


#ifdef __cplusplus
}
#endif
