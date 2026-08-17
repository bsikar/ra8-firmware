/**
 * @file ra8_io_stream_posix.c
 * @brief Raw-descriptor implementation of the portable byte-stream backend.
 * @ingroup grp_io
 *
 * @par Tag
 * [Ring 4 / Host Port] {World: Host}
 *
 * @details
 * Performs exact bounded writes through `write(2)`, retries a finite number of
 * interruptions, detects impossible kernel progress, and maps host failures to
 * ::ra8_err_t. The descriptor is borrowed and no C-runtime stream is created.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_io_stream_posix.h"

#include <errno.h>
#include <stdint.h>
#include <unistd.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_io_stream_backend.h"
#include "ra8_io_stream_posix_internal.h"

/** @brief POSIX descriptor-write retry limits. */
typedef enum : uint32_t {
  k_posix_write_interrupt_limit = 16U, /**< Maximum interrupted attempts. */
} ra8_io_stream_posix_const_t;

/**
 * @brief Map one descriptor-write errno value to the canonical vocabulary.
 *
 * @details
 * Converts only the captured error value supplied by the write loop. It does
 * not inspect or modify the process-global `errno` object.
 *
 * @param[in] error_number Captured non-zero errno value.
 *
 * @return Canonical error corresponding to @p error_number.
 * @retval k_ra8_err_protocol_error @p error_number was zero.
 * @retval k_ra8_fail No more specific canonical mapping exists.
 *
 * @pre @p error_number is the value captured for one failed write attempt.
 * @pre The caller does not require preservation of host-specific distinctions.
 * @post The returned value depends only on @p error_number.
 * @post No global error state or descriptor state is modified.
 *
 * @note Multiple host errno values may intentionally map to one portable code.
 *
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_map_errno(int error_number)
{
  if (error_number == 0) {
    return k_ra8_err_protocol_error;
  }
  if (error_number == EAGAIN
#if defined(EWOULDBLOCK) && (EWOULDBLOCK != EAGAIN)
      || error_number == EWOULDBLOCK
#endif
  ) {
    return k_ra8_err_would_block;
  }
  if (error_number == EBADF) {
    return k_ra8_err_invalid_state;
  }
  if (error_number == ENOSPC || error_number == EDQUOT) {
    return k_ra8_err_no_mem;
  }
  if (error_number == EACCES || error_number == EPERM) {
    return k_ra8_err_access_denied;
  }
  if (error_number == EFBIG || error_number == EOVERFLOW) {
    return k_ra8_err_invalid_size;
  }
  if (error_number == EINVAL) {
    return k_ra8_err_invalid_arg;
  }
  if (error_number == EPIPE || error_number == EIO) {
    return k_ra8_err_comm_error;
  }
  return k_ra8_fail;
}

/**
 * @brief Perform one write attempt within the bounded retry loop.
 * @details Issues one write, retries in place on EINTR up to the bounded
 * interrupt limit, and maps every other negative result through
 * ::internal_map_errno. A zero or over-reported result is rejected as a
 * protocol violation.
 * @param[in] fd Open POSIX descriptor.
 * @param[in] bytes Source buffer.
 * @param[in] length Total requested byte count.
 * @param[in,out] done Running written-byte count; unchanged on a retry.
 * @param[in,out] interrupts Running EINTR retry count.
 * @param[in] writer Injected native write primitive.
 * @param[in,out] writer_context Writer-specific context.
 * @return Write status.
 * @retval k_ra8_ok The write advanced @p done, or an in-limit EINTR retry
 * left it unchanged.
 * @retval k_ra8_err_retry_limit The bounded EINTR retry count was exceeded.
 * @retval k_ra8_err_protocol_error The writer reported zero or excess bytes.
 * @retval other The mapped native write error.
 * @pre @p done is strictly less than @p length on entry.
 * @pre @p bytes holds at least @p length readable bytes and @p writer is non-NULL.
 * @post On success without a retry, @p done advances by the bytes written.
 * @post On any error return @p done is unchanged, so the caller cannot skip bytes.
 * @note Not thread-safe: mutates caller-owned scratch state.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_write_loop_step(int                            fd,
                                                       const uint8_t*                 bytes,
                                                       uint32_t                       length,
                                                       uint32_t*                      done,
                                                       uint32_t*                      interrupts,
                                                       ra8_io_stream_posix_write_fn_t writer,
                                                       void* writer_context)
{
  int           error_number = 0;
  const int64_t result = writer(writer_context, fd, &bytes[*done], length - *done, &error_number);
  if (result < 0) {
    if (error_number == EINTR) {
      ++*interrupts;
      if (*interrupts > (uint32_t)k_posix_write_interrupt_limit) {
        return k_ra8_err_retry_limit;
      }
      return k_ra8_ok;
    }
    return internal_map_errno(error_number);
  }
  if ((result == 0) || ((uint64_t)result > (uint64_t)(length - *done))) {
    return k_ra8_err_protocol_error;
  }
  *done += (uint32_t)result;
  return k_ra8_ok;
}

RA8_PRIV ra8_err_t priv_ra8_io_stream_posix_write_loop(int                            fd,
                                                       const uint8_t*                 bytes,
                                                       uint32_t                       length,
                                                       uint32_t*                      out_written,
                                                       ra8_io_stream_posix_write_fn_t writer,
                                                       void* writer_context)
{
  if ((bytes == nullptr) || (out_written == nullptr) || (writer == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  *out_written        = 0U;
  uint32_t done       = 0U;
  uint32_t interrupts = 0U;
  RA8_LOOP_BOUND(UINT32_MAX);
  while (done < length) {
    const ra8_err_t err =
      internal_write_loop_step(fd, bytes, length, &done, &interrupts, writer, writer_context);
    if (err != k_ra8_ok) {
      *out_written = done;
      return err;
    }
  }
  *out_written = done;
  return k_ra8_ok;
}

/**
 * @brief Invoke the native POSIX descriptor-write primitive once.
 *
 * @details
 * Calls `write(2)` exactly once and captures `errno` immediately when the
 * syscall reports failure. Retry and exact-transfer policy belongs to the
 * surrounding injected write loop.
 *
 * @param[in] context Unused native-writer context.
 * @param[in] fd Writable descriptor.
 * @param[in] bytes Source bytes.
 * @param[in] length Maximum bytes for this attempt.
 * @param[out] out_errno Captured errno on failure, otherwise zero.
 * @return Native byte count or negative failure result.
 * @retval -1 The native descriptor write failed and @p out_errno was set.
 *
 * @pre @p bytes spans @p length readable bytes and @p out_errno is writable.
 * @pre @p fd designates a descriptor acceptable to `write(2)`.
 * @post A non-negative result never exceeds @p length.
 * @post @p out_errno is zero on success and the captured errno on failure.
 *
 * @note The caller owns bounded interruption retries.
 *
 * @since 0.1.0
 */
RA8_INTERNAL static int64_t
internal_native_write(void* context, int fd, const uint8_t* bytes, uint32_t length, int* out_errno)
{
  (void)context;
  const ssize_t result = write(fd, bytes, (size_t)length);
  *out_errno           = (result < 0) ? errno : 0;
  return (int64_t)result;
}

/**
 * @brief Stream-backend callback for one borrowed POSIX descriptor.
 *
 * @details
 * Resolves the bound descriptor from @p context and delegates the entire exact
 * transfer to ::priv_ra8_io_stream_posix_write_loop.
 *
 * @param[in] context Bound ::ra8_io_stream_posix_state_t.
 * @param[in] bytes Source bytes.
 * @param[in] length Requested byte count.
 * @param[out] out_written Proven accepted byte count.
 * @return Canonical exact-write status.
 * @retval k_ra8_ok Every requested byte was accepted.
 *
 * @pre All pointers are non-null and @p bytes spans @p length bytes.
 * @pre @p context points to an initialized ::ra8_io_stream_posix_state_t.
 * @post Success means every requested byte was accepted.
 * @post @p out_written contains the proven accepted prefix on every return.
 *
 * @note A caller that wants `EPIPE` returned must suppress the host's default
 *       `SIGPIPE` disposition at its composition boundary.
 *
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_posix_write(void* context, const uint8_t* bytes, uint32_t length, uint32_t* out_written)
{
  const ra8_io_stream_posix_state_t* state = (const ra8_io_stream_posix_state_t*)context;
  return priv_ra8_io_stream_posix_write_loop(state->fd,
                                             bytes,
                                             length,
                                             out_written,
                                             internal_native_write,
                                             nullptr);
}

/** @brief Immutable borrowed-descriptor stream backend. */
static const ra8_io_stream_iface_t s_posix_stream_iface = {
  .write = internal_posix_write,
  .flush = nullptr,
};

ra8_err_t
ra8_io_stream_posix_init(ra8_io_stream_t* stream, ra8_io_stream_posix_state_t* state, int fd)
{
  if ((stream == nullptr) || (state == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  if (fd < 0) {
    return k_ra8_err_invalid_arg;
  }
  ra8_io_stream_posix_state_t candidate = {.fd = fd};
  const ra8_err_t             error     = ra8_io_stream_bind(stream, &s_posix_stream_iface, state);
  if (error != k_ra8_ok) {
    return error;
  }
  *state = candidate;
  return k_ra8_ok;
}
