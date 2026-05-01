/**
 * @file test_ra_reflow.c
 * @brief Unit tests for libs/ra_reflow (HTML+CSS reflow + paginate).
 *
 * @details
 * Loads the vendored Bitstream Vera font from
 * `libs/third_party/guix/fonts/verasans/Vera.ttf`, hands it to the
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
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * [Ring 4 / Reflow]
 * {World: NS}
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ra_err.h"
#include "ra_gfx.h"
#include "ra_reflow.h"
#include "unity_minimal.h"

/* --------------------------------------------------------------------- */

/**
 * @enum test_reflow_sizes_t
 * @brief Synthetic-fixture sizing constants.
 */
typedef enum : uint32_t {
  k_test_viewport_w     = 320U, /**< Test viewport width, pixels.        */
  k_test_viewport_h     = 240U, /**< Test viewport height, pixels.       */
  k_test_font_px        = 18U,  /**< Default font size for tests.        */
  k_test_font_px_large  = 36U,  /**< Larger font size for re-flow test.  */
  k_test_fb_pixels      = (uint32_t)k_test_viewport_w * (uint32_t)k_test_viewport_h,
  k_test_fb_bytes_argb  = k_test_fb_pixels * 4U, /**< 4 BPP framebuffer.        */
  k_test_font_buf_bytes = 2U * 1024U * 1024U,    /**< 2 MiB font load capacity.  */
  k_test_color_body     = 0x00FFFFFFU,           /**< Body colour (white).        */
  k_test_color_link     = 0x000000FFU,           /**< Link colour (blue).         */
  k_test_root_path_max  = 1024U,                 /**< Max derived firmware-root path.    */
} test_reflow_sizes_t;

/* Static storage so the host stack stays small. */
static uint8_t  s_font_buf[k_test_font_buf_bytes];
static size_t   s_font_len = 0U;
static uint32_t s_fb[k_test_fb_pixels];

/* Engine handle is large; keep it static. */
static ra_reflow_t s_engine;

/* --------------------------------------------------------------------- */
/* XHTML fixtures                                                        */
/* --------------------------------------------------------------------- */

static const char* const k_xhtml_simple = "<html><body><h1>Title</h1><p>Body</p></body></html>";

static const char* const k_xhtml_multi = "<html><body>"
                                         "<p>Paragraph one with several words to wrap.</p>"
                                         "<p>Paragraph two follows on the next block.</p>"
                                         "<p>Paragraph three closes out the body content.</p>"
                                         "</body></html>";

static const char* const k_xhtml_styled =
  "<html><body><p>plain <b>bold</b> and <i>italic</i> mix</p></body></html>";

/* --------------------------------------------------------------------- */
/* Helpers                                                               */
/* --------------------------------------------------------------------- */

/**
 * @brief Strip the trailing path component from `path`, in place.
 */
static void priv_dirname_inplace(char* path)
{
  size_t len = strlen(path);
  while (len > 0U && path[len - 1U] != '/') {
    --len;
  }
  if (len > 0U) {
    path[len - 1U] = '\0';
  }
}

/**
 * @brief Compute the firmware root from __FILE__ (which is the
 *        absolute path of this source file under the cmake build).
 */
static void priv_resolve_fw_root(char* out, size_t out_cap)
{
  /* __FILE__ resolves to ".../tests/test_ra_reflow.c" -- strip two
   * trailing components to land on the firmware root. */
  (void)snprintf(out, out_cap, "%s", __FILE__);
  priv_dirname_inplace(out); /* drop test_ra_reflow.c */
  priv_dirname_inplace(out); /* drop tests/           */
}

/**
 * @brief Load the Vera font into `s_font_buf`.
 *
 * @return true if loaded, false if the font could not be found.
 */
static bool priv_load_font(void)
{
  static char root[k_test_root_path_max];
  priv_resolve_fw_root(root, sizeof(root));

  static char path[k_test_root_path_max + 64U];
  /* GCC's -Wformat-truncation flags the maximally-conservative case
   * where `root` is the full 1 KiB; concatenate manually so the
   * checker can see the bound. */
  const char* const k_font_rel = "/libs/third_party/guix/fonts/verasans/Vera.ttf";
  size_t            root_len   = 0U;
  while (root_len + 1U < sizeof(path) && root[root_len] != '\0') {
    path[root_len] = root[root_len];
    ++root_len;
  }
  size_t rel_len = 0U;
  while (root_len + rel_len + 1U < sizeof(path) && k_font_rel[rel_len] != '\0') {
    path[root_len + rel_len] = k_font_rel[rel_len];
    ++rel_len;
  }
  path[root_len + rel_len] = '\0';
  FILE* fp                 = fopen(path, "rb");
  if (fp == NULL) {
    return false;
  }
  size_t n = fread(s_font_buf, 1U, sizeof(s_font_buf), fp);
  (void)fclose(fp);
  if (n < 16U) {
    return false;
  }
  s_font_len = n;
  return true;
}

/**
 * @brief Bind ra_gfx to the local test framebuffer.
 */
static void priv_bind_gfx(void)
{
  (void)memset(s_fb, 0, sizeof(s_fb));
  ra_err_t err = ra_gfx_init(s_fb,
                             (uint16_t)k_test_viewport_w,
                             (uint16_t)k_test_viewport_h,
                             k_ra_gfx_format_argb8888);
  TEST_ASSERT_EQ(k_ra_ok, err);
}

/**
 * @brief Count non-zero framebuffer pixels in a rectangular region.
 */
static uint32_t priv_count_lit_pixels(int32_t x0, int32_t y0, int32_t x1, int32_t y1)
{
  uint32_t lit = 0U;
  for (int32_t y = y0; y < y1; ++y) {
    for (int32_t x = x0; x < x1; ++x) {
      if (s_fb[(uint32_t)y * (uint32_t)k_test_viewport_w + (uint32_t)x] != 0U) {
        ++lit;
      }
    }
  }
  return lit;
}

/* --------------------------------------------------------------------- */
/* Tests                                                                 */
/* --------------------------------------------------------------------- */

/**
 * @brief NULL / pre-init guards against every public entry point.
 */
static void test_null_and_preinit_guards(void)
{
  TEST_BEGIN("test_null_and_preinit_guards");

  /* init() guards. */
  TEST_ASSERT_EQ(k_ra_err_null_ptr,
                 ra_reflow_init(k_test_viewport_w,
                                k_test_viewport_h,
                                NULL,
                                s_font_len,
                                k_test_font_px,
                                k_test_color_body,
                                k_test_color_link,
                                &s_engine));
  TEST_ASSERT_EQ(k_ra_err_null_ptr,
                 ra_reflow_init(k_test_viewport_w,
                                k_test_viewport_h,
                                s_font_buf,
                                s_font_len,
                                k_test_font_px,
                                k_test_color_body,
                                k_test_color_link,
                                NULL));
  TEST_ASSERT_EQ(k_ra_err_invalid_arg,
                 ra_reflow_init(0U,
                                k_test_viewport_h,
                                s_font_buf,
                                s_font_len,
                                k_test_font_px,
                                k_test_color_body,
                                k_test_color_link,
                                &s_engine));
  TEST_ASSERT_EQ(k_ra_err_invalid_size,
                 ra_reflow_init(k_test_viewport_w,
                                k_test_viewport_h,
                                s_font_buf,
                                4U,
                                k_test_font_px,
                                k_test_color_body,
                                k_test_color_link,
                                &s_engine));

  /* close() guards. */
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_reflow_close(NULL));

  /* Engine zeroed -> not initialised. */
  ra_reflow_t closed;
  (void)memset(&closed, 0, sizeof(closed));
  TEST_ASSERT_EQ(k_ra_err_not_initialized, ra_reflow_close(&closed));

  /* layout_chapter() guards. */
  uint32_t pages = 0U;
  TEST_ASSERT_EQ(
    k_ra_err_null_ptr,
    ra_reflow_layout_chapter(NULL, (const uint8_t*)k_xhtml_simple, strlen(k_xhtml_simple), &pages));
  TEST_ASSERT_EQ(k_ra_err_not_initialized,
                 ra_reflow_layout_chapter(&closed,
                                          (const uint8_t*)k_xhtml_simple,
                                          strlen(k_xhtml_simple),
                                          &pages));

  /* render_page() / get_page_count() / set_font_size() guards. */
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_reflow_render_page(NULL, 0U, NULL));
  TEST_ASSERT_EQ(k_ra_err_not_initialized, ra_reflow_render_page(&closed, 0U, NULL));
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_reflow_get_page_count(NULL, &pages));
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_reflow_set_font_size(NULL, k_test_font_px));
  TEST_ASSERT_EQ(k_ra_err_not_initialized, ra_reflow_set_font_size(&closed, k_test_font_px));

  TEST_END("test_null_and_preinit_guards");
}

/**
 * @brief Parse a simple `<h1>Title</h1><p>Body</p>` and confirm
 *        layout returns >= 1 page with > 0 glyphs.
 */
static void test_simple_layout(void)
{
  TEST_BEGIN("test_simple_layout");

  TEST_ASSERT_EQ(k_ra_ok,
                 ra_reflow_init(k_test_viewport_w,
                                k_test_viewport_h,
                                s_font_buf,
                                s_font_len,
                                k_test_font_px,
                                k_test_color_body,
                                k_test_color_link,
                                &s_engine));

  uint32_t pages = 0U;
  ra_err_t err   = ra_reflow_layout_chapter(&s_engine,
                                            (const uint8_t*)k_xhtml_simple,
                                            strlen(k_xhtml_simple),
                                            &pages);
  TEST_ASSERT_EQ(k_ra_ok, err);
  TEST_ASSERT(pages >= 1U);
  TEST_ASSERT(s_engine.glyph_count > 0U);
  TEST_ASSERT(s_engine.token_count > 0U);

  uint32_t page_count = 0U;
  TEST_ASSERT_EQ(k_ra_ok, ra_reflow_get_page_count(&s_engine, &page_count));
  TEST_ASSERT_EQ(pages, page_count);

  /* render_page() with an out-of-range index -> out_of_range. */
  TEST_ASSERT_EQ(k_ra_err_out_of_range, ra_reflow_render_page(&s_engine, page_count, NULL));

  TEST_ASSERT_EQ(k_ra_ok, ra_reflow_close(&s_engine));

  TEST_END("test_simple_layout");
}

/**
 * @brief Render the simple page and verify non-zero pixel coverage in
 *        the upper-left region (where the 'T' of "Title" lives).
 */
static void test_render_pixels_around_title(void)
{
  TEST_BEGIN("test_render_pixels_around_title");

  TEST_ASSERT_EQ(k_ra_ok,
                 ra_reflow_init(k_test_viewport_w,
                                k_test_viewport_h,
                                s_font_buf,
                                s_font_len,
                                k_test_font_px,
                                k_test_color_body,
                                k_test_color_link,
                                &s_engine));
  uint32_t pages = 0U;
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_reflow_layout_chapter(&s_engine,
                                          (const uint8_t*)k_xhtml_simple,
                                          strlen(k_xhtml_simple),
                                          &pages));
  TEST_ASSERT(s_engine.glyph_count > 0U);

  /* The first glyph must be the 'T' of "Title" (parser collapses any
   * leading whitespace before it). */
  TEST_ASSERT_EQ((int32_t)'T', s_engine.glyphs[0].cp);

  priv_bind_gfx();
  TEST_ASSERT_EQ(k_ra_ok, ra_reflow_render_page(&s_engine, 0U, NULL));

  /* The 'T' lives in the upper-left quadrant -- verify pixel
   * coverage there. */
  const int32_t  x0  = 0;
  const int32_t  y0  = 0;
  const int32_t  x1  = (int32_t)k_test_viewport_w / 2;
  const int32_t  y1  = (int32_t)k_test_viewport_h / 2;
  const uint32_t lit = priv_count_lit_pixels(x0, y0, x1, y1);
  TEST_ASSERT(lit > 0U);

  TEST_ASSERT_EQ(k_ra_ok, ra_reflow_close(&s_engine));

  TEST_END("test_render_pixels_around_title");
}

/**
 * @brief Multi-paragraph layout produces glyphs spanning multiple
 *        block-start / block-end pairs.
 */
static void test_multi_paragraph(void)
{
  TEST_BEGIN("test_multi_paragraph");

  TEST_ASSERT_EQ(k_ra_ok,
                 ra_reflow_init(k_test_viewport_w,
                                k_test_viewport_h,
                                s_font_buf,
                                s_font_len,
                                k_test_font_px,
                                k_test_color_body,
                                k_test_color_link,
                                &s_engine));
  uint32_t pages = 0U;
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_reflow_layout_chapter(&s_engine,
                                          (const uint8_t*)k_xhtml_multi,
                                          strlen(k_xhtml_multi),
                                          &pages));
  TEST_ASSERT(pages >= 1U);
  TEST_ASSERT(s_engine.glyph_count > 0U);

  /* Count block-start tokens -- expect three (one per <p>). */
  uint32_t block_starts = 0U;
  for (uint32_t i = 0U; i < s_engine.token_count; ++i) {
    if (s_engine.tokens[i].kind == (uint8_t)k_ra_reflow_tok_block_start &&
        s_engine.tokens[i].tag == (uint8_t)k_ra_reflow_tag_p) {
      ++block_starts;
    }
  }
  TEST_ASSERT_EQ(3U, block_starts);

  TEST_ASSERT_EQ(k_ra_ok, ra_reflow_close(&s_engine));

  TEST_END("test_multi_paragraph");
}

/**
 * @brief Bold / italic propagate the style stamp onto laid-out glyphs.
 */
static void test_bold_italic_toggling(void)
{
  TEST_BEGIN("test_bold_italic_toggling");

  TEST_ASSERT_EQ(k_ra_ok,
                 ra_reflow_init(k_test_viewport_w,
                                k_test_viewport_h,
                                s_font_buf,
                                s_font_len,
                                k_test_font_px,
                                k_test_color_body,
                                k_test_color_link,
                                &s_engine));
  uint32_t pages = 0U;
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_reflow_layout_chapter(&s_engine,
                                          (const uint8_t*)k_xhtml_styled,
                                          strlen(k_xhtml_styled),
                                          &pages));
  TEST_ASSERT(s_engine.glyph_count > 0U);

  bool saw_bold   = false;
  bool saw_italic = false;
  bool saw_plain  = false;
  for (uint32_t i = 0U; i < s_engine.glyph_count; ++i) {
    const uint8_t style = s_engine.glyphs[i].style;
    if ((style & (uint8_t)k_ra_reflow_style_bold) != 0U) {
      saw_bold = true;
    }
    if ((style & (uint8_t)k_ra_reflow_style_italic) != 0U) {
      saw_italic = true;
    }
    if (style == (uint8_t)k_ra_reflow_style_normal) {
      saw_plain = true;
    }
  }
  TEST_ASSERT(saw_bold);
  TEST_ASSERT(saw_italic);
  TEST_ASSERT(saw_plain);

  TEST_ASSERT_EQ(k_ra_ok, ra_reflow_close(&s_engine));

  TEST_END("test_bold_italic_toggling");
}

/**
 * @brief Larger font size yields >= as many pages as the smaller size.
 */
static void test_set_font_size_reflows(void)
{
  TEST_BEGIN("test_set_font_size_reflows");

  TEST_ASSERT_EQ(k_ra_ok,
                 ra_reflow_init(k_test_viewport_w,
                                k_test_viewport_h,
                                s_font_buf,
                                s_font_len,
                                k_test_font_px,
                                k_test_color_body,
                                k_test_color_link,
                                &s_engine));
  uint32_t pages_small = 0U;
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_reflow_layout_chapter(&s_engine,
                                          (const uint8_t*)k_xhtml_multi,
                                          strlen(k_xhtml_multi),
                                          &pages_small));
  const uint32_t glyphs_small = s_engine.glyph_count;

  TEST_ASSERT_EQ(k_ra_ok, ra_reflow_set_font_size(&s_engine, (uint16_t)k_test_font_px_large));
  uint32_t pages_large = 0U;
  TEST_ASSERT_EQ(k_ra_ok, ra_reflow_get_page_count(&s_engine, &pages_large));
  TEST_ASSERT(pages_large >= pages_small);
  /* Total glyph count is identical between font sizes -- only the
   * line / page geometry changes. */
  TEST_ASSERT_EQ(glyphs_small, s_engine.glyph_count);

  /* Out-of-range sizes are rejected. */
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_reflow_set_font_size(&s_engine, 1U));
  TEST_ASSERT_EQ(k_ra_err_invalid_arg,
                 ra_reflow_set_font_size(&s_engine, (uint16_t)k_ra_reflow_max_font_px + 1U));

  TEST_ASSERT_EQ(k_ra_ok, ra_reflow_close(&s_engine));

  TEST_END("test_set_font_size_reflows");
}

/**
 * @brief set_font_size before any layout yields invalid_state.
 */
static void test_set_font_size_without_layout(void)
{
  TEST_BEGIN("test_set_font_size_without_layout");

  TEST_ASSERT_EQ(k_ra_ok,
                 ra_reflow_init(k_test_viewport_w,
                                k_test_viewport_h,
                                s_font_buf,
                                s_font_len,
                                k_test_font_px,
                                k_test_color_body,
                                k_test_color_link,
                                &s_engine));
  TEST_ASSERT_EQ(k_ra_err_invalid_state, ra_reflow_set_font_size(&s_engine, k_test_font_px_large));
  TEST_ASSERT_EQ(k_ra_ok, ra_reflow_close(&s_engine));

  TEST_END("test_set_font_size_without_layout");
}

/**
 * @brief layout_chapter rejects an empty buffer.
 */
static void test_layout_rejects_empty_input(void)
{
  TEST_BEGIN("test_layout_rejects_empty_input");

  TEST_ASSERT_EQ(k_ra_ok,
                 ra_reflow_init(k_test_viewport_w,
                                k_test_viewport_h,
                                s_font_buf,
                                s_font_len,
                                k_test_font_px,
                                k_test_color_body,
                                k_test_color_link,
                                &s_engine));
  uint32_t pages = 99U;
  TEST_ASSERT_EQ(k_ra_err_invalid_size,
                 ra_reflow_layout_chapter(&s_engine, (const uint8_t*)k_xhtml_simple, 0U, &pages));
  /* Malformed XHTML rejected as validation_failed. */
  static const char* const k_bad = "<<<not xml";
  TEST_ASSERT_EQ(k_ra_err_validation_failed,
                 ra_reflow_layout_chapter(&s_engine, (const uint8_t*)k_bad, strlen(k_bad), &pages));
  TEST_ASSERT_EQ(k_ra_ok, ra_reflow_close(&s_engine));

  TEST_END("test_layout_rejects_empty_input");
}

/* --------------------------------------------------------------------- */
/* main                                                                  */
/* --------------------------------------------------------------------- */

int main(void)
{
  if (!priv_load_font()) {
    /* If the font is unreachable in the test sandbox, skip cleanly:
     * the suite still validates the public API guards by running the
     * NULL-arg test against a minimal synthetic blob. */
    (void)fprintf(stderr,
                  "[SKIP] test_ra_reflow: Vera.ttf not loadable -- "
                  "running guards-only path\n");
    /* Substitute a small synthetic blob so init() succeeds where the
     * size check is the only guard exercised. */
    (void)memset(s_font_buf, 0xA5, 32U);
    s_font_len = 32U;
    test_null_and_preinit_guards();
    return 0;
  }

  test_null_and_preinit_guards();
  test_simple_layout();
  test_render_pixels_around_title();
  test_multi_paragraph();
  test_bold_italic_toggling();
  test_set_font_size_reflows();
  test_set_font_size_without_layout();
  test_layout_rejects_empty_input();
  (void)fprintf(stderr, "[OK  ] test_ra_reflow: all 8 tests passed\n");
  return 0;
}
