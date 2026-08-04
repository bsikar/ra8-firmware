/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_epub_miniz_alloc.h
 * @brief Static-arena allocator for miniz on the zero-heap firmware target.
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
 * This module provides a self-contained first-fit free-list allocator over one
 * statically-sized pool, exposing the three callbacks miniz needs on its
 * ``mz_zip_archive`` (``m_pAlloc`` / ``m_pFree`` / ``m_pRealloc``). It is a
 * documented Rule 3 deviation -- the same class as tinyxml2's static `MemPoolT`
 * arena (``ra8_epub_xml_shim.cpp``), `ra8_img_arena`, and `ra8_stbtt_alloc`.
 *
 * The free list coalesces adjacent free blocks on every free, so the pool is
 * reusable across an arbitrary number of `ra8_epub_load_chapter` calls (each
 * extract allocates then frees its decompressor) and across re-opens (closing a
 * book frees its central directory). No explicit reset is required.
 *
 * @note Not thread-safe: one shared static pool, single-book usage. `ra8_epub`
 *       drives a single archive at a time (file-scope OPF scratch), matching
 *       this constraint.
 * @note On the host unit-test build (``RA8_OFF_TARGET``) `ra8_epub` leaves
 *       miniz on its default `malloc`; these callbacks are wired only into the
 *       firmware build. They are still compiled and unit-tested everywhere.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

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
 * @brief miniz-compatible allocator (``mz_alloc_func``).
 *
 * @details
 * Returns a block of at least ``items * size`` bytes from the static pool,
 * aligned to ``max_align_t``. First-fit over the free list; splits an
 * oversized free block when the remainder can hold a header plus a minimum
 * payload. Returns NULL on overflow of ``items * size`` or pool exhaustion.
 *
 * @param[in] opaque Unused (miniz's ``m_pAlloc_opaque``); pass NULL.
 * @param[in] items  Element count.
 * @param[in] size   Element size, bytes.
 *
 * @return Pointer to an aligned block, or NULL on overflow / exhaustion.
 *
 * @pre `items * size` does not overflow `size_t` (checked; NULL if it does).
 * @pre The pool has a free block large enough (else NULL).
 * @post On success the returned block is marked in-use and is `>=` requested.
 * @post On failure the pool is unchanged.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
void* ra8_epub_miniz_alloc(void* opaque, size_t items, size_t size);

/**
 * @brief miniz-compatible free (``mz_free_func``).
 *
 * @details
 * Marks @p address's block free and coalesces it with adjacent free blocks.
 * A NULL @p address or a pointer outside the pool is ignored.
 *
 * @param[in] opaque  Unused; pass NULL.
 * @param[in] address Block previously returned by ::ra8_epub_miniz_alloc /
 *                    ::ra8_epub_miniz_realloc, or NULL.
 *
 * @pre @p address is NULL or a live block from this allocator.
 * @pre The pool is initialised (implied by a prior alloc).
 * @post @p address's block is free and merged with any free neighbours.
 * @post A NULL / out-of-pool @p address leaves the pool unchanged.
 *
 * @note Not thread-safe.
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
 * @param[in] opaque  Unused; pass NULL.
 * @param[in] address Existing block, or NULL.
 * @param[in] items   New element count.
 * @param[in] size    New element size, bytes.
 *
 * @return Pointer to a block of at least the requested size, or NULL on
 *         overflow / exhaustion (in which case @p address remains valid).
 *
 * @pre @p address is NULL or a live block from this allocator.
 * @pre `items * size` does not overflow (checked).
 * @post On a moved grow, the old payload bytes are preserved in the new block.
 * @post On NULL-return for a non-NULL @p address, that block is still valid.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
void* ra8_epub_miniz_realloc(void* opaque, void* address, size_t items, size_t size);

#ifdef __cplusplus
}
#endif
