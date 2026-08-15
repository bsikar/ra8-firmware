/**
 * @file ra8_epub_miniz_alloc.h
 * @brief Caller-owned bounded-arena allocator for miniz.
 * @ingroup grp_ereader
 *
 * @par Tag
 * [Ring 4 / Domain] {World: NS}
 *
 * @details
 * The RA8D2 firmware is deliberately zero-heap: the linker script defines no
 * `.heap` region and `_sbrk()` (``ra8_sbrk_trap.c``) traps any allocation, per
 * NASA Power of 10 Rule 3. But `ra8_epub` opens `.epub` archives through the
 * vendored **miniz** ZIP reader, whose ``mz_zip_reader_init_mem`` /
 * ``mz_zip_reader_extract_to_mem`` allocate their central-directory state and a
 * ~11 KiB ``tinfl_decompressor`` from the heap. Calling the default `malloc`
 * on the target hits the `_sbrk` trap and HardFaults.
 *
 * This module provides a self-contained first-fit free-list allocator over a
 * caller-owned workspace, exposing the three callbacks miniz needs on its
 * ``mz_zip_archive`` (``m_pAlloc`` / ``m_pFree`` / ``m_pRealloc``). Every
 * archive owns a separate arena and passes its descriptor through miniz's
 * ``m_pAlloc_opaque`` callback context. No hidden singleton or host-only heap
 * fallback exists.
 *
 * The free list coalesces adjacent free blocks on every free, so the pool is
 * reusable across an arbitrary number of `ra8_epub_load_chapter` calls (each
 * extract allocates then frees its decompressor) and across re-opens (closing a
 * book frees its central directory). No explicit reset is required.
 *
 * @note One arena is not thread-safe. Independent arenas do not share state and
 *       may back simultaneously-live EPUB and CBZ objects.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdalign.h>
#include <stddef.h>
#include <stdint.h>

#include "ra8_err.h"

/**
 * @enum ra8_epub_miniz_alloc_limits_t
 * @brief Static pool geometry for the miniz arena.
 *
 * @details
 * Sized for the peak live footprint of one open `.epub`. Two footprints share the
 * pool and must both fit alongside the central-directory arrays (which grow with
 * the archive's file count and live for the book's whole open):
 *
 *   - Whole-entry extract (`mz_zip_reader_extract_to_mem`, used by
 *     `ra8_epub_load_chapter` / `..._get_resource`): the ~11 KiB inflate
 *     decompressor plus, on the streamed path, a bounded compressed-read buffer
 *     (`MZ_ZIP_MAX_IO_BUF_SIZE`, 64 KiB).
 *   - Streaming entry cursor (`ra8_epub_entry_open`, #231, over
 *     `mz_zip_reader_extract_iter_*`): the iterator state (~8.4 KiB, embeds the
 *     inflator) plus -- for a DEFLATE entry -- a 32 KiB LZ dictionary window and
 *     the same 64 KiB compressed-read buffer. A *stored* (method-0) entry needs
 *     only the ~8.4 KiB iterator state (miniz reads it directly, no dict/IO bufs).
 *
 * The DEFLATE streaming cursor is therefore the peak: ~8.4 + 32 + 64 = ~105 KiB of
 * transient inflate state, on top of the resident central directory. 160 KiB gives
 * that peak room while staying tiny against the 1.6 MiB SRAM. A large in-content
 * image (a manga page) is the motivating #231 case and is streamed through this
 * cursor rather than materialised whole.
 */
typedef enum : uint32_t {
  k_ra8_epub_miniz_pool_bytes = 163840U, /**< Arena size, bytes (160 KiB). */
} ra8_epub_miniz_alloc_limits_t;

/**
 * @union ra8_epub_miniz_workspace_t
 * @brief Exactly-sized, maximally-aligned storage for one miniz arena.
 *
 * @details Embed this type in the object that owns a miniz archive. The union
 * gives the byte storage ``alignof(max_align_t)`` without an implementation
 * global. The bytes are private allocator state while the arena is live.
 */
typedef union {
  max_align_t align; /**< Establishes the required workspace alignment. */
  /** @brief Allocator backing bytes. */
  uint8_t bytes[k_ra8_epub_miniz_pool_bytes];
} ra8_epub_miniz_workspace_t;

/**
 * @struct ra8_epub_miniz_arena_t
 * @brief Descriptor for one caller-owned miniz allocation arena.
 *
 * @details Initialise with ::ra8_epub_miniz_arena_init, then pass this
 * descriptor as miniz's ``m_pAlloc_opaque``. Treat the fields as read-only.
 * Copying a live descriptor is forbidden because both copies would own the
 * same workspace.
 */
typedef struct {
  uint8_t* base;        /**< Aligned caller workspace, or NULL when inactive. */
  size_t   capacity;    /**< Usable bytes; exactly the documented pool size.  */
  uint8_t  initialized; /**< 1 after successful init; 0 after deinit.         */
} ra8_epub_miniz_arena_t;

/**
 * @brief Initialise or reset a miniz arena over caller-owned storage.
 *
 * @param[out] arena           Descriptor to initialise.
 * @param[in,out] workspace    Maximally-aligned backing storage.
 * @param[in] workspace_bytes  Writable bytes at @p workspace.
 * @retval k_ra8_ok Workspace accepted and reset to one free block.
 * @retval k_ra8_err_null_ptr @p arena or @p workspace is NULL.
 * @retval k_ra8_err_invalid_size Fewer than
 *         ::k_ra8_epub_miniz_pool_bytes bytes were supplied.
 * @retval k_ra8_err_invalid_arg @p workspace is not aligned for
 *         ``max_align_t``.
 * @pre No allocation from @p arena is live when reinitialising it.
 * @post On success exactly ::k_ra8_epub_miniz_pool_bytes are bound.
 * @post On failure @p arena and @p workspace are unchanged.
 * @note One arena is not thread-safe; distinct arenas are independent.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_epub_miniz_arena_init(ra8_epub_miniz_arena_t* arena, void* workspace, size_t workspace_bytes);

/**
 * @brief Invalidate an arena descriptor after miniz has released its blocks.
 * @param[in,out] arena Arena to invalidate; NULL is accepted.
 * @pre All allocations from @p arena have been released.
 * @post A non-NULL descriptor is zeroed; workspace bytes are unchanged.
 * @note Reinitialise with ::ra8_epub_miniz_arena_init before reuse.
 * @since 0.1.0
 */
void ra8_epub_miniz_arena_deinit(ra8_epub_miniz_arena_t* arena);

/**
 * @brief miniz-compatible allocator (``mz_alloc_func``).
 *
 * @details
 * Returns a block of at least ``items * size`` bytes from @p opaque's arena,
 * aligned to ``max_align_t``. First-fit over the free list; splits an
 * oversized free block when the remainder can hold a header plus a minimum
 * payload. Returns NULL on overflow of ``items * size`` or pool exhaustion.
 *
 * @param[in] opaque Pointer to an initialised ::ra8_epub_miniz_arena_t.
 * @param[in] items  Element count.
 * @param[in] size   Element size, bytes.
 *
 * @return Pointer to an aligned block, or NULL on overflow / exhaustion.
 *
 * @pre `items * size` does not overflow `size_t` (checked; NULL if it does).
 * @pre @p opaque names a live arena (NULL is rejected).
 * @pre The arena has a free block large enough (else NULL).
 * @post On success the returned block is marked in-use and is `>=` requested.
 * @post On failure the pool is unchanged.
 *
 * @note One arena is not thread-safe; distinct arenas are independent.
 * @since 0.1.0
 */
void* ra8_epub_miniz_alloc(void* opaque, size_t items, size_t size);

/**
 * @brief miniz-compatible free (``mz_free_func``).
 *
 * @details
 * Marks @p address's block free and coalesces it with adjacent free blocks.
 * A NULL @p address, invalid arena, or pointer outside that arena is ignored.
 *
 * @param[in] opaque  Pointer to the owning ::ra8_epub_miniz_arena_t.
 * @param[in] address Block previously returned by ::ra8_epub_miniz_alloc /
 *                    ::ra8_epub_miniz_realloc, or NULL.
 *
 * @pre @p address is NULL or a live block from this allocator.
 * @pre @p opaque is the same live arena that allocated @p address.
 * @post @p address's block is free and merged with any free neighbours.
 * @post A NULL / out-of-pool @p address leaves the pool unchanged.
 *
 * @note One arena is not thread-safe; distinct arenas are independent.
 * @since 0.1.0
 */
void ra8_epub_miniz_free(void* opaque, void* address);

/**
 * @brief miniz-compatible realloc (``mz_realloc_func``).
 *
 * @details
 * Grows or shrinks @p address to at least ``items * size`` bytes. Keeps the
 * block in place when it already fits; otherwise allocates a new block, copies
 * the old payload, and frees the old block. ``realloc(NULL, n)`` allocates;
 * ``realloc(p, 0)`` frees and returns NULL.
 *
 * @param[in] opaque  Pointer to the owning ::ra8_epub_miniz_arena_t.
 * @param[in] address Existing block, or NULL.
 * @param[in] items   New element count.
 * @param[in] size    New element size, bytes.
 *
 * @return Pointer to a block of at least the requested size, or NULL on
 *         overflow / exhaustion (in which case @p address remains valid).
 *
 * @pre @p opaque names a live arena.
 * @pre @p address is NULL or a live block from that arena.
 * @pre `items * size` does not overflow (checked).
 * @post On a moved grow, the old payload bytes are preserved in the new block.
 * @post On NULL-return for a non-NULL @p address, that block is still valid.
 *
 * @note One arena is not thread-safe; distinct arenas are independent.
 * @since 0.1.0
 */
void* ra8_epub_miniz_realloc(void* opaque, void* address, size_t items, size_t size);

#ifdef __cplusplus
}
#endif
