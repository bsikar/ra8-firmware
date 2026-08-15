/**
 * @file emu_host_io.c
 * @brief Bounded raw-descriptor host-I/O implementation.
 * @details Implements bounded EINTR/short-I/O loops, memory-only formatting,
 * descriptor-selected process sinks, regular-file reads, and atomic publication.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "emu_host_io_internal.h"

enum : size_t {
  k_emu_io_format_cap = 1024U, /**< Per-call stack formatting capacity. */
  k_emu_io_eintr_max  = 8U,    /**< Consecutive EINTR retry bound.      */
};

/**
 * @brief Call the default sequential input primitive.
 * @details Thin hook target keeping production and injected operations shape-compatible.
 * @param[in] fd Raw descriptor.
 * @param[out] buf Destination bytes.
 * @param[in] count Requested byte count.
 * @return The host primitive result.
 * @retval -1 The host primitive failed.
 * @pre @p fd identifies an open descriptor.
 * @pre @p buf spans @p count writable bytes when count is nonzero.
 * @post The descriptor cursor advances by the returned positive count.
 * @post On failure errno contains the host error.
 * @note Thread safety is inherited from the supplied descriptor.
 * @since 0.1.0
 */
RA8_INTERNAL static ssize_t internal_read(int fd, void* buf, size_t count)
{
  return read(fd, buf, count);
}

/**
 * @brief Call the default sequential output primitive.
 * @details Thin hook target keeping production and injected operations shape-compatible.
 * @param[in] fd Raw descriptor.
 * @param[in] buf Source bytes.
 * @param[in] count Requested byte count.
 * @return The host primitive result.
 * @retval -1 The host primitive failed.
 * @pre @p fd identifies an open descriptor.
 * @pre @p buf spans @p count readable bytes when count is nonzero.
 * @post The descriptor cursor advances by the returned positive count.
 * @post On failure errno contains the host error.
 * @note Thread safety is inherited from the supplied descriptor.
 * @since 0.1.0
 */
RA8_INTERNAL static ssize_t internal_write(int fd, const void* buf, size_t count)
{
  return write(fd, buf, count);
}

/**
 * @brief Call the default positioned input primitive.
 * @details Reads without changing the descriptor cursor.
 * @param[in] fd Raw descriptor.
 * @param[out] buf Destination bytes.
 * @param[in] count Requested byte count.
 * @param[in] offset Starting byte offset.
 * @return The host primitive result.
 * @retval -1 The host primitive failed.
 * @pre @p fd identifies an open seekable descriptor.
 * @pre @p buf spans @p count writable bytes when count is nonzero.
 * @post The descriptor cursor is unchanged.
 * @post On failure errno contains the host error.
 * @note Thread safety is inherited from the supplied descriptor.
 * @since 0.1.0
 */
RA8_INTERNAL static ssize_t internal_pread(int fd, void* buf, size_t count, off_t offset)
{
  return pread(fd, buf, count, offset);
}

/**
 * @brief Call the default positioned output primitive.
 * @details Writes without changing the descriptor cursor.
 * @param[in] fd Raw descriptor.
 * @param[in] buf Source bytes.
 * @param[in] count Requested byte count.
 * @param[in] offset Starting byte offset.
 * @return The host primitive result.
 * @retval -1 The host primitive failed.
 * @pre @p fd identifies an open seekable descriptor.
 * @pre @p buf spans @p count readable bytes when count is nonzero.
 * @post The descriptor cursor is unchanged.
 * @post On failure errno contains the host error.
 * @note Thread safety is inherited from the supplied descriptor.
 * @since 0.1.0
 */
RA8_INTERNAL static ssize_t internal_pwrite(int fd, const void* buf, size_t count, off_t offset)
{
  return pwrite(fd, buf, count, offset);
}

static const emu_io_ops_t s_k_default_ops = {
  .read_fn   = internal_read,
  .write_fn  = internal_write,
  .pread_fn  = internal_pread,
  .pwrite_fn = internal_pwrite,
};
static const emu_io_ops_t* s_ops    = &s_k_default_ops;
static int                 s_out_fd = STDOUT_FILENO;
static int                 s_err_fd = STDERR_FILENO;

/**
 * @brief Construct one complete public operation result.
 * @details Centralises field initialisation for every result path.
 * @param[in] status Semantic operation status.
 * @param[in] transferred Completed byte count.
 * @param[in] os_error Captured host error.
 * @return Fully initialised result value.
 * @retval emu_io_result_t The supplied fields in their corresponding members.
 * @pre @p status is a valid emu_io_status_t value.
 * @pre @p transferred reflects only completed bytes.
 * @post Every result member is initialised.
 * @post No global or host state changes.
 * @note Pure value constructor; thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static emu_io_result_t
internal_result(emu_io_status_t status, size_t transferred, int os_error)
{
  return (emu_io_result_t){.status = status, .transferred = transferred, .os_error = os_error};
}

void priv_emu_io_configure(int out_fd, int err_fd, const emu_io_ops_t* ops)
{
  s_out_fd = out_fd;
  s_err_fd = err_fd;
  s_ops    = (ops == nullptr) ? &s_k_default_ops : ops;
}

typedef ssize_t (*internal_transfer_fn_t)(int fd, void* buf, size_t count, off_t offset);

/**
 * @brief Complete one exact raw transfer with bounded EINTR retries.
 * @details Repeats partial operations, resets the retry budget after progress, and reports EOF.
 * @param[in] fd Raw descriptor.
 * @param[in,out] buf Transfer buffer.
 * @param[in] count Exact requested byte count.
 * @param[in] offset Initial positioned-I/O offset.
 * @param[in] fn Injected transfer primitive.
 * @param[in] input True for EOF semantics, false for zero-write error semantics.
 * @return Complete status, progress, and captured errno.
 * @retval emu_io_result_t The exact transfer outcome.
 * @pre @p fn is non-null for a valid request.
 * @pre @p buf spans @p count bytes when count is nonzero.
 * @post Success reports exactly @p count transferred bytes.
 * @post Failure reports only bytes completed before the fault.
 * @note At most eight consecutive EINTR responses are retried.
 * @since 0.1.0
 */
RA8_INTERNAL static emu_io_result_t internal_transfer(int                    fd,
                                                      void*                  buf,
                                                      size_t                 count,
                                                      off_t                  offset,
                                                      internal_transfer_fn_t fn,
                                                      bool                   input)
{
  if ((fd < 0) || ((buf == nullptr) && (count != 0U)) || (fn == nullptr)) {
    return internal_result(k_emu_io_invalid, 0U, 0);
  }
  size_t done        = 0U;
  size_t interrupted = 0U;
  while (done < count) {
    ssize_t amount = fn(fd, (uint8_t*)buf + done, count - done, offset + (off_t)done);
    if (amount > 0) {
      done += (size_t)amount;
      interrupted = 0U;
      continue;
    }
    if (amount == 0) {
      return internal_result(input ? k_emu_io_eof : k_emu_io_error, done, input ? 0 : EIO);
    }
    if ((errno == EINTR) && (interrupted < k_emu_io_eintr_max)) {
      interrupted++;
      continue;
    }
    return internal_result(k_emu_io_error, done, errno);
  }
  return internal_result(k_emu_io_ok, done, 0);
}

/**
 * @brief Adapt sequential input to the common positioned callback shape.
 * @details Ignores @p offset and dispatches through the configured operation table.
 * @param[in] fd Raw descriptor.
 * @param[out] buf Destination bytes.
 * @param[in] count Requested byte count.
 * @param[in] offset Ignored compatibility offset.
 * @return Configured primitive result.
 * @retval -1 The configured primitive failed.
 * @pre The configured read callback is non-null.
 * @pre @p buf spans @p count writable bytes when nonzero.
 * @post Sequential cursor effects come only from the configured callback.
 * @post No other global state changes.
 * @note Uses process-wide injected operations; not reconfiguration-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static ssize_t internal_read_adapter(int fd, void* buf, size_t count, off_t offset)
{
  (void)offset;
  return s_ops->read_fn(fd, buf, count);
}

/**
 * @brief Adapt sequential output to the common positioned callback shape.
 * @details Ignores @p offset and dispatches through the configured operation table.
 * @param[in] fd Raw descriptor.
 * @param[in,out] buf Source bytes carried in the common mutable callback shape.
 * @param[in] count Requested byte count.
 * @param[in] offset Ignored compatibility offset.
 * @return Configured primitive result.
 * @retval -1 The configured primitive failed.
 * @pre The configured write callback is non-null.
 * @pre @p buf spans @p count readable bytes when nonzero.
 * @post Sequential cursor effects come only from the configured callback.
 * @post No other global state changes.
 * @note Uses process-wide injected operations; not reconfiguration-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static ssize_t internal_write_adapter(int fd, void* buf, size_t count, off_t offset)
{
  (void)offset;
  return s_ops->write_fn(fd, buf, count);
}

/**
 * @brief Dispatch positioned input through the configured operation table.
 * @details Preserves the exact offset passed by the transfer loop.
 * @param[in] fd Raw descriptor.
 * @param[out] buf Destination bytes.
 * @param[in] count Requested byte count.
 * @param[in] offset Starting byte offset.
 * @return Configured primitive result.
 * @retval -1 The configured primitive failed.
 * @pre The configured pread callback is non-null.
 * @pre @p buf spans @p count writable bytes when nonzero.
 * @post Descriptor cursor state is controlled by the callback contract.
 * @post No other global state changes.
 * @note Uses process-wide injected operations; not reconfiguration-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static ssize_t internal_pread_adapter(int fd, void* buf, size_t count, off_t offset)
{
  return s_ops->pread_fn(fd, buf, count, offset);
}

/**
 * @brief Dispatch positioned output through the configured operation table.
 * @details Preserves the exact offset passed by the transfer loop.
 * @param[in] fd Raw descriptor.
 * @param[in,out] buf Source bytes carried in the common mutable callback shape.
 * @param[in] count Requested byte count.
 * @param[in] offset Starting byte offset.
 * @return Configured primitive result.
 * @retval -1 The configured primitive failed.
 * @pre The configured pwrite callback is non-null.
 * @pre @p buf spans @p count readable bytes when nonzero.
 * @post Descriptor cursor state is controlled by the callback contract.
 * @post No other global state changes.
 * @note Uses process-wide injected operations; not reconfiguration-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static ssize_t internal_pwrite_adapter(int fd, void* buf, size_t count, off_t offset)
{
  return s_ops->pwrite_fn(fd, buf, count, offset);
}

emu_io_result_t priv_emu_io_read_exact(int fd, void* buf, size_t count)
{
  return internal_transfer(fd, buf, count, 0, internal_read_adapter, true);
}

emu_io_result_t priv_emu_io_write_exact(int fd, const void* buf, size_t count)
{
  return internal_transfer(fd, (void*)buf, count, 0, internal_write_adapter, false);
}

emu_io_result_t priv_emu_io_pread_exact(int fd, void* buf, size_t count, off_t offset)
{
  return internal_transfer(fd, buf, count, offset, internal_pread_adapter, true);
}

emu_io_result_t priv_emu_io_pwrite_exact(int fd, const void* buf, size_t count, off_t offset)
{
  return internal_transfer(fd, (void*)buf, count, offset, internal_pwrite_adapter, false);
}

/**
 * @brief Render one bounded format operation and write it exactly.
 * @details Uses memory-only formatting and emits no partial text when truncation is detected.
 * @param[in] fd Destination raw descriptor.
 * @param[in] format Format string.
 * @param[in] args Format arguments.
 * @return Complete formatting/write result.
 * @retval emu_io_result_t Success, truncation, invalid input, or host failure.
 * @pre @p fd is non-negative.
 * @pre @p format is non-null and matches @p args.
 * @post Truncation writes zero bytes.
 * @post Success writes the complete formatted payload exactly once in order.
 * @note The fixed 1024-byte scratch buffer lives on the caller stack.
 * @since 0.1.0
 */
RA8_INTERNAL static emu_io_result_t internal_vformat(int fd, const char* format, va_list args)
{
  if ((fd < 0) || (format == nullptr)) {
    return internal_result(k_emu_io_invalid, 0U, 0);
  }
  char    text[k_emu_io_format_cap];
  va_list copy;
  va_copy(copy, args);
  const int needed = vsnprintf(text, sizeof(text), format, copy);
  va_end(copy);
  if (needed < 0) {
    return internal_result(k_emu_io_error, 0U, EILSEQ);
  }
  if ((size_t)needed >= sizeof(text)) {
    return internal_result(k_emu_io_truncated, 0U, 0);
  }
  return priv_emu_io_write_exact(fd, text, (size_t)needed);
}

emu_io_result_t priv_emu_io_out_text(const char* text)
{
  return (text == nullptr) ? internal_result(k_emu_io_invalid, 0U, 0)
                           : priv_emu_io_write_exact(s_out_fd, text, strlen(text));
}

emu_io_result_t priv_emu_io_err_text(const char* text)
{
  return (text == nullptr) ? internal_result(k_emu_io_invalid, 0U, 0)
                           : priv_emu_io_write_exact(s_err_fd, text, strlen(text));
}

emu_io_result_t priv_emu_io_err_bytes(const void* bytes, size_t length)
{
  return priv_emu_io_write_exact(s_err_fd, bytes, length);
}

emu_io_result_t priv_emu_io_outf(const char* format, ...)
{
  va_list args;
  va_start(args, format);
  const emu_io_result_t result = internal_vformat(s_out_fd, format, args);
  va_end(args);
  return result;
}

emu_io_result_t priv_emu_io_errf(const char* format, ...)
{
  va_list args;
  va_start(args, format);
  const emu_io_result_t result = internal_vformat(s_err_fd, format, args);
  va_end(args);
  return result;
}

emu_io_result_t priv_emu_io_file_text(int fd, const char* text)
{
  return (text == nullptr) ? internal_result(k_emu_io_invalid, 0U, 0)
                           : priv_emu_io_write_exact(fd, text, strlen(text));
}

emu_io_result_t priv_emu_io_file_char(int fd, char value)
{
  return priv_emu_io_write_exact(fd, &value, 1U);
}

emu_io_result_t priv_emu_io_filef(int fd, const char* format, ...)
{
  va_list args;
  va_start(args, format);
  const emu_io_result_t result = internal_vformat(fd, format, args);
  va_end(args);
  return result;
}

emu_io_result_t priv_emu_io_open_read(const char* path, emu_io_file_t* file)
{
  if ((path == nullptr) || (path[0] == '\0') || (file == nullptr)) {
    return internal_result(k_emu_io_invalid, 0U, 0);
  }
  file->fd     = -1;
  file->size   = 0;
  const int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return internal_result(k_emu_io_error, 0U, errno);
  }
  struct stat info = {};
  if ((fstat(fd, &info) != 0) || !S_ISREG(info.st_mode) || (info.st_size < 0)) {
    const int saved = (errno == 0) ? EINVAL : errno;
    (void)close(fd);
    return internal_result(k_emu_io_error, 0U, saved);
  }
  file->fd   = fd;
  file->size = (int64_t)info.st_size;
  return internal_result(k_emu_io_ok, 0U, 0);
}

emu_io_result_t priv_emu_io_close(emu_io_file_t* file)
{
  if ((file == nullptr) || (file->fd < 0)) {
    return internal_result(k_emu_io_invalid, 0U, 0);
  }
  const int fd = file->fd;
  file->fd     = -1;
  file->size   = 0;
  return (close(fd) == 0) ? internal_result(k_emu_io_ok, 0U, 0)
                          : internal_result(k_emu_io_error, 0U, errno);
}

emu_io_result_t priv_emu_io_txn_begin(const char* path, emu_io_txn_t* txn)
{
  if ((path == nullptr) || (txn == nullptr)) {
    return internal_result(k_emu_io_invalid, 0U, 0);
  }
  const int final_n = snprintf(txn->final, sizeof(txn->final), "%s", path);
  const int temp_n  = snprintf(txn->temp, sizeof(txn->temp), "%s.tmp.XXXXXX", path);
  if ((final_n < 1) || ((size_t)final_n >= sizeof(txn->final)) || (temp_n < 1) ||
      ((size_t)temp_n >= sizeof(txn->temp))) {
    txn->fd = -1;
    return internal_result(k_emu_io_truncated, 0U, 0);
  }
  txn->fd = mkstemp(txn->temp);
  if (txn->fd < 0) {
    return internal_result(k_emu_io_error, 0U, errno);
  }
  (void)fcntl(txn->fd, F_SETFD, FD_CLOEXEC);
  return internal_result(k_emu_io_ok, 0U, 0);
}

emu_io_result_t priv_emu_io_txn_commit(emu_io_txn_t* txn)
{
  if ((txn == nullptr) || (txn->fd < 0)) {
    return internal_result(k_emu_io_invalid, 0U, 0);
  }
  if (fsync(txn->fd) != 0) {
    return internal_result(k_emu_io_error, 0U, errno);
  }
  if (close(txn->fd) != 0) {
    txn->fd = -1;
    return internal_result(k_emu_io_error, 0U, errno);
  }
  txn->fd = -1;
  if (rename(txn->temp, txn->final) != 0) {
    return internal_result(k_emu_io_error, 0U, errno);
  }
  txn->temp[0] = '\0';
  return internal_result(k_emu_io_ok, 0U, 0);
}

void priv_emu_io_txn_abort(emu_io_txn_t* txn)
{
  if (txn == nullptr) {
    return;
  }
  if (txn->fd >= 0) {
    (void)close(txn->fd);
    txn->fd = -1;
  }
  if (txn->temp[0] != '\0') {
    (void)unlink(txn->temp);
    txn->temp[0] = '\0';
  }
}
