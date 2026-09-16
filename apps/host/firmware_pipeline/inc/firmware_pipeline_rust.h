// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

/**
 * @file firmware_pipeline_rust.h
 * @brief Private C23 contract from the Zig stage to Rust analysis.
 * @details This hand-authored header is the only Rust-to-Zig ABI definition.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

/**
 * @struct firmware_pipeline_rust_summary_t
 * @brief Fixed-layout Rust analysis result.
 * @details All fields describe the complete non-empty input.
 * @invariant Counts do not exceed `byte_count`.
 * @code
 * firmware_pipeline_rust_summary_t summary = {};
 * @endcode
 * @see firmware_pipeline_rust_analyze
 */
typedef struct {
  uint64_t byte_count;   /**< Complete input length in bytes.        */
  uint64_t zero_count;   /**< Number of zero bytes.                  */
  uint64_t erased_count; /**< Number of `0xff` bytes.                */
  uint64_t fnv1a64;      /**< FNV-1a 64-bit change-detection digest. */
} firmware_pipeline_rust_summary_t;

/** @enum firmware_pipeline_rust_layout_t @brief Compile-time Rust ABI layout constants. */
typedef enum : size_t {
  k_firmware_pipeline_rust_digest_offset = 24U, /**< Required FNV-1a field offset. */
} firmware_pipeline_rust_layout_t;

static_assert(sizeof(firmware_pipeline_rust_summary_t) == 32U, "Rust summary ABI size");
static_assert(alignof(firmware_pipeline_rust_summary_t) == 8U, "Rust summary ABI alignment");
static_assert(offsetof(firmware_pipeline_rust_summary_t, fnv1a64) ==
                k_firmware_pipeline_rust_digest_offset,
              "Rust summary digest offset");

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Analyze one non-empty firmware image in Rust.
 * @details Reads the input synchronously, retains no pointer, allocates nothing, and publishes the
 * complete output only on success.
 * @param[in] data Non-null pointer to `size` readable bytes.
 * @param[in] size Input length from zero through 16 MiB.
 * @param[in,out] out_summary Writable destination changed only on success.
 * @return Stable pipeline status represented as signed 32 bits.
 * @retval 0 Analysis succeeded.
 * @retval 1 A required pointer was null.
 * @retval 2 Input exceeded 16 MiB.
 * @retval 3 Input was empty.
 * @pre `data` is non-null and names `size` readable bytes.
 * @pre `out_summary` points to writable aligned storage.
 * @post Success initializes every output field.
 * @post Failure leaves every output byte unchanged.
 * @note Thread-safe and allocation-free.
 * @since 0.1.0
 */
int32_t firmware_pipeline_rust_analyze(const uint8_t*                    data,
                                       size_t                            size,
                                       firmware_pipeline_rust_summary_t* out_summary);

#ifdef __cplusplus
}
#endif
