/**
 * @file ra8_compositor.c
 * @brief Implementation of the zero-heap framebuffer compositor.
 *
 * @par Tag
 * [Ring 5 / UI] {World: NS}
 *
 * @details
 * Scene construction, the bounded tiling layout sweep, and the clipped paint /
 * damage compositing described in ra8_compositor.h. Pixel drawing is delegated
 * to `ra8_gfx` (used read-only); the geometry helpers are pure value functions
 * so the whole module is deterministic and host-testable. No allocation, no
 * recursion (NASA Rule 1), every loop bounded by the node count (NASA Rule 2).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "ra8_compositor.h"

#include <stddef.h>
#include <stdint.h>

#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_gfx.h"

/** @brief Component tag for diagnostic logging. */
static const char* const s_tag = "ra8_compositor";

/**
 * @enum ra8_compositor_priv_t
 * @brief Private layout magnitudes used by the tiling math.
 */
typedef enum : int32_t {
  k_ra8_compositor_edges = 2, /**< Padding is inset on both edges of an axis. */
} ra8_compositor_priv_t;

/* ===========================================================================
 * Geometry helpers (pure value functions -- no aliasing, no null branches)
 * ===========================================================================
 */

/**
 * @brief Report whether a rectangle covers no pixels.
 *
 * @details A rect is empty when either extent is non-positive; such a rect is
 * used as the "nothing" value for clips and damage unions.
 *
 * @param[in] r Rectangle to test (by value).
 *
 * @return true if @p r covers no pixels, false otherwise.
 * @retval true  @p r has a non-positive width or height.
 * @retval false @p r has positive width and height.
 *
 * @pre None.
 * @pre None.
 * @post No state is modified.
 * @post The result depends only on @p r's extents.
 *
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
static bool s_rect_empty(ra8_compositor_rect_t r)
{
  if (r.w <= 0) {
    return true;
  }
  if (r.h <= 0) {
    return true;
  }
  return false;
}

/**
 * @brief Intersect two rectangles, clamping to a non-negative result.
 *
 * @details Computes the overlapping rectangle (top-left inclusive, bottom-right
 * exclusive). When the inputs do not overlap the result is an empty rect with
 * clamped non-negative extents.
 *
 * @param[in] a First rectangle (by value).
 * @param[in] b Second rectangle (by value).
 *
 * @return The intersection of @p a and @p b.
 * @retval rect An axis-aligned rectangle with `w >= 0` and `h >= 0`.
 *
 * @pre None.
 * @pre None.
 * @post The result lies within both @p a and @p b.
 * @post The result has non-negative width and height.
 *
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
static ra8_compositor_rect_t s_rect_intersect(ra8_compositor_rect_t a, ra8_compositor_rect_t b)
{
  int32_t x0 = a.x;
  if (b.x > x0) {
    x0 = b.x;
  }
  int32_t y0 = a.y;
  if (b.y > y0) {
    y0 = b.y;
  }
  int32_t x1 = a.x + a.w;
  if ((b.x + b.w) < x1) {
    x1 = b.x + b.w;
  }
  int32_t y1 = a.y + a.h;
  if ((b.y + b.h) < y1) {
    y1 = b.y + b.h;
  }
  ra8_compositor_rect_t out = {x0, y0, x1 - x0, y1 - y0};
  if (out.w < 0) {
    out.w = 0;
  }
  if (out.h < 0) {
    out.h = 0;
  }
  return out;
}

/**
 * @brief Grow a running bounding box to also enclose another rectangle.
 *
 * @details An empty accumulator is replaced by @p r; an empty @p r leaves the
 * accumulator unchanged; otherwise the result is the smallest rect enclosing
 * both. This builds the damage union one dirty node at a time.
 *
 * @param[in] acc Running bounding box (by value; empty means "nothing yet").
 * @param[in] r   Rectangle to fold in (by value).
 *
 * @return The bounding box enclosing @p acc and @p r.
 * @retval rect The union rectangle (empty only if both inputs were empty).
 *
 * @pre None.
 * @pre None.
 * @post The result encloses every non-empty input.
 * @post The result has non-negative width and height.
 *
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
static ra8_compositor_rect_t s_rect_union(ra8_compositor_rect_t acc, ra8_compositor_rect_t r)
{
  if (s_rect_empty(r)) {
    return acc;
  }
  if (s_rect_empty(acc)) {
    return r;
  }
  int32_t x0 = acc.x;
  if (r.x < x0) {
    x0 = r.x;
  }
  int32_t y0 = acc.y;
  if (r.y < y0) {
    y0 = r.y;
  }
  int32_t x1 = acc.x + acc.w;
  if ((r.x + r.w) > x1) {
    x1 = r.x + r.w;
  }
  int32_t y1 = acc.y + acc.h;
  if ((r.y + r.h) > y1) {
    y1 = r.y + r.h;
  }
  ra8_compositor_rect_t out = {x0, y0, x1 - x0, y1 - y0};
  return out;
}

/* ===========================================================================
 * Layout helpers
 * ===========================================================================
 */

/**
 * @brief Split a flex weight's share of the leftover main-axis space.
 *
 * @details Returns 0 when there is no flex weight to divide; otherwise the
 * integer share `flex_len * weight / sum_flex`. Integer division is
 * deterministic, so the same scene always lays out identically.
 *
 * @param[in] flex_len Leftover main-axis space for flex children (px, >= 0).
 * @param[in] weight   This child's flex weight.
 * @param[in] sum_flex Total flex weight across the flex children.
 *
 * @return The child's main-axis extent in pixels.
 * @retval 0   @p sum_flex is non-positive (no flex space to share).
 * @retval >=0 The proportional share otherwise.
 *
 * @pre @p flex_len is non-negative.
 * @pre @p sum_flex matches the children being placed.
 * @post The result is non-negative.
 * @post The result is 0 when there is no flex weight.
 *
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
static int32_t s_flex_extent(int32_t flex_len, int32_t weight, int32_t sum_flex)
{
  if (sum_flex <= 0) {
    return 0;
  }
  return (flex_len * weight) / sum_flex;
}

/**
 * @brief Tally a container's visible direct children for tiling.
 *
 * @details One bounded pass over the pool: counts the visible children whose
 * `parent` is @p pi and sums their fixed extents and flex weights separately.
 *
 * @param[in]  scene     Scene being laid out (non-NULL by construction).
 * @param[in]  pi        Parent node index.
 * @param[out] out_n     Receives the visible child count.
 * @param[out] out_fixed Receives the summed fixed main-axis extents.
 * @param[out] out_flex  Receives the summed flex weights.
 *
 * @return Nothing.
 *
 * @pre @p scene and the output pointers are valid (callers pass locals).
 * @pre @p pi is a valid node index.
 * @post `*out_n` counts the visible children of @p pi.
 * @post `*out_fixed` + `*out_flex` describe the main-axis demand.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void s_measure_children(const ra8_compositor_t* scene,
                               uint16_t                pi,
                               int32_t*                out_n,
                               int32_t*                out_fixed,
                               int32_t*                out_flex)
{
  int32_t n  = 0;
  int32_t sf = 0;
  int32_t sx = 0;
  for (uint16_t j = 0U; j < scene->count; ++j) {
    if (scene->nodes[j].parent != pi) {
      continue;
    }
    if (!scene->nodes[j].visible) {
      continue;
    }
    n += 1;
    if (scene->nodes[j].fixed > 0) {
      sf += (int32_t)scene->nodes[j].fixed;
    } else {
      sx += (int32_t)scene->nodes[j].flex;
    }
  }
  *out_n     = n;
  *out_fixed = sf;
  *out_flex  = sx;
}

/**
 * @brief Assign one child's bounds at a main-axis offset within a content box.
 *
 * @details Lays the child across the cross axis and at @p offset along the main
 * axis with the given @p extent. @p axis_row selects which framebuffer axis is
 * the main one (non-zero = horizontal row, zero = vertical column).
 *
 * @param[in,out] c        Child node to position (non-NULL by construction).
 * @param[in]     content  Parent content rectangle (by value).
 * @param[in]     axis_row Non-zero to stack along X, zero to stack along Y.
 * @param[in]     offset   Main-axis offset from the content origin (px).
 * @param[in]     extent   Main-axis size of the child (px).
 * @param[in]     cross    Cross-axis size of the child (px).
 *
 * @return Nothing.
 *
 * @pre @p c is a valid node (callers pass `&scene->nodes[j]`).
 * @pre @p content has non-negative extents.
 * @post @p c->bounds is set within @p content along the chosen axis.
 * @post @p c->bounds has the requested extent and cross size.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void s_place_child(ra8_compositor_widget_t* c,
                          ra8_compositor_rect_t    content,
                          int32_t                  axis_row,
                          int32_t                  offset,
                          int32_t                  extent,
                          int32_t                  cross)
{
  if (axis_row != 0) {
    c->bounds.x = content.x + offset;
    c->bounds.y = content.y;
    c->bounds.w = extent;
    c->bounds.h = cross;
    return;
  }
  c->bounds.x = content.x;
  c->bounds.y = content.y + offset;
  c->bounds.w = cross;
  c->bounds.h = extent;
}

/**
 * @brief Tile a node's visible children inside its padded content rectangle.
 *
 * @details Computes the content rect (bounds inset by `pad`), tallies the
 * children, derives the flex space left after fixed extents and gaps, then
 * places each visible child in order along the container's axis. A node with no
 * visible children is left untouched.
 *
 * @param[in,out] scene Scene being laid out (non-NULL by construction).
 * @param[in]     pi    Index of the node whose children are tiled.
 *
 * @return Nothing.
 *
 * @pre @p scene is valid and @p pi < scene->count (guaranteed by the caller).
 * @pre @p scene->nodes[pi].bounds is already resolved.
 * @post Every visible child of @p pi has bounds within the content rectangle.
 * @post Nodes that are not children of @p pi are untouched.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void s_tile_children(ra8_compositor_t* scene, uint16_t pi)
{
  const ra8_compositor_widget_t* p       = &scene->nodes[pi];
  int32_t                        pad     = (int32_t)p->pad;
  ra8_compositor_rect_t          content = {
    p->bounds.x + pad,
    p->bounds.y + pad,
    p->bounds.w - (k_ra8_compositor_edges * pad),
    p->bounds.h - (k_ra8_compositor_edges * pad),
  };
  if (content.w < 0) {
    content.w = 0;
  }
  if (content.h < 0) {
    content.h = 0;
  }

  int32_t n_children = 0;
  int32_t sum_fixed  = 0;
  int32_t sum_flex   = 0;
  s_measure_children(scene, pi, &n_children, &sum_fixed, &sum_flex);
  if (n_children == 0) {
    return;
  }

  int32_t axis_row = (p->axis == k_ra8_compositor_axis_row) ? 1 : 0;
  int32_t main_len = (axis_row != 0) ? content.w : content.h;
  int32_t cross    = (axis_row != 0) ? content.h : content.w;
  int32_t flex_len = main_len - sum_fixed - ((n_children - 1) * (int32_t)p->gap);
  if (flex_len < 0) {
    flex_len = 0;
  }

  int32_t cursor = 0;
  for (uint16_t j = 0U; j < scene->count; ++j) {
    if (scene->nodes[j].parent != pi) {
      continue;
    }
    if (!scene->nodes[j].visible) {
      continue;
    }
    int32_t ext = (int32_t)scene->nodes[j].fixed;
    if (ext <= 0) {
      ext = s_flex_extent(flex_len, (int32_t)scene->nodes[j].flex, sum_flex);
    }
    s_place_child(&scene->nodes[j], content, axis_row, cursor, ext, cross);
    cursor += ext + (int32_t)p->gap;
  }
}

/* ===========================================================================
 * Paint helper
 * ===========================================================================
 */

/**
 * @brief Paint one node into the ra8_gfx framebuffer, clipped to its bounds.
 *
 * @details Containers paint nothing. Other kinds are clipped to the
 * intersection of their bounds and @p clip (so a node never spills its
 * rectangle), then a rect fills its bounds and a label additionally draws its
 * text. A node clipped fully away is a no-op.
 *
 * @param[in] n    Node to paint (non-NULL by construction; visible).
 * @param[in] clip Outer clip rectangle (by value).
 *
 * @return Nothing.
 *
 * @pre @p n is a valid, visible node (callers pass `&scene->nodes[i]`).
 * @pre `ra8_gfx_init` has bound a framebuffer.
 * @post Only pixels inside @p n->bounds and @p clip are modified.
 * @post Containers and fully-clipped nodes modify nothing.
 *
 * @note Not thread-safe; uses the global ra8_gfx clip state.
 * @since 0.1.0
 */
static void s_paint_node(const ra8_compositor_widget_t* n, ra8_compositor_rect_t clip)
{
  if (n->kind == k_ra8_compositor_kind_container) {
    return;
  }
  ra8_compositor_rect_t eff = s_rect_intersect(n->bounds, clip);
  if (s_rect_empty(eff)) {
    return;
  }
  (void)ra8_gfx_set_clip(eff.x, eff.y, eff.w, eff.h);
  (void)ra8_gfx_rect(n->bounds.x, n->bounds.y, n->bounds.w, n->bounds.h, n->color, true);
  if (n->kind != k_ra8_compositor_kind_label) {
    return;
  }
  if (n->text == nullptr) {
    return;
  }
  (void)ra8_gfx_text_out(n->bounds.x, n->bounds.y, n->text, &ra8_gfx_font_8x16, n->fg, n->color);
}

/* ===========================================================================
 * Scene construction
 * ===========================================================================
 */

ra8_err_t ra8_compositor_init(ra8_compositor_t* scene, ra8_compositor_widget_t* pool, uint16_t cap)
{
  RA8_CHECK_NULL_PTR(scene, s_tag, "scene must not be nullptr");
  RA8_CHECK_NULL_PTR(pool, s_tag, "pool must not be nullptr");
  if (cap == 0U) {
    return k_ra8_err_invalid_arg;
  }
  scene->nodes = pool;
  scene->cap   = cap;
  scene->count = 0U;
  return k_ra8_ok;
}

ra8_err_t ra8_compositor_add(ra8_compositor_t*              scene,
                             const ra8_compositor_widget_t* widget,
                             uint16_t                       parent,
                             uint16_t*                      out_index)
{
  RA8_CHECK_NULL_PTR(scene, s_tag, "scene must not be nullptr");
  RA8_CHECK_NULL_PTR(widget, s_tag, "widget must not be nullptr");
  RA8_CHECK_NULL_PTR(out_index, s_tag, "out_index must not be nullptr");
  if (scene->count >= scene->cap) {
    return k_ra8_err_no_mem;
  }
  if (scene->count == 0U) {
    if (parent != (uint16_t)k_ra8_compositor_no_parent) {
      return k_ra8_err_invalid_arg;
    }
  } else {
    if (parent >= scene->count) {
      return k_ra8_err_invalid_arg;
    }
  }
  uint16_t idx             = scene->count;
  scene->nodes[idx]        = *widget;
  scene->nodes[idx].parent = parent;
  scene->count             = (uint16_t)(idx + 1U);
  *out_index               = idx;
  return k_ra8_ok;
}

/* ===========================================================================
 * Layout
 * ===========================================================================
 */

ra8_err_t ra8_compositor_layout(ra8_compositor_t* scene, const ra8_compositor_rect_t* frame)
{
  RA8_CHECK_NULL_PTR(scene, s_tag, "scene must not be nullptr");
  RA8_CHECK_NULL_PTR(frame, s_tag, "frame must not be nullptr");
  if (scene->count == 0U) {
    return k_ra8_err_invalid_arg;
  }
  scene->nodes[0].bounds = *frame;
  for (uint16_t i = 0U; i < scene->count; ++i) {
    if (!scene->nodes[i].visible) {
      continue;
    }
    s_tile_children(scene, i);
  }
  return k_ra8_ok;
}

/* ===========================================================================
 * Compositing
 * ===========================================================================
 */

ra8_err_t ra8_compositor_render(const ra8_compositor_t* scene, const ra8_compositor_rect_t* clip)
{
  RA8_CHECK_NULL_PTR(scene, s_tag, "scene must not be nullptr");
  if (scene->count == 0U) {
    return k_ra8_err_invalid_arg;
  }
  ra8_compositor_rect_t outer = scene->nodes[0].bounds;
  if (clip != nullptr) {
    outer = *clip;
  }
  for (uint16_t i = 0U; i < scene->count; ++i) {
    if (!scene->nodes[i].visible) {
      continue;
    }
    s_paint_node(&scene->nodes[i], outer);
  }
  (void)ra8_gfx_reset_clip();
  return k_ra8_ok;
}

ra8_err_t ra8_compositor_damage(const ra8_compositor_t* scene,
                                ra8_compositor_rect_t*  out_rect,
                                uint16_t*               out_count)
{
  RA8_CHECK_NULL_PTR(scene, s_tag, "scene must not be nullptr");
  RA8_CHECK_NULL_PTR(out_rect, s_tag, "out_rect must not be nullptr");
  RA8_CHECK_NULL_PTR(out_count, s_tag, "out_count must not be nullptr");
  ra8_compositor_rect_t acc = {};
  uint16_t              n   = 0U;
  for (uint16_t i = 0U; i < scene->count; ++i) {
    if (!scene->nodes[i].visible) {
      continue;
    }
    if (!scene->nodes[i].dirty) {
      continue;
    }
    acc = s_rect_union(acc, scene->nodes[i].bounds);
    n += 1U;
  }
  *out_rect  = acc;
  *out_count = n;
  return k_ra8_ok;
}

ra8_err_t ra8_compositor_invalidate(ra8_compositor_t* scene, uint16_t index)
{
  RA8_CHECK_NULL_PTR(scene, s_tag, "scene must not be nullptr");
  if (index >= scene->count) {
    return k_ra8_err_invalid_arg;
  }
  scene->nodes[index].dirty = true;
  return k_ra8_ok;
}

ra8_err_t ra8_compositor_clear_dirty(ra8_compositor_t* scene)
{
  RA8_CHECK_NULL_PTR(scene, s_tag, "scene must not be nullptr");
  if (scene->count > scene->cap) {
    return k_ra8_err_invalid_state;
  }
  for (uint16_t i = 0U; i < scene->count; ++i) {
    scene->nodes[i].dirty = false;
  }
  return k_ra8_ok;
}

ra8_err_t ra8_compositor_compose_full(ra8_compositor_t* scene, const ra8_compositor_rect_t* frame)
{
  RA8_CHECK_NULL_PTR(scene, s_tag, "scene must not be nullptr");
  RA8_CHECK_NULL_PTR(frame, s_tag, "frame must not be nullptr");
  ra8_err_t err = ra8_compositor_layout(scene, frame);
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_compositor_render(scene, nullptr);
  if (err != k_ra8_ok) {
    return err;
  }
  return ra8_compositor_clear_dirty(scene);
}

ra8_err_t ra8_compositor_compose_dirty(ra8_compositor_t*            scene,
                                       const ra8_compositor_rect_t* frame,
                                       ra8_compositor_rect_t*       out_damage,
                                       uint16_t*                    out_count)
{
  RA8_CHECK_NULL_PTR(scene, s_tag, "scene must not be nullptr");
  RA8_CHECK_NULL_PTR(frame, s_tag, "frame must not be nullptr");
  RA8_CHECK_NULL_PTR(out_damage, s_tag, "out_damage must not be nullptr");
  RA8_CHECK_NULL_PTR(out_count, s_tag, "out_count must not be nullptr");
  ra8_err_t err = ra8_compositor_layout(scene, frame);
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_compositor_damage(scene, out_damage, out_count);
  if (err != k_ra8_ok) {
    return err;
  }
  if (*out_count > 0U) {
    err = ra8_compositor_render(scene, out_damage);
    if (err != k_ra8_ok) {
      return err;
    }
  }
  return ra8_compositor_clear_dirty(scene);
}
