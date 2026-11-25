/**
 * @file tests/zig_abi_pairs/ra8_box_pair/src/ra8_box_pair_runner.c
 * @brief Freestanding executor for the shared `ra8_box` ABI behavior vectors.
 *
 * @details
 * Implements `ra8_box_pair_run_all()` cleanly on both EK-RA8D2 firmware and
 * host binaries so one verification loop backs both C firmware runs and host
 * parity checks without duplicating vector evaluation logic.
 *
 * @author Brighton Sikarskie
 * @date 2026-11-24
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "ra8_box_pair_runner.h"

#include <stdalign.h>
#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_box.h"
#include "ra8_box_pair_vectors.h"

static_assert(sizeof(ra8_ui_rect_t) == k_ra8_box_pair_rect_size, "ra8_ui_rect_t ABI size");
static_assert(sizeof(ra8_box_t) == k_ra8_box_pair_node_size, "ra8_box_t ABI size");
static_assert(alignof(ra8_ui_rect_t) >= 4U, "ra8_ui_rect_t ABI alignment");
static_assert(alignof(ra8_box_t) >= 4U, "ra8_box_t ABI alignment");

/** @brief Seed `seed_count` minimal leaf nodes into an initialized tree. */
RA8_INTERNAL static bool internal_seed_tree(ra8_box_tree_t* tree, uint16_t seed_count)
{
  if (tree == nullptr) {
    return false;
  }

  const ra8_box_t leaf = {.kind = (uint8_t)k_ra8_box_leaf};
  for (uint16_t i = 0U; i < seed_count; ++i) {
    if (ra8_box_add(tree, (int16_t)k_ra8_box_none, &leaf) < 0) {
      return false;
    }
  }
  return true;
}

/** @brief Execute one shared error/guard vector from `k_ra8_box_pair_err_vectors`. */
RA8_INTERNAL static bool internal_eval_err_vector(const ra8_box_pair_err_vec_t* vec)
{
  if (vec == nullptr) {
    return false;
  }

  ra8_box_t           storage[k_ra8_box_pair_max_nodes] = {};
  ra8_box_tree_t      tree                              = {};
  const ra8_box_t     sample_leaf                       = {.kind = (uint8_t)k_ra8_box_leaf};
  const ra8_ui_rect_t sample_frame                      = {.x = 0, .y = 0, .w = 10, .h = 10};

  if (vec->op != k_ra8_box_pair_op_tree_init) {
    if (ra8_box_tree_init(&tree, storage, vec->cap) != k_ra8_ok) {
      return false;
    }
    if (!internal_seed_tree(&tree, vec->seed_count)) {
      return false;
    }
  }

  ra8_box_tree_t* const tree_arg = vec->null_tree ? nullptr : &tree;

  if (vec->op == k_ra8_box_pair_op_tree_init) {
    ra8_box_t* const buf_arg = vec->null_arg ? nullptr : storage;
    return ra8_box_tree_init(tree_arg, buf_arg, vec->cap) == vec->expected_err;
  }

  if (vec->op == k_ra8_box_pair_op_add) {
    const ra8_box_t* const node_arg = vec->null_arg ? nullptr : &sample_leaf;
    const uint16_t         before   = tree.count;
    const int16_t          got      = ra8_box_add(tree_arg, vec->index_arg, node_arg);
    if (got != vec->expected_idx) {
      return false;
    }
    if (!vec->null_tree && (tree.count != before)) {
      return false;
    }
    return true;
  }

  if (vec->op == k_ra8_box_pair_op_layout) {
    const ra8_ui_rect_t* const frame_arg = vec->null_arg ? nullptr : &sample_frame;
    return ra8_box_layout(tree_arg, vec->index_arg, frame_arg) == vec->expected_err;
  }

  return false;
}

/** @brief Compare actual laid-out node fields against expected vector fields. */
RA8_INTERNAL static bool internal_check_node(const ra8_box_t*               actual,
                                             const ra8_box_pair_node_vec_t* expected)
{
  if ((actual == nullptr) || (expected == nullptr)) {
    return false;
  }

  return (actual->kind == expected->input.kind) &&
         (actual->grid_cols == expected->input.grid_cols) &&
         (actual->fixed == expected->input.fixed) && (actual->flex == expected->input.flex) &&
         (actual->pad == expected->input.pad) && (actual->gap == expected->input.gap) &&
         (actual->fill == expected->input.fill) && (actual->border == expected->input.border) &&
         (actual->border_w == expected->input.border_w) && (actual->tag == expected->input.tag) &&
         (actual->first_child == expected->expected_first_child) &&
         (actual->next == expected->expected_next) &&
         (actual->rect.x == expected->expected_rect.x) &&
         (actual->rect.y == expected->expected_rect.y) &&
         (actual->rect.w == expected->expected_rect.w) &&
         (actual->rect.h == expected->expected_rect.h);
}

/** @brief Build and evaluate one shared layout vector from `k_ra8_box_pair_layout_vectors`. */
RA8_INTERNAL static bool internal_eval_layout_vector(const ra8_box_pair_layout_vec_t* vec)
{
  if ((vec == nullptr) || (vec->node_count == 0U) || (vec->node_count > k_ra8_box_pair_max_nodes)) {
    return false;
  }

  ra8_box_t      storage[k_ra8_box_pair_max_nodes] = {};
  ra8_box_tree_t tree                              = {};
  if (ra8_box_tree_init(&tree, storage, (uint16_t)k_ra8_box_pair_max_nodes) != k_ra8_ok) {
    return false;
  }

  for (uint16_t i = 0U; i < vec->node_count; ++i) {
    const int16_t idx = ra8_box_add(&tree, vec->nodes[i].parent, &vec->nodes[i].input);
    if (idx != vec->nodes[i].expected_index) {
      return false;
    }
  }

  if (tree.count != vec->node_count) {
    return false;
  }

  if (ra8_box_layout(&tree, vec->root, &vec->frame) != k_ra8_ok) {
    return false;
  }

  for (uint16_t i = 0U; i < vec->node_count; ++i) {
    if (!internal_check_node(&storage[i], &vec->nodes[i])) {
      return false;
    }
  }

  return true;
}

/** @brief Record one vector outcome into `out`, latching the first failure id. */
RA8_INTERNAL static void internal_record(ra8_box_pair_result_t* out, const char* id, bool ok)
{
  if (out == nullptr) {
    return;
  }

  out->total += 1U;
  if (ok) {
    out->passed += 1U;
    return;
  }

  out->failed += 1U;
  if (out->failed_id == nullptr) {
    out->failed_id = id;
  }
}

bool ra8_box_pair_run_all(ra8_box_pair_result_t* out)
{
  if (out == nullptr) {
    return false;
  }

  out->total     = 0U;
  out->passed    = 0U;
  out->failed    = 0U;
  out->failed_id = nullptr;

  for (uint16_t i = 0U; i < (uint16_t)k_ra8_box_pair_err_count; ++i) {
    const bool ok = internal_eval_err_vector(&k_ra8_box_pair_err_vectors[i]);
    internal_record(out, k_ra8_box_pair_err_vectors[i].id, ok);
  }

  for (uint16_t i = 0U; i < (uint16_t)k_ra8_box_pair_layout_count; ++i) {
    const bool ok = internal_eval_layout_vector(&k_ra8_box_pair_layout_vectors[i]);
    internal_record(out, k_ra8_box_pair_layout_vectors[i].id, ok);
  }

  return out->failed == 0U;
}
