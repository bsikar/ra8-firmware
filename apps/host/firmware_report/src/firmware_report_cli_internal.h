// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

/**
 * @file firmware_report_cli_internal.h
 * @brief Internal command-line validation contract for firmware_report.
 * @details Separates deterministic argument policy from hosted file processing.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "ra8_attributes.h"

/**
 * @enum firmware_report_cli_status_t
 * @brief Command-line validation result.
 * @details Values are process exit statuses used by the host application.
 * @invariant Every value is zero or two.
 * @code
 * firmware_report_cli_status_t status = k_firmware_report_cli_ok;
 * @endcode
 * @see priv_firmware_report_parse_args
 */
typedef enum : int {
  k_firmware_report_cli_ok    = 0, /**< One non-empty image path was supplied. */
  k_firmware_report_cli_usage = 2, /**< Arguments do not satisfy the contract. */
} firmware_report_cli_status_t;

/**
 * @brief Validate the command line and return its image path.
 * @details Accepts exactly an application name and one non-empty path. Failure leaves `*out_path`
 * unchanged; the path string remains owned by the process argument vector.
 * @param[in] argc Number of argument-vector entries.
 * @param[in] argv Argument-vector entries with a readable path at index one when valid.
 * @param[in,out] out_path Writable destination for the borrowed path pointer.
 * @return Command-line validation status.
 * @retval k_firmware_report_cli_ok Path stored successfully.
 * @retval k_firmware_report_cli_usage Count, vector, destination, or path is invalid.
 * @pre `argc` describes the readable portion of `argv` when `argv` is non-null.
 * @pre `out_path`, when non-null, points to writable pointer storage.
 * @post Success stores `argv[1]` without copying or taking ownership.
 * @post Failure leaves `*out_path` unchanged when it is writable.
 * @note Thread-safe; the function uses no shared state.
 * @since 0.1.0
 */
RA8_PRIV firmware_report_cli_status_t priv_firmware_report_parse_args(int          argc,
                                                                      char**       argv,
                                                                      const char** out_path);
