/**
 * @file ra8_rust_abi_fixture.h
 * @brief Authoritative C23 contract for the bounded Rust provider fixture.
 * @details Defines fixed-width values, ownership, and lifecycle rules shared with Rust.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

#include "ra8_err.h"

static_assert(sizeof(ra8_err_t) == 2U, "Rust ABI error width");
static_assert(k_ra8_ok == 0U, "Rust ABI success value");
static_assert(k_ra8_err_no_mem == 0x102U, "Rust ABI no-memory value");
static_assert(k_ra8_err_invalid_arg == 0x103U, "Rust ABI invalid-argument value");
static_assert(k_ra8_err_invalid_size == 0x105U, "Rust ABI invalid-size value");
static_assert(k_ra8_err_null_ptr == 0x504U, "Rust ABI null-pointer value");

/**
 * @struct ra8_rust_abi_fixture_t
 * @brief Opaque Rust-owned state with one process-wide allocation slot.
 * @details C retains only the pointer; Rust owns the representation and storage.
 * @invariant Only this fixture creates or dereferences a live handle.
 * @invariant Successful destruction invalidates the handle.
 * @code
 * ra8_rust_abi_fixture_t* handle = nullptr;
 * @endcode
 * @see ra8_rust_abi_fixture_create
 * @see ra8_rust_abi_fixture_destroy
 */
typedef struct ra8_rust_abi_fixture ra8_rust_abi_fixture_t;

/**
 * @struct ra8_rust_abi_fixture_config_t
 * @brief Fixed-layout input consumed by the Rust provider.
 * @details `enabled` is a canonical byte boolean and `reserved0` is zero.
 * @invariant `enabled` is zero or one.
 * @invariant `reserved0` is zero.
 * @code
 * ra8_rust_abi_fixture_config_t config = {
 *   .value = 7U, .factor = 3U, .enabled = 1U, .reserved0 = 0U};
 * @endcode
 * @see ra8_rust_abi_fixture_apply
 */
typedef struct {
  uint32_t value;     /**< Value to scale.                         */
  uint16_t factor;    /**< Unsigned scale factor.                  */
  uint8_t  enabled;   /**< Zero preserves value; one scales value. */
  uint8_t  reserved0; /**< Reserved; callers supply zero.          */
} ra8_rust_abi_fixture_config_t;

static_assert(sizeof(ra8_rust_abi_fixture_config_t) == 8U, "Rust ABI config size");
static_assert(alignof(ra8_rust_abi_fixture_config_t) == 4U, "Rust ABI config alignment");
static_assert(offsetof(ra8_rust_abi_fixture_config_t, value) == 0U, "Rust ABI value offset");
static_assert(offsetof(ra8_rust_abi_fixture_config_t, factor) == 4U, "Rust ABI factor offset");
static_assert(offsetof(ra8_rust_abi_fixture_config_t, enabled) == 6U, "Rust ABI enabled offset");
static_assert(offsetof(ra8_rust_abi_fixture_config_t, reserved0) == 7U, "Rust ABI reserved offset");

/**
 * @brief Validate and scale one fixed-layout value in Rust.
 * @details The adapter validates outputs first and publishes a value only
 * after safe Rust completes successfully.
 * @param[in] config Live input configuration; must not be NULL.
 * @param[out] out_result Result written only on success; must not be NULL.
 * @return Stable `ra8_err_t` result.
 * @retval k_ra8_ok The result was written.
 * @retval k_ra8_err_null_ptr A required pointer was NULL.
 * @retval k_ra8_err_invalid_arg A byte field was non-canonical.
 * @retval k_ra8_err_invalid_size Scaling overflowed `uint32_t`.
 * @pre `config` addresses readable storage for the duration of the call.
 * @pre `out_result` addresses writable storage for the duration of the call.
 * @post Success writes exactly one `uint32_t` result.
 * @post Failure leaves `out_result` unchanged.
 * @note Task-only; callers serialize access to this fixture.
 * @see ra8_rust_abi_fixture_config_t
 * @since Version 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rust_abi_fixture_apply(const ra8_rust_abi_fixture_config_t* config,
                                                   uint32_t*                            out_result);

/**
 * @brief Acquire the fixture's single Rust-owned state object.
 * @details The fixed-capacity pool publishes no partial handle on failure.
 * @param[out] out_handle Handle published only on success; must not be NULL.
 * @return Stable `ra8_err_t` result.
 * @retval k_ra8_ok A handle was published.
 * @retval k_ra8_err_null_ptr `out_handle` was NULL.
 * @retval k_ra8_err_no_mem The bounded slot is occupied or forced to fail.
 * @pre `out_handle` addresses writable pointer storage.
 * @pre The caller serializes access to the task-only fixture.
 * @post Success transfers one handle to the caller.
 * @post Failure leaves `out_handle` unchanged.
 * @note Task-only and non-reentrant.
 * @see ra8_rust_abi_fixture_t
 * @see ra8_rust_abi_fixture_destroy
 * @since Version 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rust_abi_fixture_create(ra8_rust_abi_fixture_t** out_handle);

/**
 * @brief Force exactly the next handle allocation to fail.
 * @details This test-only control clears itself after one create attempt.
 * @pre The caller serializes this call with create operations.
 * @pre No unrelated allocation occurs before the intended create.
 * @post The next create returns `k_ra8_err_no_mem`.
 * @post Later create operations resume normal bounded-pool behavior.
 * @note Task-only and non-reentrant.
 * @see ra8_rust_abi_fixture_create
 * @since Version 0.1.0
 */
void ra8_rust_abi_fixture_test_fail_next_allocation(void);

/**
 * @brief Reset the Rust apply-entry counter used by chained ABI tests.
 * @pre Calls are serialized with fixture operations.
 * @pre The caller is preparing a bounded acceptance vector.
 * @post The reported apply-entry count is zero.
 * @post Live-handle instrumentation is unchanged.
 * @note Test-only, task-context instrumentation.
 * @see ra8_rust_abi_fixture_test_apply_calls
 * @since Version 0.1.0
 */
void ra8_rust_abi_fixture_test_reset_apply_calls(void);

/**
 * @brief Read the Rust apply-entry counter.
 * @return Number of provider apply entries since the last reset.
 * @pre The caller tolerates a concurrently changing diagnostic value.
 * @pre The counter has not wrapped during the bounded test.
 * @post Provider state and ownership are unchanged.
 * @post The counter remains available for later reads.
 * @note Test-only atomic instrumentation.
 * @see ra8_rust_abi_fixture_test_reset_apply_calls
 * @since Version 0.1.0
 */
[[nodiscard]] uint32_t ra8_rust_abi_fixture_test_apply_calls(void);

/**
 * @brief Read the number of Rust-owned live fixture handles.
 * @return Zero or one live handle.
 * @pre The caller tolerates a concurrently changing diagnostic value.
 * @pre The fixture remains within its single-slot capacity.
 * @post Provider state and ownership are unchanged.
 * @post The returned value reflects the atomic ownership counter.
 * @note Test-only atomic instrumentation.
 * @see ra8_rust_abi_fixture_create
 * @see ra8_rust_abi_fixture_destroy
 * @since Version 0.1.0
 */
[[nodiscard]] uint32_t ra8_rust_abi_fixture_test_live_handles(void);

/**
 * @brief Release a Rust-owned state object and clear the caller's handle.
 * @details Rust validates ownership before reclaiming storage; failed release
 * attempts preserve the caller's exact pointer for diagnosis or retry.
 * @param[in,out] in_out_handle Handle slot; a NULL handle value is idempotent.
 * @return Stable `ra8_err_t` result.
 * @retval k_ra8_ok The handle was absent or released and cleared.
 * @retval k_ra8_err_null_ptr `in_out_handle` was NULL.
 * @retval k_ra8_err_invalid_arg The handle was not created by this fixture.
 * @pre A non-NULL handle came from `ra8_rust_abi_fixture_create` and remains live.
 * @pre `in_out_handle` addresses readable and writable pointer storage.
 * @post Success clears a live handle or preserves an already-NULL value.
 * @post Failure leaves `in_out_handle` unchanged.
 * @note Task-only and non-reentrant.
 * @see ra8_rust_abi_fixture_t
 * @see ra8_rust_abi_fixture_create
 * @since Version 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rust_abi_fixture_destroy(ra8_rust_abi_fixture_t** in_out_handle);

#ifdef __cplusplus
}
#endif
