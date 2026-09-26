/**
 * @file test_reflow_numeric_sanitize.c
 * @brief Unit tests for the numeric-character-reference sanitise rule in
 *        apps/shared_libs/reflow/src/reflow_tokenize_lex.c
 *
 * @details
 * Two halves. The first drives priv_reflow_tok_is_xml_char() across both
 * sides of every boundary in the XML 1.0 `Char` production, so a widened
 * or narrowed predicate fails here rather than in a page of rendered
 * text. The second drives priv_reflow_tok_decode_entity() over whole
 * `&#...;` sequences: a legal value passes through unchanged, an illegal
 * one is consumed whole and decodes to U+FFFD, a digit run long enough to
 * overflow a 32-bit accumulator saturates instead of wrapping, and the
 * malformed *shapes* still fail open so the caller emits the literal '&'.
 * See issue #686 part 3 and ADR-0010.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "reflow.h"
#include "reflow_tokenize_internal.h"
#include "unity_minimal.h"

/**
 * @enum t_sanitize_t
 * @brief Boundary code points of the XML 1.0 `Char` production.
 */
typedef enum : uint32_t {
  k_t_nul          = 0x00U,       /**< Excluded C0 control.              */
  k_t_ctl_last     = 0x1FU,       /**< Last excluded C0 control.         */
  k_t_space        = 0x20U,       /**< First admitted printable.         */
  k_t_bmp_pre_surr = 0xD7FFU,     /**< Last admitted before surrogates.  */
  k_t_surr_first   = 0xD800U,     /**< First surrogate.                  */
  k_t_surr_mid     = 0xDC00U,     /**< A low surrogate.                  */
  k_t_surr_last    = 0xDFFFU,     /**< Last surrogate.                   */
  k_t_pua_first    = 0xE000U,     /**< First admitted after surrogates.  */
  k_t_replace      = 0xFFFDU,     /**< U+FFFD, admitted and the output.  */
  k_t_nonchar_lo   = 0xFFFEU,     /**< Excluded non-character.           */
  k_t_nonchar_hi   = 0xFFFFU,     /**< Excluded non-character.           */
  k_t_astral_first = 0x10000U,    /**< First supplementary code point.   */
  k_t_cp_max       = 0x10FFFFU,   /**< Highest valid code point.         */
  k_t_over_max     = 0x110000U,   /**< One past the last code point.     */
  k_t_all_ones     = 0xFFFFFFFFU, /**< A value 8 hex digits can reach.   */
} t_sanitize_t;

/**
 * @brief Assert the admitted side of the `Char` production.
 *
 * @details Tab, line feed and carriage return are the only C0 controls in
 * the production; the three admitted ranges are checked at both of their
 * own boundaries.
 * @pre The tokenizer is linked into the test binary.
 * @post All assertions for the scenario have passed before this function returns.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_xml_char_admits(void)
{
  TEST_BEGIN("xml char: admitted values");
  TEST_ASSERT(priv_reflow_tok_is_xml_char((uint32_t)k_priv_uc_tab));
  TEST_ASSERT(priv_reflow_tok_is_xml_char((uint32_t)k_priv_uc_lf));
  TEST_ASSERT(priv_reflow_tok_is_xml_char((uint32_t)k_priv_uc_cr));
  TEST_ASSERT(priv_reflow_tok_is_xml_char((uint32_t)k_t_space));
  TEST_ASSERT(priv_reflow_tok_is_xml_char((uint32_t)'A'));
  TEST_ASSERT(priv_reflow_tok_is_xml_char((uint32_t)k_t_bmp_pre_surr));
  TEST_ASSERT(priv_reflow_tok_is_xml_char((uint32_t)k_t_pua_first));
  TEST_ASSERT(priv_reflow_tok_is_xml_char((uint32_t)k_t_replace));
  TEST_ASSERT(priv_reflow_tok_is_xml_char((uint32_t)k_t_astral_first));
  TEST_ASSERT(priv_reflow_tok_is_xml_char((uint32_t)k_t_cp_max));
  TEST_END();
}

/**
 * @brief Assert the excluded side of the `Char` production.
 *
 * @details Every C0 control other than the three named ones, the whole
 * surrogate block at both ends and in the middle, both BMP
 * non-characters, and everything above U+10FFFF.
 * @pre The tokenizer is linked into the test binary.
 * @post All assertions for the scenario have passed before this function returns.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_xml_char_excludes(void)
{
  TEST_BEGIN("xml char: excluded values");
  TEST_ASSERT(!priv_reflow_tok_is_xml_char((uint32_t)k_t_nul));
  for (uint32_t cp = 0x01U; cp <= (uint32_t)k_t_ctl_last; ++cp) {
    if ((cp == (uint32_t)k_priv_uc_tab) || (cp == (uint32_t)k_priv_uc_lf) ||
        (cp == (uint32_t)k_priv_uc_cr)) {
      continue;
    }
    TEST_ASSERT(!priv_reflow_tok_is_xml_char(cp));
  }
  TEST_ASSERT(!priv_reflow_tok_is_xml_char((uint32_t)k_t_surr_first));
  TEST_ASSERT(!priv_reflow_tok_is_xml_char((uint32_t)k_t_surr_mid));
  TEST_ASSERT(!priv_reflow_tok_is_xml_char((uint32_t)k_t_surr_last));
  TEST_ASSERT(!priv_reflow_tok_is_xml_char((uint32_t)k_t_nonchar_lo));
  TEST_ASSERT(!priv_reflow_tok_is_xml_char((uint32_t)k_t_nonchar_hi));
  TEST_ASSERT(!priv_reflow_tok_is_xml_char((uint32_t)k_t_over_max));
  TEST_ASSERT(!priv_reflow_tok_is_xml_char((uint32_t)k_t_all_ones));
  TEST_END();
}

/**
 * @brief Decode one whole `&...;` sequence and check code point and length.
 *
 * @param[in] src      NUL-terminated reference text.
 * @param[in] want_cp  Expected decoded code point.
 * @param[in] want_len Expected bytes consumed.
 * @pre `src` is non-null.
 * @post All assertions for the scenario have passed before this function returns.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_expect_cp(const char* src, uint32_t want_cp, size_t want_len)
{
  uint32_t cp   = 0U;
  size_t   used = 0U;
  TEST_ASSERT(priv_reflow_tok_decode_entity(src, strlen(src), &cp, &used));
  TEST_ASSERT_EQ((uint32_t)want_cp, cp);
  TEST_ASSERT_EQ((size_t)want_len, used);
}

/**
 * @brief Assert that a legal numeric reference is unchanged by the rule.
 *
 * @details Decimal and hexadecimal forms in both letter cases, the three
 * admitted C0 controls, an explicit U+FFFD, and the edges of the ranges
 * either side of the surrogate block.
 * @pre The tokenizer is linked into the test binary.
 * @post All assertions for the scenario have passed before this function returns.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_numeric_legal(void)
{
  TEST_BEGIN("numeric reference: legal values pass through");
  internal_expect_cp("&#65;", (uint32_t)'A', 5U);
  internal_expect_cp("&#x41;", (uint32_t)'A', 6U);
  internal_expect_cp("&#X41;", (uint32_t)'A', 6U);
  internal_expect_cp("&#x2014;", 0x2014U, 8U); /* em dash */
  internal_expect_cp("&#9;", (uint32_t)k_priv_uc_tab, 4U);
  internal_expect_cp("&#10;", (uint32_t)k_priv_uc_lf, 5U);
  internal_expect_cp("&#13;", (uint32_t)k_priv_uc_cr, 5U);
  internal_expect_cp("&#x7F;", 0x7FU, 6U); /* XML 1.0 admits DEL */
  internal_expect_cp("&#xD7FF;", (uint32_t)k_t_bmp_pre_surr, 8U);
  internal_expect_cp("&#xE000;", (uint32_t)k_t_pua_first, 8U);
  internal_expect_cp("&#xFFFD;", (uint32_t)k_t_replace, 8U);
  TEST_END();
}

/**
 * @brief Assert that an illegal value is consumed whole and becomes U+FFFD.
 *
 * @details The consumed length is asserted with the code point, because
 * the load-bearing half of this rule is that the walk resumes *past* the
 * reference rather than rescanning its digits as text.
 * @pre The tokenizer is linked into the test binary.
 * @post All assertions for the scenario have passed before this function returns.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_numeric_sanitized(void)
{
  TEST_BEGIN("numeric reference: illegal values become U+FFFD");
  const uint32_t rep = (uint32_t)k_priv_uc_replace;
  internal_expect_cp("&#0;", rep, 4U);     /* NUL */
  internal_expect_cp("&#x0;", rep, 5U);    /* NUL, hex form */
  internal_expect_cp("&#1;", rep, 4U);     /* C0 control */
  internal_expect_cp("&#x1F;", rep, 6U);   /* last C0 control */
  internal_expect_cp("&#xD800;", rep, 8U); /* first surrogate */
  internal_expect_cp("&#55296;", rep, 8U); /* same, decimal */
  internal_expect_cp("&#xDFFF;", rep, 8U); /* last surrogate */
  internal_expect_cp("&#xFFFE;", rep, 8U); /* non-character */
  internal_expect_cp("&#xFFFF;", rep, 8U); /* non-character */
  internal_expect_cp("&#x110000;", rep, 10U);
  TEST_END();
}

/**
 * @brief Assert the accumulator saturates rather than wrapping.
 *
 * @details `&#x` plus eight hex digits plus ';' is exactly the twelve-byte
 * scan window, which is the widest value the tokenizer can be handed. An
 * unsaturated `cp * 16 + d` would wrap through the valid range and could
 * hand back a plausible-looking character.
 * @pre The tokenizer is linked into the test binary.
 * @post All assertions for the scenario have passed before this function returns.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_numeric_overflow(void)
{
  TEST_BEGIN("numeric reference: accumulator saturates");
  const uint32_t rep = (uint32_t)k_priv_uc_replace;
  internal_expect_cp("&#xFFFFFFFF;", rep, 12U);
  internal_expect_cp("&#x10000041;", rep, 12U); /* would wrap to 'A' unguarded */
  internal_expect_cp("&#99999999;", rep, 11U);
  TEST_END();
}

/**
 * @brief Assert malformed shapes still fail open, and names are untouched.
 *
 * @details The sanitise rule applies to a *value*, so every shape the
 * decoder rejected before must still be rejected, and the named-reference
 * path must be unaffected.
 * @pre The tokenizer is linked into the test binary.
 * @post All assertions for the scenario have passed before this function returns.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_shape_unchanged(void)
{
  TEST_BEGIN("numeric reference: malformed shapes still fail open");
  static const char* const k_bad[] = {
    "&#;",
    "&#x;",
    "&#12",
    "&#zz;",
    "&#x1g;",
    "&#",
  };
  for (size_t i = 0U; i < (sizeof(k_bad) / sizeof(k_bad[0])); ++i) {
    uint32_t cp   = 0U;
    size_t   used = 0U;
    TEST_ASSERT(!priv_reflow_tok_decode_entity(k_bad[i], strlen(k_bad[i]), &cp, &used));
  }
  internal_expect_cp("&amp;", (uint32_t)'&', 5U);
  internal_expect_cp("&nbsp;", 0x00A0U, 6U);
  internal_expect_cp("&hellip;", 0x2026U, 8U);
  TEST_END();
}

/**
 * @brief Test entry point.
 *
 * @return 0 when every assertion passed.
 * @retval 0 Success.
 * @pre None.
 * @pre None.
 * @post Test results have been reported.
 * @post No global state is left modified.
 * @note Runs every scenario in this file.
 * @since 0.1.0
 */
int main(void)
{
  internal_test_xml_char_admits();
  internal_test_xml_char_excludes();
  internal_test_numeric_legal();
  internal_test_numeric_sanitized();
  internal_test_numeric_overflow();
  internal_test_shape_unchanged();
  return TEST_SUMMARY();
}
