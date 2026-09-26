/**
 * @file ra8_imgdec_scratch.h
 * @brief The one bump-scratch contract the decoder shims should share (#768).
 * @ingroup grp_io
 *
 * @par Tag
 * [Ring 3 / Imaging] {World: NS}
 *
 * @details
 * Five private scratch shims decode images in this tree and no two of them
 * agree. `ra8_img_arena.h` and `ra8_webp_arena.h` are the same bump arena
 * written twice, differing only in `live` being `size_t` in one and `uint32_t`
 * in the other; `epub_miniz_alloc.h` carries an explicit opaque handle;
 * `ra8_stbtt_alloc.h` and `unarch_xz_pool.h` are file-static pools with no
 * handle at all. The policy inside them is identical: bump a cursor, count the
 * live blocks, rewind to empty when the count reaches zero, refuse rather than
 * overrun.
 *
 * This is that policy, written once, with the context passed in. A shim keeps
 * only what its SOUP library forces on it: `STBI_MALLOC` and friends are
 * macros with no context parameter, so the implicit bind/unbind slot stays
 * with the shim, pointing at one of these. The arithmetic does not.
 *
 * @par Why not ::ra8_arena_t
 * `libs/ra8_mem/inc/ra8_arena.h` is an init-time carve with no free, by
 * design: it hands out blocks that outlive the call. A decoder's scratch is
 * the opposite shape. stb_image frees every block before returning, in any
 * order, and calls `realloc` mid-decode, so the contract it needs is a
 * reference-counted rewind. Both are bump allocators; only one of them can
 * hand the same bytes out twice.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

#include "ra8_err.h"

/**
 * @enum ra8_imgdec_scratch_limits_t
 * @brief Fixed properties of the scratch policy.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ra8_imgdec_scratch_align = 16U, /**< Alignment every block is rounded up to. */
} ra8_imgdec_scratch_limits_t;

/**
 * @struct ra8_imgdec_scratch_t
 * @brief Caller-owned bump scratch with reference-counted rewind.
 *
 * @details Zero-initialise and pass to ::ra8_imgdec_scratch_init(); the caller
 * owns the backing store and must out-live every pointer handed out of it.
 *
 * @invariant `offset <= cap`.
 * @invariant `high_water <= cap`.
 * @invariant `live == 0` implies `offset == 0`.
 *
 * @since 0.1.0
 */
typedef struct {
  uint8_t* base;       /**< Caller-owned backing store, `cap` writable bytes. */
  size_t   cap;        /**< Backing-store capacity in bytes.                  */
  size_t   offset;     /**< Bump cursor; the next block starts here.          */
  size_t   live;       /**< Blocks handed out and not yet released.           */
  size_t   high_water; /**< Deepest `offset` reached since init.              */
} ra8_imgdec_scratch_t;

/**
 * @brief Bind @p base as @p scratch's backing store and empty it.
 *
 * @param[out] scratch Scratch record to initialise.
 * @param[in]  base    Backing store of at least @p cap writable bytes.
 * @param[in]  cap     Capacity of @p base in bytes.
 *
 * @return ra8_err_t ::k_ra8_ok when @p scratch is ready to allocate from.
 * @retval k_ra8_err_invalid_arg  @p scratch or @p base was NULL.
 * @retval k_ra8_err_invalid_size @p cap was zero.
 *
 * @pre @p base points at @p cap writable bytes that out-live @p scratch.
 * @post On success @p scratch is empty and its high-water mark is zero.
 * @post On failure @p scratch is left untouched.
 *
 * @note Not thread-safe: one scratch belongs to one decode.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_imgdec_scratch_init(ra8_imgdec_scratch_t* scratch, void* base,
                                                size_t cap);

/**
 * @brief Drop every outstanding block and rewind @p scratch to empty.
 *
 * @details The high-water mark survives, which is the point of it: a caller
 * sizes the backing store from the peak of a whole run, not of one decode.
 *
 * @param[in,out] scratch Scratch to empty; NULL is ignored.
 *
 * @return None.
 *
 * @post `offset` and `live` are zero; `base`, `cap` and `high_water` are kept.
 *
 * @note Not thread-safe: one scratch belongs to one decode.
 *
 * @since 0.1.0
 */
void ra8_imgdec_scratch_reset(ra8_imgdec_scratch_t* scratch);

/**
 * @brief Hand out @p bytes of aligned scratch.
 *
 * @param[in,out] scratch Scratch to draw from; NULL fails.
 * @param[in]     bytes   Byte count requested; zero fails.
 *
 * @return void* Block of at least @p bytes, aligned to
 *               ::k_ra8_imgdec_scratch_align, or NULL.
 * @retval NULL No scratch, no backing store, @p bytes was zero, or the aligned
 *              request does not fit the remaining capacity.
 *
 * @post On success `live` is one higher and `offset` has advanced.
 * @post On failure @p scratch is unchanged.
 *
 * @note Not thread-safe: one scratch belongs to one decode.
 *
 * @since 0.1.0
 */
[[nodiscard]] void* ra8_imgdec_scratch_alloc(ra8_imgdec_scratch_t* scratch, size_t bytes);

/**
 * @brief Hand out @p count elements of @p size bytes, zeroed.
 *
 * @details The product is checked for overflow before anything is reserved,
 * which is the one thing the five shims each had to remember separately.
 *
 * @param[in,out] scratch Scratch to draw from; NULL fails.
 * @param[in]     count   Element count; zero fails.
 * @param[in]     size    Element size in bytes; zero fails.
 *
 * @return void* Zeroed block of `count * size` bytes, or NULL.
 * @retval NULL Either argument was zero, the product overflowed, or the block
 *              does not fit.
 *
 * @post On success the returned block reads as zeroes.
 * @post On failure @p scratch is unchanged.
 *
 * @note Not thread-safe: one scratch belongs to one decode.
 *
 * @since 0.1.0
 */
[[nodiscard]] void* ra8_imgdec_scratch_calloc(ra8_imgdec_scratch_t* scratch, size_t count,
                                              size_t size);

/**
 * @brief Grow @p ptr to @p new_bytes by fresh-allocating and copying.
 *
 * @details A bump allocator cannot grow a block in place, so this reserves
 * @p new_bytes, copies `min(old_bytes, new_bytes)` bytes across and releases
 * @p ptr. The old block's space is not reclaimed until the scratch next
 * drains, which is why the backing store is sized for the peak rather than
 * the net footprint.
 *
 * @param[in,out] scratch   Scratch to draw from; NULL fails.
 * @param[in]     ptr       Existing block, or NULL to allocate fresh.
 * @param[in]     old_bytes Size of @p ptr in bytes; zero when @p ptr is NULL.
 * @param[in]     new_bytes Requested size in bytes; zero fails.
 *
 * @return void* Block of @p new_bytes carrying the old contents, or NULL.
 * @retval NULL The request does not fit, or an argument was unusable.
 *
 * @post On success the first `min(old_bytes, new_bytes)` bytes match @p ptr.
 * @post On failure @p ptr is still valid and @p scratch is unchanged.
 *
 * @note Not thread-safe: one scratch belongs to one decode.
 *
 * @since 0.1.0
 */
[[nodiscard]] void* ra8_imgdec_scratch_realloc(ra8_imgdec_scratch_t* scratch, void* ptr,
                                               size_t old_bytes, size_t new_bytes);

/**
 * @brief Release one block; rewind to empty once none are live.
 *
 * @details Per-block extents are not tracked, so a release only decrements the
 * live count. When it reaches zero the scratch has fully drained and the
 * cursor rewinds. A NULL @p ptr, or a release against an already-empty
 * scratch, is ignored rather than treated as an error: the SOUP decoders this
 * fronts free on their own error paths.
 *
 * @param[in,out] scratch Scratch the block came from; NULL is ignored.
 * @param[in]     ptr     Block to release; NULL is ignored.
 *
 * @return None.
 *
 * @post `live` is one lower, never below zero.
 * @post When `live` reaches zero, `offset` is zero.
 *
 * @note Not thread-safe: one scratch belongs to one decode.
 *
 * @since 0.1.0
 */
void ra8_imgdec_scratch_free(ra8_imgdec_scratch_t* scratch, void* ptr);

/**
 * @brief Deepest byte count @p scratch has held since it was initialised.
 *
 * @param[in] scratch Scratch to query; NULL reads as zero.
 *
 * @return size_t Peak occupancy in bytes, including alignment padding.
 *
 * @post No state is mutated.
 *
 * @note Thread-safe against other readers only.
 *
 * @since 0.1.0
 */
[[nodiscard]] size_t ra8_imgdec_scratch_high_water(const ra8_imgdec_scratch_t* scratch);

#ifdef __cplusplus
}
#endif
