/**
 * @file test_c_consumer.c
 * @brief C acceptance test for the C-to-Zig-to-Rust ABI chain.
 * @details Proves values and failures through the complete linked language chain.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_abi_chain.h"

typedef enum : int {
  k_test_ok   = 0, /**< Test completed successfully.      */
  k_test_fail = 1, /**< Test detected a contract failure. */
} chain_test_result_t;
typedef enum : uint32_t {
  k_input       = 7U,          /**< Valid scalar input.                  */
  k_traced      = 0x115U,      /**< Expected Rust result plus Zig trace. */
  k_sentinel    = 0xA5A5A5A5U, /**< Unchanged-output sentinel.           */
  k_invalid_tag = 0U,          /**< Deliberately invalid Zig tag.        */
  k_no_calls    = 0U,          /**< Expected absence of provider calls.  */
  k_one_call    = 1U,          /**< Expected single provider call.       */
  k_no_handles  = 0U,          /**< Expected absence of live handles.    */
  k_one_handle  = 1U,          /**< Expected single live handle.         */
} chain_u32_t;
typedef enum : uint16_t {
  k_factor          = 3U, /**< Valid multiplication factor.              */
  k_overflow_factor = 2U, /**< Factor that overflows the selected input. */
} chain_u16_t;
typedef enum : uint8_t {
  k_disabled        = 0U, /**< Disabled fixture state.          */
  k_enabled         = 1U, /**< Enabled fixture state.           */
  k_invalid_enabled = 2U, /**< Invalid Boolean representation.  */
  k_reserved_clear  = 0U, /**< Required clear reserved value.   */
  k_reserved_set    = 1U, /**< Deliberately set reserved value. */
} chain_u8_t;
typedef enum : uintptr_t {
  k_fake_first  = 0x1U, /**< First invalid handle sentinel.  */
  k_fake_second = 0x2U, /**< Second invalid handle sentinel. */
} chain_address_t;

/**
 * @brief Prove Zig validation and exactly-one Rust apply entry.
 * @return Test result.
 * @retval k_test_ok Every call-count, result, and sentinel vector passed.
 * @retval k_test_fail A boundary contract was violated.
 * @par MC/DC:
 * Success, each Zig validation failure, each Rust failure, and Zig overflow
 * independently change their compound result/counter decisions.
 */
static chain_test_result_t test_apply(void)
{
  ra8_abi_chain_config_t config = {
    .rust      = {.value     = k_input,
                  .factor    = k_factor,
                  .enabled   = k_enabled,
                  .reserved0 = k_reserved_clear},
    .zig_tag   = k_ra8_abi_chain_tag,
    .reserved0 = k_reserved_clear,
  };
  uint32_t result = k_sentinel;
  ra8_rust_abi_fixture_test_reset_apply_calls();
  if (ra8_abi_chain_apply(&config, &result) != k_ra8_ok || result != k_traced ||
      ra8_rust_abi_fixture_test_apply_calls() != k_one_call) {
    return k_test_fail;
  }
  result = k_sentinel;
  ra8_rust_abi_fixture_test_reset_apply_calls();
  config.zig_tag = k_invalid_tag;
  if (ra8_abi_chain_apply(&config, &result) != k_ra8_err_invalid_arg || result != k_sentinel ||
      ra8_rust_abi_fixture_test_apply_calls() != k_no_calls) {
    return k_test_fail;
  }
  config.zig_tag   = k_ra8_abi_chain_tag;
  config.reserved0 = k_reserved_set;
  if (ra8_abi_chain_apply(&config, &result) != k_ra8_err_invalid_arg || result != k_sentinel ||
      ra8_rust_abi_fixture_test_apply_calls() != k_no_calls) {
    return k_test_fail;
  }
  config.reserved0    = k_reserved_clear;
  config.rust.enabled = k_invalid_enabled;
  if (ra8_abi_chain_apply(&config, &result) != k_ra8_err_invalid_arg || result != k_sentinel ||
      ra8_rust_abi_fixture_test_apply_calls() != k_one_call) {
    return k_test_fail;
  }
  ra8_rust_abi_fixture_test_reset_apply_calls();
  config.rust.enabled = k_enabled;
  config.rust.value   = UINT32_MAX;
  config.rust.factor  = k_overflow_factor;
  if (ra8_abi_chain_apply(&config, &result) != k_ra8_err_invalid_size || result != k_sentinel ||
      ra8_rust_abi_fixture_test_apply_calls() != k_one_call) {
    return k_test_fail;
  }
  ra8_rust_abi_fixture_test_reset_apply_calls();
  config.rust.enabled = k_disabled;
  if (ra8_abi_chain_apply(&config, &result) != k_ra8_err_invalid_size || result != k_sentinel ||
      ra8_rust_abi_fixture_test_apply_calls() != k_one_call) {
    return k_test_fail;
  }
  return k_test_ok;
}

/**
 * @brief Prove allocation ownership and cleanup across both membranes.
 * @return Test result.
 * @retval k_test_ok All allocation, release, and live-count vectors passed.
 * @retval k_test_fail A handle or instrumentation contract was violated.
 * @par MC/DC:
 * Forced failure, invalid release, success, stale release, and reacquisition
 * independently change each compound lifecycle decision.
 */
static chain_test_result_t test_ownership(void)
{
  ra8_abi_chain_handle_t* handle = (ra8_abi_chain_handle_t*)k_fake_first;
  ra8_rust_abi_fixture_test_fail_next_allocation();
  if (ra8_abi_chain_create(k_invalid_tag, &handle) != k_ra8_err_invalid_arg ||
      ra8_rust_abi_fixture_test_live_handles() != k_no_handles ||
      ra8_abi_chain_create(k_ra8_abi_chain_tag, &handle) != k_ra8_err_no_mem ||
      handle != (ra8_abi_chain_handle_t*)k_fake_first) {
    return k_test_fail;
  }
  if (ra8_abi_chain_create(k_ra8_abi_chain_tag, &handle) != k_ra8_ok || handle == nullptr ||
      ra8_rust_abi_fixture_test_live_handles() != k_one_handle) {
    return k_test_fail;
  }
  ra8_abi_chain_handle_t* stale   = handle;
  ra8_abi_chain_handle_t* invalid = (ra8_abi_chain_handle_t*)k_fake_second;
  if (ra8_abi_chain_destroy(&invalid) != k_ra8_err_invalid_arg ||
      invalid != (ra8_abi_chain_handle_t*)k_fake_second ||
      ra8_rust_abi_fixture_test_live_handles() != k_one_handle) {
    return k_test_fail;
  }
  if (ra8_abi_chain_destroy(&handle) != k_ra8_ok || handle != nullptr ||
      ra8_rust_abi_fixture_test_live_handles() != k_no_handles ||
      ra8_abi_chain_destroy(&stale) != k_ra8_err_invalid_arg || stale == nullptr ||
      ra8_rust_abi_fixture_test_live_handles() != k_no_handles) {
    return k_test_fail;
  }
  if (ra8_abi_chain_create(k_ra8_abi_chain_tag, &handle) != k_ra8_ok || handle == nullptr ||
      ra8_abi_chain_destroy(&handle) != k_ra8_ok || handle != nullptr ||
      ra8_rust_abi_fixture_test_live_handles() != k_no_handles) {
    return k_test_fail;
  }
  return k_test_ok;
}

/** @brief Return zero only when both ABI membranes preserve their contracts. */
int main(void)
{
  if (test_apply() != k_test_ok) {
    return k_test_fail;
  }
  return test_ownership();
}
