/**
 * @file ra8_viewer_output.c
 * @brief Typed allocation-free diagnostic writers for the host viewer.
 * @details Composes the viewer's exact messages from bounded stream primitives,
 * propagating the first short or failed write without varargs or hosted I/O.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_io_stream.h"
#include "ra8_viewer_output_internal.h"

/**
 * @brief Append text only while an output sequence remains successful.
 * @details Preserves a preceding failure without touching the destination;
 * otherwise delegates the fragment to the portable stream facade.
 * @param[in,out] output Bound byte stream.
 * @param[in] current Status from the preceding fragment.
 * @param[in] text NUL-terminated fragment.
 * @return The first failure or the current fragment status.
 * @retval k_ra8_ok The fragment was appended completely.
 * @retval other A preceding or current stream operation failed.
 * @pre @p output is bound and @p text is NUL-terminated.
 * @pre @p current is a canonical stream status.
 * @post No fragment after a failure is attempted.
 * @post Success appends the complete text without its NUL byte.
 * @note Not thread-safe for concurrent use of one stream.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_put_text(ra8_io_stream_t* output, ra8_err_t current, const char* text)
{
  return (current == k_ra8_ok) ? ra8_io_stream_puts(output, text) : current;
}

/**
 * @brief Append a decimal 32-bit value while a sequence remains successful.
 * @details Preserves a preceding failure or delegates one bounded decimal
 * rendering to the portable stream facade.
 * @param[in,out] output Bound byte stream.
 * @param[in] current Status from the preceding fragment.
 * @param[in] value Value to append.
 * @return The first failure or the current fragment status.
 * @retval k_ra8_ok The complete decimal value was appended.
 * @retval other A preceding or current stream operation failed.
 * @pre @p output is bound.
 * @pre @p current is a canonical stream status.
 * @post No fragment after a failure is attempted.
 * @post Success appends the value without padding.
 * @note Not thread-safe for concurrent use of one stream.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_put_u32(ra8_io_stream_t* output, ra8_err_t current, uint32_t value)
{
  return (current == k_ra8_ok) ? ra8_io_stream_put_u32(output, value) : current;
}

/**
 * @brief Append a decimal 64-bit value while a sequence remains successful.
 * @details Preserves a preceding failure or delegates one bounded decimal
 * rendering to the portable stream facade.
 * @param[in,out] output Bound byte stream.
 * @param[in] current Status from the preceding fragment.
 * @param[in] value Value to append.
 * @return The first failure or the current fragment status.
 * @retval k_ra8_ok The complete decimal value was appended.
 * @retval other A preceding or current stream operation failed.
 * @pre @p output is bound.
 * @pre @p current is a canonical stream status.
 * @post No fragment after a failure is attempted.
 * @post Success appends the value without padding.
 * @note Not thread-safe for concurrent use of one stream.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_put_u64(ra8_io_stream_t* output, ra8_err_t current, uint64_t value)
{
  return (current == k_ra8_ok) ? ra8_io_stream_put_u64(output, value) : current;
}

/**
 * @brief Append an unpadded hexadecimal value while a sequence remains
 * successful.
 * @details Preserves a preceding failure or delegates one bounded lowercase
 * hexadecimal rendering to the portable stream facade.
 * @param[in,out] output Bound byte stream.
 * @param[in] current Status from the preceding fragment.
 * @param[in] value Value to append.
 * @return The first failure or the current fragment status.
 * @retval k_ra8_ok The complete hexadecimal value was appended.
 * @retval other A preceding or current stream operation failed.
 * @pre @p output is bound.
 * @pre @p current is a canonical stream status.
 * @post No fragment after a failure is attempted.
 * @post Success appends at least one lowercase hexadecimal digit.
 * @note Not thread-safe for concurrent use of one stream.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_put_hex(ra8_io_stream_t* output, ra8_err_t current, uint32_t value)
{
  return (current == k_ra8_ok) ? ra8_io_stream_put_hex(output, value, 1U) : current;
}

ra8_err_t priv_viewer_output_usage(ra8_io_stream_t* output, const char* executable)
{
  ra8_err_t error = ra8_io_stream_puts(output, "usage: ");
  error           = internal_put_text(output, error, executable);
  error           = internal_put_text(output,
                                      error,
                                      " <file.jof> [--headless]\n"
                                      "         [--dump-ppm PATH [--page N | --dump-tile N]]\n"
                                      "  window: resizable, fit-to-width, continuous scroll "
                                      "(wheel/trackpad/PageUp-Dn/Home/End)\n");
  return error;
}

ra8_err_t priv_viewer_output_capacity(ra8_io_stream_t* output,
                                      const char*      subject,
                                      size_t           required,
                                      size_t           supplied)
{
  ra8_err_t error = ra8_io_stream_puts(output, subject);
  error           = internal_put_text(output, error, " workspace requires ");
  error           = internal_put_u64(output, error, (uint64_t)required);
  error           = internal_put_text(output, error, " bytes, supplied ");
  error           = internal_put_u64(output, error, (uint64_t)supplied);
  return internal_put_text(output, error, " bytes\n");
}

ra8_err_t priv_viewer_output_index_error(ra8_io_stream_t* output,
                                         const char*      operation,
                                         uint32_t         index,
                                         ra8_err_t        error_code)
{
  ra8_err_t error = ra8_io_stream_puts(output, operation);
  error           = internal_put_u32(output, error, index);
  error           = internal_put_text(output, error, " failed: 0x");
  error           = internal_put_hex(output, error, (uint32_t)error_code);
  return internal_put_text(output, error, "\n");
}

ra8_err_t
priv_viewer_output_error(ra8_io_stream_t* output, const char* operation, ra8_err_t error_code)
{
  ra8_err_t error = ra8_io_stream_puts(output, operation);
  error           = internal_put_text(output, error, " failed: 0x");
  error           = internal_put_hex(output, error, (uint32_t)error_code);
  return internal_put_text(output, error, "\n");
}

ra8_err_t
priv_viewer_output_open_error(ra8_io_stream_t* output, const char* path, ra8_err_t error_code)
{
  ra8_err_t error = ra8_io_stream_puts(output, "open '");
  error           = internal_put_text(output, error, path);
  error           = internal_put_text(output, error, "' failed: 0x");
  error           = internal_put_hex(output, error, (uint32_t)error_code);
  return internal_put_text(output, error, "\n");
}

ra8_err_t priv_viewer_output_opened(ra8_io_stream_t* output, const char* path, uint32_t page_count)
{
  ra8_err_t error = ra8_io_stream_puts(output, "opened '");
  error           = internal_put_text(output, error, path);
  error           = internal_put_text(output, error, "': ");
  error           = internal_put_u32(output, error, page_count);
  return internal_put_text(output, error, " page(s)\n");
}

ra8_err_t priv_viewer_output_wrote(ra8_io_stream_t* output, const char* path)
{
  ra8_err_t error = ra8_io_stream_puts(output, "wrote ");
  error           = internal_put_text(output, error, path);
  return internal_put_text(output, error, "\n");
}

ra8_err_t priv_viewer_output_tile(ra8_io_stream_t* output,
                                  uint32_t         tile,
                                  uint32_t         width,
                                  uint32_t         height,
                                  const char*      path)
{
  ra8_err_t error = ra8_io_stream_puts(output, "wrote tile ");
  error           = internal_put_u32(output, error, tile);
  error           = internal_put_text(output, error, " (");
  error           = internal_put_u32(output, error, width);
  error           = internal_put_text(output, error, "x");
  error           = internal_put_u32(output, error, height);
  error           = internal_put_text(output, error, ") -> ");
  error           = internal_put_text(output, error, path);
  return internal_put_text(output, error, "\n");
}

ra8_err_t priv_viewer_output_text(ra8_io_stream_t* output, const char* text)
{
  return ra8_io_stream_puts(output, text);
}
