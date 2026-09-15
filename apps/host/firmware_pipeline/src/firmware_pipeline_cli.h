// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

/**
 * @file firmware_pipeline_cli.h
 * @brief Internal deterministic command-line contract.
 * @details Separates C argument policy from file processing and ABI composition.
 */

#pragma once

/**
 * @enum firmware_pipeline_cli_status_t
 * @brief Command-line validation result.
 * @details Values are directly usable as hosted process statuses.
 * @invariant Every parse returns one declared value.
 * @code
 * firmware_pipeline_cli_status_t status = k_firmware_pipeline_cli_ok;
 * @endcode
 * @see firmware_pipeline_parse_args
 */
typedef enum : int {
  k_firmware_pipeline_cli_ok    = 0, /**< One non-empty input path was supplied.  */
  k_firmware_pipeline_cli_usage = 2, /**< Arguments violate the command contract. */
} firmware_pipeline_cli_status_t;

/**
 * @brief Validate and borrow the sole input path.
 * @details Accepts exactly two arguments and changes output only on success.
 * @param[in] argc Argument count.
 * @param[in] argv Argument vector.
 * @param[in,out] out_path Writable borrowed-path destination.
 * @return Command-line status.
 * @retval k_firmware_pipeline_cli_ok Path published.
 * @retval k_firmware_pipeline_cli_usage Input was invalid.
 * @pre `argc` describes `argv` when the vector is non-null.
 * @pre `out_path`, when non-null, is writable.
 * @post Success stores `argv[1]` without taking ownership.
 * @post Failure leaves writable output unchanged.
 * @note Thread-safe; no shared state is used.
 * @since 0.1.0
 */
firmware_pipeline_cli_status_t
firmware_pipeline_parse_args(int argc, char** argv, const char** out_path);
