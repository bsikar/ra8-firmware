/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file port/esp-hosted/src/ra8_esp_hosted_fmt.c
 * @brief Bounded printf-subset formatter used by the esp-hosted port logger.
 *
 * @par Tag
 * [Ring 4 / PORT] {World: NS}
 *
 * @details
 * Implementation of the contract in ``ra8_esp_hosted_fmt_internal.h``. The
 * design follows from two constraints that pull in the same direction:
 * the image has no heap, and every loop must have a statically provable
 * bound. So the formatter never allocates, writes only into the caller's
 * buffer, and bounds each of its three loops -- specification parsing,
 * digit generation and padding -- by a compile-time constant from
 * ``ra8_esp_hosted_fmt_bound_t``.
 *
 * Emission goes through a small cursor structure rather than a running
 * pointer so the "one byte reserved for the terminator" rule is expressed
 * once, in ::internal_put, instead of at every call site.
 *
 * @since 0.1.0
 */

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_esp_hosted_fmt_internal.h"

/**
 * @enum ra8_esp_hosted_fmt_char_t
 * @brief Characters the parser and emitter test for by name.
 *
 * @details
 * Named so the parsing logic reads as grammar rather than as arithmetic on
 * character codes, and so the project's no-magic-number rule is satisfied
 * without scattering casts through the control flow.
 *
 * @invariant Every value is a 7-bit ASCII code point.
 *
 * @par Example:
 * @code
 * if (ch == (char)k_ra8_esp_hosted_fmt_ch_percent) { parse_spec(); }
 * @endcode
 *
 * @see ra8_esp_hosted_fmt_parse
 * @since 0.1.0
 */
typedef enum : char {
  k_ra8_esp_hosted_fmt_ch_nul     = '\0', /**< String terminator.                   */
  k_ra8_esp_hosted_fmt_ch_percent = '%',  /**< Starts a conversion.                 */
  k_ra8_esp_hosted_fmt_ch_zero    = '0',  /**< Zero-pad flag and first digit.       */
  k_ra8_esp_hosted_fmt_ch_nine    = '9',  /**< Last decimal digit.                  */
  k_ra8_esp_hosted_fmt_ch_minus   = '-',  /**< Left-justify flag and sign.          */
  k_ra8_esp_hosted_fmt_ch_space   = ' ',  /**< Pad character when not zero-padding. */
  k_ra8_esp_hosted_fmt_ch_ell     = 'l',  /**< Long length modifier.                */
  k_ra8_esp_hosted_fmt_ch_zed     = 'z',  /**< Size-type length modifier.           */
} ra8_esp_hosted_fmt_char_t;

/**
 * @enum ra8_esp_hosted_fmt_radix_t
 * @brief Radices the formatter renders.
 *
 * @details
 * Only the two the vendored core's format strings ask for. Any other base
 * is rejected by ::ra8_esp_hosted_fmt_utoa rather than silently rendered,
 * because a base the caller did not intend would produce a plausible but
 * wrong number.
 *
 * @invariant Both values are within the 2..16 range the digit table
 *            covers.
 *
 * @par Example:
 * @code
 * (void)ra8_esp_hosted_fmt_utoa(buf, v, (uint8_t)k_ra8_esp_hosted_fmt_radix_hex, false);
 * @endcode
 *
 * @see ra8_esp_hosted_fmt_utoa
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_ra8_esp_hosted_fmt_radix_min = 2U,  /**< Smallest base the digit table serves. */
  k_ra8_esp_hosted_fmt_radix_dec = 10U, /**< Decimal, for the integer conversions. */
  k_ra8_esp_hosted_fmt_radix_hex = 16U, /**< Hexadecimal, for `x`, `X` and `p`.    */
} ra8_esp_hosted_fmt_radix_t;

/**
 * @struct ra8_esp_hosted_fmt_args
 * @brief Wrapper that lets a ``va_list`` be passed to a helper by address.
 *
 * @details
 * ``va_list`` is an array type on some ABIs and a structure on others, so
 * ``&ap`` does not have a portable type. Wrapping it in a structure gives
 * the helpers one spelling that advances the caller's list correctly on
 * both the ARM cross build and the x86-64 host test build.
 *
 * @invariant The wrapped list is started by the owner and ended exactly
 *            once.
 *
 * @par Example:
 * @code
 * ra8_esp_hosted_fmt_args_t args;
 * va_copy(args.ap, ap);
 * @endcode
 *
 * @see ra8_esp_hosted_fmt_vformat
 * @since 0.1.0
 */
typedef struct ra8_esp_hosted_fmt_args {
  va_list ap; /**< The wrapped variable-argument list. */
} ra8_esp_hosted_fmt_args_t;

/**
 * @struct ra8_esp_hosted_fmt_cursor
 * @brief Write cursor over the caller's output buffer.
 *
 * @details
 * Carries the capacity alongside the position so the terminator reserve is
 * enforced in one place. ``len`` is also the value the driver returns, so
 * truncation is reported without a second measurement pass.
 *
 * @invariant ``len`` is always strictly less than ``cap`` while ``cap`` is
 *            non-zero.
 * @invariant ``out`` is null only when ``cap`` is zero.
 *
 * @par Example:
 * @code
 * ra8_esp_hosted_fmt_cursor_t cur = { .out = buf, .cap = sizeof(buf), .len = 0U };
 * @endcode
 *
 * @see ra8_esp_hosted_fmt_vformat
 * @since 0.1.0
 */
typedef struct ra8_esp_hosted_fmt_cursor {
  char*    out; /**< Destination buffer.                */
  uint32_t cap; /**< Capacity including the terminator. */
  uint32_t len; /**< Characters written so far.         */
} ra8_esp_hosted_fmt_cursor_t;

/**
 * @var k_ra8_esp_hosted_fmt_digits_lower
 * @brief Digit glyphs for lower-case hexadecimal and every smaller base.
 * @details Indexed by the remainder, so the table doubles as the decimal
 * digit set.
 * @note Read-only.
 * @warning Must stay sixteen entries; the radix bound assumes it.
 * @since 0.1.0
 */
static const char k_ra8_esp_hosted_fmt_digits_lower[] = "0123456789abcdef";

/**
 * @var k_ra8_esp_hosted_fmt_digits_upper
 * @brief Digit glyphs for upper-case hexadecimal.
 * @details Selected by the ``X`` conversion only.
 * @note Read-only.
 * @warning Must stay sixteen entries; the radix bound assumes it.
 * @since 0.1.0
 */
static const char k_ra8_esp_hosted_fmt_digits_upper[] = "0123456789ABCDEF";

/**
 * @brief Append one character if the buffer still has room for it.
 *
 * @details
 * Reserves the last byte for the terminator, so a full buffer silently
 * drops further characters rather than overwriting the NUL.
 *
 * @param[in,out] cur Cursor to append through. Must be non-null.
 * @param[in] ch Character to append.
 *
 * @return Nothing.
 *
 * @pre ``cur`` is non-null and its buffer holds ``cap`` bytes.
 * @pre ``cur->len`` is less than ``cur->cap``.
 * @post ``cur->len`` grows by at most one.
 * @post ``cur->len`` stays strictly below ``cur->cap``.
 *
 * @note Reentrant; touches only the caller's cursor.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_put(ra8_esp_hosted_fmt_cursor_t* cur, char ch)
{
  if ((cur == nullptr) || (cur->out == nullptr)) {
    return;
  }
  if ((cur->len + 1U) >= cur->cap) {
    return;
  }
  cur->out[cur->len] = ch;
  cur->len++;
}

/**
 * @brief Append the pad character a bounded number of times.
 *
 * @details
 * Used for both leading and trailing padding; which one is decided by the
 * caller, which is what keeps the left-justify decision in one place.
 *
 * @param[in,out] cur Cursor to append through. Must be non-null.
 * @param[in] pad Character to repeat.
 * @param[in] count Repeat count, already clamped by the caller.
 *
 * @return Nothing.
 *
 * @pre ``cur`` is non-null.
 * @pre ``count`` is at most ::k_ra8_esp_hosted_fmt_width_max.
 * @post At most ``count`` characters are appended.
 * @post The loop runs a statically bounded number of iterations.
 *
 * @note Reentrant; touches only the caller's cursor.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_pad(ra8_esp_hosted_fmt_cursor_t* cur, char pad, uint16_t count)
{
  uint16_t remaining = count;
  if (remaining > (uint16_t)k_ra8_esp_hosted_fmt_width_max) {
    remaining = (uint16_t)k_ra8_esp_hosted_fmt_width_max;
  }
  for (uint16_t i = 0U; i < remaining; i++) {
    internal_put(cur, pad);
  }
}

/** @brief Implementation of `ra8_esp_hosted_fmt_utoa()` -- divides down and
 *  reverses in place, so no scratch beyond the caller's buffer. */
RA8_PRIV
uint8_t ra8_esp_hosted_fmt_utoa(char* buf, uint64_t value, uint8_t base, bool upper)
{
  if (buf == nullptr) {
    return 0U;
  }
  if ((base < (uint8_t)k_ra8_esp_hosted_fmt_radix_min) ||
      (base > (uint8_t)k_ra8_esp_hosted_fmt_radix_hex)) {
    return 0U;
  }

  const char* glyphs =
    upper ? k_ra8_esp_hosted_fmt_digits_upper : k_ra8_esp_hosted_fmt_digits_lower;
  uint64_t remaining = value;
  uint8_t  count     = 0U;

  for (uint8_t i = 0U; i < (uint8_t)k_ra8_esp_hosted_fmt_digits_max; i++) {
    buf[count] = glyphs[remaining % (uint64_t)base];
    count++;
    remaining /= (uint64_t)base;
    if (remaining == 0U) {
      break;
    }
  }
  buf[count] = (char)k_ra8_esp_hosted_fmt_ch_nul;

  for (uint8_t i = 0U; i < (uint8_t)(count / 2U); i++) {
    const char swap                = buf[i];
    buf[i]                         = buf[(uint8_t)(count - 1U - i)];
    buf[(uint8_t)(count - 1U - i)] = swap;
  }
  return count;
}

/**
 * @brief Recognise and record the flag characters of a specification.
 *
 * @details
 * Consumes any run of ``0`` and ``-`` characters, bounded so a pathological
 * format string cannot spin here.
 *
 * @param[in] text Text after the per-cent sign. Must be non-null.
 * @param[in,out] out Specification whose flag fields are set.
 *
 * @return Characters consumed.
 * @retval 0 No flag characters were present.
 *
 * @pre ``text`` is NUL terminated.
 * @pre ``out`` is non-null.
 * @post ``out->zero_pad`` and ``out->left_justify`` reflect the run.
 * @post The return value is at most ::k_ra8_esp_hosted_fmt_spec_max.
 *
 * @note Reentrant; no module state.
 * @since 0.1.0
 */
RA8_INTERNAL
static uint8_t internal_parse_flags(const char* text, ra8_esp_hosted_fmt_spec_t* out)
{
  uint8_t used = 0U;
  for (uint8_t i = 0U; i < (uint8_t)k_ra8_esp_hosted_fmt_spec_max; i++) {
    if (text[used] == (char)k_ra8_esp_hosted_fmt_ch_zero) {
      out->zero_pad = true;
    } else if (text[used] == (char)k_ra8_esp_hosted_fmt_ch_minus) {
      out->left_justify = true;
    } else {
      break;
    }
    used++;
  }
  return used;
}

/**
 * @brief Read the optional decimal field width of a specification.
 *
 * @details
 * Accumulates digits, clamping at ::k_ra8_esp_hosted_fmt_width_max so an
 * absurd width cannot make one conversion fill the whole line.
 *
 * @param[in] text Text positioned at the first width digit, if any.
 * @param[out] out_width Parsed width; zero when no digits were present.
 *
 * @return Characters consumed.
 * @retval 0 The next character was not a digit.
 *
 * @pre ``text`` is NUL terminated.
 * @pre ``out_width`` is non-null.
 * @post ``*out_width`` is at most ::k_ra8_esp_hosted_fmt_width_max.
 * @post The return value is at most ::k_ra8_esp_hosted_fmt_spec_max.
 *
 * @note Reentrant; no module state.
 * @since 0.1.0
 */
RA8_INTERNAL
static uint8_t internal_parse_width(const char* text, uint16_t* out_width)
{
  uint32_t width = 0U;
  uint8_t  used  = 0U;
  for (uint8_t i = 0U; i < (uint8_t)k_ra8_esp_hosted_fmt_spec_max; i++) {
    if ((text[used] < (char)k_ra8_esp_hosted_fmt_ch_zero) ||
        (text[used] > (char)k_ra8_esp_hosted_fmt_ch_nine)) {
      break;
    }
    width = (width * (uint32_t)k_ra8_esp_hosted_fmt_radix_dec) +
            (uint32_t)(text[used] - (char)k_ra8_esp_hosted_fmt_ch_zero);
    if (width > (uint32_t)k_ra8_esp_hosted_fmt_width_max) {
      width = (uint32_t)k_ra8_esp_hosted_fmt_width_max;
    }
    used++;
  }
  *out_width = (uint16_t)width;
  return used;
}

/**
 * @brief Read the optional length modifier of a specification.
 *
 * @details
 * Recognises ``l``, ``ll`` and ``z``. Anything else leaves the default
 * `int` width in place.
 *
 * @param[in] text Text positioned at the modifier, if any.
 * @param[out] out_len Parsed argument width.
 *
 * @return Characters consumed.
 * @retval 0 No modifier was present.
 *
 * @pre ``text`` is NUL terminated.
 * @pre ``out_len`` is non-null.
 * @post ``*out_len`` is one of the ::ra8_esp_hosted_fmt_len_t values.
 * @post The return value is at most two.
 *
 * @note Reentrant; no module state.
 * @since 0.1.0
 */
RA8_INTERNAL
static uint8_t internal_parse_len(const char* text, ra8_esp_hosted_fmt_len_t* out_len)
{
  *out_len = k_ra8_esp_hosted_fmt_len_int;
  if (text[0] == (char)k_ra8_esp_hosted_fmt_ch_zed) {
    *out_len = k_ra8_esp_hosted_fmt_len_size;
    return 1U;
  }
  if (text[0] != (char)k_ra8_esp_hosted_fmt_ch_ell) {
    return 0U;
  }
  if (text[1] == (char)k_ra8_esp_hosted_fmt_ch_ell) {
    *out_len = k_ra8_esp_hosted_fmt_len_llong;
    return 2U;
  }
  *out_len = k_ra8_esp_hosted_fmt_len_long;
  return 1U;
}

/**
 * @brief Whether a character is a conversion this formatter emits.
 *
 * @details
 * The set is closed deliberately: a conversion outside it is copied
 * through verbatim by the driver instead of consuming an argument, which
 * keeps every later argument aligned with its conversion.
 *
 * @param[in] conv Candidate conversion character.
 *
 * @return Whether the formatter handles it.
 * @retval true The driver will expand it.
 * @retval false The driver will copy the specification through verbatim.
 *
 * @pre None; any character value is accepted.
 * @pre The caller has already consumed the flags, width and length.
 * @post No state is modified.
 * @post The answer depends only on ``conv``.
 *
 * @note Reentrant; no module state.
 * @since 0.1.0
 */
RA8_INTERNAL
static bool internal_is_supported(char conv)
{
  return (conv == 'd') || (conv == 'i') || (conv == 'u') || (conv == 'x') || (conv == 'X') ||
         (conv == 'c') || (conv == 's') || (conv == 'p') ||
         (conv == (char)k_ra8_esp_hosted_fmt_ch_percent);
}

/** @brief Implementation of `ra8_esp_hosted_fmt_parse()` -- flags, width,
 *  length modifier and conversion, each bounded by the specification
 *  length cap. */
RA8_PRIV
bool ra8_esp_hosted_fmt_parse(const char* after_percent, ra8_esp_hosted_fmt_spec_t* out)
{
  if ((after_percent == nullptr) || (out == nullptr)) {
    return false;
  }

  *out = (ra8_esp_hosted_fmt_spec_t){};

  uint8_t used = internal_parse_flags(after_percent, out);
  used         = (uint8_t)(used + internal_parse_width(&after_percent[used], &out->width));
  used         = (uint8_t)(used + internal_parse_len(&after_percent[used], &out->len));

  const char conv = after_percent[used];
  if (!internal_is_supported(conv)) {
    return false;
  }

  out->conv     = conv;
  out->consumed = (uint8_t)(used + 1U);
  return true;
}

/**
 * @brief Pull the next unsigned argument at the width the modifier names.
 *
 * @details
 * Reading at the wrong width would misalign every later argument, so each
 * modifier reads at exactly the type it names -- ``z`` at ``size_t``, not at
 * whichever fixed-width type happens to share its size on one target. C
 * leaves ``va_arg`` undefined when the requested type is merely the same size
 * as the argument's rather than compatible with it, so the four reads cannot
 * be merged even where two of them are the same number of bytes.
 *
 * The dispatch is a sequence of guarded early returns rather than a
 * ``switch``: it matches how ::internal_emit_conv below selects a conversion,
 * and it keeps clang-tidy's ``bugprone-branch-clone`` off a construct it
 * cannot read correctly -- its statement profiler ignores the type operand of
 * ``va_arg``, so four reads of four different types profile as one.
 *
 * @param[in,out] args Wrapped argument list to advance.
 * @param[in] len Argument width to read.
 *
 * @return The argument, widened to 64 bits.
 * @retval 0 The argument itself was zero.
 *
 * @pre ``args`` is positioned at an argument of the named width.
 * @pre ``len`` is one of the ::ra8_esp_hosted_fmt_len_t values.
 * @post ``args`` has advanced past exactly one argument.
 * @post The result is zero-extended, never sign-extended.
 *
 * @note Reentrant; no module state.
 * @since 0.1.0
 */
RA8_INTERNAL
static uint64_t internal_next_unsigned(ra8_esp_hosted_fmt_args_t* args,
                                       ra8_esp_hosted_fmt_len_t   len)
{
  if (len == k_ra8_esp_hosted_fmt_len_llong) {
    /* NOLINTNEXTLINE(clang-analyzer-valist.Uninitialized) -- analyser cannot follow the va_copy. */
    return (uint64_t)va_arg(args->ap, unsigned long long);
  }
  if (len == k_ra8_esp_hosted_fmt_len_long) {
    /* NOLINTNEXTLINE(clang-analyzer-valist.Uninitialized) -- analyser cannot follow the va_copy. */
    return (uint64_t)va_arg(args->ap, unsigned long);
  }
  if (len == k_ra8_esp_hosted_fmt_len_size) {
    /* NOLINTNEXTLINE(clang-analyzer-valist.Uninitialized) -- analyser cannot follow the va_copy. */
    return (uint64_t)va_arg(args->ap, size_t);
  }
  /* NOLINTNEXTLINE(clang-analyzer-valist.Uninitialized) -- analyser cannot follow the va_copy. */
  return (uint64_t)va_arg(args->ap, unsigned int);
}

/**
 * @brief Pull the next signed argument at the width the modifier names.
 *
 * @details
 * Mirrors ::internal_next_unsigned for the ``d`` and ``i`` conversions,
 * including its reason for dispatching with guarded early returns instead of
 * a ``switch``.
 *
 * @param[in,out] args Wrapped argument list to advance.
 * @param[in] len Argument width to read.
 *
 * @return The argument, sign-extended to 64 bits.
 * @retval 0 The argument itself was zero.
 *
 * @pre ``args`` is positioned at an argument of the named width.
 * @pre ``len`` is one of the ::ra8_esp_hosted_fmt_len_t values.
 * @post ``args`` has advanced past exactly one argument.
 * @post The sign of the argument is preserved.
 *
 * @note Reentrant; no module state.
 * @since 0.1.0
 */
RA8_INTERNAL
static int64_t internal_next_signed(ra8_esp_hosted_fmt_args_t* args, ra8_esp_hosted_fmt_len_t len)
{
  if (len == k_ra8_esp_hosted_fmt_len_llong) {
    /* NOLINTNEXTLINE(clang-analyzer-valist.Uninitialized) -- analyser cannot follow the va_copy. */
    return (int64_t)va_arg(args->ap, long long);
  }
  if (len == k_ra8_esp_hosted_fmt_len_long) {
    /* NOLINTNEXTLINE(clang-analyzer-valist.Uninitialized) -- analyser cannot follow the va_copy. */
    return (int64_t)va_arg(args->ap, long);
  }
  if (len == k_ra8_esp_hosted_fmt_len_size) {
    /* NOLINTNEXTLINE(clang-analyzer-valist.Uninitialized) -- analyser cannot follow the va_copy. */
    return (int64_t)va_arg(args->ap, ptrdiff_t);
  }
  /* NOLINTNEXTLINE(clang-analyzer-valist.Uninitialized) -- analyser cannot follow the va_copy. */
  return (int64_t)va_arg(args->ap, int);
}

/**
 * @brief Emit an already-rendered token honouring width and justification.
 *
 * @details
 * The single place the padding rules live, so every conversion pads the
 * same way. Zero padding only applies when the token is right-justified;
 * a left-justified field pads with spaces regardless, matching C's own
 * rule.
 *
 * @param[in,out] cur Cursor to append through.
 * @param[in] spec Specification supplying width and flags.
 * @param[in] token NUL-terminated text to emit.
 * @param[in] token_len Length of ``token`` in characters.
 *
 * @return Nothing.
 *
 * @pre ``cur``, ``spec`` and ``token`` are non-null.
 * @pre ``token_len`` is the length of ``token``.
 * @post The field is at least ``spec->width`` characters wide, subject to
 *       the buffer's remaining capacity.
 * @post No more than ``k_ra8_esp_hosted_fmt_width_max`` pad characters are
 *       emitted.
 *
 * @note Reentrant; touches only the caller's cursor.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_emit_token(ra8_esp_hosted_fmt_cursor_t*     cur,
                                const ra8_esp_hosted_fmt_spec_t* spec,
                                const char*                      token,
                                uint16_t                         token_len)
{
  const uint16_t pad_count = (spec->width > token_len) ? (uint16_t)(spec->width - token_len) : 0U;
  const char pad = (spec->zero_pad && !spec->left_justify) ? (char)k_ra8_esp_hosted_fmt_ch_zero
                                                           : (char)k_ra8_esp_hosted_fmt_ch_space;

  if (!spec->left_justify) {
    internal_pad(cur, pad, pad_count);
  }
  for (uint16_t i = 0U; i < token_len; i++) {
    internal_put(cur, token[i]);
  }
  if (spec->left_justify) {
    internal_pad(cur, (char)k_ra8_esp_hosted_fmt_ch_space, pad_count);
  }
}

/**
 * @brief Measure a NUL-terminated string, bounded by the width cap.
 *
 * @details
 * A bounded measurement rather than ``strlen`` so a co-processor-supplied
 * string that lost its terminator cannot run off the end of memory. A
 * string longer than the cap is reported as exactly the cap and is
 * therefore truncated in the output, which is visible rather than fatal.
 *
 * @param[in] text String to measure. Must be non-null.
 *
 * @return Length in characters, at most the emitter's line budget.
 * @retval 0 The string was empty.
 *
 * @pre ``text`` is non-null.
 * @pre The caller tolerates a truncated measurement.
 * @post No state is modified.
 * @post The result never exceeds the bound.
 *
 * @note Reentrant; no module state.
 * @since 0.1.0
 */
RA8_INTERNAL
static uint16_t internal_bounded_len(const char* text)
{
  uint16_t len = 0U;
  for (uint16_t i = 0U; i < (uint16_t)k_ra8_esp_hosted_fmt_width_max; i++) {
    if (text[len] == (char)k_ra8_esp_hosted_fmt_ch_nul) {
      break;
    }
    len++;
  }
  return len;
}

/**
 * @brief Expand one parsed conversion into the cursor.
 *
 * @details
 * Dispatches on the conversion character, pulls exactly one argument for
 * the conversions that take one, and hands the rendered token to
 * ::internal_emit_token so padding is applied uniformly.
 *
 * @param[in,out] cur Cursor to append through.
 * @param[in] spec Parsed conversion specification.
 * @param[in,out] args Wrapped argument list to advance.
 *
 * @return Nothing.
 *
 * @pre ``cur`` and ``spec`` are non-null and ``spec->conv`` is supported.
 * @pre ``args`` is positioned at this conversion's argument.
 * @post Exactly one argument is consumed, except for the per-cent
 *       conversion which consumes none.
 * @post The cursor grows by at most the field width plus the token length.
 *
 * @note Reentrant; touches only the caller's cursor and argument list.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_emit_conv(ra8_esp_hosted_fmt_cursor_t*     cur,
                               const ra8_esp_hosted_fmt_spec_t* spec,
                               ra8_esp_hosted_fmt_args_t*       args)
{
  char digits[(uint32_t)k_ra8_esp_hosted_fmt_digits_max + 2U] = {};

  if (spec->conv == (char)k_ra8_esp_hosted_fmt_ch_percent) {
    internal_put(cur, (char)k_ra8_esp_hosted_fmt_ch_percent);
    return;
  }
  if (spec->conv == 'c') {
    /* NOLINTNEXTLINE(clang-analyzer-valist.Uninitialized) -- analyser cannot follow the va_copy. */
    digits[0] = (char)va_arg(args->ap, int);
    internal_emit_token(cur, spec, digits, 1U);
    return;
  }
  if (spec->conv == 's') {
    /* NOLINTNEXTLINE(clang-analyzer-valist.Uninitialized) -- analyser cannot follow the va_copy. */
    const char* text = va_arg(args->ap, const char*);
    const char* safe = (text == nullptr) ? "(null)" : text;
    internal_emit_token(cur, spec, safe, internal_bounded_len(safe));
    return;
  }
  if ((spec->conv == 'd') || (spec->conv == 'i')) {
    const int64_t  value     = internal_next_signed(args, spec->len);
    const bool     negative  = (value < 0);
    const uint64_t magnitude = negative ? (uint64_t)(-(value + 1)) + 1U : (uint64_t)value;
    uint8_t        used      = 0U;
    if (negative) {
      digits[0] = (char)k_ra8_esp_hosted_fmt_ch_minus;
      used      = 1U;
    }
    const uint8_t n = ra8_esp_hosted_fmt_utoa(&digits[used],
                                              magnitude,
                                              (uint8_t)k_ra8_esp_hosted_fmt_radix_dec,
                                              false);
    internal_emit_token(cur, spec, digits, (uint16_t)(used + n));
    return;
  }

  const bool    is_pointer = (spec->conv == 'p');
  const bool    upper      = (spec->conv == 'X');
  const uint8_t base       = ((spec->conv == 'u') ? (uint8_t)k_ra8_esp_hosted_fmt_radix_dec
                                                  : (uint8_t)k_ra8_esp_hosted_fmt_radix_hex);
  /* NOLINTNEXTLINE(clang-analyzer-valist.Uninitialized) -- analyser cannot follow the va_copy. */
  const uint64_t value = is_pointer ? (uint64_t)(uintptr_t)va_arg(args->ap, void*)
                                    : internal_next_unsigned(args, spec->len);
  const uint8_t  n     = ra8_esp_hosted_fmt_utoa(digits, value, base, upper);
  internal_emit_token(cur, spec, digits, (uint16_t)n);
}

/** @brief Implementation of `ra8_esp_hosted_fmt_vformat()` -- single pass,
 *  every loop bounded, no allocation. */
RA8_PRIV
uint32_t ra8_esp_hosted_fmt_vformat(char* out, uint32_t cap, const char* fmt, va_list ap)
{
  if ((out == nullptr) || (cap == 0U) || (fmt == nullptr)) {
    if ((out != nullptr) && (cap != 0U)) {
      out[0] = (char)k_ra8_esp_hosted_fmt_ch_nul;
    }
    return 0U;
  }

  ra8_esp_hosted_fmt_cursor_t cur = {.out = out, .cap = cap, .len = 0U};
  ra8_esp_hosted_fmt_args_t   args;
  uint32_t                    pos = 0U;

  va_copy(args.ap, ap);

  for (uint32_t guard = 0U; guard < cap; guard++) {
    const char ch = fmt[pos];
    if (ch == (char)k_ra8_esp_hosted_fmt_ch_nul) {
      break;
    }
    pos++;
    if (ch != (char)k_ra8_esp_hosted_fmt_ch_percent) {
      internal_put(&cur, ch);
      continue;
    }

    ra8_esp_hosted_fmt_spec_t spec = {};
    if (!ra8_esp_hosted_fmt_parse(&fmt[pos], &spec)) {
      internal_put(&cur, (char)k_ra8_esp_hosted_fmt_ch_percent);
      continue;
    }
    internal_emit_conv(&cur, &spec, &args);
    pos += spec.consumed;
  }

  va_end(args.ap);
  cur.out[cur.len] = (char)k_ra8_esp_hosted_fmt_ch_nul;
  return cur.len;
}
