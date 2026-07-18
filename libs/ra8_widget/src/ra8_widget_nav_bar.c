/**
 * @file ra8_widget_nav_bar.c
 * @brief Navigation-strip leaf widget for the ra8_widget tree model (#145 Phase 2).
 *
 * @details
 * Extracts the e-reader bottom navigation strip into a reusable ::ra8_widget
 * leaf: `count` equal-width cells, each a centred label (active vs muted colour),
 * each a tap target. The cell geometry (::internal_nav_cell) is shared between
 * render (draw each label) and the hit maths (::internal_nav_hit), so a tap lands
 * in the cell it looks like it lands in. Painting goes through the injected
 * ::ra8_widget_paint_t backend, so the file carries no `ra8_gfx` dependency.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * [Ring 5 / UI]
 * {World: NS}
 *
 * @since 0.1.0
 */

#include "ra8_widget_nav_bar.h"

#include "ra8_check.h"
#include "ra8_ui.h"
#include "ra8_widget.h"
#include "ra8_widget_internal.h"

/** @brief Logging / check tag. */
static const char* s_tag = "ra8_widget_nav_bar";

/**
 * @brief Empty sentinel: an empty strip (no items) draws / routes nothing.
 */
typedef enum : uint16_t {
  k_ra8_widget_nav_empty = 0U, /**< A `count` of this many is a no-op. */
} ra8_widget_nav_geom_t;

/**
 * @brief Compute cell @p idx of a `count`-cell strip laid inside @p strip.
 * @details Splits @p strip into @p count equal cells using rounded prefix widths
 *          (`w*i/count`), so cells tile the strip with no gap and absorb the
 *          remainder deterministically. Shared by render and hit-testing.
 * @param[in] strip The nav-strip widget rect (non-NULL).
 * @param[in] idx   Cell index (`< count`).
 * @param[in] count Number of cells (>= 1).
 * @return The cell rectangle.
 * @retval rect The @p idx-th cell within @p strip.
 * @pre @p strip is non-NULL; @p count >= 1; @p idx < @p count.
 * @pre None.
 * @post The returned cell lies within @p strip.
 * @post No state is modified.
 * @note Pure.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_ui_rect_t internal_nav_cell(const ra8_ui_rect_t* strip, uint16_t idx, uint16_t count)
{
  const int32_t x0 = (strip->w * (int32_t)idx) / (int32_t)count;
  const int32_t x1 = (strip->w * (int32_t)(idx + 1U)) / (int32_t)count;
  ra8_ui_rect_t r  = {};
  r.x              = strip->x + x0;
  r.y              = strip->y;
  r.w              = x1 - x0;
  r.h              = strip->h;
  return r;
}

/**
 * @brief Draw one nav item's centred label in its cell.
 * @details Places @p text centred in @p cell via ::ra8_widget_priv_text_pos and
 *          draws it in @p fg over @p bg. A NULL @p text is a no-op.
 * @param[in] paint Draw backend (non-NULL; `draw_text` already checked non-NULL).
 * @param[in] cell  The item's cell rectangle (non-NULL).
 * @param[in] text  Item label, or NULL to skip.
 * @param[in] fg    Text colour, 0xRRGGBB.
 * @param[in] bg    Background colour, 0xRRGGBB.
 * @return Nothing.
 * @pre @p paint, @p cell are non-NULL; @p paint->draw_text is non-NULL.
 * @pre None.
 * @post At most one `draw_text` call is issued.
 * @post No draw call is issued when @p text is NULL.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_nav_item(const ra8_widget_paint_t* paint,
                              const ra8_ui_rect_t*      cell,
                              const char*               text,
                              uint32_t                  fg,
                              uint32_t                  bg)
{
  if (text == nullptr) {
    return;
  }
  int32_t tx = 0;
  int32_t ty = 0;
  ra8_widget_priv_text_pos(paint, cell, text, (int16_t)0, k_ra8_widget_align_center, &tx, &ty);
  paint->draw_text(paint->user, tx, ty, text, fg, bg);
}

/**
 * @brief Nav-strip vtable render: fill the strip, centre each cell's label.
 * @details Reads the ::ra8_widget_nav_bar_t from `w->ctx`, fills the strip with
 *          `bg`, then draws each of the `count` items centred in its cell with
 *          `fg_active` for the `active` index and `fg_muted` for the rest. An
 *          empty strip (`count == 0`) or a missing paint backend is a no-op.
 * @param[in] w The nav-strip widget (its `ctx` is a ::ra8_widget_nav_bar_t).
 * @return Nothing.
 * @pre @p w is non-NULL (guaranteed by ::ra8_widget_render_dirty).
 * @pre @p w->ctx points at a valid nav-strip descriptor or is NULL.
 * @post At most a strip fill plus `count` label draws are issued.
 * @post No pixels are touched when the strip has no paint backend.
 * @note Not thread-safe; loop bounded by `count` (NASA Rule 2).
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_nav_render(ra8_widget_t* w)
{
  ra8_widget_nav_bar_t* nav = (ra8_widget_nav_bar_t*)w->ctx;
  if (nav == nullptr) {
    return;
  }
  if (nav->paint == nullptr) {
    return;
  }
  const ra8_widget_paint_t* p     = nav->paint;
  const ra8_ui_rect_t*      strip = &w->rect;
  ra8_widget_priv_fill_box(p, strip, nav->bg, nav->bg, (int16_t)0);
  if (nav->count == (uint16_t)k_ra8_widget_nav_empty) {
    return;
  }
  if ((p->draw_text == nullptr) || (nav->items == nullptr)) {
    return;
  }
  for (uint16_t i = 0U; i < nav->count; ++i) {
    const ra8_ui_rect_t cell = internal_nav_cell(strip, i, nav->count);
    const uint32_t      fg   = (i == nav->active) ? nav->fg_active : nav->fg_muted;
    internal_nav_item(p, &cell, nav->items[i], fg, nav->bg);
  }
}

/**
 * @brief Map a touch X to a cell index within a `count`-cell strip.
 * @details Guards a degenerate strip (`count == 0` or `w <= 0`) and a tap outside
 *          the strip, then returns the proportional cell index clamped to the
 *          last cell. Split out so ::internal_nav_on_input stays small.
 * @param[in]  strip The nav-strip rect (non-NULL).
 * @param[in]  count Number of cells.
 * @param[in]  px    Touch X (pixels).
 * @param[out] out   Receives the hit cell index (only on a true return).
 * @return true if @p px lands on a cell, false otherwise.
 * @retval true  A cell was hit; `*out` holds its index.
 * @retval false Degenerate strip or a tap outside it.
 * @pre @p strip and @p out are non-NULL.
 * @pre None.
 * @post On true `*out < count` (the in-range guard makes `px - x < w`, so the
 *       proportional index is strictly below @p count -- no clamp needed).
 * @post No state is modified.
 * @note Pure.
 * @since 0.1.0
 */
RA8_INTERNAL
static bool internal_nav_hit(const ra8_ui_rect_t* strip, uint16_t count, int32_t px, uint16_t* out)
{
  if (count == (uint16_t)k_ra8_widget_nav_empty) {
    return false;
  }
  if (strip->w <= 0) {
    return false;
  }
  if ((px < strip->x) || (px >= (strip->x + strip->w))) {
    return false;
  }
  const int32_t idx = ((px - strip->x) * (int32_t)count) / strip->w;
  *out              = (uint16_t)idx;
  return true;
}

/**
 * @brief Nav-strip vtable input: route a touch to the cell it hit.
 * @details A touch inside a cell records it in `selected`, self-invalidates fast,
 *          fires the optional `on_select`, and reports the event consumed. A
 *          non-touch event or a tap outside the strip is declined.
 * @param[in] w  The nav-strip widget (its `ctx` is a ::ra8_widget_nav_bar_t).
 * @param[in] ev The event already routed to this strip.
 * @return true if a cell touch was routed, false otherwise.
 * @retval true  @p ev was a touch on a cell and it was routed.
 * @retval false Non-touch, a tap off the strip, or `ctx` is NULL.
 * @pre @p w and @p ev are non-NULL.
 * @pre @p w->ctx points at a valid nav-strip descriptor or is NULL.
 * @post On a cell touch `selected` holds the hit index and `w` is dirty/fast.
 * @post No state changes on a declined event.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static bool internal_nav_on_input(ra8_widget_t* w, const ra8_widget_event_t* ev)
{
  ra8_widget_nav_bar_t* nav = (ra8_widget_nav_bar_t*)w->ctx;
  if (nav == nullptr) {
    return false;
  }
  if (ev->kind != k_ra8_widget_ev_touch) {
    return false;
  }
  uint16_t idx = 0U;
  if (!internal_nav_hit(&w->rect, nav->count, ev->x, &idx)) {
    return false;
  }
  nav->selected = idx;
  (void)ra8_widget_invalidate(w, k_ra8_widget_refresh_fast);
  if (nav->on_select != nullptr) {
    nav->on_select(w, idx);
  }
  return true;
}

/** @brief The single immutable vtable shared by every nav strip. */
static const ra8_widget_vtable_t s_nav_vt = {
  .measure  = nullptr,
  .render   = internal_nav_render,
  .on_input = internal_nav_on_input,
};

const ra8_widget_vtable_t* ra8_widget_nav_bar_vtable(void)
{
  return &s_nav_vt;
}

[[nodiscard]] ra8_err_t ra8_widget_nav_bar_init(ra8_widget_t* w, ra8_widget_nav_bar_t* nav)
{
  RA8_CHECK_NULL_PTR(w, s_tag, "w must not be nullptr");
  RA8_CHECK_NULL_PTR(nav, s_tag, "nav must not be nullptr");
  w->vt      = ra8_widget_nav_bar_vtable();
  w->ctx     = nav;
  w->visible = true;
  return k_ra8_ok;
}
