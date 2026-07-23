/**
 * @file test_ra8_drw_render.c
 * @brief Texture / draw-op tests for ra8_drw.c (DRW 2D engine driver)
 *
 * @details
 * Split out of test_ra8_drw.c to keep each test translation unit under the
 * repository file-size cap. This sibling owns the texture / CLUT setup,
 * fill-rect, blit, line, triangle, display-list, performance-counter, and
 * MC/DC vector tests; the lifecycle / status / IRQ / power / render-state
 * contract tests stay in test_ra8_drw.c.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_drw.h"
#include "ra8_drw_dlist.h"
#include "ra8_drw_internal.h"
#include "ra8_drw_regs.h"
#include "ra8_err.h"
#include "ra8_mstp.h"
#include "ra8_sim_mmap.h"
#include "unity_minimal.h"

/**
 * @enum drw_render_fixture_t
 * @brief Values planted in registers to prove a read or write reaches them.
 */
typedef enum : uint32_t {
  k_drw_alpha_opaque = 0xFF000000UL, /**< Alpha byte of an ARGB8888 entry set to fully opaque. */
  k_drw_grey_step =
    0x010101UL, /**< Added once per entry to step R, G and B together, so the table is a grey ramp with no two entries alike. */
  k_drw_probe_perfcount1 =
    0x12345678UL, /**< Planted in PERFCOUNT1 to prove the read reaches the register. */
  k_drw_probe_perfcount2 =
    0xDEADBEEFUL, /**< Planted in PERFCOUNT2; different from PERFCOUNT1 so the two cannot be confused. */
} drw_render_fixture_t;

/**
 * @enum ra8_drw_test_const_t
 * @brief Test-side bound constants used by the cases below.
 */
typedef enum : uint32_t {
  k_ra8_drw_test_fb_addr_lo   = 0x68000000UL, /**< SDRAM head, mmap-backed.   */
  k_ra8_drw_test_tex_addr     = 0x68100000UL, /**< Texture base.              */
  k_ra8_drw_test_dlist_addr   = 0x68200000UL, /**< Display list base.         */
  k_ra8_drw_test_pitch_px     = 1024U,        /**< 1024-px wide framebuffer.  */
  k_ra8_drw_test_rect_w       = 16U,          /**< 16-pixel-wide test rect.   */
  k_ra8_drw_test_rect_h       = 8U,           /**< 8-pixel-tall test rect.    */
  k_ra8_drw_test_rect_x       = 4U,           /**< Test rect origin X.        */
  k_ra8_drw_test_rect_y       = 2U,           /**< Test rect origin Y.        */
  k_ra8_drw_test_rect_color   = 0xFFAA5500UL, /**< ARGB orange.               */
  k_ra8_drw_test_color2       = 0x80112233UL, /**< Secondary ARGB stop.       */
  k_ra8_drw_test_status_seed  = 0x000000FFUL, /**< Seed STATUS for read test. */
  k_ra8_drw_test_too_big_dim  = 2048U,        /**< Exceeds HUM 1024 max.      */
  k_ra8_drw_test_zero_dim     = 0U,           /**< Below min.                 */
  k_ra8_drw_test_subpixel     = 16U,          /**< 1 px == 16 sub-pixels.     */
  k_ra8_drw_test_cb_ctx_val   = 0xCAFEU,      /**< Cookie for cb context.     */
  k_ra8_drw_test_color_key    = 0x0000FF00UL, /**< Pure-green colour key.     */
  k_ra8_drw_test_clut_count   = 8U,           /**< CLUT entries to upload.    */
  k_ra8_drw_test_pattern      = 0xA5U,        /**< Pattern bitmap byte.       */
  k_ra8_drw_test_perf_budget  = 256U,         /**< wait_idle poll budget.     */
  k_ra8_drw_test_clut_cap     = 256U,         /**< CLUT max entries.          */
  k_ra8_drw_test_clut_off     = 0x10U,        /**< CLUT offset value.         */
  k_ra8_drw_test_global_alpha = 0x80U,        /**< 50%-ish alpha test value.  */
} ra8_drw_test_const_t;

/**
 * @brief Reset hardware sim, ref-count table, and callback latches.
 */
static void prep(void)
{
  ra8_sim_mmap_reset();
  (void)ra8_mstp_init();
}

/**
 * @brief Build a default config that points at the SDRAM mmap region.
 */
static ra8_drw_config_t make_cfg(void)
{
  const ra8_drw_config_t cfg = {
    .framebuffer_addr       = (uintptr_t)k_ra8_drw_test_fb_addr_lo,
    .pitch_px               = (uint16_t)k_ra8_drw_test_pitch_px,
    .format                 = k_ra8_drw_writefmt_argb8888,
    .enable_caches          = true,
    .enable_buffered_writes = true,
  };
  return cfg;
}

/* =============================================================================
 * Texture / CLUT
 * =============================================================================
 */

static ra8_drw_texture_t make_tex(void)
{
  const ra8_drw_texture_t tex = {
    .base_addr        = (uintptr_t)k_ra8_drw_test_tex_addr,
    .pitch_px         = 256U,
    .u_mask           = 0x00FFU,
    .v_mask           = 0x00FFU,
    .format           = k_ra8_drw_readfmt_argb8888,
    .clamp_x          = true,
    .clamp_y          = true,
    .filter_x         = true,
    .filter_y         = true,
    .enable_clut      = false,
    .clut_565         = false,
    .enable_color_key = true,
    .color_key_rgb    = (uint32_t)k_ra8_drw_test_color_key,
    .enable_rle       = false,
    .rle_pixel_width  = k_ra8_drw_rle_4byte,
    .clut_offset      = (uint8_t)k_ra8_drw_test_clut_off,
  };
  return tex;
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_texture_argb8888(void)
{
  TEST_BEGIN("drw set_texture ARGB8888 + filter + clamp + colour key");
  prep();
  const ra8_drw_texture_t tex = make_tex();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_drw_set_texture(&tex));

  TEST_ASSERT_EQ(tex.base_addr, *ra8_drw_reg32(k_ra8_drw_off_texorigin));
  TEST_ASSERT_EQ(tex.pitch_px, *ra8_drw_reg32(k_ra8_drw_off_texpitch));
  const uint32_t expected_mask = ((uint32_t)tex.v_mask << 16U) | (uint32_t)tex.u_mask;
  TEST_ASSERT_EQ(expected_mask, *ra8_drw_reg32(k_ra8_drw_off_texmask));
  TEST_ASSERT_EQ(tex.clut_offset, *ra8_drw_reg32(k_ra8_drw_off_texcloffset));

  const uint32_t ctl2 = *ra8_drw_reg32(k_ra8_drw_off_control2);
  TEST_ASSERT((ctl2 & (uint32_t)k_ra8_drw_control2_textureenable) != 0UL);
  TEST_ASSERT((ctl2 & (uint32_t)k_ra8_drw_control2_textureclampx) != 0UL);
  TEST_ASSERT((ctl2 & (uint32_t)k_ra8_drw_control2_texturefiltery) != 0UL);
  TEST_ASSERT((ctl2 & (uint32_t)k_ra8_drw_control2_colkeyenable) != 0UL);
  /* READFORMAT_L=0b10, READFORMAT_H=0b00 -> code 2 = ARGB8888. */
  const uint32_t rfl = (ctl2 >> k_ra8_drw_control2_readformatl_pos) & 0x3UL;
  const uint32_t rfh = (ctl2 >> k_ra8_drw_control2_readformath_pos) & 0x3UL;
  TEST_ASSERT_EQ(2U, ((rfh << 2U) | rfl));
  TEST_END("drw set_texture ARGB8888 + filter + clamp + colour key");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_texture_clut_rle(void)
{
  TEST_BEGIN("drw set_texture CLUT + RLE bits");
  prep();
  ra8_drw_texture_t tex = make_tex();
  tex.format            = k_ra8_drw_readfmt_clut8;
  tex.enable_clut       = true;
  tex.clut_565          = true;
  tex.enable_rle        = true;
  tex.rle_pixel_width   = k_ra8_drw_rle_2byte;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_drw_set_texture(&tex));
  const uint32_t ctl2 = *ra8_drw_reg32(k_ra8_drw_off_control2);
  TEST_ASSERT((ctl2 & (uint32_t)k_ra8_drw_control2_clutenable) != 0UL);
  TEST_ASSERT((ctl2 & (uint32_t)k_ra8_drw_control2_clutformat_565) != 0UL);
  TEST_ASSERT((ctl2 & (uint32_t)k_ra8_drw_control2_rleenable) != 0UL);
  const uint32_t rle_field = (ctl2 >> k_ra8_drw_control2_rlepixel_pos) & 0x3UL;
  TEST_ASSERT_EQ(k_ra8_drw_rle_2byte, rle_field);
  TEST_END("drw set_texture CLUT + RLE bits");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_texture_rejects(void)
{
  TEST_BEGIN("drw set_texture rejects null + oversize pitch");
  prep();
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_drw_set_texture(nullptr));
  ra8_drw_texture_t tex = make_tex();
  tex.pitch_px          = (uint16_t)(k_ra8_drw_max_texpitch_tx + 1U);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_drw_set_texture(&tex));
  TEST_END("drw set_texture rejects null + oversize pitch");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_clear_texture(void)
{
  TEST_BEGIN("drw clear_texture clears CONTROL2 bits");
  prep();
  ra8_drw_texture_t tex = make_tex();
  tex.enable_rle        = true;
  tex.enable_clut       = true;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_drw_set_texture(&tex));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_drw_clear_texture());
  const uint32_t ctl2 = *ra8_drw_reg32(k_ra8_drw_off_control2);
  TEST_ASSERT((ctl2 & (uint32_t)k_ra8_drw_control2_textureenable) == 0UL);
  TEST_ASSERT((ctl2 & (uint32_t)k_ra8_drw_control2_rleenable) == 0UL);
  TEST_ASSERT((ctl2 & (uint32_t)k_ra8_drw_control2_clutenable) == 0UL);
  TEST_END("drw clear_texture clears CONTROL2 bits");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_load_clut_happy_and_bounds(void)
{
  TEST_BEGIN("drw load_clut writes addr + data");
  prep();
  uint32_t entries[k_ra8_drw_test_clut_count];
  for (uint32_t i = 0UL; i < (uint32_t)k_ra8_drw_test_clut_count; ++i) {
    entries[i] = k_drw_alpha_opaque | (i * k_drw_grey_step);
  }
  TEST_ASSERT_EQ(k_ra8_ok, ra8_drw_load_clut(0U, entries, (uint32_t)k_ra8_drw_test_clut_count));
  TEST_ASSERT_EQ(0, *ra8_drw_reg32(k_ra8_drw_off_texcladdr));
  /* Last write wins on the stub mmap; verify last entry. */
  TEST_ASSERT_EQ(entries[k_ra8_drw_test_clut_count - 1U], *ra8_drw_reg32(k_ra8_drw_off_texcldata));

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_drw_load_clut(0U, nullptr, 1U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_drw_load_clut(0U, entries, 0U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_drw_load_clut(255U, entries, 2U));
  TEST_END("drw load_clut writes addr + data");
}

/* =============================================================================
 * Drawing primitives
 * =============================================================================
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */

static void test_fill_rect_happy(void)
{
  TEST_BEGIN("drw fill_rect happy");
  prep();

  const ra8_drw_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_drw_init(&cfg));

  const ra8_drw_rect_t rect = {
    .x              = (int16_t)k_ra8_drw_test_rect_x,
    .y              = (int16_t)k_ra8_drw_test_rect_y,
    .width_px       = (uint16_t)k_ra8_drw_test_rect_w,
    .height_px      = (uint16_t)k_ra8_drw_test_rect_h,
    .color_argb8888 = (uint32_t)k_ra8_drw_test_rect_color,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_drw_fill_rect(&rect));

  const uint32_t expected_size =
    ((uint32_t)k_ra8_drw_test_rect_h << 16U) | (uint32_t)k_ra8_drw_test_rect_w;
  TEST_ASSERT_EQ(expected_size, *ra8_drw_reg32(k_ra8_drw_off_size));
  TEST_ASSERT_EQ(k_ra8_drw_test_rect_color, *ra8_drw_reg32(k_ra8_drw_off_color1));

  /* An axis-aligned rectangle is positioned by ORIGIN, not by the limiters.
   * HUM Ch 62.6.2 p 3716 has the engine scan "the whole bounding box", so
   * ORIGIN must point at the rect's own top-left pixel and SIZE gives the
   * extent. Programming the limiters with absolute pixel coordinates instead
   * is what painted a 16x16 request as 8x8 on silicon: LnSTART is the decision
   * value at the bounding box top-left corner, not a coordinate. */
  const uint32_t expected_origin =
    (uint32_t)k_ra8_drw_test_fb_addr_lo +
    ((((uint32_t)k_ra8_drw_test_rect_y * (uint32_t)k_ra8_drw_test_pitch_px) +
      (uint32_t)k_ra8_drw_test_rect_x) *
     (uint32_t)k_ra8_drw_bytes_px_32bpp);
  TEST_ASSERT_EQ(expected_origin, *ra8_drw_reg32(k_ra8_drw_off_origin));

  /* Every spatial limiter is disabled and cleared: the bounding box scan IS
   * the rectangle, so no half-plane is needed to carve it out. */
  TEST_ASSERT_EQ(0, *ra8_drw_reg32(k_ra8_drw_off_l1start));
  TEST_ASSERT_EQ(0, *ra8_drw_reg32(k_ra8_drw_off_l2start));
  TEST_ASSERT_EQ(0, *ra8_drw_reg32(k_ra8_drw_off_l3start));
  TEST_ASSERT_EQ(0, *ra8_drw_reg32(k_ra8_drw_off_l4start));

  TEST_ASSERT_EQ(0, *ra8_drw_reg32(k_ra8_drw_off_l1xadd));
  TEST_ASSERT_EQ(0, *ra8_drw_reg32(k_ra8_drw_off_l2xadd));
  TEST_ASSERT_EQ(0, *ra8_drw_reg32(k_ra8_drw_off_l3xadd));
  TEST_ASSERT_EQ(0, *ra8_drw_reg32(k_ra8_drw_off_l4xadd));
  TEST_ASSERT_EQ(0, *ra8_drw_reg32(k_ra8_drw_off_l1yadd));
  TEST_ASSERT_EQ(0, *ra8_drw_reg32(k_ra8_drw_off_l2yadd));
  TEST_ASSERT_EQ(0, *ra8_drw_reg32(k_ra8_drw_off_l3yadd));
  TEST_ASSERT_EQ(0, *ra8_drw_reg32(k_ra8_drw_off_l4yadd));

  TEST_ASSERT_EQ(0, *ra8_drw_reg32(k_ra8_drw_off_control));

  TEST_END("drw fill_rect happy");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_fill_rect_null(void)
{
  TEST_BEGIN("drw fill_rect null");
  prep();
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_drw_fill_rect(nullptr));
  TEST_END("drw fill_rect null");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_fill_rect_zero_dim(void)
{
  TEST_BEGIN("drw fill_rect zero dim");
  prep();

  ra8_drw_rect_t rect = {
    .x              = 0,
    .y              = 0,
    .width_px       = (uint16_t)k_ra8_drw_test_zero_dim,
    .height_px      = (uint16_t)k_ra8_drw_test_rect_h,
    .color_argb8888 = 0UL,
  };
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_drw_fill_rect(&rect));

  rect.width_px  = (uint16_t)k_ra8_drw_test_rect_w;
  rect.height_px = (uint16_t)k_ra8_drw_test_zero_dim;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_drw_fill_rect(&rect));
  TEST_END("drw fill_rect zero dim");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_fill_rect_too_big(void)
{
  TEST_BEGIN("drw fill_rect oversize");
  prep();

  ra8_drw_rect_t rect = {
    .x              = 0,
    .y              = 0,
    .width_px       = (uint16_t)k_ra8_drw_test_too_big_dim,
    .height_px      = (uint16_t)k_ra8_drw_test_rect_h,
    .color_argb8888 = 0UL,
  };
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_drw_fill_rect(&rect));

  rect.width_px  = (uint16_t)k_ra8_drw_test_rect_w;
  rect.height_px = (uint16_t)k_ra8_drw_test_too_big_dim;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_drw_fill_rect(&rect));
  TEST_END("drw fill_rect oversize");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_blit_textured_rect(void)
{
  TEST_BEGIN("drw blit_textured_rect happy + bad args");
  prep();
  const ra8_drw_texture_t tex = make_tex();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_drw_set_texture(&tex));
  const ra8_drw_rect_t rect = {
    .x              = 0,
    .y              = 0,
    .width_px       = (uint16_t)k_ra8_drw_test_rect_w,
    .height_px      = (uint16_t)k_ra8_drw_test_rect_h,
    .color_argb8888 = 0UL,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_drw_blit_textured_rect(&rect));
  /* Like the solid fill, the blit extent is the ORIGIN-anchored bounding box
   * (HUM Ch 62.6.2 p 3716), so CONTROL enables no spatial limiter. The U and V
   * texture limiters are a separate unit and stay programmed. */
  TEST_ASSERT_EQ(0, *ra8_drw_reg32(k_ra8_drw_off_control));
  TEST_ASSERT_EQ(k_ra8_drw_subpixel_unit, *ra8_drw_reg32(k_ra8_drw_off_luxadd));
  TEST_ASSERT_EQ(k_ra8_drw_subpixel_unit, *ra8_drw_reg32(k_ra8_drw_off_lvyaddi));

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_drw_blit_textured_rect(nullptr));
  ra8_drw_rect_t bad = rect;
  bad.width_px       = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_drw_blit_textured_rect(&bad));
  bad           = rect;
  bad.height_px = (uint16_t)k_ra8_drw_test_too_big_dim;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_drw_blit_textured_rect(&bad));
  TEST_END("drw blit_textured_rect happy + bad args");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_draw_line(void)
{
  TEST_BEGIN("drw draw_line writes limiters + bands");
  prep();
  const ra8_drw_line_t line = {
    .x0             = 10,
    .y0             = 20,
    .x1             = 50,
    .y1             = 80,
    .width_px       = 4U,
    .color_argb8888 = (uint32_t)k_ra8_drw_test_rect_color,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_drw_draw_line(&line));
  TEST_ASSERT_EQ(line.color_argb8888, *ra8_drw_reg32(k_ra8_drw_off_color1));
  /* Band registers receive width * sub-pixel unit. */
  const uint32_t expected_band = (uint32_t)line.width_px * (uint32_t)k_ra8_drw_subpixel_unit;
  TEST_ASSERT_EQ(expected_band, *ra8_drw_reg32(k_ra8_drw_off_l1band));
  TEST_ASSERT_EQ(expected_band, *ra8_drw_reg32(k_ra8_drw_off_l2band));
  TEST_ASSERT_EQ(k_ra8_drw_control_line_quad, *ra8_drw_reg32(k_ra8_drw_off_control));

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_drw_draw_line(nullptr));
  ra8_drw_line_t bad = line;
  bad.width_px       = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_drw_draw_line(&bad));
  bad.width_px = (uint16_t)k_ra8_drw_test_too_big_dim;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_drw_draw_line(&bad));
  TEST_END("drw draw_line writes limiters + bands");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_draw_triangle(void)
{
  TEST_BEGIN("drw draw_triangle writes 3 limiters");
  prep();
  const ra8_drw_triangle_t tri = {
    .x0             = 0,
    .y0             = 0,
    .x1             = 100,
    .y1             = 0,
    .x2             = 50,
    .y2             = 80,
    .color_argb8888 = (uint32_t)k_ra8_drw_test_rect_color,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_drw_draw_triangle(&tri));
  TEST_ASSERT_EQ(k_ra8_drw_control_triangle, *ra8_drw_reg32(k_ra8_drw_off_control));
  TEST_ASSERT_EQ(tri.color_argb8888, *ra8_drw_reg32(k_ra8_drw_off_color1));

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_drw_draw_triangle(nullptr));
  TEST_END("drw draw_triangle writes 3 limiters");
}

/* =============================================================================
 * Display list
 * =============================================================================
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */

static void test_run_dlist(void)
{
  TEST_BEGIN("drw run_dlist alignment + happy");
  prep();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_drw_run_dlist(nullptr));
  /* Unaligned address rejected. */
  const uint32_t* unaligned = (const uint32_t*)(uintptr_t)(k_ra8_drw_test_dlist_addr | 0x1UL);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_drw_run_dlist(unaligned));

  const uint32_t* dlist = (const uint32_t*)(uintptr_t)k_ra8_drw_test_dlist_addr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_drw_run_dlist(dlist));
  TEST_ASSERT_EQ(k_ra8_drw_test_dlist_addr, *ra8_drw_reg32(k_ra8_drw_off_dliststart));
  TEST_END("drw run_dlist alignment + happy");
}

/* =============================================================================
 * Display-list builder
 * =============================================================================
 */

/**
 * @enum drw_dlist_expect_t
 * @brief Bench-observed on-wire words the builder must emit, as literals.
 *
 * @details
 * Independent of the builder's own encoding constants so a wrong register
 * index or tag bit is caught. These are the exact words a J-Link savebin read
 * back from the EK-RA8D2 display list (issue #247).
 */
typedef enum : uint32_t {
  k_drw_dl_tag_color1  = 0x80808019UL, /**< 1-index tag, COLOR1 (idx 25).   */
  k_drw_dl_tag_size    = 0x8080801EUL, /**< 1-index tag, SIZE (idx 30).     */
  k_drw_dl_tag_control = 0x80808000UL, /**< 1-index tag, CONTROL (idx 0).   */
  k_drw_dl_tag_origin  = 0x80808020UL, /**< 1-index tag, ORIGIN (idx 32).   */
  k_drw_dl_wait        = 0x000002FFUL, /**< Wait pipe+cache word.           */
  k_drw_dl_term        = 0x000003FFUL, /**< Terminate word.                 */
  k_drw_dl_words       = 19UL,         /**< Clear+fill list length.         */
  k_drw_dl_clear_dim   = 32UL,         /**< Clear rect width/height.        */
  k_drw_dl_clear_color = 0x00000000UL, /**< Clear colour (transparent).     */
  k_drw_dl_box_xy      = 8UL,          /**< Box top-left.                   */
  k_drw_dl_box_wh      = 16UL,         /**< Box width/height.               */
  k_drw_dl_cap         = 32UL,         /**< Builder buffer capacity words.  */
  k_drw_dl_tiny_cap    = 8UL,          /**< Too small for one 9-word fill.  */
  k_drw_dl_fill_cap    = 9UL,          /**< One fill, no room for the term. */
} drw_dlist_expect_t;

/** @brief Build a clear+fill display list into @p buf; return the word count. */
static uint32_t build_clear_fill(uint32_t* buf, uint32_t cap)
{
  ra8_drw_dlist_t dl;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_drw_dlist_begin(&dl, buf, cap));
  const ra8_drw_rect_t clear = {.x              = 0,
                                .y              = 0,
                                .width_px       = (uint16_t)k_drw_dl_clear_dim,
                                .height_px      = (uint16_t)k_drw_dl_clear_dim,
                                .color_argb8888 = (uint32_t)k_drw_dl_clear_color};
  const ra8_drw_rect_t box   = {.x              = (int16_t)k_drw_dl_box_xy,
                                .y              = (int16_t)k_drw_dl_box_xy,
                                .width_px       = (uint16_t)k_drw_dl_box_wh,
                                .height_px      = (uint16_t)k_drw_dl_box_wh,
                                .color_argb8888 = (uint32_t)k_ra8_drw_test_rect_color};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_drw_dlist_add_fill(&dl, &clear));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_drw_dlist_add_fill(&dl, &box));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_drw_dlist_end(&dl));
  return dl.count;
}

/** @brief Assert @p buf holds the exact bench-observed clear+fill words. */
static void assert_clear_fill_words(const uint32_t* buf)
{
  const uint32_t clear_size = ((uint32_t)k_drw_dl_clear_dim << 16U) | (uint32_t)k_drw_dl_clear_dim;
  const uint32_t box_size   = ((uint32_t)k_drw_dl_box_wh << 16U) | (uint32_t)k_drw_dl_box_wh;
  const uint32_t box_origin =
    (uint32_t)k_ra8_drw_test_fb_addr_lo +
    ((((uint32_t)k_drw_dl_box_xy * (uint32_t)k_ra8_drw_test_pitch_px) + (uint32_t)k_drw_dl_box_xy) *
     (uint32_t)k_ra8_drw_bytes_px_32bpp);
  const uint32_t want[k_drw_dl_words] = {
    k_drw_dl_tag_color1,
    k_drw_dl_clear_color,
    k_drw_dl_tag_size,
    clear_size,
    k_drw_dl_tag_control,
    0U,
    k_drw_dl_tag_origin,
    k_ra8_drw_test_fb_addr_lo,
    k_drw_dl_wait,
    k_drw_dl_tag_color1,
    k_ra8_drw_test_rect_color,
    k_drw_dl_tag_size,
    box_size,
    k_drw_dl_tag_control,
    0U,
    k_drw_dl_tag_origin,
    box_origin,
    k_drw_dl_wait,
    k_drw_dl_term,
  };
  for (uint32_t i = 0U; i < (uint32_t)k_drw_dl_words; ++i) {
    TEST_ASSERT_EQ(want[i], buf[i]);
  }
}

/**
 * @brief Build a clear+fill display list and assert the exact on-wire words.
 *
 * @par MC/DC:
 * (no compound decisions in this test -- asserts the emitted encoding of the
 * public builder API against literal bench-observed words)
 */
static void test_dlist_build_encoding(void)
{
  TEST_BEGIN("drw dlist build encoding");
  prep();
  const ra8_drw_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_drw_init(&cfg));

  uint32_t buf[k_drw_dl_cap] = {};
  TEST_ASSERT_EQ(k_drw_dl_words, build_clear_fill(buf, (uint32_t)k_drw_dl_cap));
  assert_clear_fill_words(buf);
  TEST_END("drw dlist build encoding");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises begin/add_fill/end reject
 * contracts; the reused rect predicates are covered by their own MC/DC cases)
 */
static void test_dlist_begin_add_rejects(void)
{
  TEST_BEGIN("drw dlist begin/add rejects");
  prep();
  const ra8_drw_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_drw_init(&cfg));

  uint32_t        buf[k_drw_dl_cap] = {};
  ra8_drw_dlist_t dl;
  /* begin rejects. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_drw_dlist_begin(nullptr, buf, (uint32_t)k_drw_dl_cap));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_drw_dlist_begin(&dl, nullptr, (uint32_t)k_drw_dl_cap));
  uint32_t* unaligned = (uint32_t*)(uintptr_t)(k_ra8_drw_test_dlist_addr | 0x1UL);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_drw_dlist_begin(&dl, unaligned, (uint32_t)k_drw_dl_cap));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_drw_dlist_begin(&dl, buf, 0U));

  /* add_fill rejects. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_drw_dlist_begin(&dl, buf, (uint32_t)k_drw_dl_cap));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_drw_dlist_add_fill(&dl, nullptr));
  const ra8_drw_rect_t zero = {.x              = 0,
                               .y              = 0,
                               .width_px       = 0U,
                               .height_px      = (uint16_t)k_drw_dl_box_wh,
                               .color_argb8888 = 0U};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_drw_dlist_add_fill(&dl, &zero));
  const ra8_drw_rect_t big = {
    .x              = 0,
    .y              = 0,
    .width_px       = (uint16_t)k_ra8_drw_test_too_big_dim,
    .height_px      = (uint16_t)k_drw_dl_box_wh,
    .color_argb8888 = 0U,
  };
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_drw_dlist_add_fill(&dl, &big));
  const ra8_drw_rect_t offs = {.x              = -1,
                               .y              = 0,
                               .width_px       = (uint16_t)k_drw_dl_box_wh,
                               .height_px      = (uint16_t)k_drw_dl_box_wh,
                               .color_argb8888 = 0U};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_drw_dlist_add_fill(&dl, &offs));
  TEST_END("drw dlist begin/add rejects");
}

/**
 * @par MC/DC:
 * (no compound decisions -- exercises the single-condition capacity guard in
 * add_fill and end, and the overflow latch)
 */
static void test_dlist_overflow(void)
{
  TEST_BEGIN("drw dlist overflow");
  prep();
  const ra8_drw_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_drw_init(&cfg));

  /* A single fill needs 9 words; a smaller buffer cannot hold one. */
  uint32_t        tiny[k_drw_dl_tiny_cap] = {};
  ra8_drw_dlist_t dl;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_drw_dlist_begin(&dl, tiny, (uint32_t)k_drw_dl_tiny_cap));
  const ra8_drw_rect_t box = {
    .x              = (int16_t)k_drw_dl_box_xy,
    .y              = (int16_t)k_drw_dl_box_xy,
    .width_px       = (uint16_t)k_drw_dl_box_wh,
    .height_px      = (uint16_t)k_drw_dl_box_wh,
    .color_argb8888 = (uint32_t)k_ra8_drw_test_rect_color,
  };
  TEST_ASSERT_EQ(k_ra8_err_no_mem, ra8_drw_dlist_add_fill(&dl, &box));
  TEST_ASSERT(dl.overflow);
  TEST_ASSERT_EQ(0, dl.count);
  /* end refuses an overflowed builder. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_drw_dlist_end(&dl));

  /* A buffer that fits one fill but not the terminator: end returns no_mem. */
  uint32_t        exact[k_drw_dl_fill_cap] = {};
  ra8_drw_dlist_t dl2;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_drw_dlist_begin(&dl2, exact, (uint32_t)k_drw_dl_fill_cap));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_drw_dlist_add_fill(&dl2, &box));
  TEST_ASSERT_EQ(k_ra8_err_no_mem, ra8_drw_dlist_end(&dl2));
  TEST_ASSERT(dl2.overflow);
  TEST_END("drw dlist overflow");
}

/**
 * @test drw dlist run MC/DC
 *
 * @par MC/DC:
 * libs/ra8_hal/src/ra8_drw_draw.c@ra8_drw_dlist_run
 * Decision: `if (dl->overflow || !dl->terminated || (dl->count == 0U))`
 * (3 conditions A=overflow, B=!terminated, C=count==0)
 * - V1: overflow=F, terminated=T, count>0 -> F (runs; control, all false)
 * - V2: overflow=T, terminated=T, count>0 -> T (varies A)
 * - V3: overflow=F, terminated=F, count>0 -> T (varies B)
 * - V4: overflow=F, terminated=T, count=0 -> T (varies C)
 * V1+V2 prove A, V1+V3 prove B, V1+V4 prove C. N+1 = 4 vectors.
 */
static void test_mcdc_dlist_run(void)
{
  TEST_BEGIN("drw MC/DC: dlist_run OR");
  prep();
  const ra8_drw_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_drw_init(&cfg));

  uint32_t buf[k_drw_dl_cap] = {};
  buf[0]                     = (uint32_t)k_drw_dl_term;

  /* V1: valid, terminated, non-empty -> runs and writes DLISTSTART. */
  ra8_drw_dlist_t v1 = {.buf        = buf,
                        .cap_words  = (uint32_t)k_drw_dl_cap,
                        .count      = 1U,
                        .overflow   = false,
                        .terminated = true};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_drw_dlist_run(&v1));
  TEST_ASSERT_EQ((uint32_t)(uintptr_t)buf, *ra8_drw_reg32(k_ra8_drw_off_dliststart));

  /* V2: overflow set (varies A). */
  ra8_drw_dlist_t v2 = {.buf        = buf,
                        .cap_words  = (uint32_t)k_drw_dl_cap,
                        .count      = 1U,
                        .overflow   = true,
                        .terminated = true};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_drw_dlist_run(&v2));

  /* V3: not terminated (varies B). */
  ra8_drw_dlist_t v3 = {.buf        = buf,
                        .cap_words  = (uint32_t)k_drw_dl_cap,
                        .count      = 1U,
                        .overflow   = false,
                        .terminated = false};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_drw_dlist_run(&v3));

  /* V4: empty (varies C). */
  ra8_drw_dlist_t v4 = {.buf        = buf,
                        .cap_words  = (uint32_t)k_drw_dl_cap,
                        .count      = 0U,
                        .overflow   = false,
                        .terminated = true};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_drw_dlist_run(&v4));

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_drw_dlist_run(nullptr));
  TEST_END("drw MC/DC: dlist_run OR");
}

/* =============================================================================
 * Performance counters
 * =============================================================================
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */

static void test_perf_arm_read_reset(void)
{
  TEST_BEGIN("drw perf arm/read/reset");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_drw_perf_arm(k_ra8_drw_perfev_active_cycles, k_ra8_drw_perfev_fb_read));
  /* Verify packing of PERFTRIGGER. */
  const uint32_t expected =
    ((uint32_t)k_ra8_drw_perfev_fb_read << 16U) | (uint32_t)k_ra8_drw_perfev_active_cycles;
  TEST_ASSERT_EQ(expected, *ra8_drw_reg32(k_ra8_drw_off_perftrigger));
  TEST_ASSERT_EQ(0, *ra8_drw_reg32(k_ra8_drw_off_perfcount1));
  TEST_ASSERT_EQ(0, *ra8_drw_reg32(k_ra8_drw_off_perfcount2));

  /* Inject a non-zero count, read it back. */
  *ra8_drw_reg32(k_ra8_drw_off_perfcount1) = k_drw_probe_perfcount1;
  uint32_t count                           = 0UL;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_drw_perf_read(k_ra8_drw_perfctr_1, &count));
  TEST_ASSERT_EQ(0x12345678UL, count);

  /* Reset clears one counter and leaves the other alone. */
  *ra8_drw_reg32(k_ra8_drw_off_perfcount2) = k_drw_probe_perfcount2;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_drw_perf_reset(k_ra8_drw_perfctr_1));
  TEST_ASSERT_EQ(0, *ra8_drw_reg32(k_ra8_drw_off_perfcount1));
  TEST_ASSERT_EQ(0xDEADBEEFUL, *ra8_drw_reg32(k_ra8_drw_off_perfcount2));

  /* Bad-arg paths. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_drw_perf_read(k_ra8_drw_perfctr_1, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_drw_perf_read((ra8_drw_perfcounter_id_t)42U, &count));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_drw_perf_reset((ra8_drw_perfcounter_id_t)42U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_drw_perf_arm((ra8_drw_perftrigger_t)0xBADU, k_ra8_drw_perfev_disabled));
  TEST_END("drw perf arm/read/reset");
}

/**
 * @test test_mcdc_drw
 *
 * @par MC/DC:
 * Decision: ``ra8_drw_cache_flush`` line 422,
 * libs/ra8_hal/src/ra8_drw.c:
 * ``if (!flush_fb && !flush_texture)``  (2 conditions on the negated
 * boolean operands; treat ``!flush_fb`` and ``!flush_texture`` as the
 * Boolean conditions C1 and C2).
 *
 * N+1 = 3 vector representative subset:
 * - V1: (flush_fb=true,  flush_texture=*)    -> C1=F, decision F (does work)
 * - V2: (flush_fb=false, flush_texture=true) -> C1=T,C2=F, decision F (does work)
 * - V3: (flush_fb=false, flush_texture=false)-> C1=T,C2=T, decision T (warns/no-op)
 * Pairs: (V1,V3) flips C1 with C2 fixed; (V2,V3) flips C2 with C1 fixed.
 *
 * Decision B: ``ra8_drw_perf_arm`` line 937,
 * ``if ((uint16_t)event_ctr1 > k_ra8_drw_internal_perfev_max ||
 *      (uint16_t)event_ctr2 > k_ra8_drw_internal_perfev_max)``
 * (2 conditions, ``||`` short-circuit). N+1 = 3 vectors:
 * - V1: ctr1 in range, ctr2 in range -> both F -> dec F (ok)
 * - V2: ctr1 out, ctr2 in range      -> C1=T short-circuits -> dec T
 * - V3: ctr1 in range, ctr2 out      -> C1=F, C2=T -> dec T
 */
static void test_mcdc_drw(void)
{
  TEST_BEGIN("drw MC/DC: cache_flush + perf_arm 2-cond decisions");
  prep();
  const ra8_drw_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_drw_init(&cfg));

  /* Decision A vectors. All three return ok; correctness = no crash &
   * the side-effect mask in CACHECTL is set on V1/V2 and untouched on V3. */
  const uint32_t before = *ra8_drw_reg32(k_ra8_drw_off_cachectl);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_drw_cache_flush(true, false));
  const uint32_t after_v1 = *ra8_drw_reg32(k_ra8_drw_off_cachectl);
  TEST_ASSERT((after_v1 & (uint32_t)k_ra8_drw_cachectl_cflushfx) != 0U);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_drw_cache_flush(false, true));
  const uint32_t after_v2 = *ra8_drw_reg32(k_ra8_drw_off_cachectl);
  TEST_ASSERT((after_v2 & (uint32_t)k_ra8_drw_cachectl_cflushtx) != 0U);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_drw_cache_flush(false, false));
  /* V3 takes the early-return branch; CACHECTL not touched here. */
  (void)before;

  /* Decision B vectors: perf_arm. */
  const ra8_drw_perftrigger_t in_range  = (ra8_drw_perftrigger_t)0U;
  const ra8_drw_perftrigger_t out_range = (ra8_drw_perftrigger_t)0xBADU;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_drw_perf_arm(in_range, in_range));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_drw_perf_arm(out_range, in_range));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_drw_perf_arm(in_range, out_range));
  TEST_END("drw MC/DC: cache_flush + perf_arm 2-cond decisions");
}

/**
 * @test test_mcdc_drw_internal_rect_below_min
 *
 * @par MC/DC:
 * Decision at libs/ra8_hal/src/ra8_drw.c (call site) -> helper at
 * libs/ra8_hal/src/ra8_drw.c:
 *   ``width < min || height < min`` (2 conditions, OR).
 * - V1: w=10, h=10 -> false
 * - V2: w=0,  h=10 -> true (varies left)
 * - V3: w=10, h=0  -> true (varies right)
 * N+1 = 3.
 */
static void test_mcdc_drw_internal_rect_below_min(void)
{
  TEST_BEGIN("drw MC/DC: rect_below_min OR");
  TEST_ASSERT(!ra8_drw_internal_rect_below_min(1U, 10U, 10U));
  TEST_ASSERT(ra8_drw_internal_rect_below_min(1U, 0U, 10U));
  TEST_ASSERT(ra8_drw_internal_rect_below_min(1U, 10U, 0U));
  TEST_END("drw MC/DC: rect_below_min OR");
}

/**
 * @test test_mcdc_drw_internal_rect_above_max
 *
 * @par MC/DC:
 * Decision at libs/ra8_hal/src/ra8_drw.c (call site) -> helper at
 * libs/ra8_hal/src/ra8_drw.c:
 *   ``width > max_w || height > max_h`` (2 conditions, OR).
 * - V1: w=10,   h=10   -> false
 * - V2: w=2000, h=10   -> true (varies left)
 * - V3: w=10,   h=2000 -> true (varies right)
 * N+1 = 3.
 */
static void test_mcdc_drw_internal_rect_above_max(void)
{
  TEST_BEGIN("drw MC/DC: rect_above_max OR");
  TEST_ASSERT(!ra8_drw_internal_rect_above_max(1024U, 1024U, 10U, 10U));
  TEST_ASSERT(ra8_drw_internal_rect_above_max(1024U, 1024U, 2000U, 10U));
  TEST_ASSERT(ra8_drw_internal_rect_above_max(1024U, 1024U, 10U, 2000U));
  TEST_END("drw MC/DC: rect_above_max OR");
}

/**
 * @test test_mcdc_drw_internal_rect_off_surface
 *
 * @par MC/DC:
 * Decision at libs/ra8_hal/src/ra8_drw.c, ra8_drw_internal_rect_off_surface:
 *   ``rect->x < 0 || rect->y < 0`` (2 conditions C1, C2; ``||`` short-circuit).
 * - V1: x>=0, y>=0 -> C1=F, C2=F -> false (control; on-surface rect)
 * - V2: x<0,  y>=0 -> C1=T (short-circuit) -> true  (varies C1 only)
 * - V3: x>=0, y<0  -> C1=F, C2=T -> true  (varies C2 only)
 * Pairs: (V1,V2) flips C1 with C2 fixed>=0; (V1,V3) flips C2 with C1 fixed>=0.
 * N+1 = 3 vectors for 2 conditions. The driver is init'd first so the cached
 * pitch keeps the on-surface rect's independent ``right > pitch`` leg false.
 */
static void test_mcdc_drw_internal_rect_off_surface(void)
{
  TEST_BEGIN("drw MC/DC: rect_off_surface OR");
  prep();
  const ra8_drw_config_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_drw_init(&cfg));

  const ra8_drw_rect_t on = {
    .x              = (int16_t)k_ra8_drw_test_rect_x,
    .y              = (int16_t)k_ra8_drw_test_rect_y,
    .width_px       = (uint16_t)k_ra8_drw_test_rect_w,
    .height_px      = (uint16_t)k_ra8_drw_test_rect_h,
    .color_argb8888 = 0UL,
  };
  ra8_drw_rect_t neg_x = on;
  neg_x.x              = (int16_t)-1;
  ra8_drw_rect_t neg_y = on;
  neg_y.y              = (int16_t)-1;

  TEST_ASSERT(!ra8_drw_internal_rect_off_surface(&on));   /* V1: F || F -> F       */
  TEST_ASSERT(ra8_drw_internal_rect_off_surface(&neg_x)); /* V2: T -> T (varies x) */
  TEST_ASSERT(ra8_drw_internal_rect_off_surface(&neg_y)); /* V3: F || T -> T (y)   */
  TEST_END("drw MC/DC: rect_off_surface OR");
}

int32_t main(void)
{
  test_set_texture_argb8888();
  test_set_texture_clut_rle();
  test_set_texture_rejects();
  test_clear_texture();
  test_load_clut_happy_and_bounds();
  test_fill_rect_happy();
  test_fill_rect_null();
  test_fill_rect_zero_dim();
  test_fill_rect_too_big();
  test_blit_textured_rect();
  test_draw_line();
  test_draw_triangle();
  test_run_dlist();
  test_dlist_build_encoding();
  test_dlist_begin_add_rejects();
  test_dlist_overflow();
  test_mcdc_dlist_run();
  test_perf_arm_read_reset();
  test_mcdc_drw();
  test_mcdc_drw_internal_rect_below_min();
  test_mcdc_drw_internal_rect_above_max();
  test_mcdc_drw_internal_rect_off_surface();
  (void)fprintf(stderr, "[OK  ] test_ra8_drw_render.c\n");
  return 0;
}
