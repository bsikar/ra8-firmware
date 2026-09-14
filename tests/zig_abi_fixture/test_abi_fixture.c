/**
 * @file test_abi_fixture.c
 * @brief C23 consumer acceptance test for the Zig ABI fixture.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_abi_fixture.h"

/** @enum abi_fixture_test_result_t @brief Process result values for this fixture. */
typedef enum : int {
  k_abi_fixture_test_success = 0,
  k_abi_fixture_test_failure = 1,
} abi_fixture_test_result_t;

/**
 * @brief Exercise the C header, linked Zig export, and failure-output contract.
 *
 * @details The test uses only the fixture's public header and linked library.
 * It verifies success plus null, validation, and overflow mappings.
 *
 * @return Process status for CTest.
 * @retval k_abi_fixture_test_success Every ABI assertion held.
 * @retval k_abi_fixture_test_failure One ABI assertion failed.
 * @pre The Zig fixture library was linked by CMake.
 * @pre The fixture header declarations match the library exports.
 * @post A nonzero result identifies a C ABI contract regression.
 * @post No private Zig declaration is referenced.
 * @note Host-only task context; no ISR behavior is exercised.
 * @since Version 0.1.0
 */
int main(void)
{
  ra8_abi_fixture_config_t config = {
    .value     = 7U,
    .factor    = 3U,
    .enabled   = 1U,
    .reserved0 = 0U,
  };
  uint32_t result = 0xA5A5A5A5U;

  if (ra8_abi_fixture_apply(&config, &result) != k_ra8_ok || result != 21U) {
    return k_abi_fixture_test_failure;
  }

  result = 0xA5A5A5A5U;
  if (ra8_abi_fixture_apply(nullptr, &result) != k_ra8_err_null_ptr || result != 0xA5A5A5A5U) {
    return k_abi_fixture_test_failure;
  }

  config.enabled = 2U;
  if (ra8_abi_fixture_apply(&config, &result) != k_ra8_err_invalid_arg || result != 0xA5A5A5A5U) {
    return k_abi_fixture_test_failure;
  }

  config.enabled   = 1U;
  config.reserved0 = 1U;
  if (ra8_abi_fixture_apply(&config, &result) != k_ra8_err_invalid_arg || result != 0xA5A5A5A5U) {
    return k_abi_fixture_test_failure;
  }

  config.reserved0 = 0U;
  config.value     = UINT32_MAX;
  config.factor    = 2U;
  if (ra8_abi_fixture_apply(&config, &result) != k_ra8_err_invalid_size || result != 0xA5A5A5A5U) {
    return k_abi_fixture_test_failure;
  }

  if (ra8_abi_fixture_apply(&config, nullptr) != k_ra8_err_null_ptr) {
    return k_abi_fixture_test_failure;
  }

  return k_abi_fixture_test_success;
}
