/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_widget_toolbar.c
 * @brief Toolbar leaf widget for the ra8_widget tree model (#145 Phase 2).
 *
 * @details
 * Extracts the e-reader Library toolbar (a search field + a count chip) into a
 * reusable ::ra8_widget leaf. The search-field rect is computed once by
 * ::internal_tb_field_rect and shared between `render` (paint the bordered box +
 * hint) and `on_input` (hit-test a tap), so the tap target is exactly the drawn
 * field. Painting goes through the injected ::ra8_widget_paint_t backend, so the
 * file carries no `ra8_gfx` dependency and the whole thing is host-testable with
 * a recording mock paint.
 *
 *
 * [Ring 5 / UI]
 * {World: NS}
 *
 * @since 0.1.0
 */

#include "ra8_widget_toolbar.h"

#include "ra8_check.h"
#include "ra8_ui.h"
#include "ra8_widget.h"
#include "ra8_widget_internal.h"

/** @brief Logging / check tag. */
static const char* s_tag = "ra8_widget_toolbar";

/**
 * @brief Compute the search-field sub-rectangle inside a toolbar band.
 * @details The band is inset by `pad` on every side; the count chip reserves a
 *          fixed `count_w` (plus a `pad` gutter) on the right and the field fills
 *          the remaining width. Shared by render (draw the field) and on_input
 *          (hit-test a tap) so the drawn box and the tap target coincide.
 * @param[in] bar  Toolbar descriptor (non-NULL).
 * @param[in] band The toolbar widget rect (non-NULL).
 * @return The search-field rectangle (may have zero width on a narrow band).
 * @retval rect The inset field rectangle.
 * @pre @p bar and @p band are non-NULL.
 * @pre None.
 * @post The returned rect lies within @p band.
 * @post No state is modified.
 * @note Pure.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_ui_rect_t internal_tb_field_rect(const ra8_widget_toolbar_t* bar,
                                            const ra8_ui_rect_t*        band)
{
  const int32_t pad     = (int32_t)bar->pad;
  const int32_t chip    = (int32_t)bar->count_w;
  const int32_t field_w = band->w - (pad + pad) - chip - pad;
  ra8_ui_rect_t r       = {};
  r.x                   = band->x + pad;
  r.y                   = band->y + pad;
  r.w                   = (field_w > 0) ? field_w : 0;
  r.h                   = band->h - (pad + pad);
  return r;
}

/**
 * @brief Toolbar vtable render: band fill, bordered field + hint, count chip.
 * @details Reads the ::ra8_widget_toolbar_t from `w->ctx`, fills the band with
 *          `bg`, draws the `border`-framed search field with its left-aligned
 *          `hint`, then the right-aligned `count` chip -- each through the paint
 *          backend and only when a `draw_text` callback is present.
 * @param[in] w The toolbar widget (its `ctx` is a ::ra8_widget_toolbar_t).
 * @return Nothing.
 * @pre @p w is non-NULL (guaranteed by ::ra8_widget_render_dirty).
 * @pre @p w->ctx points at a valid toolbar descriptor or is NULL.
 * @post At most a band fill, a framed field, a hint draw and a count draw run.
 * @post No pixels are touched when the toolbar has no paint backend.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_tb_render(ra8_widget_t* w)
{
  ra8_widget_toolbar_t* bar = (ra8_widget_toolbar_t*)w->ctx;
  if (bar == nullptr) {
    return;
  }
  if (bar->paint == nullptr) {
    return;
  }
  const ra8_widget_paint_t* p     = bar->paint;
  const ra8_ui_rect_t*      band  = &w->rect;
  const ra8_ui_rect_t       field = internal_tb_field_rect(bar, band);
  ra8_widget_priv_fill_box(p, band, bar->bg, bar->bg, (int16_t)0);
  ra8_widget_priv_fill_box(p, &field, bar->field, bar->border, bar->border_w);
  if (p->draw_text == nullptr) {
    return;
  }
  if (bar->hint != nullptr) {
    int32_t hx = 0;
    int32_t hy = 0;
    ra8_widget_priv_text_pos(p, &field, bar->hint, bar->pad, k_ra8_widget_align_left, &hx, &hy);
    p->draw_text(p->user, hx, hy, bar->hint, bar->hint_fg, bar->field);
  }
  if (bar->count != nullptr) {
    int32_t cx = 0;
    int32_t cy = 0;
    ra8_widget_priv_text_pos(p, band, bar->count, bar->pad, k_ra8_widget_align_right, &cx, &cy);
    p->draw_text(p->user, cx, cy, bar->count, bar->count_fg, bar->bg);
  }
}

/**
 * @brief Toolbar vtable input: latch a touch inside the search field.
 * @details A touch that lands inside the search-field rect bumps `searches`,
 *          self-invalidates fast (so only this band re-flushes), fires the
 *          optional `on_search`, and reports the event consumed. A tap outside
 *          the field, or a button event, is declined so it keeps routing.
 * @param[in] w  The toolbar widget (its `ctx` is a ::ra8_widget_toolbar_t).
 * @param[in] ev The event already routed to this toolbar.
 * @return true if a search-field touch was latched, false otherwise.
 * @retval true  @p ev was a touch inside the field and it latched.
 * @retval false Non-touch, a tap outside the field, or `ctx` is NULL.
 * @pre @p w and @p ev are non-NULL.
 * @pre @p w->ctx points at a valid toolbar descriptor or is NULL.
 * @post On a field touch `searches` grew by one and `w` is dirty/fast.
 * @post No state changes on a declined event.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static bool internal_tb_on_input(ra8_widget_t* w, const ra8_widget_event_t* ev)
{
  ra8_widget_toolbar_t* bar = (ra8_widget_toolbar_t*)w->ctx;
  if (bar == nullptr) {
    return false;
  }
  if (ev->kind != k_ra8_widget_ev_touch) {
    return false;
  }
  const ra8_ui_rect_t field = internal_tb_field_rect(bar, &w->rect);
  if (!ra8_ui_rect_contains(&field, ev->x, ev->y)) {
    return false;
  }
  bar->searches++;
  (void)ra8_widget_invalidate(w, k_ra8_widget_refresh_fast);
  if (bar->on_search != nullptr) {
    bar->on_search(w);
  }
  return true;
}

/** @brief The single immutable vtable shared by every toolbar. */
static const ra8_widget_vtable_t s_tb_vt = {
  .measure  = nullptr,
  .render   = internal_tb_render,
  .on_input = internal_tb_on_input,
};

const ra8_widget_vtable_t* ra8_widget_toolbar_vtable(void)
{
  return &s_tb_vt;
}

[[nodiscard]] ra8_err_t ra8_widget_toolbar_init(ra8_widget_t* w, ra8_widget_toolbar_t* bar)
{
  RA8_CHECK_NULL_PTR(w, s_tag, "w must not be nullptr");
  RA8_CHECK_NULL_PTR(bar, s_tag, "bar must not be nullptr");
  w->vt      = ra8_widget_toolbar_vtable();
  w->ctx     = bar;
  w->visible = true;
  return k_ra8_ok;
}
