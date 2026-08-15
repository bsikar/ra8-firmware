/**
 * @file ra8_zoom_render.c
 * @brief Strip composite for the tap-to-zoom viewer: magnify, re-dither, blit (#478).
 *
 * @details
 * The render half of ra8_zoom.h. It never holds the visible window: a 1024x600
 * gray8 window would be 600 KiB and cannot be allocated (NASA P10 Rule 3), so the
 * viewport is composited in horizontal strips of at most
 * ::ra8_zoom_view_t::strip_rows destination rows and the resident cost is
 * `O(dst.w * strip_rows)`.
 *
 * Each strip goes through the same three steps:
 *
 *  1. **Magnify.** Nearest-neighbour replication straight into the destination
 *     grid, one source row read per *distinct* source row (consecutive
 *     destination rows at zoom `n` share a source row, and the row buffer is
 *     reused rather than re-faulted). Uncovered pixels are filled with
 *     ::k_ra8_zoom_bg_gray, so the caller never pre-clears the viewport.
 *  2. **Re-dither at the destination resolution**, not the source's. The issue
 *     asks for the retained gray8 to be "re-dithered at the current zoom": if the
 *     source were dithered and then magnified, a 4x view would show 4x4 blue-noise
 *     blobs. Dithering after magnification keeps the grain one panel pixel wide at
 *     every zoom. The mask phase is the *magnified-plane* coordinate, so a given
 *     image pixel keeps its threshold as the viewport moves -- panning does not
 *     re-roll the grain (see the ra8_zoom.h docblock).
 *  3. **Blit** the packed 4 bpp strip through ::ra8_gfx_blit_gray4_zoom at native
 *     scale. The magnification has already happened, so this call is a
 *     nibble-unpack and clipped store -- the same tested primitive the shelf loupe
 *     uses, rather than a second pixel path.
 *
 * Every operation is integer arithmetic over source bytes and a `const` mask, so
 * a framebuffer hash of the result is identical on the unit-test host, in
 * ra8_emulator and on silicon.
 *
 *
 * [Ring 4 / Domain] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_gfx.h"
#include "ra8_gfx_dither.h"
#include "ra8_zoom.h"
#include "ra8_zoom_internal.h"

/** @brief Component tag for `RA8_CHECK_*` log lines. */
static const char* const s_tag = "ra8_zoom_render";

/**
 * @enum zoom_blit_t
 * @brief Fixed arguments of the per-strip packed-gray4 blit.
 *
 * @details The strip is already at destination resolution when it reaches
 *          ::ra8_gfx_blit_gray4_zoom, so the blit magnifies by
 *          @ref k_zoom_blit_native (1) and takes the whole strip from its origin.
 *          Naming them keeps the call self-describing and keeps the "no magic
 *          numbers" rule honest at the one place a reader would otherwise have to
 *          count arguments.
 *
 * @invariant k_zoom_blit_native == 1: this module owns the magnification itself.
 *
 * @par Example:
 * @code
 * (void)ra8_gfx_blit_gray4_zoom(packed, w, rows, k_zoom_blit_origin, k_zoom_blit_origin,
 *                               w, rows, k_zoom_blit_native, x, y);
 * @endcode
 *
 * @see ra8_zoom_view_render
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_zoom_blit_origin = 0U, /**< Sub-rect origin inside the packed strip.         */
  k_zoom_blit_native = 1U, /**< Blit magnification (the strip is already sized). */
} zoom_blit_t;

/**
 * @brief Replicate one source row across the covered destination columns.
 *
 * @details The horizontal half of the nearest-neighbour magnify: destination
 *          offset `d` shows plane column `anchor_x + d`, hence source column
 *          `(anchor_x + d) / scale`, which is an index into the row buffer once
 *          the axis' own @c s0 is subtracted. Columns outside `[d0, d1)` are
 *          letterbox and are left at the background fill the caller wrote.
 *
 * @param[in]  v    Open view (supplies the anchor, scale and row buffer).
 * @param[in]  ax   Resolved horizontal axis for this frame.
 * @param[out] drow One destination strip row, at least `v->dst.w` bytes.
 *
 * @return Nothing.
 *
 * @pre  `v->scratch.row` holds source columns `[ax->s0, ax->s0 + ax->count)`.
 * @pre  @p drow addresses at least `v->dst.w` writable bytes.
 * @post Every destination offset in `[ax->d0, ax->d1)` holds its source sample.
 * @post No offset outside that half-open span is written.
 *
 * @note Not thread-safe (writes @p drow); pure with respect to @p v.
 * @see priv_zoom_axis
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_expand_row(const ra8_zoom_view_t* v, const ra8_zoom_axis_t* ax, uint8_t* drow)
{
  const int32_t scale = (int32_t)v->scale;
  /* The whole index calculation stays signed and the only conversion is of the
   * axis' own simple `s0` -- a cast of the composite `(anchor + d) / scale` to
   * an unsigned type would change essential type mid-expression (MISRA 10.8)
   * for no benefit: every value here is non-negative by construction. */
  const int32_t base = (int32_t)ax->s0;
  for (int32_t d = ax->d0; d < ax->d1; ++d) {
    const int32_t si = ((v->anchor_x + d) / scale) - base;
    drow[d]          = v->scratch.row[si];
  }
}

/**
 * @brief Composite one destination row: decide coverage, fault, replicate.
 *
 * @details Where a destination row decides what it is. A row above the image,
 *          below it, or on an axis with no horizontal coverage is letterbox and
 *          returns having written nothing -- the caller has already filled it
 *          with the background. A covered row reads its source row only when it
 *          differs from the one already in the row buffer, which at zoom `n` is
 *          once per `n` destination rows and never twice across a strip
 *          boundary, since @p cached_sy carries over.
 *
 * @param[in,out] v         Open view (its row scratch is written).
 * @param[in]     ax        Resolved horizontal axis for this frame.
 * @param[in]     plane_y   Magnified-plane row this destination row shows.
 * @param[out]    drow      The destination strip row, pre-filled with background.
 * @param[in,out] cached_sy Source row currently held in `v->scratch.row`, or -1.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok    The row is composited, or is letterbox and left alone.
 * @retval k_ra8_err_* Propagated verbatim from the source reader.
 *
 * @pre  @p v is open and its scratch capacities were validated at open.
 * @pre  @p drow holds at least `v->dst.w` writable bytes.
 * @post On k_ra8_ok every covered offset of @p drow holds its source sample.
 * @post On failure `*cached_sy` is -1, so a retry cannot reuse a partial row.
 *
 * @note Not thread-safe.
 * @see internal_expand_row
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_fill_row(ra8_zoom_view_t*       v,
                                   const ra8_zoom_axis_t* ax,
                                   int32_t                plane_y,
                                   uint8_t*               drow,
                                   int32_t*               cached_sy)
{
  const int32_t scale  = (int32_t)v->scale;
  const int32_t height = (int32_t)v->src.height;
  const int32_t sy     = (plane_y >= 0) ? (plane_y / scale) : -1;
  /* Decision: this destination row shows image only if the plane row lies
   * inside the image. The horizontal span is NOT re-tested here -- it is
   * constant across the frame and ::internal_fill_strip settles it once, which
   * keeps this decision to two conditions that are each independently
   * reachable rather than three where one never varies. */
  const bool covered = (sy >= 0) && (sy < height);
  if (!covered) {
    return k_ra8_ok;
  }
  if (sy != *cached_sy) {
    const ra8_err_t rc =
      v->src.read(v->src.ctx, ax->s0, (uint32_t)sy, ax->count, 1U, v->scratch.row, ax->count);
    if (rc != k_ra8_ok) {
      /* Name the row. The caller only learns that the frame was abandoned, and
       * on a paged source which source row faulted IS the diagnostic. */
      ra8_log_error_val(s_tag, "source read failed at row", (uint32_t)sy);
      *cached_sy = -1;
      return rc;
    }
    *cached_sy = sy;
  }
  internal_expand_row(v, ax, drow);
  return k_ra8_ok;
}

/**
 * @brief Build one gray8 destination strip: background fill, then per-row composite.
 *
 * @details The background fill is unconditional and covers the whole strip in
 *          one pass, so a letterbox row needs no second visit and the caller
 *          never has to pre-clear the viewport. The horizontal-coverage test is
 *          settled here, once per strip, rather than per row: it is constant
 *          across the frame, and a zero-width row would violate the source
 *          seam's `w > 0` contract if it ever reached ::ra8_zoom_read_fn.
 *          Everything else is ::internal_fill_row's decision.
 *
 * @param[in,out] v         Open view (its row/strip scratch is written).
 * @param[in]     ax        Resolved horizontal axis for this frame.
 * @param[in]     top       Destination row of the strip's first line.
 * @param[in]     rows      Rows in this strip (`>= 1`).
 * @param[in,out] cached_sy Source row currently held in `v->scratch.row`, or -1.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok    The strip holds `rows` composited destination lines.
 * @retval k_ra8_err_* Propagated verbatim from the source reader.
 *
 * @pre  @p v is open and its scratch capacities were validated at open.
 * @pre  `top + rows <= v->dst.h`.
 * @post On k_ra8_ok `v->scratch.strip[0 .. rows*dst.w)` is fully defined.
 * @post On failure the strip is partially written and must not be blitted.
 *
 * @note Not thread-safe.
 * @see internal_fill_row
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_fill_strip(ra8_zoom_view_t*       v,
                                     const ra8_zoom_axis_t* ax,
                                     int32_t                top,
                                     int32_t                rows,
                                     int32_t*               cached_sy)
{
  const size_t stride = (size_t)v->dst.w;
  (void)memset(v->scratch.strip, (int)k_ra8_zoom_bg_gray, (size_t)rows * stride);
  if (ax->count == 0U) {
    /* The image misses the viewport horizontally, so every row is letterbox and
     * the background fill above is the whole answer. */
    return k_ra8_ok;
  }
  for (int32_t r = 0; r < rows; ++r) {
    uint8_t* drow = &v->scratch.strip[(size_t)r * stride];
    RA8_RETURN_ON_ERROR(internal_fill_row(v, ax, v->anchor_y + top + r, drow, cached_sy),
                        s_tag,
                        "strip row");
  }
  return k_ra8_ok;
}

/**
 * @brief One strip, end to end: magnify, re-dither at plane phase, blit.
 *
 * @details The three-step pipeline the file docblock describes, kept together
 *          so ::ra8_zoom_view_render is only the strip loop. The dither phase is
 *          the magnified-plane origin of this strip, which is what makes the
 *          grain belong to the image rather than to the panel.
 *
 * @param[in,out] v         Open view.
 * @param[in]     ax        Resolved horizontal axis for this frame.
 * @param[in]     top       Destination row of the strip's first line.
 * @param[in]     rows      Rows in this strip (`>= 1`).
 * @param[in,out] cached_sy Source row currently held in `v->scratch.row`, or -1.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok    The strip is on the framebuffer.
 * @retval k_ra8_err_* Propagated verbatim from the source reader or ra8_gfx.
 *
 * @pre  ::ra8_gfx_init has bound a framebuffer.
 * @pre  `top + rows <= v->dst.h`.
 * @post On k_ra8_ok destination rows `[top, top + rows)` of `v->dst` are painted.
 * @post On failure nothing past the failing step was written.
 *
 * @note Not thread-safe; writes the single ra8_gfx framebuffer binding.
 * @see internal_fill_strip
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_emit_strip(ra8_zoom_view_t*       v,
                                     const ra8_zoom_axis_t* ax,
                                     int32_t                top,
                                     int32_t                rows,
                                     int32_t*               cached_sy)
{
  RA8_RETURN_ON_ERROR(internal_fill_strip(v, ax, top, rows, cached_sy), s_tag, "strip fill");
  uint32_t packed_len = 0U;
  RA8_RETURN_ON_ERROR(ra8_gfx_dither_gray8_to_gray4(v->scratch.strip,
                                                    v->dst.w,
                                                    rows,
                                                    v->anchor_x,
                                                    v->anchor_y + top,
                                                    v->scratch.packed,
                                                    v->scratch.packed_cap,
                                                    &packed_len),
                      s_tag,
                      "strip dither");
  return ra8_gfx_blit_gray4_zoom(v->scratch.packed,
                                 v->dst.w,
                                 rows,
                                 (int32_t)k_zoom_blit_origin,
                                 (int32_t)k_zoom_blit_origin,
                                 v->dst.w,
                                 rows,
                                 (int32_t)k_zoom_blit_native,
                                 v->dst.x,
                                 v->dst.y + top);
}

/**
 * @brief Whether a view is in a state that can be composited at all.
 *
 * @details Split out of ::ra8_zoom_view_render so the render body is the strip
 *          loop and nothing else. The strip-height check is not defensive
 *          padding: it is the loop bound, and a zero would make the strip loop
 *          fail to advance (NASA P10 Rule 2).
 *
 * @param[in] v Candidate view.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                The view can be rendered.
 * @retval k_ra8_err_null_ptr      @p v is NULL, or its source seam is.
 * @retval k_ra8_err_invalid_state @p v is closed, or was never opened properly.
 *
 * @pre  None -- NULL is an accepted input and is reported.
 * @pre  The caller has not yet touched the framebuffer.
 * @post No state is modified.
 * @post On k_ra8_ok `v->strip_rows >= 1`, so the strip loop terminates.
 *
 * @note Not thread-safe (logs).
 * @see ra8_zoom_view_render
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_render_ready(const ra8_zoom_view_t* v)
{
  RA8_CHECK_NULL_PTR(v, s_tag, "view must not be nullptr");
  RA8_CHECK_NULL_PTR(v->src.read, s_tag, "view source seam must not be nullptr");
  if (!v->active) {
    ra8_log_error(s_tag, "render on a closed view");
    return k_ra8_err_invalid_state;
  }
  if (v->strip_rows == 0U) {
    ra8_log_error(s_tag, "view has no strip height; it was not opened properly");
    return k_ra8_err_invalid_state;
  }
  return k_ra8_ok;
}

ra8_err_t ra8_zoom_view_render(ra8_zoom_view_t* v)
{
  RA8_RETURN_ON_ERROR(internal_render_ready(v), s_tag, "render preconditions");

  ra8_zoom_axis_t ax = {};
  priv_zoom_axis(v->anchor_x, v->dst.w, (int32_t)v->scale, (int32_t)v->src.width, &ax);

  const int32_t rows_max  = (int32_t)v->strip_rows;
  int32_t       cached_sy = -1;
  for (int32_t top = 0; top < v->dst.h; top += rows_max) {
    int32_t rows = v->dst.h - top;
    if (rows > rows_max) {
      rows = rows_max;
    }
    RA8_RETURN_ON_ERROR(internal_emit_strip(v, &ax, top, rows, &cached_sy), s_tag, "strip emit");
  }
  return k_ra8_ok;
}
