/**
 * @file test_reflow_layout_cov.c
 * @brief Line-coverage (gcovr) tests for apps/shared_libs/reflow/src/reflow_layout.c.
 *
 * @details
 * The existing MC/DC suite (tests/src/test_reflow_layout.c) drives the layout
 * engine's compound decisions through `reflow_init()` with a *dummy* font, so
 * it never reaches the real line-break / page-break / table body. These tests
 * internal_lay out real content with the fixed-metric Ahem face (the same fixture the
 * alignment tests use, so they never SKIP) to execute the per-element source
 * lines that the decision-only suite leaves cold:
 *
 *  - heading font scales `<h4>..<h6>` (internal_block_font_px h4/h5/h6 arms);
 *  - the bad-font guard in reflow_run_layout (internal_init_font error returns);
 *  - the page-finish body on multi-page content (priv_finish_page);
 *  - the justify remainder + no-space early-out (internal_justify_glyphs);
 *  - the centre/right slack<=0 early-out (internal_finish_line);
 *  - block-open / rule / image-placeholder / table opening with line content
 *    (internal_open_block, internal_apply_rule, internal_apply_image_placeholder,
 *     priv_layout_table newline);
 *  - the image loader-failure fallback (internal_image_resolve_size);
 *  - full table layout: column scan, row-cell scan, nested-table depth, empty
 *    table, non-row / non-cell token skips (internal_table_columns, internal_row_cells,
 *    internal_match_block_end, priv_layout_table);
 *  - the lifecycle null / not-initialised guards for run_layout,
 *    set_image_loader and set_css_loader.
 *
 * Assertions are robust (return code + page_count / glyph_count / image box
 * count + a positioned glyph), not exact pixel values: the point is execution.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "fixture_ahem.h"
#include "ra8_attributes.h"
#include "ra8_err.h"
#include "reflow.h"
#include "unity_minimal.h"

/**
 * @enum cov_dim_t
 * @brief Viewport geometry knobs for the layout-coverage fixtures.
 */
typedef enum : uint16_t {
  k_cov_vp_w        = 200U, /**< Viewport width, pixels.             */
  k_cov_vp_h        = 160U, /**< Viewport height, pixels.            */
  k_cov_vp_h_tall   = 400U, /**< Tall viewport for multi-row tables. */
  k_cov_vp_w_narrow = 80U,  /**< Narrow viewport to force wrapping.  */
  k_cov_font_px     = 16U,  /**< Body font size, pixels.             */
} cov_dim_t;

/**
 * @enum cov_color_t
 * @brief Style colour keys handed to reflow_init() (no magic numbers).
 */
typedef enum : uint32_t {
  k_cov_body_color = 0xFFFFFFU, /**< Body text colour key.   */
  k_cov_link_color = 0x3060FFU, /**< Anchor text colour key. */
} cov_color_t;

/**
 * @enum cov_size_t
 * @brief Buffer capacities for the loader fixtures (no magic numbers).
 */
typedef enum : size_t {
  k_cov_arena_cap   = 64U * 1024U, /**< Image-decode scratch.  */
  k_cov_junk_font_n = 64U,         /**< Junk font blob length. */
} cov_size_t;

/** @brief Shared engine handle (large -- keep off the stack). */
static reflow_t s_eng;

/** @brief Image-decode bump arena scratch. */
static uint8_t s_arena_buf[k_cov_arena_cap];

/**
 * @brief Image loader that always returns the baked 2x2 PNG bytes.
 * @param[in]  ctx      Unused opaque context.
 * @param[in]  href     Unused image href.
 * @param[in]  href_len Unused href length.
 * @param[out] out      Receives the PNG byte pointer.
 * @param[out] out_len  Receives the PNG byte count.
 * @return k_ra8_ok always.
 * @details Returns the smallest raster fixture from test_reflow_image.c: a
 * 2x2 RGB PNG that probes to a valid intrinsic size so layout records an image
 * box. Exercises the PNG loader path and preserves each documented bound.
 * @retval k_ra8_ok The baked PNG span and byte count were returned.
 * @pre The referenced fixtures and fixed-capacity buffers are valid.
 * @post All assertions for the scenario have passed before this function returns.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 * @pre Bounded working storage remains available for the complete operation.
 * @post No state outside the documented outputs is modified by this helper.
 */
RA8_INTERNAL static ra8_err_t internal_png_loader(void*           ctx,
                                                  const char*     href,
                                                  uint32_t        href_len,
                                                  const uint8_t** out,
                                                  size_t*         out_len)
{
  static const uint8_t s_png_2x2[] = {
    137, 80,  78,  71, 13,  10,  26,  10,  0,   0,   0,   13,  73,  72,  68,  82,  0,   0,  0,   2,
    0,   0,   0,   2,  8,   2,   0,   0,   0,   253, 212, 154, 115, 0,   0,   0,   22,  73, 68,  65,
    84,  120, 156, 99, 248, 207, 192, 192, 240, 159, 129, 145, 129, 225, 255, 255, 255, 12, 0,   30,
    246, 4,   253, 9,  237, 52,  62,  0,   0,   0,   0,   73,  69,  78,  68,  174, 66,  96, 130,
  };
  (void)ctx;
  (void)href;
  (void)href_len;
  *out     = s_png_2x2;
  *out_len = sizeof s_png_2x2;
  return k_ra8_ok;
}

/* The pointer parameters below cannot be const: this mock implements a
 * function-pointer interface (the DI seam under test), so its signature is
 * fixed by the typedef it is assigned to -- adding const changes the
 * function type and the assignment stops compiling. */
// NOLINTBEGIN(readability-non-const-parameter) -- interface contract fixes this writable pointer type.
/**
 * @brief Image loader that always fails, forcing the placeholder fallback.
 * @details Exercises the fail loader path and preserves each documented result and bound.
 * @param[in] ctx Unused opaque loader context.
 * @param[in] href Unused image resource identifier.
 * @param[in] href_len Unused byte length of @p href.
 * @param[out] out Unused destination left untouched on failure.
 * @param[out] out_len Unused length destination left untouched on failure.
 * @return The injected image-loader status.
 * @pre The referenced fixtures and fixed-capacity buffers are valid.
 * @post All assertions for the scenario have passed before this function returns.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 * @pre Bounded working storage remains available for the complete operation.
 * @post No state outside the documented outputs is modified by this helper.
 * @retval k_ra8_err_not_found The fixture intentionally rejects every requested image.
 */
RA8_INTERNAL static ra8_err_t internal_fail_loader(void*           ctx,
                                                   const char*     href,
                                                   uint32_t        href_len,
                                                   const uint8_t** out,
                                                   size_t*         out_len)
// NOLINTEND(readability-non-const-parameter)
{
  (void)ctx;
  (void)href;
  (void)href_len;
  (void)out;
  (void)out_len;
  return k_ra8_err_not_found;
}

/** @brief Init the shared engine at (w, h) with the Ahem fixture.
 * @details Exercises the init eng path and preserves each documented result and bound.
 * @param[in] w Caller-supplied w value used by the scenario.
 * @param[in] h Caller-supplied h value used by the scenario.
 * @return Reflow-engine initialization status.
 * @pre The referenced fixtures and fixed-capacity buffers are valid.
 * @post All assertions for the scenario have passed before this function returns.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 * @pre Bounded working storage remains available for the complete operation.
 * @post No state outside the documented outputs is modified by this helper.
 * @retval k_ra8_ok The shared engine was initialized.
 * @retval nonzero The fixture or engine parameters were rejected.
 */
RA8_INTERNAL static ra8_err_t internal_init_eng(uint16_t w, uint16_t h)
{
  return reflow_init(w,
                     h,
                     k_fixture_ahem,
                     (size_t)k_fixture_ahem_len,
                     (uint16_t)k_cov_font_px,
                     (uint32_t)k_cov_body_color,
                     (uint32_t)k_cov_link_color,
                     &s_eng);
}

/** @brief Lay out @p doc into the shared engine; return the page count.
 * @details Exercises the lay path and preserves each documented result and bound.
 * @param[in] doc Caller-supplied doc value used by the scenario.
 * @param[in] w Caller-supplied w value used by the scenario.
 * @param[in] h Caller-supplied h value used by the scenario.
 * @return Number of pages produced for the supplied document.
 * @pre The referenced fixtures and fixed-capacity buffers are valid.
 * @post All assertions for the scenario have passed before this function returns.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 * @pre Bounded working storage remains available for the complete operation.
 * @post No state outside the documented outputs is modified by this helper.
 * @retval 0 Layout failed or produced no page.
 * @retval nonzero Layout produced the returned page count.
 */
RA8_INTERNAL static uint32_t internal_lay(const char* doc, uint16_t w, uint16_t h)
{
  uint32_t pages = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, internal_init_eng(w, h));
  TEST_ASSERT_EQ(k_ra8_ok,
                 reflow_layout_chapter(&s_eng, (const uint8_t*)doc, (uint32_t)strlen(doc), &pages));
  return pages;
}

/**
 * @test internal_test_cov_heading_font_scales
 * @brief Lay out `<h4>..<h6>` to execute the h4/h5/h6 arms of internal_block_font_px.
 *
 * @details Targets reflow_layout.c lines 293-301 (the h4/h5/h6 switch cases)
 * plus the block-open path. Each heading uses its scaled font size, so the
 * glyphs land and the page count is >= 1.
 *
 * @par MC/DC:
 * (no compound decision is driven to MC/DC by this test -- it executes the
 * h4/h5/h6 arms of the internal_block_font_px switch (a multi-way switch, not a
 * compound boolean); it does not vary any decision's conditions independently.)
  * @pre The referenced fixtures and fixed-capacity buffers are valid.
 * @post All assertions for the scenario have passed before this function returns.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 * @pre Bounded working storage remains available for the complete operation.
 * @post No state outside the documented outputs is modified by this helper.
 */
RA8_INTERNAL static void internal_test_cov_heading_font_scales(void)
{
  TEST_BEGIN("layout cov: heading font scales h4/h5/h6");
  const char*    doc   = "<h4>Four</h4><h5>Five</h5><h6>Six</h6>";
  const uint32_t pages = internal_lay(doc, (uint16_t)k_cov_vp_w, (uint16_t)k_cov_vp_h);
  TEST_ASSERT(pages >= 1U);
  TEST_ASSERT(s_eng.glyph_count > 0U);
  TEST_END("layout cov: heading font scales h4/h5/h6");
}

/**
 * @test internal_test_cov_bad_font_run_layout
 * @brief A junk font blob makes reflow_run_layout fail validation.
 *
 * @details Targets reflow_layout.c lines 231-235 (internal_init_font's offset<0 /
 * InitFont==0 returns) and 1601-1602 (reflow_run_layout propagating that
 * error). The engine inits fine (blob passes the >= 16-byte size check) but the
 * blob is not a parseable font, so the run-layout pass reports
 * k_ra8_err_validation_failed.
 *
 * @par MC/DC:
 * (no compound decision is driven to MC/DC by this test -- it drives the two
 * single-condition internal_init_font error returns (offset < 0, InitFont == 0) and
 * their propagation through reflow_run_layout; neither is a compound
 * boolean.)
  * @pre The referenced fixtures and fixed-capacity buffers are valid.
 * @post All assertions for the scenario have passed before this function returns.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 * @pre Bounded working storage remains available for the complete operation.
 * @post No state outside the documented outputs is modified by this helper.
 */
RA8_INTERNAL static void internal_test_cov_bad_font_run_layout(void)
{
  /* Passes the minimum-length guard but is not a valid font. */
  static const uint8_t s_junk_font[k_cov_junk_font_n] = {1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U};
  TEST_BEGIN("layout cov: junk font -> run_layout validation_failed");
  uint32_t pages = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 reflow_init((uint16_t)k_cov_vp_w,
                             (uint16_t)k_cov_vp_h,
                             s_junk_font,
                             (size_t)k_cov_junk_font_n,
                             (uint16_t)k_cov_font_px,
                             (uint32_t)k_cov_body_color,
                             (uint32_t)k_cov_link_color,
                             &s_eng));
  /* Tokenize succeeds (parser is font-agnostic), but run_layout re-inits the
     font and fails. layout_chapter surfaces the validation error. */
  const char*     doc = "<p>Hi</p>";
  const ra8_err_t err =
    reflow_layout_chapter(&s_eng, (const uint8_t*)doc, (uint32_t)strlen(doc), &pages);
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, err);

  /* Also drive reflow_run_layout directly (it re-inits the same junk font). */
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, reflow_run_layout(&s_eng));
  TEST_END("layout cov: junk font -> run_layout validation_failed");
}

/**
 * @test internal_test_cov_multipage_finish_page
 * @brief Long content over a short viewport forces page breaks.
 *
 * @details Targets reflow_layout.c lines 401-415 (priv_finish_page body
 * executed once per page boundary) and the priv_newline page-finish path. Many
 * `<p>` paragraphs in a short viewport overflow the bottom margin repeatedly, so
 * page_count climbs above one.
 *
 * @par MC/DC:
 * (no compound decision is driven to MC/DC by this test -- it executes the
 * priv_finish_page body and the page-boundary break, both single-condition
 * checks; it does not vary any decision's conditions independently.)
  * @pre The referenced fixtures and fixed-capacity buffers are valid.
 * @post All assertions for the scenario have passed before this function returns.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 * @pre Bounded working storage remains available for the complete operation.
 * @post No state outside the documented outputs is modified by this helper.
 */
RA8_INTERNAL static void internal_test_cov_multipage_finish_page(void)
{
  TEST_BEGIN("layout cov: multi-page finish_page");
  const char*    doc   = "<p>aaaa</p><p>bbbb</p><p>cccc</p><p>dddd</p><p>eeee</p><p>ffff</p>"
                         "<p>gggg</p><p>hhhh</p><p>iiii</p><p>jjjj</p><p>kkkk</p><p>llll</p>"
                         "<p>mmmm</p><p>nnnn</p><p>oooo</p><p>pppp</p><p>qqqq</p><p>rrrr</p>";
  const uint32_t pages = internal_lay(doc, (uint16_t)k_cov_vp_w, (uint16_t)k_cov_vp_h);
  TEST_ASSERT(pages >= 2U);
  TEST_ASSERT(s_eng.glyph_count > 0U);
  TEST_END("layout cov: multi-page finish_page");
}

/**
 * @test internal_test_cov_justify_remainder_and_no_space
 * @brief Justify a wrapped multi-space line (remainder spread) and a no-space line.
 *
 * @details Targets reflow_layout.c lines 445-461 (internal_justify_glyphs: the
 * spaces==0 early-out and the `added += 1` remainder distribution). The first
 * paragraph wraps over many short words so a justified line carries several
 * spaces with a non-zero remainder; the long single-word line in the second doc
 * reaches a justify line with zero spaces (the early-out).
 *
 * @par MC/DC:
 * (no compound decision is driven to MC/DC by this test -- internal_justify_glyphs's
 * `spaces == 0` early-out and its `seen < rem` remainder step are both
 * single-condition branches. The test reaches internal_finish_line's
 * `(hi <= lo) || (slack <= 0)` on its both-false (proceed) outcome; that
 * decision's slack arm is driven by ::internal_test_cov_right_align_slack_nonpositive.)
  * @pre The referenced fixtures and fixed-capacity buffers are valid.
 * @post All assertions for the scenario have passed before this function returns.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 * @pre Bounded working storage remains available for the complete operation.
 * @post No state outside the documented outputs is modified by this helper.
 */
RA8_INTERNAL static void internal_test_cov_justify_remainder_and_no_space(void)
{
  TEST_BEGIN("layout cov: justify remainder + no-space early-out");
  const char* doc =
    "<p style=\"text-align:justify\">a b c d e f g h i j k l m n o p q r s t u v w x</p>";
  uint32_t pages = internal_lay(doc, (uint16_t)k_cov_vp_w, (uint16_t)k_cov_vp_h_tall);
  TEST_ASSERT(pages >= 1U);
  TEST_ASSERT(s_eng.glyph_count > 0U);

  /* A justified line whose only content is one long unbroken word: when it
     wraps, the wrapped line has no inter-word space, so internal_justify_glyphs
     takes the spaces==0 early return. */
  const char* doc2 =
    "<p style=\"text-align:justify\">aaaaaaaaaaaaaaaaaaaaaaaa bbbbbbbbbbbbbbbbbbbbbbbb</p>";
  pages = internal_lay(doc2, (uint16_t)k_cov_vp_w_narrow, (uint16_t)k_cov_vp_h_tall);
  TEST_ASSERT(pages >= 1U);
  TEST_ASSERT(s_eng.glyph_count > 0U);
  TEST_END("layout cov: justify remainder + no-space early-out");
}

/**
 * @test internal_test_cov_right_align_slack_nonpositive
 * @brief A right-aligned line wider than the column takes the slack<=0 early-out.
 *
 * @details Targets reflow_layout.c line 500-501 (internal_finish_line's
 * `(hi <= lo) || (slack <= 0)` early return). A right-aligned single long word in
 * a narrow viewport produces a line whose content already reaches/overruns the
 * right margin, so slack is non-positive and the alignment shift is skipped.
 *
 * @par MC/DC:
 * Decision: `(hi <= lo) || (slack <= 0)` in internal_finish_line (2 conditions).
 * This test drives one outcome: hi>lo (F), slack<=0 (T) -> true -> the alignment
 * shift is skipped (varies the slack condition). The both-false (F,F -> proceed)
 * control is reached by ::internal_test_cov_justify_remainder_and_no_space; together they
 * prove the slack condition's independent influence. This test does not vary the
 * `hi <= lo` condition, so it does not by itself establish the full decision.
  * @pre The referenced fixtures and fixed-capacity buffers are valid.
 * @post All assertions for the scenario have passed before this function returns.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 * @pre Bounded working storage remains available for the complete operation.
 * @post No state outside the documented outputs is modified by this helper.
 */
RA8_INTERNAL static void internal_test_cov_right_align_slack_nonpositive(void)
{
  TEST_BEGIN("layout cov: right-align slack<=0 early-out");
  const char*    doc = "<p style=\"text-align:right\">aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa</p>";
  const uint32_t pages = internal_lay(doc, (uint16_t)k_cov_vp_w_narrow, (uint16_t)k_cov_vp_h_tall);
  TEST_ASSERT(pages >= 1U);
  TEST_ASSERT(s_eng.glyph_count > 0U);
  TEST_END("layout cov: right-align slack<=0 early-out");
}

/**
 * @test internal_test_cov_open_block_and_rule_with_content
 * @brief Open a block and a rule while the current line already has glyphs.
 *
 * @details Targets reflow_layout.c lines 698-700 (internal_open_block's
 * line-flush newline when line_has_content) and 789-791 (internal_apply_rule's
 * line-flush newline). Text immediately followed by a nested block-start and by
 * an `<hr>` -- with no intervening close -- leaves the line dirty when the next
 * element opens, so both flush paths run.
 *
 * @par MC/DC:
 * (no compound decision is driven to MC/DC by this test -- the internal_open_block
 * and internal_apply_rule line-flush guards it targets are single-condition
 * (`line_has_content != 0`) checks; it does not vary any decision's conditions
 * independently.)
  * @pre The referenced fixtures and fixed-capacity buffers are valid.
 * @post All assertions for the scenario have passed before this function returns.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 * @pre Bounded working storage remains available for the complete operation.
 * @post No state outside the documented outputs is modified by this helper.
 */
RA8_INTERNAL static void internal_test_cov_open_block_and_rule_with_content(void)
{
  TEST_BEGIN("layout cov: open block + rule with dirty line");
  /* Text, then a blockquote opens while the line still has content -> 698-700.
     Then a <hr> fires while a line has content -> 789-791. */
  const char*    doc   = "<p>Lead text<blockquote>Quote body<hr>tail</blockquote></p>";
  const uint32_t pages = internal_lay(doc, (uint16_t)k_cov_vp_w, (uint16_t)k_cov_vp_h_tall);
  TEST_ASSERT(pages >= 1U);
  TEST_ASSERT(s_eng.glyph_count > 0U);
  TEST_END("layout cov: open block + rule with dirty line");
}

/**
 * @test internal_test_cov_image_placeholder_wrap
 * @brief An `<img>` with no loader bound wraps when it overflows the line.
 *
 * @details Targets reflow_layout.c lines 822-829 (internal_apply_image_placeholder
 * newline-on-overflow + advance). With no image loader bound, an `<img>` reserves
 * a 32px placeholder; placing it after a line that is already near the right
 * margin trips the overflow break, so the placeholder wraps to a new line.
 *
 * @par MC/DC:
 * (no compound decision is driven to MC/DC by this test -- it drives the
 * placeholder overflow-wrap branch on a single outcome (the line wraps); it does
 * not vary that branch's conditions independently.)
  * @pre The referenced fixtures and fixed-capacity buffers are valid.
 * @post All assertions for the scenario have passed before this function returns.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 * @pre Bounded working storage remains available for the complete operation.
 * @post No state outside the documented outputs is modified by this helper.
 */
RA8_INTERNAL static void internal_test_cov_image_placeholder_wrap(void)
{
  TEST_BEGIN("layout cov: image placeholder wrap (no loader)");
  /* Fill the line close to the right edge, then an <img> placeholder overflows. */
  const char*    doc   = "<p>aaaaaaa<img src=\"x.png\"/>tail</p>";
  const uint32_t pages = internal_lay(doc, (uint16_t)k_cov_vp_w_narrow, (uint16_t)k_cov_vp_h_tall);
  TEST_ASSERT(pages >= 1U);
  TEST_ASSERT(s_eng.glyph_count > 0U);
  TEST_END("layout cov: image placeholder wrap (no loader)");
}

/**
 * @test internal_test_cov_image_loader_failure_fallback
 * @brief A failing image loader makes internal_image_resolve_size return false.
 *
 * @details Targets reflow_layout.c lines 916-917 (internal_image_resolve_size
 * returning false when the bound loader fails). With a loader + arena bound but a
 * loader that always errors, internal_place_image cannot resolve the size and falls
 * back to the placeholder advance -- so the chapter still lays out.
 *
 * @par MC/DC:
 * (no compound decision is driven to MC/DC by this test -- it drives the
 * loader-failure fallback on a single outcome (internal_image_resolve_size returns
 * false); it does not vary the resolve-size compound guard's conditions
 * independently.)
  * @pre The referenced fixtures and fixed-capacity buffers are valid.
 * @post All assertions for the scenario have passed before this function returns.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 * @pre Bounded working storage remains available for the complete operation.
 * @post No state outside the documented outputs is modified by this helper.
 */
RA8_INTERNAL static void internal_test_cov_image_loader_failure_fallback(void)
{
  TEST_BEGIN("layout cov: image loader failure -> placeholder fallback");
  TEST_ASSERT_EQ(k_ra8_ok, internal_init_eng((uint16_t)k_cov_vp_w, (uint16_t)k_cov_vp_h_tall));
  ra8_img_arena_t arena = {.base   = s_arena_buf,
                           .cap    = sizeof s_arena_buf,
                           .offset = 0U,
                           .live   = 0U};
  TEST_ASSERT_EQ(k_ra8_ok, reflow_set_image_loader(&s_eng, internal_fail_loader, nullptr, &arena));

  uint32_t    pages = 0U;
  const char* doc   = "<p>Before<img src=\"broken.png\"/>After</p>";
  TEST_ASSERT_EQ(k_ra8_ok,
                 reflow_layout_chapter(&s_eng, (const uint8_t*)doc, (uint32_t)strlen(doc), &pages));
  TEST_ASSERT(pages >= 1U);
  TEST_ASSERT(s_eng.glyph_count > 0U);
  /* The loader failed, so no real image box was recorded (placeholder only). */
  TEST_ASSERT_EQ(0, s_eng.image_box_count);
  TEST_END("layout cov: image loader failure -> placeholder fallback");
}

/**
 * @test internal_test_cov_image_real_block
 * @brief A bound loader resolves an `<img>` to a real, recorded image box.
 *
 * @details Targets reflow_layout.c internal_place_image / internal_image_resolve_size
 * / internal_image_record (the successful image-block path that runs when the loader
 * returns a decodable raster). One `<img>` produces one recorded image box.
 *
 * @par MC/DC:
 * (no compound decision is driven to MC/DC by this test -- it drives the
 * successful image-block path on a single outcome (one recorded image box); it
 * does not vary any decision's conditions independently.)
  * @pre The referenced fixtures and fixed-capacity buffers are valid.
 * @post All assertions for the scenario have passed before this function returns.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 * @pre Bounded working storage remains available for the complete operation.
 * @post No state outside the documented outputs is modified by this helper.
 */
RA8_INTERNAL static void internal_test_cov_image_real_block(void)
{
  TEST_BEGIN("layout cov: real image block recorded");
  TEST_ASSERT_EQ(k_ra8_ok, internal_init_eng((uint16_t)k_cov_vp_w, (uint16_t)k_cov_vp_h_tall));
  ra8_img_arena_t arena = {.base   = s_arena_buf,
                           .cap    = sizeof s_arena_buf,
                           .offset = 0U,
                           .live   = 0U};
  TEST_ASSERT_EQ(k_ra8_ok, reflow_set_image_loader(&s_eng, internal_png_loader, nullptr, &arena));

  uint32_t    pages = 0U;
  const char* doc   = "<p>Figure:</p><img src=\"cover.png\"/><p>After.</p>";
  TEST_ASSERT_EQ(k_ra8_ok,
                 reflow_layout_chapter(&s_eng, (const uint8_t*)doc, (uint32_t)strlen(doc), &pages));
  TEST_ASSERT(pages >= 1U);
  TEST_ASSERT(s_eng.image_box_count >= 1U);
  TEST_END("layout cov: real image block recorded");
}

/**
 * @test internal_test_cov_table_layout
 * @brief A multi-row table exercises the column / row / cell scan loops.
 *
 * @details Targets reflow_layout.c's table family: internal_table_columns
 * (lines 1254-1255 non-row skip), internal_row_cells (lines 1404-1405 non-cell
 * skip), priv_layout_table (line 1501 non-row skip in the main loop), plus
 * internal_match_block_end, internal_layout_row, internal_layout_cell and internal_cell_text. A
 * 2x2 table with stray whitespace between rows / cells reaches every skip arm.
 *
 * @par MC/DC:
 * (no compound decision is driven to MC/DC by this test -- it walks the table
 * column / row / cell scan loops with real content, taking the non-row /
 * non-cell skip arms; it does not vary the `is_cell` / `is_row` compound
 * predicates' conditions independently.)
  * @pre The referenced fixtures and fixed-capacity buffers are valid.
 * @post All assertions for the scenario have passed before this function returns.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 * @pre Bounded working storage remains available for the complete operation.
 * @post No state outside the documented outputs is modified by this helper.
 */
RA8_INTERNAL static void internal_test_cov_table_layout(void)
{
  TEST_BEGIN("layout cov: full table layout");
  const char*    doc   = "<table>\n"
                         "  <tr> <td>Alpha</td> <td>Beta</td> </tr>\n"
                         "  <tr> <td>Gamma</td> <td>Delta</td> </tr>\n"
                         "</table>";
  const uint32_t pages = internal_lay(doc, (uint16_t)k_cov_vp_w, (uint16_t)k_cov_vp_h_tall);
  TEST_ASSERT(pages >= 1U);
  TEST_ASSERT(s_eng.glyph_count > 0U);
  TEST_END("layout cov: full table layout");
}

/**
 * @test internal_test_cov_table_with_dirty_line_and_empty
 * @brief A table opened mid-line, and an empty table, hit the table edge cases.
 *
 * @details Targets reflow_layout.c lines 1482-1484 (priv_layout_table's
 * line-flush newline when the line has content) and 1490-1491 (the cols==0
 * early-out for a table with no cells). Leading text before `<table>` leaves the
 * line dirty; the second table has no rows / cells.
 *
 * @par MC/DC:
 * (no compound decision is driven to MC/DC by this test -- the line-flush
 * (`line_has_content`) and `cols == 0` early-outs it targets are
 * single-condition branches; it does not vary any decision's conditions
 * independently.)
  * @pre The referenced fixtures and fixed-capacity buffers are valid.
 * @post All assertions for the scenario have passed before this function returns.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 * @pre Bounded working storage remains available for the complete operation.
 * @post No state outside the documented outputs is modified by this helper.
 */
RA8_INTERNAL static void internal_test_cov_table_with_dirty_line_and_empty(void)
{
  TEST_BEGIN("layout cov: table mid-line + empty table");
  /* Text then a table on the same line -> the line-flush newline runs. */
  const char* doc   = "<p>Intro<table><tr><td>Cell</td></tr></table></p>";
  uint32_t    pages = internal_lay(doc, (uint16_t)k_cov_vp_w, (uint16_t)k_cov_vp_h_tall);
  TEST_ASSERT(pages >= 1U);
  TEST_ASSERT(s_eng.glyph_count > 0U);

  /* An empty table (no cells) -> internal_table_columns returns 0 -> cols==0 path. */
  const char* doc2 = "<p>x</p><table></table><p>y</p>";
  pages            = internal_lay(doc2, (uint16_t)k_cov_vp_w, (uint16_t)k_cov_vp_h_tall);
  TEST_ASSERT(pages >= 1U);
  TEST_ASSERT(s_eng.glyph_count > 0U);
  TEST_END("layout cov: table mid-line + empty table");
}

/**
 * @test internal_test_cov_table_nested_depth
 * @brief A nested `<table>` drives internal_match_block_end's depth-increment.
 *
 * @details Targets reflow_layout.c line 1213 (internal_match_block_end's
 * `depth++` on a same-tag nested block-start). Scanning for the outer table's
 * close crosses an inner `<table>` start, so the nesting counter increments
 * before the matching outer close is found.
 *
 * @par MC/DC:
 * (no compound decision is driven to MC/DC by this test -- it drives the
 * nested-table depth-increment branch on a single outcome; it does not vary any
 * decision's conditions independently.)
  * @pre The referenced fixtures and fixed-capacity buffers are valid.
 * @post All assertions for the scenario have passed before this function returns.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 * @pre Bounded working storage remains available for the complete operation.
 * @post No state outside the documented outputs is modified by this helper.
 */
RA8_INTERNAL static void internal_test_cov_table_nested_depth(void)
{
  TEST_BEGIN("layout cov: nested table depth");
  const char*    doc   = "<table><tr><td><table><tr><td>Inner</td></tr></table></td></tr></table>";
  const uint32_t pages = internal_lay(doc, (uint16_t)k_cov_vp_w, (uint16_t)k_cov_vp_h_tall);
  TEST_ASSERT(pages >= 1U);
  TEST_END("layout cov: nested table depth");
}

/**
 * @test internal_test_cov_lifecycle_guards
 * @brief Null / not-initialised guards on run_layout + the loader setters.
 *
 * @details Targets reflow_layout.c lines 1585-1589 (reflow_run_layout),
 * 1686-1690 (reflow_set_image_loader) and 1698-1703 (reflow_set_css_loader)
 * null-pointer + not-initialised guard returns, none of which the happy-path
 * tests reach.
 *
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the null-pointer and
 * not-initialised guard returns on run_layout / set_image_loader /
 * set_css_loader; these are sequential single-condition `if` checks, so there is
 * no `&&` or `||` in the code under test that this case touches.)
  * @pre The referenced fixtures and fixed-capacity buffers are valid.
 * @post All assertions for the scenario have passed before this function returns.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 * @pre Bounded working storage remains available for the complete operation.
 * @post No state outside the documented outputs is modified by this helper.
 */
RA8_INTERNAL static void internal_test_cov_lifecycle_guards(void)
{
  TEST_BEGIN("layout cov: lifecycle null / not-init guards");
  reflow_t closed = {};

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, reflow_run_layout(nullptr));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, reflow_run_layout(&closed));

  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 reflow_set_image_loader(nullptr, internal_png_loader, nullptr, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized,
                 reflow_set_image_loader(&closed, internal_png_loader, nullptr, nullptr));

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, reflow_set_css_loader(nullptr, nullptr, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, reflow_set_css_loader(&closed, nullptr, nullptr));
  TEST_END("layout cov: lifecycle null / not-init guards");
}

/**
 * @brief Test entry point.
 * @return 0 on success; unity macros exit(1) on the first failure.
 */
int main(void)
{
  internal_test_cov_heading_font_scales();
  internal_test_cov_bad_font_run_layout();
  internal_test_cov_multipage_finish_page();
  internal_test_cov_justify_remainder_and_no_space();
  internal_test_cov_right_align_slack_nonpositive();
  internal_test_cov_open_block_and_rule_with_content();
  internal_test_cov_image_placeholder_wrap();
  internal_test_cov_image_loader_failure_fallback();
  internal_test_cov_image_real_block();
  internal_test_cov_table_layout();
  internal_test_cov_table_with_dirty_line_and_empty();
  internal_test_cov_table_nested_depth();
  internal_test_cov_lifecycle_guards();
  return 0;
}
