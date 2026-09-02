/**
 * @file test_ra8_esp_hosted_fmt.c
 * @brief Unit tests for the esp-hosted port's bounded printf-subset formatter.
 *
 * @par Tag
 * [Test / Host] {World: N/A}
 *
 * @details
 * Drives all three promoted entry points of
 * ``port/esp-hosted/src/ra8_esp_hosted_fmt.c`` -- ``priv_ra8_esp_hosted_fmt_parse``,
 * ``priv_ra8_esp_hosted_fmt_utoa`` and ``priv_ra8_esp_hosted_fmt_vformat`` -- plus every
 * static helper they reach: the write cursor, the pad emitter, the flag,
 * width and length-modifier scanners, the supported-conversion set, the two
 * variable-argument readers, the token emitter and the bounded string
 * measurement.
 *
 * The formatter is the one piece of the port with no hardware in it at all,
 * so every decision it carries is reachable from a host test. The tests below
 * are organised by decision rather than by entry point, and each carries the
 * MC/DC vector set for the decisions it drives.
 *
 * @par What is deliberately not asserted
 * ``%p`` renders a host pointer, whose value is not knowable at authoring
 * time. Rather than assert a fixed string, the pointer test renders the same
 * value through ``priv_ra8_esp_hosted_fmt_utoa`` and compares -- which is the
 * property that actually matters (the two agree) without pinning an address.
 *
 * No hardware registers are touched; no ``ra8_fake_mmap`` window is required.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <limits.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_esp_hosted_fmt_internal.h"
#include "unity_minimal.h"

/**
 * @enum t_fmt_const_t
 * @brief Fixture sizes and bounds this translation unit works within.
 *
 * @details
 * Named rather than spelled inline so a reader can tell a buffer size from a
 * field width at the point of use.
 *
 * @invariant ::k_t_fmt_digits_cap is at least
 *            ``k_ra8_esp_hosted_fmt_digits_max + 2``, the size the formatter's
 *            own digit scratch uses.
 *
 * @par Example:
 * @code
 * char digits[k_t_fmt_digits_cap] = {};
 * @endcode
 *
 * @see priv_ra8_esp_hosted_fmt_utoa
 */
typedef enum : uint32_t {
  k_t_fmt_digits_cap    = 32U,  /**< Digit-scratch buffer size.            */
  k_t_fmt_line_cap      = 256U, /**< Formatted-line buffer size.           */
  k_t_fmt_long_str      = 100U, /**< Length of the over-long `%s` fixture. */
  k_t_fmt_u64_dec_width = 20U,  /**< Decimal digits `UINT64_MAX` renders to.
 */
} t_fmt_const_t;

/**
 * @enum t_fmt_u32_sample_t
 * @brief The unsigned argument values the conversion tests feed the formatter.
 *
 * @details
 * Each is chosen for the boundary it drives rather than for its own sake: the
 * 32-bit maximum proves an unsigned read is not sign-extended, the hexadecimal
 * word proves both digit cases render every nibble, and the small size value
 * proves the `z` length modifier reads a `size_t` rather than an `int`.
 *
 * @invariant Every value is representable in `uint32_t`, so each is passed to
 *            the variadic formatter as an `unsigned int` with no promotion.
 *
 * @par Example:
 * @code
 * internal_t_format(line, cap, "%x", k_t_fmt_hex_sample);
 * @endcode
 *
 * @see t_fmt_i32_sample_t
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_t_fmt_u32_max     = 4294967295U, /**< `UINT32_MAX`, the widest `%u` argument.      */
  k_t_fmt_hex_sample  = 0xDEADBEEFU, /**< Hexadecimal word exercising `%x` and `%X`.   */
  k_t_fmt_size_sample = 99U,         /**< Value rendered through the `%zu` conversion. */
} t_fmt_u32_sample_t;

/**
 * @enum t_fmt_i32_sample_t
 * @brief The signed argument values the conversion and padding tests feed in.
 *
 * @details
 * ::k_t_fmt_int_sample renders as the two-character token every padding vector
 * is measured against, and ::k_t_fmt_wide_sample renders as the four-character
 * token that is wider than the field the width vector requests.
 * ::k_t_fmt_small_int_sample is negated at the call site to drive the sign
 * branch, so the same name carries both the positive and negative vector.
 *
 * @invariant ::k_t_fmt_wide_sample renders to more characters than the
 *            narrowest field width any padding vector requests, which is what
 *            makes the "no truncation" assertion meaningful.
 *
 * @par Example:
 * @code
 * internal_t_format(line, cap, "%5d", k_t_fmt_int_sample);
 * @endcode
 *
 * @see t_fmt_u32_sample_t
 * @since 0.1.0
 */
typedef enum : int32_t {
  k_t_fmt_int_sample       = 42,   /**< Two-character positive token ("42").              */
  k_t_fmt_small_int_sample = 7,    /**< Negated at the call site to render "-7".          */
  k_t_fmt_wide_sample      = 1234, /**< Four-character token, wider than the `%1d` field. */
} t_fmt_i32_sample_t;

/**
 * @enum t_fmt_i64_sample_t
 * @brief The 64-bit argument values driving the `l` / `ll` length modifiers.
 *
 * @details
 * Both sit one step outside the 32-bit range in opposite directions, so a
 * formatter that read only 32 bits would render `0` for the positive vector
 * and `0` (or a wrapped value) for the negative one. Held as one signed enum
 * because the positive vector is cast to `unsigned long` at its `%lu` call
 * site, where the exact parameter type is what the conversion promises.
 *
 * @invariant `|value|` is `UINT32_MAX + 1` for both enumerators, the first
 *            magnitude a 32-bit read cannot represent.
 *
 * @par Example:
 * @code
 * internal_t_format(line, cap, "%lld", k_t_fmt_i64_below_neg_u32);
 * @endcode
 *
 * @see t_fmt_u32_sample_t
 * @since 0.1.0
 */
typedef enum : int64_t {
  k_t_fmt_i64_above_u32     = 4294967296LL,  /**< First value above `UINT32_MAX`.      */
  k_t_fmt_i64_below_neg_u32 = -4294967296LL, /**< Its negation, for the signed `%lld`. */
} t_fmt_i64_sample_t;

/**
 * @brief Format through the variable-argument entry point under test.
 *
 * @details
 * ``priv_ra8_esp_hosted_fmt_vformat`` takes an already-started ``va_list``, so a
 * test needs one varargs bridge to reach it. Deliberately carries NO
 * ``[[gnu::format]]`` attribute: several tests pass a conversion the compiler
 * would reject under ``-Wformat`` (``%f`` with no argument is exactly the
 * case being tested), and the point of those tests is that the formatter --
 * not the compiler -- decides what happens.
 *
 * @param[out] out Destination buffer; may be null to drive that guard.
 * @param[in] cap Capacity of @p out including the terminator.
 * @param[in] fmt Format string; may be null to drive that guard.
 * @param[in] ... Arguments consumed by @p fmt.
 *
 * @return Characters written, as reported by the function under test.
 *
 * @pre @p out has room for @p cap bytes when both are non-degenerate.
 * @pre The variable arguments match the conversions the test intends.
 * @post The argument list is ended exactly once.
 * @post No test state is modified.
 *
 * @note Not thread-safe; single-threaded host test driver.
 * @retval 0 The formatter rejected the request or emitted an empty result.
 * @since 0.1.0
 */
RA8_INTERNAL static uint32_t internal_t_format(char* out, uint32_t cap, const char* fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  const uint32_t written = priv_ra8_esp_hosted_fmt_vformat(out, cap, fmt, ap);
  va_end(ap);
  return written;
}

/**
 * @brief Assert that a formatted line equals the expected text.
 *
 * @details
 * Compares both the returned length and the buffer contents, because the two
 * are separate promises: the return value is what a caller uses to detect
 * truncation, and the buffer is what reaches the log.
 *
 * @param[in] want Expected NUL-terminated text.
 * @param[in] got Buffer the formatter wrote.
 * @param[in] written Value the formatter returned.
 *
 * @pre @p want and @p got are non-null NUL-terminated strings.
 * @pre @p written is the value returned for @p got.
 * @post Returns only when both the text and the length match.
 * @post The process has exited with status 1 on mismatch.
 *
 * @note Not thread-safe; writes to the shared stderr stream on failure.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_t_assert_text(const char* want, const char* got, uint32_t written)
{
  TEST_ASSERT(strcmp(want, got) == 0);
  TEST_ASSERT_EQ(strlen(want), written);
}

/**
 * @test internal_test_utoa_guards
 *
 * @brief `priv_ra8_esp_hosted_fmt_utoa` rejects a null buffer and an unusable base.
 *
 * @details
 * The two guards are separate decisions and are driven separately. The base
 * guard is the compound one; the null guard precedes it and is single.
 *
 * @par MC/DC:
 * Both decisions below are the entry guards of
 * `port/esp-hosted/src/ra8_esp_hosted_fmt.c@priv_ra8_esp_hosted_fmt_utoa`, taken
 * in the order the function evaluates them.
 * Decision A: `if (buf == nullptr)` (1 condition, 2 vectors)
 * - Vector A1: buf=null            -> true  (returns 0)
 * - Vector A2: buf=valid           -> false (falls through to the base guard)
 *
 * Decision B: `if ((base < k_radix_min) || (base > k_radix_hex))`
 * (2 conditions)
 * - Vector B1: base=10             -> false (control: both conditions false)
 * - Vector B2: base=1              -> true  (varies base-below-minimum only)
 * - Vector B3: base=17             -> true  (varies base-above-maximum only)
 * B1+B2 prove the lower bound independently affects the outcome; B1+B3 prove
 * the same for the upper bound. N+1 = 3 vectors for N=2: minimal MC/DC.
 *
 * @pre None.
 * @post No process state is mutated beyond the local scratch buffer.
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 * @pre Static fixture storage needed by this scenario is available.
 * @post The focused scenario leaves no unverified result.
 */
RA8_INTERNAL static void internal_test_utoa_guards(void)
{
  TEST_BEGIN("fmt utoa guards");
  char digits[k_t_fmt_digits_cap] = {};

  TEST_ASSERT_EQ(0, priv_ra8_esp_hosted_fmt_utoa(nullptr, 42U, 10U, false));

  TEST_ASSERT_EQ(0, priv_ra8_esp_hosted_fmt_utoa(digits, 42U, 1U, false));
  TEST_ASSERT_EQ(0, priv_ra8_esp_hosted_fmt_utoa(digits, 42U, 17U, false));

  TEST_ASSERT_EQ(2, priv_ra8_esp_hosted_fmt_utoa(digits, 42U, 10U, false));
  internal_t_assert_text("42", digits, 2U);

  /* The two admissible bounds themselves are accepted, not merely values
     inside them. */
  TEST_ASSERT_EQ(6, priv_ra8_esp_hosted_fmt_utoa(digits, 42U, 2U, false));
  internal_t_assert_text("101010", digits, 6U);
  TEST_ASSERT_EQ(2, priv_ra8_esp_hosted_fmt_utoa(digits, 42U, 16U, false));
  internal_t_assert_text("2a", digits, 2U);
  TEST_END("fmt utoa guards");
}

/**
 * @test internal_test_utoa_renders
 *
 * @brief `priv_ra8_esp_hosted_fmt_utoa` renders every supported shape correctly.
 *
 * @details
 * Covers zero (the single-digit early break), the multi-digit reversal path,
 * both hexadecimal cases, and the widest value the digit bound was sized for.
 * The reversal is what a naive division loop gets wrong, so a value whose
 * digits are not a palindrome is used deliberately.
 *
 * @par MC/DC:
 * Decision: `if (remaining == 0U) { break; }` inside the digit loop
 * (1 condition, 2 vectors)
 * - Vector 1: value=0     -> true on the first iteration (single `0` digit)
 * - Vector 2: value=1234  -> false for three iterations, then true
 *
 * Decision: `for (i < count / 2U)` reversal bound (1 condition, 2 vectors)
 * - Vector 1: count=1 (value=0)     -> bound is zero, loop body never runs
 * - Vector 2: count=4 (value=1234)  -> two swaps run; "4321" would be the
 *   unreversed answer, so the assertion distinguishes them.
 *
 * @pre None.
 * @post No process state is mutated beyond the local scratch buffer.
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 * @pre Static fixture storage needed by this scenario is available.
 * @post The focused scenario leaves no unverified result.
 */
RA8_INTERNAL static void internal_test_utoa_renders(void)
{
  TEST_BEGIN("fmt utoa rendering");
  char digits[k_t_fmt_digits_cap] = {};

  TEST_ASSERT_EQ(1, priv_ra8_esp_hosted_fmt_utoa(digits, 0U, 10U, false));
  internal_t_assert_text("0", digits, 1U);

  TEST_ASSERT_EQ(4, priv_ra8_esp_hosted_fmt_utoa(digits, 1234U, 10U, false));
  internal_t_assert_text("1234", digits, 4U);

  TEST_ASSERT_EQ(2, priv_ra8_esp_hosted_fmt_utoa(digits, 255U, 16U, false));
  internal_t_assert_text("ff", digits, 2U);
  TEST_ASSERT_EQ(2, priv_ra8_esp_hosted_fmt_utoa(digits, 255U, 16U, true));
  internal_t_assert_text("FF", digits, 2U);

  /* The widest decimal run the digit bound was sized for: UINT64_MAX is
     exactly k_ra8_esp_hosted_fmt_digits_max digits. */
  TEST_ASSERT_EQ(k_t_fmt_u64_dec_width,
                 priv_ra8_esp_hosted_fmt_utoa(digits, UINT64_MAX, 10U, false));
  internal_t_assert_text("18446744073709551615", digits, k_t_fmt_u64_dec_width);
  TEST_ASSERT_EQ(16, priv_ra8_esp_hosted_fmt_utoa(digits, UINT64_MAX, 16U, true));
  internal_t_assert_text("FFFFFFFFFFFFFFFF", digits, 16U);
  TEST_END("fmt utoa rendering");
}

/**
 * @test internal_test_parse_guards_and_flags
 *
 * @brief `priv_ra8_esp_hosted_fmt_parse` guards its pointers and reads both flags.
 *
 * @details
 * The flag scanner accepts any interleaving of the two flag characters and
 * stops at the first character that is neither, which is what lets the width
 * scanner run next without re-examining what the flag scanner consumed.
 *
 * @par MC/DC:
 * Decision A: `if ((after_percent == nullptr) || (out == nullptr))` in
 * `port/esp-hosted/src/ra8_esp_hosted_fmt.c@priv_ra8_esp_hosted_fmt_parse`
 * (2 conditions, 3 vectors)
 * - Vector A1: after_percent=valid, out=valid -> false (control)
 * - Vector A2: after_percent=null,  out=valid -> true  (varies the text only)
 * - Vector A3: after_percent=valid, out=null  -> true  (varies the output only)
 * A1+A2 prove the text pointer independently affects the outcome; A1+A3 prove
 * the same for the output pointer. N+1 = 3 vectors for N=2: minimal MC/DC.
 *
 * Decision B: the flag scanner's `text[used] == '0'` / `== '-'` chain
 * (2 single-condition decisions, 3 vectors each covered by)
 * - Vector B1: "d"   -> both false on the first character; no flag consumed
 * - Vector B2: "0d"  -> zero-pad true
 * - Vector B3: "-d"  -> zero-pad false, left-justify true
 * - Vector B4: "0-d" -> both flags, proving the scanner loops rather than
 *   accepting at most one flag
 *
 * @pre None.
 * @post No process state is mutated.
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 * @pre Static fixture storage needed by this scenario is available.
 * @post The focused scenario leaves no unverified result.
 */
RA8_INTERNAL static void internal_test_parse_guards_and_flags(void)
{
  TEST_BEGIN("fmt parse guards and flags");
  ra8_esp_hosted_fmt_spec_t spec = {};

  TEST_ASSERT(!priv_ra8_esp_hosted_fmt_parse(nullptr, &spec));
  TEST_ASSERT(!priv_ra8_esp_hosted_fmt_parse("d", nullptr));
  TEST_ASSERT(!priv_ra8_esp_hosted_fmt_parse(nullptr, nullptr));

  TEST_ASSERT(priv_ra8_esp_hosted_fmt_parse("d", &spec));
  TEST_ASSERT(!spec.zero_pad);
  TEST_ASSERT(!spec.left_justify);
  TEST_ASSERT_EQ('d', spec.conv);
  TEST_ASSERT_EQ(1, spec.consumed);

  TEST_ASSERT(priv_ra8_esp_hosted_fmt_parse("0d", &spec));
  TEST_ASSERT(spec.zero_pad);
  TEST_ASSERT(!spec.left_justify);
  TEST_ASSERT_EQ(2, spec.consumed);

  TEST_ASSERT(priv_ra8_esp_hosted_fmt_parse("-d", &spec));
  TEST_ASSERT(!spec.zero_pad);
  TEST_ASSERT(spec.left_justify);
  TEST_ASSERT_EQ(2, spec.consumed);

  TEST_ASSERT(priv_ra8_esp_hosted_fmt_parse("0-d", &spec));
  TEST_ASSERT(spec.zero_pad);
  TEST_ASSERT(spec.left_justify);
  TEST_ASSERT_EQ(3, spec.consumed);
  TEST_END("fmt parse guards and flags");
}

/**
 * @test internal_test_parse_width_and_length
 *
 * @brief The width scanner clamps, and every length modifier is recognised.
 *
 * @details
 * A width wider than the formatter's own line budget is clamped rather than
 * honoured, which is what stops one conversion from filling an arbitrary
 * buffer. The flag scanner's loop bound is driven here too, with a run of
 * flag characters longer than the specification cap.
 *
 * @par MC/DC:
 * Decision A: `if ((text[used] < '0') || (text[used] > '9'))` in the width
 * scanner,
 * `port/esp-hosted/src/ra8_esp_hosted_fmt.c@internal_parse_width`
 * (2 conditions, 3 vectors)
 * - Vector A1: next char is '8'  -> false (control: a digit; scanning continues)
 * - Vector A2: next char is '.'  -> true  (varies below-'0' only)
 * - Vector A3: next char is 'd'  -> true  (varies above-'9' only)
 * A1+A2 prove the lower comparison independently affects the outcome; A1+A3
 * prove the same for the upper one. N+1 = 3 vectors for N=2: minimal MC/DC.
 *
 * Decision B: `if (width > k_width_max) { width = k_width_max; }`
 * (1 condition, 2 vectors)
 * - Vector B1: "8d"   -> false; width is honoured as 8
 * - Vector B2: "999d" -> true; width is clamped to the 64-character budget
 *
 * Decision C: the length-modifier chain `'z'`, then `'l'`, then a second `'l'`
 * (3 single-condition decisions, 4 vectors)
 * - Vector C1: "d"   -> all false; the default `int` width stands
 * - Vector C2: "zu"  -> the `z` test is true
 * - Vector C3: "ld"  -> `z` false, first `l` true, second `l` false
 * - Vector C4: "lld" -> `z` false, first `l` true, second `l` true
 *
 * @pre None.
 * @post No process state is mutated.
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 * @pre Static fixture storage needed by this scenario is available.
 * @post The focused scenario leaves no unverified result.
 */
RA8_INTERNAL static void internal_test_parse_width_and_length(void)
{
  TEST_BEGIN("fmt parse width and length");
  ra8_esp_hosted_fmt_spec_t spec = {};

  TEST_ASSERT(priv_ra8_esp_hosted_fmt_parse("d", &spec));
  TEST_ASSERT_EQ(0, spec.width);
  TEST_ASSERT_EQ(k_ra8_esp_hosted_fmt_len_int, spec.len);

  TEST_ASSERT(priv_ra8_esp_hosted_fmt_parse("8d", &spec));
  TEST_ASSERT_EQ(8, spec.width);
  TEST_ASSERT_EQ(2, spec.consumed);

  TEST_ASSERT(priv_ra8_esp_hosted_fmt_parse("999d", &spec));
  TEST_ASSERT_EQ(k_ra8_esp_hosted_fmt_width_max, spec.width);
  TEST_ASSERT_EQ(4, spec.consumed);

  /* A character below '0' ends the width scan just as a character above '9'
     does; here it also makes the conversion unsupported, which is the
     documented answer for a specification this formatter does not emit. */
  TEST_ASSERT(!priv_ra8_esp_hosted_fmt_parse(".5d", &spec));

  TEST_ASSERT(priv_ra8_esp_hosted_fmt_parse("ld", &spec));
  TEST_ASSERT_EQ(k_ra8_esp_hosted_fmt_len_long, spec.len);
  TEST_ASSERT_EQ(2, spec.consumed);

  TEST_ASSERT(priv_ra8_esp_hosted_fmt_parse("lld", &spec));
  TEST_ASSERT_EQ(k_ra8_esp_hosted_fmt_len_llong, spec.len);
  TEST_ASSERT_EQ(3, spec.consumed);

  TEST_ASSERT(priv_ra8_esp_hosted_fmt_parse("zu", &spec));
  TEST_ASSERT_EQ(k_ra8_esp_hosted_fmt_len_size, spec.len);
  TEST_ASSERT_EQ(2, spec.consumed);

  /* Thirteen flag characters: the scanner's bound stops it at twelve, and the
     thirteenth is then read by the width scanner as the digit zero. */
  TEST_ASSERT(priv_ra8_esp_hosted_fmt_parse("0000000000000d", &spec));
  TEST_ASSERT(spec.zero_pad);
  TEST_ASSERT_EQ(0, spec.width);
  TEST_ASSERT_EQ(14, spec.consumed);
  TEST_END("fmt parse width and length");
}

/**
 * @test internal_test_parse_conversion_set
 *
 * @brief Exactly the documented conversion set is accepted; nothing else is.
 *
 * @details
 * The supported-conversion test is a nine-term disjunction, and every term
 * has to be shown to matter on its own -- otherwise a mis-typed term would
 * silently accept or reject one conversion while the rest of the suite stayed
 * green.
 *
 * @par MC/DC:
 * Decision: `(conv=='d') || (conv=='i') || (conv=='u') || (conv=='x') ||
 * (conv=='X') || (conv=='c') || (conv=='s') || (conv=='p') || (conv=='%')` in
 * `port/esp-hosted/src/ra8_esp_hosted_fmt.c@internal_is_supported`
 * (9 conditions, 10 vectors)
 * - Vector 1:  'd' -> term 1 true, every earlier term absent   -> decision true
 * - Vector 2:  'i' -> terms 1 false, term 2 true               -> decision true
 * - Vector 3:  'u' -> terms 1-2 false, term 3 true             -> decision true
 * - Vector 4:  'x' -> terms 1-3 false, term 4 true             -> decision true
 * - Vector 5:  'X' -> terms 1-4 false, term 5 true             -> decision true
 * - Vector 6:  'c' -> terms 1-5 false, term 6 true             -> decision true
 * - Vector 7:  's' -> terms 1-6 false, term 7 true             -> decision true
 * - Vector 8:  'p' -> terms 1-7 false, term 8 true             -> decision true
 * - Vector 9:  '%' -> terms 1-8 false, term 9 true             -> decision true
 * - Vector 10: 'f' -> every term false                         -> decision false
 * Each of vectors 1..9 pairs with vector 10 (all conditions false) to prove
 * that its term independently affects the outcome, since in a disjunction the
 * only way to isolate one term is to hold every other term false. N+1 = 10
 * vectors for N=9: minimal MC/DC.
 *
 * @pre None.
 * @post No process state is mutated.
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 * @pre Static fixture storage needed by this scenario is available.
 * @post The focused scenario leaves no unverified result.
 */
RA8_INTERNAL static void internal_test_parse_conversion_set(void)
{
  TEST_BEGIN("fmt parse conversion set");
  ra8_esp_hosted_fmt_spec_t spec        = {};
  static const char* const  supported[] = {"d", "i", "u", "x", "X", "c", "s", "p", "%"};
  const size_t              count       = sizeof(supported) / sizeof(supported[0]);

  for (size_t i = 0U; i < count; i++) {
    TEST_ASSERT(priv_ra8_esp_hosted_fmt_parse(supported[i], &spec));
    TEST_ASSERT_EQ(supported[i][0], spec.conv);
    TEST_ASSERT_EQ(1, spec.consumed);
  }

  /* Two conversions real printf implements and this one deliberately does
     not: floating point, and the write-back conversion. */
  TEST_ASSERT(!priv_ra8_esp_hosted_fmt_parse("f", &spec));
  TEST_ASSERT_EQ(0, spec.conv);
  TEST_ASSERT_EQ(0, spec.consumed);
  TEST_ASSERT(!priv_ra8_esp_hosted_fmt_parse("n", &spec));

  /* A specification that runs off the end of the string is not a
     specification: the terminator is not a supported conversion. */
  TEST_ASSERT(!priv_ra8_esp_hosted_fmt_parse("", &spec));
  TEST_ASSERT(!priv_ra8_esp_hosted_fmt_parse("08", &spec));
  TEST_END("fmt parse conversion set");
}

/**
 * @test internal_test_vformat_guards
 *
 * @brief `priv_ra8_esp_hosted_fmt_vformat` guards all three of its inputs.
 *
 * @details
 * The outer guard decides whether anything is formatted at all; the inner one
 * decides whether the buffer can still be terminated on the way out. They are
 * separate decisions over overlapping conditions, and both are driven here.
 *
 * @par MC/DC:
 * Decision A: `if ((out == nullptr) || (cap == 0U) || (fmt == nullptr))` in
 * `port/esp-hosted/src/ra8_esp_hosted_fmt.c@priv_ra8_esp_hosted_fmt_vformat`
 * (3 conditions, 4 vectors)
 * - Vector A1: out=valid, cap=32, fmt="x"    -> false (control: all false)
 * - Vector A2: out=null,  cap=32, fmt="x"    -> true  (varies the buffer only)
 * - Vector A3: out=valid, cap=0,  fmt="x"    -> true  (varies the capacity only)
 * - Vector A4: out=valid, cap=32, fmt=null   -> true  (varies the format only)
 * A1+A2, A1+A3 and A1+A4 each prove one condition independently affects the
 * outcome. N+1 = 4 vectors for N=3: minimal MC/DC.
 *
 * Decision B: `if ((out != nullptr) && (cap != 0U))` -- the terminate-anyway
 * arm, reachable only when decision A was true (2 conditions, 3 vectors)
 * - Vector B1: out=valid, cap=32 (with fmt=null) -> true  (control: both true)
 * - Vector B2: out=null,  cap=32                 -> false (varies the buffer)
 * - Vector B3: out=valid, cap=0                  -> false (varies the capacity)
 * B1+B2 prove the buffer condition independently affects the outcome; B1+B3
 * prove the same for the capacity. N+1 = 3 vectors for N=2: minimal MC/DC.
 * B1 is observed by the buffer being terminated, B3 by the sentinel byte in a
 * zero-capacity buffer surviving untouched.
 *
 * @pre None.
 * @post No process state is mutated beyond the local buffers.
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 * @pre Static fixture storage needed by this scenario is available.
 * @post The focused scenario leaves no unverified result.
 */
RA8_INTERNAL static void internal_test_vformat_guards(void)
{
  TEST_BEGIN("fmt vformat guards");
  char line[k_t_fmt_line_cap] = {};

  TEST_ASSERT_EQ(1, internal_t_format(line, (uint32_t)sizeof(line), "x"));
  internal_t_assert_text("x", line, 1U);

  TEST_ASSERT_EQ(0, internal_t_format(nullptr, (uint32_t)sizeof(line), "x"));

  /* A zero capacity must leave the buffer entirely alone -- there is not even
     room for a terminator, so writing one would overrun. */
  line[0] = 'Z';
  TEST_ASSERT_EQ(0, internal_t_format(line, 0U, "x"));
  TEST_ASSERT_EQ('Z', line[0]);

  /* A null format still terminates the buffer, so the caller never reads
     whatever was there before. */
  line[0] = 'Z';
  TEST_ASSERT_EQ(0, internal_t_format(line, (uint32_t)sizeof(line), nullptr));
  TEST_ASSERT_EQ(0, line[0]);
  TEST_END("fmt vformat guards");
}

/**
 * @test internal_test_vformat_conversions
 *
 * @brief Every supported conversion round-trips to the expected text.
 *
 * @details
 * One assertion per conversion, at the argument width each modifier selects.
 * The widths matter more than the values: reading an argument at the wrong
 * width does not merely mis-print one field, it misaligns every field after
 * it, so each length modifier is exercised with a value that would not
 * survive a narrower read.
 *
 * @par MC/DC:
 * Decision A: `if ((spec->conv == 'd') || (spec->conv == 'i'))` in the
 * conversion dispatcher,
 * `port/esp-hosted/src/ra8_esp_hosted_fmt.c@internal_emit_conv`
 * (2 conditions, 3 vectors)
 * - Vector A1: conv='d' -> true  (varies the first condition; second not evaluated)
 * - Vector A2: conv='i' -> true  (first false, second true)
 * - Vector A3: conv='u' -> false (control: both conditions false)
 * A1+A3 prove the `d` test independently affects the outcome; A2+A3 prove the
 * same for the `i` test. N+1 = 3 vectors for N=2: minimal MC/DC.
 *
 * Decision B: `const bool negative = (value < 0)` (1 condition, 2 vectors)
 * - Vector B1: value=::k_t_fmt_int_sample (42)        -> false; no sign is emitted
 * - Vector B2: value=-::k_t_fmt_small_int_sample (-7) -> true;  a leading minus
 *   is emitted
 *
 * Decision C: `text == nullptr ? "(null)" : text` for `%s`
 * (1 condition, 2 vectors)
 * - Vector C1: a real string -> false; the string is emitted
 * - Vector C2: a null string -> true;  the placeholder is emitted
 *
 * Decision D: `is_pointer` / `upper` / base selection, each a single condition
 * - `%p` true / `%x` false; `%X` true / `%x` false; `%u` decimal / `%x` hex.
 *
 * @pre None.
 * @post No process state is mutated beyond the local buffers.
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 * @pre Static fixture storage needed by this scenario is available.
 * @post The focused scenario leaves no unverified result.
 */
RA8_INTERNAL static void internal_test_vformat_conversions(void)
{
  TEST_BEGIN("fmt vformat conversions");
  char line[k_t_fmt_line_cap]     = {};
  char digits[k_t_fmt_digits_cap] = {};

  internal_t_assert_text("hello", line, internal_t_format(line, (uint32_t)sizeof(line), "hello"));

  internal_t_assert_text("42",
                         line,
                         internal_t_format(line, (uint32_t)sizeof(line), "%d", k_t_fmt_int_sample));
  internal_t_assert_text(
    "-7",
    line,
    internal_t_format(line, (uint32_t)sizeof(line), "%i", -k_t_fmt_small_int_sample));
  internal_t_assert_text("4294967295",
                         line,
                         internal_t_format(line, (uint32_t)sizeof(line), "%u", k_t_fmt_u32_max));
  internal_t_assert_text("deadbeef",
                         line,
                         internal_t_format(line, (uint32_t)sizeof(line), "%x", k_t_fmt_hex_sample));
  internal_t_assert_text("DEADBEEF",
                         line,
                         internal_t_format(line, (uint32_t)sizeof(line), "%X", k_t_fmt_hex_sample));
  internal_t_assert_text("Z", line, internal_t_format(line, (uint32_t)sizeof(line), "%c", 'Z'));
  internal_t_assert_text("abc", line, internal_t_format(line, (uint32_t)sizeof(line), "%s", "abc"));
  internal_t_assert_text(
    "(null)",
    line,
    internal_t_format(line, (uint32_t)sizeof(line), "%s", (const char*)nullptr));
  internal_t_assert_text("%", line, internal_t_format(line, (uint32_t)sizeof(line), "%%"));

  /* Length modifiers, each with a value that a narrower read would truncate
     or a wider read would misalign. */
  internal_t_assert_text(
    "4294967296",
    line,
    internal_t_format(line, (uint32_t)sizeof(line), "%lu", (unsigned long)k_t_fmt_i64_above_u32));
  internal_t_assert_text(
    "-4294967296",
    line,
    internal_t_format(line, (uint32_t)sizeof(line), "%lld", (long long)k_t_fmt_i64_below_neg_u32));
  internal_t_assert_text(
    "99",
    line,
    internal_t_format(line, (uint32_t)sizeof(line), "%zu", (size_t)k_t_fmt_size_sample));

  /* The pointer conversion renders an address, so assert the property that
     matters -- that it agrees with the digit renderer on the same value --
     rather than pinning a host address. */
  const uint64_t address = (uint64_t)(uintptr_t)line;
  const uint8_t  n       = priv_ra8_esp_hosted_fmt_utoa(digits, address, 16U, false);
  TEST_ASSERT(n > 0U);
  internal_t_assert_text(digits,
                         line,
                         internal_t_format(line, (uint32_t)sizeof(line), "%p", (void*)line));
  TEST_END("fmt vformat conversions");
}

/**
 * @test internal_test_vformat_negative_extremes
 *
 * @brief The signed path renders `INT_MIN` without overflowing the negation.
 *
 * @details
 * Negating the most-negative two's-complement value is undefined behaviour,
 * so the formatter computes the magnitude as `(-(value + 1)) + 1` in unsigned
 * arithmetic. That is exactly the case a naive `-value` gets wrong, and it is
 * silent when it does -- the wrong digits are still plausible digits.
 *
 * @par MC/DC:
 * Decision: `const bool negative = (value < 0)` on the magnitude path
 * (1 condition, 2 vectors)
 * - Vector 1: value=INT_MIN  -> true; magnitude is 2147483648, not 0 or a
 *   wrapped positive
 * - Vector 2: value=INT_MAX  -> false; the unsigned cast is taken directly
 * Both extremes of the same width are asserted so an off-by-one in the
 * magnitude arithmetic cannot pass.
 *
 * @pre None.
 * @post No process state is mutated beyond the local buffer.
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 * @pre Static fixture storage needed by this scenario is available.
 * @post The focused scenario leaves no unverified result.
 */
RA8_INTERNAL static void internal_test_vformat_negative_extremes(void)
{
  TEST_BEGIN("fmt vformat negative extremes");
  char line[k_t_fmt_line_cap] = {};

  internal_t_assert_text("-2147483648",
                         line,
                         internal_t_format(line, (uint32_t)sizeof(line), "%d", INT_MIN));
  internal_t_assert_text("2147483647",
                         line,
                         internal_t_format(line, (uint32_t)sizeof(line), "%d", INT_MAX));
  internal_t_assert_text("-1", line, internal_t_format(line, (uint32_t)sizeof(line), "%d", -1));
  internal_t_assert_text("0", line, internal_t_format(line, (uint32_t)sizeof(line), "%d", 0));
  TEST_END("fmt vformat negative extremes");
}

/**
 * @test internal_test_vformat_padding
 *
 * @brief Width, zero padding and left justification combine as C's rules say.
 *
 * @details
 * Zero padding applies only to a right-justified field; a left-justified one
 * pads with spaces regardless, because leading zeros on the right of a number
 * would change its value. Both flags together therefore select spaces, which
 * is the case a hand-rolled emitter most often gets wrong.
 *
 * @par MC/DC:
 * Decision A: `const char pad = (spec->zero_pad && !spec->left_justify) ? '0' : ' '`
 * in `port/esp-hosted/src/ra8_esp_hosted_fmt.c@internal_emit_token`
 * (2 conditions, 3 vectors)
 * - Vector A1: zero_pad=true,  left_justify=false -> true  (control: both true
 *   as evaluated, since the second condition is the negation)
 * - Vector A2: zero_pad=false, left_justify=false -> false (varies zero_pad only)
 * - Vector A3: zero_pad=true,  left_justify=true  -> false (varies left_justify only)
 * A1+A2 prove zero_pad independently affects the pad character; A1+A3 prove
 * the same for left_justify. N+1 = 3 vectors for N=2: minimal MC/DC.
 *
 * Decision B: `(spec->width > token_len) ? (width - token_len) : 0`
 * (1 condition, 2 vectors)
 * - Vector B1: width=5, token="42" from ::k_t_fmt_int_sample -> true;  three
 *   pad characters
 * - Vector B2: width=1, token="1234" from ::k_t_fmt_wide_sample -> false; no
 *   padding, and the token is not truncated to the width
 *
 * Decision C: `if (!spec->left_justify)` / `if (spec->left_justify)`
 * (1 condition, 2 vectors)
 * - Vector C1: left_justify=false -> padding precedes the token
 * - Vector C2: left_justify=true  -> padding follows the token
 *
 * @pre None.
 * @post No process state is mutated beyond the local buffer.
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 * @pre Static fixture storage needed by this scenario is available.
 * @post The focused scenario leaves no unverified result.
 */
RA8_INTERNAL static void internal_test_vformat_padding(void)
{
  TEST_BEGIN("fmt vformat padding");
  char line[k_t_fmt_line_cap] = {};

  internal_t_assert_text(
    "   42",
    line,
    internal_t_format(line, (uint32_t)sizeof(line), "%5d", k_t_fmt_int_sample));
  internal_t_assert_text(
    "00042",
    line,
    internal_t_format(line, (uint32_t)sizeof(line), "%05d", k_t_fmt_int_sample));
  internal_t_assert_text(
    "42   ",
    line,
    internal_t_format(line, (uint32_t)sizeof(line), "%-5d", k_t_fmt_int_sample));
  internal_t_assert_text(
    "42   ",
    line,
    internal_t_format(line, (uint32_t)sizeof(line), "%-05d", k_t_fmt_int_sample));

  /* A width narrower than the token neither truncates nor pads. */
  internal_t_assert_text(
    "1234",
    line,
    internal_t_format(line, (uint32_t)sizeof(line), "%1d", k_t_fmt_wide_sample));

  /* Padding is applied to strings and characters, not only to numbers. */
  internal_t_assert_text("      ab",
                         line,
                         internal_t_format(line, (uint32_t)sizeof(line), "%8s", "ab"));
  internal_t_assert_text("ab      ",
                         line,
                         internal_t_format(line, (uint32_t)sizeof(line), "%-8s", "ab"));
  internal_t_assert_text("   Z", line, internal_t_format(line, (uint32_t)sizeof(line), "%4c", 'Z'));
  TEST_END("fmt vformat padding");
}

/**
 * @test internal_test_vformat_truncation
 *
 * @brief A buffer too small for the output truncates and stays terminated.
 *
 * @details
 * The contract is that the return value is what was written, never what would
 * have been written, and that the buffer is always terminated when the
 * capacity is non-zero. Together those let a caller detect truncation without
 * a second pass and without ever reading past the end.
 *
 * @par MC/DC:
 * Decision: `if ((cur->len + 1U) >= cur->cap) { return; }` in the cursor,
 * `port/esp-hosted/src/ra8_esp_hosted_fmt.c@internal_put`
 * (1 condition, 2 vectors)
 * - Vector 1: len=0, cap=4  -> false; the character is stored
 * - Vector 2: len=3, cap=4  -> true;  the character is dropped, the reserved
 *   terminator byte survives
 *
 * The cursor and its output pointer are established by the validated
 * formatter entry point, so ::internal_put relies on that invariant and has
 * no second, unreachable pointer decision.
 *
 * @pre None.
 * @post No process state is mutated beyond the local buffer.
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 * @pre Static fixture storage needed by this scenario is available.
 * @post The focused scenario leaves no unverified result.
 */
RA8_INTERNAL static void internal_test_vformat_truncation(void)
{
  TEST_BEGIN("fmt vformat truncation");
  char           small[k_t_fmt_digits_cap] = {};
  const uint32_t cap                       = 4U;

  const uint32_t written = internal_t_format(small, cap, "abcdef");
  TEST_ASSERT_EQ((cap - 1U), written);
  TEST_ASSERT_EQ(0, small[cap - 1U]);
  internal_t_assert_text("abc", small, written);

  /* Truncation mid-conversion terminates just the same. */
  const uint32_t n = internal_t_format(small, cap, "%d", 123456);
  TEST_ASSERT_EQ((cap - 1U), n);
  internal_t_assert_text("123", small, n);

  /* A single-byte buffer holds nothing but the terminator. */
  small[0] = 'Z';
  TEST_ASSERT_EQ(0, internal_t_format(small, 1U, "abc"));
  TEST_ASSERT_EQ(0, small[0]);
  TEST_END("fmt vformat truncation");
}

/**
 * @test internal_test_vformat_unsupported_keeps_arguments_aligned
 *
 * @brief An unsupported conversion is copied through and consumes no argument.
 *
 * @details
 * This is the property the whole "copy through verbatim" design exists for.
 * If an unhandled specifier consumed an argument, every later conversion
 * would read the wrong one -- and the result would still look like a
 * plausible log line, which is why the alignment is asserted explicitly
 * rather than inferred from the unsupported field alone.
 *
 * @par MC/DC:
 * Decision: `if (!priv_ra8_esp_hosted_fmt_parse(&fmt[pos], &spec))` in the driver
 * (1 condition, 2 vectors)
 * - Vector 1: "%d"  -> parse succeeds; the conversion is expanded and one
 *   argument is consumed
 * - Vector 2: "%f"  -> parse fails; a bare per-cent is emitted, the position
 *   is not advanced past it, and no argument is consumed
 * The second vector is paired with a following `%d` whose value proves the
 * argument list did not advance.
 *
 * @pre None.
 * @post No process state is mutated beyond the local buffer.
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 * @pre Static fixture storage needed by this scenario is available.
 * @post The focused scenario leaves no unverified result.
 */
RA8_INTERNAL static void internal_test_vformat_unsupported_keeps_arguments_aligned(void)
{
  TEST_BEGIN("fmt vformat unsupported conversion");
  char line[k_t_fmt_line_cap] = {};

  /* The k_t_fmt_int_sample argument belongs to the %d. If the %f had consumed
     it, the %d would print whatever followed on the argument list instead. */
  internal_t_assert_text(
    "%f|42",
    line,
    internal_t_format(line, (uint32_t)sizeof(line), "%f|%d", k_t_fmt_int_sample));
  internal_t_assert_text(
    "%n|7",
    line,
    internal_t_format(line, (uint32_t)sizeof(line), "%n|%d", k_t_fmt_small_int_sample));

  /* A trailing per-cent with nothing after it is not a specification either. */
  internal_t_assert_text("end%", line, internal_t_format(line, (uint32_t)sizeof(line), "end%"));
  TEST_END("fmt vformat unsupported conversion");
}

/**
 * @test internal_test_vformat_bounded_string
 *
 * @brief A string longer than the measurement bound is truncated, not chased.
 *
 * @details
 * The string measurement is bounded rather than a `strlen`, so a
 * co-processor-supplied buffer that lost its terminator cannot walk off the
 * end of memory. The consequence a caller sees is a visibly truncated field,
 * which this test pins: a 100-character argument renders as exactly the
 * 64-character bound.
 *
 * @par MC/DC:
 * Decision: `if (text[len] == '\0') { break; }` inside the bounded
 * measurement (1 condition, 2 vectors)
 * - Vector 1: a 3-character string  -> true at index 3; the loop bound is
 *   never reached
 * - Vector 2: a 100-character string -> false for all 64 iterations; the loop
 *   bound stops it and the reported length is the bound itself
 *
 * @pre None.
 * @post No process state is mutated beyond the local buffers.
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 * @pre Static fixture storage needed by this scenario is available.
 * @post The focused scenario leaves no unverified result.
 */
RA8_INTERNAL static void internal_test_vformat_bounded_string(void)
{
  TEST_BEGIN("fmt vformat bounded string");
  char line[k_t_fmt_line_cap]           = {};
  char long_text[k_t_fmt_long_str + 1U] = {};

  for (uint32_t i = 0U; i < (uint32_t)k_t_fmt_long_str; i++) {
    long_text[i] = 'a';
  }

  const uint32_t written = internal_t_format(line, (uint32_t)sizeof(line), "%s", long_text);
  TEST_ASSERT_EQ(k_ra8_esp_hosted_fmt_width_max, written);
  TEST_ASSERT_EQ(k_ra8_esp_hosted_fmt_width_max, strlen(line));
  TEST_ASSERT_EQ('a', line[0]);
  TEST_END("fmt vformat bounded string");
}

int main(void)
{
  internal_test_utoa_guards();
  internal_test_utoa_renders();
  internal_test_parse_guards_and_flags();
  internal_test_parse_width_and_length();
  internal_test_parse_conversion_set();
  internal_test_vformat_guards();
  internal_test_vformat_conversions();
  internal_test_vformat_negative_extremes();
  internal_test_vformat_padding();
  internal_test_vformat_truncation();
  internal_test_vformat_unsupported_keeps_arguments_aligned();
  internal_test_vformat_bounded_string();
  return 0;
}
