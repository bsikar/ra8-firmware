/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_io_stream.c
 * @brief Byte-stream dispatcher + no-varargs formatted-output primitives.
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * Validates the stream handle then forwards through the bound sink vtable. The
 * formatted helpers (`puts` / `put_u32` / `put_hex`) render into a small bounded
 * stack buffer and call ::ra8_io_stream_write -- no varargs, no allocation, so
 * the `_sbrk` trap and the bounded-stack budget stay intact.
 */

#include "ra8_io_stream.h"

#include <stddef.h>
#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_io_stream_internal.h"

/** @brief Module log tag. */
static const char* const s_tag = "ra8_io_stream";

/**
 * @enum ra8_io_stream_const_t
 * @brief Rendering constants for the formatted-output helpers.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ra8_io_dec_base       = 10,    /**< Base for ::ra8_io_stream_put_u32.            */
  k_ra8_io_hex_base       = 16,    /**< Base for ::ra8_io_stream_put_hex.            */
  k_ra8_io_u32_max_digits = 10,    /**< Decimal digits in UINT32_MAX.                */
  k_ra8_io_hex_max_digits = 8,     /**< Hex digits in a 32-bit value.                */
  k_ra8_io_puts_max       = 65535, /**< Bounded scan limit for ::ra8_io_stream_puts. */
} ra8_io_stream_const_t;

/**
 * @brief Reject a handle that is NULL or has no sink bound.
 *
 * @details
 * Run on every entry point. Kept tiny so each public function stays under the
 * NASA Power-of-10 Rule 4 sixty-line cap.
 *
 * @param[in] s Candidate handle.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                  `s` is non-NULL with a bound sink.
 * @retval k_ra8_err_null_ptr        `s` was NULL.
 * @retval k_ra8_err_not_initialized `s->iface` was NULL (never bound).
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
RA8_INTERNAL
static ra8_err_t internal_validate(const ra8_io_stream_t* s)
{
  if (s == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (s->iface == nullptr) {
    return k_ra8_err_not_initialized;
  }
  return k_ra8_ok;
}

ra8_err_t
ra8_io_stream_write(ra8_io_stream_t* s, const uint8_t* buf, uint32_t len, uint32_t* out_written)
{
  const ra8_err_t v = internal_validate(s);
  if (v != k_ra8_ok) {
    return v;
  }
  RA8_CHECK_NULL_PTR(buf, s_tag, "buf must not be nullptr");
  RA8_CHECK_NULL_PTR(s->iface->write, s_tag, "sink write op missing");
  return s->iface->write(s->ctx, buf, len, out_written);
}

ra8_err_t ra8_io_stream_flush(ra8_io_stream_t* s)
{
  const ra8_err_t v = internal_validate(s);
  if (v != k_ra8_ok) {
    return v;
  }
  if (s->iface->flush == nullptr) {
    return k_ra8_ok;
  }
  return s->iface->flush(s->ctx);
}

ra8_err_t ra8_io_stream_putc(ra8_io_stream_t* s, char c)
{
  const ra8_err_t v = internal_validate(s);
  if (v != k_ra8_ok) {
    return v;
  }
  const uint8_t b = (uint8_t)c;
  return ra8_io_stream_write(s, &b, 1U, nullptr);
}

ra8_err_t ra8_io_stream_puts(ra8_io_stream_t* s, const char* str)
{
  const ra8_err_t v = internal_validate(s);
  if (v != k_ra8_ok) {
    return v;
  }
  RA8_CHECK_NULL_PTR(str, s_tag, "str must not be nullptr");
  uint32_t len = 0;
  while (len < (uint32_t)k_ra8_io_puts_max) {
    if (str[len] == '\0') {
      break;
    }
    ++len;
  }
  return ra8_io_stream_write(s, (const uint8_t*)str, len, nullptr);
}

ra8_err_t ra8_io_stream_put_u32(ra8_io_stream_t* s, uint32_t value)
{
  const ra8_err_t v = internal_validate(s);
  if (v != k_ra8_ok) {
    return v;
  }
  uint8_t  tmp[k_ra8_io_u32_max_digits];
  uint32_t i = 0;
  uint32_t x = value;
  do {
    tmp[i] = (uint8_t)('0' + (x % (uint32_t)k_ra8_io_dec_base));
    x /= (uint32_t)k_ra8_io_dec_base;
    ++i;
  } while (x != 0U);
  uint8_t out[k_ra8_io_u32_max_digits];
  for (uint32_t j = 0; j < i; ++j) {
    out[j] = tmp[i - 1U - j];
  }
  return ra8_io_stream_write(s, out, i, nullptr);
}

ra8_err_t ra8_io_stream_put_hex(ra8_io_stream_t* s, uint32_t value, uint8_t min_digits)
{
  const ra8_err_t v = internal_validate(s);
  if (v != k_ra8_ok) {
    return v;
  }
  if (min_digits == 0U) {
    return k_ra8_err_invalid_arg;
  }
  if (min_digits > (uint8_t)k_ra8_io_hex_max_digits) {
    return k_ra8_err_invalid_arg;
  }
  static const char k_hex[] = "0123456789abcdef";
  uint8_t           tmp[k_ra8_io_hex_max_digits];
  uint32_t          i = 0;
  uint32_t          x = value;
  do {
    tmp[i] = (uint8_t)k_hex[x % (uint32_t)k_ra8_io_hex_base];
    x /= (uint32_t)k_ra8_io_hex_base;
    ++i;
  } while (x != 0U);
  while (i < (uint32_t)min_digits) {
    tmp[i] = (uint8_t)'0';
    ++i;
  }
  uint8_t out[k_ra8_io_hex_max_digits];
  for (uint32_t j = 0; j < i; ++j) {
    out[j] = tmp[i - 1U - j];
  }
  return ra8_io_stream_write(s, out, i, nullptr);
}
