/**
 * @file ra_io_stream.c
 * @brief Byte-stream dispatcher + no-varargs formatted-output primitives.
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * Validates the stream handle then forwards through the bound sink vtable. The
 * formatted helpers (`puts` / `put_u32` / `put_hex`) render into a small bounded
 * stack buffer and call ::ra_io_stream_write -- no varargs, no allocation, so
 * the `_sbrk` trap and the bounded-stack budget stay intact.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_io_stream.h"

#include <stddef.h>
#include <stdint.h>

#include "ra_check.h"
#include "ra_err.h"
#include "ra_io_stream_internal.h"

/** @brief Module log tag. */
static const char* const s_tag = "ra_io_stream";

/**
 * @enum ra_io_stream_const_t
 * @brief Rendering constants for the formatted-output helpers.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ra_io_dec_base       = 10,    /**< Base for ::ra_io_stream_put_u32.            */
  k_ra_io_hex_base       = 16,    /**< Base for ::ra_io_stream_put_hex.            */
  k_ra_io_u32_max_digits = 10,    /**< Decimal digits in UINT32_MAX.               */
  k_ra_io_hex_max_digits = 8,     /**< Hex digits in a 32-bit value.               */
  k_ra_io_puts_max       = 65535, /**< Bounded scan limit for ::ra_io_stream_puts. */
} ra_io_stream_const_t;

/**
 * @brief Reject a handle that is NULL or has no sink bound.
 *
 * @details
 * Run on every entry point. Kept tiny so each public function stays under the
 * NASA Power-of-10 Rule 4 sixty-line cap.
 *
 * @param[in] s Candidate handle.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok                  `s` is non-NULL with a bound sink.
 * @retval k_ra_err_null_ptr        `s` was NULL.
 * @retval k_ra_err_not_initialized `s->iface` was NULL (never bound).
 *
 * @pre None.
 * @pre None.
 * @post No state is mutated.
 * @post The return reflects only the binding state of `s`.
 *
 * @note Thread-safe.
 *
 * @since 0.1.0
 */
static ra_err_t internal_validate(const ra_io_stream_t* s)
{
  if (s == nullptr) {
    return k_ra_err_null_ptr;
  }
  if (s->iface == nullptr) {
    return k_ra_err_not_initialized;
  }
  return k_ra_ok;
}

ra_err_t
ra_io_stream_write(ra_io_stream_t* s, const uint8_t* buf, uint32_t len, uint32_t* out_written)
{
  const ra_err_t v = internal_validate(s);
  if (v != k_ra_ok) {
    return v;
  }
  RA_CHECK_NULL_PTR(buf, s_tag, "buf must not be nullptr");
  RA_CHECK_NULL_PTR(s->iface->write, s_tag, "sink write op missing");
  return s->iface->write(s->ctx, buf, len, out_written);
}

ra_err_t ra_io_stream_flush(ra_io_stream_t* s)
{
  const ra_err_t v = internal_validate(s);
  if (v != k_ra_ok) {
    return v;
  }
  if (s->iface->flush == nullptr) {
    return k_ra_ok;
  }
  return s->iface->flush(s->ctx);
}

ra_err_t ra_io_stream_putc(ra_io_stream_t* s, char c)
{
  const ra_err_t v = internal_validate(s);
  if (v != k_ra_ok) {
    return v;
  }
  const uint8_t b = (uint8_t)c;
  return ra_io_stream_write(s, &b, 1U, nullptr);
}

ra_err_t ra_io_stream_puts(ra_io_stream_t* s, const char* str)
{
  const ra_err_t v = internal_validate(s);
  if (v != k_ra_ok) {
    return v;
  }
  RA_CHECK_NULL_PTR(str, s_tag, "str must not be nullptr");
  uint32_t len = 0;
  while (len < (uint32_t)k_ra_io_puts_max) {
    if (str[len] == '\0') {
      break;
    }
    ++len;
  }
  return ra_io_stream_write(s, (const uint8_t*)str, len, nullptr);
}

ra_err_t ra_io_stream_put_u32(ra_io_stream_t* s, uint32_t value)
{
  const ra_err_t v = internal_validate(s);
  if (v != k_ra_ok) {
    return v;
  }
  uint8_t  tmp[k_ra_io_u32_max_digits];
  uint32_t i = 0;
  uint32_t x = value;
  do {
    tmp[i] = (uint8_t)('0' + (x % (uint32_t)k_ra_io_dec_base));
    x /= (uint32_t)k_ra_io_dec_base;
    ++i;
  } while (x != 0U);
  uint8_t out[k_ra_io_u32_max_digits];
  for (uint32_t j = 0; j < i; ++j) {
    out[j] = tmp[i - 1U - j];
  }
  return ra_io_stream_write(s, out, i, nullptr);
}

ra_err_t ra_io_stream_put_hex(ra_io_stream_t* s, uint32_t value, uint8_t min_digits)
{
  const ra_err_t v = internal_validate(s);
  if (v != k_ra_ok) {
    return v;
  }
  if (min_digits == 0U) {
    return k_ra_err_invalid_arg;
  }
  if (min_digits > (uint8_t)k_ra_io_hex_max_digits) {
    return k_ra_err_invalid_arg;
  }
  static const char k_hex[] = "0123456789abcdef";
  uint8_t           tmp[k_ra_io_hex_max_digits];
  uint32_t          i = 0;
  uint32_t          x = value;
  do {
    tmp[i] = (uint8_t)k_hex[x % (uint32_t)k_ra_io_hex_base];
    x /= (uint32_t)k_ra_io_hex_base;
    ++i;
  } while (x != 0U);
  while (i < (uint32_t)min_digits) {
    tmp[i] = (uint8_t)'0';
    ++i;
  }
  uint8_t out[k_ra_io_hex_max_digits];
  for (uint32_t j = 0; j < i; ++j) {
    out[j] = tmp[i - 1U - j];
  }
  return ra_io_stream_write(s, out, i, nullptr);
}
