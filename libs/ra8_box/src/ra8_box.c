/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_box.c
 * @brief Implementation of the bounded box-model layout engine.
 *
 * @par Tag
 * [Ring 5 / UI] {World: NS}
 *
 * @details
 * Single forward-pass layout (no recursion, P10 Rule 1). See ra8_box.h
 * for the contract. The engine computes geometry only; colours/tags are
 * carried through untouched for the renderer.
 *
 * @since 0.1.0
 */

#include "ra8_box.h"

#include <stddef.h>
#include <stdint.h>

#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_ui.h"

/** @brief Component tag for diagnostic logging. */
static const char* const s_tag = "ra8_box";

/**
 * @brief Inset a rectangle by a uniform padding, clamping size to >= 0.
 *
 * @details
 * Shifts the origin in by @p pad on each side and shrinks width/height by
 * twice @p pad, flooring both at zero so an over-large padding collapses
 * the content box rather than producing a negative extent.
 *
 * @param[in] outer Rectangle to inset.
 * @param[in] pad   Padding applied to all four sides.
 * @return The content rectangle.
 * @retval rect Padded rectangle with width/height clamped to >= 0.
 *
 * @pre None.
 * @pre None.
 * @post Returned width/height are never negative.
 * @post Returned origin is shifted in by @p pad.
 *
 * @note Pure.
 * @since 0.1.0
 */
static ra8_ui_rect_t internal_inset(ra8_ui_rect_t outer, int32_t pad)
{
  ra8_ui_rect_t r;
  r.x = outer.x + pad;
  r.y = outer.y + pad;
  r.w = outer.w - (2 * pad);
  r.h = outer.h - (2 * pad);
  if (r.w < 0) {
    r.w = 0;
  }
  if (r.h < 0) {
    r.h = 0;
  }
  return r;
}

/**
 * @brief Child-walk loop guard: link is live and the bound is not hit.
 *
 * @details
 * Centralises the "link is live, and the iteration bound is not yet
 * reached" test used by every child / sibling traversal, so the compound
 * decision lives in one place (one MC/DC obligation). The bound arm is a
 * defensive guard
 * against a malformed cyclic link; ra8_box_add never builds a cycle, so it
 * only ever evaluates true on a well-formed tree.
 *
 * @param[in] link  Next node index, or k_ra8_box_none.
 * @param[in] guard Iteration counter.
 * @param[in] count Node count (hard upper bound on iterations).
 * @return true to continue the walk, false to stop.
 * @retval true  The link is live and the bound is not yet reached.
 * @retval false The link is none, or the iteration bound was hit.
 *
 * @pre None.
 * @pre None.
 * @post No state modified.
 * @post Returns false once @p link is none or @p guard reaches @p count.
 *
 * @note Pure.
 * @since 0.1.0
 */
static bool internal_iter_live(int32_t link, uint16_t guard, uint16_t count)
{
  /*
   * The (guard < count) clause is a NASA Power-of-10 Rule 2 defensive iteration
   * bound. Every sibling/child chain built through the public ra8_box API is
   * acyclic and terminates when `link` becomes k_ra8_box_none, before `guard`
   * can ever reach `count`. guard >= count is reachable only on a corrupted
   * (cyclic) tree that no public-API sequence produces, so the second condition
   * cannot be given independent MC/DC influence on any reachable path.
   */
  /* mcdc-deactivated: guard<count is an acyclic-tree cycle bound; guard>=count is unreachable via the public API. */
  return (link != (int32_t)k_ra8_box_none) && (guard < count);
}

/**
 * @brief Sum child count, fixed extent, and flex weight of a container.
 *
 * @details
 * One bounded walk of the child chain: fixed children (fixed greater than
 * zero) contribute to the fixed total, the rest contribute their flex
 * weight. The result drives flex-space distribution in the stack/grid
 * passes.
 *
 * @param[in]  tree         Tree being laid out.
 * @param[in]  parent       Container node index.
 * @param[out] out_count    Receives the child count.
 * @param[out] out_fixed    Receives the total fixed main-axis extent.
 * @param[out] out_flex     Receives the total flex weight.
 *
 * @pre All pointers non-NULL; @p parent valid.
 * @pre Child links terminate (guarded by the node count).
 * @post The three out-params summarise the direct children.
 * @post No node is modified.
 *
 * @note Pure read.
 * @since 0.1.0
 */
static void internal_tally(const ra8_box_tree_t* tree,
                           int16_t               parent,
                           uint16_t*             out_count,
                           int32_t*              out_fixed,
                           uint32_t*             out_flex)
{
  uint16_t n     = 0U;
  int32_t  fixed = 0;
  uint32_t flex  = 0U;
  int32_t  c     = tree->nodes[parent].first_child;
  for (uint16_t guard = 0U; internal_iter_live(c, guard, tree->count); ++guard) {
    const ra8_box_t* child = &tree->nodes[c];
    if (child->fixed > 0) {
      fixed += (int32_t)child->fixed;
    } else {
      flex += (uint32_t)child->flex;
    }
    n++;
    c = child->next;
  }
  *out_count = n;
  *out_fixed = fixed;
  *out_flex  = flex;
}

/**
 * @brief Lay out a stack container's children along one axis.
 *
 * @details
 * Insets the container by its padding, tallies fixed extent and flex
 * weight, then walks the children placing each at a running cursor:
 * fixed children take their `fixed` size, flex children split the
 * leftover space by weight, and `gap` separates them. The cross axis
 * fills the content box.
 *
 * @param[in,out] tree       Tree being laid out.
 * @param[in]     parent     Stack node index (rect already set).
 * @param[in]     horizontal true for a row (main axis X), false a column.
 *
 * @pre @p tree non-NULL; @p parent a valid stack node with rect set.
 * @pre Child links terminate.
 * @post Each child's `rect` is positioned along the main axis.
 * @post Fixed children take `fixed`; the rest split leftover by `flex`.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void internal_layout_stack(ra8_box_tree_t* tree, int16_t parent, bool horizontal)
{
  const ra8_box_t*    p    = &tree->nodes[parent];
  const ra8_ui_rect_t cont = internal_inset(p->rect, (int32_t)p->pad);
  const int32_t       gap  = (int32_t)p->gap;

  uint16_t n     = 0U;
  int32_t  fixed = 0;
  uint32_t flex  = 0U;
  internal_tally(tree, parent, &n, &fixed, &flex);

  const int32_t main_extent = horizontal ? cont.w : cont.h;
  const int32_t gaps        = (n > 0U) ? ((int32_t)(n - 1U) * gap) : 0;
  int32_t       flex_space  = main_extent - fixed - gaps;
  if (flex_space < 0) {
    flex_space = 0;
  }

  int32_t cursor = horizontal ? cont.x : cont.y;
  int32_t c      = p->first_child;
  for (uint16_t guard = 0U; internal_iter_live(c, guard, tree->count); ++guard) {
    ra8_box_t* child = &tree->nodes[c];
    int32_t    main_sz;
    if (child->fixed > 0) {
      main_sz = (int32_t)child->fixed;
    } else if (flex > 0U) {
      main_sz = (flex_space * (int32_t)child->flex) / (int32_t)flex;
    } else {
      main_sz = 0;
    }
    if (horizontal) {
      child->rect.x = cursor;
      child->rect.y = cont.y;
      child->rect.w = main_sz;
      child->rect.h = cont.h;
    } else {
      child->rect.x = cont.x;
      child->rect.y = cursor;
      child->rect.w = cont.w;
      child->rect.h = main_sz;
    }
    cursor += main_sz + gap;
    c = child->next;
  }
}

/**
 * @brief Lay out a grid container's children row-major.
 *
 * @details
 * Insets by padding, derives a uniform cell width from the column count
 * and gap, and a uniform cell height from the implied row count (ceil of
 * child count over columns). Children are placed left-to-right, top-to-
 * bottom; a child with a fixed extent keeps that as its height.
 *
 * @param[in,out] tree   Tree being laid out.
 * @param[in]     parent Grid node index (rect already set).
 *
 * @pre @p tree non-NULL; @p parent a valid grid node with rect set.
 * @pre Child links terminate.
 * @post Each child sits in its row-major cell; fixed children keep their
 *       own height, otherwise rows share the content height evenly.
 * @post Cell width/height are floored at zero for tiny content boxes.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void internal_layout_grid(ra8_box_tree_t* tree, int16_t parent)
{
  const ra8_box_t*    p    = &tree->nodes[parent];
  const ra8_ui_rect_t cont = internal_inset(p->rect, (int32_t)p->pad);
  const int32_t       gap  = (int32_t)p->gap;
  const int32_t       cols = (p->grid_cols >= 1U) ? (int32_t)p->grid_cols : 1;

  uint16_t n     = 0U;
  int32_t  fixed = 0;
  uint32_t flex  = 0U;
  internal_tally(tree, parent, &n, &fixed, &flex);

  const int32_t rows    = ((int32_t)n + cols - 1) / cols;
  const int32_t row_cnt = (rows > 0) ? rows : 1;
  int32_t       cell_w  = (cont.w - ((cols - 1) * gap)) / cols;
  if (cell_w < 0) {
    cell_w = 0;
  }
  int32_t cell_h = (cont.h - ((row_cnt - 1) * gap)) / row_cnt;
  if (cell_h < 0) {
    cell_h = 0;
  }

  int32_t idx = 0;
  int32_t c   = p->first_child;
  for (uint16_t guard = 0U; internal_iter_live(c, guard, tree->count); ++guard) {
    ra8_box_t*    child = &tree->nodes[c];
    const int32_t col   = idx % cols;
    const int32_t row   = idx / cols;
    child->rect.x       = cont.x + (col * (cell_w + gap));
    child->rect.y       = cont.y + (row * (cell_h + gap));
    child->rect.w       = cell_w;
    child->rect.h       = (child->fixed > 0) ? (int32_t)child->fixed : cell_h;
    idx++;
    c = child->next;
  }
}

ra8_err_t ra8_box_tree_init(ra8_box_tree_t* tree, ra8_box_t* storage, uint16_t cap)
{
  RA8_CHECK_NULL_PTR(tree, s_tag, "tree must not be nullptr");
  RA8_CHECK_NULL_PTR(storage, s_tag, "storage must not be nullptr");
  if (cap == 0U) {
    return k_ra8_err_invalid_arg;
  }
  tree->nodes = storage;
  tree->cap   = cap;
  tree->count = 0U;
  return k_ra8_ok;
}

int16_t ra8_box_add(ra8_box_tree_t* tree, int16_t parent, const ra8_box_t* node)
{
  if ((tree == nullptr) || (node == nullptr)) {
    return (int16_t)k_ra8_box_none;
  }
  if (tree->count >= tree->cap) {
    return (int16_t)k_ra8_box_none;
  }
  if ((parent != (int16_t)k_ra8_box_none) && ((parent < 0) || (parent >= (int16_t)tree->count))) {
    return (int16_t)k_ra8_box_none;
  }

  const int16_t idx            = (int16_t)tree->count;
  tree->nodes[idx]             = *node;
  tree->nodes[idx].first_child = (int32_t)k_ra8_box_none;
  tree->nodes[idx].next        = (int32_t)k_ra8_box_none;
  tree->count++;

  if (parent != (int16_t)k_ra8_box_none) {
    ra8_box_t* par = &tree->nodes[parent];
    if (par->first_child == (int32_t)k_ra8_box_none) {
      par->first_child = (int32_t)idx;
    } else {
      int32_t sib = par->first_child;
      for (uint16_t guard = 0U; internal_iter_live(tree->nodes[sib].next, guard, tree->count);
           ++guard) {
        sib = tree->nodes[sib].next;
      }
      tree->nodes[sib].next = (int32_t)idx;
    }
  }
  return idx;
}

ra8_err_t ra8_box_layout(ra8_box_tree_t* tree, int16_t root, const ra8_ui_rect_t* frame)
{
  RA8_CHECK_NULL_PTR(tree, s_tag, "tree must not be nullptr");
  RA8_CHECK_NULL_PTR(frame, s_tag, "frame must not be nullptr");
  if ((tree->count == 0U) || (root < 0) || (root >= (int16_t)tree->count)) {
    return k_ra8_err_invalid_arg;
  }

  tree->nodes[root].rect = *frame;
  for (uint16_t i = 0U; i < tree->count; ++i) {
    switch ((ra8_box_kind_t)tree->nodes[i].kind) {
      case k_ra8_box_stack_v:
        internal_layout_stack(tree, (int16_t)i, false);
        break;
      case k_ra8_box_stack_h:
        internal_layout_stack(tree, (int16_t)i, true);
        break;
      case k_ra8_box_grid:
        internal_layout_grid(tree, (int16_t)i);
        break;
      case k_ra8_box_leaf:
      default:
        break;
    }
  }
  return k_ra8_ok;
}
