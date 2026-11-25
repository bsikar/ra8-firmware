/**
 * @file ra8_viewer_host_fd.c
 * @brief POSIX host-descriptor edge for the portable viewer reader core.
 * @details Adapts host regular files and best-effort standard-error diagnostics
 * onto ::ra8_viewer_reader_requirements_source and ::ra8_viewer_open_source so
 * portable core translation units avoid POSIX descriptor headers.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_fmt_io.h"
#include "ra8_viewer_reader.h"
#include "ra8_viewer_reader_internal.h"

#ifndef O_CLOEXEC
/** @brief No-op close-on-exec fallback for hosts lacking the flag. */
#define O_CLOEXEC (0)
#endif

/**
 * @brief Best-effort exact diagnostic fragment over standard error.
 * @details Advances across short writes and retries interrupted writes.
 * @param[in] text NUL-terminated diagnostic fragment.
 * @pre @p text is non-NULL.
 * @pre Standard error may accept descriptor writes.
 * @post The complete fragment was attempted.
 * @post Reader state remains unchanged.
 * @note Concurrent fragments may interleave.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_stderr_write(const char* text)
{
  size_t       offset = 0U;
  const size_t length = strlen(text);
  while (offset < length) {
    const ssize_t written = write(STDERR_FILENO, &text[offset], length - offset);
    if ((written < 0) && (errno == EINTR)) {
      continue;
    }
    if (written <= 0) {
      return;
    }
    offset += (size_t)written;
  }
}

/**
 * @brief Emit the truthful unsupported-format reason.
 * @details Distinguishes codec-contract and unknown formats.
 * @param[in] format Classified path family.
 * @param[in] path NUL-terminated path for the diagnostic.
 * @return Always unsupported.
 * @retval k_ra8_err_not_supported The format cannot open in this checkpoint.
 * @pre @p path is non-NULL and terminated.
 * @pre @p format came from ::ra8_viewer_classify_path.
 * @post A reason naming @p path was attempted.
 * @post No reader state is mutated.
 * @note Descriptor output is best-effort.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_reject(ra8_viewer_format_t format, const char* path)
{
  internal_stderr_write("ra8_viewer: '");
  internal_stderr_write(path);
  if (format == k_ra8_viewer_format_comic_wrap) {
    internal_stderr_write("' recognised but wrapped comics require a streaming or "
                          "descriptor-spooled gzip/XZ output seam");
  } else {
    internal_stderr_write("' has an unsupported file type");
  }
  internal_stderr_write("\n");
  return k_ra8_err_not_supported;
}

/**
 * @brief Open and size one non-empty regular host file.
 * @details Acquires a close-on-exec descriptor and records its stat size.
 * @param[out] file Descriptor context.
 * @param[in] path NUL-terminated host path.
 * @return Open status.
 * @retval k_ra8_ok A non-empty regular file is owned by @p file.
 * @retval k_ra8_err_not_found Open or validation failed.
 * @pre @p file is writable and @p path is NUL-terminated.
 * @post Success publishes one owned descriptor; failure leaves none open.
 * @note The caller must close success via ::internal_host_file_close.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_host_file_open(viewer_file_ctx_t* file, const char* path)
{
  if ((file == nullptr) || (path == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  const int descriptor = open(path, O_RDONLY | O_CLOEXEC);
  if (descriptor < 0) {
    return k_ra8_err_not_found;
  }
  struct stat metadata = {};
  if ((fstat(descriptor, &metadata) != 0) || !S_ISREG(metadata.st_mode) ||
      (metadata.st_size <= 0)) {
    (void)close(descriptor);
    return k_ra8_err_not_found;
  }
  *file =
    (viewer_file_ctx_t){.size = (uint64_t)metadata.st_size, .fd = descriptor, .is_open = true};
  return k_ra8_ok;
}

/**
 * @brief Close an owned host file context if open and reset fields.
 * @param[in,out] ctx Pointer to ::viewer_file_ctx_t, or NULL.
 * @pre @p ctx is NULL or points to an initialized ::viewer_file_ctx_t.
 * @post Any open descriptor is closed and state is marked closed (`fd = -1`).
 * @note Idempotent across repeated calls.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_host_file_close(void* ctx)
{
  viewer_file_ctx_t* file = (viewer_file_ctx_t*)ctx;
  if (file == nullptr) {
    return;
  }
  if (file->is_open && (file->fd >= 0)) {
    (void)close(file->fd);
  }
  *file = (viewer_file_ctx_t){.fd = -1};
}

/** @copydoc ra8_fmt_pread_fn */
RA8_INTERNAL static ra8_err_t
internal_host_pread(void* ctx, uint64_t offset, uint8_t* buffer, size_t length, size_t* out_read)
{
  viewer_file_ctx_t* file = (viewer_file_ctx_t*)ctx;
  if (out_read == nullptr) {
    return k_ra8_err_null_ptr;
  }
  *out_read = 0U;
  if ((file == nullptr) || ((buffer == nullptr) && (length != 0U))) {
    return k_ra8_err_null_ptr;
  }
  if (!file->is_open || (offset > (uint64_t)INT64_MAX)) {
    return k_ra8_err_invalid_state;
  }
  if ((offset >= file->size) || (length == 0U)) {
    return k_ra8_ok;
  }
  const uint64_t available = file->size - offset;
  const size_t   requested = ((uint64_t)length > available) ? (size_t)available : length;
  size_t         completed = 0U;
  while (completed < requested) {
    size_t pass = requested - completed;
    if (pass > (size_t)SSIZE_MAX) {
      pass = (size_t)SSIZE_MAX;
    }
    const ssize_t got =
      pread(file->fd, &buffer[completed], pass, (off_t)(offset + (uint64_t)completed));
    if ((got < 0) && (errno == EINTR)) {
      continue;
    }
    if (got <= 0) {
      if (got == 0) {
        *out_read = completed;
        return k_ra8_ok;
      }
      return k_ra8_err_not_found;
    }
    completed += (size_t)got;
  }
  *out_read = completed;
  return k_ra8_ok;
}

ra8_err_t ra8_viewer_reader_requirements(const char* path, ra8_viewer_reader_requirements_t* out)
{
  if ((path == nullptr) || (out == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  *out                             = (ra8_viewer_reader_requirements_t){};
  const ra8_viewer_format_t format = ra8_viewer_classify_path(path);
  if ((format != k_ra8_viewer_format_jof) && (format != k_ra8_viewer_format_comic) &&
      (format != k_ra8_viewer_format_epub) && (format != k_ra8_viewer_format_rabook)) {
    return internal_reject(format, path);
  }
  viewer_file_ctx_t file  = {.fd = -1};
  ra8_err_t         error = internal_host_file_open(&file, path);
  if (error == k_ra8_ok) {
    const ra8_fmt_source_t source = {
      .read_at = internal_host_pread,
      .ctx     = &file,
      .size    = file.size,
    };
    error = ra8_viewer_reader_requirements_source(format, &source, out);
  }
  internal_host_file_close(&file);
  return error;
}

ra8_err_t ra8_viewer_open(ra8_viewer_reader_t* reader, const char* path)
{
  if ((reader == nullptr) || (path == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  if (!reader->is_bound || reader->is_open || reader->host_file.is_open) {
    return k_ra8_err_invalid_state;
  }
  const ra8_viewer_format_t format = ra8_viewer_classify_path(path);
  if (!priv_viewer_engine_matches(reader, format)) {
    return k_ra8_err_invalid_state;
  }
  ra8_err_t error = internal_host_file_open(&reader->host_file, path);
  if (error == k_ra8_ok) {
    const ra8_fmt_source_t source = {
      .read_at = internal_host_pread,
      .ctx     = &reader->host_file,
      .size    = reader->host_file.size,
    };
    error = priv_viewer_open_bound_source(reader, format, &source, internal_host_file_close);
  }
  if (error != k_ra8_ok) {
    internal_host_file_close(&reader->host_file);
  }
  return error;
}
