/**
 * @file test_c_consumer.c
 * @brief C23 compile, layout, runtime, error, and ownership proof for Rust.
 * @details Exercises the authoritative header against the linked Rust provider archive.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_rust_abi_fixture.h"

/**
 * @brief Exercise Rust scalar-value and validation behavior.
 * @par MC/DC:
 * Success and each validation failure independently vary the result and
 * sentinel comparisons in every compound decision.
 */
static int test_apply(void)
{
  ra8_rust_abi_fixture_config_t config = {.value     = 7U,
                                          .factor    = 3U,
                                          .enabled   = 1U,
                                          .reserved0 = 0U};
  uint32_t                      result = 0xA5A5A5A5U;
  if (ra8_rust_abi_fixture_apply(&config, &result) != k_ra8_ok || result != 21U) {
    return 1;
  }
  config.enabled = 0U;
  if (ra8_rust_abi_fixture_apply(&config, &result) != k_ra8_ok || result != 7U) {
    return 2;
  }
  config.enabled = 1U;
  result         = 0xA5A5A5A5U;
  if (ra8_rust_abi_fixture_apply(nullptr, &result) != k_ra8_err_null_ptr || result != 0xA5A5A5A5U) {
    return 3;
  }
  config.enabled = 2U;
  if (ra8_rust_abi_fixture_apply(&config, &result) != k_ra8_err_invalid_arg ||
      result != 0xA5A5A5A5U) {
    return 4;
  }
  config.enabled   = 1U;
  config.reserved0 = 1U;
  if (ra8_rust_abi_fixture_apply(&config, &result) != k_ra8_err_invalid_arg ||
      result != 0xA5A5A5A5U) {
    return 5;
  }
  config.reserved0 = 0U;
  config.value     = UINT32_MAX;
  config.factor    = 2U;
  if (ra8_rust_abi_fixture_apply(&config, &result) != k_ra8_err_invalid_size ||
      result != 0xA5A5A5A5U || ra8_rust_abi_fixture_apply(&config, nullptr) != k_ra8_err_null_ptr) {
    return 6;
  }
  return 0;
}

/**
 * @brief Exercise Rust allocation ownership and failure behavior.
 * @par MC/DC:
 * Forced failure, success, exhaustion, invalid destruction, and repeated
 * destruction independently vary each result and handle-state condition.
 */
static int test_ownership(void)
{
  ra8_rust_abi_fixture_t* handle = (ra8_rust_abi_fixture_t*)(uintptr_t)0x1U;
  if (ra8_rust_abi_fixture_create(nullptr) != k_ra8_err_null_ptr) {
    return 7;
  }
  ra8_rust_abi_fixture_test_fail_next_allocation();
  if (ra8_rust_abi_fixture_create(&handle) != k_ra8_err_no_mem ||
      handle != (ra8_rust_abi_fixture_t*)(uintptr_t)0x1U) {
    return 8;
  }
  if (ra8_rust_abi_fixture_create(&handle) != k_ra8_ok || handle == nullptr) {
    return 9;
  }
  ra8_rust_abi_fixture_t* second = (ra8_rust_abi_fixture_t*)(uintptr_t)0x2U;
  if (ra8_rust_abi_fixture_create(&second) != k_ra8_err_no_mem ||
      second != (ra8_rust_abi_fixture_t*)(uintptr_t)0x2U) {
    return 10;
  }
  if (ra8_rust_abi_fixture_destroy(&second) != k_ra8_err_invalid_arg ||
      second != (ra8_rust_abi_fixture_t*)(uintptr_t)0x2U ||
      ra8_rust_abi_fixture_destroy(nullptr) != k_ra8_err_null_ptr) {
    return 11;
  }
  if (ra8_rust_abi_fixture_destroy(&handle) != k_ra8_ok || handle != nullptr ||
      ra8_rust_abi_fixture_destroy(&handle) != k_ra8_ok) {
    return 12;
  }
  return 0;
}

/** @brief Return zero only when every Rust-provider ABI vector passes. */
int main(void)
{
  const int apply_status = test_apply();
  return apply_status == 0 ? test_ownership() : apply_status;
}
