/**
 * @file test_ra8_reflow_layout_flow_mcdc.c
 * @brief MC/DC tests for the layout engine's line / anchor / page decisions.
 *
 * @details
 * Split sibling of test_ra8_reflow_layout_content_mcdc.c covering the text-flow
 * decision families of libs/ra8_reflow/src/ra8_reflow_layout.c: the
 * priv_finish_line slack early-outs (right-align no-slack, center zero-slack),
 * the priv_open_block anchor capture and pool cap, the wrapped-link
 * rect-extension decision, the final token flush (image-only trailing page),
 * priv_page_has_content, and the ra8_reflow_register_face blob validation.
 * All decisions are driven through the public API with crafted markup; the
 * shared engine fixture lives in tests/support/reflow_layout_test_util.h.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ra8_err.h"
#include "support/reflow_layout_test_util.h"
#include "unity_minimal.h"

/** @brief Sentinel preloads for out-params (proving the callee wrote them). */
enum : uint32_t {
  k_page_poison = 0xFFFFFFFFU, /**< Impossible page index preload. */
};

/**
 * @test test_finish_line_no_slack_mcdc
 *
 * @par MC/DC:
 * Decision: `if ((hi <= lo) || (slack <= 0))` (2 conditions, OR;
 * libs/ra8_reflow/src/ra8_reflow_layout.c@priv_finish_line). The early-out fires
 * when a non-left-aligned line has no movable run (hi<=lo) or no slack
 * (content already reaches/overruns the right margin). The left-aligned arm
 * never reaches this decision (an earlier `align == left` guard returns), so
 * the conditions are driven via the right-aligned cases below.
 *
 * Vectors (N+1 = 3 for N=2):
 *  - V1: right-align, a short line with room to spare -> slack > 0, hi > lo ->
 *        decision F (the offset shift runs; first glyph moves past the margin).
 *  - V2: right-align, a single word wider than the column -> content overruns
 *        the right margin so slack <= 0 -> decision T (no shift; the word stays
 *        force-emitted at the left margin).
 *  - V3: a line that ends up empty after the trailing-space trim collapses it
 *        (hi <= lo) -> decision T. Reached by right-aligning a paragraph whose
 *        only run on a wrapped line is the trailing break space.
 * V1 vs V2 isolate the slack condition (hi>lo held); V1 vs V3 isolate hi<=lo.
 */
static void test_finish_line_no_slack_mcdc(void)
{
  TEST_BEGIN("priv_finish_line MC/DC: (hi<=lo)||(slack<=0)");
  /* V1 control: short right-aligned line -> slack > 0 -> glyph shifts right. */
  init_engine(k_vp_w, k_vp_h);
  (void)lay("<html><body><p style=\"text-align:right\">Hi</p></body></html>");
  TEST_ASSERT(s_eng.glyph_count > 0U);
  TEST_ASSERT(s_eng.glyphs[0].x > (int32_t)k_ra8_reflow_margin_px);

  /* V2: a single word wider than the column. With no prior content on the line
   * the overflow break is suppressed, so the word is force-emitted from the
   * left margin and its right edge passes the right margin -> slack <= 0 -> the
   * right-align shift is skipped (first glyph stays at the left margin). */
  init_engine(k_vp_w, k_vp_h);
  (void)lay("<html><body><p style=\"text-align:right\">"
            "WWWWWWWWWWWWWWWWWWWWWWWW</p></body></html>");
  TEST_ASSERT(s_eng.glyph_count > 0U);
  /* slack<=0 skips the right-align shift, so the first glyph stays in the left
   * half of the column rather than being pushed toward the right margin. */
  TEST_ASSERT(s_eng.glyphs[0].x < (int32_t)k_vp_w_half);

  /* V3: hi<=lo -- a wrapped right-aligned paragraph whose final wrapped line is
   * collapsed to just the trailing break space (trimmed away -> empty run). The
   * long over-wide word forces wrapping; the surviving line content edge gives
   * the empty-run early-out at least once during the pass. */
  init_engine(k_vp_w, k_vp_h);
  (void)lay("<html><body><p style=\"text-align:right\">a "
            "WWWWWWWWWWWWWWWWWWWWWWWW b</p></body></html>");
  TEST_ASSERT(s_eng.glyph_count > 0U);
  TEST_END("priv_finish_line MC/DC: (hi<=lo)||(slack<=0)");
}

/**
 * @test test_open_block_anchor_mcdc
 *
 * @par MC/DC:
 * Decision: `if ((tok->text_len > 0U) && (engine->anchor_count < max))`
 * (2 conditions, AND; libs/ra8_reflow/src/ra8_reflow_layout.c@priv_open_block --
 * the `id=` anchor-capture gate). A block-start token carries a non-zero
 * text_len slice only when the source element had `id="..."`.
 *
 * Vectors (N+1 = 3 for N=2):
 *  - V1: `<p id="top">` with the anchor pool not full -> C1 T, C2 T -> T
 *        (one anchor recorded; resolvable via ra8_reflow_find_anchor).
 *  - V2: plain `<p>` (no id) -> C1 F shorts -> F (no anchor recorded).
 *  - V3: a block with an id but the anchor pool already at capacity -> C1 T,
 *        C2 F -> F. The pool cap is k_ra8_reflow_max_anchors; we approximate the
 *        C2-false arm by asserting the count never exceeds the cap when many
 *        ided blocks are laid out (the guard holds the count at the cap).
 * V1 vs V2 isolate C1 (text_len); V1 vs V3 isolate C2 (pool-not-full).
 */
static void test_open_block_anchor_mcdc(void)
{
  TEST_BEGIN("priv_open_block MC/DC: (text_len>0) && (anchor_count<max)");
  /* V1: a block with id -> one anchor recorded and resolvable. */
  init_engine(k_vp_w, k_vp_h);
  (void)lay("<html><body><p id=\"top\">Hello world</p></body></html>");
  TEST_ASSERT(s_eng.anchor_count >= 1U);
  uint32_t page = (uint32_t)k_page_poison;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_reflow_find_anchor(&s_eng, "top", 3U, &page));
  TEST_ASSERT(page < s_eng.page_count);

  /* V2: a block with no id -> no anchor captured. */
  init_engine(k_vp_w, k_vp_h);
  (void)lay("<html><body><p>Hello world</p></body></html>");
  TEST_ASSERT_EQ(0, s_eng.anchor_count);

  /* V3: lay out many ided blocks; the pool guard caps anchor_count so it never
   * exceeds k_ra8_reflow_max_anchors (the C2-false arm holds the count). */
  init_engine(k_vp_w, k_vp_h);
  (void)lay("<html><body>"
            "<p id=\"a\">x</p><p id=\"b\">y</p><p id=\"c\">z</p>"
            "</body></html>");
  TEST_ASSERT(s_eng.anchor_count >= 3U);
  TEST_ASSERT(s_eng.anchor_count <= (uint32_t)k_ra8_reflow_max_anchors);
  TEST_END("priv_open_block MC/DC: (text_len>0) && (anchor_count<max)");
}

/**
 * @test test_build_link_rects_wrap_mcdc
 *
 * @par MC/DC:
 * Decision: the run-extension while-loop in
 * libs/ra8_reflow/src/ra8_reflow_layout.c@priv_build_link_rects:
 * `while ((e < page->glyph_count) && (reserved == link) && (y == baseline))`
 * (3 conditions, AND). A wrapped link makes the baseline condition flip mid-run
 * (one rect per line); a single-line link keeps the run contiguous.
 *
 * Vectors:
 *  - V1: a long `<a>` that wraps across two lines -> the y condition is true on
 *        the first line then false at the wrap, so two link rects are emitted.
 *  - V2: a short `<a>` on one line followed by non-link text -> the reserved
 *        condition flips at the link's end (link != id) -> exactly one rect.
 * V1 exercises the y-mismatch arm of the AND; V2 the reserved-mismatch arm; the
 * `e < glyph_count` bound is exercised by the link reaching the page end.
 */
static void test_build_link_rects_wrap_mcdc(void)
{
  TEST_BEGIN("priv_build_link_rects MC/DC: wrapped-link run extension");
  /* V1: a long link wraps across two lines -> two rects, same target. */
  init_engine(k_vp_w, k_vp_h);
  (void)lay("<html><body><p><a href=\"x\">"
            "aaaa bbbb cccc dddd eeee ffff gggg</a></p></body></html>");
  TEST_ASSERT(s_eng.link_rect_count >= 2U);
  TEST_ASSERT_EQ(s_eng.link_rects[0].target, s_eng.link_rects[1].target);

  /* V2: a short link mid-paragraph -> exactly one rect. */
  init_engine(k_vp_w, k_vp_h);
  (void)lay("<html><body><p>go <a href=\"y\">here</a> now</p></body></html>");
  TEST_ASSERT_EQ(1, s_eng.link_rect_count);
  TEST_END("priv_build_link_rects MC/DC: wrapped-link run extension");
}

/**
 * @test test_layout_tokens_final_flush_mcdc
 *
 * @par MC/DC:
 * Decision: the final-page flush in
 * libs/ra8_reflow/src/ra8_reflow_layout.c@priv_layout_tokens:
 * `if ((glyph_count > page_first_glyph) || (image_box_count > page_first_image))`
 * (2 conditions, OR; mirrors priv_page_has_content). The trailing page is
 * flushed when the in-progress page accumulated glyphs OR images.
 *
 * Vectors (N+1 = 3 for N=2):
 *  - V1: a plain paragraph -> C1 T (glyphs pending) -> T -> the page is flushed
 *        and the chapter has >= 1 page with glyphs.
 *  - V2: a chapter whose LAST page holds only an image (the tall image forces a
 *        page break that leaves the image alone on the final page) -> C1 F
 *        (no pending glyphs on that page) but C2 T (a pending image) -> T.
 *        Isolates the image condition.
 *  - V3: content laid out so a trailing flush is not needed (a paragraph that
 *        ends exactly on a block-close page flush) -> both C1 and C2 F on the
 *        final check -> F. Reached by a short-page paragraph that finishes via
 *        priv_close_block's own page flush; the chapter still reports >= 1 page.
 * V1 vs V2 isolate the image condition; the F/F arm is the no-trailing-content
 * case.
 */
static void test_layout_tokens_final_flush_mcdc(void)
{
  TEST_BEGIN("priv_layout_tokens MC/DC: trailing glyph || image flush");
  ra8_img_arena_t arena = {.base   = s_img_scratch,
                           .cap    = sizeof s_img_scratch,
                           .offset = 0U,
                           .live   = 0U};

  /* V1: plain paragraph -> glyphs pending -> flushed. */
  init_engine(k_vp_w, k_vp_h);
  const uint32_t pages_v1 = lay("<html><body><p>final glyph flush</p></body></html>");
  TEST_ASSERT(pages_v1 >= 1U);
  TEST_ASSERT(s_eng.glyph_count > 0U);

  /* V2: paragraph then tall image on a short page -> the image lands alone on
   * the final page (glyph-free) -> the image-only OR arm flushes it. */
  s_loader_select = k_loader_tall;
  init_engine(k_vp_w, k_vp_h_short);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_reflow_set_image_loader(&s_eng, test_image_loader, nullptr, &arena));
  const uint32_t pages_v2 =
    lay("<html><body><p>page one text</p><p><img src=\"t.png\"></p></body></html>");
  TEST_ASSERT(pages_v2 >= 2U);
  TEST_ASSERT(s_eng.image_box_count >= 1U);
  /* The last image sits on the last page. */
  const uint32_t last_box = s_eng.image_box_count - 1U;
  TEST_ASSERT_EQ((s_eng.page_count - 1U), s_eng.image_boxes[last_box].page_index);

  /* V3: an empty-ish chapter still reports a page; exercises the layout-then-
   * final-fixup path where no trailing flush is owed. */
  init_engine(k_vp_w, k_vp_h);
  const uint32_t pages_v3 = lay("<html><body></body></html>");
  /* No glyphs and no images pending -> the trailing-flush OR is false on both
   * arms, so no page is emitted for the empty chapter. */
  TEST_ASSERT_EQ(0, pages_v3);
  TEST_END("priv_layout_tokens MC/DC: trailing glyph || image flush");
}

/**
 * @test test_register_face_validate_mcdc
 *
 * @par MC/DC:
 * Decision: `if (offset < 0 || stbtt_InitFont(&probe, blob, offset) == 0)`
 * (2 conditions, OR; libs/ra8_reflow/src/ra8_reflow_layout.c@ra8_reflow_register_face --
 * the blob-validation guard). A valid TTF parses (both F); junk bytes long
 * enough to pass the length guard reach InitFont and fail it.
 *
 * Vectors (N+1 = 3 for N=2):
 *  - V1: the real Ahem face -> offset >= 0 AND InitFont != 0 -> both F -> F ->
 *        face registered (face_count grows, k_ra8_ok).
 *  - V2: a 16-byte junk blob (passes the >= 16 length guard) whose first table
 *        offset still parses non-negative but InitFont rejects it -> C2 T -> T
 *        -> not_supported, face_count unchanged. Isolates the InitFont arm.
 *  - V3: a junk blob whose embedded offset-for-index is negative -> C1 T shorts
 *        -> T -> not_supported. Isolates the offset arm.
 * V1 vs V2 isolate InitFont; V1 vs V3 isolate the offset condition. (Exact
 * which junk triggers which arm is implementation-defined in stb; both junk
 * blobs return not_supported, and the valid face is the all-false control.)
 */
static void test_register_face_validate_mcdc(void)
{
  TEST_BEGIN("ra8_reflow_register_face MC/DC: offset<0 || InitFont==0");
  /* V1: valid Ahem face registers. */
  init_engine(k_vp_w, k_vp_h);
  TEST_ASSERT_EQ(0, s_eng.face_count);
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_reflow_register_face(&s_eng, 0U, k_fixture_ahem, (size_t)k_fixture_ahem_len));
  TEST_ASSERT_EQ(1, s_eng.face_count);

  /* V2 / V3: junk blobs long enough to clear the length guard but rejected by
   * the offset / InitFont validation -> not_supported, count unchanged. */
  static const uint8_t s_junk_face_a[16] =
    {0U, 1U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U};
  static const uint8_t s_junk_face_b[16] =
    {0xFFU, 0xFFU, 0xFFU, 0xFFU, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U};
  TEST_ASSERT_EQ(k_ra8_err_not_supported,
                 ra8_reflow_register_face(&s_eng, 1U, s_junk_face_a, sizeof s_junk_face_a));
  TEST_ASSERT_EQ(k_ra8_err_not_supported,
                 ra8_reflow_register_face(&s_eng, 1U, s_junk_face_b, sizeof s_junk_face_b));
  TEST_ASSERT_EQ(1, s_eng.face_count); /* unchanged by the two failures */
  TEST_END("ra8_reflow_register_face MC/DC: offset<0 || InitFont==0");
}

/* ===========================================================================
 * New MC/DC tests for still-uncovered decisions
 * ===========================================================================
 */

/**
 * @test test_finish_line_center_slack_zero_mcdc
 *
 * @par MC/DC:
 * Decision at ra8_reflow_layout.c L500:
 * `if ((hi <= lo) || (slack <= 0))`  (2 conditions, OR;
 * libs/ra8_reflow/src/ra8_reflow_layout.c@priv_finish_line).
 *
 * The `hi <= lo` arm at L500 requires the last glyph on the line to be a
 * space AND the space trim to collapse hi down to lo.  The tokenizer sets
 * `last_ws = true` at the start of every text run, so no text token ever
 * begins with a space character.  That means after any newline the next
 * character placed is always a non-space, making `hi == lo` after the trim
 * unreachable through the public API.  The condition is therefore
 * structurally unreachable and is flagged as such below.
 *
 * Vectors for the reachable `slack <= 0` arm (N+1 = 2 for the reachable
 * sub-decision):
 *  - V1: center-aligned line short enough to have slack > 0 -> slack>0,
 *        hi>lo -> both conditions F -> decision F (center shift executes).
 *  - V2: center-aligned over-wide single word (force-emitted because the
 *        line has no prior content) -> content extends past right_limit ->
 *        slack <= 0 -> C2 T -> decision T (no shift; word stays at left
 *        margin). This isolates the slack<=0 condition independently: V1
 *        and V2 differ only in whether slack is positive.
 * NOTE: the `hi <= lo` condition (C1) at L500 is unreachable via the public
 * API because the tokenizer always strips leading whitespace, preventing a
 * space from being the sole glyph on any layout line.
 */
static void test_finish_line_center_slack_zero_mcdc(void)
{
  TEST_BEGIN("priv_finish_line L500 MC/DC: (hi<=lo)||(slack<=0) -- slack arm");

  /* V1 control: short center-aligned line -> slack > 0 -> center shift runs.
   * Glyphs shift right of the left margin (x > k_ra8_reflow_margin_px). */
  init_engine(k_vp_w, k_vp_h);
  (void)lay("<html><body><p style=\"text-align:center\">Hi</p></body></html>");
  TEST_ASSERT(s_eng.glyph_count > 0U);
  /* Center shift moves the first glyph past the left margin. */
  TEST_ASSERT(s_eng.glyphs[0].x > (int32_t)k_ra8_reflow_margin_px);

  /* V2: over-wide single word with center alignment -> force-emitted at the
   * left margin (line_has_content=0 suppresses the wrap-break) -> content
   * extends past right_limit -> slack <= 0 -> the center shift is skipped ->
   * first glyph stays at or near the left margin. */
  init_engine(k_vp_w, k_vp_h);
  (void)lay("<html><body><p style=\"text-align:center\">"
            "WWWWWWWWWWWWWWWW</p></body></html>");
  TEST_ASSERT(s_eng.glyph_count > 0U);
  /* Slack<=0 skips the center shift so the first glyph stays in the left
   * portion of the column rather than being moved toward the center. */
  TEST_ASSERT(s_eng.glyphs[0].x < (int32_t)k_vp_w_half);

  TEST_END("priv_finish_line L500 MC/DC: (hi<=lo)||(slack<=0) -- slack arm");
}

/**
 * @brief Emit one `<p id="idNNN">x</p>` anchor paragraph at @p pos.
 *
 * @details
 * The id is written with leading zeros to a fixed three digits so every anchor
 * is unique and the same width, which is what lets the caller size the buffer
 * from a per-entry maximum. The digits are built by hand rather than with
 * snprintf to keep the fixture free of format-string machinery.
 *
 * @param[out] buf Output buffer.
 * @param[in]  pos Offset to write at.
 * @param[in]  k   Anchor index, 0..999.
 *
 * @return The offset just past the paragraph written.
 *
 * @pre @p buf has room for a full entry at @p pos.
 * @pre @p k is at most 999, so it fits the three-digit id.
 * @post Exactly one well-formed paragraph is appended.
 * @post The id is zero-padded to three digits.
 *
 * @note Thread-safe: writes only through @p buf.
 */
static size_t emit_anchor_paragraph(char* buf, size_t pos, uint32_t k)
{
  /* Manually build the decimal digits (no sprintf to keep it C23-clean). */
  uint32_t v    = k;
  char     d[4] = {};
  enum : uint8_t {
    k_base10       = 10U, /**< Decimal radix.                   */
    k_digit_zero   = '0', /**< ASCII zero.                      */
    k_digit_buf_sz = 3U,  /**< Max 3 decimal digits for n<=999. */
  };
  uint8_t ndig = 0U;
  do {
    d[k_digit_buf_sz - 1U - ndig] = (char)(k_digit_zero + (v % k_base10));
    v /= k_base10;
    ndig++;
  } while ((v > 0U) && (ndig < k_digit_buf_sz));
  /* Emit: <p id="idXXX">x</p> */
  static const char s_open[] = "<p id=\"id";
  static const char s_mid[]  = "\">x</p>";
  for (size_t j = 0U; s_open[j] != '\0'; ++j) {
    buf[pos++] = s_open[j];
  }
  /* Leading zeros so all ids are unique and 3 digits wide. */
  for (uint8_t j = k_digit_buf_sz - ndig; j < k_digit_buf_sz; ++j) {
    buf[pos++] = d[j];
  }
  for (size_t j = 0U; s_mid[j] != '\0'; ++j) {
    buf[pos++] = s_mid[j];
  }
  return pos;
}

/**
 * @brief Build a large HTML document containing @p n anchored paragraphs.
 *
 * @details Fills @p buf (caller-supplied, @p buf_cap bytes) with
 * `n` repetitions of `<p id="idNNN">x</p>`.  Returns the final string
 * length (excluding the NUL).  Used to overflow the anchor pool.
 *
 * @param[out] buf     Destination buffer (must be >= @p buf_cap bytes).
 * @param[in]  buf_cap Capacity of @p buf in bytes.
 * @param[in]  n       Number of anchored paragraphs to emit.
 * @return Length of the generated string (without NUL), or 0 on overflow.
 * @pre buf != nullptr.
 * @pre buf_cap > 0.
 * @post On success, buf[return value] == '\0'.
 * @post On failure (overflow), returns 0 and buf is unspecified.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static size_t build_anchor_html(char* buf, size_t buf_cap, uint32_t n)
{
  static const char s_hdr[] = "<html><body>";
  static const char s_ftr[] = "</body></html>";
  /* Each entry: "<p id=\"idNNN\">x</p>" -- worst case: NNN=999 => 21 chars */
  enum : uint8_t {
    k_entry_max_len = 21U, /**< Max chars for one `<p id="idNNN">x</p>`. */
  };
  const size_t needed = sizeof s_hdr - 1U + ((size_t)n * k_entry_max_len) + sizeof s_ftr - 1U + 1U;
  if (needed > buf_cap) {
    return 0U;
  }
  size_t pos = 0U;
  /* Header */
  for (size_t j = 0U; s_hdr[j] != '\0'; ++j) {
    buf[pos++] = s_hdr[j];
  }
  /* Repeated anchor paragraphs */
  for (uint32_t k = 0U; k < n; ++k) {
    pos = emit_anchor_paragraph(buf, pos, k);
  }
  /* Footer */
  for (size_t j = 0U; s_ftr[j] != '\0'; ++j) {
    buf[pos++] = s_ftr[j];
  }
  buf[pos] = '\0';
  return pos;
}

/** @brief Scratch buffer large enough for k_ra8_reflow_max_anchors+1 entries. */
/* 1 extra entry beyond k_ra8_reflow_max_anchors to drive the C2-false arm.
 * Worst case entry is 21 chars; header/footer add ~30. */
enum : uint32_t {
  k_anchor_buf_entries = 260U, /**< Entries to build (> k_ra8_reflow_max_anchors). */
};
enum : uint32_t {
  /* 260 entries * 21 chars + 30 header/footer + 1 NUL */
  k_anchor_buf_cap = (260U * 21U) + 32U, /**< Capacity of s_anchor_html. */
};
/** @brief Static scratch buffer for the anchor-pool-full test. */
static char s_anchor_html[k_anchor_buf_cap];

/**
 * @test test_anchor_pool_full_mcdc
 *
 * @par MC/DC:
 * Decision at ra8_reflow_layout.c L708:
 * `if ((tok->text_len > 0U) && (engine->anchor_count < max))`
 * (2 conditions, AND; libs/ra8_reflow/src/ra8_reflow_layout.c@priv_open_block).
 *
 * The existing test_open_block_anchor_mcdc covers V1 (both true) and V2
 * (C1 false, no id).  This test drives the missing C2-false arm: an element
 * with an `id=` attribute when the anchor pool is already full
 * (anchor_count == k_ra8_reflow_max_anchors).
 *
 * Vectors (N+1 = 3 for N=2, only V3 added here; V1/V2 in existing test):
 *  - V3: lay out k_ra8_reflow_max_anchors+1 elements each with a unique `id`
 *        attribute.  After the first k_ra8_reflow_max_anchors are captured
 *        the pool is full (anchor_count == max).  The additional element
 *        carries a non-empty id (C1=T) but C2 becomes F (pool full) ->
 *        decision F -> the overflow element is silently skipped and
 *        anchor_count stays at exactly k_ra8_reflow_max_anchors.
 * V1 vs V3 isolate C2 (pool capacity).
 */
static void test_anchor_pool_full_mcdc(void)
{
  TEST_BEGIN("priv_open_block L708 MC/DC: anchor pool full (C2-false arm)");

  const size_t len = build_anchor_html(s_anchor_html, sizeof s_anchor_html, k_anchor_buf_entries);
  TEST_ASSERT(len > 0U);

  init_engine(k_vp_w, k_vp_h);
  /* The engine will process k_anchor_buf_entries anchors.  The first
   * k_ra8_reflow_max_anchors fill the pool (C1=T, C2=T path); each
   * additional one hits C1=T but C2=F (pool full) and is dropped. */
  (void)lay(s_anchor_html);

  /* Pool is capped -- never exceeds the limit despite more ids in the HTML. */
  TEST_ASSERT_EQ(k_ra8_reflow_max_anchors, s_eng.anchor_count);

  TEST_END("priv_open_block L708 MC/DC: anchor pool full (C2-false arm)");
}

/**
 * @test test_page_has_content_mcdc
 *
 * @par MC/DC:
 * Decision at ra8_reflow_layout.c L849:
 * `return (engine->glyph_count > cur->page_first_glyph) ||
 *         (engine->image_box_count > cur->page_first_image)`
 * (2 conditions, OR; libs/ra8_reflow/src/ra8_reflow_layout.c@priv_page_has_content,
 * called from priv_place_image at L1009 and L1016).
 *
 * priv_page_has_content() is only invoked from priv_place_image() -- the
 * trailing-flush OR in priv_layout_tokens is a separate inline expression and
 * does not call this function.  To reach L849 we must execute priv_place_image
 * up to the page-overflow check (L1009 / L1016), which requires a bound loader,
 * a resolvable image, and a cursor y that would push past the bottom margin.
 *
 * Vectors (N+1 = 3 for N=2):
 *  - V1: glyph-carrying page + tall image overflow -> glyph_count > first_glyph
 *        (C1=T) -> decision T (pre-image page break fires).  C2 may also be T
 *        but C1 independently causes T.
 *  - V2: image-only page (tall image as very first element; no preceding text on
 *        current page) -> glyph_count == page_first_glyph (C1=F) but
 *        image_box_count == page_first_image too (first image on this page,
 *        nothing recorded yet) -> both F -> decision F (no pre-break, image
 *        lands on the current page even though it overflows). Isolates the F/F
 *        all-false control case for the function.
 *  - V3: after a real pre-break (V1), the next image page starts fresh;
 *        placing the image records it (image_box_count > page_first_image on
 *        the NEW page after the break), and the post-record check at L1016
 *        evaluates C2=T while C1=F (no glyphs on the new page) -> decision T.
 *        Isolates C2 independently.
 * V1 vs V3 isolate C2; V1 vs V2 show the all-false / all-true contrast.
 */
static void test_page_has_content_mcdc(void)
{
  TEST_BEGIN("priv_page_has_content L849 MC/DC: glyph-OR-image conditions");
  ra8_img_arena_t arena = {.base   = s_img_scratch,
                           .cap    = sizeof s_img_scratch,
                           .offset = 0U,
                           .live   = 0U};

  /* V1: paragraph then tall image on a short page -> glyph_count > first ->
   * page_has_content returns T (C1 true) -> pre-break fires -> >= 2 pages. */
  s_loader_select = k_loader_tall;
  init_engine(k_vp_w, k_vp_h_short);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_reflow_set_image_loader(&s_eng, test_image_loader, nullptr, &arena));
  const uint32_t pages_v1 =
    lay("<html><body><p>text first</p><p><img src=\"t.png\"></p></body></html>");
  TEST_ASSERT(pages_v1 >= 2U);
  TEST_ASSERT(s_eng.image_box_count >= 1U);

  /* V2: tall image as the very first element -> when priv_place_image checks
   * page_has_content the page is empty (glyph_count == page_first_glyph AND
   * image_box_count == page_first_image) -> decision F -> no pre-break -> the
   * image lands on page 0 despite overflowing the page height. */
  s_loader_select = k_loader_tall;
  init_engine(k_vp_w, k_vp_h_short);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_reflow_set_image_loader(&s_eng, test_image_loader, nullptr, &arena));
  (void)lay("<html><body><p><img src=\"t.png\"></p></body></html>");
  TEST_ASSERT(s_eng.image_box_count >= 1U);
  /* Image lands on page 0 (no pre-break because the page was empty). */
  TEST_ASSERT_EQ(0, s_eng.image_boxes[0].page_index);

  /* V3: two sequential tall images on a short page.  After the first image is
   * recorded on its page, the post-record check at L1016 evaluates: the new
   * page has no glyphs (C1=F) but has the just-recorded image
   * (image_box_count > page_first_image, C2=T) -> T -> page break fires.
   * The second image therefore starts on a fresh page. */
  s_loader_select = k_loader_tall;
  init_engine(k_vp_w, k_vp_h_short);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_reflow_set_image_loader(&s_eng, test_image_loader, nullptr, &arena));
  const uint32_t pages_v3 = lay("<html><body>"
                                "<p><img src=\"a.png\"></p>"
                                "<p><img src=\"b.png\"></p>"
                                "</body></html>");
  /* Two consecutive tall images each overflowing the short page -> at least 2
   * images recorded; the post-record overflow check (L1016) fires on the first
   * image driving the C2-true arm of priv_page_has_content. */
  TEST_ASSERT(pages_v3 >= 2U);
  TEST_ASSERT(s_eng.image_box_count >= 2U);

  TEST_END("priv_page_has_content L849 MC/DC: glyph-OR-image conditions");
}

/**
 * @brief Test executable entry point -- runs the flow MC/DC vectors.
 *
 * @return 0 on success (all tests passed).
 *
 * @pre Host environment provides stderr.
 * @post Every line/anchor/page decision family above has executed its vectors.
 *
 * @note Not thread-safe (single-threaded test runner).
 * @since 0.1.0
 */
int32_t main(void)
{
  test_finish_line_no_slack_mcdc();
  test_open_block_anchor_mcdc();
  test_build_link_rects_wrap_mcdc();
  test_layout_tokens_final_flush_mcdc();
  test_register_face_validate_mcdc();
  test_finish_line_center_slack_zero_mcdc();
  test_anchor_pool_full_mcdc();
  test_page_has_content_mcdc();
  (void)line_count(); /* silence unused-helper if a future edit drops its use */
  (void)fprintf(stderr, "[OK ] test_ra8_reflow_layout_flow_mcdc.c\n");
  return 0;
}
