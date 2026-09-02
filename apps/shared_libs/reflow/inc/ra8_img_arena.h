/**
 * @file ra8_img_arena.h
 * @brief Heap-free scratch allocator hooks for the stb_image single-TU build.
 * @ingroup grp_ereader
 *
 * @details
 * stb_image decodes JPEG/PNG/GIF/BMP through `STBI_MALLOC` / `STBI_FREE` /
 * `STBI_REALLOC_SIZED`. The firmware has no heap (`_sbrk` traps after init),
 * so `stb_image_impl.c` redirects those macros to the functions below, which
 * bump-allocate out of a **caller-bound** arena (::ra8_img_arena_t). Unlike the
 * stb_truetype arena (a fixed file-scope array), an image decode's scratch
 * sizing spans three orders of magnitude -- a few KiB for a thumbnail, a few
 * MiB for a full-page cover -- so the backing store is supplied per call by
 * the consumer via ra8_img_arena_bind() rather than baked in here.
 *
 * The allocator is a bump arena with reference-counted auto-reset: each
 * ra8_img_arena_malloc() bumps `offset` and increments `live`; each
 * ra8_img_arena_free() decrements `live` and, when it reaches zero, rewinds
 * `offset` to the base. stb_image frees all of its scratch before returning,
 * so the arena fully drains after each decode -- no caller bookkeeping, robust
 * to any free order, immune to fragmentation. On exhaustion the malloc hook
 * returns nullptr; stb_image propagates that as a decode failure rather than
 * corrupting memory.
 *
 * NASA Power-of-10 Rule 3 (no dynamic allocation after init): the backing
 * store is caller-owned static/SRAM/SDRAM storage, never `malloc`.
 *
 *
 * [Ring 4 / Reflow] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */
#pragma once

#include <stddef.h>

#include "reflow_image.h" /* ra8_img_arena_t */

/**
 * @brief Bind @p arena as the active scratch for subsequent decode allocations.
 *
 * @details Records @p arena in a file-static slot and resets it to empty
 * (`offset = 0`, `live = 0`). Every ra8_img_arena_malloc() / _free() / _realloc
 * until the next ra8_img_arena_bind() or ra8_img_arena_unbind() operates on it.
 *
 * @param[in,out] arena Arena to make active; reset to empty on entry. NULL
 *                      unbinds (equivalent to ra8_img_arena_unbind()).
 * @return None.
 * @pre @p arena, if non-NULL, has `base` pointing at `cap` writable bytes.
 * @pre Called single-threaded around one decode (decoding is not re-entrant).
 * @post The active arena is @p arena and, if non-NULL, it is empty.
 * @post A subsequent ra8_img_arena_malloc() draws from @p arena.
 * @note Not thread-safe: image decoding is single-threaded.
 * @since 0.1.0
 */
void ra8_img_arena_bind(ra8_img_arena_t* arena);

/**
 * @brief Unbind the active arena; subsequent allocation hooks fail with nullptr.
 *
 * @details Clears the file-static active-arena slot. Used to fence the stb hooks
 * outside a decode so a stray allocation cannot scribble on a stale buffer.
 *
 * @return None.
 * @pre None.
 * @pre Called single-threaded around one decode.
 * @post The active-arena slot is NULL.
 * @post A subsequent ra8_img_arena_malloc() returns nullptr until the next bind.
 * @note Not thread-safe: image decoding is single-threaded.
 * @since 0.1.0
 */
void ra8_img_arena_unbind(void);

/**
 * @brief stb_image `STBI_MALLOC` hook: bump @p n bytes from the bound arena.
 *
 * @details Rounds @p n up to the 16-byte alignment, bumps the bound arena's
 * offset, and increments its live-block count. Fails with nullptr when no arena
 * is bound, when @p n exceeds the capacity, or when the request does not fit the
 * remaining capacity.
 *
 * @param[in] n Byte count requested by stb_image.
 * @return 16-byte-aligned pointer into the bound arena, or nullptr.
 * @retval nullptr No arena bound, or the request does not fit.
 * @pre Either an arena is bound (then a decode is in progress) or the call fails.
 * @pre Called single-threaded from the decoder.
 * @post On success the live-block count is incremented and the offset advances.
 * @post On failure the arena state is unchanged.
 * @note Not thread-safe: image decoding is single-threaded.
 * @since 0.1.0
 */
void* ra8_img_arena_malloc(size_t n);

/**
 * @brief stb_image `STBI_FREE` hook: release a block; auto-reset when none live.
 *
 * @details Decrements the bound arena's live-block count; the bump offset is not
 * tracked per block. When the count reaches zero the arena has fully drained, so
 * the offset rewinds to the base. A nullptr argument, or no bound arena, is
 * ignored.
 *
 * @param[in] p Pointer to release; nullptr is ignored.
 * @return None.
 * @pre @p p was returned by ra8_img_arena_malloc()/_realloc_sized(), or is nullptr.
 * @pre Called single-threaded from the decoder.
 * @post The live-block count is decremented (never below zero).
 * @post When the live-block count reaches zero the arena offset rewinds to base.
 * @note Not thread-safe: image decoding is single-threaded.
 * @since 0.1.0
 */
void ra8_img_arena_free(void* p);

/**
 * @brief stb_image `STBI_REALLOC_SIZED` hook: fresh-allocate, copy, free old.
 *
 * @details A bump arena cannot grow a block in place, so this allocates @p newsz
 * fresh, copies `min(oldsz, newsz)` bytes from @p p, and releases @p p. The old
 * block's space is not reclaimed until the arena next drains -- which is why the
 * caller sizes the arena for the decode's peak, not its net, footprint.
 *
 * @param[in] p     Existing block (may be nullptr -> behaves as malloc).
 * @param[in] oldsz Current size of @p p in bytes (0 if @p p is nullptr).
 * @param[in] newsz Requested new size in bytes.
 * @return Pointer to @p newsz bytes with the old contents copied, or nullptr.
 * @retval nullptr No arena bound, or the new request does not fit.
 * @pre @p p was returned by a prior hook call, or is nullptr.
 * @pre Called single-threaded from the decoder.
 * @post On success the first `min(oldsz, newsz)` bytes match @p p's old contents.
 * @post On failure @p p remains valid and the arena state is unchanged.
 * @note Not thread-safe: image decoding is single-threaded.
 * @since 0.1.0
 */
void* ra8_img_arena_realloc_sized(void* p, size_t oldsz, size_t newsz);
