/**
 * @file ra8_widget_reflow_view.c
 * @brief Reflowed-reading-body leaf widget for the ra8_widget tree (#145 Phase 2).
 *
 * @details
 * Extracts the e-reader Reading body into a reusable ::ra8_widget leaf that owns
 * the paging state + tap routing while reaching the `ra8_reflow` engine through
 * the injected ::ra8_widget_reflow_ops_t seam. That seam is what keeps the file
 * (and the whole `ra8_widget` library) free of `ra8_reflow` / `ra8_gfx`: the pure
 * page-turn clamping (::internal_rv_turn / ::internal_rv_clamp) and link routing
 * are host-testable with a mock seam; the pixel work lives in the app-bound
 * callbacks.
 *
 *
 * [Ring 5 / UI]
 * {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "ra8_widget_reflow_view.h"

#include "ra8_check.h"
#include "ra8_ui.h"
#include "ra8_widget.h"
#include "ra8_widget_internal.h"

/** @brief Logging / check tag. */
static const char* s_tag = "ra8_widget_reflow_view";

/**
 * @enum ra8_widget_rv_geom_t
 * @brief Paging constants (no magic numbers).
 */
typedef enum : uint16_t {
  k_ra8_widget_rv_first = 0U, /**< First page index / empty book. */
  k_ra8_widget_rv_one   = 1U, /**< One-page step / minimum count. */
} ra8_widget_rv_geom_t;

/**
 * @brief Body rectangle: the widget rect inset by the view margins.
 * @details Shrinks @p rect by @p mx horizontally and @p my vertically so the
 *          reflowed page paints inside the margins rather than against the band
 *          edge. The seam's `render_page` receives this rect.
 * @param[in] rect The widget rect (non-NULL).
 * @param[in] mx   Horizontal inset, pixels.
 * @param[in] my   Vertical inset, pixels.
 * @return The inset body rectangle.
 * @retval body The inset body rect.
 * @pre @p rect is non-NULL.
 * @pre None.
 * @post The returned rect lies within @p rect.
 * @post No state is modified.
 * @note Pure.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_ui_rect_t internal_rv_body(const ra8_ui_rect_t* rect, int16_t mx, int16_t my)
{
  const int32_t x    = (int32_t)mx;
  const int32_t y    = (int32_t)my;
  ra8_ui_rect_t body = {};
  body.x             = rect->x + x;
  body.y             = rect->y + y;
  body.w             = rect->w - (x + x);
  body.h             = rect->h - (y + y);
  return body;
}

/**
 * @brief Clamp a page index into `[0, count - 1]`.
 * @details Guards a link destination the seam may return out of range: an empty
 *          book (`count == 0`) clamps to 0, a too-large page clamps to the last
 *          page, otherwise the page passes through unchanged.
 * @param[in] page  Candidate page.
 * @param[in] count Total pages.
 * @return @p page clamped into range (0 for an empty book).
 * @retval page The clamped page index.
 * @pre None.
 * @pre None.
 * @post The result is < @p count when @p count > 0, else 0.
 * @post No state is modified.
 * @note Pure.
 * @since 0.1.0
 */
RA8_INTERNAL
static uint16_t internal_rv_clamp(uint16_t page, uint16_t count)
{
  if (count == (uint16_t)k_ra8_widget_rv_first) {
    return (uint16_t)k_ra8_widget_rv_first;
  }
  if (page >= count) {
    return (uint16_t)(count - (uint16_t)k_ra8_widget_rv_one);
  }
  return page;
}

/**
 * @brief Turn the page by a tap side, clamped at both ends.
 * @details A tap left of @p mid steps back one page (a no-op on the first page);
 *          a tap at or right of @p mid steps forward one page (a no-op on the
 *          last page). Split out so ::internal_rv_on_input stays small and the
 *          boundary decisions are branch-coverable.
 * @param[in,out] v   Reflow-view descriptor (non-NULL).
 * @param[in]     px  Tap X (pixels).
 * @param[in]     mid Body-rect horizontal midpoint (pixels).
 * @return true if `v->page` changed, false at a boundary.
 * @retval true  The page stepped back or forward.
 * @retval false The tap was at a page boundary (no change).
 * @pre @p v is non-NULL.
 * @pre None.
 * @post `v->page` stays within `[0, page_count - 1]`.
 * @post Returns whether the page changed.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static bool internal_rv_turn(ra8_widget_reflow_view_t* v, int32_t px, int32_t mid)
{
  if (px < mid) {
    if (v->page == (uint16_t)k_ra8_widget_rv_first) {
      return false;
    }
    v->page = (uint16_t)(v->page - (uint16_t)k_ra8_widget_rv_one);
    return true;
  }
  if ((uint16_t)(v->page + (uint16_t)k_ra8_widget_rv_one) >= v->page_count) {
    return false;
  }
  v->page = (uint16_t)(v->page + (uint16_t)k_ra8_widget_rv_one);
  return true;
}

/**
 * @brief Reflow-view vtable render: clear the body, ask the seam to paint it.
 * @details Reads the ::ra8_widget_reflow_view_t from `w->ctx`, clears the widget
 *          rect to `bg` through the paint backend, then -- when the seam has a
 *          `render_page` callback -- asks it to paint the current page inside the
 *          body rect (the widget rect inset by the margins).
 * @param[in] w The reflow-view widget (its `ctx` is a ::ra8_widget_reflow_view_t).
 * @return Nothing.
 * @pre @p w is non-NULL (guaranteed by ::ra8_widget_render_dirty).
 * @pre @p w->ctx points at a valid reflow-view descriptor or is NULL.
 * @post At most a background clear plus one page paint are issued.
 * @post No page is painted when the seam is inert.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_rv_render(ra8_widget_t* w)
{
  ra8_widget_reflow_view_t* v = (ra8_widget_reflow_view_t*)w->ctx;
  if (v == nullptr) {
    return;
  }
  if (v->paint != nullptr) {
    priv_widget_fill_box(v->paint, &w->rect, v->bg, v->bg, (int16_t)0);
  }
  if ((v->ops == nullptr) || (v->ops->render_page == nullptr)) {
    return;
  }
  const ra8_ui_rect_t body = internal_rv_body(&w->rect, v->margin_x, v->margin_y);
  v->ops->render_page(v->ops->user, v->page, &body);
}

/**
 * @brief Try to follow a link at the tap through the seam.
 * @details When the seam supplies a `follow_link` callback and it reports a link
 *          was followed, adopts the (clamped) destination page and marks the view
 *          dirty for a full redraw. Split out so ::internal_rv_on_input stays
 *          small.
 * @param[in,out] w  The reflow-view widget (non-NULL).
 * @param[in]     v  The view descriptor (non-NULL).
 * @param[in]     ev The touch event (non-NULL).
 * @return true if a link was followed (page adopted), false otherwise.
 * @retval true  A link jump changed the page.
 * @retval false No seam / no link at the tap.
 * @pre @p w, @p v, @p ev are non-NULL.
 * @pre None.
 * @post On true `v->page` is the clamped link destination and `w` is dirty.
 * @post No state changes when no link is followed.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static bool
internal_rv_link(ra8_widget_t* w, ra8_widget_reflow_view_t* v, const ra8_widget_event_t* ev)
{
  if ((v->ops == nullptr) || (v->ops->follow_link == nullptr)) {
    return false;
  }
  uint16_t dest = v->page;
  if (!v->ops->follow_link(v->ops->user, v->page, ev->x, ev->y, &dest)) {
    return false;
  }
  v->page = internal_rv_clamp(dest, v->page_count);
  (void)ra8_widget_invalidate(w, k_ra8_widget_refresh_quality);
  return true;
}

/**
 * @brief Reflow-view vtable input: follow a link, else turn the page.
 * @details A touch is offered to the link seam first; if no link is followed it
 *          turns the page by the tap side (::internal_rv_turn), marking the view
 *          dirty for a quality redraw only when the page actually changed. Any
 *          body touch is consumed; a button event is declined.
 * @param[in] w  The reflow-view widget (its `ctx` is a ::ra8_widget_reflow_view_t).
 * @param[in] ev The event already routed to this view.
 * @return true if a body touch was consumed, false otherwise.
 * @retval true  @p ev was a touch (link-followed or page-turned or a no-op tap).
 * @retval false @p ev was not a touch (or `ctx` is NULL).
 * @pre @p w and @p ev are non-NULL.
 * @pre @p w->ctx points at a valid reflow-view descriptor or is NULL.
 * @post On a page change `w` is dirty with the quality hint.
 * @post No state changes on a non-touch event.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static bool internal_rv_on_input(ra8_widget_t* w, const ra8_widget_event_t* ev)
{
  ra8_widget_reflow_view_t* v = (ra8_widget_reflow_view_t*)w->ctx;
  if (v == nullptr) {
    return false;
  }
  if (ev->kind != k_ra8_widget_ev_touch) {
    return false;
  }
  if (internal_rv_link(w, v, ev)) {
    return true;
  }
  const int32_t mid = w->rect.x + (w->rect.w / 2);
  if (internal_rv_turn(v, ev->x, mid)) {
    (void)ra8_widget_invalidate(w, k_ra8_widget_refresh_quality);
  }
  return true;
}

/** @brief The single immutable vtable shared by every reflow view. */
static const ra8_widget_vtable_t s_rv_vt = {
  .measure  = nullptr,
  .render   = internal_rv_render,
  .on_input = internal_rv_on_input,
};

const ra8_widget_vtable_t* ra8_widget_reflow_view_vtable(void)
{
  return &s_rv_vt;
}

[[nodiscard]] ra8_err_t ra8_widget_reflow_view_init(ra8_widget_t* w, ra8_widget_reflow_view_t* view)
{
  RA8_CHECK_NULL_PTR(w, s_tag, "w must not be nullptr");
  RA8_CHECK_NULL_PTR(view, s_tag, "view must not be nullptr");
  w->vt      = ra8_widget_reflow_view_vtable();
  w->ctx     = view;
  w->visible = true;
  return k_ra8_ok;
}
