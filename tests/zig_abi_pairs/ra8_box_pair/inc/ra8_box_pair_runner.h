/**
 * @file tests/zig_abi_pairs/ra8_box_pair/inc/ra8_box_pair_runner.h
 * @brief Shared freestanding runner for the `ra8_box` paired ABI behavior vectors.
 *
 * @details
 * Evaluates every error vector and layout tree vector defined in
 * `ra8_box_pair_vectors.h` against the `ra8_box` C ABI (`ra8_box_tree_init`,
 * `ra8_box_add`, `ra8_box_layout`) using fixed stack storage and zero heap
 * allocation or host libc dependencies.
 *
 * @author Brighton Sikarskie
 * @date 2026-11-24
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

/** @brief Aggregate execution result across all shared behavior vectors. */
typedef struct {
  uint16_t    total;     /**< Total vectors evaluated (error + layout).     */
  uint16_t    passed;    /**< Number of vectors whose assertions held.      */
  uint16_t    failed;    /**< Number of failed vectors (zero on pass).      */
  const char* failed_id; /**< Identifier of the first failed vector, or nullptr. */
} ra8_box_pair_result_t;

/**
 * @brief Execute every shared ABI error and layout vector against `ra8_box`.
 *
 * @details
 * Verifies static struct layout invariants (`ra8_ui_rect_t` and `ra8_box_t`
 * byte sizes and alignments), then iterates through every vector in
 * `k_ra8_box_pair_err_vectors` and `k_ra8_box_pair_layout_vectors`.
 *
 * @param[out] out Summary counters and first failing vector identifier.
 *
 * @return `true` if `out` is non-null and every vector passed; `false` otherwise.
 *
 * @pre The `ra8_box` library archive is linked into the caller binary.
 * @pre `out` points to writable caller storage.
 * @post On return with non-null `out`, `out->total == out->passed + out->failed`.
 * @post If `out->failed > 0`, `out->failed_id` names the first failing vector.
 * @note Uses bounded stack arrays only; safe before RTOS or heap initialization.
 * @since 0.1.0
 */
[[nodiscard]] bool ra8_box_pair_run_all(ra8_box_pair_result_t* out);
