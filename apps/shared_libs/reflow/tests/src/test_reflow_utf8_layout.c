/**
 * @file test_reflow_utf8_layout.c
 * @brief UTF-8 decode correctness in the v1 layout walk (#686 Tier 1).
 *
 * @details
 * Two halves. The first drives ::priv_reflow_tok_utf8_decode directly over
 * the well-formed and malformed shapes the byte pool can hold, pinning the
 * substitution contract: a scalar value or U+FFFD, never a surrogate, and a
 * consumed length always in 1..4 so a caller that advances by it can neither
 * stall nor overrun. The second lays real markup through the public API and
 * asserts the laid-out glyph code points, which is the behaviour #686 is
 * actually about: before this change
 * apps/shared_libs/reflow/src/reflow_layout.c emitted one glyph per BYTE, so
 * every non-ASCII character in an EPUB became two to four tofu boxes.
 *
 * The round-trip test ties the decoder to the encoder that already existed
 * (::priv_reflow_tok_utf8_encode), so the pair cannot drift apart.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "reflow_layout_test_util.h"
#include "reflow_tokenize_internal.h"
#include "unity_minimal.h"

/** @brief Fixture code points and expected lengths (no magic numbers). */
enum : uint32_t {
  k_cp_dollar     = 0x24U,    /**< U+0024, 1-byte.                        */
  k_cp_cent       = 0xA2U,    /**< U+00A2, 2-byte.                        */
  k_cp_eacute     = 0xE9U,    /**< U+00E9 LATIN SMALL LETTER E ACUTE.     */
  k_cp_euro       = 0x20ACU,  /**< U+20AC, 3-byte.                        */
  k_cp_emdash     = 0x2014U,  /**< U+2014 EM DASH, common in prose.       */
  k_cp_lquote     = 0x201CU,  /**< U+201C LEFT DOUBLE QUOTATION MARK.     */
  k_cp_rquote     = 0x201DU,  /**< U+201D RIGHT DOUBLE QUOTATION MARK.    */
  k_cp_linear_b   = 0x10348U, /**< U+10348, 4-byte.                       */
  k_cp_ascii_max  = 0x7FU,    /**< Highest 1-byte code point.             */
  k_len_1         = 1U,       /**< One byte consumed.                     */
  k_len_2         = 2U,       /**< Two bytes consumed.                    */
  k_len_3         = 3U,       /**< Three bytes consumed.                  */
  k_len_4         = 4U,       /**< Four bytes consumed.                   */
  k_byte_space_ct = 256U,     /**< Lead-byte sweep bound (every uint8_t). */
};

/**
 * @test internal_test_utf8_decode_well_formed
 *
 * @brief Every well-formed length decodes to its code point and consumes
 *        exactly the encoded number of bytes.
 */
static void internal_test_utf8_decode_well_formed(void)
{
  static const uint8_t s_dollar[] = {0x24U};
  static const uint8_t s_cent[]   = {0xC2U, 0xA2U};
  static const uint8_t s_euro[]   = {0xE2U, 0x82U, 0xACU};
  static const uint8_t s_linb[]   = {0xF0U, 0x90U, 0x8DU, 0x88U};

  uint32_t cp = 0U;

  TEST_ASSERT_EQ(k_len_1, (uint32_t)priv_reflow_tok_utf8_decode(s_dollar, sizeof s_dollar, &cp));
  TEST_ASSERT_EQ(k_cp_dollar, cp);

  TEST_ASSERT_EQ(k_len_2, (uint32_t)priv_reflow_tok_utf8_decode(s_cent, sizeof s_cent, &cp));
  TEST_ASSERT_EQ(k_cp_cent, cp);

  TEST_ASSERT_EQ(k_len_3, (uint32_t)priv_reflow_tok_utf8_decode(s_euro, sizeof s_euro, &cp));
  TEST_ASSERT_EQ(k_cp_euro, cp);

  TEST_ASSERT_EQ(k_len_4, (uint32_t)priv_reflow_tok_utf8_decode(s_linb, sizeof s_linb, &cp));
  TEST_ASSERT_EQ(k_cp_linear_b, cp);
}

/**
 * @test internal_test_utf8_decode_round_trip
 *
 * @brief encode -> decode is the identity over a spread of code points, so
 *        the new decoder cannot drift from the encoder already in the tree.
 */
static void internal_test_utf8_decode_round_trip(void)
{
  static const uint32_t s_cases[] = {
    0U,
    (uint32_t)k_cp_dollar,
    (uint32_t)k_cp_ascii_max,
    (uint32_t)k_priv_uc_2byte,
    (uint32_t)k_cp_eacute,
    (uint32_t)k_priv_uc_3byte - 1U,
    (uint32_t)k_priv_uc_3byte,
    (uint32_t)k_cp_emdash,
    (uint32_t)k_cp_lquote,
    (uint32_t)k_cp_rquote,
    (uint32_t)k_priv_uc_surr_lo - 1U,
    (uint32_t)k_priv_uc_surr_hi + 1U,
    (uint32_t)k_priv_uc_4byte - 1U,
    (uint32_t)k_priv_uc_4byte,
    (uint32_t)k_cp_linear_b,
    (uint32_t)k_priv_uc_max,
  };

  for (size_t i = 0U; i < (sizeof s_cases / sizeof s_cases[0]); ++i) {
    uint8_t      enc[k_len_4] = {0U};
    const size_t n            = priv_reflow_tok_utf8_encode(s_cases[i], enc);
    uint32_t     cp           = 0U;
    const size_t used         = priv_reflow_tok_utf8_decode(enc, n, &cp);
    TEST_ASSERT_EQ((uint32_t)n, (uint32_t)used);
    TEST_ASSERT_EQ(s_cases[i], cp);
  }
}

/**
 * @test internal_test_utf8_decode_malformed_one_byte
 *
 * @brief The four "extent unknown" shapes each substitute U+FFFD and consume
 *        exactly ONE byte, which is what lets the walk resynchronise on the
 *        byte that broke the sequence instead of swallowing good text.
 */
static void internal_test_utf8_decode_malformed_one_byte(void)
{
  /* A continuation byte first; 0xF8, which is no lead form at all; a 3-byte
   * lead with only two bytes readable; and a 3-byte lead whose second byte
   * is plain ASCII rather than a continuation. */
  static const uint8_t s_stray_cont[]  = {0x80U, 0x41U};
  static const uint8_t s_bad_lead[]    = {0xF8U, 0x41U};
  static const uint8_t s_truncated[]   = {0xE2U, 0x82U};
  static const uint8_t s_broken_cont[] = {0xE2U, 0x41U, 0x42U};
  static const uint8_t s_empty[]       = {0x00U};

  uint32_t cp = 0U;

  TEST_ASSERT_EQ(k_len_1,
                 (uint32_t)priv_reflow_tok_utf8_decode(s_stray_cont, sizeof s_stray_cont, &cp));
  TEST_ASSERT_EQ((uint32_t)k_priv_uc_replace, cp);

  TEST_ASSERT_EQ(k_len_1,
                 (uint32_t)priv_reflow_tok_utf8_decode(s_bad_lead, sizeof s_bad_lead, &cp));
  TEST_ASSERT_EQ((uint32_t)k_priv_uc_replace, cp);

  TEST_ASSERT_EQ(k_len_1,
                 (uint32_t)priv_reflow_tok_utf8_decode(s_truncated, sizeof s_truncated, &cp));
  TEST_ASSERT_EQ((uint32_t)k_priv_uc_replace, cp);

  TEST_ASSERT_EQ(k_len_1,
                 (uint32_t)priv_reflow_tok_utf8_decode(s_broken_cont, sizeof s_broken_cont, &cp));
  TEST_ASSERT_EQ((uint32_t)k_priv_uc_replace, cp);

  /* Zero readable bytes still advances, so no caller can stall on it. */
  TEST_ASSERT_EQ(k_len_1, (uint32_t)priv_reflow_tok_utf8_decode(s_empty, 0U, &cp));
  TEST_ASSERT_EQ((uint32_t)k_priv_uc_replace, cp);
}

/**
 * @test internal_test_utf8_decode_malformed_whole_sequence
 *
 * @brief Overlong, surrogate and out-of-range sequences are structurally
 *        complete, so their extent IS known: substitute U+FFFD but consume
 *        the whole sequence rather than re-reading its continuation bytes.
 */
static void internal_test_utf8_decode_malformed_whole_sequence(void)
{
  /* '/' encoded overlong as two and as three bytes, the surrogate U+D800,
   * and U+110000, one past the last code point. */
  static const uint8_t s_overlong_2[] = {0xC0U, 0xAFU};
  static const uint8_t s_overlong_3[] = {0xE0U, 0x80U, 0xAFU};
  static const uint8_t s_surrogate[]  = {0xEDU, 0xA0U, 0x80U};
  static const uint8_t s_too_big[]    = {0xF4U, 0x90U, 0x80U, 0x80U};

  uint32_t cp = 0U;

  TEST_ASSERT_EQ(k_len_2,
                 (uint32_t)priv_reflow_tok_utf8_decode(s_overlong_2, sizeof s_overlong_2, &cp));
  TEST_ASSERT_EQ((uint32_t)k_priv_uc_replace, cp);

  TEST_ASSERT_EQ(k_len_3,
                 (uint32_t)priv_reflow_tok_utf8_decode(s_overlong_3, sizeof s_overlong_3, &cp));
  TEST_ASSERT_EQ((uint32_t)k_priv_uc_replace, cp);

  TEST_ASSERT_EQ(k_len_3,
                 (uint32_t)priv_reflow_tok_utf8_decode(s_surrogate, sizeof s_surrogate, &cp));
  TEST_ASSERT_EQ((uint32_t)k_priv_uc_replace, cp);

  TEST_ASSERT_EQ(k_len_4, (uint32_t)priv_reflow_tok_utf8_decode(s_too_big, sizeof s_too_big, &cp));
  TEST_ASSERT_EQ((uint32_t)k_priv_uc_replace, cp);
}

/**
 * @test internal_test_utf8_decode_never_stalls
 *
 * @brief Sweep every lead byte 0x00..0xFF against every available-length
 *        1..4 and assert the two invariants a byte-pool walk depends on:
 *        the consumed length is in 1..4, and it never exceeds `avail`.
 *        A decoder that returned 0 anywhere here would hang the layout loop.
 */
static void internal_test_utf8_decode_never_stalls(void)
{
  for (uint32_t lead = 0U; lead < (uint32_t)k_byte_space_ct; ++lead) {
    for (uint32_t avail = k_len_1; avail <= k_len_4; ++avail) {
      /* Continuation-byte tail: the shape most likely to over-consume. */
      uint8_t buf[k_len_4] = {(uint8_t)lead, 0xBFU, 0xBFU, 0xBFU};
      uint32_t cp          = 0U;
      const size_t used    = priv_reflow_tok_utf8_decode(buf, (size_t)avail, &cp);
      TEST_ASSERT(used >= (size_t)k_len_1);
      TEST_ASSERT(used <= (size_t)k_len_4);
      TEST_ASSERT(used <= (size_t)avail);
      const bool is_surrogate =
        (cp >= (uint32_t)k_priv_uc_surr_lo) && (cp <= (uint32_t)k_priv_uc_surr_hi);
      TEST_ASSERT(!is_surrogate);
      TEST_ASSERT(cp <= (uint32_t)k_priv_uc_max);
    }
  }
}

/**
 * @test internal_test_layout_emits_one_glyph_per_code_point
 *
 * @brief The #686 regression itself: a paragraph of typographic punctuation
 *        lays out as one glyph per CHARACTER, carrying the real code points.
 *        Against the old byte-walk this paragraph produced 13 glyphs (each
 *        multi-byte character split into its bytes) instead of 7.
 */
static void internal_test_layout_emits_one_glyph_per_code_point(void)
{
  init_engine(k_vp_w, k_vp_h);
  /* "\u201Cca\u2014fe\u201D" -- 7 characters, 13 UTF-8 bytes. */
  (void)lay("<html><body><p>\xE2\x80\x9C" "ca\xE2\x80\x94" "fe\xE2\x80\x9D" "</p></body></html>");

  static const int32_t s_expect[] = {
    (int32_t)k_cp_lquote, (int32_t)'c', (int32_t)'a',        (int32_t)k_cp_emdash,
    (int32_t)'f',         (int32_t)'e', (int32_t)k_cp_rquote,
  };
  const uint32_t expect_n = (uint32_t)(sizeof s_expect / sizeof s_expect[0]);

  TEST_ASSERT_EQ(expect_n, s_eng.glyph_count);
  for (uint32_t i = 0U; i < expect_n; ++i) {
    TEST_ASSERT_EQ(s_expect[i], s_eng.glyphs[i].cp);
  }
}

/**
 * @test internal_test_layout_substitutes_malformed_bytes
 *
 * @brief A paragraph carrying a lone 0x80 lays out as U+FFFD followed by the
 *        surrounding ASCII: the walk substitutes and resynchronises rather
 *        than losing the next character.
 */
static void internal_test_layout_substitutes_malformed_bytes(void)
{
  init_engine(k_vp_w, k_vp_h);
  (void)lay("<html><body><p>a\x80z</p></body></html>");

  static const int32_t s_expect[] = {
    (int32_t)'a',
    (int32_t)k_priv_uc_replace,
    (int32_t)'z',
  };
  const uint32_t expect_n = (uint32_t)(sizeof s_expect / sizeof s_expect[0]);

  TEST_ASSERT_EQ(expect_n, s_eng.glyph_count);
  for (uint32_t i = 0U; i < expect_n; ++i) {
    TEST_ASSERT_EQ(s_expect[i], s_eng.glyphs[i].cp);
  }
}

/**
 * @test internal_test_layout_measure_and_emit_agree
 *
 * @brief The greedy pre-measure and the emit pass share one decoder, so a
 *        word of multi-byte characters wraps on character count, not byte
 *        count. With the fixed-metric Ahem face at 16 px in a 200 px
 *        viewport the two passes disagreeing would show up as a wrong line
 *        count for the same visual text.
 */
static void internal_test_layout_measure_and_emit_agree(void)
{
  /* Six em-dashes, then six ASCII 'M': same character count, same metrics. */
  init_engine(k_vp_w, k_vp_h);
  (void)lay("<html><body><p>\xE2\x80\x94\xE2\x80\x94\xE2\x80\x94"
            "\xE2\x80\x94\xE2\x80\x94\xE2\x80\x94</p></body></html>");
  const uint32_t glyphs_wide = s_eng.glyph_count;
  const uint32_t lines_wide  = line_count();

  init_engine(k_vp_w, k_vp_h);
  (void)lay("<html><body><p>MMMMMM</p></body></html>");

  TEST_ASSERT_EQ(s_eng.glyph_count, glyphs_wide);
  TEST_ASSERT_EQ(line_count(), lines_wide);
}

/**
 * @brief Test entry point.
 *
 * @return Process exit status.
 * @retval 0 Every assertion held.
 */
int main(void)
{
  internal_test_utf8_decode_well_formed();
  internal_test_utf8_decode_round_trip();
  internal_test_utf8_decode_malformed_one_byte();
  internal_test_utf8_decode_malformed_whole_sequence();
  internal_test_utf8_decode_never_stalls();
  internal_test_layout_emits_one_glyph_per_code_point();
  internal_test_layout_substitutes_malformed_bytes();
  internal_test_layout_measure_and_emit_agree();
  return 0;
}
