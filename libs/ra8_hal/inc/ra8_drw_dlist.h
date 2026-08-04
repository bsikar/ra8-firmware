/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_drw_dlist.h
 * @brief 2D Drawing Engine (DRW / D/AVE 2D) display-list builder -- public API
 * @ingroup grp_hal_display
 *
 * @details
 * A display list lets the DRW clear and repaint a framebuffer end to end with
 * no CPU framebuffer writes (issue #247). This header carries the caller-side
 * builder API that composes such a list into a caller-owned word buffer:
 * ::ra8_drw_dlist_begin binds the builder, ::ra8_drw_dlist_add_fill appends
 * solid-rectangle primitives, ::ra8_drw_dlist_end terminates the list, and
 * ::ra8_drw_dlist_run triggers it. The build calls never touch MMIO; only
 * ::ra8_drw_dlist_run writes DLISTSTART (via ::ra8_drw_run_dlist in ra8_drw.h).
 *
 * The builder state type ::ra8_drw_dlist_t and the low-level trigger
 * ::ra8_drw_run_dlist live in ra8_drw.h, which this header includes; the split
 * is organizational (ra8_drw.h stays under the 1000-line cap) and changes no
 * public symbol or contract.
 *
 * @note Not thread-safe; one builder per thread.
 * @see ra8_drw.h
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_drw.h"
#include "ra8_err.h"

/**
 * @brief Bind a display-list builder to a caller-owned word buffer.
 *
 * @details
 * Resets the builder to empty and records the target buffer. The buffer must
 * be 4-byte aligned and live in a region the DRW bus initiator can read
 * (SRAM). No words are written yet.
 *
 * @param[out] dl        Builder to initialize.
 * @param[in]  buf       4-byte-aligned word buffer in DRW-readable memory.
 * @param[in]  cap_words Capacity of @p buf in 32-bit words (>= 1).
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Builder bound and cleared.
 * @retval k_ra8_err_null_ptr ``dl`` or ``buf`` was nullptr.
 * @retval k_ra8_err_invalid_arg ``buf`` not 4-byte aligned or ``cap_words`` 0.
 *
 * @pre @p dl and @p buf are non-null.
 * @pre @p buf is 4-byte aligned and @p cap_words >= 1.
 * @post ``dl->count == 0`` and ``dl->overflow == false``.
 * @post ``dl->buf == buf`` and ``dl->terminated == false``.
 *
 * @note Not thread-safe; one builder per thread.
 * @see ra8_drw_dlist_add_fill
 * @see ra8_drw_dlist_end
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_drw_dlist_begin(ra8_drw_dlist_t* dl, uint32_t* buf, uint32_t cap_words);

/**
 * @brief Append one solid-rectangle fill primitive to a display list.
 *
 * @details
 * Emits the display-list entries the DRW needs to paint an axis-aligned solid
 * rectangle -- COLOR1, SIZE, CONTROL (limiters off, the bounding-box scan is
 * the rectangle, HUM Ch 62.6.2 p 3716) and ORIGIN, the write that triggers the
 * render -- followed by a "wait for pipeline and cache" word so the primitive
 * fully drains before the next one begins. This is the same geometry the
 * register-mode ::ra8_drw_fill_rect uses; the surface format (CONTROL2, PITCH,
 * cache) stays as ::ra8_drw_init programmed it. A full-surface rectangle with
 * colour 0 is how a display list clears the framebuffer.
 *
 * @param[in,out] dl   Bound builder (::ra8_drw_dlist_begin succeeded).
 * @param[in]     rect Rectangle in pixels; ``color_argb8888`` is 0xAARRGGBB.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Primitive appended.
 * @retval k_ra8_err_null_ptr ``dl`` or ``rect`` was nullptr.
 * @retval k_ra8_err_invalid_arg Dimensions out of range or off the surface.
 * @retval k_ra8_err_no_mem The buffer cannot hold this primitive.
 *
 * @pre @p dl was bound by ::ra8_drw_dlist_begin and not yet ended.
 * @pre ``rect->width_px`` / ``height_px`` in [1..1024]; the rect lies on-surface.
 * @post On success ``dl->count`` grew by the primitive's word span.
 * @post On overflow ``dl->overflow`` is true and no partial entry was written.
 *
 * @note Not thread-safe; writes only the caller buffer, never MMIO.
 * @see ra8_drw_dlist_end
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_drw_dlist_add_fill(ra8_drw_dlist_t* dl, const ra8_drw_rect_t* rect);

/**
 * @brief Close a display list with its terminator word.
 *
 * @details
 * Appends the end-of-list marker so the DLR stops fetching after the last
 * primitive. Must be called before ::ra8_drw_dlist_run.
 *
 * @param[in,out] dl Bound builder holding at least one primitive.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Terminator appended; the list is runnable.
 * @retval k_ra8_err_null_ptr ``dl`` was nullptr.
 * @retval k_ra8_err_no_mem The buffer cannot hold the terminator.
 * @retval k_ra8_err_invalid_arg The builder already overflowed.
 *
 * @pre @p dl is non-null and was bound by ::ra8_drw_dlist_begin.
 * @pre @p dl has not overflowed.
 * @post ``dl->terminated == true`` on success.
 * @post ``dl->count`` grew by one word.
 *
 * @note Not thread-safe.
 * @see ra8_drw_dlist_run
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_drw_dlist_end(ra8_drw_dlist_t* dl);

/**
 * @brief Trigger a built display list (writes DLISTSTART).
 *
 * @details
 * Convenience wrapper over ::ra8_drw_run_dlist that first checks the builder
 * closed cleanly. It flushes the DRW caches and writes DLISTSTART with the
 * buffer address; the DLR then clears and repaints the framebuffer with no CPU
 * framebuffer writes. Poll ::ra8_drw_wait_idle (or STATUS.DLISTACTIVE) for
 * completion before reading the framebuffer.
 *
 * @param[in] dl Terminated builder (::ra8_drw_dlist_end succeeded).
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Display list triggered.
 * @retval k_ra8_err_null_ptr ``dl`` was nullptr.
 * @retval k_ra8_err_invalid_arg Builder overflowed, empty or not terminated.
 *
 * @pre @p dl is non-null, terminated and did not overflow.
 * @pre The DRW is initialized and idle.
 * @post DLISTSTART = ``dl->buf``; the engine begins fetching.
 * @post On completion STATUS.DLISTIRQ is raised.
 *
 * @note Not thread-safe; writes MMIO.
 * @see ra8_drw_dlist_add_fill
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_drw_dlist_run(const ra8_drw_dlist_t* dl);

#ifdef __cplusplus
}
#endif
