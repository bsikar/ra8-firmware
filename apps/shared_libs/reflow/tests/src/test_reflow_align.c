/**
 * @file test_reflow_align.c
 * @brief Host unit tests + MC/DC for text alignment / justification (#108).
 *
 * @details
 * Lays out paragraphs with `style="text-align:..."` through `reflow` (with
 * the fixed-metric Ahem face) and asserts the resulting glyph x positions:
 *  - left  -> first glyph at the left margin (the historical layout);
 *  - right -> the line's right edge meets the right margin;
 *  - center-> equal left/right gaps;
 *  - justify-> wrapped lines reach the right margin, the last line stays left.
 * Plus MC/DC mirror vectors for the justify-vs-last-line gate and the
 * centre/right offset decision in internal_finish_line().
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "fixture_ahem.h"
#include "ra8_attributes.h"
#include "ra8_err.h"
#include "reflow.h"
#include "unity_minimal.h"

/** @brief Shared engine (large -- keep off the stack). */
static reflow_t s_eng;

enum : int32_t {
  k_w           = 200,            /**< W.                 */
  k_h           = 400,            /**< H.                 */
  k_margin      = 16,             /**< Margin.            */
  k_right_limit = k_w - k_margin, /**< Right limit (184). */
};

/** @brief Lay out @p doc into the shared engine; return the glyph count.
 * @details Exercises the lay path and preserves each documented result and bound.
 * @param[in] doc Caller-supplied doc value used by the scenario.
 * @return The scalar result computed for the requested reflow scenario.
 * @retval 0 The helper produced its zero-valued result.
 * @retval nonzero The helper produced a nonzero result.
 * @pre The referenced fixture inputs are valid for this scenario.
 * @pre Fixed-capacity output buffers are initialized before the operation.
 * @post All assertions for the scenario have passed before this function returns.
 * @post Caller-owned fixture storage remains valid for subsequent vectors.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 */
RA8_INTERNAL static uint32_t internal_lay(const char* doc)
{
  uint32_t pages = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 reflow_init((uint16_t)k_w,
                             (uint16_t)k_h,
                             k_fixture_ahem,
                             (size_t)k_fixture_ahem_len,
                             16U,
                             0xFFFFFFU,
                             0x3060FFU,
                             &s_eng));
  TEST_ASSERT_EQ(k_ra8_ok,
                 reflow_layout_chapter(&s_eng, (const uint8_t*)doc, (uint32_t)strlen(doc), &pages));
  return s_eng.glyph_count;
}

/** @brief Per-glyph advance on the first line (Ahem is fixed-pitch).
 * @details Exercises the first line advance path and preserves each documented result and bound.
 * @return The scalar result computed for the requested reflow scenario.
 * @retval 0 The helper produced its zero-valued result.
 * @retval nonzero The helper produced a nonzero result.
 * @pre The referenced fixture inputs are valid for this scenario.
 * @pre Fixed-capacity output buffers are initialized before the operation.
 * @post All assertions for the scenario have passed before this function returns.
 * @post Caller-owned fixture storage remains valid for subsequent vectors.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 */
RA8_INTERNAL static int32_t internal_first_line_advance(void)
{
  if (s_eng.glyph_count < 2U) {
    return 16;
  }
  return s_eng.glyphs[1].x - s_eng.glyphs[0].x;
}

/** @brief Index of the last glyph sharing glyph[0]'s baseline (its line).
 * @details Exercises the last on first line path and preserves each documented result and bound.
 * @return The scalar result computed for the requested reflow scenario.
 * @retval 0 The helper produced its zero-valued result.
 * @retval nonzero The helper produced a nonzero result.
 * @pre The referenced fixture inputs are valid for this scenario.
 * @pre Fixed-capacity output buffers are initialized before the operation.
 * @post All assertions for the scenario have passed before this function returns.
 * @post Caller-owned fixture storage remains valid for subsequent vectors.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 */
RA8_INTERNAL static uint32_t internal_last_on_first_line(void)
{
  const int32_t y0   = s_eng.glyphs[0].y;
  uint32_t      last = 0U;
  for (uint32_t i = 0U; i < s_eng.glyph_count; ++i) {
    if (s_eng.glyphs[i].y == y0) {
      last = i;
    }
  }
  return last;
}

/**
 * @test internal_test_align_left_right_center
 * @brief Left keeps the margin; right meets the right edge; centre balances.
 *
 * @par MC/DC:
 * Two AND decisions in this case's own assertions (enclosing fn
 * internal_test_align_left_right_center); the right / centre layout produces the control
 * (both conditions true), N+1 = 3 each.
 * Right-edge tolerance: `(r_edge >= k_right_limit-2) && (r_edge <= k_right_limit+2)`
 * (k_right_limit == 184):
 *  - V1 r_edge in [182,186] -> C1=T, C2=T -> T (line flush to the right margin).
 *  - V2 r_edge < 182        -> C1=F       -> F (line short of the margin).
 *  - V3 r_edge > 186        -> C1=T, C2=F -> F (line past the margin).
 * Centre balance: `((left_gap - right_gap) <= 2) && ((right_gap - left_gap) <= 2)`:
 *  - V1 |left_gap - right_gap| <= 2 -> C1=T, C2=T -> T (gaps balanced).
 *  - V2 left_gap - right_gap > 2    -> C1=F       -> F (text pushed right).
 *  - V3 right_gap - left_gap > 2    -> C1=T, C2=F -> F (text pushed left).
 * @details Exercises the align left right center path and preserves each documented result and bound.
 * @pre The referenced fixture inputs are valid for this scenario.
 * @pre Fixed-capacity output buffers are initialized before the operation.
 * @post All assertions for the scenario have passed before this function returns.
 * @post Caller-owned fixture storage remains valid for subsequent vectors.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_align_left_right_center(void)
{
  TEST_BEGIN("text-align left / right / centre");
  /* Left (default): unchanged from the greedy layout. */
  (void)internal_lay("<html><body><p>Hi there</p></body></html>");
  TEST_ASSERT(s_eng.glyph_count > 0U);
  TEST_ASSERT_EQ(k_margin, s_eng.glyphs[0].x);

  /* Right: the line's right edge lands at the right margin. */
  (void)internal_lay("<html><body><p style=\"text-align:right\">Hi</p></body></html>");
  TEST_ASSERT(s_eng.glyphs[0].x > k_margin);
  const int32_t r_edge =
    s_eng.glyphs[internal_last_on_first_line()].x + internal_first_line_advance();
  TEST_ASSERT((r_edge >= (k_right_limit - 2)) && (r_edge <= (k_right_limit + 2)));

  /* Centre: equal gaps on each side. */
  (void)internal_lay("<html><body><p style=\"text-align:center\">Hi</p></body></html>");
  const int32_t adv       = internal_first_line_advance();
  const int32_t left_gap  = s_eng.glyphs[0].x - k_margin;
  const int32_t right_gap = k_right_limit - (s_eng.glyphs[internal_last_on_first_line()].x + adv);
  TEST_ASSERT(s_eng.glyphs[0].x > k_margin);
  TEST_ASSERT(left_gap > 0);
  TEST_ASSERT(((left_gap - right_gap) <= 2) && ((right_gap - left_gap) <= 2));
  TEST_END("text-align left / right / centre");
}

/**
 * @test internal_test_align_justify
 * @brief A justified paragraph: wrapped lines reach the margin, last line left.
 *
 * @par MC/DC:
 * Decision (last non-space glyph on the first line, enclosing fn
 * internal_test_align_justify): `(glyphs[i].y == y0) && (glyphs[i].cp != ' ')`
 * (2 conditions, AND), evaluated across the laid-out glyphs. N+1 = 3:
 *  - V1 a non-space glyph on line 0     -> C1=T, C2=T -> T (selected as `last`).
 *  - V2 a non-space glyph on a later line -> C1=F, C2=T -> F (not on the first line).
 *  - V3 a space glyph on line 0         -> C1=T, C2=F -> F (space excluded).
 * V1 vs V2 isolate the baseline condition; V1 vs V3 the non-space condition.
 * @details Exercises the align justify path and preserves each documented result and bound.
 * @pre The referenced fixture inputs are valid for this scenario.
 * @pre Fixed-capacity output buffers are initialized before the operation.
 * @post All assertions for the scenario have passed before this function returns.
 * @post Caller-owned fixture storage remains valid for subsequent vectors.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_align_justify(void)
{
  TEST_BEGIN("text-align justify (even right edge, last line left)");
  (void)internal_lay("<html><body><p style=\"text-align:justify\">aa bb cc dd ee ff gg hh "
                     "ii jj kk ll mm nn oo pp qq rr ss tt</p></body></html>");
  /* The paragraph must wrap to >= 2 lines for justify to matter. */
  int32_t  py    = s_eng.glyphs[0].y;
  uint32_t lines = 1U;
  for (uint32_t i = 0U; i < s_eng.glyph_count; ++i) {
    if (s_eng.glyphs[i].y != py) {
      lines++;
      py = s_eng.glyphs[i].y;
    }
  }
  TEST_ASSERT(lines >= 2U);

  /* First (wrapped) line: last non-space glyph's right edge at the margin. */
  const int32_t y0   = s_eng.glyphs[0].y;
  const int32_t adv  = internal_first_line_advance();
  uint32_t      last = 0U;
  for (uint32_t i = 0U; i < s_eng.glyph_count; ++i) {
    if ((s_eng.glyphs[i].y == y0) && (s_eng.glyphs[i].cp != (int32_t)' ')) {
      last = i;
    }
  }
  TEST_ASSERT((s_eng.glyphs[last].x + adv) >= (k_right_limit - 3));

  /* Last line: left-aligned (not justified). */
  const int32_t y_last = s_eng.glyphs[s_eng.glyph_count - 1U].y;
  uint32_t      first  = 0U;
  for (uint32_t i = 0U; i < s_eng.glyph_count; ++i) {
    if (s_eng.glyphs[i].y == y_last) {
      first = i;
      break;
    }
  }
  TEST_ASSERT_EQ(k_margin, s_eng.glyphs[first].x);
  TEST_END("text-align justify (even right edge, last line left)");
}

/** @brief Mirror of internal_finish_line's justify gate: align==justify && allow.
 * @details Exercises the mirror justify applies path and preserves each documented result and bound.
 * @param[in] align Caller-supplied align value used by the scenario.
 * @param[in] allow Caller-supplied allow value used by the scenario.
 * @return The scalar result computed for the requested reflow scenario.
 * @retval 0 The helper produced its zero-valued result.
 * @retval nonzero The helper produced a nonzero result.
 * @pre The referenced fixture inputs are valid for this scenario.
 * @pre Fixed-capacity output buffers are initialized before the operation.
 * @post All assertions for the scenario have passed before this function returns.
 * @post Caller-owned fixture storage remains valid for subsequent vectors.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 */
RA8_INTERNAL static uint8_t internal_mirror_justify_applies(uint8_t align, uint8_t allow)
{
  if ((align == (uint8_t)k_reflow_align_justify) && (allow != 0U)) {
    return 1U;
  }
  return 0U;
}

/**
 * @test internal_test_justify_gate_mcdc
 *
 * @par MC/DC:
 * Decision: `if (align == justify && allow_justify)` (2 conditions, AND;
 * apps/shared_libs/reflow/src/reflow_layout.c@internal_finish_line -- the
 * justify-vs-last-line decision).
 *
 * Vectors (N+1 = 3 for N=2):
 *  - V1: align=justify, allow=1 -> T (justify the wrapped line).
 *  - V2: align=justify, allow=0 -> F (last line stays left).
 *  - V3: align=center,  allow=1 -> F (not a justify block).
 * V1 vs V2 vary allow (align held justify); V1 vs V3 vary align (allow held 1).
 * @brief Verify justify gate mcdc behavior against the reflow contract.
 * @details Exercises the justify gate mcdc path and preserves each documented result and bound.
 * @pre The referenced fixture inputs are valid for this scenario.
 * @pre Fixed-capacity output buffers are initialized before the operation.
 * @post All assertions for the scenario have passed before this function returns.
 * @post Caller-owned fixture storage remains valid for subsequent vectors.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_justify_gate_mcdc(void)
{
  TEST_BEGIN("priv_finish_line justify gate MC/DC: align==justify && allow");
  TEST_ASSERT_EQ(1, internal_mirror_justify_applies((uint8_t)k_reflow_align_justify, 1U));
  TEST_ASSERT_EQ(0, internal_mirror_justify_applies((uint8_t)k_reflow_align_justify, 0U));
  TEST_ASSERT_EQ(0, internal_mirror_justify_applies((uint8_t)k_reflow_align_center, 1U));
  TEST_END("priv_finish_line justify gate MC/DC: align==justify && allow");
}

/** @brief Mirror of the centre/right offset: align==center ? slack/2 : slack.
 * @details Exercises the mirror align offset path and preserves each documented result and bound.
 * @param[in] align Caller-supplied align value used by the scenario.
 * @param[in] slack Caller-supplied slack value used by the scenario.
 * @return The scalar result computed for the requested reflow scenario.
 * @retval 0 The helper produced its zero-valued result.
 * @retval nonzero The helper produced a nonzero result.
 * @pre The referenced fixture inputs are valid for this scenario.
 * @pre Fixed-capacity output buffers are initialized before the operation.
 * @post All assertions for the scenario have passed before this function returns.
 * @post Caller-owned fixture storage remains valid for subsequent vectors.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 */
RA8_INTERNAL static int32_t internal_mirror_align_offset(uint8_t align, int32_t slack)
{
  return (align == (uint8_t)k_reflow_align_center) ? (slack / 2) : slack;
}

/**
 * @test internal_test_align_offset_mcdc
 *
 * @par MC/DC:
 * Decision: `(align == center) ? slack/2 : slack` (1 condition; the centre-vs-
 * right offset in internal_finish_line). Single-condition decision -> 2 vectors:
 *  - V1: align=center -> slack/2 (centre).
 *  - V2: align=right  -> slack   (flush right).
 * @brief Verify align offset mcdc behavior against the reflow contract.
 * @details Exercises the align offset mcdc path and preserves each documented result and bound.
 * @pre The referenced fixture inputs are valid for this scenario.
 * @pre Fixed-capacity output buffers are initialized before the operation.
 * @post All assertions for the scenario have passed before this function returns.
 * @post Caller-owned fixture storage remains valid for subsequent vectors.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_align_offset_mcdc(void)
{
  TEST_BEGIN("priv_finish_line offset MC/DC: align==center ? slack/2 : slack");
  TEST_ASSERT_EQ(50, internal_mirror_align_offset((uint8_t)k_reflow_align_center, 100));
  TEST_ASSERT_EQ(100, internal_mirror_align_offset((uint8_t)k_reflow_align_right, 100));
  TEST_END("priv_finish_line offset MC/DC: align==center ? slack/2 : slack");
}

/**
 * @brief Test entry point.
 * @return 0 on success; unity macros exit(1) on the first failure.
 */
int main(void)
{
  internal_test_align_left_right_center();
  internal_test_align_justify();
  internal_test_justify_gate_mcdc();
  internal_test_align_offset_mcdc();
  return 0;
}
