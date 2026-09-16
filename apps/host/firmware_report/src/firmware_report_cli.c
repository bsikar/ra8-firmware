// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

/**
 * @file firmware_report_cli.c
 * @brief Deterministic firmware_report command-line validation.
 * @details Implements the internal policy declared by `firmware_report_cli_internal.h`.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>

#include "firmware_report_cli_internal.h"

RA8_PRIV firmware_report_cli_status_t priv_firmware_report_parse_args(int          argc,
                                                                      char**       argv,
                                                                      const char** out_path)
{
  if ((argc != 2) || (argv == nullptr) || (out_path == nullptr) || (argv[1] == nullptr) ||
      (argv[1][0] == '\0')) {
    return k_firmware_report_cli_usage;
  }
  *out_path = argv[1];
  return k_firmware_report_cli_ok;
}
