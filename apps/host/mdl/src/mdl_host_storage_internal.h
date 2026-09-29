/**
 * @file mdl_host_storage_internal.h
 * @brief Host composition-root filesystem spans, carved from one block.
 *
 * @details The CLI needs four caller-owned filesystem spans: one open-file
 * backend state, one staged-publish backend state, one directory cursor and
 * one shared streaming buffer. They used to be four independent objects in
 * `main.c`, so the host's filesystem footprint was four separate figures and
 * nothing stated their sum or proved the last one fit. They are now one block
 * carved by ::ra8_arena_carve_all, which is the arena's answer to the
 * hand-written offset chain (#757).
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#ifndef MDL_HOST_STORAGE_INTERNAL_H
#define MDL_HOST_STORAGE_INTERNAL_H

#include <stdint.h>

#include "mdl_app.h"
#include "mdl_storage.h"
#include "ra8_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @enum mdl_host_storage_extent_t
 * @brief Byte requirement of each span carved out of the storage block.
 *
 * @details Each span keeps the extent its own object had before the carve, so
 *          the backend capacity comparisons ::mdl_storage_init makes are
 *          unchanged. The block is their sum, stated once.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_host_file_work_bytes        = (uint32_t)k_storage_work_bytes,   /**< Open-file state. */
  k_host_transaction_work_bytes = (uint32_t)k_storage_work_bytes,   /**< Staged publish.  */
  k_host_directory_work_bytes   = (uint32_t)k_mdl_storage_io_bytes, /**< Cursor state.    */
  k_host_io_buffer_bytes        = (uint32_t)k_mdl_storage_io_bytes, /**< Stream scratch.  */
  k_host_storage_slots          = 4U,                               /**< Declared slots.  */
  k_host_storage_block_bytes =
    k_host_file_work_bytes + k_host_transaction_work_bytes + k_host_directory_work_bytes +
    k_host_io_buffer_bytes, /**< Whole block. */
} mdl_host_storage_extent_t;

/**
 * @struct mdl_host_storage_spans_t
 * @brief The four filesystem spans one successful carve publishes.
 *
 * @details Every member borrows process-lifetime storage owned by the
 *          composition root. No member is freed and none may outlive it.
 *
 * @since 0.1.0
 */
typedef struct {
  void*    file_workspace;        /**< Open-file backend state.  */
  void*    transaction_workspace; /**< Staged-publish state.     */
  void*    directory_workspace;   /**< Directory-cursor state.   */
  uint8_t* io_buffer;             /**< Shared streaming scratch. */
} mdl_host_storage_spans_t;

/**
 * @brief Carve the four filesystem spans out of the one storage block.
 *
 * @details Declares one slot per span and hands the whole set to
 *          ::ra8_arena_carve_all, so "do all four fit?" is a question the
 *          arena answers before any pointer is published. Every span is
 *          `max_align_t`-aligned, which is what each one had as its own
 *          object, so the backend alignment contracts ::mdl_storage_init
 *          checks are unchanged. Calling it twice re-carves the same block and
 *          hands back the same four addresses.
 *
 * @param[out] out Receives the four published spans.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok           Every slot carved; @p out is complete.
 * @retval k_ra8_err_null_ptr @p out was NULL.
 * @retval other              The block could not hold the declared slots.
 *
 * @pre @p out addresses writable storage for one complete object.
 * @post On success the four spans are disjoint and in declaration order.
 * @post On failure @p out is untouched.
 *
 * @note Not thread-safe; the CLI binds storage once on one thread.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t mdl_host_storage_carve(mdl_host_storage_spans_t* out);

#ifdef __cplusplus
}
#endif

#endif /* MDL_HOST_STORAGE_INTERNAL_H */
