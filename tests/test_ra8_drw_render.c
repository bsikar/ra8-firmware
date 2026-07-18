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
#include "ra8_drw_internal.h"
#include "ra8_drw_regs.h"
#include "ra8_err.h"
#include "ra8_mstp.h"
#include "ra8_sim_mmap.h"
#include "unity_minimal.h"

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
    entries[i] = 0xFF000000UL | (i * 0x010101UL);
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

  const uint32_t l1 = (uint32_t)((int32_t)k_ra8_drw_test_rect_x * (int32_t)k_ra8_drw_test_subpixel);
  const uint32_t l2 = (uint32_t)(((int32_t)k_ra8_drw_test_rect_x + (int32_t)k_ra8_drw_test_rect_w) *
                                 (int32_t)k_ra8_drw_test_subpixel);
  const uint32_t l3 = (uint32_t)((int32_t)k_ra8_drw_test_rect_y * (int32_t)k_ra8_drw_test_subpixel);
  const uint32_t l4 = (uint32_t)(((int32_t)k_ra8_drw_test_rect_y + (int32_t)k_ra8_drw_test_rect_h) *
                                 (int32_t)k_ra8_drw_test_subpixel);
  TEST_ASSERT_EQ(l1, *ra8_drw_reg32(k_ra8_drw_off_l1start));
  TEST_ASSERT_EQ(l2, *ra8_drw_reg32(k_ra8_drw_off_l2start));
  TEST_ASSERT_EQ(l3, *ra8_drw_reg32(k_ra8_drw_off_l3start));
  TEST_ASSERT_EQ(l4, *ra8_drw_reg32(k_ra8_drw_off_l4start));

  TEST_ASSERT_EQ(k_ra8_drw_test_subpixel, *ra8_drw_reg32(k_ra8_drw_off_l1xadd));
  TEST_ASSERT_EQ(k_ra8_drw_test_subpixel, *ra8_drw_reg32(k_ra8_drw_off_l2xadd));
  TEST_ASSERT_EQ(0, *ra8_drw_reg32(k_ra8_drw_off_l3xadd));
  TEST_ASSERT_EQ(0, *ra8_drw_reg32(k_ra8_drw_off_l4xadd));
  TEST_ASSERT_EQ(0, *ra8_drw_reg32(k_ra8_drw_off_l1yadd));
  TEST_ASSERT_EQ(0, *ra8_drw_reg32(k_ra8_drw_off_l2yadd));
  TEST_ASSERT_EQ(k_ra8_drw_test_subpixel, *ra8_drw_reg32(k_ra8_drw_off_l3yadd));
  TEST_ASSERT_EQ(k_ra8_drw_test_subpixel, *ra8_drw_reg32(k_ra8_drw_off_l4yadd));

  TEST_ASSERT_EQ(k_ra8_drw_control_quad_box, *ra8_drw_reg32(k_ra8_drw_off_control));

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
  TEST_ASSERT_EQ(k_ra8_drw_control_quad_box, *ra8_drw_reg32(k_ra8_drw_off_control));
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
  *ra8_drw_reg32(k_ra8_drw_off_perfcount1) = 0x12345678UL;
  uint32_t count                           = 0UL;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_drw_perf_read(k_ra8_drw_perfctr_1, &count));
  TEST_ASSERT_EQ(0x12345678UL, count);

  /* Reset clears one counter and leaves the other alone. */
  *ra8_drw_reg32(k_ra8_drw_off_perfcount2) = 0xDEADBEEFUL;
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
  test_perf_arm_read_reset();
  test_mcdc_drw();
  test_mcdc_drw_internal_rect_below_min();
  test_mcdc_drw_internal_rect_above_max();
  (void)fprintf(stderr, "[OK  ] test_ra8_drw_render.c\n");
  return 0;
}
