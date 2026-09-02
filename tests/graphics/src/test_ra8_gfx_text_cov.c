/**
 * @file test_ra8_gfx_text_cov.c
 * @brief Line-coverage tests for libs/ra8_gfx/src/ra8_gfx_text.c
 *
 * @details
 * Companion to test_ra8_gfx_text.c (which carries the MC/DC vectors). This TU
 * targets source lines the existing suite never executes, driving them through
 * the real public ra8_gfx API on a host-side framebuffer + the bundled 8x16
 * font. Each test documents the source lines it lands; execution -- not exact
 * pixels -- is the goal, so the assertions check return codes, measured sizes,
 * and that a covered pixel differs from the cleared background.
 *
 * Because the ra8_gfx module keeps its bind state in a single file-static, the
 * "not yet initialised" early-return guards can only be reached before any
 * successful ra8_gfx_init() in this process. test_uninitialized_guards() runs
 * first from main() for exactly that reason.
 *
 * Targeted source lines (libs/ra8_gfx/src/ra8_gfx_text.c):
 *   277-281  internal_put_pixel RGB565 case (via ra8_gfx_pixel)
 *   322-327  internal_get_pixel RGB565 case (via ra8_gfx_blit, src rgb565)
 *   329-333  internal_get_pixel RGB888 case (via ra8_gfx_blit, src rgb888)
 *   343      internal_get_pixel defensive `return 0U` -- UNREACHABLE via API
 *   565      ra8_gfx_clear not-initialised guard
 *   592,603,606,613,616  ra8_gfx_set_clip guard + corner clamps
 *   628      ra8_gfx_reset_clip not-initialised guard
 *   640      ra8_gfx_pixel not-initialised guard
 *   650,652  internal_gray_to_color (via ra8_gfx_blit_gray8)
 *   662,671-680,683  internal_blit_gray8_565 (RGB565 blit_gray8)
 *   687,689-691,693,696  internal_blit_gray8_slow (non-RGB565 blit_gray8)
 *   698,700-701,703-704,708-710,713-718,720  ra8_gfx_blit_gray8 body
 *   726      ra8_gfx_line not-initialised guard
 *   759      ra8_gfx_rect not-initialised guard
 *   833      ra8_gfx_circle not-initialised guard
 *   1002     ra8_gfx_text_out not-initialised guard
 *   1049     ra8_gfx_blit not-initialised guard
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_gfx.h"
#include "ra8_gfx_font.h"
#include "unity_minimal.h"

/** @brief Framebuffer geometry shared by every test in this TU. */
typedef enum : uint16_t {
  k_cov_dim = 64U, /**< Square framebuffer edge in pixels. */
} cov_dim_t;

/** @brief Bytes per pixel for the formats exercised here. */
typedef enum : uint32_t {
  k_cov_bpp_565  = 2U, /**< RGB565 bytes per pixel.   */
  k_cov_bpp_888  = 3U, /**< RGB888 bytes per pixel.   */
  k_cov_bpp_8888 = 4U, /**< ARGB8888 bytes per pixel. */
} cov_bpp_t;

/** @brief Distinct 0xAARRGGBB colours used for clear vs. draw. */
typedef enum : uint32_t {
  k_cov_col_bg = 0x000000FFU, /**< Background: blue (RGB565 lo!=hi).      */
  k_cov_col_fg = 0x00FF0000U, /**< Foreground: red  (distinct in RGB565). */
} cov_col_t;

/** @brief On-screen geometry for draws that must land inside the buffer. */
typedef enum : int32_t {
  k_cov_pos      = 3,    /**< On-screen draw origin (x and y).             */
  k_cov_blit_w   = 5,    /**< Source image width in pixels.                */
  k_cov_blit_h   = 4,    /**< Source image height in pixels.               */
  k_cov_off_pos  = 70,   /**< Origin past the right/bottom edge (> 64).    */
  k_cov_neg_span = -100, /**< Negative width/height to collapse the clip.  */
  k_cov_gray_dim = 6,    /**< Square gray block edge (clip-resolved path). */
} cov_geom_t;

/** @brief Gray sample level for the blit source rows (differs from background). */
typedef enum : uint8_t {
  k_cov_gray_hi = 0xE0U, /**< Bright gray sample (differs from background). */
} cov_gray_t;

/** @brief Module framebuffer: sized for the widest (ARGB8888) format. */
static uint8_t s_fb[(size_t)k_cov_dim * (size_t)k_cov_dim * (size_t)k_cov_bpp_8888];

/** @brief Source image for the RGB-blit get_pixel paths (ARGB8888-sized). */
static uint8_t s_blit_src[(size_t)k_cov_blit_w * (size_t)k_cov_blit_h * (size_t)k_cov_bpp_8888];

/** @brief Source gray buffer for ra8_gfx_blit_gray8 (one byte per sample). */
static uint8_t s_gray_src[(size_t)k_cov_gray_dim * (size_t)k_cov_gray_dim];

/**
 * @brief Byte offset of pixel (@p x, @p y) for stride @p bpp.
 *
 * @param[in] x   Pixel column.
 * @param[in] y   Pixel row.
 * @param[in] bpp Bytes per pixel of the bound format.
 * @return Byte index of that pixel's first byte in s_fb.
 * @retval 0 For the top-left pixel with any stride.
 * @pre The framebuffer is bound at k_cov_dim width.
 * @pre bpp is the bound format's bytes per pixel.
 * @post The returned offset is within s_fb.
 * @post The returned offset addresses the pixel's first byte.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static size_t cov_off(int32_t x, int32_t y, size_t bpp)
{
  return (((size_t)(uint32_t)y * (size_t)k_cov_dim) + (size_t)(uint32_t)x) * bpp;
}

/**
 * @brief Fill the gray source buffer with a non-background level.
 *
 * @return Nothing.
 * @pre s_gray_src has k_cov_gray_dim*k_cov_gray_dim bytes.
 * @pre k_cov_gray_hi differs from the background colour's gray.
 * @post Every sample equals k_cov_gray_hi.
 * @post No byte outside s_gray_src is touched.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static void cov_fill_gray(void)
{
  const size_t n = (size_t)k_cov_gray_dim * (size_t)k_cov_gray_dim;
  for (size_t i = 0; i < n; i++) {
    s_gray_src[i] = (uint8_t)k_cov_gray_hi;
  }
}

/**
 * @test test_uninitialized_guards
 *
 * @par Coverage:
 * Lands every "ra8_gfx_init() was not called" early return without first binding
 * the module, so it MUST run before any successful init in this TU. Targets
 * lines 565 (ra8_gfx_clear), 592 (ra8_gfx_set_clip), 628 (ra8_gfx_reset_clip),
 * 640 (ra8_gfx_pixel), 700-701 (ra8_gfx_blit_gray8), 726 (ra8_gfx_line),
 * 759 (ra8_gfx_rect), 833 (ra8_gfx_circle), 1002 (ra8_gfx_text_out, reached past
 * the non-null str/font check), and 1049 (ra8_gfx_blit, reached past the
 * non-null src_buf check).
 *
 * @par MC/DC:
 * The guard asserted for every API is the single-condition `if (!initialized)`
 * early return. Two entry points evaluate a leading check first and are reached
 * with valid pointers: ra8_gfx_blit's single-condition `src_buf == nullptr`, and
 * ra8_gfx_text_out's compound `(str == nullptr) || (font == nullptr)`. The latter
 * is thus traversed as the both-false control vector only -- no independence
 * vector is introduced here.
 */
static void test_uninitialized_guards(void)
{
  TEST_BEGIN("uninitialized guards: every API returns not_initialized");

  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_gfx_clear(k_cov_col_bg));       /* 565 */
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_gfx_set_clip(0, 0, 1, 1));      /* 592 */
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_gfx_reset_clip());              /* 628 */
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_gfx_pixel(0, 0, k_cov_col_fg)); /* 640 */
  /* 700-701 */
  TEST_ASSERT_EQ(k_ra8_err_not_initialized,
                 ra8_gfx_blit_gray8(s_gray_src, k_cov_gray_dim, k_cov_gray_dim, 0, 0));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_gfx_line(0, 0, 1, 1, k_cov_col_fg));       /* 726 */
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_gfx_rect(0, 0, 1, 1, k_cov_col_fg, true)); /* 759 */
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_gfx_circle(0, 0, 1, k_cov_col_fg, true));  /* 833 */
  TEST_ASSERT_EQ(
    k_ra8_err_not_initialized,
    ra8_gfx_text_out(0, 0, "x", &ra8_gfx_font_8x16, k_cov_col_fg, k_cov_col_bg)); /* 1002 */
  TEST_ASSERT_EQ(k_ra8_err_not_initialized,
                 ra8_gfx_blit(s_blit_src,
                              (uint16_t)k_cov_blit_w,
                              (uint16_t)k_cov_blit_h,
                              k_ra8_gfx_format_rgb565,
                              0,
                              0)); /* 1049 */

  TEST_END("uninitialized guards: every API returns not_initialized");
}

/**
 * @test test_put_pixel_565_path
 *
 * @par Coverage:
 * ra8_gfx_pixel in RGB565 routes through internal_plot -> internal_put_pixel and
 * takes the k_ra8_gfx_format_rgb565 switch arm, landing lines 277-281. The
 * covered pixel must differ from the cleared background.
 *
 * @par MC/DC:
 * This coverage companion drives the RGB565 arm of the internal_put_pixel switch
 * (a switch, not a boolean decision). The compound guards traversed on the way --
 * ra8_gfx_init's range checks, ra8_gfx_pixel's
 * `(x<0)||(y<0)||(x>=width)||(y>=height)` bounds check, and priv_gfx_text_plot's clip
 * ORs -- are all reached as all-false control vectors; their independence vectors
 * live in test_ra8_gfx_text.c. No new compound vector is introduced here.
 */
static void test_put_pixel_565_path(void)
{
  TEST_BEGIN("put_pixel: RGB565 switch arm (277-281)");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gfx_init(s_fb, k_cov_dim, k_cov_dim, k_ra8_gfx_format_rgb565));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gfx_clear(k_cov_col_bg));
  const size_t  off  = cov_off(k_cov_pos, k_cov_pos, (size_t)k_cov_bpp_565);
  const uint8_t base = s_fb[off];
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gfx_pixel(k_cov_pos, k_cov_pos, k_cov_col_fg)); /* 277-281 */
  TEST_ASSERT(s_fb[off] != base);
  TEST_END("put_pixel: RGB565 switch arm (277-281)");
}

/**
 * @test test_blit_get_pixel_565_888
 *
 * @par Coverage:
 * ra8_gfx_blit reads each source pixel with internal_get_pixel. A source bound as
 * k_ra8_gfx_format_rgb565 lands the RGB565 read arm (322-327); a source bound as
 * k_ra8_gfx_format_rgb888 lands the RGB888 read arm (329-333). The framebuffer
 * stays RGB565, so the converted pixels differ from the cleared background.
 *
 * @par MC/DC:
 * This drives the RGB565 and RGB888 arms of the internal_get_pixel switch (a
 * switch, not a boolean decision) by varying only the source format. The
 * `(src_w==0)||(src_h==0)||!internal_format_ok(src_format)` guard in ra8_gfx_blit
 * and priv_gfx_text_plot's clip ORs are traversed as all-false control vectors; the
 * independence vectors for those decisions live in test_ra8_gfx_text.c.
 */
static void test_blit_get_pixel_565_888(void)
{
  TEST_BEGIN("blit get_pixel: RGB565 (322-327) + RGB888 (329-333) read arms");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gfx_init(s_fb, k_cov_dim, k_cov_dim, k_ra8_gfx_format_rgb565));

  /* RGB565 source: two non-zero bytes per pixel -> non-background read. */
  const size_t n565 = (size_t)k_cov_blit_w * (size_t)k_cov_blit_h * (size_t)k_cov_bpp_565;
  for (size_t i = 0; i < n565; i++) {
    s_blit_src[i] = (uint8_t)k_cov_gray_hi;
  }
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gfx_clear(k_cov_col_bg));
  const size_t  off565  = cov_off(k_cov_pos, k_cov_pos, (size_t)k_cov_bpp_565);
  const uint8_t base565 = s_fb[off565];
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_gfx_blit(s_blit_src,
                              (uint16_t)k_cov_blit_w,
                              (uint16_t)k_cov_blit_h,
                              k_ra8_gfx_format_rgb565,
                              k_cov_pos,
                              k_cov_pos)); /* 322-327 */
  TEST_ASSERT(s_fb[off565] != base565);

  /* RGB888 source: three non-zero bytes per pixel -> non-background read. */
  const size_t n888 = (size_t)k_cov_blit_w * (size_t)k_cov_blit_h * (size_t)k_cov_bpp_888;
  for (size_t i = 0; i < n888; i++) {
    s_blit_src[i] = (uint8_t)k_cov_gray_hi;
  }
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gfx_clear(k_cov_col_bg));
  const size_t  off888  = cov_off(k_cov_pos, k_cov_pos, (size_t)k_cov_bpp_565);
  const uint8_t base888 = s_fb[off888];
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_gfx_blit(s_blit_src,
                              (uint16_t)k_cov_blit_w,
                              (uint16_t)k_cov_blit_h,
                              k_ra8_gfx_format_rgb888,
                              k_cov_pos,
                              k_cov_pos)); /* 329-333 */
  TEST_ASSERT(s_fb[off888] != base888);

  TEST_END("blit get_pixel: RGB565 (322-327) + RGB888 (329-333) read arms");
}

/**
 * @test test_set_clip_corner_clamps
 *
 * @par Coverage:
 * ra8_gfx_set_clip with a fully off-screen positive origin and negative spans
 * exercises the corner clamps: x0 > fbw (603), y0 > fbh (606), then x1 < x0
 * (613) and y1 < y0 (616). The result is an empty clip, so a following full
 * clear leaves the buffer untouched at a sampled pixel.
 *
 * @par MC/DC:
 * ra8_gfx_set_clip's corner clamps are single-condition (x0>fbw, y0>fbh, x1<x0,
 * y1<y0), each driven to its taken branch here; the not-taken branches are the
 * in-bounds set_clip in test_clip_confines_drawing (test_ra8_gfx_text.c). The
 * resulting empty clip then drives internal_fill_rect_565's
 * `(x1 <= x0) || (y1 <= y0)` to true (x1<=x0), matching V2 of that decision in
 * test_mcdc_fill_rect_565_clip, where its F,F and (y1<=y0) vectors also live.
 */
static void test_set_clip_corner_clamps(void)
{
  TEST_BEGIN("set_clip corner clamps: x0>fbw,y0>fbh,x1<x0,y1<y0 (603,606,613,616)");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gfx_init(s_fb, k_cov_dim, k_cov_dim, k_ra8_gfx_format_rgb565));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gfx_reset_clip());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gfx_clear(k_cov_col_bg));
  const size_t  off  = cov_off(k_cov_pos, k_cov_pos, (size_t)k_cov_bpp_565);
  const uint8_t base = s_fb[off];

  /* Origin past the bottom-right corner (clamps x0->fbw, y0->fbh) with negative
   * spans (xr,yr fall below the origin, so x1<x0 and y1<y0). */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_gfx_set_clip(k_cov_off_pos,
                                  k_cov_off_pos,
                                  k_cov_neg_span,
                                  k_cov_neg_span)); /* 603,606,613,616 */

  /* The clip is empty, so a full-screen clear must not touch the pixel. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gfx_clear(k_cov_col_fg));
  TEST_ASSERT_EQ(base, s_fb[off]);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gfx_reset_clip());
  TEST_END("set_clip corner clamps: x0>fbw,y0>fbh,x1<x0,y1<y0 (603,606,613,616)");
}

/**
 * @test test_blit_gray8_565_fast_path
 *
 * @par Coverage:
 * ra8_gfx_blit_gray8 in RGB565 runs its body (698,703-704) and the clipped fast
 * path: the format check (708) is false so it resolves the clip once (713-718)
 * and calls internal_blit_gray8_565 (671-680,683 plus its prologue 662), which
 * expands each gray sample through internal_gray_to_color (650,652) and stores
 * RGB565 bytes. With a visible on-screen block the covered pixel changes; line
 * 720 returns k_ra8_ok.
 *
 * @par MC/DC:
 * Decision: `(x0 < x1) && (y0 < y1)` in ra8_gfx_blit_gray8 (2 conditions), the
 * clipped-block guard before internal_blit_gray8_565. The on-screen block gives
 * x0<x1, y0<y1 (T,T) -> the taken vector (the covered pixel changes). This test
 * supplies only that both-true vector; the false vectors (a fully-clipped block)
 * are not varied here. The `(src==nullptr)||(w<=0)||(h<=0)` guard is all-false
 * control, its src==nullptr true leg being test_blit_gray8_arg_guard.
 */
static void test_blit_gray8_565_fast_path(void)
{
  TEST_BEGIN("blit_gray8 RGB565 fast path (650,652,662,671-680,683,698,703-704,713-718,720)");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gfx_init(s_fb, k_cov_dim, k_cov_dim, k_ra8_gfx_format_rgb565));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gfx_reset_clip());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gfx_clear(k_cov_col_bg));
  cov_fill_gray();
  const size_t  off  = cov_off(k_cov_pos, k_cov_pos, (size_t)k_cov_bpp_565);
  const uint8_t base = s_fb[off];
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_gfx_blit_gray8(s_gray_src,
                                    k_cov_gray_dim,
                                    k_cov_gray_dim,
                                    k_cov_pos,
                                    k_cov_pos)); /* 713-718,720 -> 662,671-683 */
  TEST_ASSERT(s_fb[off] != base);
  TEST_END("blit_gray8 RGB565 fast path (650,652,662,671-680,683,698,703-704,713-718,720)");
}

/**
 * @test test_blit_gray8_slow_path
 *
 * @par Coverage:
 * ra8_gfx_blit_gray8 in a non-RGB565 format takes the format-mismatch arm at 708
 * and calls internal_blit_gray8_slow (687,689-691,693,696 plus internal_plot),
 * returning at 709-710. The covered ARGB8888 pixel changes from the cleared
 * background.
 *
 * @par MC/DC:
 * This drives the single-condition `if (format != k_ra8_gfx_format_rgb565)`
 * dispatch in ra8_gfx_blit_gray8 to its true branch (the non-RGB565 per-pixel
 * slow path); the false branch is test_blit_gray8_565_fast_path. The
 * `(src==nullptr)||(w<=0)||(h<=0)` guard and priv_gfx_text_plot's clip ORs are
 * all-false control vectors, covered for independence in test_ra8_gfx_text.c.
 */
static void test_blit_gray8_slow_path(void)
{
  TEST_BEGIN("blit_gray8 non-RGB565 slow path (687,689-691,693,696,708-710)");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gfx_init(s_fb, k_cov_dim, k_cov_dim, k_ra8_gfx_format_argb8888));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gfx_clear(k_cov_col_bg));
  cov_fill_gray();
  const size_t  off  = cov_off(k_cov_pos, k_cov_pos, (size_t)k_cov_bpp_8888);
  const uint8_t base = s_fb[off];
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_gfx_blit_gray8(s_gray_src,
                                    k_cov_gray_dim,
                                    k_cov_gray_dim,
                                    k_cov_pos,
                                    k_cov_pos)); /* 708-710 -> 687,689-696 */
  TEST_ASSERT(s_fb[off] != base);
  TEST_END("blit_gray8 non-RGB565 slow path (687,689-691,693,696,708-710)");
}

/**
 * @test test_blit_gray8_arg_guard
 *
 * @par Coverage:
 * ra8_gfx_blit_gray8's argument guard (703-704): a NULL source returns
 * k_ra8_err_invalid_arg, confirming the validation line executes after the
 * not-initialised check passes.
 *
 * @par MC/DC:
 * Decision: `(src == nullptr) || (w <= 0) || (h <= 0)` in ra8_gfx_blit_gray8
 * (3 conditions). This supplies one vector: src=NULL, w=6, h=6 -> src==nullptr T
 * (short-circuit) -> k_ra8_err_invalid_arg. The all-false control vector is
 * test_blit_gray8_565_fast_path / test_blit_gray8_slow_path; the independent true
 * vectors for (w<=0) and (h<=0) are not exercised in this suite, so this
 * decision's MC/DC is only partially demonstrated here.
 */
static void test_blit_gray8_arg_guard(void)
{
  TEST_BEGIN("blit_gray8 arg guard: NULL src -> invalid_arg (703-704)");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gfx_init(s_fb, k_cov_dim, k_cov_dim, k_ra8_gfx_format_rgb565));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_gfx_blit_gray8(nullptr, k_cov_gray_dim, k_cov_gray_dim, 0, 0)); /* 703-704 */
  TEST_END("blit_gray8 arg guard: NULL src -> invalid_arg (703-704)");
}

/**
 * @test test_text_size_measures
 *
 * @par Coverage:
 * Exercises ra8_gfx_text_size as a supporting measurement check: a 3-character
 * string measures 3*glyph_width wide and glyph_height tall, confirming the
 * font is bound and usable for the rendering tests above.
 *
 * @par MC/DC:
 * ra8_gfx_text_size's only compound decision is its null guard
 * `(str==nullptr)||(font==nullptr)||(out_w==nullptr)||(out_h==nullptr)`; this test
 * passes four valid pointers, so it is traversed as the all-false control vector
 * only. The character-count loop's `str[i] == '\0'` test is single-condition. No
 * independence vector is introduced here.
 */
static void test_text_size_measures(void)
{
  TEST_BEGIN("text_size: measured width/height match the bound font");
  uint32_t w = 0;
  uint32_t h = 0;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gfx_text_size("abc", &ra8_gfx_font_8x16, &w, &h));
  TEST_ASSERT_EQ(ra8_gfx_font_8x16.glyph_width * 3U, w);
  TEST_ASSERT_EQ(ra8_gfx_font_8x16.glyph_height, h);
  TEST_END("text_size: measured width/height match the bound font");
}

/**
 * @brief Test entry point.
 *
 * @return Process exit status.
 * @retval 0 All tests passed (a failure exits non-zero via TEST_FAIL_FMT).
 * @pre The bundled 8x16 font is linked in.
 * @pre s_fb is large enough for an ARGB8888 64x64 framebuffer.
 * @post Every targeted source line has executed at least once.
 * @post stderr carries the per-test [RUN]/[PASS] trace.
 * @note Not thread-safe; runs single-threaded.
 * @since 0.1.0
 */
int main(void)
{
  /* MUST be first: lands the not-initialised guards before any init. */
  test_uninitialized_guards();
  test_put_pixel_565_path();
  test_blit_get_pixel_565_888();
  test_set_clip_corner_clamps();
  test_blit_gray8_565_fast_path();
  test_blit_gray8_slow_path();
  test_blit_gray8_arg_guard();
  test_text_size_measures();
  return 0;
}
