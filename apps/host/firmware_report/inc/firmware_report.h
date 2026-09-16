// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

/**
 * @file firmware_report.h
 * @brief Stable C23 interface to deterministic Rust firmware-image analysis.
 * @details This hand-authored header is the sole cross-language contract. The provider keeps one
 * fixed-capacity report slot, issues non-repeating opaque tokens, performs no heap allocation, and
 * never retains image bytes.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

/**
 * @enum firmware_report_status_t
 * @brief Result of a firmware-report ABI operation.
 * @details Values have an exact signed 32-bit representation across C and Rust.
 * @invariant Every value is one of the declared constants.
 * @code
 * firmware_report_status_t status = k_firmware_report_ok;
 * @endcode
 * @see firmware_report_create
 */
typedef enum : int32_t {
  k_firmware_report_ok                   = 0, /**< Operation completed successfully.        */
  k_firmware_report_invalid_argument     = 1, /**< A pointer, slot, or handle is invalid.   */
  k_firmware_report_invalid_size         = 2, /**< Input exceeds the 16 MiB boundary.       */
  k_firmware_report_capacity_unavailable = 3, /**< The provider slot is already borrowed.   */
  k_firmware_report_internal             = 4, /**< Provider synchronization is unavailable. */
} firmware_report_status_t;

/**
 * @struct firmware_report_handle
 * @brief Opaque generation token borrowed by a successful caller.
 * @details The provider owns its storage; tokens are never dereferenced or reused. Callers may only
 * query and release the live generation.
 * @invariant At most one handle is live at a time.
 * @code
 * firmware_report_handle_t *handle = nullptr;
 * @endcode
 * @see firmware_report_create
 */
typedef struct firmware_report_handle firmware_report_handle_t;

/**
 * @struct firmware_report_summary_t
 * @brief Fixed-layout deterministic firmware-image summary.
 * @details Every field covers the complete input image and is valid after a successful query.
 * @invariant Counts do not exceed `byte_count`.
 * @code
 * firmware_report_summary_t summary = {};
 * @endcode
 * @see firmware_report_query
 */
typedef struct {
  uint64_t byte_count;   /**< Complete input length in bytes.                               */
  uint64_t zero_count;   /**< Count of bytes equal to zero.                                 */
  uint64_t erased_count; /**< Count of bytes equal to erased-flash value `0xff`.            */
  uint64_t fnv1a64;      /**< FNV-1a 64-bit digest; not a cryptographic authenticity check. */
} firmware_report_summary_t;

static_assert(sizeof(firmware_report_status_t) == 4U, "ABI status width");
static_assert(sizeof(firmware_report_summary_t) == 32U, "ABI summary size");
static_assert(alignof(firmware_report_summary_t) == 8U, "ABI summary alignment");
static_assert(offsetof(firmware_report_summary_t, byte_count) == 0U, "ABI byte_count offset");
static_assert(offsetof(firmware_report_summary_t, zero_count) == 8U, "ABI zero_count offset");
static_assert(offsetof(firmware_report_summary_t, erased_count) == 16U, "ABI erased_count offset");
static_assert(offsetof(firmware_report_summary_t, fnv1a64) == 24U, // MAGIC-OK: canonical ABI offset
              "ABI digest offset");

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Analyze an image and borrow the provider's sole report handle.
 * @details The provider synchronously reads `size` bytes and retains no input pointer. It uses a
 * fixed internal slot and performs no heap allocation. Failure leaves `*out_handle` unchanged.
 * @param[in] data Readable image bytes, or null only when `size` is zero.
 * @param[in] size Image length in bytes in the inclusive range 0 through 16 MiB.
 * @param[in,out] out_handle Writable null handle slot that receives the borrow on success.
 * @return Status describing the complete operation.
 * @retval k_firmware_report_ok Handle borrowed successfully.
 * @retval k_firmware_report_invalid_argument Pointer/length pair or output slot is invalid.
 * @retval k_firmware_report_invalid_size Input exceeds 16 MiB.
 * @retval k_firmware_report_capacity_unavailable Another handle is already borrowed.
 * @retval k_firmware_report_internal Provider synchronization failed.
 * @pre `out_handle` points to writable storage.
 * @pre `*out_handle` is null and `data` names `size` readable bytes when `size` is nonzero.
 * @post Success sets `*out_handle` non-null without retaining `data`.
 * @post Failure leaves `*out_handle` unchanged and owns no caller resource.
 * @note Thread-safe; calls serialize on the provider registry.
 * @since 0.1.0
 */
firmware_report_status_t
firmware_report_create(const uint8_t* data, size_t size, firmware_report_handle_t** out_handle);

/**
 * @brief Copy the summary from a live borrowed handle.
 * @details Registry membership is validated before handle access. Every failure leaves the
 * complete output object byte-for-byte unchanged.
 * @param[in] handle Non-null live handle returned by `firmware_report_create`.
 * @param[in,out] out_summary Writable destination updated only on success.
 * @return Status describing the query.
 * @retval k_firmware_report_ok Summary copied successfully.
 * @retval k_firmware_report_invalid_argument Handle or output pointer is invalid.
 * @retval k_firmware_report_internal Provider synchronization failed.
 * @pre `handle` remains borrowed and has not been released.
 * @pre `out_summary` points to writable, correctly aligned storage.
 * @post Success initializes every summary field.
 * @post Failure leaves every destination byte unchanged.
 * @note Thread-safe; calls serialize on the provider registry.
 * @since 0.1.0
 */
firmware_report_status_t firmware_report_query(const firmware_report_handle_t* handle,
                                               firmware_report_summary_t*      out_summary);

/**
 * @brief End a report borrow and clear the caller's handle slot.
 * @details Only the unique live registry handle is accepted. No caller memory is freed because the
 * provider uses fixed internal storage. Every failure leaves the slot unchanged.
 * @param[in,out] in_out_handle Writable slot containing the live borrowed handle.
 * @return Status describing teardown.
 * @retval k_firmware_report_ok Borrow ended and caller slot cleared.
 * @retval k_firmware_report_invalid_argument Slot is null, empty, stale, or foreign.
 * @retval k_firmware_report_internal Provider synchronization failed.
 * @pre `in_out_handle` points to writable storage.
 * @pre `*in_out_handle` is the unique live handle returned by create.
 * @post Success clears `*in_out_handle` and makes capacity available.
 * @post Failure leaves `*in_out_handle` unchanged.
 * @note Thread-safe; calls serialize on the provider registry.
 * @since 0.1.0
 */
firmware_report_status_t firmware_report_release(firmware_report_handle_t** in_out_handle);

#ifdef __cplusplus
}
#endif
