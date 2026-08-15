/**
 * @file ra8_test_output.h
 * @brief Caller-owned bounded output sink for host tests and benchmarks
 * @details Provides an injected exact-write contract, a raw POSIX descriptor
 * adapter, and allocation-free typed formatting. Every sink and adapter state
 * is supplied by the caller; there is no process-global output singleton.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <unistd.h>

#include "../../libs/ra8_core/inc/ra8_attributes.h"

/** @brief Test-output bounds and rendering radices. */
typedef enum : size_t {
  k_ra8_test_output_text_cap    = 65535U, /**< Maximum accepted NUL-terminated text. */
  k_ra8_test_output_dec_cap     = 20U,    /**< Decimal digits in UINT64_MAX.         */
  k_ra8_test_output_hex_cap     = 16U,    /**< Hexadecimal digits in UINT64_MAX.     */
  k_ra8_test_output_dec_base    = 10U,    /**< Decimal rendering radix.              */
  k_ra8_test_output_hex_base    = 16U,    /**< Hexadecimal rendering radix.          */
  k_ra8_test_output_fixed_scale = 100U,   /**< Hundredths per fixed-point whole.     */
} ra8_test_output_limit_t;

/** @brief Result classes for one injected or composed output operation. */
typedef enum : uint8_t {
  k_ra8_test_output_ok = 0U, /**< The complete operation succeeded. */
  k_ra8_test_output_invalid, /**< A pointer, callback, descriptor, or bound
                                failed. */
  k_ra8_test_output_short,   /**< A sink made no progress before completion.   */
  k_ra8_test_output_error,   /**< The destination reported an operating error. */
} ra8_test_output_status_t;

/** @brief Result returned by one low-level sink callback. */
typedef struct {
  ra8_test_output_status_t status;   /**< Callback completion class. */
  size_t                   accepted; /**< Source bytes consumed.     */
  int                      os_error; /**< Host error, or zero.       */
} ra8_test_output_result_t;

/**
 * @brief Consume at most one source span.
 * @param[in,out] context Caller-owned sink context.
 * @param[in] bytes Readable source bytes.
 * @param[in] length Requested source length.
 * @return Callback result and exact accepted-byte count.
 * @pre @p context is the context bound with ::internal_test_output_init.
 * @pre @p bytes spans @p length readable bytes when length is nonzero.
 * @post `accepted` never exceeds @p length.
 * @post Success with a short positive count may be retried by the caller.
 * @since 0.1.0
 */
typedef ra8_test_output_result_t (*ra8_test_output_write_fn_t)(void*          context,
                                                               const uint8_t* bytes,
                                                               size_t         length);

/** @brief Caller-owned injected output handle with a first-error latch. */
typedef struct {
  ra8_test_output_write_fn_t write;    /**< Bound sink callback.       */
  void*                      context;  /**< Caller-owned sink context. */
  ra8_test_output_status_t   status;   /**< First composed failure.    */
  int                        os_error; /**< First host error, or zero. */
} ra8_test_output_t;

/** @brief Caller-owned raw-descriptor adapter state. */
typedef struct {
  int descriptor; /**< Borrowed open descriptor. */
} ra8_test_output_fd_t;

/**
 * @brief Bind an injected callback and clear the first-error latch.
 * @details Initializes one caller-owned handle without allocating storage or
 * installing any process-global destination.
 * @param[out] output Handle to initialize.
 * @param[in] write Sink callback.
 * @param[in,out] context Caller-owned callback context.
 * @return Whether the handle was initialized.
 * @retval true All arguments were valid.
 * @retval false A required pointer was null.
 * @pre @p output points to writable storage.
 * @pre @p context remains alive while the bound handle is used.
 * @post Success binds exactly @p write and @p context.
 * @post Success clears the status and host-error latches.
 * @note Ownership of @p context remains with the caller.
 * @since 0.1.0
 */
RA8_INTERNAL static inline bool internal_test_output_init(ra8_test_output_t*         output,
                                                          ra8_test_output_write_fn_t write,
                                                          void*                      context)
{
  if ((output == nullptr) || (write == nullptr) || (context == nullptr)) {
    return false;
  }
  output->write    = write;
  output->context  = context;
  output->status   = k_ra8_test_output_ok;
  output->os_error = 0;
  return true;
}

/**
 * @brief Perform one raw-descriptor write, retrying interrupted system calls.
 * @details Adapts one POSIX `write()` result to the injected sink contract;
 * positive short writes are intentionally returned to the exact-write loop.
 * @param[in,out] context Bound ::ra8_test_output_fd_t.
 * @param[in] bytes Readable source bytes.
 * @param[in] length Requested byte count.
 * @return Accepted count or exact descriptor failure.
 * @retval k_ra8_test_output_ok In `status` after positive progress or a zero request.
 * @retval k_ra8_test_output_error In `status` after a host write error.
 * @pre @p context and @p bytes are non-null for a nonzero request.
 * @pre The descriptor stored in @p context remains borrowed and open.
 * @post Success reports the positive count returned by `write()`.
 * @post Failure preserves the host `errno` value.
 * @note This callback never closes or duplicates the descriptor.
 * @since 0.1.0
 */
RA8_INTERNAL static inline ra8_test_output_result_t
internal_test_output_fd_write(void* context, const uint8_t* bytes, size_t length)
{
  ra8_test_output_result_t result = {};
  if ((context == nullptr) || ((bytes == nullptr) && (length != 0U))) {
    result.status = k_ra8_test_output_invalid;
    return result;
  }
  const ra8_test_output_fd_t* state = (const ra8_test_output_fd_t*)context;
  if (state->descriptor < 0) {
    result.status = k_ra8_test_output_invalid;
    return result;
  }
  if (length == 0U) {
    result.status = k_ra8_test_output_ok;
    return result;
  }
  ssize_t accepted = -1;
  do {
    accepted = write(state->descriptor, bytes, length);
  } while ((accepted < 0) && (errno == EINTR));
  if (accepted < 0) {
    result.status   = k_ra8_test_output_error;
    result.os_error = errno;
    return result;
  }
  if (accepted == 0) {
    result.status = k_ra8_test_output_short;
    return result;
  }
  result.status   = k_ra8_test_output_ok;
  result.accepted = (size_t)accepted;
  return result;
}

/**
 * @brief Bind one borrowed raw descriptor into caller-owned adapter state.
 * @details Initializes both the descriptor context and injected output handle
 * while leaving descriptor lifetime under caller control.
 * @param[out] output Handle to initialize.
 * @param[out] state Descriptor adapter state to initialize.
 * @param[in] descriptor Borrowed descriptor; ownership remains with the caller.
 * @return Whether the descriptor and output pointers were valid.
 * @retval true The descriptor adapter is ready.
 * @retval false A pointer was null or the descriptor was negative.
 * @pre @p output and @p state point to writable storage.
 * @pre @p descriptor designates a descriptor the caller may write later.
 * @post Success never closes or duplicates @p descriptor.
 * @post No global output state is changed.
 * @note A successful bind performs no I/O.
 * @since 0.1.0
 */
RA8_INTERNAL static inline bool
internal_test_output_fd_init(ra8_test_output_t* output, ra8_test_output_fd_t* state, int descriptor)
{
  if ((output == nullptr) || (state == nullptr) || (descriptor < 0)) {
    return false;
  }
  state->descriptor = descriptor;
  return internal_test_output_init(output, internal_test_output_fd_write, state);
}

/**
 * @brief Latch one failure without replacing an earlier failure.
 * @details Implements the composed sink's first-error rule so later fragments
 * cannot conceal the operation that first made the output incomplete.
 * @param[in,out] output Bound output handle.
 * @param[in] status Failure class.
 * @param[in] os_error Optional host error.
 * @return The handle's first failure.
 * @retval k_ra8_test_output_ok No failure has been supplied or latched.
 * @retval k_ra8_test_output_invalid The handle pointer was null.
 * @pre @p output is non-null.
 * @pre @p status is a valid ::ra8_test_output_status_t value.
 * @post The first non-success status is retained.
 * @post A later fragment cannot erase the first host error.
 * @note A zero @p os_error is valid for contract failures without host errors.
 * @since 0.1.0
 */
RA8_INTERNAL static inline ra8_test_output_status_t
internal_test_output_latch(ra8_test_output_t* output, ra8_test_output_status_t status, int os_error)
{
  if (output == nullptr) {
    return k_ra8_test_output_invalid;
  }
  if ((output->status == k_ra8_test_output_ok) && (status != k_ra8_test_output_ok)) {
    output->status   = status;
    output->os_error = os_error;
  }
  return output->status;
}

/**
 * @brief Write a complete span through the injected sink.
 * @details Repeats positive short writes over the remaining source span and
 * stops at the first callback contract violation or destination error.
 * @param[in,out] output Bound output handle.
 * @param[in] bytes Readable source span.
 * @param[in] length Exact source length.
 * @return Success or the first latched failure.
 * @retval k_ra8_test_output_ok Every requested byte was accepted.
 * @retval k_ra8_test_output_short A callback made no progress.
 * @pre @p output is initialized and @p bytes spans @p length bytes.
 * @pre The callback context remains valid for the complete operation.
 * @post Positive short writes are retried until complete.
 * @post Zero progress, impossible counts, and sink errors are latched.
 * @note A previously failed handle performs no further callback calls.
 * @since 0.1.0
 */
RA8_INTERNAL static inline ra8_test_output_status_t
internal_test_output_write_all(ra8_test_output_t* output, const uint8_t* bytes, size_t length)
{
  if ((output == nullptr) || (output->write == nullptr) || ((bytes == nullptr) && (length != 0U))) {
    return internal_test_output_latch(output, k_ra8_test_output_invalid, 0);
  }
  if (output->status != k_ra8_test_output_ok) {
    return output->status;
  }
  size_t offset = 0U;
  while (offset < length) {
    const size_t                   remaining = length - offset;
    const ra8_test_output_result_t result =
      output->write(output->context, &bytes[offset], remaining);
    if (result.accepted > remaining) {
      return internal_test_output_latch(output, k_ra8_test_output_invalid, result.os_error);
    }
    if (result.status != k_ra8_test_output_ok) {
      return internal_test_output_latch(output, result.status, result.os_error);
    }
    if (result.accepted == 0U) {
      return internal_test_output_latch(output, k_ra8_test_output_short, result.os_error);
    }
    offset += result.accepted;
  }
  return k_ra8_test_output_ok;
}

/**
 * @brief Append bounded NUL-terminated text without its terminator.
 * @details Scans no farther than the public text cap before delegating the
 * exact non-terminator span to ::internal_test_output_write_all.
 * @param[in,out] output Bound output handle.
 * @param[in] text NUL-terminated text.
 * @return Success or the first latched failure.
 * @retval k_ra8_test_output_ok The complete text was accepted.
 * @retval k_ra8_test_output_invalid The text pointer or termination bound failed.
 * @pre @p text terminates within ::k_ra8_test_output_text_cap bytes.
 * @pre @p output is initialized and its context remains alive.
 * @post Success appends the complete text and no terminator.
 * @post An unterminated span is rejected without an out-of-bound read.
 * @note The terminating NUL is never written to the destination.
 * @since 0.1.0
 */
RA8_INTERNAL static inline ra8_test_output_status_t
internal_test_output_text(ra8_test_output_t* output, const char* text)
{
  if (text == nullptr) {
    return internal_test_output_latch(output, k_ra8_test_output_invalid, 0);
  }
  size_t length = 0U;
  while (length < (size_t)k_ra8_test_output_text_cap) {
    if (text[length] == '\0') {
      return internal_test_output_write_all(output, (const uint8_t*)text, length);
    }
    ++length;
  }
  return internal_test_output_latch(output, k_ra8_test_output_invalid, 0);
}

/**
 * @brief Write one text fragment through caller-local raw-descriptor state.
 * @details Convenience composition edge for one fragment; it constructs the
 * descriptor adapter on the stack and delegates to the regular text writer.
 * @param[in] descriptor Borrowed descriptor.
 * @param[in] text NUL-terminated text.
 * @return Complete-write status.
 * @retval k_ra8_test_output_ok The complete text was accepted.
 * @retval k_ra8_test_output_invalid A pointer, descriptor, or bound failed.
 * @pre @p descriptor is nonnegative and @p text is NUL-terminated.
 * @pre The caller keeps @p descriptor open for the complete call.
 * @post Success appends the complete text without its terminator.
 * @post No descriptor is closed and no global sink is installed.
 * @note Use a persistent handle when several fragments form one atomic contract.
 * @since 0.1.0
 */
RA8_INTERNAL static inline ra8_test_output_status_t internal_test_output_fd_text(int descriptor,
                                                                                 const char* text)
{
  ra8_test_output_t    output = {};
  ra8_test_output_fd_t state  = {};
  if (!internal_test_output_fd_init(&output, &state, descriptor)) {
    return k_ra8_test_output_invalid;
  }
  return internal_test_output_text(&output, text);
}

/**
 * @brief Append an unsigned decimal value without padding.
 * @details Builds the digits in reverse order in a bounded local array, then
 * reverses them into the exact span supplied to the sink.
 * @param[in,out] output Bound output handle.
 * @param[in] value Value to render.
 * @return Success or the first latched failure.
 * @retval k_ra8_test_output_ok Every rendered digit was accepted.
 * @retval k_ra8_test_output_error The destination rejected a digit span.
 * @pre @p output is initialized.
 * @pre The bound callback context remains alive during rendering.
 * @post Success appends one to twenty decimal digits.
 * @post No dynamic storage is used.
 * @note Zero is rendered as one digit rather than an empty span.
 * @since 0.1.0
 */
RA8_INTERNAL static inline ra8_test_output_status_t
internal_test_output_u64(ra8_test_output_t* output, uint64_t value)
{
  uint8_t  reversed[k_ra8_test_output_dec_cap];
  size_t   digits    = 0U;
  uint64_t remaining = value;
  do {
    reversed[digits] = (uint8_t)('0' + (remaining % (uint64_t)k_ra8_test_output_dec_base));
    remaining /= (uint64_t)k_ra8_test_output_dec_base;
    ++digits;
  } while (remaining != 0U);
  uint8_t rendered[k_ra8_test_output_dec_cap];
  for (size_t index = 0U; index < digits; ++index) {
    rendered[index] = reversed[digits - 1U - index];
  }
  return internal_test_output_write_all(output, rendered, digits);
}

/**
 * @brief Append a signed decimal value without padding.
 * @details Emits a sign when required and converts the magnitude using an
 * overflow-safe INT64_MIN path before delegating to the unsigned renderer.
 * @param[in,out] output Bound output handle.
 * @param[in] value Value to render.
 * @return Success or the first latched failure.
 * @retval k_ra8_test_output_ok The complete signed value was accepted.
 * @retval k_ra8_test_output_error The destination rejected a fragment.
 * @pre @p output is initialized.
 * @pre The bound callback context remains alive during rendering.
 * @post Success emits a minus sign exactly when @p value is negative.
 * @post INT64_MIN is converted without signed overflow.
 * @note Positive values never receive an explicit plus sign.
 * @since 0.1.0
 */
RA8_INTERNAL static inline ra8_test_output_status_t
internal_test_output_i64(ra8_test_output_t* output, int64_t value)
{
  uint64_t magnitude = (uint64_t)value;
  if (value < 0) {
    if (internal_test_output_write_all(output, (const uint8_t*)"-", 1U) != k_ra8_test_output_ok) {
      return output->status;
    }
    magnitude = (uint64_t)(-(value + 1)) + 1U;
  }
  return internal_test_output_u64(output, magnitude);
}

/**
 * @brief Append an unsigned hexadecimal value with bounded zero padding.
 * @details Converts through bounded reverse and forward local arrays so both
 * minimum-width padding and alphabet case are explicit.
 * @param[in,out] output Bound output handle.
 * @param[in] value Value to render.
 * @param[in] min_digits Minimum output width from one through sixteen.
 * @param[in] uppercase Whether alphabetic digits use upper case.
 * @return Success or the first latched failure.
 * @retval k_ra8_test_output_ok The complete hexadecimal value was accepted.
 * @retval k_ra8_test_output_invalid The requested width was outside its contract.
 * @pre @p min_digits is in the documented range.
 * @pre @p output is initialized and its context remains alive.
 * @post Success appends at most sixteen hexadecimal digits.
 * @post No prefix is added.
 * @note Width is a minimum; significant leading digits are never discarded.
 * @since 0.1.0
 */
RA8_INTERNAL static inline ra8_test_output_status_t
internal_test_output_hex64(ra8_test_output_t* output,
                           uint64_t           value,
                           uint8_t            min_digits,
                           bool               uppercase)
{
  if ((min_digits == 0U) || (min_digits > (uint8_t)k_ra8_test_output_hex_cap)) {
    return internal_test_output_latch(output, k_ra8_test_output_invalid, 0);
  }
  const char* digits = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";
  uint8_t     reversed[k_ra8_test_output_hex_cap];
  size_t      count     = 0U;
  uint64_t    remaining = value;
  do {
    reversed[count] = (uint8_t)digits[remaining % (uint64_t)k_ra8_test_output_hex_base];
    remaining /= (uint64_t)k_ra8_test_output_hex_base;
    ++count;
  } while (remaining != 0U);
  while (count < (size_t)min_digits) {
    reversed[count] = (uint8_t)'0';
    ++count;
  }
  uint8_t rendered[k_ra8_test_output_hex_cap];
  for (size_t index = 0U; index < count; ++index) {
    rendered[index] = reversed[count - 1U - index];
  }
  return internal_test_output_write_all(output, rendered, count);
}

/**
 * @brief Append a nonnegative hundredths value with exactly two decimals.
 * @details Renders the whole component with the unsigned converter and writes
 * exactly two fractional digits from the fixed-point remainder.
 * @param[in,out] output Bound output handle.
 * @param[in] hundredths Fixed-point value scaled by one hundred.
 * @return Success or the first latched failure.
 * @retval k_ra8_test_output_ok The complete fixed-point value was accepted.
 * @retval k_ra8_test_output_error The destination rejected a fragment.
 * @pre @p output is initialized.
 * @pre @p hundredths already contains the caller's desired rounding result.
 * @post Success emits `<whole>.<two digits>`.
 * @post The conversion uses no floating-point formatter.
 * @note This function does not perform rounding; it only renders hundredths.
 * @since 0.1.0
 */
RA8_INTERNAL static inline ra8_test_output_status_t
internal_test_output_fixed2(ra8_test_output_t* output, uint64_t hundredths)
{
  ra8_test_output_status_t status =
    internal_test_output_u64(output, hundredths / (uint64_t)k_ra8_test_output_fixed_scale);
  if (status == k_ra8_test_output_ok) {
    status = internal_test_output_write_all(output, (const uint8_t*)".", 1U);
  }
  if (status == k_ra8_test_output_ok) {
    const uint8_t fraction[2] = {
      (uint8_t)('0' + ((hundredths / (uint64_t)k_ra8_test_output_dec_base) %
                       (uint64_t)k_ra8_test_output_dec_base)),
      (uint8_t)('0' + (hundredths % (uint64_t)k_ra8_test_output_dec_base)),
    };
    status = internal_test_output_write_all(output, fraction, sizeof(fraction));
  }
  return status;
}
