/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_zoom_internal.h
 * @brief Module-private seam shared by the ra8_zoom state machine and its render.
 *
 * @details
 * ra8_zoom.c owns the viewport state and ra8_zoom_render.c owns the strip
 * composite, but both have to answer exactly the same geometry question --
 * "given this anchor, viewport extent and magnification, which destination
 * columns are covered and which source columns feed them?" -- and answering it
 * twice is how a render and a residency report drift apart. It is answered once,
 * here, by ::ra8_zoom_priv_axis.
 *
 * Nothing in this header is public API. It is not installed, it is not reachable
 * from `inc/`, and production code outside `libs/ra8_zoom` must never call it.
 *
 *
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

#include "ra8_attributes.h"

/**
 * @struct ra8_zoom_axis_t
 * @brief One axis of the viewport resolved against the image: coverage + source span.
 *
 * @details
 * All four members describe the same axis at once, because they are derived
 * together and are only consistent together. @c d0 / @c d1 are offsets *within*
 * the viewport (not framebuffer coordinates), half-open, and the region outside
 * them is letterbox. @c s0 / @c count name the source indices that feed
 * `[d0, d1)`, which is exactly the residency footprint of one frame on this axis.
 *
 * @invariant `0 <= d0 <= d1 <= extent`.
 * @invariant `count == 0` exactly when `d0 == d1` (the image misses the viewport).
 * @invariant When `count > 0`, `s0 + count <= src_dim`.
 *
 * @par Example:
 * @code
 * ra8_zoom_axis_t ax = {};
 * ra8_zoom_priv_axis(v->anchor_x, v->dst.w, (int32_t)v->scale, (int32_t)v->src.width, &ax);
 * @endcode
 *
 * @see ra8_zoom_priv_axis
 * @since 0.1.0
 */
typedef struct {
  int32_t  d0;    /**< First covered destination offset inside the viewport. */
  int32_t  d1;    /**< One past the last covered destination offset.         */
  uint32_t s0;    /**< First source index feeding the covered span.          */
  uint32_t count; /**< Source indices feeding the covered span (0 if none).  */
} ra8_zoom_axis_t;

/**
 * @brief Resolve one viewport axis against the image at the current magnification.
 *
 * @details
 * The single definition of the viewer's geometry. Destination offset @c d maps to
 * magnified-plane coordinate `anchor + d` and therefore to source index
 * `(anchor + d) / scale`; the covered span is where that plane coordinate lies
 * inside `[0, src_dim * scale)`. Both the render (which columns to fill, which
 * source row segment to read) and ::ra8_zoom_view_window (which source pixels are
 * live) are views onto this one result. The division is only ever performed on a
 * non-negative plane coordinate, so it never depends on the implementation of
 * signed truncation.
 *
 * @param[in]  anchor  Magnified-plane coordinate shown at viewport offset 0.
 * @param[in]  extent  Viewport extent on this axis, destination pixels (`> 0`).
 * @param[in]  scale   Integer magnification (`>= 1`).
 * @param[in]  src_dim Source extent on this axis, pixels (`>= 1`).
 * @param[out] out     Receives the resolved axis.
 *
 * @return Nothing; @p out is always fully populated.
 *
 * @pre  @p out is non-NULL (callers are inside this module and hold storage).
 * @pre  `scale >= 1` and `src_dim * scale` fits in int32 (guaranteed by
 *       ::k_ra8_zoom_dim_max and ::k_ra8_zoom_scale_max).
 * @post Every invariant of ::ra8_zoom_axis_t holds.
 * @post No input is modified.
 *
 * @note Pure; thread-safe.
 * @see ra8_zoom_axis_t
 * @since 0.1.0
 */
RA8_PRIV
void ra8_zoom_priv_axis(int32_t          anchor,
                        int32_t          extent,
                        int32_t          scale,
                        int32_t          src_dim,
                        ra8_zoom_axis_t* out);
