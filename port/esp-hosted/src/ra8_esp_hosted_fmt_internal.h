/**
 * @file port/esp-hosted/src/ra8_esp_hosted_fmt_internal.h
 * @brief Bounded, heap-free printf-subset formatter for the esp-hosted port.
 *
 * @par Tag
 * [Ring 4 / PORT] {World: NS}
 *
 * @details
 * The vendored esp-hosted core logs with printf-style calls
 * (``ESP_LOGI(TAG, "len %u if %d", len, if_type)``), but this project's
 * logger takes a plain message string and has no formatting at all. Some
 * formatter therefore has to exist between them.
 *
 * It is written here rather than delegated to ``vsnprintf`` because the C
 * library's printf family is not admissible in this image: NASA Power of
 * 10 Rule 3 forbids allocation after initialisation, this board has no
 * heap at all (``_sbrk`` is a strong symbol that reports a fatal error),
 * and newlib's formatting paths are not contractually allocation-free. A
 * bounded formatter that only ever writes into a caller-supplied buffer
 * removes the question.
 *
 * @par Supported conversions
 * The set is derived from what the vendored tree actually uses, measured
 * across every format string in ``libs/third_party/esp-hosted/``:
 * ``%d %i %u %x %X %c %s %p %%``, with length modifiers ``l``, ``ll`` and
 * ``z``, the flags ``0`` (zero pad) and ``-`` (left justify), and a decimal
 * field width. Floating point is deliberately absent: the only ``%f`` in
 * the tree is in the raw-throughput test path, which this build disables,
 * and pulling in soft-float formatting to serve a disabled path would be
 * a poor trade.
 *
 * An unsupported conversion is copied through **verbatim**, so an
 * unhandled specifier shows up in the log as itself rather than silently
 * consuming an argument and desynchronising every later one.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stdarg.h>
#include <stdint.h>

#include "ra8_attributes.h"

/**
 * @enum ra8_esp_hosted_fmt_bound_t
 * @brief Fixed bounds the formatter works within.
 *
 * @details
 * Every loop in the formatter is bounded by one of these, which is what
 * keeps it inside NASA Power of 10 Rule 2 while parsing a format string it
 * did not write. The digit bound covers the widest value the supported
 * bases can produce; the width bound is what a field width is clamped to,
 * so one conversion can never fill an arbitrary buffer.
 *
 * @invariant ::k_ra8_esp_hosted_fmt_digits_max is at least the number of
 *            decimal digits in ``UINT64_MAX`` (twenty).
 * @invariant ::k_ra8_esp_hosted_fmt_width_max is at most the log line
 *            budget, so a clamped width still fits.
 *
 * @par Example:
 * @code
 * char digits[k_ra8_esp_hosted_fmt_digits_max + 1U] = {};
 * @endcode
 *
 * @see ra8_esp_hosted_fmt_utoa
 * @since 0.1.0
 */
typedef enum : uint16_t {
  /** Widest digit run the supported bases produce, from ``UINT64_MAX`` in
      base ten. */
  k_ra8_esp_hosted_fmt_digits_max = 20U,
  /** Largest field width honoured; anything wider is clamped. */
  k_ra8_esp_hosted_fmt_width_max = 64U,
  /** Longest run of specification characters accepted after a per-cent
      sign, which bounds the parser's loops. */
  k_ra8_esp_hosted_fmt_spec_max = 12U,
} ra8_esp_hosted_fmt_bound_t;

/**
 * @enum ra8_esp_hosted_fmt_len_t
 * @brief Argument width selected by a conversion's length modifier.
 *
 * @details
 * Decides how much the formatter pulls off the variable-argument list.
 * Getting this wrong does not merely mis-print one value, it misaligns
 * every argument after it, so the modifier is parsed explicitly rather
 * than assumed.
 *
 * @invariant ``k_ra8_esp_hosted_fmt_len_int`` is the value used when no
 *            modifier is present, matching default argument promotion.
 *
 * @par Example:
 * @code
 * ra8_esp_hosted_fmt_spec_t spec = {};
 * (void)ra8_esp_hosted_fmt_parse("%llu", &spec);
 * @endcode
 *
 * @see ra8_esp_hosted_fmt_spec_t
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_ra8_esp_hosted_fmt_len_int   = 0U, /**< No modifier; argument is `int`. */
  k_ra8_esp_hosted_fmt_len_long  = 1U, /**< `l`; argument is `long`.        */
  k_ra8_esp_hosted_fmt_len_llong = 2U, /**< `ll`; argument is `long long`.  */
  k_ra8_esp_hosted_fmt_len_size  = 3U, /**< `z`; argument is `size_t`.      */
} ra8_esp_hosted_fmt_len_t;

/**
 * @struct ra8_esp_hosted_fmt_spec
 * @brief One parsed conversion specification.
 *
 * @details
 * Produced by ::ra8_esp_hosted_fmt_parse from the characters following a
 * per-cent sign, and consumed by the formatter driver. Splitting parsing
 * from emission is what keeps both functions inside the project's function
 * length limit and lets the parser be tested on its own.
 *
 * @invariant ``consumed`` counts the characters after the per-cent sign,
 *            including the conversion character, and is zero only when the
 *            specification is malformed.
 * @invariant ``conv`` is zero when, and only when, parsing failed.
 *
 * @par Example:
 * @code
 * ra8_esp_hosted_fmt_spec_t spec = {};
 * if (ra8_esp_hosted_fmt_parse("08x rest", &spec)) { emit(&spec); }
 * @endcode
 *
 * @see ra8_esp_hosted_fmt_parse
 * @since 0.1.0
 */
typedef struct ra8_esp_hosted_fmt_spec {
  uint16_t                 width;        /**< Minimum field width; zero if absent.     */
  uint8_t                  consumed;     /**< Characters consumed after the per-cent.  */
  char                     conv;         /**< Conversion character, zero on failure.   */
  ra8_esp_hosted_fmt_len_t len;          /**< Argument width from the length modifier. */
  bool                     zero_pad;     /**< The `0` flag was present.                */
  bool                     left_justify; /**< The `-` flag was present.                */
} ra8_esp_hosted_fmt_spec_t;

/**
 * @brief Parse one conversion specification.
 *
 * @details
 * Reads flags, then an optional decimal width, then an optional length
 * modifier, then the conversion character, from the text immediately
 * following a per-cent sign. Stops at the first character that cannot
 * belong to a specification.
 *
 * A width longer than the formatter's own line budget is clamped rather
 * than honoured, so a hostile or mistyped format string cannot make a
 * single conversion consume the whole buffer.
 *
 * @param[in] after_percent Text following the per-cent sign, NUL
 *                          terminated. Must be non-null.
 * @param[out] out Parsed specification. Must be non-null. Fully written on
 *                 success; on failure ``conv`` and ``consumed`` are zero.
 *
 * @return Whether a complete specification was recognised.
 * @retval true ``out`` holds a specification and ``consumed`` is non-zero.
 * @retval false A pointer was null, the text ended mid-specification, or
 *         the conversion character is not one this formatter emits.
 *
 * @pre ``after_percent`` points at a NUL-terminated string.
 * @pre ``out`` points at writable storage for one specification.
 * @post On success ``out->consumed`` is the exact number of characters to
 *       skip past, including the conversion character.
 * @post On failure ``out->conv`` is zero, which the driver reads as
 *       "copy the specification through verbatim".
 *
 * @note Pure; no module state, safe from any context.
 *
 * @par MC/DC:
 * Promoted from `static` so the flag, width, length-modifier and
 * conversion decisions can be driven directly. The production caller is
 * ::ra8_esp_hosted_fmt_vformat; nothing outside `tests/` may call it.
 *
 * @par Example:
 * @code
 * ra8_esp_hosted_fmt_spec_t spec = {};
 * (void)ra8_esp_hosted_fmt_parse("-8s: ", &spec);
 * @endcode
 *
 * @see ra8_esp_hosted_fmt_vformat
 * @since 0.1.0
 */
RA8_PRIV
[[nodiscard]] bool ra8_esp_hosted_fmt_parse(const char*                after_percent,
                                            ra8_esp_hosted_fmt_spec_t* out);

/**
 * @brief Render an unsigned value into a digit buffer, least digit last.
 *
 * @details
 * Writes the digits of ``value`` in ``base`` into ``buf``, NUL terminated,
 * and reports the digit count. Zero renders as a single ``0`` rather than
 * an empty string. The buffer must be large enough for the widest result
 * the base allows, which is 64 binary-ish digits plus a terminator; the
 * caller sizes it from ::k_ra8_esp_hosted_fmt_digits_max.
 *
 * @param[out] buf Destination for the digits. Must be non-null and at
 *                 least ``k_ra8_esp_hosted_fmt_digits_max + 1`` bytes.
 * @param[in] value Value to render.
 * @param[in] base Radix; only 10 and 16 are produced by this formatter.
 * @param[in] upper Whether hexadecimal digits use upper case.
 *
 * @return Number of digits written, excluding the terminator.
 * @retval 0 ``buf`` was null or ``base`` was outside 2..16.
 *
 * @pre ``buf`` has room for the widest result in ``base``.
 * @pre ``base`` is between 2 and 16 inclusive.
 * @post ``buf`` is NUL terminated whenever a non-zero count is returned.
 * @post The returned count never exceeds
 *       ::k_ra8_esp_hosted_fmt_digits_max.
 *
 * @note Pure; no module state, safe from any context.
 *
 * @par MC/DC:
 * Promoted from `static` so the base and null guards can be exercised
 * without going through a format string. Tests only.
 *
 * @par Example:
 * @code
 * char digits[k_ra8_esp_hosted_fmt_digits_max + 1U] = {};
 * const uint8_t n = ra8_esp_hosted_fmt_utoa(digits, 255U, 16U, false);
 * @endcode
 *
 * @see ra8_esp_hosted_fmt_vformat
 * @since 0.1.0
 */
RA8_PRIV
[[nodiscard]] uint8_t ra8_esp_hosted_fmt_utoa(char* buf, uint64_t value, uint8_t base, bool upper);

/**
 * @brief Format into a bounded buffer from a variable-argument list.
 *
 * @details
 * Walks ``fmt``, copying ordinary characters and expanding the supported
 * conversions listed in the file-level documentation. Writing stops when
 * the buffer is one byte from full; the result is always NUL terminated
 * when ``cap`` is non-zero, and the return value is the number of
 * characters actually written, not the number that would have been
 * written. Truncation is therefore visible to the caller without a second
 * pass and without ever reporting a length past the end of the buffer.
 *
 * @param[out] out Destination buffer. Must be non-null when ``cap`` is
 *                 non-zero.
 * @param[in] cap Capacity of ``out`` in bytes, including the terminator.
 * @param[in] fmt Format string. Must be non-null.
 * @param[in] ap Argument list positioned at the first conversion argument.
 *               The caller owns starting and ending it.
 *
 * @return Characters written, excluding the terminator.
 * @retval 0 ``cap`` was zero, or a pointer was null, or ``fmt`` was empty.
 *
 * @pre ``out`` has room for ``cap`` bytes.
 * @pre ``ap`` supplies one argument per conversion in ``fmt``.
 * @post ``out`` is NUL terminated whenever ``cap`` is non-zero.
 * @post The return value is strictly less than ``cap``.
 *
 * @note Pure with respect to module state; the only writes are into the
 *       caller's buffer, so it is reentrant and safe from interrupt
 *       context.
 * @warning An unsupported conversion is copied through verbatim and its
 *          argument is NOT consumed, so later arguments stay aligned with
 *          their conversions.
 *
 * @par MC/DC:
 * Promoted from `static` so the truncation, conversion-dispatch and
 * padding decisions can be driven directly. Production callers reach it
 * through ``ra8_esp_hosted_log_write``; tests may call it directly.
 *
 * @par Example:
 * @code
 * char line[64] = {};
 * va_list ap;
 * va_start(ap, fmt);
 * (void)ra8_esp_hosted_fmt_vformat(line, sizeof(line), fmt, ap);
 * va_end(ap);
 * @endcode
 *
 * @see ra8_esp_hosted_fmt_parse
 * @since 0.1.0
 */
RA8_PRIV
uint32_t ra8_esp_hosted_fmt_vformat(char* out, uint32_t cap, const char* fmt, va_list ap);
