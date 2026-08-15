/**
 * @file reader_vmem_host.c
 * @brief Bounded raw-descriptor trace composition for reader_vmem
 *
 * @details
 * Trace bytes are written only to a same-directory sibling temporary. A
 * complete trace is synced and atomically renamed into place; reusable cache
 * and workspace logic has no POSIX dependency.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#define _POSIX_C_SOURCE (200809L)

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h> /* POSIX renameat declaration; no hosted stream is used. */
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include "reader_vmem_internal.h"

#ifndef O_CLOEXEC
/** @brief No-op close-on-exec fallback for hosts lacking the flag. */
#define O_CLOEXEC (0)
#endif

#ifndef O_DIRECTORY
/** @brief No-op directory-open fallback for hosts lacking the flag. */
#define O_DIRECTORY (0)
#endif

#ifndef O_NOFOLLOW
/** @brief No-op no-follow fallback for hosts lacking the flag. */
#define O_NOFOLLOW (0)
#endif

/** @brief Host formatting, retry, and creation bounds. */
typedef enum : uint32_t {
  k_host_temp_attempts  = 128U,  /**< Exclusive-create retry bound. */
  k_host_decimal_digits = 20U,   /**< Maximum `uint64_t` digits.    */
  k_host_decimal_base   = 10U,   /**< Decimal conversion radix.     */
  k_host_create_mode    = 0666U, /**< Hosted trace creation mode.   */
} host_limit_t;

void priv_rv_diag(const char* text)
{
  if (text == nullptr) {
    return;
  }
  size_t       offset = 0U;
  const size_t length = strlen(text);
  while (offset < length) {
    const ssize_t put = write(STDERR_FILENO, &text[offset], length - offset);
    if (put < 0 && errno == EINTR) {
      continue;
    }
    if (put <= 0) {
      return;
    }
    offset += (size_t)put;
  }
}

/**
 * @brief Encode one unsigned value into a caller's fixed buffer.
 * @details Builds digits in reverse locally, then emits a forward NUL-terminated spelling.
 * @param[in] value Unsigned integer to encode in base ten.
 * @param[out] out Buffer with capacity for every `uint64_t` digit plus NUL.
 * @return Number of decimal digits, excluding NUL.
 * @retval digits A value from one through ::k_host_decimal_digits.
 * @pre @p out is non-null and has its declared fixed capacity.
 * @pre ::k_host_decimal_digits covers every `uint64_t` value.
 * @post @p out contains the exact NUL-terminated decimal spelling.
 * @post No filesystem or global state changes.
 * @note Pure apart from caller output and thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static size_t internal_decimal(uint64_t value, char out[k_host_decimal_digits + 1U])
{
  char   reverse[k_host_decimal_digits];
  size_t digits = 0U;
  do {
    reverse[digits++] = (char)('0' + (value % (uint64_t)k_host_decimal_base));
    value /= (uint64_t)k_host_decimal_base;
  } while (value != 0U);
  for (size_t i = 0U; i < digits; ++i) {
    out[i] = reverse[digits - i - 1U];
  }
  out[digits] = '\0';
  return digits;
}

void priv_rv_diag_u64(uint64_t value)
{
  char text[k_host_decimal_digits + 1U];
  (void)internal_decimal(value, text);
  priv_rv_diag(text);
}

/**
 * @brief Write an exact positioned byte range.
 * @details Retries interrupted calls, handles short writes, and rejects offset overflow.
 * @param[in] fd Open private trace descriptor.
 * @param[in] offset Absolute starting byte offset.
 * @param[in] bytes Source spanning @p length bytes.
 * @param[in] length Exact byte count to append positionally.
 * @return Whether every requested byte was written.
 * @retval true Complete range reached the host page cache.
 * @retval false Offset overflow or unrecoverable/zero write occurred.
 * @pre @p bytes spans @p length readable bytes.
 * @pre @p fd remains open for the duration of the call.
 * @post Descriptor stream position is unchanged.
 * @post Failure may leave only a strict prefix in the unpublished temporary.
 * @note Not thread-safe for overlapping ranges.
 * @since 0.1.0
 */
RA8_INTERNAL static bool
internal_pwrite_exact(int fd, uint64_t offset, const uint8_t* bytes, size_t length)
{
  if (offset > (uint64_t)INT64_MAX || length > ((uint64_t)INT64_MAX - offset)) {
    return false;
  }
  size_t done = 0U;
  while (done < length) {
    size_t request = length - done;
    if (request > (size_t)SSIZE_MAX) {
      request = (size_t)SSIZE_MAX;
    }
    const ssize_t put = pwrite(fd, &bytes[done], request, (off_t)(offset + done));
    if (put < 0 && errno == EINTR) {
      continue;
    }
    if (put <= 0) {
      return false;
    }
    done += (size_t)put;
  }
  return true;
}

/**
 * @brief Split a bounded output path into parent and leaf.
 * @details Rejects truncation, an empty leaf, and dot traversal components.
 * @param[in] path Requested final trace path.
 * @param[out] parent Receives the existing parent path.
 * @param[out] leaf Receives the final filename component.
 * @return Whether both complete components fit and are usable.
 * @retval true Both outputs are NUL-terminated.
 * @retval false Path is empty, overlong, or ends in an unusable leaf.
 * @pre All pointers are non-null and outputs have their declared capacities.
 * @pre @p path is NUL-terminated.
 * @post Success outputs reconstruct @p path.
 * @post Failure touches no filesystem object.
 * @note Pure and thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_split_path(const char* path,
                                             char        parent[k_rv_host_path_cap],
                                             char        leaf[k_rv_host_name_cap])
{
  const size_t length = strlen(path);
  if (length == 0U || length >= (size_t)k_rv_host_path_cap) {
    return false;
  }
  size_t slash = length;
  while (slash > 0U && path[slash - 1U] != '/') {
    --slash;
  }
  const size_t leaf_bytes = length - slash;
  if (leaf_bytes == 0U || leaf_bytes >= (size_t)k_rv_host_name_cap) {
    return false;
  }
  (void)memcpy(leaf, &path[slash], leaf_bytes);
  leaf[leaf_bytes] = '\0';
  if (strcmp(leaf, ".") == 0 || strcmp(leaf, "..") == 0) {
    return false;
  }
  if (slash == 0U) {
    (void)memcpy(parent, ".", 2U);
  } else if (slash == 1U) {
    (void)memcpy(parent, "/", 2U);
  } else {
    (void)memcpy(parent, path, slash - 1U);
    parent[slash - 1U] = '\0';
  }
  return true;
}

/**
 * @brief Build one hidden sibling-temporary leaf.
 * @details Combines fixed prefix, process identifier, and retry index without allocation.
 * @param[out] out Temporary-leaf buffer.
 * @param[in] process Process identifier.
 * @param[in] attempt Exclusive-create retry index.
 * @return Whether the complete slash-free name fit.
 * @retval true @p out contains one NUL-terminated hidden leaf.
 * @retval false Fixed capacity was insufficient.
 * @pre @p out has ::k_rv_host_name_cap writable bytes.
 * @pre Numeric values are finite and decimal helper bounds hold.
 * @post Success builds a name determined only by the arguments.
 * @post No filesystem object is touched.
 * @note Pure and thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static bool
internal_temp_name(char out[k_rv_host_name_cap], uint64_t process, uint32_t attempt)
{
  static const char s_prefix[] = ".reader_vmem.tmp.";
  char              number[k_host_decimal_digits + 1U];
  size_t            offset = sizeof(s_prefix) - 1U;
  (void)memcpy(out, s_prefix, offset);
  const uint64_t values[2] = {process, attempt};
  for (size_t field = 0U; field < 2U; ++field) {
    const size_t digits = internal_decimal(values[field], number);
    if (offset + digits + 2U > (size_t)k_rv_host_name_cap) {
      return false;
    }
    (void)memcpy(&out[offset], number, digits);
    offset += digits;
    out[offset++] = (field == 0U) ? '.' : '\0';
  }
  return true;
}

bool priv_rv_trace_begin(const char* path, rv_trace_t* trace)
{
  if (path == nullptr || trace == nullptr) {
    return false;
  }
  *trace = (rv_trace_t){.directory_fd = -1, .trace_fd = -1};
  char parent[k_rv_host_path_cap];
  if (!internal_split_path(path, parent, trace->final_name)) {
    return false;
  }
  trace->directory_fd = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (trace->directory_fd < 0) {
    return false;
  }
  for (uint32_t attempt = 0U; attempt < (uint32_t)k_host_temp_attempts; ++attempt) {
    if (!internal_temp_name(trace->temp_name, (uint64_t)getpid(), attempt)) {
      break;
    }
    trace->trace_fd = openat(trace->directory_fd,
                             trace->temp_name,
                             O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                             (mode_t)k_host_create_mode);
    if (trace->trace_fd >= 0 || errno != EEXIST) {
      break;
    }
  }
  if (trace->trace_fd < 0) {
    priv_rv_trace_abort(trace);
    return false;
  }
  trace->temp_exists = true;
  return true;
}

bool priv_rv_trace_reference(rv_trace_t* trace, uint32_t object_id, uint32_t frame)
{
  char   line[(2U * k_host_decimal_digits) + 3U];
  size_t length  = internal_decimal(object_id, line);
  line[length++] = ' ';
  char         number[k_host_decimal_digits + 1U];
  const size_t frame_digits = internal_decimal(frame, number);
  (void)memcpy(&line[length], number, frame_digits);
  length += frame_digits;
  line[length++] = '\n';
  if (trace == nullptr || trace->trace_fd < 0 || trace->io_failed ||
      !internal_pwrite_exact(trace->trace_fd, trace->offset, (const uint8_t*)line, length)) {
    if (trace != nullptr) {
      trace->io_failed = true;
    }
    return false;
  }
  trace->offset += length;
  return true;
}

bool priv_rv_trace_commit(rv_trace_t* trace)
{
  if (trace == nullptr || trace->trace_fd < 0 || trace->directory_fd < 0 || trace->io_failed) {
    priv_rv_trace_abort(trace);
    return false;
  }
  const bool file_synced = fsync(trace->trace_fd) == 0;
  const bool file_closed = close(trace->trace_fd) == 0;
  trace->trace_fd        = -1;
  if (!file_synced || !file_closed) {
    priv_rv_trace_abort(trace);
    return false;
  }
  if (renameat(trace->directory_fd, trace->temp_name, trace->directory_fd, trace->final_name) !=
      0) {
    priv_rv_trace_abort(trace);
    return false;
  }
  trace->temp_exists = false;
  const bool synced  = fsync(trace->directory_fd) == 0;
  (void)close(trace->directory_fd);
  trace->directory_fd = -1;
  return synced;
}

void priv_rv_trace_abort(rv_trace_t* trace)
{
  if (trace == nullptr) {
    return;
  }
  if (trace->trace_fd >= 0) {
    (void)close(trace->trace_fd);
    trace->trace_fd = -1;
  }
  if (trace->temp_exists && trace->directory_fd >= 0) {
    (void)unlinkat(trace->directory_fd, trace->temp_name, 0);
    trace->temp_exists = false;
  }
  if (trace->directory_fd >= 0) {
    (void)close(trace->directory_fd);
    trace->directory_fd = -1;
  }
}
