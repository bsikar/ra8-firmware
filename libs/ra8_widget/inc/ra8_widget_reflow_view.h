/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_widget_reflow_view.h
 * @brief Reflowed-reading-body leaf widget for the ra8_widget tree (#145 Phase 2).
 * @ingroup grp_ereader
 *
 * @details
 * The e-reader Reading screen's body is a paginated block of reflowed text drawn
 * by `ra8_reflow`, with page turns on a left / right tap and `<a href>` links
 * that jump pages. ::ra8_widget_reflow_view_t wraps that as a reusable
 * ::ra8_widget leaf that owns the **view state** (current page, page count, body
 * margins) and the tap routing (link-follow, then page-turn with boundary
 * clamping), while the `ra8_reflow` engine itself is reached through an injected
 * ::ra8_widget_reflow_ops_t seam.
 *
 * That seam is what keeps this widget -- and the whole `ra8_widget` library --
 * free of `ra8_reflow` (which pulls in `ra8_gfx`, stb, and the glyph atlas): the
 * app binds `render_page` to `ra8_reflow_render_page_at` and `follow_link` to
 * `ra8_reflow_hit_test_link` + href navigation, so the pixel-touching and
 * engine-specific work lives on-target in the app, and the pure page-state /
 * routing logic is host-testable with a recording mock seam.
 *
 *
 * [Ring 5 / UI]
 * {World: NS}
 *
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_ui.h"
#include "ra8_widget.h"

/**
 * @struct ra8_widget_reflow_ops_t
 * @brief Injected seam driving a reflow engine from the reflow-view widget.
 *
 * @details
 * A Dependency-Inversion seam (like ::ra8_widget_paint_t) that keeps
 * `ra8_widget` free of any `ra8_reflow` / `ra8_gfx` dependency: the widget calls
 * these callbacks, the app binds them to the real engine. `render_page` paints
 * the given page inside the body rectangle; `follow_link` hit-tests a tap for a
 * link and, if one is followed, reports the destination page. Either callback may
 * be NULL (a view with no engine renders nothing / follows no links).
 *
 * @invariant `follow_link`, when it returns true, writes `*out_page`.
 *
 * @see ra8_widget_reflow_view_t
 * @since 0.1.0
 */
typedef struct ra8_widget_reflow_ops {
  /** @brief Opaque engine handle passed back to every callback below. */
  void* user;
  /** @brief Paint page @p page of the engine inside @p body (on-target only). */
  void (*render_page)(void* user, uint16_t page, const ra8_ui_rect_t* body);
  /**
   * @brief Hit-test a tap for a link on @p page; follow it if present.
   * @return true and set `*out_page` if a link was followed, else false.
   */
  bool (*follow_link)(void* user, uint16_t page, int32_t x, int32_t y, uint16_t* out_page);
} ra8_widget_reflow_ops_t;

/**
 * @struct ra8_widget_reflow_view_t
 * @brief A paginated reflowed-text body: page state + margins + engine seam.
 *
 * @details
 * Caller-owned plain data bound to a ::ra8_widget_t with
 * ::ra8_widget_reflow_view_init. `render` clears the widget rect to `bg` (through
 * the injected ::ra8_widget_paint_t) and asks the seam to paint `page` inside the
 * body rect (the widget rect inset by `margin_x` / `margin_y`). `on_input` routes
 * a touch: it offers the tap to `follow_link` first (a link jump adopts the
 * returned page); otherwise it turns the page -- a tap on the left half steps
 * back, the right half steps forward, clamped at page 0 and the last page. A body
 * tap is always consumed.
 *
 * @invariant `page < page_count` (clamped on every mutation).
 * @invariant `page_count >= 1` for a non-empty book.
 *
 * @par Example:
 * @code
 * ra8_widget_reflow_ops_t ops = {.user = &engine,
 *                                .render_page = paint_page,
 *                                .follow_link = tap_link};
 * ra8_widget_reflow_view_t rv = {.paint = &paint, .ops = &ops, .bg = 0x00FFFFFFU,
 *                                .page = 0, .page_count = 12,
 *                                .margin_x = 24, .margin_y = 8};
 * ra8_widget_t w = {};
 * (void)ra8_widget_reflow_view_init(&w, &rv);
 * @endcode
 *
 * @see ra8_widget_reflow_view_init
 * @see ra8_widget_reflow_ops_t
 * @since 0.1.0
 */
typedef struct ra8_widget_reflow_view {
  const ra8_widget_paint_t*      paint;      /**< Background-clear backend (NULL -> no clear). */
  const ra8_widget_reflow_ops_t* ops;        /**< Reflow engine seam (NULL -> inert).          */
  uint32_t                       bg;         /**< Body background colour, 0xRRGGBB.            */
  uint16_t                       page;       /**< Current page (0-based).                      */
  uint16_t                       page_count; /**< Total pages (>= 1).                          */
  int16_t                        margin_x;   /**< Horizontal body inset, pixels.               */
  int16_t                        margin_y;   /**< Vertical body inset, pixels.                 */
} ra8_widget_reflow_view_t;

/**
 * @brief Return the shared vtable backing every reflow-view widget.
 *
 * @details
 * One immutable vtable serves all reflow views: `render` clears the body and asks
 * the seam to paint the current page; `on_input` follows a link or turns the
 * page; `measure` is NULL. ::ra8_widget_reflow_view_init binds it.
 *
 * @return Non-NULL pointer to the static reflow-view vtable.
 *
 * @pre None.
 * @pre None.
 * @post The returned pointer is non-NULL and references static storage.
 * @post No state is modified.
 *
 * @note Pure; thread-safe (returns a pointer to immutable static data).
 * @see ra8_widget_reflow_view_init
 * @since 0.1.0
 */
const ra8_widget_vtable_t* ra8_widget_reflow_view_vtable(void);

/**
 * @brief Bind a widget instance to a reflow view.
 *
 * @details
 * Wires @p w to render + route @p view: sets its vtable to
 * ::ra8_widget_reflow_view_vtable, points its `ctx` at @p view, and makes it
 * visible. The caller still sets @p w's `fixed` / `flex` for its parent's layout.
 *
 * @param[in,out] w    Widget to turn into a reflow view (non-NULL).
 * @param[in]     view Reflow-view descriptor (non-NULL; outlives @p w's use).
 *
 * @return ra8_err_t
 * @retval k_ra8_ok           Bound; @p w renders + routes @p view.
 * @retval k_ra8_err_null_ptr @p w or @p view is NULL.
 *
 * @pre @p w and @p view are non-NULL.
 * @pre @p view outlives every render / dispatch of @p w.
 * @post On success `w->vt == ra8_widget_reflow_view_vtable()`, `w->ctx == view`,
 *       `w->visible == true`.
 * @post On failure @p w is left unchanged.
 *
 * @note Not thread-safe.
 * @see ra8_widget_reflow_view_vtable
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_widget_reflow_view_init(ra8_widget_t*             w,
                                                    ra8_widget_reflow_view_t* view);

#ifdef __cplusplus
}
#endif
