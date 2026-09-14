/**
 * @file ra8_abi_fixture.h
 * @brief Public C23 contract used to prove a Zig-built library boundary.
 *
 * @details This host-only fixture is the reusable ABI-harness reference. It
 * exposes fixed-width values only; the implementation remains private Zig.
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

static_assert(sizeof(ra8_err_t) == 2U, "ABI fixture error width");
static_assert(k_ra8_ok == 0U, "ABI fixture success value");
static_assert(k_ra8_err_invalid_arg == 0x103U, "ABI fixture invalid arg value");
static_assert(k_ra8_err_invalid_size == 0x105U, "ABI fixture invalid size value");
static_assert(k_ra8_err_null_ptr == 0x504U, "ABI fixture null pointer value");

/**
 * @struct ra8_abi_fixture_config_t
 * @brief Fixed-layout input accepted by `ra8_abi_fixture_apply()`.
 *
 * @details The explicit reserved byte makes the host and target layout
 * observable. `enabled` is a `uint8_t` boolean and accepts only zero or one.
 *
 * @invariant `enabled` is zero or one.
 * @invariant `reserved0` is zero.
 */
typedef struct {
  uint32_t value;     /**< Value to scale. */
  uint16_t factor;    /**< Unsigned scale factor. */
  uint8_t  enabled;   /**< Canonical boolean: zero disables scaling, one enables it. */
  uint8_t  reserved0; /**< Reserved byte; caller supplies zero. */
} ra8_abi_fixture_config_t;

static_assert(sizeof(ra8_abi_fixture_config_t) == 8U, "ABI fixture structure size");
static_assert(alignof(ra8_abi_fixture_config_t) == 4U, "ABI fixture structure alignment");
static_assert(offsetof(ra8_abi_fixture_config_t, value) == 0U, "ABI fixture value offset");
static_assert(offsetof(ra8_abi_fixture_config_t, factor) == 4U, "ABI fixture factor offset");
static_assert(offsetof(ra8_abi_fixture_config_t, enabled) == 6U, "ABI fixture enabled offset");
static_assert(offsetof(ra8_abi_fixture_config_t, reserved0) == 7U, "ABI fixture reserved offset");

/**
 * @brief Validate and scale one fixed-layout ABI value.
 *
 * @details The output pointer is validated before the input pointer. On every
 * failure the function leaves `out_result` unchanged; recoverable native Zig
 * errors map to the existing `ra8_err_t` vocabulary.
 *
 * @param[in] config Input configuration; must not be NULL.
 * @param[out] out_result Scaled result written only on success; must not be NULL.
 *
 * @return `ra8_err_t` result.
 * @retval k_ra8_ok The result was written.
 * @retval k_ra8_err_null_ptr `out_result` or `config` was NULL.
 * @retval k_ra8_err_invalid_arg `enabled` or `reserved0` was invalid.
 * @retval k_ra8_err_invalid_size Scaling overflowed `uint32_t`.
 *
 * @pre `config` addresses a live `ra8_abi_fixture_config_t`.
 * @pre `out_result` addresses writable `uint32_t` storage.
 * @post Success writes exactly one `uint32_t` to `out_result`.
 * @post Failure leaves `out_result` unchanged.
 * @note Task-only and non-reentrant; this host fixture has no ISR path.
 * @since Version 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_abi_fixture_apply(const ra8_abi_fixture_config_t* config,
                                              uint32_t*                       out_result);

#ifdef __cplusplus
}
#endif
