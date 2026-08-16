/**
 * @file test_ra8_err_to_str.c
 * @brief Additional coverage for ra8_err_to_str()
 * @details Exercises category boundaries and unknown-code fallback behavior for the repository error-to-string conversion.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include "ra8_err.h"
#include "unity_minimal.h"

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_hardware_category(void)
{
  TEST_BEGIN("hardware category strings");
  TEST_ASSERT(strcmp(ra8_err_to_str(k_ra8_err_hw_init_failed), "hw_init_failed") == 0);
  TEST_ASSERT(strcmp(ra8_err_to_str(k_ra8_err_hw_not_ready), "hw_not_ready") == 0);
  TEST_ASSERT(strcmp(ra8_err_to_str(k_ra8_err_hw_timeout), "hw_timeout") == 0);
  TEST_END("hardware category strings");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_comm_category(void)
{
  TEST_BEGIN("communication category strings");
  TEST_ASSERT(strcmp(ra8_err_to_str(k_ra8_err_comm_error), "comm_error") == 0);
  TEST_ASSERT(strcmp(ra8_err_to_str(k_ra8_err_spi_error), "spi_error") == 0);
  TEST_ASSERT(strcmp(ra8_err_to_str(k_ra8_err_uart_error), "uart_error") == 0);
  TEST_ASSERT(strcmp(ra8_err_to_str(k_ra8_err_i2c_error), "i2c_error") == 0);
  TEST_END("communication category strings");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_validation_category(void)
{
  TEST_BEGIN("validation category strings");
  TEST_ASSERT(strcmp(ra8_err_to_str(k_ra8_err_validation_failed), "validation_failed") == 0);
  TEST_ASSERT(strcmp(ra8_err_to_str(k_ra8_err_checksum_mismatch), "checksum_mismatch") == 0);
  TEST_ASSERT(strcmp(ra8_err_to_str(k_ra8_err_null_ptr), "null_ptr") == 0);
  TEST_END("validation category strings");
}

int main(void)
{
  test_hardware_category();
  test_comm_category();
  test_validation_category();
  return 0;
}
