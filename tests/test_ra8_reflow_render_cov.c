/**
 * @file test_ra8_reflow_render_cov.c
 * @brief Line-coverage (gcovr) tests for libs/ra8_reflow/src/ra8_reflow_render.c.
 *
 * @details
 * The existing MC/DC render suite (tests/test_ra8_reflow_render.c) drives the
 * glyph-size, register-face and image-loader decisions, but leaves several whole
 * source paths cold. These end-to-end tests drive the real renderer through the
 * public `ra8_reflow_render_page()` / `ra8_reflow_render_page_at()` API against an
 * `ra8_gfx` framebuffer with the bundled Literata face (loaded relative to
 * `__FILE__` exactly as the existing render test does; each test SKIPs if the
 * font is absent) to execute the missing lines:
 *
 *  - internal_init_font's malformed-font returns (offset < 0 / InitFont == 0) and
 *    their propagation through internal_init_faces -> internal_render_page;
 *  - internal_draw_underline (the link-underline strip) via an `<a>` run whose glyphs
 *    carry the underline style bit;
 *  - internal_render_images / internal_render_one_image / internal_render_svg (the SVG-route
 *    *and* the raster-decode route) by laying out `<img>` boxes resolved by a
 *    bound loader and rendering the page that holds them;
 *  - the defensive out-of-range face-index clamp (fi >= nfaces -> fi = 0).
 *
 * Assertions are robust (return code + a framebuffer pixel differing from the
 * zeroed background), not exact pixel values: the point is execution.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_gfx.h"
#include "ra8_reflow.h"
#include "support/ra8_test_file.h"
#include "support/ra8_test_output.h"
#include "unity_minimal.h"

/**
 * @enum render_dim_t
 * @brief Viewport / framebuffer geometry knobs (no magic numbers).
 */
typedef enum : uint16_t {
  k_rc_vp_w    = 240U, /**< Viewport + framebuffer width, pixels.  */
  k_rc_vp_h    = 180U, /**< Viewport + framebuffer height, pixels. */
  k_rc_font_px = 18U,  /**< Body font size, pixels.                */
} render_dim_t;

/**
 * @enum render_color_t
 * @brief Style colour keys handed to ra8_reflow_init() (no magic numbers).
 */
typedef enum : uint32_t {
  k_rc_body_color = 0x101010FFU, /**< Body text colour key.   */
  k_rc_link_color = 0x0040C0FFU, /**< Anchor text colour key. */
} render_color_t;

/**
 * @enum render_size_t
 * @brief Buffer capacities for the render fixtures (no magic numbers).
 */
typedef enum : size_t {
  k_rc_font_cap  = 2U * 1024U * 1024U,                         /**< Literata subset < 2 MiB. */
  k_rc_path_cap  = 1024U,                                      /**< Font path buffer.        */
  k_rc_arena_cap = 64U * 1024U,                                /**< Image decode scratch.    */
  k_rc_fb_bytes  = (size_t)k_rc_vp_w * (size_t)k_rc_vp_h * 3U, /**< RGB888 framebuffer.      */
} render_size_t;

/**
 * @enum render_face_t
 * @brief Face-index packing constants for the out-of-range clamp test.
 */
typedef enum : uint8_t {
  k_rc_face_one_nibble = 0x10U, /**< Face index 1 stamped into the style high nibble. */
} render_face_t;

/** @brief Bundled Literata Latin-1 face bytes (loaded once). */
static uint8_t s_font[k_rc_font_cap];
static uint8_t s_font_staging[k_rc_font_cap];

/** @brief Byte length of the loaded Literata face. */
static size_t s_font_len;

/** @brief Shared engine handle (large -- keep off the stack). */
static ra8_reflow_t s_engine;

/** @brief RGB888 framebuffer bound via ra8_gfx_init(). */
static uint8_t s_fb[k_rc_fb_bytes];

/** @brief Image-decode bump arena scratch. */
static uint8_t s_arena_buf[k_rc_arena_cap];

/** @brief Junk bytes that are not a valid font (>= 16 bytes for the init guard). */
static const uint8_t s_junk_font[64] = {1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U};

/**
 * @brief A 2x2 RGB PNG: TL red, TR green, BL blue, BR white.
 * @details Same fixture as tests/test_ra8_reflow_image.c -- the smallest raster
 * the image loader can return so layout records an image box the render path
 * decodes + blits.
 */
static const uint8_t s_png_2x2[] = {
  137, 80,  78,  71, 13,  10,  26,  10,  0,   0,   0,   13,  73,  72,  68,  82,  0,   0,  0,   2,
  0,   0,   0,   2,  8,   2,   0,   0,   0,   253, 212, 154, 115, 0,   0,   0,   22,  73, 68,  65,
  84,  120, 156, 99, 248, 207, 192, 192, 240, 159, 129, 145, 129, 225, 255, 255, 255, 12, 0,   30,
  246, 4,   253, 9,  237, 52,  62,  0,   0,   0,   0,   73,  69,  78,  68,  174, 66,  96, 130,
};

/** @brief A cover-wrapper SVG: `<svg><image href="cover.png"/></svg>`. */
static const char s_svg_cover[] =
  "<svg viewBox=\"0 0 100 100\"><image width=\"100\" height=\"100\" "
  "xlink:href=\"cover.png\"/></svg>";

/** @brief A shapes SVG with a filled rect (drawn via ra8_svg_render, no image). */
static const char s_svg_shapes[] =
  "<svg viewBox=\"0 0 100 100\"><rect x=\"0\" y=\"0\" width=\"100\" height=\"100\" "
  "fill=\"#ff0000\"/></svg>";

/**
 * @brief Loader that returns the 2x2 PNG for any href.
 * @param[in]  ctx      Unused.
 * @param[in]  href     Unused.
 * @param[in]  href_len Unused.
 * @param[out] out      Receives the PNG byte pointer.
 * @param[out] out_len  Receives the PNG byte count.
 * @return k_ra8_ok always.

 * @details Performs one bounded, deterministic operation for this host test.
 * @retval 0 Zero or false result; nonzero values describe the alternate result.
 * @pre Pointer arguments, when present, address their documented test storage.
 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post The helper completes only the bounded test operation described above.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
 * @since 0.1.0
*/
RA8_INTERNAL static ra8_err_t internal_png_loader(void*           ctx,
                                                  const char*     href,
                                                  uint32_t        href_len,
                                                  const uint8_t** out,
                                                  size_t*         out_len)
{
  (void)ctx;
  (void)href;
  (void)href_len;
  *out     = s_png_2x2;
  *out_len = sizeof s_png_2x2;
  return k_ra8_ok;
}

/**
 * @brief Loader that returns an SVG for `.svg` hrefs and the 2x2 PNG otherwise.
 *
 * @details Lets one chapter carry both an SVG cover-wrapper `<img>` (resolved to
 * SVG bytes) and lets internal_render_svg's second loader call (for the unwrapped
 * `cover.png` href) return the raster -- driving both SVG sub-routes.
 *
 * @param[in]  ctx      Unused.
 * @param[in]  href     Image href (not NUL-terminated).
 * @param[in]  href_len Length of @p href, bytes.
 * @param[out] out      Receives the byte pointer.
 * @param[out] out_len  Receives the byte count.
 * @return k_ra8_ok always.

 * @retval 0 Zero or false result; nonzero values describe the alternate result.
 * @pre Pointer arguments, when present, address their documented test storage.
 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post The helper completes only the bounded test operation described above.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
 * @since 0.1.0
*/
RA8_INTERNAL static ra8_err_t internal_svg_loader(void*           ctx,
                                                  const char*     href,
                                                  uint32_t        href_len,
                                                  const uint8_t** out,
                                                  size_t*         out_len)
{
  (void)ctx;
  bool is_svg = false;
  if (href_len >= 4U) {
    const char* tail = &href[href_len - 4U];
    is_svg           = (memcmp(tail, ".svg", 4U) == 0);
  }
  if (is_svg) {
    *out     = (const uint8_t*)s_svg_cover;
    *out_len = sizeof s_svg_cover - 1U;
  } else {
    *out     = s_png_2x2;
    *out_len = sizeof s_png_2x2;
  }
  return k_ra8_ok;
}

/**
 * @brief Loader that returns the shapes SVG for any href.
 * @param[in]  ctx      Unused.
 * @param[in]  href     Unused.
 * @param[in]  href_len Unused.
 * @param[out] out      Receives the SVG byte pointer.
 * @param[out] out_len  Receives the SVG byte count.
 * @return k_ra8_ok always.

 * @details Performs one bounded, deterministic operation for this host test.
 * @retval 0 Zero or false result; nonzero values describe the alternate result.
 * @pre Pointer arguments, when present, address their documented test storage.
 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post The helper completes only the bounded test operation described above.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
 * @since 0.1.0
*/
RA8_INTERNAL static ra8_err_t internal_svg_shapes_loader(void*           ctx,
                                                         const char*     href,
                                                         uint32_t        href_len,
                                                         const uint8_t** out,
                                                         size_t*         out_len)
{
  (void)ctx;
  (void)href;
  (void)href_len;
  *out     = (const uint8_t*)s_svg_shapes;
  *out_len = sizeof s_svg_shapes - 1U;
  return k_ra8_ok;
}

/**
 * @brief Load the bundled Literata subset relative to this source file.
 * @return 1 on success, 0 if the font is absent (caller SKIPs).

 * @details Performs one bounded, deterministic operation for this host test.
 * @retval 0 Zero or false result; nonzero values describe the alternate result.
 * @pre Pointer arguments, when present, address their documented test storage.
 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post The helper completes only the bounded test operation described above.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
 * @since 0.1.0
*/
RA8_INTERNAL static int internal_load_font(void)
{
  char         path[k_rc_path_cap];
  const char*  here = __FILE__;
  const size_t flen = strlen(here);
  size_t       base = flen;
  while (base > 0U && here[base - 1U] != '/') {
    base--; /* drop "test_ra8_reflow_render_cov.c" */
  }
  while (base > 0U && here[base - 1U] == '/') {
    base--;
  }
  while (base > 0U && here[base - 1U] != '/') {
    base--; /* drop "tests" -> repo root */
  }
  if (base == 0U || base >= sizeof(path)) {
    return 0;
  }
  memcpy(path, here, base);
  (void)snprintf(&path[base], sizeof(path) - base, "libs/ra8_fonts/literata_latin1.ttf");
  const ra8_test_file_result_t result =
    internal_test_file_read(path, s_font, sizeof(s_font), s_font_staging, sizeof(s_font_staging));
  if (result.status != k_ra8_test_file_ok) {
    return 0;
  }
  s_font_len = result.transferred;
  return (s_font_len > 0U) ? 1 : 0;
}

/** @brief Init the engine with the loaded font; returns the init result.
 * @details Performs one bounded, deterministic operation for this host test.
 * @return Function-specific result consumed by the calling test.
 * @retval 0 Zero or false result; nonzero values describe the alternate result.
 * @pre Pointer arguments, when present, address their documented test storage.
 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post The helper completes only the bounded test operation described above.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
 * @since 0.1.0
*/
RA8_INTERNAL static ra8_err_t internal_init_engine(void)
{
  return ra8_reflow_init((uint16_t)k_rc_vp_w,
                         (uint16_t)k_rc_vp_h,
                         s_font,
                         s_font_len,
                         (uint16_t)k_rc_font_px,
                         (uint32_t)k_rc_body_color,
                         (uint32_t)k_rc_link_color,
                         &s_engine);
}

/** @brief Bind the RGB888 framebuffer (cleared to zero) to ra8_gfx.
 * @details Performs one bounded, deterministic operation for this host test.
 * @pre Pointer arguments, when present, address their documented test storage.
 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post The helper completes only the bounded test operation described above.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
 * @since 0.1.0
*/
RA8_INTERNAL static void internal_bind_clear_fb(void)
{
  memset(s_fb, 0, sizeof s_fb);
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_gfx_init(s_fb, (uint16_t)k_rc_vp_w, (uint16_t)k_rc_vp_h, k_ra8_gfx_format_rgb888));
}

/** @brief True iff any framebuffer pixel differs from the zeroed background.
 * @details Performs one bounded, deterministic operation for this host test.
 * @return Function-specific result consumed by the calling test.
 * @retval 0 Zero or false result; nonzero values describe the alternate result.
 * @pre Pointer arguments, when present, address their documented test storage.
 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post The helper completes only the bounded test operation described above.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
 * @since 0.1.0
*/
RA8_INTERNAL static bool internal_fb_has_drawn_pixel(void)
{
  for (size_t i = 0U; i < (size_t)k_rc_fb_bytes; ++i) {
    if (s_fb[i] != 0U) {
      return true;
    }
  }
  return false;
}

/**
 * @test internal_test_cov_render_bad_default_font
 * @brief A junk default font makes internal_init_faces / internal_render_page fail.
 *
 * @details Targets ra8_reflow_render.c lines 89-93 (internal_init_font's offset<0 /
 * InitFont==0 returns), 361-362 (internal_init_faces propagating that error) and
 * 393-394 (internal_render_page returning it). A valid chapter is laid out first
 * (the layout pass keeps the good font in font_data), then font_data is swapped
 * for junk so the *render* pass re-inits a broken font and bails.
 *
 * @par MC/DC:
 * (no compound decisions in this test -- the malformed default font makes
 * internal_init_font return at its `offset < 0` check (a single condition; the
 * `stbtt_InitFont(...) == 0` check is a separate single-condition `if`), so
 * internal_init_faces -> internal_render_page bail before any glyph is blitted or image
 * rendered; no `&&` or `||` in the code under test that this case touches)

 * @pre Pointer arguments, when present, address their documented test storage.
 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post The helper completes only the bounded test operation described above.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
 * @since 0.1.0
*/
RA8_INTERNAL static void internal_test_cov_render_bad_default_font(void)
{
  TEST_BEGIN("render cov: junk default font -> validation_failed");
  if (internal_load_font() == 0) {
    TEST_ASSERT_EQ(
      k_ra8_test_output_ok,
      internal_test_output_fd_text(STDERR_FILENO,
                                   "[SKIP] literata_latin1.ttf not found; skipping render\n"));
    TEST_END("render cov: junk default font -> validation_failed");
    return;
  }
  TEST_ASSERT_EQ(k_ra8_ok, internal_init_engine());
  uint32_t    pages = 0U;
  const char* xhtml = "<p>Body text.</p>";
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_reflow_layout_chapter(&s_engine, (const uint8_t*)xhtml, strlen(xhtml), &pages));
  TEST_ASSERT(pages >= 1U);

  internal_bind_clear_fb();
  /* Swap the default face for a junk blob: the render pass re-inits it and the
     internal_init_font -> internal_init_faces -> internal_render_page error chain fires. */
  s_engine.font_data = s_junk_font;
  s_engine.font_len  = sizeof s_junk_font;
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, ra8_reflow_render_page(&s_engine, 0U, nullptr));
  TEST_END("render cov: junk default font -> validation_failed");
}

/**
 * @test internal_test_cov_render_link_underline
 * @brief An `<a>` link run renders its underline strip.
 *
 * @details Targets ra8_reflow_render.c line 219-220 (internal_blit_glyph's underline
 * gate) and 156-170 (internal_draw_underline). The tokenizer stamps every `<a>`
 * glyph with k_ra8_reflow_style_underline, so rendering the page draws the
 * underline beneath the link glyphs.
 *
 * @par MC/DC:
 * The underline gate `if ((g->style & k_ra8_reflow_style_underline) != 0U)` and
 * the internal_draw_underline strip are single-condition / straight-line. The only
 * compound decision the render path crosses is internal_blit_glyph's
 * `if ((w > 0) && (h > 0))` glyph-box guard, which is MC/DC-deactivated in the
 * source (the box extents are co-dependent -- a glyph is inked with w,h both
 * positive or empty with both zero, so the mixed vectors are unreachable). Its
 * reachable arms are exercised by the sibling test_render_glyph_size_both_arms.

 * @pre Pointer arguments, when present, address their documented test storage.
 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post The helper completes only the bounded test operation described above.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
 * @since 0.1.0
*/
RA8_INTERNAL static void internal_test_cov_render_link_underline(void)
{
  TEST_BEGIN("render cov: link underline strip");
  if (internal_load_font() == 0) {
    TEST_ASSERT_EQ(
      k_ra8_test_output_ok,
      internal_test_output_fd_text(STDERR_FILENO,
                                   "[SKIP] literata_latin1.ttf not found; skipping render\n"));
    TEST_END("render cov: link underline strip");
    return;
  }
  TEST_ASSERT_EQ(k_ra8_ok, internal_init_engine());
  uint32_t    pages = 0U;
  const char* xhtml = "<p>See <a href=\"next.html\">this link</a> now.</p>";
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_reflow_layout_chapter(&s_engine, (const uint8_t*)xhtml, strlen(xhtml), &pages));
  TEST_ASSERT(pages >= 1U);

  internal_bind_clear_fb();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_reflow_render_page(&s_engine, 0U, nullptr));
  TEST_ASSERT(internal_fb_has_drawn_pixel());
  TEST_END("render cov: link underline strip");
}

/**
 * @test internal_test_cov_render_raster_image
 * @brief A bound loader's raster `<img>` is decoded + blitted by the page render.
 *
 * @details Targets ra8_reflow_render.c lines 305-316 (internal_render_images loop),
 * 284-302 (internal_render_one_image: href fetch + raster branch) and the
 * ra8_img_decode_blit call. A loader returning the 2x2 PNG yields a recorded image
 * box; rendering its page decodes + blits it nearest-neighbour.
 *
 * @par MC/DC:
 * Decision: `if ((engine->img_loader == NULL) || (engine->img_arena == NULL))`
 * in internal_render_images (libs/ra8_reflow/src/ra8_reflow_render.c), evaluated here
 * at its all-false control only -- a loader and arena are bound, so the loop runs
 * and the 2x2 PNG is decoded and blitted. The `ra8_svg_is_svg` selector in
 * internal_render_one_image is single-condition (false for a PNG). The loader/arena
 * OR's independent-influence vectors are carried by the sibling
 * test_render_images_loader_both_arms; the glyph-box `(w > 0) && (h > 0)` guard
 * crossed for the page text is MC/DC-deactivated (co-dependent extents).

 * @pre Pointer arguments, when present, address their documented test storage.
 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post The helper completes only the bounded test operation described above.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
 * @since 0.1.0
*/
RA8_INTERNAL static void internal_test_cov_render_raster_image(void)
{
  TEST_BEGIN("render cov: raster image decode + blit");
  if (internal_load_font() == 0) {
    TEST_ASSERT_EQ(
      k_ra8_test_output_ok,
      internal_test_output_fd_text(STDERR_FILENO,
                                   "[SKIP] literata_latin1.ttf not found; skipping render\n"));
    TEST_END("render cov: raster image decode + blit");
    return;
  }
  TEST_ASSERT_EQ(k_ra8_ok, internal_init_engine());
  ra8_img_arena_t arena = {.base   = s_arena_buf,
                           .cap    = sizeof s_arena_buf,
                           .offset = 0U,
                           .live   = 0U};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_reflow_set_image_loader(&s_engine, internal_png_loader, nullptr, &arena));

  uint32_t    pages = 0U;
  const char* xhtml = "<p>Figure:</p><img src=\"pic.png\"/><p>After.</p>";
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_reflow_layout_chapter(&s_engine, (const uint8_t*)xhtml, strlen(xhtml), &pages));
  TEST_ASSERT(s_engine.image_box_count >= 1U);

  internal_bind_clear_fb();
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_reflow_render_page(&s_engine, s_engine.image_boxes[0].page_index, nullptr));
  TEST_ASSERT(internal_fb_has_drawn_pixel());
  TEST_END("render cov: raster image decode + blit");
}

/**
 * @test internal_test_cov_render_svg_image
 * @brief SVG `<img>` boxes drive both the cover-wrapper and shapes SVG routes.
 *
 * @details Targets ra8_reflow_render.c lines 257-281 (internal_render_svg: the
 * image-href unwrap path *and* the ra8_svg_render shapes path) and 297-299
 * (internal_render_one_image's SVG branch). A loader returning a cover-wrapper SVG
 * (`<svg><image href="cover.png"/></svg>`) makes internal_render_svg extract the href
 * and re-load the raster; a second engine whose loader returns a shapes SVG
 * drives the ra8_svg_render branch.
 *
 * @par MC/DC:
 * Decision: `if ((engine->img_loader == NULL) || (engine->img_arena == NULL))`
 * in internal_render_images, evaluated here at its all-false control only (loader +
 * arena bound). The routing checks it reaches -- `ra8_svg_is_svg` (true for SVG),
 * `ra8_svg_image_href`, the href-length guard and the loader-result check in
 * internal_render_svg -- are all single-condition. The loader/arena OR's
 * independent-influence vectors are carried by the sibling
 * test_render_images_loader_both_arms; the glyph-box `(w > 0) && (h > 0)` guard
 * crossed for the page text is MC/DC-deactivated (co-dependent extents).

 * @pre Pointer arguments, when present, address their documented test storage.
 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post The helper completes only the bounded test operation described above.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
 * @since 0.1.0
*/
RA8_INTERNAL static void internal_test_cov_render_svg_image(void)
{
  TEST_BEGIN("render cov: SVG image (cover-wrapper + shapes)");
  if (internal_load_font() == 0) {
    TEST_ASSERT_EQ(
      k_ra8_test_output_ok,
      internal_test_output_fd_text(STDERR_FILENO,
                                   "[SKIP] literata_latin1.ttf not found; skipping render\n"));
    TEST_END("render cov: SVG image (cover-wrapper + shapes)");
    return;
  }

  /* Route A: cover-wrapper SVG -> href unwrap -> second loader call -> raster. */
  TEST_ASSERT_EQ(k_ra8_ok, internal_init_engine());
  ra8_img_arena_t arena = {.base   = s_arena_buf,
                           .cap    = sizeof s_arena_buf,
                           .offset = 0U,
                           .live   = 0U};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_reflow_set_image_loader(&s_engine, internal_svg_loader, nullptr, &arena));
  uint32_t    pages = 0U;
  const char* doc_a = "<p>Cover:</p><img src=\"cover.svg\"/><p>End.</p>";
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_reflow_layout_chapter(&s_engine, (const uint8_t*)doc_a, strlen(doc_a), &pages));
  TEST_ASSERT(s_engine.image_box_count >= 1U);
  internal_bind_clear_fb();
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_reflow_render_page(&s_engine, s_engine.image_boxes[0].page_index, nullptr));
  TEST_ASSERT(internal_fb_has_drawn_pixel());

  /* Route B: shapes SVG -> ra8_svg_render branch (no embedded <image> href). */
  TEST_ASSERT_EQ(k_ra8_ok, internal_init_engine());
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_reflow_set_image_loader(&s_engine, internal_svg_shapes_loader, nullptr, &arena));
  const char* doc_b = "<p>Shapes:</p><img src=\"art.svg\"/><p>Done.</p>";
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_reflow_layout_chapter(&s_engine, (const uint8_t*)doc_b, strlen(doc_b), &pages));
  TEST_ASSERT(s_engine.image_box_count >= 1U);
  internal_bind_clear_fb();
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_reflow_render_page(&s_engine, s_engine.image_boxes[0].page_index, nullptr));
  TEST_ASSERT(internal_fb_has_drawn_pixel());
  TEST_END("render cov: SVG image (cover-wrapper + shapes)");
}

/**
 * @test internal_test_cov_render_face_index_clamp
 * @brief A glyph with an out-of-range face index falls back to the default face.
 *
 * @details Targets ra8_reflow_render.c lines 402-403 (the `fi >= nfaces ->
 * fi = 0` defensive clamp in internal_render_page). With no faces registered,
 * nfaces == 1; stamping a glyph's style high nibble with face index 1 makes
 * `fi (1) >= nfaces (1)` true, so the renderer clamps it to the default face and
 * the page still rasterises. Driven via ra8_reflow_render_page_at() to also
 * execute the offset render entry point.
 *
 * @par MC/DC:
 * The defensive `if (fi >= nfaces)` face-index clamp in internal_render_page is a
 * single-condition comparison, not a compound boolean. The only compound decision
 * the render path crosses is internal_blit_glyph's `if ((w > 0) && (h > 0))` glyph-box
 * guard, which is MC/DC-deactivated in the source (co-dependent box extents; mixed
 * vectors unreachable); its reachable arms are exercised by the sibling
 * test_render_glyph_size_both_arms.

 * @pre Pointer arguments, when present, address their documented test storage.
 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post The helper completes only the bounded test operation described above.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
 * @since 0.1.0
*/
RA8_INTERNAL static void internal_test_cov_render_face_index_clamp(void)
{
  TEST_BEGIN("render cov: out-of-range face index clamp");
  if (internal_load_font() == 0) {
    TEST_ASSERT_EQ(
      k_ra8_test_output_ok,
      internal_test_output_fd_text(STDERR_FILENO,
                                   "[SKIP] literata_latin1.ttf not found; skipping render\n"));
    TEST_END("render cov: out-of-range face index clamp");
    return;
  }
  TEST_ASSERT_EQ(k_ra8_ok, internal_init_engine());
  uint32_t    pages = 0U;
  const char* xhtml = "<p>Clamp me.</p>";
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_reflow_layout_chapter(&s_engine, (const uint8_t*)xhtml, strlen(xhtml), &pages));
  TEST_ASSERT(s_engine.glyph_count > 0U);

  /* Stamp face index 1 into every glyph's style high nibble. No face is
     registered (nfaces == 1), so fi == 1 >= nfaces forces the clamp. */
  for (uint32_t i = 0U; i < s_engine.glyph_count; ++i) {
    s_engine.glyphs[i].style = (uint8_t)(s_engine.glyphs[i].style | (uint8_t)k_rc_face_one_nibble);
  }

  internal_bind_clear_fb();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_reflow_render_page_at(&s_engine, 0, 0, 0));
  TEST_ASSERT(internal_fb_has_drawn_pixel());
  TEST_END("render cov: out-of-range face index clamp");
}

/**
 * @brief Test entry point.
 * @return 0 on success; unity macros exit(1) on the first failure.
 */
int main(void)
{
  internal_test_cov_render_bad_default_font();
  internal_test_cov_render_link_underline();
  internal_test_cov_render_raster_image();
  internal_test_cov_render_svg_image();
  internal_test_cov_render_face_index_clamp();
  TEST_ASSERT_EQ(
    k_ra8_test_output_ok,
    internal_test_output_fd_text(STDERR_FILENO, "[OK ] test_ra8_reflow_render_cov.c\n"));
  return 0;
}
