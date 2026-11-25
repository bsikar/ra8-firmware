/**
 * @file tests/zig_abi_pairs/ra8_box_pair/inc/ra8_box_pair_vectors.h
 * @brief C23 projection of the canonical `ra8_box` paired ABI behavior vectors.
 *
 * @details
 * Every value in this header mirrors `tests/zig_abi_pairs/ra8_box_pair/vectors.json`
 * one-to-one and is cross-checked by `verify_vectors_test.zig`. Both the C
 * firmware client and host verification consume this contract.
 *
 * @author Brighton Sikarskie
 * @date 2026-11-24
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "ra8_box.h"
#include "ra8_err.h"

/** @brief Shared bounds matching `constants` in `vectors.json`. */
typedef enum : uint32_t {
  k_ra8_box_pair_max_nodes     = 8U,          /**< Maximum nodes in one layout vector.     */
  k_ra8_box_pair_rect_size     = 16U,         /**< Expected sizeof(ra8_ui_rect_t).         */
  k_ra8_box_pair_node_size     = 48U,         /**< Expected sizeof(ra8_box_t).             */
  k_ra8_box_pair_err_count     = 13U,         /**< Number of error vectors.                */
  k_ra8_box_pair_layout_count  = 5U,          /**< Number of layout tree vectors.          */
  k_ra8_box_pair_stack_v_fill0 = 0xFF112233U, /**< Sample fill colour 0.           */
  k_ra8_box_pair_stack_v_bdr0  = 0xFF223344U, /**< Sample border colour 0.         */
  k_ra8_box_pair_stack_v_fill1 = 0xFFFF0000U, /**< Sample fill colour 1.           */
  k_ra8_box_pair_stack_v_fill2 = 0xFF00FF00U, /**< Sample fill colour 2.           */
  k_ra8_box_pair_stack_v_fill3 = 0xFF0000FFU, /**< Sample fill colour 3.           */
} ra8_box_pair_limits_t;

/** @brief Target C ABI operation for an error/guard vector. */
typedef enum : uint8_t {
  k_ra8_box_pair_op_tree_init = 0U, /**< Invoke `ra8_box_tree_init`. */
  k_ra8_box_pair_op_add       = 1U, /**< Invoke `ra8_box_add`.       */
  k_ra8_box_pair_op_layout    = 2U, /**< Invoke `ra8_box_layout`.    */
} ra8_box_pair_op_t;

/** @brief One shared error/boundary vector from `error_vectors`. */
typedef struct {
  const char*       id;           /**< Stable vector identifier matching JSON. */
  ra8_box_pair_op_t op;           /**< API function exercised.                 */
  bool              null_tree;    /**< Pass nullptr for `tree` argument.       */
  bool              null_arg;     /**< Pass nullptr for secondary pointer arg. */
  uint16_t          cap;          /**< Storage capacity to configure.          */
  uint16_t          seed_count;   /**< Nodes seeded before invoking `op`.      */
  int16_t           index_arg;    /**< `parent` (`add`) or `root` (`layout`).  */
  ra8_err_t         expected_err; /**< Expected status for err-returning ops.  */
  int16_t           expected_idx; /**< Expected index for index-returning ops. */
} ra8_box_pair_err_vec_t;

/** @brief Input descriptor and expected result for one node in a tree vector. */
typedef struct {
  int16_t       parent;               /**< Parent index or `k_ra8_box_none`.   */
  ra8_box_t     input;                /**< Node input passed to `ra8_box_add`. */
  int16_t       expected_index;       /**< Expected slot index from add().     */
  int32_t       expected_first_child; /**< Expected first-child link.          */
  int32_t       expected_next;        /**< Expected sibling link.              */
  ra8_ui_rect_t expected_rect;        /**< Expected rectangle after layout().  */
} ra8_box_pair_node_vec_t;

/** @brief One complete layout tree vector from `layout_vectors`. */
typedef struct {
  const char*             id;                              /**< Stable vector id.   */
  ra8_ui_rect_t           frame;                           /**< Root frame arg.     */
  int16_t                 root;                            /**< Root node index.    */
  uint16_t                node_count;                      /**< Active node count.  */
  ra8_box_pair_node_vec_t nodes[k_ra8_box_pair_max_nodes]; /**< Node vector specs.  */
} ra8_box_pair_layout_vec_t;

/** @brief Canonical shared error vectors matching `vectors.json`. */
static const ra8_box_pair_err_vec_t k_ra8_box_pair_err_vectors[k_ra8_box_pair_err_count] = {
  {
    .id           = "tree_init_null_tree",
    .op           = k_ra8_box_pair_op_tree_init,
    .null_tree    = true,
    .null_arg     = false,
    .cap          = 4U,
    .seed_count   = 0U,
    .index_arg    = 0,
    .expected_err = k_ra8_err_null_ptr,
    .expected_idx = (int16_t)k_ra8_box_none,
  },
  {
    .id           = "tree_init_null_storage",
    .op           = k_ra8_box_pair_op_tree_init,
    .null_tree    = false,
    .null_arg     = true,
    .cap          = 4U,
    .seed_count   = 0U,
    .index_arg    = 0,
    .expected_err = k_ra8_err_null_ptr,
    .expected_idx = (int16_t)k_ra8_box_none,
  },
  {
    .id           = "tree_init_zero_cap",
    .op           = k_ra8_box_pair_op_tree_init,
    .null_tree    = false,
    .null_arg     = false,
    .cap          = 0U,
    .seed_count   = 0U,
    .index_arg    = 0,
    .expected_err = k_ra8_err_invalid_arg,
    .expected_idx = (int16_t)k_ra8_box_none,
  },
  {
    .id           = "add_null_tree",
    .op           = k_ra8_box_pair_op_add,
    .null_tree    = true,
    .null_arg     = false,
    .cap          = 4U,
    .seed_count   = 0U,
    .index_arg    = (int16_t)k_ra8_box_none,
    .expected_err = k_ra8_ok,
    .expected_idx = (int16_t)k_ra8_box_none,
  },
  {
    .id           = "add_null_node",
    .op           = k_ra8_box_pair_op_add,
    .null_tree    = false,
    .null_arg     = true,
    .cap          = 4U,
    .seed_count   = 0U,
    .index_arg    = (int16_t)k_ra8_box_none,
    .expected_err = k_ra8_ok,
    .expected_idx = (int16_t)k_ra8_box_none,
  },
  {
    .id           = "add_bad_parent_neg",
    .op           = k_ra8_box_pair_op_add,
    .null_tree    = false,
    .null_arg     = false,
    .cap          = 4U,
    .seed_count   = 1U,
    .index_arg    = -2,
    .expected_err = k_ra8_ok,
    .expected_idx = (int16_t)k_ra8_box_none,
  },
  {
    .id           = "add_bad_parent_oob",
    .op           = k_ra8_box_pair_op_add,
    .null_tree    = false,
    .null_arg     = false,
    .cap          = 4U,
    .seed_count   = 1U,
    .index_arg    = 1,
    .expected_err = k_ra8_ok,
    .expected_idx = (int16_t)k_ra8_box_none,
  },
  {
    .id           = "add_overflow_cap",
    .op           = k_ra8_box_pair_op_add,
    .null_tree    = false,
    .null_arg     = false,
    .cap          = 1U,
    .seed_count   = 1U,
    .index_arg    = 0,
    .expected_err = k_ra8_ok,
    .expected_idx = (int16_t)k_ra8_box_none,
  },
  {
    .id           = "layout_null_tree",
    .op           = k_ra8_box_pair_op_layout,
    .null_tree    = true,
    .null_arg     = false,
    .cap          = 4U,
    .seed_count   = 1U,
    .index_arg    = 0,
    .expected_err = k_ra8_err_null_ptr,
    .expected_idx = (int16_t)k_ra8_box_none,
  },
  {
    .id           = "layout_null_frame",
    .op           = k_ra8_box_pair_op_layout,
    .null_tree    = false,
    .null_arg     = true,
    .cap          = 4U,
    .seed_count   = 1U,
    .index_arg    = 0,
    .expected_err = k_ra8_err_null_ptr,
    .expected_idx = (int16_t)k_ra8_box_none,
  },
  {
    .id           = "layout_empty_tree",
    .op           = k_ra8_box_pair_op_layout,
    .null_tree    = false,
    .null_arg     = false,
    .cap          = 4U,
    .seed_count   = 0U,
    .index_arg    = 0,
    .expected_err = k_ra8_err_invalid_arg,
    .expected_idx = (int16_t)k_ra8_box_none,
  },
  {
    .id           = "layout_bad_root_neg",
    .op           = k_ra8_box_pair_op_layout,
    .null_tree    = false,
    .null_arg     = false,
    .cap          = 4U,
    .seed_count   = 1U,
    .index_arg    = -1,
    .expected_err = k_ra8_err_invalid_arg,
    .expected_idx = (int16_t)k_ra8_box_none,
  },
  {
    .id           = "layout_bad_root_oob",
    .op           = k_ra8_box_pair_op_layout,
    .null_tree    = false,
    .null_arg     = false,
    .cap          = 4U,
    .seed_count   = 1U,
    .index_arg    = 1,
    .expected_err = k_ra8_err_invalid_arg,
    .expected_idx = (int16_t)k_ra8_box_none,
  },
};

/** @brief Canonical shared layout tree vectors matching `vectors.json`. */
static const ra8_box_pair_layout_vec_t
  k_ra8_box_pair_layout_vectors[k_ra8_box_pair_layout_count] =
    {
      {
        .id         = "stack_v_padded_fixed_flex",
        .frame      = {0, 0, 100, 200},
        .root       = 0,
        .node_count = 4U,
        .nodes =
          {
            {
              .parent = (int16_t)k_ra8_box_none,
              .input =
                {
                  .kind      = (uint8_t)k_ra8_box_stack_v,
                  .grid_cols = 1U,
                  .pad       = 10,
                  .gap       = 10,
                  .fill      = (uint32_t)k_ra8_box_pair_stack_v_fill0,
                  .border    = (uint32_t)k_ra8_box_pair_stack_v_bdr0,
                  .border_w  = 1,
                  .tag       = 100,
                },
              .expected_index       = 0,
              .expected_first_child = 1,
              .expected_next        = (int32_t)k_ra8_box_none,
              .expected_rect        = {0, 0, 100, 200},
            },
            {
              .parent = 0,
              .input =
                {
                  .kind  = (uint8_t)k_ra8_box_leaf,
                  .fixed = 30,
                  .fill  = (uint32_t)k_ra8_box_pair_stack_v_fill1,
                  .tag   = 101,
                },
              .expected_index       = 1,
              .expected_first_child = (int32_t)k_ra8_box_none,
              .expected_next        = 2,
              .expected_rect        = {10, 10, 80, 30},
            },
            {
              .parent = 0,
              .input =
                {
                  .kind = (uint8_t)k_ra8_box_leaf,
                  .flex = 1U,
                  .fill = (uint32_t)k_ra8_box_pair_stack_v_fill2,
                  .tag  = 102,
                },
              .expected_index       = 2,
              .expected_first_child = (int32_t)k_ra8_box_none,
              .expected_next        = 3,
              .expected_rect        = {10, 50, 80, 110},
            },
            {
              .parent = 0,
              .input =
                {
                  .kind  = (uint8_t)k_ra8_box_leaf,
                  .fixed = 20,
                  .fill  = (uint32_t)k_ra8_box_pair_stack_v_fill3,
                  .tag   = 103,
                },
              .expected_index       = 3,
              .expected_first_child = (int32_t)k_ra8_box_none,
              .expected_next        = (int32_t)k_ra8_box_none,
              .expected_rect        = {10, 170, 80, 20},
            },
          },
      },
      {
        .id         = "stack_h_weighted_flex",
        .frame      = {10, 20, 260, 60},
        .root       = 0,
        .node_count = 4U,
        .nodes =
          {
            {
              .parent = (int16_t)k_ra8_box_none,
              .input =
                {
                  .kind      = (uint8_t)k_ra8_box_stack_h,
                  .grid_cols = 1U,
                  .pad       = 10,
                  .gap       = 10,
                  .tag       = 200,
                },
              .expected_index       = 0,
              .expected_first_child = 1,
              .expected_next        = (int32_t)k_ra8_box_none,
              .expected_rect        = {10, 20, 260, 60},
            },
            {
              .parent = 0,
              .input =
                {
                  .kind     = (uint8_t)k_ra8_box_leaf,
                  .fixed    = 40,
                  .fill     = 1U,
                  .border   = 2U,
                  .border_w = 1,
                  .tag      = 201,
                },
              .expected_index       = 1,
              .expected_first_child = (int32_t)k_ra8_box_none,
              .expected_next        = 2,
              .expected_rect        = {20, 30, 40, 40},
            },
            {
              .parent = 0,
              .input =
                {
                  .kind     = (uint8_t)k_ra8_box_leaf,
                  .flex     = 1U,
                  .fill     = 3U,
                  .border   = 4U,
                  .border_w = 2,
                  .tag      = 202,
                },
              .expected_index       = 2,
              .expected_first_child = (int32_t)k_ra8_box_none,
              .expected_next        = 3,
              .expected_rect        = {70, 30, 60, 40},
            },
            {
              .parent = 0,
              .input =
                {
                  .kind     = (uint8_t)k_ra8_box_leaf,
                  .flex     = 2U,
                  .fill     = 5U,
                  .border   = 6U,
                  .border_w = 3,
                  .tag      = 203,
                },
              .expected_index       = 3,
              .expected_first_child = (int32_t)k_ra8_box_none,
              .expected_next        = (int32_t)k_ra8_box_none,
              .expected_rect        = {140, 30, 120, 40},
            },
          },
      },
      {
        .id         = "grid_2col_partial_row",
        .frame      = {0, 0, 100, 100},
        .root       = 0,
        .node_count = 4U,
        .nodes =
          {
            {
              .parent = (int16_t)k_ra8_box_none,
              .input =
                {
                  .kind      = (uint8_t)k_ra8_box_grid,
                  .grid_cols = 2U,
                  .gap       = 10,
                  .tag       = 300,
                },
              .expected_index       = 0,
              .expected_first_child = 1,
              .expected_next        = (int32_t)k_ra8_box_none,
              .expected_rect        = {0, 0, 100, 100},
            },
            {
              .parent               = 0,
              .input                = {.kind = (uint8_t)k_ra8_box_leaf, .tag = 301},
              .expected_index       = 1,
              .expected_first_child = (int32_t)k_ra8_box_none,
              .expected_next        = 2,
              .expected_rect        = {0, 0, 45, 45},
            },
            {
              .parent               = 0,
              .input                = {.kind = (uint8_t)k_ra8_box_leaf, .fixed = 25, .tag = 302},
              .expected_index       = 2,
              .expected_first_child = (int32_t)k_ra8_box_none,
              .expected_next        = 3,
              .expected_rect        = {55, 0, 45, 25},
            },
            {
              .parent               = 0,
              .input                = {.kind = (uint8_t)k_ra8_box_leaf, .tag = 303},
              .expected_index       = 3,
              .expected_first_child = (int32_t)k_ra8_box_none,
              .expected_next        = (int32_t)k_ra8_box_none,
              .expected_rect        = {0, 55, 45, 45},
            },
          },
      },
      {
        .id         = "nested_chrome_tree",
        .frame      = {0, 0, 320, 240},
        .root       = 0,
        .node_count = 8U,
        .nodes =
          {
            {
              .parent = (int16_t)k_ra8_box_none,
              .input =
                {
                  .kind      = (uint8_t)k_ra8_box_stack_v,
                  .grid_cols = 1U,
                  .pad       = 8,
                  .gap       = 8,
                  .tag       = 400,
                },
              .expected_index       = 0,
              .expected_first_child = 1,
              .expected_next        = (int32_t)k_ra8_box_none,
              .expected_rect        = {0, 0, 320, 240},
            },
            {
              .parent = 0,
              .input =
                {
                  .kind      = (uint8_t)k_ra8_box_stack_h,
                  .grid_cols = 1U,
                  .fixed     = 40,
                  .gap       = 4,
                  .tag       = 401,
                },
              .expected_index       = 1,
              .expected_first_child = 3,
              .expected_next        = 2,
              .expected_rect        = {8, 8, 304, 40},
            },
            {
              .parent = 0,
              .input =
                {
                  .kind      = (uint8_t)k_ra8_box_grid,
                  .grid_cols = 3U,
                  .flex      = 1U,
                  .gap       = 10,
                  .tag       = 402,
                },
              .expected_index       = 2,
              .expected_first_child = 5,
              .expected_next        = (int32_t)k_ra8_box_none,
              .expected_rect        = {8, 56, 304, 176},
            },
            {
              .parent               = 1,
              .input                = {.kind = (uint8_t)k_ra8_box_leaf, .fixed = 60, .tag = 403},
              .expected_index       = 3,
              .expected_first_child = (int32_t)k_ra8_box_none,
              .expected_next        = 4,
              .expected_rect        = {8, 8, 60, 40},
            },
            {
              .parent               = 1,
              .input                = {.kind = (uint8_t)k_ra8_box_leaf, .flex = 1U, .tag = 404},
              .expected_index       = 4,
              .expected_first_child = (int32_t)k_ra8_box_none,
              .expected_next        = (int32_t)k_ra8_box_none,
              .expected_rect        = {72, 8, 240, 40},
            },
            {
              .parent               = 2,
              .input                = {.kind = (uint8_t)k_ra8_box_leaf, .tag = 405},
              .expected_index       = 5,
              .expected_first_child = (int32_t)k_ra8_box_none,
              .expected_next        = 6,
              .expected_rect        = {8, 56, 94, 176},
            },
            {
              .parent               = 2,
              .input                = {.kind = (uint8_t)k_ra8_box_leaf, .tag = 406},
              .expected_index       = 6,
              .expected_first_child = (int32_t)k_ra8_box_none,
              .expected_next        = 7,
              .expected_rect        = {112, 56, 94, 176},
            },
            {
              .parent               = 2,
              .input                = {.kind = (uint8_t)k_ra8_box_leaf, .tag = 407},
              .expected_index       = 7,
              .expected_first_child = (int32_t)k_ra8_box_none,
              .expected_next        = (int32_t)k_ra8_box_none,
              .expected_rect        = {216, 56, 94, 176},
            },
          },
      },
      {
        .id         = "clamp_underflow_zero_floor",
        .frame      = {5, 5, 20, 12},
        .root       = 0,
        .node_count = 3U,
        .nodes =
          {
            {
              .parent         = (int16_t)k_ra8_box_none,
              .input          = {.kind = (uint8_t)k_ra8_box_grid, .pad = 15, .gap = 20, .tag = 500},
              .expected_index = 0,
              .expected_first_child = 1,
              .expected_next        = (int32_t)k_ra8_box_none,
              .expected_rect        = {5, 5, 20, 12},
            },
            {
              .parent               = 0,
              .input                = {.kind = (uint8_t)k_ra8_box_leaf, .tag = 501},
              .expected_index       = 1,
              .expected_first_child = (int32_t)k_ra8_box_none,
              .expected_next        = 2,
              .expected_rect        = {20, 20, 0, 0},
            },
            {
              .parent               = 0,
              .input                = {.kind = (uint8_t)k_ra8_box_leaf, .fixed = 10, .tag = 502},
              .expected_index       = 2,
              .expected_first_child = (int32_t)k_ra8_box_none,
              .expected_next        = (int32_t)k_ra8_box_none,
              .expected_rect        = {20, 40, 0, 10},
            },
          },
      },
};
