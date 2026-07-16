/**
 * @file ra8_webp_arena.h
 * @brief Heap-free scratch allocator hooks for the vendored libwebp decoder.
 * @ingroup grp_ereader
 *
 * @details
 * libwebp funnels every heap request through `WebPSafeMalloc` /
 * `WebPSafeCalloc` / `WebPSafeFree` in `libs/third_party/libwebp/src/utils/
 * utils.c`. The firmware has no heap (`_sbrk` traps after init), so that file
 * carries a delimited `RA8 LOCAL PATCH` (documented in `docs/SOUP/libwebp.md`)
 * that -- when built with `-DRA8_WEBP_USE_ARENA` -- redirects those three
 * functions to the hooks below, exactly the way `stb_image` is fronted by
 * ::ra8_img_arena (see `libs/third_party/stb/stb_image_impl.c`). This arena is
 * a deliberate sibling of ra8_img_arena rather than a reuse of it: keeping the
 * WebP decoder decoupled from `libs/ra8_reflow` until the #289 band-tile render
 * path lands avoids a premature cross-library dependency, and the WebP path
 * additionally needs a zeroing ::ra8_webp_arena_calloc that the stb hooks do
 * not expose.
 *
 * The allocator is a bump arena with reference-counted auto-reset: each
 * ra8_webp_arena_malloc()/_calloc() bumps `offset` and increments `live`; each
 * ra8_webp_arena_free() decrements `live` and, when it reaches zero, rewinds
 * `offset` to the base. A one-shot WebP decode frees all of its scratch before
 * returning, so the arena fully drains after each decode -- no caller
 * bookkeeping, robust to any free order, immune to fragmentation. On exhaustion
 * the malloc hook returns nullptr; libwebp propagates that as a decode failure
 * rather than corrupting memory.
 *
 * NASA Power-of-10 Rule 3 (no dynamic allocation after init): the backing
 * store is caller-owned static/SRAM/SDRAM storage, never `malloc`.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * [Ring 4 / WebP] {World: NS}
 *
 * @since 0.1.0
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

/**
 * @struct ra8_webp_arena_t
 * @brief Caller-owned bump arena backing a single WebP decode.
 *
 * @details A linear bump allocator over `base[0..cap)` with a live-block count:
 * libwebp's allocations bump `offset`, each free decrements `live`, and the
 * arena auto-resets to empty when `live` reaches 0 -- so it fully drains after
 * each decode with no caller bookkeeping and no fragmentation.
 *
 * @invariant `offset <= cap` at all times.
 * @invariant `live == 0` implies `offset == 0` (fully drained).
 *
 * @par Example:
 * @code
 * alignas(16) static uint8_t backing[2U * 1024U * 1024U];
 * ra8_webp_arena_t arena = {.base = backing, .cap = sizeof backing,
 *                           .offset = 0U, .live = 0U};
 * ra8_webp_arena_bind(&arena);
 * // ... WebP decode draws scratch from `arena` ...
 * ra8_webp_arena_unbind();
 * @endcode
 *
 * @see ra8_webp_arena_bind()
 * @since 0.1.0
 */
typedef struct {
  uint8_t* base;   /**< First byte of caller-owned backing store (>= @c cap bytes). */
  size_t   cap;    /**< Backing-store capacity in bytes.                            */
  size_t   offset; /**< Bump cursor; next allocation starts here. Managed field.    */
  uint32_t live;   /**< Count of outstanding (unfreed) allocations. Managed field.  */
} ra8_webp_arena_t;

/**
 * @brief Bind @p arena as the active scratch for subsequent decode allocations.
 *
 * @details Records @p arena in a file-static slot and resets it to empty
 * (`offset = 0`, `live = 0`). Every ra8_webp_arena_malloc() / _calloc() / _free()
 * until the next ra8_webp_arena_bind() or ra8_webp_arena_unbind() operates on it.
 *
 * @param[in,out] arena Arena to make active; reset to empty on entry. NULL
 *                      unbinds (equivalent to ra8_webp_arena_unbind()).
 * @return None.
 * @pre @p arena, if non-NULL, has `base` pointing at `cap` writable bytes.
 * @pre Called single-threaded around one decode (decoding is not re-entrant).
 * @post The active arena is @p arena and, if non-NULL, it is empty.
 * @post A subsequent ra8_webp_arena_malloc() draws from @p arena.
 * @note Not thread-safe: image decoding is single-threaded on this target.
 * @see ra8_webp_arena_unbind()
 * @since 0.1.0
 */
void ra8_webp_arena_bind(ra8_webp_arena_t* arena);

/**
 * @brief Unbind the active arena; subsequent allocation hooks fail with nullptr.
 *
 * @details Clears the file-static active-arena slot. Used to fence the libwebp
 * hooks outside a decode so a stray allocation cannot scribble on a stale buffer.
 *
 * @return None.
 * @pre None.
 * @pre Called single-threaded around one decode.
 * @post The active-arena slot is NULL.
 * @post A subsequent ra8_webp_arena_malloc() returns nullptr until the next bind.
 * @note Not thread-safe: image decoding is single-threaded on this target.
 * @see ra8_webp_arena_bind()
 * @since 0.1.0
 */
void ra8_webp_arena_unbind(void);

/**
 * @brief libwebp `WebPSafeMalloc` hook: bump @p n bytes from the bound arena.
 *
 * @details Rounds @p n up to the 16-byte alignment, bumps the bound arena's
 * offset, and increments its live-block count. Fails with nullptr when no arena
 * is bound, when @p n exceeds the capacity, or when the request does not fit the
 * remaining capacity.
 *
 * @param[in] n Byte count requested by libwebp.
 * @return 16-byte-aligned pointer into the bound arena, or nullptr.
 * @retval nullptr No arena bound, or the request does not fit.
 * @pre Either an arena is bound (then a decode is in progress) or the call fails.
 * @pre Called single-threaded from the decoder.
 * @post On success the live-block count is incremented and the offset advances.
 * @post On failure the arena state is unchanged.
 * @note Not thread-safe: image decoding is single-threaded on this target.
 * @see ra8_webp_arena_calloc()
 * @since 0.1.0
 */
void* ra8_webp_arena_malloc(size_t n);

/**
 * @brief libwebp `WebPSafeCalloc` hook: zeroed @p nmemb * @p size bytes.
 *
 * @details Computes the product with an overflow guard, bump-allocates it via
 * the same path as ra8_webp_arena_malloc(), then zero-fills the block (the
 * arena backing store is reused across decodes and is not pre-zeroed).
 *
 * @param[in] nmemb Element count requested by libwebp.
 * @param[in] size  Per-element size in bytes.
 * @return 16-byte-aligned zeroed pointer into the bound arena, or nullptr.
 * @retval nullptr No arena bound, the product overflows `size_t`, or it does
 *                 not fit the remaining capacity.
 * @pre Either an arena is bound (then a decode is in progress) or the call fails.
 * @pre Called single-threaded from the decoder.
 * @post On success the returned block is fully zeroed and `live` is incremented.
 * @post On failure the arena state is unchanged.
 * @note Not thread-safe: image decoding is single-threaded on this target.
 * @see ra8_webp_arena_malloc()
 * @since 0.1.0
 */
void* ra8_webp_arena_calloc(size_t nmemb, size_t size);

/**
 * @brief libwebp `WebPSafeFree` hook: release a block; auto-reset when none live.
 *
 * @details Decrements the bound arena's live-block count; the bump offset is not
 * tracked per block. When the count reaches zero the arena has fully drained, so
 * the offset rewinds to the base. A nullptr argument, or no bound arena, is
 * ignored.
 *
 * @param[in] p Pointer to release; nullptr is ignored.
 * @return None.
 * @pre @p p was returned by ra8_webp_arena_malloc()/_calloc(), or is nullptr.
 * @pre Called single-threaded from the decoder.
 * @post The live-block count is decremented (never below zero).
 * @post When the live-block count reaches zero the arena offset rewinds to base.
 * @note Not thread-safe: image decoding is single-threaded on this target.
 * @see ra8_webp_arena_malloc()
 * @since 0.1.0
 */
void ra8_webp_arena_free(void* p);
