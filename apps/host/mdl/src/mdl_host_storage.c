/**
 * @file mdl_host_storage.c
 * @brief One caller-owned block carved into the host's filesystem spans.
 *
 * @details Holds the block and the slot declaration that replaces four
 * independent workspace objects in the composition root (#757).
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>

#include "mdl_host_storage_internal.h"
#include "ra8_arena.h"
#include "ra8_attributes.h"

/**
 * @struct host_storage_block_t
 * @brief Maximally aligned storage block the four filesystem spans share.
 *
 * @since 0.1.0
 */
typedef struct {
  alignas(max_align_t) uint8_t bytes[k_host_storage_block_bytes]; /**< Carved bytes. */
} host_storage_block_t;

/** @brief Process-lifetime storage block owned by the composition root. */
static host_storage_block_t s_block;

ra8_err_t mdl_host_storage_carve(mdl_host_storage_spans_t* out)
{
  if (out == nullptr) {
    return k_ra8_err_null_ptr;
  }
  ra8_arena_t     arena = {};
  const ra8_err_t bound = ra8_arena_init(&arena, s_block.bytes, (uint32_t)sizeof s_block.bytes);
  if (bound != k_ra8_ok) {
    return bound;
  }
  mdl_host_storage_spans_t spans   = {};
  void*                    io_span = nullptr;
  const ra8_arena_slot_t   slots[] = {
    {
        .bytes   = (uint32_t)k_host_file_work_bytes,
        .align   = (uint32_t)alignof(max_align_t),
        .out_ptr = &spans.file_workspace,
    },
    {
        .bytes   = (uint32_t)k_host_transaction_work_bytes,
        .align   = (uint32_t)alignof(max_align_t),
        .out_ptr = &spans.transaction_workspace,
    },
    {
        .bytes   = (uint32_t)k_host_directory_work_bytes,
        .align   = (uint32_t)alignof(max_align_t),
        .out_ptr = &spans.directory_workspace,
    },
    {
        .bytes   = (uint32_t)k_host_io_buffer_bytes,
        .align   = (uint32_t)alignof(max_align_t),
        .out_ptr = &io_span,
    },
  };
  const ra8_err_t carved = ra8_arena_carve_all(&arena, slots, (uint32_t)k_host_storage_slots);
  if (carved != k_ra8_ok) {
    return carved;
  }
  spans.io_buffer = (uint8_t*)io_span;
  *out            = spans;
  return k_ra8_ok;
}
