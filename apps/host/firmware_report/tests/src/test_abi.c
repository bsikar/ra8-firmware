// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

/**
 * @file test_abi.c
 * @brief Linked C-to-Rust firmware_report ABI lifecycle tests.
 * @details Proves values, failures, unchanged outputs, capacity, and teardown across the ABI.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdlib.h>
#include <string.h>

#include "firmware_report.h"

/**
 * @brief Terminate the test immediately when a required condition is false.
 * @details Provides an always-active verdict mechanism under every CMake build type.
 * @param[in] condition Required test condition.
 * @pre The caller supplies a fully evaluated boolean expression.
 * @pre The hosted runtime provides `abort()`.
 * @post A true condition returns without side effects.
 * @post A false condition terminates with an unsuccessful process result.
 * @note Single-thread compatible; failure terminates the process.
 * @since 0.1.0
 */
static void require(bool condition)
{
  if (!condition) {
    abort();
  }
}

/**
 * @brief Prove successful values and ordinary invalid-query behavior.
 * @details Exercises one complete create, query, and release lifecycle.
 * @pre The Rust provider is linked and has no live token.
 * @pre C and Rust layout assertions compiled successfully.
 * @post The provider token is released.
 * @post Invalid queries left the full output unchanged.
 * @note Single-threaded test helper.
 * @since 0.1.0
 */
static void test_lifecycle(void)
{
  const uint8_t             input[] = {0U, 0xffU, 7U};
  firmware_report_handle_t* handle  = nullptr;
  firmware_report_summary_t output  = {
    .byte_count   = UINT64_MAX,
    .zero_count   = UINT64_MAX,
    .erased_count = UINT64_MAX,
    .fnv1a64      = UINT64_MAX,
  };
  require(firmware_report_create(input, sizeof(input), &handle) == k_firmware_report_ok);
  require(handle != nullptr);
  require(firmware_report_query(handle, &output) == k_firmware_report_ok);
  require(output.byte_count == 3U);
  require(output.zero_count == 1U);
  require(output.erased_count == 1U);
  const firmware_report_summary_t saved   = output;
  const firmware_report_handle_t* foreign = (const firmware_report_handle_t*)&saved;
  require(firmware_report_query(nullptr, &output) == k_firmware_report_invalid_argument);
  require(memcmp(&output, &saved, sizeof(output)) == 0);
  require(firmware_report_query(foreign, &output) == k_firmware_report_invalid_argument);
  require(memcmp(&output, &saved, sizeof(output)) == 0);
  require(firmware_report_release(&handle) == k_firmware_report_ok);
  require(handle == nullptr);
  require(firmware_report_release(&handle) == k_firmware_report_invalid_argument);
}

/**
 * @brief Prove an old token cannot access a later generation.
 * @details Retains an alias across release and recreate, then attacks query and release.
 * @pre The provider has no live token.
 * @pre The input array remains readable throughout the helper.
 * @post The current generation is released normally.
 * @post The stale token and query output remain unchanged on failure.
 * @note Single-threaded test helper.
 * @since 0.1.0
 */
static void test_stale_generation(void)
{
  const uint8_t             input[] = {0U, 0xffU, 7U};
  firmware_report_handle_t* handle  = nullptr;
  require(firmware_report_create(input, sizeof(input), &handle) == k_firmware_report_ok);
  firmware_report_handle_t* stale = handle;
  require(firmware_report_release(&handle) == k_firmware_report_ok);
  require(firmware_report_create(input, sizeof(input), &handle) == k_firmware_report_ok);
  const firmware_report_summary_t saved = {
    .byte_count   = UINT64_MAX,
    .zero_count   = UINT64_MAX,
    .erased_count = UINT64_MAX,
    .fnv1a64      = UINT64_MAX,
  };
  firmware_report_summary_t output = saved;
  require(firmware_report_query(stale, &output) == k_firmware_report_invalid_argument);
  require(memcmp(&output, &saved, sizeof(output)) == 0);
  require(firmware_report_release(&stale) == k_firmware_report_invalid_argument);
  require(stale != nullptr);
  require(firmware_report_release(&handle) == k_firmware_report_ok);
}

/**
 * @brief Prove create rejects an invalid pointer/length pair without output.
 * @details Calls create with null data and a nonzero size.
 * @pre The provider has no live token.
 * @pre The output slot is writable and initially null.
 * @post The output slot remains null.
 * @post The provider retains no live token.
 * @note Single-threaded test helper.
 * @since 0.1.0
 */
static void test_invalid_create(void)
{
  firmware_report_handle_t* handle = nullptr;
  require(firmware_report_create(nullptr, 1U, &handle) == k_firmware_report_invalid_argument);
  require(handle == nullptr);
}

/**
 * @brief Run the linked C-to-Rust ABI test helpers.
 * @details Keeps each independent contract scenario below complexity limits.
 * @return Test process status.
 * @retval 0 Every always-active requirement passed.
 * @pre The Rust static provider archive is linked.
 * @pre The hosted runtime provides the always-active `require()` verdict helper.
 * @post The provider has no live borrowed token.
 * @post Every helper completed.
 * @note Single-threaded test.
 * @since 0.1.0
 */
int main(void)
{
  test_lifecycle();
  test_stale_generation();
  test_invalid_create();
  return 0;
}
