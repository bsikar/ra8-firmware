/**
 * @file fw_if_fs_posix_common.c
 * @brief Error and descriptor lifecycle helpers for the POSIX filesystem port.
 * @ingroup grp_io
 *
 * @par Tag
 * [Ring 4 / Host Port] {World: Host}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>

#include "fw_if_fs_posix_internal.h"
#include "ra8_err.h"

ra8_err_t fw_fs_posix_errno(int value)
{
  if (value == 0) {
    return k_ra8_ok;
  }
  if (value == ENOENT || value == ENOTDIR) {
    return k_ra8_err_not_found;
  }
  if (value == EEXIST) {
    return k_ra8_err_exists;
  }
  if (value == ENOSPC || value == EDQUOT || value == EMFILE || value == ENFILE) {
    return k_ra8_err_no_mem;
  }
  if (value == ENOTEMPTY) {
    return k_ra8_err_not_empty;
  }
  if (value == EACCES || value == EPERM || value == ELOOP) {
    return k_ra8_err_access_denied;
  }
  if (value == EINVAL || value == EXDEV || value == ENAMETOOLONG || value == EISDIR) {
    return k_ra8_err_invalid_arg;
  }
  if (value == EFBIG || value == EOVERFLOW) {
    return k_ra8_err_invalid_size;
  }
  if (value == EBADF) {
    return k_ra8_err_invalid_state;
  }
  if (value == EBUSY) {
    return k_ra8_err_busy;
  }
#if defined(ENOTSUP)
  if (value == ENOTSUP) {
    return k_ra8_err_not_supported;
  }
#endif
  return k_ra8_fail;
}

ra8_err_t fw_fs_posix_close_fd(int* fd)
{
  const int value = *fd;
  *fd             = -1;
  if (value < 0) {
    return k_ra8_err_invalid_state;
  }
  if (close(value) != 0) {
    return fw_fs_posix_errno(errno);
  }
  return k_ra8_ok;
}

fw_fs_timestamp_t fw_fs_posix_timestamp(time_t seconds, long nanoseconds)
{
  fw_fs_timestamp_t portable = {};
  struct tm         utc      = {};
  if (nanoseconds < 0L || nanoseconds > 999999999L || gmtime_r(&seconds, &utc) == nullptr) {
    return portable;
  }
  const int64_t year = (int64_t)utc.tm_year + 1900;
  if (year < 0 || year > (int64_t)UINT16_MAX) {
    return portable;
  }
  portable.value.nanosecond     = (uint32_t)nanoseconds;
  portable.value.year           = (uint16_t)year;
  portable.value.utc_offset_min = 0;
  portable.value.month          = (uint8_t)(utc.tm_mon + 1);
  portable.value.day            = (uint8_t)utc.tm_mday;
  portable.value.hour           = (uint8_t)utc.tm_hour;
  portable.value.minute         = (uint8_t)utc.tm_min;
  portable.value.second         = (uint8_t)utc.tm_sec;
  portable.valid                = true;
  portable.utc_offset_valid     = true;
  return portable;
}

ra8_err_t fw_fs_posix_copy_path(char* out, const char* path)
{
  for (uint16_t i = 0U; i < (uint16_t)k_fw_fs_path_cap; ++i) {
    out[i] = path[i];
    if (path[i] == '\0') {
      return k_ra8_ok;
    }
  }
  return k_ra8_err_invalid_size;
}

/** @brief Render the bounded six-digit transaction suffix. */
static void internal_hex6(char out[6], uint32_t value)
{
  static const char digits[] = "0123456789abcdef";
  for (uint8_t i = 0U; i < 6U; ++i) {
    const uint8_t shift = (uint8_t)((5U - i) * 4U);
    out[i]              = digits[(value >> shift) & 0x0FU];
  }
}

ra8_err_t fw_fs_posix_stage_path(const char* destination, uint32_t id, char* out)
{
  uint16_t last_slash = 0U;
  uint16_t length     = 0U;
  while (length < (uint16_t)k_fw_fs_path_cap && destination[length] != '\0') {
    if (destination[length] == '/') {
      last_slash = length;
    }
    ++length;
  }
  if (length >= (uint16_t)k_fw_fs_path_cap) {
    return k_ra8_err_invalid_size;
  }
  if ((uint16_t)(last_slash + 13U) >= (uint16_t)k_fw_fs_path_cap) {
    return k_ra8_err_invalid_size;
  }
  for (uint16_t i = 0U; i <= last_slash; ++i) {
    out[i] = destination[i];
  }
  uint16_t cursor = (uint16_t)(last_slash + 1U);
  out[cursor++]   = 'T';
  out[cursor++]   = 'X';
  internal_hex6(&out[cursor], id & 0x00FFFFFFUL);
  cursor        = (uint16_t)(cursor + 6U);
  out[cursor++] = '.';
  out[cursor++] = 'T';
  out[cursor++] = 'M';
  out[cursor++] = 'P';
  out[cursor]   = '\0';
  return k_ra8_ok;
}
