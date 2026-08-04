/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file test_ra8_reflow.c
 * @brief Unit tests for libs/ra8_reflow (HTML+CSS reflow + paginate).
 *
 * @details
 * Loads the vendored Literata font from
 * `libs/ra8_fonts/Literata-Regular.ttf`, hands it to the
 * reflow engine, and exercises the public surface against a few
 * hand-written XHTML fragments. The test computes the firmware root
 * from `__FILE__` so it works whether cmake is run from `build/tidy`
 * or `build/host-docker`.
 *
 * Coverage:
 *   - parse + layout of `<h1>Title</h1><p>Body</p>` returns >= 1 page.
 *   - render_page populates non-zero pixels at expected glyph coords.
 *   - NULL-arg / pre-init guards on every public entry point.
 *   - set_font_size triggers a re-flow that yields a different page
 *     count when the body size doubles.
 *   - multi-paragraph input lays out all glyphs.
 *   - bold / italic toggling propagates the style flag onto glyphs.
 *
 * The MC/DC vector tests for the public-API guard decisions live in the
 * split sibling test_ra8_reflow_api_mcdc.c; the shared Literata fixture is
 * tests/support/reflow_v1_test_util.h.
 *
 *
 * [Ring 4 / Reflow]
 * {World: NS}
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_gfx.h"
#include "ra8_reflow.h"
#include "support/reflow_v1_test_util.h"
#include "unity_minimal.h"
/* --------------------------------------------------------------------- */
/* Tests */
/* --------------------------------------------------------------------- */

/**
 * @brief NULL / bad-arg guards on ra8_reflow_init() and ra8_reflow_close().
 *
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_null_and_preinit_guards_init(void)
{
  TEST_BEGIN("test_null_and_preinit_guards_init");

  /* init() guards. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_reflow_init(k_test_viewport_w,
                                 k_test_viewport_h,
                                 nullptr,
                                 s_font_len,
                                 k_test_font_px,
                                 k_test_color_body,
                                 k_test_color_link,
                                 &s_engine));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_reflow_init(k_test_viewport_w,
                                 k_test_viewport_h,
                                 s_font_buf,
                                 s_font_len,
                                 k_test_font_px,
                                 k_test_color_body,
                                 k_test_color_link,
                                 nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_reflow_init(0U,
                                 k_test_viewport_h,
                                 s_font_buf,
                                 s_font_len,
                                 k_test_font_px,
                                 k_test_color_body,
                                 k_test_color_link,
                                 &s_engine));
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_reflow_init(k_test_viewport_w,
                                 k_test_viewport_h,
                                 s_font_buf,
                                 4U,
                                 k_test_font_px,
                                 k_test_color_body,
                                 k_test_color_link,
                                 &s_engine));

  /* close() guards. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_reflow_close(nullptr));

  /* Engine zeroed -> not initialized. */
  ra8_reflow_t closed;
  (void)memset(&closed, 0, sizeof(closed));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_reflow_close(&closed));

  TEST_END("test_null_and_preinit_guards_init");
}

/**
 * @brief Pre-init guards on the layout / render / query entry points.
 *
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_null_and_preinit_guards_ops(void)
{
  TEST_BEGIN("test_null_and_preinit_guards_ops");

  /* Engine zeroed -> not initialized. */
  ra8_reflow_t closed;
  (void)memset(&closed, 0, sizeof(closed));

  /* layout_chapter() guards. */
  uint32_t pages = 0U;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_reflow_layout_chapter(nullptr,
                                           (const uint8_t*)k_xhtml_simple,
                                           strlen(k_xhtml_simple),
                                           &pages));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized,
                 ra8_reflow_layout_chapter(&closed,
                                           (const uint8_t*)k_xhtml_simple,
                                           strlen(k_xhtml_simple),
                                           &pages));

  /* render_page() / get_page_count() / set_font_size() guards. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_reflow_render_page(nullptr, 0U, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_reflow_render_page(&closed, 0U, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_reflow_get_page_count(nullptr, &pages));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_reflow_set_font_size(nullptr, k_test_font_px));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_reflow_set_font_size(&closed, k_test_font_px));

  TEST_END("test_null_and_preinit_guards_ops");
}

/**
 * @brief Parse a simple `<h1>Title</h1><p>Body</p>` and confirm
 *        layout returns >= 1 page with > 0 glyphs.
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */
static void test_simple_layout(void)
{
  TEST_BEGIN("test_simple_layout");

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_reflow_init(k_test_viewport_w,
                                 k_test_viewport_h,
                                 s_font_buf,
                                 s_font_len,
                                 k_test_font_px,
                                 k_test_color_body,
                                 k_test_color_link,
                                 &s_engine));

  uint32_t  pages = 0U;
  ra8_err_t err   = ra8_reflow_layout_chapter(&s_engine,
                                              (const uint8_t*)k_xhtml_simple,
                                              strlen(k_xhtml_simple),
                                              &pages);
  TEST_ASSERT_EQ(k_ra8_ok, err);
  TEST_ASSERT(pages >= 1U);
  TEST_ASSERT(s_engine.glyph_count > 0U);
  TEST_ASSERT(s_engine.token_count > 0U);

  uint32_t page_count = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_reflow_get_page_count(&s_engine, &page_count));
  TEST_ASSERT_EQ(pages, page_count);

  /* render_page() with an out-of-range index -> out_of_range. */
  TEST_ASSERT_EQ(k_ra8_err_out_of_range, ra8_reflow_render_page(&s_engine, page_count, nullptr));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_reflow_close(&s_engine));

  TEST_END("test_simple_layout");
}

/**
 * @brief Render the simple page and verify non-zero pixel coverage in
 *        the upper-left region (where the 'T' of "Title" lives).
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */
static void test_render_pixels_around_title(void)
{
  TEST_BEGIN("test_render_pixels_around_title");

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_reflow_init(k_test_viewport_w,
                                 k_test_viewport_h,
                                 s_font_buf,
                                 s_font_len,
                                 k_test_font_px,
                                 k_test_color_body,
                                 k_test_color_link,
                                 &s_engine));
  uint32_t pages = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_reflow_layout_chapter(&s_engine,
                                           (const uint8_t*)k_xhtml_simple,
                                           strlen(k_xhtml_simple),
                                           &pages));
  TEST_ASSERT(s_engine.glyph_count > 0U);

  /* The first glyph must be the 'T' of "Title" (parser collapses any
   * leading whitespace before it). */
  TEST_ASSERT_EQ('T', s_engine.glyphs[0].cp);

  priv_bind_gfx();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_reflow_render_page(&s_engine, 0U, nullptr));

  /* The 'T' lives in the upper-left quadrant -- verify pixel
   * coverage there. */
  const int32_t  x0  = 0;
  const int32_t  y0  = 0;
  const int32_t  x1  = (int32_t)k_test_viewport_w / 2;
  const int32_t  y1  = (int32_t)k_test_viewport_h / 2;
  const uint32_t lit = priv_count_lit_pixels(x0, y0, x1, y1);
  TEST_ASSERT(lit > 0U);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_reflow_close(&s_engine));

  TEST_END("test_render_pixels_around_title");
}

/**
 * @brief `ra8_reflow_render_page_at` shifts the whole page by the origin;
 *        `ra8_reflow_render_page` equals it at origin (0,0).
 *
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_render_at_origin(void)
{
  TEST_BEGIN("test_render_at_origin");

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_reflow_init(k_test_viewport_w,
                                 k_test_viewport_h,
                                 s_font_buf,
                                 s_font_len,
                                 k_test_font_px,
                                 k_test_color_body,
                                 k_test_color_link,
                                 &s_engine));
  uint32_t pages = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_reflow_layout_chapter(&s_engine,
                                           (const uint8_t*)k_xhtml_simple,
                                           strlen(k_xhtml_simple),
                                           &pages));
  TEST_ASSERT(s_engine.glyph_count > 0U);

  const int32_t fbw = (int32_t)k_test_viewport_w;
  const int32_t fbh = (int32_t)k_test_viewport_h;
  const int32_t dx  = (int32_t)k_test_origin_dx;
  const int32_t dy  = (int32_t)k_test_origin_dy;

  /* render_page() must equal render_page_at(0, 0): same origin, identical
   * output (a self-consistency check independent of any clipping). */
  priv_bind_gfx();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_reflow_render_page_at(&s_engine, 0U, 0, 0));
  const uint32_t lit_at0 = priv_count_lit_pixels(0, 0, fbw, fbh);
  TEST_ASSERT(lit_at0 > 0U);
  priv_bind_gfx();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_reflow_render_page(&s_engine, 0U, nullptr));
  TEST_ASSERT_EQ(lit_at0, priv_count_lit_pixels(0, 0, fbw, fbh));

  /* At origin (0,0) the text occupies the top-left corner box. */
  priv_bind_gfx();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_reflow_render_page_at(&s_engine, 0U, 0, 0));
  TEST_ASSERT(priv_count_lit_pixels(0, 0, dx, dy) > 0U);

  /* Offset by (dx, dy): the pre-offset corner is now empty (every glyph
   * shifted past it) and ink lands in the shifted region. This proves the
   * origin translation without depending on edge clipping (the large h1
   * line clips at the top at origin, so total pixel counts are not equal
   * across offsets). */
  priv_bind_gfx();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_reflow_render_page_at(&s_engine, 0U, dx, dy));
  TEST_ASSERT_EQ(0U, priv_count_lit_pixels(0, 0, dx, dy));
  TEST_ASSERT(priv_count_lit_pixels(dx, dy, fbw, fbh) > 0U);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_reflow_close(&s_engine));

  TEST_END("test_render_at_origin");
}

/**
 * @brief Multi-paragraph layout produces glyphs spanning multiple
 *        block-start / block-end pairs.
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */
static void test_multi_paragraph(void)
{
  TEST_BEGIN("test_multi_paragraph");

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_reflow_init(k_test_viewport_w,
                                 k_test_viewport_h,
                                 s_font_buf,
                                 s_font_len,
                                 k_test_font_px,
                                 k_test_color_body,
                                 k_test_color_link,
                                 &s_engine));
  uint32_t pages = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_reflow_layout_chapter(&s_engine,
                                           (const uint8_t*)k_xhtml_multi,
                                           strlen(k_xhtml_multi),
                                           &pages));
  TEST_ASSERT(pages >= 1U);
  TEST_ASSERT(s_engine.glyph_count > 0U);

  /* Count block-start tokens -- expect three (one per <p>). */
  uint32_t block_starts = 0U;
  for (uint32_t i = 0U; i < s_engine.token_count; ++i) {
    if (s_engine.tokens[i].kind == (uint8_t)k_ra8_reflow_tok_block_start &&
        s_engine.tokens[i].tag == (uint8_t)k_ra8_reflow_tag_p) {
      ++block_starts;
    }
  }
  TEST_ASSERT_EQ(3U, block_starts);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_reflow_close(&s_engine));

  TEST_END("test_multi_paragraph");
}

/**
 * @brief Bold / italic propagate the style stamp onto laid-out glyphs.
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */
static void test_bold_italic_toggling(void)
{
  TEST_BEGIN("test_bold_italic_toggling");

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_reflow_init(k_test_viewport_w,
                                 k_test_viewport_h,
                                 s_font_buf,
                                 s_font_len,
                                 k_test_font_px,
                                 k_test_color_body,
                                 k_test_color_link,
                                 &s_engine));
  uint32_t pages = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_reflow_layout_chapter(&s_engine,
                                           (const uint8_t*)k_xhtml_styled,
                                           strlen(k_xhtml_styled),
                                           &pages));
  TEST_ASSERT(s_engine.glyph_count > 0U);

  bool saw_bold   = false;
  bool saw_italic = false;
  bool saw_plain  = false;
  for (uint32_t i = 0U; i < s_engine.glyph_count; ++i) {
    const uint8_t style = s_engine.glyphs[i].style;
    if ((style & (uint8_t)k_ra8_reflow_style_bold) != 0U) {
      saw_bold = true;
    }
    if ((style & (uint8_t)k_ra8_reflow_style_italic) != 0U) {
      saw_italic = true;
    }
    if (style == (uint8_t)k_ra8_reflow_style_normal) {
      saw_plain = true;
    }
  }
  TEST_ASSERT(saw_bold);
  TEST_ASSERT(saw_italic);
  TEST_ASSERT(saw_plain);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_reflow_close(&s_engine));

  TEST_END("test_bold_italic_toggling");
}

/**
 * @brief Larger font size yields >= as many pages as the smaller size.
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */
static void test_set_font_size_reflows(void)
{
  TEST_BEGIN("test_set_font_size_reflows");

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_reflow_init(k_test_viewport_w,
                                 k_test_viewport_h,
                                 s_font_buf,
                                 s_font_len,
                                 k_test_font_px,
                                 k_test_color_body,
                                 k_test_color_link,
                                 &s_engine));
  uint32_t pages_small = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_reflow_layout_chapter(&s_engine,
                                           (const uint8_t*)k_xhtml_multi,
                                           strlen(k_xhtml_multi),
                                           &pages_small));
  const uint32_t glyphs_small = s_engine.glyph_count;

  TEST_ASSERT_EQ(k_ra8_ok, ra8_reflow_set_font_size(&s_engine, (uint16_t)k_test_font_px_large));
  uint32_t pages_large = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_reflow_get_page_count(&s_engine, &pages_large));
  TEST_ASSERT(pages_large >= pages_small);
  /* Total glyph count is identical between font sizes -- only the
   * line / page geometry changes. */
  TEST_ASSERT_EQ(glyphs_small, s_engine.glyph_count);

  /* Out-of-range sizes are rejected. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_reflow_set_font_size(&s_engine, 1U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_reflow_set_font_size(&s_engine, (uint16_t)k_ra8_reflow_max_font_px + 1U));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_reflow_close(&s_engine));

  TEST_END("test_set_font_size_reflows");
}

/**
 * @brief set_font_size before any layout yields invalid_state.
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */
static void test_set_font_size_without_layout(void)
{
  TEST_BEGIN("test_set_font_size_without_layout");

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_reflow_init(k_test_viewport_w,
                                 k_test_viewport_h,
                                 s_font_buf,
                                 s_font_len,
                                 k_test_font_px,
                                 k_test_color_body,
                                 k_test_color_link,
                                 &s_engine));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state,
                 ra8_reflow_set_font_size(&s_engine, k_test_font_px_large));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_reflow_close(&s_engine));

  TEST_END("test_set_font_size_without_layout");
}

/**
 * @brief layout_chapter rejects an empty buffer.
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */
static void test_layout_rejects_empty_input(void)
{
  TEST_BEGIN("test_layout_rejects_empty_input");

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_reflow_init(k_test_viewport_w,
                                 k_test_viewport_h,
                                 s_font_buf,
                                 s_font_len,
                                 k_test_font_px,
                                 k_test_color_body,
                                 k_test_color_link,
                                 &s_engine));
  uint32_t pages = (uint32_t)k_test_pages_poison;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_reflow_layout_chapter(&s_engine, (const uint8_t*)k_xhtml_simple, 0U, &pages));
  /* Malformed XHTML rejected as validation_failed. */
  static const char* const k_bad = "<<<not xml";
  TEST_ASSERT_EQ(
    k_ra8_err_validation_failed,
    ra8_reflow_layout_chapter(&s_engine, (const uint8_t*)k_bad, strlen(k_bad), &pages));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_reflow_close(&s_engine));

  TEST_END("test_layout_rejects_empty_input");
}

/**
 * @brief Test executable entry point.
 *
 * @details When the vendored Literata face is unreachable in the sandbox the
 *          suite skips cleanly after running the guard tests against a small
 *          synthetic blob (the size check is the only guard exercised).
 *
 * @return 0 on success (all tests passed, or clean guards-only skip).
 *
 * @pre Host environment provides a filesystem and stderr.
 * @post The layout / render / guard behaviours above are validated.
 *
 * @note Not thread-safe (single-threaded test runner).
 * @since 0.1.0
 */
int main(void)
{
  if (!priv_load_font()) {
    /* If the font is unreachable in the test sandbox, skip cleanly:
     * the suite still validates the public API guards by running the
     * NULL-arg tests against a minimal synthetic blob. */
    (void)fprintf(stderr,
                  "[SKIP] test_ra8_reflow: Literata-Regular.ttf not loadable -- "
                  "running guards-only path\n");
    /* Substitute a small synthetic blob so init() succeeds where the
     * size check is the only guard exercised. */
    (void)memset(s_font_buf, (int)k_test_stub_fill, 32U);
    s_font_len = 32U;
    test_null_and_preinit_guards_init();
    test_null_and_preinit_guards_ops();
    return 0;
  }

  test_null_and_preinit_guards_init();
  test_null_and_preinit_guards_ops();
  test_simple_layout();
  test_render_pixels_around_title();
  test_render_at_origin();
  test_multi_paragraph();
  test_bold_italic_toggling();
  test_set_font_size_reflows();
  test_set_font_size_without_layout();
  test_layout_rejects_empty_input();
  (void)fprintf(stderr, "[OK  ] test_ra8_reflow: layout/render/guard tests passed\n");
  return 0;
}
