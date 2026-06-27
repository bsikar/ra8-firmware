/**
 * @file ra_widget.h
 * @brief Zero-heap composable widget layer (dwm-style) over ra_box + ra_ui.
 *
 * @details
 * `ra_widget` is the composable-UI layer issue #145 asks for: render
 * *everything* as widgets the way a tiling WM (dwm) composes a screen from
 * opt-in pieces. A status bar is a widget; a book view is a widget; an
 * on-screen keyboard is a widget. A screen (or an app, issue #146) is just an
 * array of widgets laid out by a container.
 *
 * The split that keeps this testable on the host AND zero-heap on the RA8D2
 * (NASA Rule 3):
 *
 *   - **Layout** is delegated to `ra_box` (the bounded box-model engine):
 *     children stack in a row / column with fixed-or-flex main-axis sizing,
 *     padding and gap. Invisible widgets are skipped (dwm-style opt-in).
 *   - **Hit routing** is delegated to `ra_ui` (`ra_ui_rect_contains`): a touch
 *     is routed to the visible widget whose rect contains it.
 *   - **Damage** is a union of the dirty widgets' rects plus a refresh hint
 *     (fast/A2 for text, quality/GC16 for full redraws) -- the minimal
 *     `display_pal` flush the compositor must issue.
 *   - **Render** is a per-widget vtable callback, so this library carries **no**
 *     `ra_gfx` / framebuffer dependency: the layout/route/damage logic is pure
 *     and unit-tested on the host; the concrete pixel drawing lives in the
 *     widget's `render` callback (exercised on-target).
 *
 * Everything is caller-owned plain data (static widget + box arrays, no
 * globals, no allocation), so the same composition runs identically on the
 * host test harness and on the board.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
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

#include "ra_box.h"
#include "ra_err.h"
#include "ra_ui.h"

/* ===========================================================================
 * Events + refresh hints
 * ===========================================================================
 */

/**
 * @enum ra_widget_ev_kind_t
 * @brief Input event kind routed to widgets.
 *
 * @details
 * Selects how ::ra_widget_dispatch routes an event: a `touch` is delivered to
 * the single widget whose rect contains `(x, y)`, while a `button` is offered
 * to each visible widget in order until one consumes it.
 */
typedef enum : uint8_t {
  k_ra_widget_ev_touch  = 0U, /**< A touch / tap at (x, y).             */
  k_ra_widget_ev_button = 1U, /**< A physical button press (button_id). */
} ra_widget_ev_kind_t;

/**
 * @struct ra_widget_event_t
 * @brief One input event delivered to ::ra_widget_dispatch.
 */
typedef struct {
  ra_widget_ev_kind_t kind;      /**< Touch or button.                       */
  uint8_t             reserved;  /**< Padding.                               */
  uint16_t            button_id; /**< Button id (for k_ra_widget_ev_button). */
  int32_t             x;         /**< Touch X (for k_ra_widget_ev_touch).    */
  int32_t             y;         /**< Touch Y (for k_ra_widget_ev_touch).    */
} ra_widget_event_t;

/**
 * @enum ra_widget_refresh_t
 * @brief E-ink-style refresh hint carried by a dirty widget.
 *
 * @details
 * Mirrors `ra_display_pal`'s `display_refresh_hint_t`. A widget that changed
 * only text/glyphs marks `fast` (A2 partial update, no flash); a widget that
 * redrew its whole area marks `quality` (GC16, ghost-free). The compositor
 * folds the dirty widgets' hints into the *strongest* one for the flush.
 *
 * @note Hint strength is ordered `none < fast < quality`; folding keeps the
 *       highest value present across the dirty widgets.
 */
typedef enum : uint8_t {
  k_ra_widget_refresh_none    = 0U, /**< Clean -- not dirty.              */
  k_ra_widget_refresh_fast    = 1U, /**< Partial / A2 (text-only change). */
  k_ra_widget_refresh_quality = 2U, /**< Full / GC16 (whole-area redraw). */
} ra_widget_refresh_t;

/* ===========================================================================
 * Widget + vtable
 * ===========================================================================
 */

struct ra_widget; /* fwd */

/**
 * @struct ra_widget_vtable_t
 * @brief Behaviour table shared by all widgets of one kind.
 *
 * @details
 * `measure` and `on_input` are **pure** (no framebuffer) so the compositor's
 * layout + routing logic is host-testable. `render` is the only callback that
 * touches pixels; it is invoked on-target and may be NULL for a widget that
 * draws nothing (a pure spacer / hit area).
 */
typedef struct {
  /**
   * @brief Report the widget's desired size within an available box.
   * @param[in]  w      The widget instance.
   * @param[in]  avail_w Available width (pixels).
   * @param[in]  avail_h Available height (pixels).
   * @param[out] out_w  Desired width (clamped by the caller).
   * @param[out] out_h  Desired height (clamped by the caller).
   */
  void (*measure)(struct ra_widget* w,
                  int32_t           avail_w,
                  int32_t           avail_h,
                  int32_t*          out_w,
                  int32_t*          out_h);
  /**
   * @brief Draw the widget within its current `rect` (on-target only).
   * @param[in] w The widget instance. NULL is a no-op (spacer).
   */
  void (*render)(struct ra_widget* w);
  /**
   * @brief Handle an input event.
   * @param[in] w  The widget instance.
   * @param[in] ev The event (already routed to this widget).
   * @return true if the widget consumed the event, false to keep routing.
   */
  bool (*on_input)(struct ra_widget* w, const ra_widget_event_t* ev);
} ra_widget_vtable_t;

/**
 * @struct ra_widget_t
 * @brief One widget instance (caller-owned, no allocation).
 *
 * @details
 * `fixed` / `flex` drive the container layout the same way `ra_box` does:
 * `fixed > 0` pins the main-axis extent, otherwise `flex` weight splits the
 * leftover. `visible == false` removes the widget from layout / routing /
 * render entirely (dwm opt-in). `dirty` + `refresh` track pending damage.
 */
typedef struct ra_widget {
  const ra_widget_vtable_t* vt;        /**< Behaviour table (non-NULL).     */
  void*                     ctx;       /**< Widget-specific state.          */
  ra_ui_rect_t              rect;      /**< Current bounds (layout output). */
  int16_t                   fixed;     /**< Fixed main-axis extent, or 0.   */
  uint16_t                  flex;      /**< Flex weight when not fixed.     */
  uint16_t                  action_id; /**< Hit-routing id (0 = none).      */
  uint8_t                   refresh;   /**< ra_widget_refresh_t (pending).  */
  bool                      visible;   /**< Laid out / drawn when true.     */
  bool                      dirty;     /**< Needs a redraw + flush.         */
} ra_widget_t;

/**
 * @enum ra_widget_axis_t
 * @brief Main axis a container stacks its children along.
 */
typedef enum : uint8_t {
  k_ra_widget_axis_col = 0U, /**< Vertical stack (top -> bottom).   */
  k_ra_widget_axis_row = 1U, /**< Horizontal stack (left -> right). */
} ra_widget_axis_t;

/* ===========================================================================
 * Container operations (all pure except the render dispatch)
 * ===========================================================================
 */

/**
 * @brief Lay a stack of widgets out inside a frame (delegates to ra_box).
 *
 * @details
 * Builds a transient `ra_box` tree -- one container plus a leaf per **visible**
 * widget (invisible widgets are skipped, dwm-style) carrying that widget's
 * `fixed` / `flex` -- runs `ra_box_layout`, then copies each visible widget's
 * computed rect back into `widget->rect`. Invisible widgets keep their old
 * rect. No allocation: the caller supplies the `ra_box` scratch.
 *
 * @param[in,out] widgets  Widget array.
 * @param[in]     count    Number of widgets.
 * @param[in]     frame    Outer rectangle the stack fills.
 * @param[in]     axis     k_ra_widget_axis_col / _row.
 * @param[in]     gap      Gap between children (pixels).
 * @param[in]     pad      Inner padding of the frame (pixels).
 * @param[in]     box_scratch Caller `ra_box_t` scratch (>= count + 1 nodes).
 * @param[in]     box_cap  Capacity of `box_scratch`.
 *
 * @return ra_err_t
 * @retval k_ra_ok               Laid out; every visible widget's rect set.
 * @retval k_ra_err_null_ptr     `widgets`, `frame`, or `box_scratch` NULL.
 * @retval k_ra_err_invalid_arg  `box_cap` < (visible count + 1).
 *
 * @pre `widgets` covers `count`; `frame` non-NULL.
 * @pre `box_scratch` holds at least (visible_count + 1) nodes.
 * @post Every visible widget reachable has its `rect` assigned.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_widget_layout_stack(ra_widget_t*        widgets,
                                              uint16_t            count,
                                              const ra_ui_rect_t* frame,
                                              ra_widget_axis_t    axis,
                                              int16_t             gap,
                                              int16_t             pad,
                                              ra_box_t*           box_scratch,
                                              uint16_t            box_cap);

/**
 * @brief Route an event to the widget that should handle it.
 *
 * @details
 * For a **touch** event, finds the first visible widget whose rect contains
 * `(ev->x, ev->y)` (earlier widgets win on overlap) and calls its `on_input`.
 * For a **button** event, offers it to each visible widget in order until one
 * consumes it. A widget with a NULL `on_input` never consumes.
 *
 * @param[in,out] widgets     Widget array.
 * @param[in]     count       Number of widgets.
 * @param[in]     ev          The event to route.
 * @param[out]    out_handled Receives true if some widget consumed the event.
 *
 * @return ra_err_t
 * @retval k_ra_ok            Routed (see @p out_handled).
 * @retval k_ra_err_null_ptr  `widgets` (with count>0), `ev`, or
 *                            `out_handled` is NULL.
 *
 * @pre `ev` and `out_handled` non-NULL.
 * @post On a hit `*out_handled == true`; otherwise false.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_widget_dispatch(ra_widget_t*             widgets,
                                          uint16_t                 count,
                                          const ra_widget_event_t* ev,
                                          bool*                    out_handled);

/**
 * @brief Mark a widget dirty with a refresh hint (folds upward in strength).
 *
 * @param[in,out] w       Widget to invalidate.
 * @param[in]     refresh k_ra_widget_refresh_fast / _quality.
 *
 * @return ra_err_t
 * @retval k_ra_ok           Marked dirty.
 * @retval k_ra_err_null_ptr `w` is NULL.
 * @retval k_ra_err_invalid_arg `refresh` is k_ra_widget_refresh_none.
 *
 * @pre `w` non-NULL.
 * @post `w->dirty == true`; `w->refresh` is the stronger of old/new.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_widget_invalidate(ra_widget_t* w, ra_widget_refresh_t refresh);

/**
 * @brief Compute the minimal damage rectangle + refresh hint to flush.
 *
 * @details
 * Unions the rects of every **visible AND dirty** widget into one bounding
 * rectangle and folds their refresh hints into the strongest one. This is the
 * single `display_pal` flush the compositor issues: changing only the status
 * bar yields just the status bar's rect with the fast hint; a full redraw
 * yields the whole frame with the quality hint.
 *
 * @param[in]  widgets   Widget array.
 * @param[in]  count     Number of widgets.
 * @param[out] out_rect  Receives the union rect (w == h == 0 if nothing dirty).
 * @param[out] out_hint  Receives the folded refresh hint (none if clean).
 * @param[out] out_count Receives the number of dirty widgets.
 *
 * @return ra_err_t
 * @retval k_ra_ok           Computed (see @p out_count).
 * @retval k_ra_err_null_ptr Any output pointer, or `widgets` (count>0), NULL.
 *
 * @pre All output pointers non-NULL.
 * @post `*out_count == 0` iff nothing was dirty (then `*out_rect` is empty).
 *
 * @note Pure; not thread-safe vs concurrent mutation.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_widget_damage(const ra_widget_t*   widgets,
                                        uint16_t             count,
                                        ra_ui_rect_t*        out_rect,
                                        ra_widget_refresh_t* out_hint,
                                        uint16_t*            out_count);

/**
 * @brief Render every visible + dirty widget, clearing its damage.
 *
 * @details
 * Calls each visible, dirty widget's `render` (skipping NULL render callbacks)
 * and then clears its `dirty` flag and resets `refresh` to none. The selection
 * of *which* widgets render is pure (host-testable with a recording mock vtable);
 * the pixel drawing inside `render` is the on-target part.
 *
 * @param[in,out] widgets Widget array.
 * @param[in]     count   Number of widgets.
 *
 * @return ra_err_t
 * @retval k_ra_ok           Rendered + cleared.
 * @retval k_ra_err_null_ptr `widgets` is NULL while `count > 0`.
 *
 * @pre `widgets` covers `count`.
 * @post Every rendered widget has `dirty == false`, `refresh == none`.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_widget_render_dirty(ra_widget_t* widgets, uint16_t count);

/* ===========================================================================
 * Container panel: a composite widget that nests a child widget array
 * ===========================================================================
 */

/**
 * @struct ra_widget_panel_t
 * @brief A container widget's child array + stack-layout parameters (#145).
 *
 * @details
 * The flat container ops above (::ra_widget_layout_stack et al.) lay out one
 * array of widgets inside a frame. A `panel` lifts that into a **tree**: a
 * panel is itself a ::ra_widget_t (bind it with ::ra_widget_panel_init), so a
 * panel can be a child of another panel -- the dwm-style composition the issue
 * asks for, where the screen is a tree of opt-in pieces. Rendering a panel lays
 * its children out inside the panel's *own* `rect` and composites them; routing
 * offers an input event to those children. Everything is caller-owned (the
 * child array and the `ra_box` scratch), so a panel allocates nothing
 * (NASA Rule 3) and the same tree runs on the host and on the board.
 *
 * @invariant `box_scratch` holds at least `count + 1` nodes.
 * @invariant `children` covers `count` entries.
 *
 * @par Example:
 * @code
 * static ra_widget_t   kids[2];
 * static ra_box_t      scratch[3];
 * ra_widget_panel_t    body = {.children = kids, .box_scratch = scratch,
 *                              .count = 2U, .box_cap = 3U,
 *                              .axis = k_ra_widget_axis_row};
 * ra_widget_t          panel = {};
 * (void)ra_widget_panel_init(&panel, &body);
 * @endcode
 *
 * @see ra_widget_panel_init    Bind a widget to a panel.
 * @see ra_widget_panel_compose Run the top-level compose cycle.
 * @since 0.1.0
 */
typedef struct ra_widget_panel {
  ra_widget_t*     children;    /**< Child widget array (caller-owned).         */
  ra_box_t*        box_scratch; /**< Layout scratch (>= count + 1 ra_box_t).    */
  uint16_t         count;       /**< Number of children.                        */
  uint16_t         box_cap;     /**< Capacity of @ref box_scratch.              */
  int16_t          gap;         /**< Gap between children (pixels).             */
  int16_t          pad;         /**< Inner padding inset on the panel (pixels). */
  ra_widget_axis_t axis;        /**< Stack main axis (col / row).               */
  uint8_t          reserved;    /**< Padding to a 4-byte boundary.              */
} ra_widget_panel_t;

/**
 * @brief Return the shared vtable that every container panel uses.
 *
 * @details
 * One immutable vtable backs all panels: its `render` lays out the panel's
 * children inside the panel widget's `rect` and composites them (a dirty panel
 * repaints its whole subtree); its `on_input` routes the event to the children
 * via ::ra_widget_dispatch; `measure` is NULL (a panel sizes from its parent's
 * `fixed`/`flex` like any widget). ::ra_widget_panel_init binds this vtable, so
 * callers rarely need it directly -- it is exposed for tests and for building a
 * widget by hand.
 *
 * @return Non-NULL pointer to the static panel vtable.
 *
 * @pre None.
 * @pre None.
 * @post The returned pointer is non-NULL and references static storage.
 * @post No state is modified.
 *
 * @note Pure; thread-safe (returns a pointer to immutable static data).
 * @note Render/route descend the tree, so nesting depth is bounded by the
 *       caller's static tree -- there is no data-dependent self-call
 *       (NASA Rule 1).
 * @see ra_widget_panel_init
 * @since 0.1.0
 */
const ra_widget_vtable_t* ra_widget_panel_vtable(void);

/**
 * @brief Bind a widget instance to a container panel.
 *
 * @details
 * Wires @p w to act as a container: sets its vtable to ::ra_widget_panel_vtable,
 * points its `ctx` at @p panel, and makes it visible. The caller still sets
 * @p w's `fixed` / `flex` for its parent's layout (a root panel is typically
 * pinned by ::ra_widget_panel_compose, which sets its `rect` directly).
 *
 * @param[in,out] w     Widget to turn into a panel (non-NULL).
 * @param[in]     panel Panel descriptor (non-NULL; `children` covers `count`).
 *
 * @return ra_err_t
 * @retval k_ra_ok               Bound; @p w renders/routes @p panel's children.
 * @retval k_ra_err_null_ptr     @p w or @p panel is NULL.
 * @retval k_ra_err_invalid_arg  @p panel has `count > 0` but a NULL child array
 *                               or `box_cap < count + 1`.
 *
 * @pre @p w and @p panel are non-NULL.
 * @pre @p panel->box_scratch holds at least `count + 1` nodes.
 * @post On success `w->vt == ra_widget_panel_vtable()`, `w->ctx == panel`,
 *       `w->visible == true`.
 * @post On failure @p w is left unchanged.
 *
 * @note Not thread-safe.
 * @see ra_widget_panel_compose
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_widget_panel_init(ra_widget_t* w, ra_widget_panel_t* panel);

/**
 * @brief Run one top-level compose cycle over a panel and report the flush.
 *
 * @details
 * The compositor pass the issue asks for, packaged as one call:
 *   1. Pin the panel to @p frame (`panel_w->rect = *frame`).
 *   2. Lay the panel's children out inside it (::ra_widget_layout_stack).
 *   3. Compute the minimal damage rectangle + folded refresh hint over the
 *      **dirty** children (::ra_widget_damage) -- what the caller hands to
 *      `display_flush`.
 *   4. Composite by rendering only the dirty children
 *      (::ra_widget_render_dirty); a dirty child that is itself a panel
 *      repaints its whole subtree.
 *
 * Marking the whole tree dirty before the call yields a full-frame quality
 * flush; marking only one child (e.g. the status bar) yields just that child's
 * rect with its hint -- the damage-tracked partial update.
 *
 * @param[in,out] panel_w    The root panel widget (bound via panel_init).
 * @param[in]     frame      Outer rectangle the panel fills (the framebuffer).
 * @param[out]    out_damage Receives the union rect to flush (empty if clean).
 * @param[out]    out_hint   Receives the folded refresh hint (none if clean).
 * @param[out]    out_dirty  Receives the number of dirty children composited.
 *
 * @return ra_err_t
 * @retval k_ra_ok               Composed; see @p out_dirty / @p out_damage.
 * @retval k_ra_err_null_ptr     Any pointer argument is NULL.
 * @retval k_ra_err_invalid_arg  @p panel_w is not a panel, or its scratch is
 *                               too small (forwarded from the layout step).
 *
 * @pre All pointer arguments are non-NULL.
 * @pre @p panel_w was bound by ::ra_widget_panel_init.
 * @post On success every dirty child has been rendered and cleared.
 * @post `*out_dirty == 0` iff nothing was dirty (then `*out_damage` is empty).
 *
 * @note Not thread-safe. Descends the tree; depth is statically bounded by the
 *       caller's widget tree (NASA Rule 1).
 * @see ra_widget_damage
 * @see ra_widget_render_dirty
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_widget_panel_compose(ra_widget_t*         panel_w,
                                               const ra_ui_rect_t*  frame,
                                               ra_ui_rect_t*        out_damage,
                                               ra_widget_refresh_t* out_hint,
                                               uint16_t*            out_dirty);

#ifdef __cplusplus
}
#endif
