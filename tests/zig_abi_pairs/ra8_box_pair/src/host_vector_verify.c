/**
 * @file tests/zig_abi_pairs/ra8_box_pair/src/host_vector_verify.c
 * @brief Host verifier and JSON serializer for the C `ra8_box` pair vectors.
 *
 * @details
 * Runs `ra8_box_pair_run_all()` against the host `libra8_box.a` archive and,
 * when invoked with `--dump-json`, serializes the compiled C vector table so
 * Zig parity tests can verify exact equivalence with `vectors.json`.
 *
 * @author Brighton Sikarskie
 * @date 2026-11-24
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_box.h"
#include "ra8_box_pair_runner.h"
#include "ra8_box_pair_vectors.h"

/** @brief Emit JSON string array representation of `op`. */
RA8_INTERNAL static const char* internal_op_name(ra8_box_pair_op_t op)
{
  switch (op) {
    case k_ra8_box_pair_op_tree_init:
      return "tree_init";
    case k_ra8_box_pair_op_add:
      return "add";
    case k_ra8_box_pair_op_layout:
      return "layout";
    default:
      return "unknown";
  }
}

/** @brief Print the compiled C constants and vector tables as canonical JSON. */
RA8_INTERNAL static void internal_dump_json(void)
{
  printf("{\n");
  printf("  \"schema\": 1,\n");
  printf("  \"library\": \"ra8_box\",\n");
  printf("  \"header\": \"libs/ra8_box/inc/ra8_box.h\",\n");
  printf("  \"constants\": {\n");
  printf("    \"k_ra8_box_none\": %d,\n", (int)k_ra8_box_none);
  printf("    \"k_ra8_box_no_colour\": %d,\n", (int)k_ra8_box_no_colour);
  printf("    \"k_ra8_box_stack_v\": %u,\n", (unsigned)k_ra8_box_stack_v);
  printf("    \"k_ra8_box_stack_h\": %u,\n", (unsigned)k_ra8_box_stack_h);
  printf("    \"k_ra8_box_grid\": %u,\n", (unsigned)k_ra8_box_grid);
  printf("    \"k_ra8_box_leaf\": %u,\n", (unsigned)k_ra8_box_leaf);
  printf("    \"k_ra8_ok\": %u,\n", (unsigned)k_ra8_ok);
  printf("    \"k_ra8_err_invalid_arg\": %u,\n", (unsigned)k_ra8_err_invalid_arg);
  printf("    \"k_ra8_err_null_ptr\": %u,\n", (unsigned)k_ra8_err_null_ptr);
  printf("    \"sizeof_ra8_ui_rect_t\": %zu,\n", sizeof(ra8_ui_rect_t));
  printf("    \"sizeof_ra8_box_t\": %zu,\n", sizeof(ra8_box_t));
  printf("    \"max_nodes_per_case\": %u\n", (unsigned)k_ra8_box_pair_max_nodes);
  printf("  },\n");

  printf("  \"error_vectors\": [\n");
  for (uint16_t i = 0U; i < (uint16_t)k_ra8_box_pair_err_count; ++i) {
    const ra8_box_pair_err_vec_t* v = &k_ra8_box_pair_err_vectors[i];
    printf("    {\"id\":\"%s\",\"op\":\"%s\",\"null_tree\":%s,\"null_arg\":%s,"
           "\"cap\":%u,\"seed_count\":%u,\"index_arg\":%d,"
           "\"expected_err\":%u,\"expected_idx\":%d}%s\n",
           v->id,
           internal_op_name(v->op),
           v->null_tree ? "true" : "false",
           v->null_arg ? "true" : "false",
           (unsigned)v->cap,
           (unsigned)v->seed_count,
           (int)v->index_arg,
           (unsigned)v->expected_err,
           (int)v->expected_idx,
           ((i + 1U) < (uint16_t)k_ra8_box_pair_err_count) ? "," : "");
  }
  printf("  ],\n");

  printf("  \"layout_vectors\": [\n");
  for (uint16_t i = 0U; i < (uint16_t)k_ra8_box_pair_layout_count; ++i) {
    const ra8_box_pair_layout_vec_t* lv = &k_ra8_box_pair_layout_vectors[i];
    printf("    {\"id\":\"%s\",\"frame\":{\"x\":%" PRId32 ",\"y\":%" PRId32 ",\"w\":%" PRId32
           ",\"h\":%" PRId32 "},\"root\":%d,\"nodes\":[\n",
           lv->id,
           lv->frame.x,
           lv->frame.y,
           lv->frame.w,
           lv->frame.h,
           (int)lv->root);
    for (uint16_t j = 0U; j < lv->node_count; ++j) {
      const ra8_box_pair_node_vec_t* nv = &lv->nodes[j];
      printf("      {\"parent\":%d,\"kind\":%u,\"grid_cols\":%u,\"fixed\":%d,"
             "\"flex\":%u,\"pad\":%d,\"gap\":%d,\"fill\":%" PRIu32 ",\"border\":%" PRIu32
             ",\"border_w\":%d,\"tag\":%d,"
             "\"expected_index\":%d,\"expected_first_child\":%" PRId32 ",\"expected_next\":%" PRId32
             ",\"expected_rect\":{\"x\":%" PRId32 ",\"y\":%" PRId32 ",\"w\":%" PRId32
             ",\"h\":%" PRId32 "}}%s\n",
             (int)nv->parent,
             (unsigned)nv->input.kind,
             (unsigned)nv->input.grid_cols,
             (int)nv->input.fixed,
             (unsigned)nv->input.flex,
             (int)nv->input.pad,
             (int)nv->input.gap,
             nv->input.fill,
             nv->input.border,
             (int)nv->input.border_w,
             (int)nv->input.tag,
             (int)nv->expected_index,
             nv->expected_first_child,
             nv->expected_next,
             nv->expected_rect.x,
             nv->expected_rect.y,
             nv->expected_rect.w,
             nv->expected_rect.h,
             ((j + 1U) < lv->node_count) ? "," : "");
    }
    printf("    ]}%s\n", ((i + 1U) < (uint16_t)k_ra8_box_pair_layout_count) ? "," : "");
  }
  printf("  ]\n");
  printf("}\n");
}

/**
 * @brief Program entrypoint for host vector execution and JSON dump parity.
 *
 * @param[in] argc Argument count.
 * @param[in] argv Argument vector (`--dump-json` emits JSON and exits 0).
 *
 * @return `0` on full vector pass, non-zero on failure or invalid usage.
 *
 * @since 0.1.0
 */
int main(int argc, char** argv)
{
  if ((argc == 2) && (strcmp(argv[1], "--dump-json") == 0)) {
    internal_dump_json();
    return 0;
  }
  if (argc != 1) {
    fprintf(stderr, "usage: host_vector_verify [--dump-json]\n");
    return 2;
  }

  ra8_box_pair_result_t result = {};
  if (!ra8_box_pair_run_all(&result)) {
    fprintf(stderr,
            "ra8_box_pair: FAIL %u/%u (first failed: %s)\n",
            (unsigned)result.failed,
            (unsigned)result.total,
            (result.failed_id != nullptr) ? result.failed_id : "unknown");
    return 1;
  }

  printf("ra8_box_pair: PASS %u/%u\n", (unsigned)result.passed, (unsigned)result.total);
  return 0;
}
