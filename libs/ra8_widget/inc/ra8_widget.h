/**
 * @file ra8_widget.h
 * @brief Zero-heap composable widget layer (dwm-style) over ra8_box + ra8_ui.
 * @ingroup grp_ereader
 *
 * @details
 * `ra8_widget` is the composable-UI layer issue #145 asks for: render
 * *everything* as widgets the way a tiling WM (dwm) composes a screen from
 * opt-in pieces. A status bar is a widget; a book view is a widget; an
 * on-screen keyboard is a widget. A screen (or an app, issue #146) is just an
 * array of widgets laid out by a container.
 *
 * The split that keeps this testable on the host AND zero-heap on the RA8D2
 * (NASA Rule 3):
 *
 *   - **Layout** is delegated to `ra8_box` (the bounded box-model engine):
 *     children stack in a row / column with fixed-or-flex main-axis sizing,
 *     padding and gap. Invisible widgets are skipped (dwm-style opt-in).
 *   - **Hit routing** is delegated to `ra8_ui` (`ra8_ui_rect_contains`): a touch
 *     is routed to the visible widget whose rect contains it.
 *   - **Damage** is a union of the dirty widgets' rects plus a refresh hint
 *     (fast/A2 for text, quality/GC16 for full redraws) -- the minimal
 *     `display_pal` flush the compositor must issue.
 *   - **Render** is a per-widget vtable callback, so this library carries **no**
 *     `ra8_gfx` / framebuffer dependency: the layout/route/damage logic is pure
 *     and unit-tested on the host; the concrete pixel drawing lives in the
 *     widget's `render` callback (exercised on-target).
 *
 * Everything is caller-owned plain data (static widget + box arrays, no
 * globals, no allocation), so the same composition runs identically on the
 * host test harness and on the board.
 *
 *
 * [Ring 5 / UI]
 * {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_box.h"
#include "ra8_err.h"
#include "ra8_ui.h"

/* ===========================================================================
 * Events + refresh hints
 * ===========================================================================
 */

/**
 * @enum ra8_widget_ev_kind_t
 * @brief Input event kind routed to widgets.
 *
 * @details
 * Selects how ::ra8_widget_dispatch routes an event: a `touch` is delivered to
 * the single widget whose rect contains `(x, y)`, while a `button` is offered
 * to each visible widget in order until one consumes it.
 */
typedef enum : uint8_t {
  k_ra8_widget_ev_touch  = 0U, /**< A touch / tap at (x, y).             */
  k_ra8_widget_ev_button = 1U, /**< A physical button press (button_id). */
} ra8_widget_ev_kind_t;

/**
 * @struct ra8_widget_event_t
 * @brief One input event delivered to ::ra8_widget_dispatch.
 */
typedef struct {
  ra8_widget_ev_kind_t kind;      /**< Touch or button.                        */
  uint8_t              reserved;  /**< Padding.                                */
  uint16_t             button_id; /**< Button id (for k_ra8_widget_ev_button). */
  int32_t              x;         /**< Touch X (for k_ra8_widget_ev_touch).    */
  int32_t              y;         /**< Touch Y (for k_ra8_widget_ev_touch).    */
} ra8_widget_event_t;

/**
 * @enum ra8_widget_refresh_t
 * @brief E-ink-style refresh hint carried by a dirty widget.
 *
 * @details
 * Mirrors `ra8_display_pal`'s `display_refresh_hint_t`. A widget that changed
 * only text/glyphs marks `fast` (A2 partial update, no flash); a widget that
 * redrew its whole area marks `quality` (GC16, ghost-free). The compositor
 * folds the dirty widgets' hints into the *strongest* one for the flush.
 *
 * @note Hint strength is ordered `none < fast < quality`; folding keeps the
 *       highest value present across the dirty widgets.
 */
typedef enum : uint8_t {
  k_ra8_widget_refresh_none    = 0U, /**< Clean -- not dirty.              */
  k_ra8_widget_refresh_fast    = 1U, /**< Partial / A2 (text-only change). */
  k_ra8_widget_refresh_quality = 2U, /**< Full / GC16 (whole-area redraw). */
} ra8_widget_refresh_t;

/* ===========================================================================
 * Widget + vtable
 * ===========================================================================
 */

struct ra8_widget; /* fwd */

/**
 * @struct ra8_widget_vtable_t
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
  void (*measure)(struct ra8_widget* w,
                  int32_t            avail_w,
                  int32_t            avail_h,
                  int32_t*           out_w,
                  int32_t*           out_h);
  /**
   * @brief Draw the widget within its current `rect` (on-target only).
   * @param[in] w The widget instance. NULL is a no-op (spacer).
   */
  void (*render)(struct ra8_widget* w);
  /**
   * @brief Handle an input event.
   * @param[in] w  The widget instance.
   * @param[in] ev The event (already routed to this widget).
   * @return true if the widget consumed the event, false to keep routing.
   */
  bool (*on_input)(struct ra8_widget* w, const ra8_widget_event_t* ev);
} ra8_widget_vtable_t;

/**
 * @struct ra8_widget_t
 * @brief One widget instance (caller-owned, no allocation).
 *
 * @details
 * `fixed` / `flex` drive the container layout the same way `ra8_box` does:
 * `fixed > 0` pins the main-axis extent, otherwise `flex` weight splits the
 * leftover. `visible == false` removes the widget from layout / routing /
 * render entirely (dwm opt-in). `dirty` + `refresh` track pending damage.
 */
typedef struct ra8_widget {
  const ra8_widget_vtable_t* vt;        /**< Behaviour table (non-NULL).     */
  void*                      ctx;       /**< Widget-specific state.          */
  ra8_ui_rect_t              rect;      /**< Current bounds (layout output). */
  int16_t                    fixed;     /**< Fixed main-axis extent, or 0.   */
  uint16_t                   flex;      /**< Flex weight when not fixed.     */
  uint16_t                   action_id; /**< Hit-routing id (0 = none).      */
  uint8_t                    refresh;   /**< ra8_widget_refresh_t (pending). */
  bool                       visible;   /**< Laid out / drawn when true.     */
  bool                       dirty;     /**< Needs a redraw + flush.         */
} ra8_widget_t;

/**
 * @enum ra8_widget_axis_t
 * @brief Main axis a container stacks its children along.
 */
typedef enum : uint8_t {
  k_ra8_widget_axis_col = 0U, /**< Vertical stack (top -> bottom).   */
  k_ra8_widget_axis_row = 1U, /**< Horizontal stack (left -> right). */
} ra8_widget_axis_t;

/* ===========================================================================
 * Container operations (all pure except the render dispatch)
 * ===========================================================================
 */

/**
 * @brief Lay a stack of widgets out inside a frame (delegates to ra8_box).
 *
 * @details
 * Builds a transient `ra8_box` tree -- one container plus a leaf per **visible**
 * widget (invisible widgets are skipped, dwm-style) carrying that widget's
 * `fixed` / `flex` -- runs `ra8_box_layout`, then copies each visible widget's
 * computed rect back into `widget->rect`. Invisible widgets keep their old
 * rect. No allocation: the caller supplies the `ra8_box` scratch.
 *
 * @param[in,out] widgets  Widget array.
 * @param[in]     count    Number of widgets.
 * @param[in]     frame    Outer rectangle the stack fills.
 * @param[in]     axis     k_ra8_widget_axis_col / _row.
 * @param[in]     gap      Gap between children (pixels).
 * @param[in]     pad      Inner padding of the frame (pixels).
 * @param[in]     box_scratch Caller `ra8_box_t` scratch (>= count + 1 nodes).
 * @param[in]     box_cap  Capacity of `box_scratch`.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok               Laid out; every visible widget's rect set.
 * @retval k_ra8_err_null_ptr     `widgets`, `frame`, or `box_scratch` NULL.
 * @retval k_ra8_err_invalid_arg  `box_cap` < (visible count + 1).
 *
 * @pre `widgets` covers `count`; `frame` non-NULL.
 * @pre `box_scratch` holds at least (visible_count + 1) nodes.
 * @post Every visible widget reachable has its `rect` assigned.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_widget_layout_stack(ra8_widget_t*        widgets,
                                                uint16_t             count,
                                                const ra8_ui_rect_t* frame,
                                                ra8_widget_axis_t    axis,
                                                int16_t              gap,
                                                int16_t              pad,
                                                ra8_box_t*           box_scratch,
                                                uint16_t             box_cap);

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
 * @return ra8_err_t
 * @retval k_ra8_ok            Routed (see @p out_handled).
 * @retval k_ra8_err_null_ptr  `widgets` (with count>0), `ev`, or
 *                            `out_handled` is NULL.
 *
 * @pre `ev` and `out_handled` non-NULL.
 * @post On a hit `*out_handled == true`; otherwise false.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_widget_dispatch(ra8_widget_t*             widgets,
                                            uint16_t                  count,
                                            const ra8_widget_event_t* ev,
                                            bool*                     out_handled);

/**
 * @brief Mark a widget dirty with a refresh hint (folds upward in strength).
 *
 * @param[in,out] w       Widget to invalidate.
 * @param[in]     refresh k_ra8_widget_refresh_fast / _quality.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok           Marked dirty.
 * @retval k_ra8_err_null_ptr `w` is NULL.
 * @retval k_ra8_err_invalid_arg `refresh` is k_ra8_widget_refresh_none.
 *
 * @pre `w` non-NULL.
 * @post `w->dirty == true`; `w->refresh` is the stronger of old/new.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_widget_invalidate(ra8_widget_t* w, ra8_widget_refresh_t refresh);

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
 * @return ra8_err_t
 * @retval k_ra8_ok           Computed (see @p out_count).
 * @retval k_ra8_err_null_ptr Any output pointer, or `widgets` (count>0), NULL.
 *
 * @pre All output pointers non-NULL.
 * @post `*out_count == 0` iff nothing was dirty (then `*out_rect` is empty).
 *
 * @note Pure; not thread-safe vs concurrent mutation.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_widget_damage(const ra8_widget_t*   widgets,
                                          uint16_t              count,
                                          ra8_ui_rect_t*        out_rect,
                                          ra8_widget_refresh_t* out_hint,
                                          uint16_t*             out_count);

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
 * @return ra8_err_t
 * @retval k_ra8_ok           Rendered + cleared.
 * @retval k_ra8_err_null_ptr `widgets` is NULL while `count > 0`.
 *
 * @pre `widgets` covers `count`.
 * @post Every rendered widget has `dirty == false`, `refresh == none`.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_widget_render_dirty(ra8_widget_t* widgets, uint16_t count);

/* ===========================================================================
 * Container panel: a composite widget that nests a child widget array
 * ===========================================================================
 */

/**
 * @struct ra8_widget_panel_t
 * @brief A container widget's child array + stack-layout parameters (#145).
 *
 * @details
 * The flat container ops above (::ra8_widget_layout_stack et al.) lay out one
 * array of widgets inside a frame. A `panel` lifts that into a **tree**: a
 * panel is itself a ::ra8_widget_t (bind it with ::ra8_widget_panel_init), so a
 * panel can be a child of another panel -- the dwm-style composition the issue
 * asks for, where the screen is a tree of opt-in pieces. Rendering a panel lays
 * its children out inside the panel's *own* `rect` and composites them; routing
 * offers an input event to those children. Everything is caller-owned (the
 * child array and the `ra8_box` scratch), so a panel allocates nothing
 * (NASA Rule 3) and the same tree runs on the host and on the board.
 *
 * @invariant `box_scratch` holds at least `count + 1` nodes.
 * @invariant `children` covers `count` entries.
 *
 * @par Example:
 * @code
 * static ra8_widget_t   kids[2];
 * static ra8_box_t      scratch[3];
 * ra8_widget_panel_t    body = {.children = kids, .box_scratch = scratch,
 *                              .count = 2U, .box_cap = 3U,
 *                              .axis = k_ra8_widget_axis_row};
 * ra8_widget_t          panel = {};
 * (void)ra8_widget_panel_init(&panel, &body);
 * @endcode
 *
 * @see ra8_widget_panel_init    Bind a widget to a panel.
 * @see ra8_widget_panel_compose Run the top-level compose cycle.
 * @since 0.1.0
 */
typedef struct ra8_widget_panel {
  ra8_widget_t*     children;    /**< Child widget array (caller-owned).         */
  ra8_box_t*        box_scratch; /**< Layout scratch (>= count + 1 ra8_box_t).   */
  uint16_t          count;       /**< Number of children.                        */
  uint16_t          box_cap;     /**< Capacity of @ref box_scratch.              */
  int16_t           gap;         /**< Gap between children (pixels).             */
  int16_t           pad;         /**< Inner padding inset on the panel (pixels). */
  ra8_widget_axis_t axis;        /**< Stack main axis (col / row).               */
  uint8_t           reserved;    /**< Padding to a 4-byte boundary.              */
} ra8_widget_panel_t;

/**
 * @brief Return the shared vtable that every container panel uses.
 *
 * @details
 * One immutable vtable backs all panels: its `render` lays out the panel's
 * children inside the panel widget's `rect` and composites them (a dirty panel
 * repaints its whole subtree); its `on_input` routes the event to the children
 * via ::ra8_widget_dispatch; `measure` is NULL (a panel sizes from its parent's
 * `fixed`/`flex` like any widget). ::ra8_widget_panel_init binds this vtable, so
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
 * @see ra8_widget_panel_init
 * @since 0.1.0
 */
const ra8_widget_vtable_t* ra8_widget_panel_vtable(void);

/**
 * @brief Bind a widget instance to a container panel.
 *
 * @details
 * Wires @p w to act as a container: sets its vtable to ::ra8_widget_panel_vtable,
 * points its `ctx` at @p panel, and makes it visible. The caller still sets
 * @p w's `fixed` / `flex` for its parent's layout (a root panel is typically
 * pinned by ::ra8_widget_panel_compose, which sets its `rect` directly).
 *
 * @param[in,out] w     Widget to turn into a panel (non-NULL).
 * @param[in]     panel Panel descriptor (non-NULL; `children` covers `count`).
 *
 * @return ra8_err_t
 * @retval k_ra8_ok               Bound; @p w renders/routes @p panel's children.
 * @retval k_ra8_err_null_ptr     @p w or @p panel is NULL.
 * @retval k_ra8_err_invalid_arg  @p panel has `count > 0` but a NULL child array
 *                               or `box_cap < count + 1`.
 *
 * @pre @p w and @p panel are non-NULL.
 * @pre @p panel->box_scratch holds at least `count + 1` nodes.
 * @post On success `w->vt == ra8_widget_panel_vtable()`, `w->ctx == panel`,
 *       `w->visible == true`.
 * @post On failure @p w is left unchanged.
 *
 * @note Not thread-safe.
 * @see ra8_widget_panel_compose
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_widget_panel_init(ra8_widget_t* w, ra8_widget_panel_t* panel);

/**
 * @brief Run one top-level compose cycle over a panel and report the flush.
 *
 * @details
 * The compositor pass the issue asks for, packaged as one call:
 *   1. Pin the panel to @p frame (`panel_w->rect = *frame`).
 *   2. Lay the panel's children out inside it (::ra8_widget_layout_stack).
 *   3. Compute the minimal damage rectangle + folded refresh hint over the
 *      **dirty** children (::ra8_widget_damage) -- what the caller hands to
 *      `display_flush`.
 *   4. Composite by rendering only the dirty children
 *      (::ra8_widget_render_dirty); a dirty child that is itself a panel
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
 * @return ra8_err_t
 * @retval k_ra8_ok               Composed; see @p out_dirty / @p out_damage.
 * @retval k_ra8_err_null_ptr     Any pointer argument is NULL.
 * @retval k_ra8_err_invalid_arg  @p panel_w is not a panel, or its scratch is
 *                               too small (forwarded from the layout step).
 *
 * @pre All pointer arguments are non-NULL.
 * @pre @p panel_w was bound by ::ra8_widget_panel_init.
 * @post On success every dirty child has been rendered and cleared.
 * @post `*out_dirty == 0` iff nothing was dirty (then `*out_damage` is empty).
 *
 * @note Not thread-safe. Descends the tree; depth is statically bounded by the
 *       caller's widget tree (NASA Rule 1).
 * @see ra8_widget_damage
 * @see ra8_widget_render_dirty
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_widget_panel_compose(ra8_widget_t*         panel_w,
                                                 const ra8_ui_rect_t*  frame,
                                                 ra8_ui_rect_t*        out_damage,
                                                 ra8_widget_refresh_t* out_hint,
                                                 uint16_t*             out_dirty);

/* ===========================================================================
 * Concrete leaf widgets: text label + push button (DI paint backend)
 * ===========================================================================
 */

/**
 * @struct ra8_widget_paint_t
 * @brief Drawing backend the concrete leaf widgets paint through (a DI seam).
 *
 * @details
 * The compositor logic in this library never touches pixels -- that is what
 * keeps the layout / route / damage cycle host-testable and free of any
 * `ra8_gfx` / framebuffer dependency. The concrete leaf widgets below
 * (::ra8_widget_label_t, ::ra8_widget_button_t) *do* draw, so their pixel
 * primitives are **injected** as a small set of callbacks rather than linked
 * directly. On the board the app binds these to its renderer (`ra8_gfx_rect`,
 * `ra8_gfx_text_out`, `ra8_gfx_text_size`); a host test binds a recording mock.
 * This Dependency-Inversion seam lets the very same widget run on the GLCDC
 * panel and under the unit-test harness, and means an app that wants only the
 * pure compositor never pulls in a graphics library.
 *
 * @invariant `fill_rect` is non-NULL for any widget that should paint a face.
 * @invariant `draw_text` is non-NULL for any widget that should paint a label.
 *
 * @par Example:
 * @code
 * static void gfx_fill(void* u, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t c) {
 *   (void)u;
 *   (void)ra8_gfx_rect(x, y, w, h, c, true);
 * }
 * static const ra8_widget_paint_t paint = {.user = nullptr, .fill_rect = gfx_fill};
 * @endcode
 *
 * @see ra8_widget_label_init
 * @see ra8_widget_button_init
 * @since 0.1.0
 */
typedef struct ra8_widget_paint {
  /** @brief Opaque backend handle handed back to every callback below. */
  void* user;
  /** @brief Fill an axis-aligned rectangle with one solid colour. */
  void (*fill_rect)(void* user, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color);
  /** @brief Draw a NUL-terminated ASCII string in @p fg over @p bg. */
  void (*draw_text)(void* user, int32_t x, int32_t y, const char* str, uint32_t fg, uint32_t bg);
  /** @brief Measure a string's pixel size; NULL disables centre/right align. */
  void (*text_size)(void* user, const char* str, int32_t* out_w, int32_t* out_h);
} ra8_widget_paint_t;

/**
 * @enum ra8_widget_align_t
 * @brief Horizontal placement of a leaf widget's text within its rect.
 *
 * @details
 * `center` and `right` need the string's pixel width, so they fall back to
 * `left` when the paint backend supplies no ::ra8_widget_paint_t::text_size.
 */
typedef enum : uint8_t {
  k_ra8_widget_align_left   = 0U, /**< Hug the left inner inset.        */
  k_ra8_widget_align_center = 1U, /**< Centre horizontally in the rect. */
  k_ra8_widget_align_right  = 2U, /**< Hug the right inner inset.       */
} ra8_widget_align_t;

/**
 * @struct ra8_widget_label_t
 * @brief A text-label leaf widget: a background fill plus an aligned string.
 *
 * @details
 * The simplest concrete leaf -- it paints its `bg` over its rect and draws
 * `text` (aligned, vertically centred when the backend can measure it). It is
 * display-only, so its vtable leaves `on_input` NULL: a label never consumes a
 * touch. Caller-owned plain data, bound to a ::ra8_widget_t with
 * ::ra8_widget_label_init.
 *
 * @invariant `paint` is NULL or points at a valid ::ra8_widget_paint_t.
 *
 * @see ra8_widget_label_init
 * @since 0.1.0
 */
typedef struct ra8_widget_label {
  const ra8_widget_paint_t* paint;    /**< Draw backend (NULL -> draws nothing).   */
  const char*               text;     /**< NUL-terminated ASCII (NULL -> bg only). */
  uint32_t                  fg;       /**< Text colour, 0xRRGGBB.                  */
  uint32_t                  bg;       /**< Background fill colour, 0xRRGGBB.       */
  int16_t                   pad;      /**< Inner text inset, pixels.               */
  ra8_widget_align_t        align;    /**< Horizontal text alignment.              */
  uint8_t                   reserved; /**< Padding to a 4-byte boundary.           */
} ra8_widget_label_t;

/**
 * @struct ra8_widget_button_t
 * @brief A push-button leaf widget: a bordered face + label that latches on tap.
 *
 * @details
 * Paints a filled, optionally bordered face (a different fill while `pressed`)
 * with an aligned label. Its `on_input` consumes a **touch** (the dispatcher
 * has already hit-tested it onto the button): each tap flips `pressed`,
 * increments `presses`, marks the button dirty with the fast hint (so only its
 * rect re-flushes), then invokes the optional `on_press` callback. Button
 * events are ignored (left for other widgets). Caller-owned plain data, bound
 * with ::ra8_widget_button_init.
 *
 * @invariant `border_w >= 0`.
 * @invariant `presses` only ever increases.
 *
 * @see ra8_widget_button_init
 * @since 0.1.0
 */
typedef struct ra8_widget_button {
  const ra8_widget_paint_t* paint;        /**< Draw backend (NULL -> no draw).      */
  const char*               text;         /**< Label (NULL -> face only).           */
  void (*on_press)(struct ra8_widget* w); /**< Press callback (optional / NULL).    */
  uint32_t           fg;                  /**< Label colour, 0xRRGGBB.              */
  uint32_t           face;                /**< Face fill when released, 0xRRGGBB.   */
  uint32_t           face_pressed;        /**< Face fill when pressed, 0xRRGGBB.    */
  uint32_t           border;              /**< Outline colour, 0xRRGGBB.            */
  uint32_t           presses;             /**< Count of taps consumed (observable). */
  int16_t            pad;                 /**< Inner label inset, pixels.           */
  int16_t            border_w;            /**< Outline thickness, pixels (>= 0).    */
  ra8_widget_align_t align;               /**< Label alignment.                     */
  bool               pressed;             /**< Latched pressed state.               */
  uint8_t            reserved;            /**< Padding to a 4-byte boundary.        */
} ra8_widget_button_t;

/**
 * @brief Return the shared vtable backing every text-label widget.
 *
 * @details
 * One immutable vtable serves all labels: `render` fills the background and
 * draws the aligned text through the label's ::ra8_widget_paint_t; `measure` and
 * `on_input` are NULL (a label sizes from its parent's fixed/flex and never
 * consumes input). ::ra8_widget_label_init binds it, so callers rarely need it
 * directly -- it is exposed for tests and hand-built widgets.
 *
 * @return Non-NULL pointer to the static label vtable.
 *
 * @pre None.
 * @pre None.
 * @post The returned pointer is non-NULL and references static storage.
 * @post No state is modified.
 *
 * @note Pure; thread-safe (returns a pointer to immutable static data).
 * @see ra8_widget_label_init
 * @since 0.1.0
 */
const ra8_widget_vtable_t* ra8_widget_label_vtable(void);

/**
 * @brief Bind a widget instance to a text label.
 *
 * @details
 * Wires @p w to render @p label: sets its vtable to ::ra8_widget_label_vtable,
 * points its `ctx` at @p label, and makes it visible. The caller still sets
 * @p w's `fixed` / `flex` for its parent's layout.
 *
 * @param[in,out] w     Widget to turn into a label (non-NULL).
 * @param[in]     label Label descriptor (non-NULL; outlives @p w's use).
 *
 * @return ra8_err_t
 * @retval k_ra8_ok           Bound; @p w renders @p label.
 * @retval k_ra8_err_null_ptr @p w or @p label is NULL.
 *
 * @pre @p w and @p label are non-NULL.
 * @pre @p label outlives every render of @p w.
 * @post On success `w->vt == ra8_widget_label_vtable()`, `w->ctx == label`,
 *       `w->visible == true`.
 * @post On failure @p w is left unchanged.
 *
 * @note Not thread-safe.
 * @see ra8_widget_label_vtable
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_widget_label_init(ra8_widget_t* w, ra8_widget_label_t* label);

/**
 * @brief Return the shared vtable backing every push-button widget.
 *
 * @details
 * One immutable vtable serves all buttons: `render` paints the (optionally
 * bordered) face and aligned label through the button's ::ra8_widget_paint_t;
 * `on_input` latches a touch (flip `pressed`, bump `presses`, self-invalidate
 * fast, call `on_press`) and consumes it, ignoring button events; `measure` is
 * NULL. ::ra8_widget_button_init binds it.
 *
 * @return Non-NULL pointer to the static button vtable.
 *
 * @pre None.
 * @pre None.
 * @post The returned pointer is non-NULL and references static storage.
 * @post No state is modified.
 *
 * @note Pure; thread-safe (returns a pointer to immutable static data).
 * @see ra8_widget_button_init
 * @since 0.1.0
 */
const ra8_widget_vtable_t* ra8_widget_button_vtable(void);

/**
 * @brief Bind a widget instance to a push button.
 *
 * @details
 * Wires @p w to render + route @p button: sets its vtable to
 * ::ra8_widget_button_vtable, points its `ctx` at @p button, and makes it
 * visible. The caller still sets @p w's `fixed` / `flex` (and usually its
 * `action_id`) for its parent's layout and hit routing.
 *
 * @param[in,out] w      Widget to turn into a button (non-NULL).
 * @param[in]     button Button descriptor (non-NULL; outlives @p w's use).
 *
 * @return ra8_err_t
 * @retval k_ra8_ok           Bound; @p w renders + routes @p button.
 * @retval k_ra8_err_null_ptr @p w or @p button is NULL.
 *
 * @pre @p w and @p button are non-NULL.
 * @pre @p button outlives every render / dispatch of @p w.
 * @post On success `w->vt == ra8_widget_button_vtable()`, `w->ctx == button`,
 *       `w->visible == true`.
 * @post On failure @p w is left unchanged.
 *
 * @note Not thread-safe.
 * @see ra8_widget_button_vtable
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_widget_button_init(ra8_widget_t* w, ra8_widget_button_t* button);

#ifdef __cplusplus
}
#endif
