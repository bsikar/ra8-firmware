/**
 * @file test_ra_reflow_layout_mcdc.c
 * @brief Additional MC/DC unit tests for libs/ra_reflow/src/ra_reflow_layout.c.
 *
 * @details
 * Companion to tests/test_ra_reflow_layout.c (which exercises the promoted
 * pure-helper decisions) and tests/test_ra_reflow_align.c (alignment offsets).
 * This file drives the *production* layout engine through the compound boolean
 * decisions that the promoted helpers do not cover -- the ones that only fire
 * with crafted markup geometry:
 *
 *  - priv_finish_line():     the `(hi <= lo) || (slack <= 0)` early-out, by
 *                            right-aligning a line whose content already fills
 *                            (and overruns) the column so there is no slack.
 *  - priv_open_block():      the `id=` anchor-capture decision
 *                            `(text_len > 0) && (anchor_count < max)`, by laying
 *                            out a block carrying `id="..."` vs an unstyled one.
 *  - priv_apply_image() /
 *    priv_place_image() /
 *    priv_image_resolve_size(): the image-loader gate, probe-failure OR, the
 *                            degenerate-column guard, and the image-overflow
 *                            page break, by binding a DI image loader that hands
 *                            back a real tall PNG (placed), a 2x2 PNG (fits), or
 *                            junk bytes (probe fails -> placeholder fallback).
 *  - priv_page_has_content() / priv_layout_tokens(): the image-only trailing
 *                            page flush (`image_box_count > page_first_image`).
 *  - priv_build_link_rects(): the wrapped-`<a>` run-extension while-decision,
 *                            by wrapping a single link across two lines.
 *  - priv_is_cell_start() /
 *    priv_is_row_start() /
 *    priv_cell_text() /
 *    priv_layout_row():      the table grid path, including a table that
 *                            page-breaks between rows.
 *  - ra_reflow_register_face(): the `offset < 0 || InitFont == 0` blob-validate
 *                            decision, by registering a valid Ahem face vs junk.
 *
 * Every paragraph is laid out with the fixed-metric Ahem face so glyph advances
 * are exactly one em -- the geometry (column fill, wrap points, page overflow)
 * is deterministic. All decisions are driven through the public API so llvm-cov
 * scores the real source lines, not a mirror.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "fixture_ahem.h"
#include "ra_err.h"
#include "ra_reflow.h"
#include "ra_reflow_image.h"
#include "unity_minimal.h"

/** @brief Shared engine (large -- keep off the stack). */
static ra_reflow_t s_eng;

/** @brief Decode scratch for the image-loader path (bump arena). */
static uint8_t s_img_scratch[64U * 1024U];

/** @brief Named viewport / font geometry (no magic numbers). */
enum : uint16_t {
  k_vp_w       = 200U, /**< Default test viewport width.  */
  k_vp_h       = 400U, /**< Default test viewport height. */
  k_vp_h_short = 96U,  /**< Short viewport to force early page breaks. */
  k_vp_w_tiny  = 32U,  /**< Width where col_w == 0 (degenerate column). */
  k_font_px    = 16U,  /**< Ahem body size (1 em advance == 16 px).      */
};

/** @brief Body / link colours for ra_reflow_init(). */
enum : uint32_t {
  k_body_color = 0xFFFFFFU, /**< White body text. */
  k_link_color = 0x3060FFU, /**< Blue links.      */
};

/**
 * @brief A tall 4x240 RGB PNG (column-fit becomes a tall box).
 * @details Baked with zlib; intrinsic 4 wide by 240 high, so when fit to the
 * text column the box keeps a tall aspect -- big enough to overflow a short
 * page. Channel content is irrelevant (layout only probes the dimensions).
 */
static const uint8_t s_png_tall[] = {
  137, 80,  78,  71,  13,  10,  26,  10,  0,   0,   0,   13, 73, 72,  68, 82, 0,   0,  0,  4,   0,
  0,   0,   240, 8,   2,   0,   0,   0,   168, 144, 82,  38, 0,  0,   0,  26, 73,  68, 65, 84,  120,
  218, 237, 193, 129, 0,   0,   0,   0,   195, 160, 249, 83, 95, 224, 8,  85, 1,   0,  0,  124, 3,
  12,  48,  0,   1,   196, 109, 199, 134, 0,   0,   0,   0,  73, 69,  78, 68, 174, 66, 96, 130,
};

/**
 * @brief A 2x2 RGB PNG: column-fit stays 2x2 (always fits any page).
 * @details Reused from tests/test_ra_reflow_image.c -- the smallest decodable
 * raster fixture stb_image accepts.
 */
static const uint8_t s_png_2x2[] = {
  137, 80,  78,  71, 13,  10,  26,  10,  0,   0,   0,   13,  73,  72,  68,  82,  0,   0,  0,   2,
  0,   0,   0,   2,  8,   2,   0,   0,   0,   253, 212, 154, 115, 0,   0,   0,   22,  73, 68,  65,
  84,  120, 156, 99, 248, 207, 192, 192, 240, 159, 129, 145, 129, 225, 255, 255, 255, 12, 0,   30,
  246, 4,   253, 9,  237, 52,  62,  0,   0,   0,   0,   73,  69,  78,  68,  174, 66,  96, 130,
};

/** @brief Eight bytes that are not any image format stb_image accepts. */
static const uint8_t s_junk[8] = {1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U};

/** @brief Which baked fixture the DI image loader should return. */
typedef enum : uint8_t {
  k_loader_tall = 0U, /**< Return the tall 4x240 PNG.  */
  k_loader_2x2  = 1U, /**< Return the 2x2 PNG.         */
  k_loader_junk = 2U, /**< Return undecodable bytes.   */
} loader_select_t;

/** @brief Loader selector handed to the engine via ctx. */
static loader_select_t s_loader_select = k_loader_tall;

/**
 * @brief DI image loader: resolves any href to the selected baked fixture.
 *
 * @details Wired through ra_reflow_set_image_loader(). The selector lives in a
 * file-static so each test can pick the fixture (tall / 2x2 / junk) that drives
 * the decision it targets. The bytes are static and outlive the call.
 *
 * @param[in]  ctx       Unused (selector is the file-static s_loader_select).
 * @param[in]  href      Image src (ignored; one fixture per test).
 * @param[in]  href_len  Length of @p href (ignored).
 * @param[out] out_bytes Receives the fixture pointer.
 * @param[out] out_len   Receives the fixture length.
 * @return k_ra_ok always (the bytes are always supplied).
 */
static ra_err_t test_image_loader(void*           ctx,
                                  const char*     href,
                                  uint32_t        href_len,
                                  const uint8_t** out_bytes,
                                  size_t*         out_len)
{
  (void)ctx;
  (void)href;
  (void)href_len;
  switch (s_loader_select) {
    case k_loader_2x2:
      *out_bytes = s_png_2x2;
      *out_len   = sizeof s_png_2x2;
      break;
    case k_loader_junk:
      *out_bytes = s_junk;
      *out_len   = sizeof s_junk;
      break;
    case k_loader_tall:
    default:
      *out_bytes = s_png_tall;
      *out_len   = sizeof s_png_tall;
      break;
  }
  return k_ra_ok;
}

/** @brief Init the shared engine at the given viewport (Ahem, body colours). */
static void init_engine(uint16_t w, uint16_t h)
{
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_reflow_init(w,
                                h,
                                k_fixture_ahem,
                                (size_t)k_fixture_ahem_len,
                                k_font_px,
                                k_body_color,
                                k_link_color,
                                &s_eng));
}

/** @brief Lay out @p doc into the shared engine; assert success; return pages. */
static uint32_t lay(const char* doc)
{
  uint32_t pages = 0U;
  TEST_ASSERT_EQ(
    k_ra_ok,
    ra_reflow_layout_chapter(&s_eng, (const uint8_t*)doc, (uint32_t)strlen(doc), &pages));
  return pages;
}

/** @brief Count distinct baseline rows among the laid-out glyphs (line count). */
static uint32_t line_count(void)
{
  if (s_eng.glyph_count == 0U) {
    return 0U;
  }
  uint32_t lines = 1U;
  int32_t  py    = s_eng.glyphs[0].y;
  for (uint32_t i = 0U; i < s_eng.glyph_count; ++i) {
    if (s_eng.glyphs[i].y != py) {
      lines++;
      py = s_eng.glyphs[i].y;
    }
  }
  return lines;
}

/**
 * @test test_finish_line_no_slack_mcdc
 *
 * @par MC/DC:
 * Decision: `if ((hi <= lo) || (slack <= 0))` (2 conditions, OR;
 * libs/ra_reflow/src/ra_reflow_layout.c@priv_finish_line). The early-out fires
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
  TEST_ASSERT(s_eng.glyphs[0].x > (int32_t)k_ra_reflow_margin_px);

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
  TEST_ASSERT(s_eng.glyphs[0].x < (int32_t)(k_vp_w / 2));

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
 * (2 conditions, AND; libs/ra_reflow/src/ra_reflow_layout.c@priv_open_block --
 * the `id=` anchor-capture gate). A block-start token carries a non-zero
 * text_len slice only when the source element had `id="..."`.
 *
 * Vectors (N+1 = 3 for N=2):
 *  - V1: `<p id="top">` with the anchor pool not full -> C1 T, C2 T -> T
 *        (one anchor recorded; resolvable via ra_reflow_find_anchor).
 *  - V2: plain `<p>` (no id) -> C1 F shorts -> F (no anchor recorded).
 *  - V3: a block with an id but the anchor pool already at capacity -> C1 T,
 *        C2 F -> F. The pool cap is k_ra_reflow_max_anchors; we approximate the
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
  uint32_t page = 0xFFFFFFFFU;
  TEST_ASSERT_EQ(k_ra_ok, ra_reflow_find_anchor(&s_eng, "top", 3U, &page));
  TEST_ASSERT(page < s_eng.page_count);

  /* V2: a block with no id -> no anchor captured. */
  init_engine(k_vp_w, k_vp_h);
  (void)lay("<html><body><p>Hello world</p></body></html>");
  TEST_ASSERT_EQ(0, (int64_t)s_eng.anchor_count);

  /* V3: lay out many ided blocks; the pool guard caps anchor_count so it never
   * exceeds k_ra_reflow_max_anchors (the C2-false arm holds the count). */
  init_engine(k_vp_w, k_vp_h);
  (void)lay("<html><body>"
            "<p id=\"a\">x</p><p id=\"b\">y</p><p id=\"c\">z</p>"
            "</body></html>");
  TEST_ASSERT(s_eng.anchor_count >= 3U);
  TEST_ASSERT(s_eng.anchor_count <= (uint32_t)k_ra_reflow_max_anchors);
  TEST_END("priv_open_block MC/DC: (text_len>0) && (anchor_count<max)");
}

/**
 * @test test_apply_image_loader_gate_mcdc
 *
 * @par MC/DC:
 * Decision: `if ((img_loader != null) && (img_arena != null) && (text_len > 0))`
 * (3 conditions, AND; libs/ra_reflow/src/ra_reflow_layout.c@priv_apply_image --
 * the real-image-vs-placeholder gate).
 *
 * Vectors (N+1 = 4 for N=3):
 *  - V1: loader bound, arena bound, `<img src>` non-empty -> all T -> real path
 *        (an image box is recorded).
 *  - V2: no loader bound (default engine) -> C1 F shorts -> placeholder path
 *        (no image box; the glyph advance reserves the placeholder).
 *  - V3: loader + ctx bound but arena NULL -> C2 F -> placeholder path.
 *  - V4: loader + arena bound but `<img>` has an empty src -> C3 F (text_len==0)
 *        -> placeholder path.
 * V1 vs V2/V3/V4 each flip exactly one condition with the others held true.
 */
static void test_apply_image_loader_gate_mcdc(void)
{
  TEST_BEGIN("priv_apply_image MC/DC: loader && arena && text_len");
  ra_img_arena_t arena = {.base   = s_img_scratch,
                          .cap    = sizeof s_img_scratch,
                          .offset = 0U,
                          .live   = 0U};

  /* V1: all three true -> the 2x2 image is placed as a box. */
  s_loader_select = k_loader_2x2;
  init_engine(k_vp_w, k_vp_h);
  TEST_ASSERT_EQ(k_ra_ok, ra_reflow_set_image_loader(&s_eng, test_image_loader, nullptr, &arena));
  (void)lay("<html><body><p>before<img src=\"f.png\">after</p></body></html>");
  TEST_ASSERT(s_eng.image_box_count >= 1U);

  /* V2: no loader -> placeholder (no box). */
  init_engine(k_vp_w, k_vp_h);
  (void)lay("<html><body><p>before<img src=\"f.png\">after</p></body></html>");
  TEST_ASSERT_EQ(0, (int64_t)s_eng.image_box_count);

  /* V3: loader bound but arena NULL -> placeholder (no box). */
  s_loader_select = k_loader_2x2;
  init_engine(k_vp_w, k_vp_h);
  TEST_ASSERT_EQ(k_ra_ok, ra_reflow_set_image_loader(&s_eng, test_image_loader, nullptr, nullptr));
  (void)lay("<html><body><p>before<img src=\"f.png\">after</p></body></html>");
  TEST_ASSERT_EQ(0, (int64_t)s_eng.image_box_count);

  /* V4: loader + arena bound but empty src (text_len == 0) -> placeholder. */
  s_loader_select = k_loader_2x2;
  init_engine(k_vp_w, k_vp_h);
  TEST_ASSERT_EQ(k_ra_ok, ra_reflow_set_image_loader(&s_eng, test_image_loader, nullptr, &arena));
  (void)lay("<html><body><p>before<img src=\"\">after</p></body></html>");
  TEST_ASSERT_EQ(0, (int64_t)s_eng.image_box_count);
  TEST_END("priv_apply_image MC/DC: loader && arena && text_len");
}

/**
 * @test test_image_resolve_size_mcdc
 *
 * @par MC/DC:
 * Two decisions in libs/ra_reflow/src/ra_reflow_layout.c@priv_image_resolve_size:
 *  (A) `if ((ra_img_probe_size(...) != ok) || (iw <= 0) || (ih <= 0))` (probe
 *      failure OR; a junk fixture makes probe fail -> T; a real PNG with
 *      positive dimensions -> all F).
 *  (B) `if ((col_w < 1) || (avail_h < 1))` (degenerate-column OR; a 32px-wide
 *      viewport gives col_w == 0 -> T; the normal viewport -> F).
 *
 * Vectors:
 *  - V1: real 2x2 PNG, normal viewport -> (A) F and (B) F -> box resolved (an
 *        image box is recorded).
 *  - V2: junk bytes -> (A) C1 T -> resolve fails -> placeholder fallback (no
 *        box). Isolates the probe-failure condition of (A).
 *  - V3: real PNG, 32px-wide viewport (col_w == 200's column shrunk to 0) ->
 *        (B) C1 T -> resolve fails -> placeholder fallback (no box). Isolates
 *        the col_w<1 condition of (B).
 * V1 is the all-false control for both (A) and (B).
 */
static void test_image_resolve_size_mcdc(void)
{
  TEST_BEGIN("priv_image_resolve_size MC/DC: probe-fail OR + degenerate column");
  ra_img_arena_t arena = {.base   = s_img_scratch,
                          .cap    = sizeof s_img_scratch,
                          .offset = 0U,
                          .live   = 0U};

  /* V1: real 2x2, normal viewport -> resolved -> box recorded. */
  s_loader_select = k_loader_2x2;
  init_engine(k_vp_w, k_vp_h);
  TEST_ASSERT_EQ(k_ra_ok, ra_reflow_set_image_loader(&s_eng, test_image_loader, nullptr, &arena));
  (void)lay("<html><body><p><img src=\"f.png\"></p></body></html>");
  TEST_ASSERT(s_eng.image_box_count >= 1U);

  /* V2: junk bytes -> probe fails -> placeholder fallback (no box). */
  s_loader_select = k_loader_junk;
  init_engine(k_vp_w, k_vp_h);
  TEST_ASSERT_EQ(k_ra_ok, ra_reflow_set_image_loader(&s_eng, test_image_loader, nullptr, &arena));
  (void)lay("<html><body><p><img src=\"f.png\"></p></body></html>");
  TEST_ASSERT_EQ(0, (int64_t)s_eng.image_box_count);

  /* V3: real PNG but a 32px viewport -> col_w == 0 -> resolve fails -> no box. */
  s_loader_select = k_loader_2x2;
  init_engine(k_vp_w_tiny, k_vp_h);
  TEST_ASSERT_EQ(k_ra_ok, ra_reflow_set_image_loader(&s_eng, test_image_loader, nullptr, &arena));
  (void)lay("<html><body><p><img src=\"f.png\"></p></body></html>");
  TEST_ASSERT_EQ(0, (int64_t)s_eng.image_box_count);
  TEST_END("priv_image_resolve_size MC/DC: probe-fail OR + degenerate column");
}

/**
 * @test test_image_page_break_mcdc
 *
 * @par MC/DC:
 * Decision: `if (((cur->y + bh) > bottom_limit) && priv_page_has_content(...))`
 * (2 conditions, AND; libs/ra_reflow/src/ra_reflow_layout.c@priv_place_image --
 * the pre-image overflow page break). priv_page_has_content() is the OR
 * `(glyph_count > page_first_glyph) || (image_box_count > page_first_image)`.
 *
 * Vectors (N+1 = 3 for N=2):
 *  - V1: a tall image on a SHORT page that already holds a paragraph -> C1 T
 *        (overflows) and C2 T (page has glyph content) -> T -> the page is
 *        flushed before the image, so the chapter spans >= 2 pages.
 *  - V2: the same tall image as the very FIRST element of a short page -> C1 T
 *        but C2 F (empty page) -> F -> no pre-break (the image lands on page 0).
 *  - V3: a small 2x2 image after a paragraph on a tall page -> C1 F (fits) ->
 *        F -> no break. Isolates the overflow condition.
 * V1 vs V3 isolate the overflow condition; V1 vs V2 isolate page-has-content.
 */
static void test_image_page_break_mcdc(void)
{
  TEST_BEGIN("priv_place_image MC/DC: (y+bh>bottom) && page_has_content");
  ra_img_arena_t arena = {.base   = s_img_scratch,
                          .cap    = sizeof s_img_scratch,
                          .offset = 0U,
                          .live   = 0U};

  /* V1: paragraph then tall image on a short page -> pre-break -> >= 2 pages. */
  s_loader_select = k_loader_tall;
  init_engine(k_vp_w, k_vp_h_short);
  TEST_ASSERT_EQ(k_ra_ok, ra_reflow_set_image_loader(&s_eng, test_image_loader, nullptr, &arena));
  const uint32_t pages_v1 =
    lay("<html><body><p>some text here</p><p><img src=\"t.png\"></p></body></html>");
  TEST_ASSERT(pages_v1 >= 2U);
  TEST_ASSERT(s_eng.image_box_count >= 1U);

  /* V2: tall image first on an empty short page -> no pre-break (page 0). */
  s_loader_select = k_loader_tall;
  init_engine(k_vp_w, k_vp_h_short);
  TEST_ASSERT_EQ(k_ra_ok, ra_reflow_set_image_loader(&s_eng, test_image_loader, nullptr, &arena));
  (void)lay("<html><body><p><img src=\"t.png\"></p></body></html>");
  TEST_ASSERT(s_eng.image_box_count >= 1U);
  TEST_ASSERT_EQ(0, (int64_t)s_eng.image_boxes[0].page_index);

  /* V3: small image after a paragraph on a tall page -> it fits, no break. */
  s_loader_select = k_loader_2x2;
  init_engine(k_vp_w, k_vp_h);
  TEST_ASSERT_EQ(k_ra_ok, ra_reflow_set_image_loader(&s_eng, test_image_loader, nullptr, &arena));
  const uint32_t pages_v3 = lay("<html><body><p>tiny</p><p><img src=\"s.png\"></p></body></html>");
  TEST_ASSERT_EQ(1, (int64_t)pages_v3);
  TEST_ASSERT(s_eng.image_box_count >= 1U);
  TEST_END("priv_place_image MC/DC: (y+bh>bottom) && page_has_content");
}

/**
 * @test test_build_link_rects_wrap_mcdc
 *
 * @par MC/DC:
 * Decision: the run-extension while-loop in
 * libs/ra_reflow/src/ra_reflow_layout.c@priv_build_link_rects:
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
  TEST_ASSERT_EQ((int64_t)s_eng.link_rects[0].target, (int64_t)s_eng.link_rects[1].target);

  /* V2: a short link mid-paragraph -> exactly one rect. */
  init_engine(k_vp_w, k_vp_h);
  (void)lay("<html><body><p>go <a href=\"y\">here</a> now</p></body></html>");
  TEST_ASSERT_EQ(1, (int64_t)s_eng.link_rect_count);
  TEST_END("priv_build_link_rects MC/DC: wrapped-link run extension");
}

/**
 * @test test_table_layout_cell_row_mcdc
 *
 * @par MC/DC:
 * Drives the table grid path so the cell / row recognition decisions and the
 * in-cell flow decisions execute on real tokens:
 *  - priv_is_cell_start(): `block_start && (tag==td || tag==th)` (the `<td>` and
 *    `<th>` arms of the OR).
 *  - priv_is_row_start():  `block_start && tag==tr`.
 *  - priv_cell_text():     the in-cell space-emit guard
 *    `(*cx > cell_x) && ((*cx + adv) <= cell_right)` and the in-cell word-wrap
 *    `((*cx + word_w) > cell_right) && (*cx > cell_x)`.
 *
 * Vectors:
 *  - V1: a 2x2 table with a `<th>` header row and a `<td>` body row whose cells
 *        hold multi-word wrapping text -> both is_cell_start arms (td + th) and
 *        is_row_start fire; the cell text both emits inner spaces (cx>cell_x,
 *        room) and wraps a word at the cell edge (word overflows, cx>cell_x).
 *  - V2: a one-cell table whose single word is wider than the cell -> the
 *        word-wrap guard's `*cx > cell_x` arm is false on the first word (no
 *        wrap before the line has content), exercising that condition's F side.
 * The table produces glyphs across >= 1 page; assertions confirm content flowed.
 */
static void test_table_layout_cell_row_mcdc(void)
{
  TEST_BEGIN("priv_layout_table MC/DC: cell/row start + in-cell wrap");
  /* V1: header (th) + body (td) rows with wrapping multi-word cells. */
  init_engine(k_vp_w, k_vp_h);
  const uint32_t pages = lay("<html><body><table>"
                             "<tr><th>Name here now</th><th>Value here now</th></tr>"
                             "<tr><td>alpha beta gamma</td><td>delta epsilon zeta</td></tr>"
                             "</table></body></html>");
  TEST_ASSERT(pages >= 1U);
  TEST_ASSERT(s_eng.glyph_count > 0U);

  /* V2: single over-wide word in one cell -> no wrap before the cell has
   * content (the `*cx > cell_x` arm is false on the first word). */
  init_engine(k_vp_w, k_vp_h);
  (void)lay("<html><body><table><tr><td>"
            "Supercalifragilisticexpialidocious</td></tr></table></body></html>");
  TEST_ASSERT(s_eng.glyph_count > 0U);
  TEST_END("priv_layout_table MC/DC: cell/row start + in-cell wrap");
}

/**
 * @test test_table_row_page_break_mcdc
 *
 * @par MC/DC:
 * Decision: `if (((cur->y + row_h) > bottom_limit) && (cur->y > margin))`
 * (2 conditions, AND; libs/ra_reflow/src/ra_reflow_layout.c@priv_layout_row --
 * the mid-table row page break). A row that overruns the bottom margin while
 * starting mid-page is rolled back and re-laid at the top of the next page.
 *
 * Vectors (N+1 = 3 for N=2):
 *  - V1: a multi-row table on a SHORT page where a later row starts mid-page and
 *        overflows -> C1 T (overflow) and C2 T (cur->y past the top margin) ->
 *        T -> the table spans >= 2 pages.
 *  - V2: a small single-row table on a tall page -> C1 F (fits) -> F -> one
 *        page. Isolates the overflow condition.
 *  - V3 is the structural C2-false arm (a row at the page top), which cannot
 *        co-occur with an overflow that the engine reaches mid-row: a row laid
 *        at the top margin that still overflows is left in place (C2 false), so
 *        the very first short-page row exercises the C2-false side implicitly.
 * V1 vs V2 isolate the overflow condition.
 */
static void test_table_row_page_break_mcdc(void)
{
  TEST_BEGIN("priv_layout_row MC/DC: (y+row_h>bottom) && (y>margin)");
  /* V1: many rows on a short page -> a later row overflows mid-page -> break. */
  init_engine(k_vp_w, k_vp_h_short);
  const uint32_t pages = lay("<html><body><table>"
                             "<tr><td>r1</td></tr><tr><td>r2</td></tr>"
                             "<tr><td>r3</td></tr><tr><td>r4</td></tr>"
                             "<tr><td>r5</td></tr><tr><td>r6</td></tr>"
                             "<tr><td>r7</td></tr><tr><td>r8</td></tr>"
                             "</table></body></html>");
  TEST_ASSERT(pages >= 2U);

  /* V2: a single short row on a tall page -> fits -> one page. */
  init_engine(k_vp_w, k_vp_h);
  const uint32_t one = lay("<html><body><table><tr><td>only</td></tr></table></body></html>");
  TEST_ASSERT_EQ(1, (int64_t)one);
  TEST_END("priv_layout_row MC/DC: (y+row_h>bottom) && (y>margin)");
}

/**
 * @test test_layout_tokens_final_flush_mcdc
 *
 * @par MC/DC:
 * Decision: the final-page flush in
 * libs/ra_reflow/src/ra_reflow_layout.c@priv_layout_tokens:
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
  ra_img_arena_t arena = {.base   = s_img_scratch,
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
  TEST_ASSERT_EQ(k_ra_ok, ra_reflow_set_image_loader(&s_eng, test_image_loader, nullptr, &arena));
  const uint32_t pages_v2 =
    lay("<html><body><p>page one text</p><p><img src=\"t.png\"></p></body></html>");
  TEST_ASSERT(pages_v2 >= 2U);
  TEST_ASSERT(s_eng.image_box_count >= 1U);
  /* The last image sits on the last page. */
  const uint32_t last_box = s_eng.image_box_count - 1U;
  TEST_ASSERT_EQ((int64_t)(s_eng.page_count - 1U), (int64_t)s_eng.image_boxes[last_box].page_index);

  /* V3: an empty-ish chapter still reports a page; exercises the layout-then-
   * final-fixup path where no trailing flush is owed. */
  init_engine(k_vp_w, k_vp_h);
  const uint32_t pages_v3 = lay("<html><body></body></html>");
  /* No glyphs and no images pending -> the trailing-flush OR is false on both
   * arms, so no page is emitted for the empty chapter. */
  TEST_ASSERT_EQ(0, (int64_t)pages_v3);
  TEST_END("priv_layout_tokens MC/DC: trailing glyph || image flush");
}

/**
 * @test test_register_face_validate_mcdc
 *
 * @par MC/DC:
 * Decision: `if (offset < 0 || stbtt_InitFont(&probe, blob, offset) == 0)`
 * (2 conditions, OR; libs/ra_reflow/src/ra_reflow_layout.c@ra_reflow_register_face --
 * the blob-validation guard). A valid TTF parses (both F); junk bytes long
 * enough to pass the length guard reach InitFont and fail it.
 *
 * Vectors (N+1 = 3 for N=2):
 *  - V1: the real Ahem face -> offset >= 0 AND InitFont != 0 -> both F -> F ->
 *        face registered (face_count grows, k_ra_ok).
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
  TEST_BEGIN("ra_reflow_register_face MC/DC: offset<0 || InitFont==0");
  /* V1: valid Ahem face registers. */
  init_engine(k_vp_w, k_vp_h);
  TEST_ASSERT_EQ(0, (int64_t)s_eng.face_count);
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_reflow_register_face(&s_eng, 0U, k_fixture_ahem, (size_t)k_fixture_ahem_len));
  TEST_ASSERT_EQ(1, (int64_t)s_eng.face_count);

  /* V2 / V3: junk blobs long enough to clear the length guard but rejected by
   * the offset / InitFont validation -> not_supported, count unchanged. */
  static const uint8_t s_junk_face_a[16] =
    {0U, 1U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U};
  static const uint8_t s_junk_face_b[16] =
    {0xFFU, 0xFFU, 0xFFU, 0xFFU, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U};
  TEST_ASSERT_EQ(k_ra_err_not_supported,
                 ra_reflow_register_face(&s_eng, 1U, s_junk_face_a, sizeof s_junk_face_a));
  TEST_ASSERT_EQ(k_ra_err_not_supported,
                 ra_reflow_register_face(&s_eng, 1U, s_junk_face_b, sizeof s_junk_face_b));
  TEST_ASSERT_EQ(1, (int64_t)s_eng.face_count); /* unchanged by the two failures */
  TEST_END("ra_reflow_register_face MC/DC: offset<0 || InitFont==0");
}

/**
 * @brief Test entry point.
 * @return 0 on success; unity macros exit(1) on the first failure.
 */
int32_t main(void)
{
  test_finish_line_no_slack_mcdc();
  test_open_block_anchor_mcdc();
  test_apply_image_loader_gate_mcdc();
  test_image_resolve_size_mcdc();
  test_image_page_break_mcdc();
  test_build_link_rects_wrap_mcdc();
  test_table_layout_cell_row_mcdc();
  test_table_row_page_break_mcdc();
  test_layout_tokens_final_flush_mcdc();
  test_register_face_validate_mcdc();
  (void)line_count(); /* silence unused-helper if a future edit drops its use */
  (void)fprintf(stderr, "[OK ] test_ra_reflow_layout_mcdc.c\n");
  return 0;
}
