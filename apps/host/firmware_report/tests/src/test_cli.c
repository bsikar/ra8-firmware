// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

/**
 * @file test_cli.c
 * @brief Native C tests for firmware_report argument policy.
 * @details Covers success and every invalid pointer, count, and empty-path class.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdlib.h>

#include "firmware_report_cli_internal.h"

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
 * @brief Prove a valid path is borrowed without copying.
 * @details Supplies exactly the application name and one non-empty image path.
 * @pre Test strings and the output slot remain writable/readable.
 * @pre The argument vector contains a null terminator after its declared entries.
 * @post The output points to the original image string.
 * @post No memory is allocated or retained.
 * @note Single-threaded test helper.
 * @since 0.1.0
 */
static void test_valid_path(void)
{
  const char* path    = "sentinel";
  char        app[]   = "firmware_report";
  char        image[] = "image.bin";
  char*       valid[] = {app, image, nullptr};
  require(priv_firmware_report_parse_args(2, valid, &path) == k_firmware_report_cli_ok);
  require(path == image);
}

/**
 * @brief Prove every invalid argument class returns usage without output.
 * @details Covers count, empty path, null vector, and null destination independently.
 * @pre Test strings and vectors remain valid for every call.
 * @pre The writable output starts with the sentinel pointer.
 * @post Every writable-output failure leaves the sentinel unchanged.
 * @post No memory is allocated or retained.
 * @note Single-threaded test helper.
 * @since 0.1.0
 */
static void test_invalid_arguments(void)
{
  const char* path      = "sentinel";
  char        app[]     = "firmware_report";
  char        image[]   = "image.bin";
  char        empty[]   = "";
  char*       valid[]   = {app, image, nullptr};
  char*       missing[] = {app, nullptr};
  char*       blank[]   = {app, empty, nullptr};
  require(priv_firmware_report_parse_args(1, missing, &path) == k_firmware_report_cli_usage);
  require(path[0] == 's');
  require(priv_firmware_report_parse_args(2, blank, &path) == k_firmware_report_cli_usage);
  require(priv_firmware_report_parse_args(3, valid, &path) == k_firmware_report_cli_usage);
  require(priv_firmware_report_parse_args(2, nullptr, &path) == k_firmware_report_cli_usage);
  require(priv_firmware_report_parse_args(2, valid, nullptr) == k_firmware_report_cli_usage);
  require(path[0] == 's');
}

/**
 * @brief Run the deterministic command-line parser tests.
 * @details Keeps successful and rejected contracts in separate low-complexity helpers.
 * @return Test process status.
 * @retval 0 Every always-active requirement passed.
 * @pre The parser implementation is linked.
 * @pre The hosted runtime provides the always-active `require()` verdict helper.
 * @post No storage is allocated or retained.
 * @post Every parser contract helper completed.
 * @note Single-threaded test.
 * @since 0.1.0
 */
int main(void)
{
  test_valid_path();
  test_invalid_arguments();
  return 0;
}
