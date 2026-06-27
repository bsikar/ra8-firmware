/**
 * @file ra_widget_label.c
 * @brief Text-label leaf widget for the ra_widget tree model (#145 Phase 2).
 *
 * @details
 * The simplest concrete leaf the panel compositor can composite: a background
 * fill plus an aligned string. Its `render` paints through the injected
 * ::ra_widget_paint_t backend (so the file stays free of any `ra_gfx`
 * dependency) and its vtable leaves `on_input` NULL because a label is purely
 * display -- it never consumes a touch. The geometry is delegated to the shared
 * `RA_PRIV` helpers in `ra_widget_paint.c`, so a label is just "fill the box,
 * place the text". Binding a widget with ::ra_widget_label_init makes it a
 * label that drops into any panel's child array.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * [Ring 5 / UI]
 * {World: NS}
 *
 * @since 0.1.0
 */

#include "ra_check.h"
#include "ra_widget.h"
#include "ra_widget_internal.h"

/** @brief Logging / check tag. */
static const char* s_tag = "ra_widget_label";

/**
 * @brief No border on a plain label -- a single background fill.
 * @details Passed as the `border_w` to ::ra_widget_priv_fill_box so the label
 *          paints exactly one fill (no frame), unlike the bordered button.
 */
typedef enum : int16_t {
  k_ra_widget_label_no_border = 0,
} ra_widget_label_geom_t;

/**
 * @brief Label vtable render: fill the background, then draw the aligned text.
 * @details Reads the ::ra_widget_label_t from `w->ctx`, fills the rect with
 *          `bg`, then -- when there is text and a `draw_text` backend -- places
 *          it via ::ra_widget_priv_text_pos and draws it. Each guard is a
 *          single condition so a missing backend / text is a safe no-op.
 * @param[in] w The label widget (its `ctx` is a ::ra_widget_label_t).
 * @return Nothing.
 * @pre @p w is non-NULL (guaranteed by ::ra_widget_render_dirty).
 * @pre @p w->ctx points at a valid label descriptor or is NULL.
 * @post At most the background fill plus one text draw are issued.
 * @post No pixels are touched when the label has no paint backend.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void internal_label_render(ra_widget_t* w)
{
  ra_widget_label_t* l = (ra_widget_label_t*)w->ctx;
  if (l == nullptr) {
    return;
  }
  if (l->paint == nullptr) {
    return;
  }
  ra_widget_priv_fill_box(l->paint, &w->rect, l->bg, l->bg, (int16_t)k_ra_widget_label_no_border);
  if (l->text == nullptr) {
    return;
  }
  if (l->paint->draw_text == nullptr) {
    return;
  }
  int32_t tx = 0;
  int32_t ty = 0;
  ra_widget_priv_text_pos(l->paint, &w->rect, l->text, l->pad, l->align, &tx, &ty);
  l->paint->draw_text(l->paint->user, tx, ty, l->text, l->fg, l->bg);
}

/** @brief The single immutable vtable shared by every text label. */
static const ra_widget_vtable_t s_label_vt = {
  .measure  = nullptr,
  .render   = internal_label_render,
  .on_input = nullptr,
};

const ra_widget_vtable_t* ra_widget_label_vtable(void)
{
  return &s_label_vt;
}

[[nodiscard]] ra_err_t ra_widget_label_init(ra_widget_t* w, ra_widget_label_t* label)
{
  RA_CHECK_NULL_PTR(w, s_tag, "w must not be nullptr");
  RA_CHECK_NULL_PTR(label, s_tag, "label must not be nullptr");
  w->vt      = ra_widget_label_vtable();
  w->ctx     = label;
  w->visible = true;
  return k_ra_ok;
}
