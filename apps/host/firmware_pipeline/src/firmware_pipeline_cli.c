// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

/**
 * @file firmware_pipeline_cli.c
 * @brief Deterministic firmware_pipeline command-line validation.
 * @details Implements the internal C argument policy.
 */

#include "firmware_pipeline_cli.h"

#include <stddef.h>

firmware_pipeline_cli_status_t
firmware_pipeline_parse_args(int argc, char** argv, const char** out_path)
{
  if ((argc != 2) || (argv == nullptr) || (out_path == nullptr) || (argv[1] == nullptr) ||
      (argv[1][0] == '\0')) {
    return k_firmware_pipeline_cli_usage;
  }
  *out_path = argv[1];
  return k_firmware_pipeline_cli_ok;
}
