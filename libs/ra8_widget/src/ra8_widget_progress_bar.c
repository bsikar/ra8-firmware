/**
 * @file ra8_widget_progress_bar.c
 * @brief Progress-bar leaf widget for the ra8_widget tree model (#145 Phase 2).
 *
 * @details
 * Extracts the e-reader footer / book-card progress indicator into a reusable
 * ::ra8_widget leaf: a track fill plus a proportional `value / total` fill,
 * painted through the injected ::ra8_widget_paint_t backend so the file carries
 * no `ra8_gfx` dependency. The fill-fraction maths (::internal_pb_fill_w) clamps
 * `value` into `[0, total]` and guards a zero `total` / zero-width rect, so a bar
 * never divides by zero and never overflows its rect. Display-only: the vtable
 * leaves `on_input` NULL.
 *
 *
 * [Ring 5 / UI]
 * {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "ra8_widget_progress_bar.h"

#include "ra8_check.h"
#include "ra8_widget.h"
#include "ra8_widget_internal.h"

/** @brief Logging / check tag. */
static const char* s_tag = "ra8_widget_progress_bar";

/**
 * @brief Empty-bar sentinel: at or below this fill width, no fill is painted.
 */
typedef enum : int32_t {
  k_ra8_widget_pb_empty = 0, /**< No filled pixels (0% or degenerate rect). */
} ra8_widget_pb_geom_t;

/**
 * @brief Progress-bar vtable render: fill the track, then the proportional fill.
 * @details Reads the ::ra8_widget_progress_bar_t from `w->ctx`, fills the whole
 *          rect with `track`, then -- when the fill fraction is non-zero -- fills
 *          the leftmost ::internal_pb_fill_w pixels with `fill`. A missing paint
 *          backend / `fill_rect` callback is a safe no-op.
 * @param[in] w The progress-bar widget (its `ctx` is a ::ra8_widget_progress_bar_t).
 * @return Nothing.
 * @pre @p w is non-NULL (guaranteed by ::ra8_widget_render_dirty).
 * @pre @p w->ctx points at a valid bar descriptor or is NULL.
 * @post At most a track fill plus one proportional fill are issued.
 * @post No pixels are touched when the bar has no paint backend.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_pb_render(ra8_widget_t* w)
{
  ra8_widget_progress_bar_t* pb = (ra8_widget_progress_bar_t*)w->ctx;
  if (pb == nullptr) {
    return;
  }
  if (pb->paint == nullptr) {
    return;
  }
  if (pb->paint->fill_rect == nullptr) {
    return;
  }
  const ra8_ui_rect_t* r = &w->rect;
  pb->paint->fill_rect(pb->paint->user, r->x, r->y, r->w, r->h, pb->track);
  const int32_t fw = priv_widget_fill_frac(pb->value, pb->total, r->w);
  if (fw > (int32_t)k_ra8_widget_pb_empty) {
    pb->paint->fill_rect(pb->paint->user, r->x, r->y, fw, r->h, pb->fill);
  }
}

/** @brief The single immutable vtable shared by every progress bar. */
static const ra8_widget_vtable_t s_pb_vt = {
  .measure  = nullptr,
  .render   = internal_pb_render,
  .on_input = nullptr,
};

const ra8_widget_vtable_t* ra8_widget_progress_bar_vtable(void)
{
  return &s_pb_vt;
}

[[nodiscard]] ra8_err_t ra8_widget_progress_bar_init(ra8_widget_t*              w,
                                                     ra8_widget_progress_bar_t* bar)
{
  RA8_CHECK_NULL_PTR(w, s_tag, "w must not be nullptr");
  RA8_CHECK_NULL_PTR(bar, s_tag, "bar must not be nullptr");
  w->vt      = ra8_widget_progress_bar_vtable();
  w->ctx     = bar;
  w->visible = true;
  return k_ra8_ok;
}
