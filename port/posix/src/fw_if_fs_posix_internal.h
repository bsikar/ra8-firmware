/**
 * @file fw_if_fs_posix_internal.h
 * @brief Shared errno/descriptor helpers for the POSIX filesystem port.
 * @ingroup grp_io
 *
 * @details
 * Defines the fixed caller-workspace layouts and bounded helper contracts used
 * across the POSIX adapter translation units. These declarations keep hosted
 * descriptor, timestamp, path, and transaction-name policy private to the
 * port while avoiding duplicated platform logic.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdint.h>
#include <time.h>

#include "fw_if_fs.h"
#include "ra8_attributes.h"
#include "ra8_err.h"

/** @brief Local component and stage-search bounds. */
typedef enum : uint16_t {
  k_posix_stage_leaf_span = 13U,  /**< Stage leaf plus terminating NUL.     */
  k_posix_max_open_files  = 64U,  /**< Truthful hosted descriptor capacity. */
  k_posix_component_cap   = 256U, /**< Component buffer including NUL.      */
  k_posix_file_mode       = 0600, /**< Owner-only created-file mode.        */
  k_posix_directory_mode  = 0700, /**< Owner-only created-directory mode.   */
  k_posix_stage_attempts  = 64U,  /**< Collision-search attempt cap.        */
} posix_limits_t;

/** @brief Hexadecimal stage-name encoding constants. */
typedef enum : uint8_t {
  k_posix_hex_nibble_bits  = 4U, /**< Bits represented by one hex digit. */
  k_posix_stage_hex_digits = 6U, /**< Hex digits in a stage identifier.  */
  /** @brief Highest valid index in a stage identifier. */
  k_posix_hex_last_digit  = k_posix_stage_hex_digits - 1U,
  k_posix_hex_nibble_mask = 0x0FU, /**< Low-nibble mask. */
} posix_hex_limits_t;

/** @brief POSIX timestamp and transaction arithmetic constants. */
typedef enum : uint32_t {
  k_posix_epoch_year_offset   = 1900U,       /**< `struct tm` year origin. */
  k_posix_nanosecond_max      = 999999999U,  /**< Largest valid subsecond. */
  k_posix_transaction_id_mask = 0x00FFFFFFU, /**< Six hexadecimal digits.  */
} posix_numeric_limits_t;

/** @brief POSIX state placed in caller file workspace. */
typedef struct {
  int fd;
} posix_file_state_t;

/** @brief POSIX state placed in caller transaction workspace. */
typedef struct {
  char                       destination[k_fw_fs_path_cap];
  char                       stage[k_fw_fs_path_cap];
  posix_file_state_t         file_state;
  fw_fs_transaction_policy_t policy;
  bool                       writer_open;
  bool                       stage_exists;
} posix_transaction_state_t;

/** @brief Map one captured errno value into ::ra8_err_t. */
RA8_PRIV [[nodiscard]] ra8_err_t priv_fs_posix_errno(int value);

/** @brief Close exactly once and invalidate the caller's descriptor. */
RA8_PRIV [[nodiscard]] ra8_err_t priv_fs_posix_close_fd(int* fd);

/** @brief Convert a POSIX UTC instant into the portable civil representation. */
RA8_PRIV [[nodiscard]] fw_fs_timestamp_t priv_fs_posix_timestamp(time_t seconds, long nanoseconds);

/** @brief Copy one bounded portable path. */
RA8_PRIV [[nodiscard]] ra8_err_t priv_fs_posix_copy_path(char* out, const char* path);

/** @brief Build an 8.3-compatible sibling transaction path. */
RA8_PRIV [[nodiscard]] ra8_err_t
priv_fs_posix_stage_path(const char* destination, uint32_t id, char* out);
