// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

/**
 * @file test_cli.c
 * @brief Always-active native C tests for command-line policy.
 * @details Proves successful borrowing and unchanged output for rejected arguments.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdlib.h>

#include "firmware_pipeline_cli_internal.h"

/**
 * @brief Abort when an always-active test requirement is false.
 * @param[in] condition Requirement result.
 * @pre None.
 * @post Returns only when the condition is true.
 * @note Calls abort on failure in every build mode.
 * @since 0.1.0
 */
static void require(bool condition)
{
  if (!condition) {
    abort();
  }
}

/**
 * @brief Exercise valid and invalid command-line vectors.
 * @return Zero after every requirement passes.
 * @retval 0 All argument-policy transitions passed.
 * @pre Hosted abort semantics are available.
 * @post No argument storage is retained or changed.
 * @note Requirements remain active under `NDEBUG`.
 * @since 0.1.0
 */
int main(void)
{
  const char* path      = "sentinel";
  char        app[]     = "firmware_pipeline";
  char        image[]   = "image.bin";
  char        empty[]   = "";
  char*       valid[]   = {app, image, nullptr};
  char*       missing[] = {app, nullptr};
  char*       blank[]   = {app, empty, nullptr};
  require(priv_firmware_pipeline_parse_args(2, valid, &path) == k_firmware_pipeline_cli_ok);
  require(path == image);
  path = "sentinel";
  require(priv_firmware_pipeline_parse_args(1, missing, &path) == k_firmware_pipeline_cli_usage);
  require(priv_firmware_pipeline_parse_args(2, missing, &path) == k_firmware_pipeline_cli_usage);
  require(priv_firmware_pipeline_parse_args(2, blank, &path) == k_firmware_pipeline_cli_usage);
  require(priv_firmware_pipeline_parse_args(2, nullptr, &path) == k_firmware_pipeline_cli_usage);
  require(priv_firmware_pipeline_parse_args(2, valid, nullptr) == k_firmware_pipeline_cli_usage);
  require(path[0] == 's');
  return 0;
}
