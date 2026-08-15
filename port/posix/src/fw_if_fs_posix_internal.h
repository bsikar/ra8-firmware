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
#include "ra8_err.h"

/** @brief Local component and stage-search bounds. */
typedef enum : uint16_t {
  k_posix_component_cap  = 256U,
  k_posix_stage_attempts = 64U,
} posix_limits_t;

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
[[nodiscard]] ra8_err_t fw_fs_posix_errno(int value);

/** @brief Close exactly once and invalidate the caller's descriptor. */
[[nodiscard]] ra8_err_t fw_fs_posix_close_fd(int* fd);

/** @brief Convert a POSIX UTC instant into the portable civil representation. */
[[nodiscard]] fw_fs_timestamp_t fw_fs_posix_timestamp(time_t seconds, long nanoseconds);

/** @brief Copy one bounded portable path. */
[[nodiscard]] ra8_err_t fw_fs_posix_copy_path(char* out, const char* path);

/** @brief Build an 8.3-compatible sibling transaction path. */
[[nodiscard]] ra8_err_t fw_fs_posix_stage_path(const char* destination, uint32_t id, char* out);
