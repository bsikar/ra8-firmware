/**
 * @file ra8_widget.c
 * @brief Implementation of the ra8_widget composable-UI layer (#145).
 * @details Implements bounded widget traversal, invalidation, event routing,
 * and drawing over caller-owned widget and rendering state.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_widget.h"

#include "ra8_attributes.h"
#include "ra8_check.h"

/** @brief Logging / check tag. */
static const char* s_tag = "ra8_widget";

/**
 * @brief True if a rect covers no pixels (used as the union identity).
 * @details A `w <= 0` and `h <= 0` rect is the empty/identity element of
 *          ::internal_rect_union, so the damage accumulator can start at `{}`.
 * @param[in] r Rect to test (non-NULL).
 * @return true if @p r covers no pixels, false otherwise.
 * @retval true  Both width and height are non-positive.
 * @retval false At least one dimension is positive.
 * @pre @p r is non-NULL.
 * @pre None.
 * @post No state is modified.
 * @post Return reflects emptiness only.
 * @note Pure.
 * @since 0.1.0
 */
RA8_INTERNAL
static bool internal_rect_empty(const ra8_ui_rect_t* r)
{
  return (r->w <= 0) && (r->h <= 0);
}

/**
 * @brief Smaller of two signed values.
 * @details Branch-style min used by the rect-union corner math.
 * @param[in] a First value.
 * @param[in] b Second value.
 * @return The lesser of @p a and @p b.
 * @retval a When `a < b`.
 * @retval b Otherwise.
 * @pre None.
 * @pre None.
 * @post No state is modified.
 * @post Return is `min(a, b)`.
 * @note Pure.
 * @since 0.1.0
 */
RA8_INTERNAL
static int32_t internal_min_i32(int32_t a, int32_t b)
{
  return (a < b) ? a : b;
}

/**
 * @brief Larger of two signed values.
 * @details Branch-style max used by the rect-union corner math.
 * @param[in] a First value.
 * @param[in] b Second value.
 * @return The greater of @p a and @p b.
 * @retval a When `a > b`.
 * @retval b Otherwise.
 * @pre None.
 * @pre None.
 * @post No state is modified.
 * @post Return is `max(a, b)`.
 * @note Pure.
 * @since 0.1.0
 */
RA8_INTERNAL
static int32_t internal_max_i32(int32_t a, int32_t b)
{
  return (a > b) ? a : b;
}

/**
 * @brief Bounding union of two rects (empty acts as the identity).
 * @details Returns the smallest rect covering both inputs; an empty input
 *          returns the other unchanged, so a `{}` accumulator folds cleanly.
 * @param[in] acc Accumulated rect (or empty identity).
 * @param[in] r   Rect to fold in.
 * @return The bounding union of @p acc and @p r.
 * @retval r   When @p acc is empty.
 * @retval acc When @p r is empty.
 * @pre None.
 * @pre None.
 * @post No input is modified (by value).
 * @post Return contains every pixel of both non-empty inputs.
 * @note Pure.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_ui_rect_t internal_rect_union(ra8_ui_rect_t acc, ra8_ui_rect_t r)
{
  if (internal_rect_empty(&acc)) {
    return r;
  }
  if (internal_rect_empty(&r)) {
    return acc;
  }
  const int32_t       x0  = internal_min_i32(acc.x, r.x);
  const int32_t       y0  = internal_min_i32(acc.y, r.y);
  const int32_t       x1  = internal_max_i32(acc.x + acc.w, r.x + r.w);
  const int32_t       y1  = internal_max_i32(acc.y + acc.h, r.y + r.h);
  const ra8_ui_rect_t out = {.x = x0, .y = y0, .w = x1 - x0, .h = y1 - y0};
  return out;
}

/**
 * @brief Count the visible widgets in @p widgets[0..count).
 * @details Used to size the `ra8_box` scratch (one leaf per visible widget plus
 *          the container) before laying a stack out.
 * @param[in] widgets Widget array (non-NULL when @p count > 0).
 * @param[in] count   Number of widgets.
 * @return The number of widgets whose `visible` flag is set.
 * @retval 0 No widget is visible.
 * @pre @p widgets covers @p count entries.
 * @pre None.
 * @post No state is modified.
 * @post Return is in `[0, count]`.
 * @note Pure; loop bounded by @p count (NASA Rule 2).
 * @since 0.1.0
 */
RA8_INTERNAL
static uint16_t internal_visible_count(const ra8_widget_t* widgets, uint16_t count)
{
  uint16_t vis = 0U;
  for (uint16_t i = 0U; i < count; ++i) {
    if (widgets[i].visible) {
      ++vis;
    }
  }
  return vis;
}

/**
 * @brief Build a ra8_box stack tree: one container + a leaf per visible widget.
 * @details Sizes the scratch against the visible count, adds a stack container
 *          (row/column) then one leaf per visible widget carrying its
 *          `fixed`/`flex`. The caller then runs `ra8_box_layout` and copies the
 *          leaf rects back. Split out so ::ra8_widget_layout_stack stays within
 *          the NASA Rule 4 function-size cap.
 * @param[in]  widgets  Widget array.
 * @param[in]  count    Number of widgets.
 * @param[in]  axis     Stack main axis.
 * @param[in]  gap      Gap between children.
 * @param[in]  pad      Inner padding.
 * @param[in]  scratch  Caller ra8_box scratch.
 * @param[in]  cap      Capacity of @p scratch.
 * @param[out] out_tree Receives the built tree.
 * @param[out] out_root Receives the container root index.
 * @return ra8_err_t
 * @retval k_ra8_ok              Tree built; @p out_root is the container.
 * @retval k_ra8_err_invalid_arg Scratch too small or a box add failed.
 * @retval <ra8_box_tree_init's> Forwarded from `ra8_box_tree_init`.
 * @pre @p out_tree / @p out_root non-NULL; @p scratch covers @p cap.
 * @pre @p cap >= visible_count + 1.
 * @post On success @p out_tree holds the container + visible leaves.
 * @post On failure the caller must not use @p out_root.
 * @note Not thread-safe; loop bounded by @p count (NASA Rule 2).
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_build_stack_tree(const ra8_widget_t* widgets,
                                           uint16_t            count,
                                           ra8_widget_axis_t   axis,
                                           int16_t             gap,
                                           int16_t             pad,
                                           ra8_box_t*          scratch,
                                           uint16_t            cap,
                                           ra8_box_tree_t*     out_tree,
                                           int16_t*            out_root)
{
  const uint16_t vis = internal_visible_count(widgets, count);
  if ((uint32_t)cap < ((uint32_t)vis + 1U)) {
    return k_ra8_err_invalid_arg;
  }
  const ra8_err_t ierr = ra8_box_tree_init(out_tree, scratch, cap);
  if (ierr != k_ra8_ok) {
    return ierr;
  }
  ra8_box_t container = {};
  container.kind =
    (axis == k_ra8_widget_axis_row) ? (uint8_t)k_ra8_box_stack_h : (uint8_t)k_ra8_box_stack_v;
  container.pad  = pad;
  container.gap  = gap;
  container.flex = 1U;
  container.tag  = (int16_t)k_ra8_box_none;
  *out_root      = ra8_box_add(out_tree, (int16_t)k_ra8_box_none, &container);
  if (*out_root == (int16_t)k_ra8_box_none) {
    return k_ra8_err_invalid_arg;
  }
  for (uint16_t i = 0U; i < count; ++i) {
    if (!widgets[i].visible) {
      continue;
    }
    ra8_box_t leaf = {};
    leaf.kind      = (uint8_t)k_ra8_box_leaf;
    leaf.fixed     = widgets[i].fixed;
    leaf.flex      = widgets[i].flex;
    leaf.tag       = (int16_t)widgets[i].action_id;
    if (ra8_box_add(out_tree, *out_root, &leaf) == (int16_t)k_ra8_box_none) {
      return k_ra8_err_invalid_arg;
    }
  }
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_widget_layout_stack(ra8_widget_t*        widgets,
                                                uint16_t             count,
                                                const ra8_ui_rect_t* frame,
                                                ra8_widget_axis_t    axis,
                                                int16_t              gap,
                                                int16_t              pad,
                                                ra8_box_t*           box_scratch,
                                                uint16_t             box_cap)
{
  RA8_CHECK_NULL_PTR(widgets, s_tag, "widgets must not be nullptr");
  RA8_CHECK_NULL_PTR(frame, s_tag, "frame must not be nullptr");
  RA8_CHECK_NULL_PTR(box_scratch, s_tag, "box_scratch must not be nullptr");

  ra8_box_tree_t  tree = {};
  int16_t         root = (int16_t)k_ra8_box_none;
  const ra8_err_t berr =
    internal_build_stack_tree(widgets, count, axis, gap, pad, box_scratch, box_cap, &tree, &root);
  if (berr != k_ra8_ok) {
    return berr;
  }
  const ra8_err_t lerr = ra8_box_layout(&tree, root, frame);
  if (lerr != k_ra8_ok) {
    return lerr;
  }

  /* Box nodes 1..vis are the visible children in add order. */
  uint16_t box_idx = 1U;
  for (uint16_t i = 0U; i < count; ++i) {
    if (!widgets[i].visible) {
      continue;
    }
    widgets[i].rect = box_scratch[box_idx].rect;
    ++box_idx;
  }
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_widget_dispatch(ra8_widget_t*             widgets,
                                            uint16_t                  count,
                                            const ra8_widget_event_t* ev,
                                            bool*                     out_handled)
{
  RA8_CHECK_NULL_PTR(ev, s_tag, "ev must not be nullptr");
  RA8_CHECK_NULL_PTR(out_handled, s_tag, "out_handled must not be nullptr");
  if ((count > 0U) && (widgets == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  *out_handled = false;

  for (uint16_t i = 0U; i < count; ++i) {
    ra8_widget_t* w = &widgets[i];
    if (!w->visible || (w->vt == nullptr) || (w->vt->on_input == nullptr)) {
      continue;
    }
    if (ev->kind == k_ra8_widget_ev_touch) {
      /* Touch: only the widget under the point is offered the event. */
      if (!ra8_ui_rect_contains(&w->rect, ev->x, ev->y)) {
        continue;
      }
      *out_handled = w->vt->on_input(w, ev);
      return k_ra8_ok;
    }
    /* Button: offer to each visible widget until one consumes it. */
    if (w->vt->on_input(w, ev)) {
      *out_handled = true;
      return k_ra8_ok;
    }
  }
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_widget_invalidate(ra8_widget_t* w, ra8_widget_refresh_t refresh)
{
  RA8_CHECK_NULL_PTR(w, s_tag, "w must not be nullptr");
  if (refresh == k_ra8_widget_refresh_none) {
    return k_ra8_err_invalid_arg;
  }
  w->dirty = true;
  if ((uint8_t)refresh > w->refresh) {
    w->refresh = (uint8_t)refresh;
  }
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_widget_damage(const ra8_widget_t*   widgets,
                                          uint16_t              count,
                                          ra8_ui_rect_t*        out_rect,
                                          ra8_widget_refresh_t* out_hint,
                                          uint16_t*             out_count)
{
  RA8_CHECK_NULL_PTR(out_rect, s_tag, "out_rect must not be nullptr");
  RA8_CHECK_NULL_PTR(out_hint, s_tag, "out_hint must not be nullptr");
  RA8_CHECK_NULL_PTR(out_count, s_tag, "out_count must not be nullptr");
  if ((count > 0U) && (widgets == nullptr)) {
    return k_ra8_err_null_ptr;
  }

  ra8_ui_rect_t acc   = {.x = 0, .y = 0, .w = 0, .h = 0};
  uint8_t       hint  = (uint8_t)k_ra8_widget_refresh_none;
  uint16_t      dirty = 0U;
  for (uint16_t i = 0U; i < count; ++i) {
    if (!widgets[i].visible || !widgets[i].dirty) {
      continue;
    }
    acc = internal_rect_union(acc, widgets[i].rect);
    if (widgets[i].refresh > hint) {
      hint = widgets[i].refresh;
    }
    ++dirty;
  }
  *out_rect  = acc;
  *out_hint  = (ra8_widget_refresh_t)hint;
  *out_count = dirty;
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_widget_render_dirty(ra8_widget_t* widgets, uint16_t count)
{
  if ((count > 0U) && (widgets == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  for (uint16_t i = 0U; i < count; ++i) {
    ra8_widget_t* w = &widgets[i];
    if (!w->visible || !w->dirty) {
      continue;
    }
    if ((w->vt != nullptr) && (w->vt->render != nullptr)) {
      w->vt->render(w);
    }
    w->dirty   = false;
    w->refresh = (uint8_t)k_ra8_widget_refresh_none;
  }
  return k_ra8_ok;
}
