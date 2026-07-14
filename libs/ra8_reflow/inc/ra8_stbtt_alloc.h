/**
 * @file ra8_stbtt_alloc.h
 * @brief Heap-free scratch allocator for stb_truetype (glyph rasterise).
 * @ingroup grp_ereader
 *
 * @details
 * stb_truetype allocates short-lived scratch (glyph vertices, rasteriser
 * edge/point lists) through `STBTT_malloc` / `STBTT_free` for *every*
 * glyph -- on both the one-shot `stbtt_GetCodepointBitmap()` path and the
 * two-step `stbtt_GetCodepointBitmapBox()` + `stbtt_MakeCodepointBitmap()`
 * path. The firmware has no heap (`_sbrk` traps after init), so the build
 * redirects those macros to the functions below (see the CMake
 * `STBTT_malloc` / `STBTT_free` compile definitions on the
 * `stb_truetype_impl.c` source).
 *
 * The allocator is a fixed-capacity bump arena with reference-counted
 * auto-reset: each successful `ra8_stbtt_malloc()` bumps an offset and a
 * live-block count; each `ra8_stbtt_free()` decrements the count and, when
 * it reaches zero, rewinds the offset to the base. Every top-level
 * stb_truetype call frees all of its scratch before returning, so the
 * arena fully drains after each glyph -- no caller cooperation, robust to
 * any free order (stb frees composite-glyph buffers out of LIFO order),
 * and immune to fragmentation. On exhaustion `ra8_stbtt_malloc()` returns
 * NULL; stb_truetype tolerates this and the offending glyph is skipped
 * rather than corrupting memory.
 *
 * NASA Power-of-10 Rule 3 (no dynamic allocation after init): the backing
 * store is a single file-scope array.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * [Ring 4 / Reflow] {World: NS}
 *
 * @since 0.1.0
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

/**
 * @brief Allocate `n` bytes of glyph scratch from the static arena.
 *
 * @details Bumps the arena offset by `n` rounded up to the 16-byte
 * alignment and increments the live-block count. Requests larger than the
 * arena, or that do not fit the remaining capacity, fail with nullptr.
 *
 * @param[in] n Byte count requested by stb_truetype.
 * @return 16-byte-aligned pointer into the arena, or nullptr if the
 *         request does not fit the remaining capacity.
 * @retval nullptr The request exceeds the arena or remaining capacity.
 * @pre The arena is in a consistent state (statically zero-initialised).
 * @pre Called single-threaded from the rasteriser.
 * @post On success the live-block count is incremented and the offset
 *       advances by the aligned size.
 * @post On failure the arena state is unchanged.
 * @note Not thread-safe: rasterisation is single-threaded.
 * @since 0.1.0
 */
void* ra8_stbtt_malloc(size_t n);

/**
 * @brief Release a block previously returned by ra8_stbtt_malloc().
 *
 * @details Decrements the live-block count; the bump offset is not tracked
 * per block. When the count reaches zero the arena is fully drained, so
 * the offset rewinds to the base. A nullptr argument is ignored.
 *
 * @param[in] p Pointer to release; nullptr is ignored.
 * @return None.
 * @pre `p` was returned by ra8_stbtt_malloc(), or is nullptr.
 * @pre Called single-threaded from the rasteriser.
 * @post The live-block count is decremented (never below zero).
 * @post When the live-block count reaches zero the arena offset rewinds
 *       to the base (full auto-reset).
 * @note Not thread-safe: rasterisation is single-threaded.
 * @since 0.1.0
 */
void ra8_stbtt_free(void* p);

/**
 * @brief Worst-case arena high-water mark observed since boot, in bytes.
 *
 * @details Diagnostic hook for sizing/regression: lets a host test or an
 * on-target probe confirm the configured capacity has headroom.
 *
 * @return Peak number of simultaneously-bumped bytes since boot.
 * @retval 0 No allocation has been made yet.
 * @pre None beyond a consistent arena state.
 * @pre Called single-threaded from the rasteriser.
 * @post The arena state is left unchanged (pure read).
 * @post The returned value never decreases across calls.
 * @note Not thread-safe: rasterisation is single-threaded.
 * @since 0.1.0
 */
size_t ra8_stbtt_alloc_high_water(void);
