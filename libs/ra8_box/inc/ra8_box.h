/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_box.h
 * @brief Bounded, allocation-free box-model layout for e-reader chrome.
 * @ingroup grp_ereader
 *
 * @details
 * `ra8_box` is the minimal CSS-style box model the e-reader chrome needs
 * (issue #80, #76). The full document engine (`ra8_reflow`) reflows
 * arbitrary book XHTML; the *chrome* (library grid, reading bars, menus)
 * is instead a small fixed box tree laid out once per screen. This is
 * the only "web-like" layout compatible with the no-malloc / bounded /
 * MC/DC bar: the screen is a caller-owned array of nodes, the layout is
 * a single forward pass (no recursion, P10 Rule 1), and the engine
 * produces only positioned rectangles -- it never touches a font or a
 * framebuffer, so the same layout runs on the host test harness and on
 * the RA8D2, and the renderer (`ra8_gfx`) and hit-tester (`ra8_ui`) are
 * layered on top.
 *
 * Supported box kinds: a vertical stack (column), a horizontal stack
 * (row), a fixed-column grid, and a leaf. Children size either to a
 * fixed extent along the parent's main axis or to a flex weight that
 * splits the leftover space. Padding insets a container's content; gap
 * separates its children. Fill / border colours are carried on each
 * node for the renderer to consume; the engine itself only computes
 * geometry.
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

/* ===========================================================================
 * Sentinels
 * ===========================================================================
 */

/**
 * @enum ra8_box_const_t
 * @brief Box-tree sentinels and the "no colour" marker.
 */
typedef enum : int32_t {
  k_ra8_box_none      = -1,         /**< No child / no sibling link.  */
  k_ra8_box_no_colour = (int32_t)0, /**< Carried fill/border: absent. */
} ra8_box_const_t;

/**
 * @enum ra8_box_kind_t
 * @brief Box layout kind.
 */
typedef enum : uint8_t {
  k_ra8_box_stack_v = 0U, /**< Vertical stack: children stack top->bottom. */
  k_ra8_box_stack_h = 1U, /**< Horizontal stack: children left->right.     */
  k_ra8_box_grid    = 2U, /**< Fixed-column grid, row-major.               */
  k_ra8_box_leaf    = 3U, /**< Terminal box (no children laid out).        */
} ra8_box_kind_t;

/* ===========================================================================
 * Node + tree
 * ===========================================================================
 */

/**
 * @struct ra8_box_t
 * @brief One node in a box tree (caller-owned).
 *
 * @details
 * `rect` is the layout *output*. The tree links (`first_child`, `next`)
 * are maintained by ::ra8_box_add. Sizing along the parent's main axis is
 * `fixed` when `fixed > 0`, otherwise `flex` weight (0 flex = collapse).
 * `fill` / `border` are opaque 0xRRGGBB values the renderer reads;
 * `k_ra8_box_no_colour` (0, i.e. black) is reserved as "none" -- chrome
 * uses the grayscale ramp where pure black is `ink`, so leaves that need
 * a true black fill should use `k_ra8_box_ink` instead.
 */
typedef struct {
  uint8_t       kind;        /**< ra8_box_kind_t.                            */
  uint8_t       grid_cols;   /**< Columns for k_ra8_box_grid (>= 1).         */
  int16_t       fixed;       /**< Fixed main-axis extent; 0 => use flex.     */
  uint16_t      flex;        /**< Flex weight when not fixed.                */
  int16_t       pad;         /**< Inner padding (all four sides).            */
  int16_t       gap;         /**< Gap between children.                      */
  uint32_t      fill;        /**< Background colour, or k_ra8_box_no_colour. */
  uint32_t      border;      /**< Border colour, or k_ra8_box_no_colour.     */
  int16_t       border_w;    /**< Border width (0 = no border draw).         */
  int16_t       tag;         /**< Caller tag (e.g. ra8_ui action id), or -1. */
  int32_t       first_child; /**< First child index, or k_ra8_box_none.      */
  int32_t       next;        /**< Next sibling index, or k_ra8_box_none.     */
  ra8_ui_rect_t rect;        /**< OUTPUT: computed rectangle.                */
} ra8_box_t;

/**
 * @struct ra8_box_tree_t
 * @brief Append-only builder over caller-owned node storage.
 *
 * @details
 * Nodes are appended in construction order, so a parent always has a
 * lower index than its children -- which is what lets ::ra8_box_layout run
 * as a single in-order forward pass with no recursion.
 */
typedef struct {
  ra8_box_t* nodes; /**< Caller storage.      */
  uint16_t   cap;   /**< Capacity of `nodes`. */
  uint16_t   count; /**< Nodes used.          */
} ra8_box_tree_t;

/* ===========================================================================
 * API
 * ===========================================================================
 */

/**
 * @brief Bind a tree builder to caller-owned node storage.
 *
 * @param[out] tree    Tree to initialise.
 * @param[in]  storage Node array owned by the caller.
 * @param[in]  cap     Number of nodes `storage` holds (>= 1).
 *
 * @return ra8_err_t
 * @retval k_ra8_ok             Initialised (empty tree).
 * @retval k_ra8_err_null_ptr   `tree` or `storage` is NULL.
 * @retval k_ra8_err_invalid_arg `cap` is 0.
 *
 * @pre `tree` and `storage` non-NULL; `cap >= 1`.
 * @pre None.
 * @post `tree->count == 0`.
 * @post `tree` references `storage`.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_box_tree_init(ra8_box_tree_t* tree, ra8_box_t* storage, uint16_t cap);

/**
 * @brief Append a node and link it as a child of @p parent.
 *
 * @details
 * Copies @p node into storage, resets its tree links, and appends it to
 * @p parent's child chain (or makes it a root when `parent ==
 * k_ra8_box_none`). Returns the new node's index for use as a parent in
 * later calls.
 *
 * @param[in,out] tree   Initialised tree.
 * @param[in]     parent Parent index, or `k_ra8_box_none` for a root.
 * @param[in]     node   Node template (kind / sizing / colours).
 *
 * @return New node index in `[0, cap)`, or `k_ra8_box_none` on error.
 *
 * @pre `tree` and `node` non-NULL; `tree` initialised.
 * @pre `parent` is `k_ra8_box_none` or a valid earlier index.
 * @post On success `tree->count` grew by one and the node is linked.
 * @post On overflow / bad parent the tree is unchanged.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] int16_t ra8_box_add(ra8_box_tree_t* tree, int16_t parent, const ra8_box_t* node);

/**
 * @brief Lay out the tree, filling every node's `rect`.
 *
 * @details
 * Sets `nodes[root].rect = *frame`, then makes one forward pass: each
 * container distributes its content box (its rect inset by `pad`) across
 * its children -- fixed children take their `fixed` extent, the rest
 * split the leftover by `flex` weight, separated by `gap`. Grids place
 * children row-major across `grid_cols`. Because parents precede children
 * in index order, a single in-order pass suffices (no recursion).
 *
 * @param[in,out] tree  Built tree (>= 1 node).
 * @param[in]     root  Index of the root node.
 * @param[in]     frame Outer rectangle the root fills.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok               Laid out; every reachable `rect` set.
 * @retval k_ra8_err_null_ptr     `tree` or `frame` is NULL.
 * @retval k_ra8_err_invalid_arg  `root` out of range, or empty tree.
 *
 * @pre `tree` and `frame` non-NULL; `0 <= root < tree->count`.
 * @pre Children indices exceed their parent's (ensured by ra8_box_add).
 * @post Every node reachable from `root` has its `rect` computed.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_box_layout(ra8_box_tree_t* tree, int16_t root, const ra8_ui_rect_t* frame);

#ifdef __cplusplus
}
#endif
