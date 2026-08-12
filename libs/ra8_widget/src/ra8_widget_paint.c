/**
 * @file ra8_widget_paint.c
 * @brief Shared geometry helpers for the concrete leaf widgets (#145 Phase 2).
 *
 * @details
 * Holds the two `RA8_PRIV` helpers the text-label and push-button leaf widgets
 * both need: aligned text placement and a filled / bordered box. They compute
 * pixel geometry and dispatch through the injected ::ra8_widget_paint_t backend,
 * so -- like the rest of `ra8_widget` -- this file carries no `ra8_gfx`
 * dependency. The full contracts live on the declarations in
 * `ra8_widget_internal.h`; the definitions below are deliberately comment-free.
 *
 * Both helpers take an already-validated, non-NULL @p paint (the widget render
 * callbacks guard that before delegating here), so they keep only the guards a
 * caller can still vary -- the optional `text_size` / `fill_rect` callbacks and
 * the geometry selectors -- which keeps every branch reachable from the host
 * tests through the public render path.
 *
 *
 * [Ring 5 / UI]
 * {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "ra8_widget_internal.h"

RA8_PRIV void ra8_widget_priv_text_pos(const ra8_widget_paint_t* paint,
                                       const ra8_ui_rect_t*      rect,
                                       const char*               text,
                                       int16_t                   pad,
                                       ra8_widget_align_t        align,
                                       int32_t*                  out_x,
                                       int32_t*                  out_y)
{
  *out_x = rect->x + (int32_t)pad;
  *out_y = rect->y + (int32_t)pad;
  if (align == k_ra8_widget_align_left) {
    return;
  }
  if (paint->text_size == nullptr) {
    return;
  }
  int32_t tw = 0;
  int32_t th = 0;
  paint->text_size(paint->user, text, &tw, &th);
  *out_y = rect->y + ((rect->h - th) / 2);
  if (align == k_ra8_widget_align_right) {
    *out_x = (rect->x + rect->w) - (int32_t)pad - tw;
    return;
  }
  *out_x = rect->x + ((rect->w - tw) / 2);
}

RA8_PRIV void ra8_widget_priv_fill_box(const ra8_widget_paint_t* paint,
                                       const ra8_ui_rect_t*      rect,
                                       uint32_t                  fill,
                                       uint32_t                  border,
                                       int16_t                   border_w)
{
  if (paint->fill_rect == nullptr) {
    return;
  }
  if (border_w <= 0) {
    paint->fill_rect(paint->user, rect->x, rect->y, rect->w, rect->h, fill);
    return;
  }
  const int32_t bw = (int32_t)border_w;
  paint->fill_rect(paint->user, rect->x, rect->y, rect->w, rect->h, border);
  paint->fill_rect(paint->user,
                   rect->x + bw,
                   rect->y + bw,
                   rect->w - (bw + bw),
                   rect->h - (bw + bw),
                   fill);
}

/** @brief No filled pixels (0% or a degenerate rect). */
typedef enum : int32_t {
  k_ra8_widget_frac_empty = 0, /**< RA8 widget frac empty. */
} ra8_widget_frac_geom_t;

RA8_PRIV int32_t ra8_widget_priv_fill_frac(uint32_t value, uint32_t total, int32_t width)
{
  if (total == 0U) {
    return (int32_t)k_ra8_widget_frac_empty;
  }
  if (width <= (int32_t)k_ra8_widget_frac_empty) {
    return (int32_t)k_ra8_widget_frac_empty;
  }
  uint32_t v = value;
  if (v > total) {
    v = total;
  }
  return (int32_t)(((uint32_t)width * v) / total);
}
