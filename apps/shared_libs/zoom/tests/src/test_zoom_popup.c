/**
 * @file test_zoom_popup.c
 * @brief Host unit and C-ABI tests for ::zoom_mem and ::zoom_popup.
 *
 * @details
 * Exercises the complete EPUB raster-figure tap-to-zoom pipeline on host:
 *   1. ::ra8_img_decode_gray8 decodes an encoded 2x2 PNG fixture into
 *      caller-owned gray8 storage (zero heap, arena drained).
 *   2. ::zoom_mem_src_init + ::zoom_mem_src_bind expose strided and dense
 *      gray8 planes over the generic ::zoom_source_t seam.
 *   3. ::zoom_popup_open + ::zoom_popup_render draw the chrome bar, top-left
 *      Close chip, status badge, and zoomed figure into a live ``ra8_gfx``
 *      ARGB8888 framebuffer.
 *   4. ::zoom_popup_tap drives centre magnification cycling, 4-direction edge
 *      panning, settle timer promotion (A2 -> GC16), and Close dismissal.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_gfx.h"
#include "ra8_img_arena.h"
#include "reflow_image.h"
#include "unity_minimal.h"
#include "zoom.h"
#include "zoom_mem.h"
#include "zoom_popup.h"

/**
 * @enum t_popup_geom_t
 * @brief Shared host framebuffer and scratch capacities for popup tests.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_t_fb_w       = 128U,  /**< Small host test panel width, px.  */
  k_t_fb_h       = 96U,   /**< Small host test panel height, px. */
  k_t_bar_h      = 24U,   /**< Top chrome bar height, px.        */
  k_t_close_w    = 56U,   /**< Top-left Close chip width, px.    */
  k_t_strip_rows = 8U,    /**< Strip composite height, rows.     */
  k_t_arena_cap  = 16384U /**< Scratch arena capacity for PNG.   */
} t_popup_geom_t;

/** @brief 2x2 RGB PNG: TL red, TR green, BL blue, BR white (matches test_reflow_image). */
static const uint8_t s_png_2x2[] = {
  137U, 80U,  78U,  71U,  13U,  10U,  26U,  10U,  0U,   0U,   0U,   13U,  73U,  72U,  68U,  82U,
  0U,   0U,   0U,   2U,   0U,   0U,   0U,   2U,   8U,   2U,   0U,   0U,   0U,   253U, 212U, 154U,
  115U, 0U,   0U,   0U,   22U,  73U,  68U,  65U,  84U,  120U, 156U, 99U,  248U, 207U, 192U, 192U,
  240U, 159U, 129U, 145U, 129U, 225U, 255U, 255U, 255U, 12U,  0U,   30U,  246U, 4U,   253U, 9U,
  237U, 52U,  62U,  0U,   0U,   0U,   0U,   73U,  69U,  78U,  68U,  174U, 66U,  96U,  130U};

static uint32_t s_fb[(size_t)k_t_fb_w * (size_t)k_t_fb_h];
static uint8_t  s_row[k_t_fb_w];
static uint8_t  s_strip[(size_t)k_t_fb_w * (size_t)k_t_strip_rows];
static uint8_t  s_packed[((size_t)k_t_fb_w * (size_t)k_t_strip_rows) / 2U];

/** @brief Populate a caller-owned zoom_scratch_t backed by the static buffers. */
static zoom_scratch_t t_scratch(void)
{
  return (zoom_scratch_t){.row        = s_row,
                          .row_cap    = (uint32_t)sizeof s_row,
                          .strip      = s_strip,
                          .strip_cap  = (uint32_t)sizeof s_strip,
                          .packed     = s_packed,
                          .packed_cap = (uint32_t)sizeof s_packed};
}

/** @brief Bind caller framebuffer to ra8_gfx and zero-fill before drawing. */
static void t_bind_fb(void)
{
  (void)memset(s_fb, 0, sizeof s_fb);
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_gfx_init(s_fb, (uint16_t)k_t_fb_w, (uint16_t)k_t_fb_h, k_ra8_gfx_format_argb8888));
}

/** @brief Test zoom_mem_src_init, zoom_mem_src_bind, and strided zoom_mem_read. */
static void test_mem_src_and_read(void)
{
  TEST_BEGIN("zoom_mem: init, bind, strided sub-rect read, and guards");
  static const uint8_t k_img[12] = {
    1U,
    2U,
    3U,
    0xAAU,
    4U,
    5U,
    6U,
    0xBBU,
    7U,
    8U,
    9U,
    0xCCU,
  };
  zoom_mem_src_t ms = {};
  zoom_source_t  zs = {};

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, zoom_mem_src_init(nullptr, k_img, 3U, 3U, 4U));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, zoom_mem_src_init(&ms, nullptr, 3U, 3U, 4U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, zoom_mem_src_init(&ms, k_img, 0U, 3U, 0U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, zoom_mem_src_init(&ms, k_img, 3U, 3U, 2U));
  TEST_ASSERT_EQ(k_ra8_ok, zoom_mem_src_init(&ms, k_img, 3U, 3U, 4U));
  TEST_ASSERT_EQ(4U, ms.stride);

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, zoom_mem_src_bind(nullptr, &zs));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, zoom_mem_src_bind(&ms, nullptr));
  TEST_ASSERT_EQ(k_ra8_ok, zoom_mem_src_bind(&ms, &zs));
  TEST_ASSERT_EQ(3U, zs.width);
  TEST_ASSERT_EQ(3U, zs.height);

  uint8_t out[6] = {0};
  TEST_ASSERT_EQ(k_ra8_ok, zs.read(zs.ctx, 1U, 1U, 2U, 2U, out, 3U));
  TEST_ASSERT_EQ(5U, out[0]);
  TEST_ASSERT_EQ(6U, out[1]);
  TEST_ASSERT_EQ(0U, out[2]);
  TEST_ASSERT_EQ(8U, out[3]);
  TEST_ASSERT_EQ(9U, out[4]);

  TEST_ASSERT_EQ(k_ra8_err_out_of_range, zoom_mem_read(&ms, 2U, 0U, 2U, 1U, out, 2U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, zoom_mem_read(&ms, 0U, 0U, 2U, 1U, out, 1U));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, zoom_mem_read(nullptr, 0U, 0U, 1U, 1U, out, 1U));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, zoom_mem_read(&ms, 0U, 0U, 1U, 1U, nullptr, 1U));
  TEST_END("zoom_mem: init, bind, strided sub-rect read, and guards");
}

/** @brief End-to-end PNG gray8 decode -> popup open/render -> tap cycle -> close. */
static void test_png_to_popup_pan_zoom_close(void)
{
  TEST_BEGIN("zoom_popup: PNG gray8 -> render -> tap zoom/pan/settle/close");
  static uint8_t  s_arena_buf[k_t_arena_cap];
  static uint8_t  s_gray_fig[64];
  ra8_img_arena_t arena = {.base = s_arena_buf, .cap = sizeof s_arena_buf};
  uint32_t        fig_w = 0U;
  uint32_t        fig_h = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_img_decode_gray8(&arena,
                                      s_png_2x2,
                                      sizeof s_png_2x2,
                                      s_gray_fig,
                                      sizeof s_gray_fig,
                                      &fig_w,
                                      &fig_h));
  TEST_ASSERT_EQ(2U, fig_w);
  TEST_ASSERT_EQ(2U, fig_h);
  TEST_ASSERT_EQ(0U, arena.offset);

  /* Build a 160x120 source seeded from the decoded 2x2 figure so zooming/panning moves pixels. */
  static uint8_t s_large[160U * 120U];
  for (uint32_t y = 0U; y < 120U; ++y) {
    for (uint32_t x = 0U; x < 160U; ++x) {
      s_large[(y * 160U) + x] = s_gray_fig[((y / 60U) * 2U) + (x / 80U)];
    }
  }

  zoom_mem_src_t ms = {};
  zoom_source_t  zs = {};
  TEST_ASSERT_EQ(k_ra8_ok, zoom_mem_src_init(&ms, s_large, 160U, 120U, 0U));
  TEST_ASSERT_EQ(k_ra8_ok, zoom_mem_src_bind(&ms, &zs));

  const zoom_popup_cfg_t cfg = {
    .src       = zs,
    .scratch   = t_scratch(),
    .fb_w      = (uint16_t)k_t_fb_w,
    .fb_h      = (uint16_t)k_t_fb_h,
    .bar_h     = (uint16_t)k_t_bar_h,
    .close_w   = (uint16_t)k_t_close_w,
    .edge_band = 20U,
    .scale     = 1U,
    .scale_max = 4U,
    .policy    = k_zoom_policy_responsive,
    .settle_ms = 200U,
    .focus_x   = 80,
    .focus_y   = 60,
  };

  zoom_popup_t popup = {};
  TEST_ASSERT_EQ(k_ra8_ok, zoom_popup_open(&popup, &cfg));
  TEST_ASSERT(zoom_popup_active(&popup));

  t_bind_fb();
  TEST_ASSERT_EQ(k_ra8_ok, zoom_popup_render(&popup));
  /* Verify bar white margin pixel, bottom hairline black pixel, and close chip background. */
  TEST_ASSERT_EQ(0x00FFFFFFU, s_fb[(1U * k_t_fb_w) + 120U] & 0x00FFFFFFU);
  TEST_ASSERT_EQ(0x00000000U, s_fb[((k_t_bar_h - 1U) * k_t_fb_w) + 120U] & 0x00FFFFFFU);
  TEST_ASSERT_EQ(0x00000000U, s_fb[(5U * k_t_fb_w) + 5U] & 0x00FFFFFFU);

  zoom_present_t pres = {};
  TEST_ASSERT_EQ(k_ra8_ok, zoom_popup_present(&popup, &pres));
  TEST_ASSERT(pres.present);
  TEST_ASSERT_EQ(k_zoom_refresh_quality, pres.refresh);
  TEST_ASSERT_EQ(0, pres.rect.y);
  TEST_ASSERT_EQ((int32_t)k_t_fb_h, pres.rect.h);

  char status[32] = {};
  TEST_ASSERT_EQ(k_ra8_ok, zoom_popup_status(&popup, status, sizeof status));
  TEST_ASSERT_EQ(0, strcmp(status, "FIG 160x120 1x"));

  /* Verify tap zone hits across chrome and 5-zone viewport. */
  TEST_ASSERT_EQ(k_zoom_popup_zone_close, zoom_popup_zone_hit(&popup, 10, 10));
  TEST_ASSERT_EQ(k_zoom_popup_zone_none, zoom_popup_zone_hit(&popup, 100, 10));
  TEST_ASSERT_EQ(k_zoom_popup_zone_pan_up, zoom_popup_zone_hit(&popup, 64, 30));
  TEST_ASSERT_EQ(k_zoom_popup_zone_pan_down, zoom_popup_zone_hit(&popup, 64, 90));
  TEST_ASSERT_EQ(k_zoom_popup_zone_pan_left, zoom_popup_zone_hit(&popup, 10, 60));
  TEST_ASSERT_EQ(k_zoom_popup_zone_pan_right, zoom_popup_zone_hit(&popup, 118, 60));
  TEST_ASSERT_EQ(k_zoom_popup_zone_zoom, zoom_popup_zone_hit(&popup, 64, 60));

  /* Centre tap doubles scale (1x -> 2x) and marks top status bar dirty. */
  zoom_popup_action_t act = k_zoom_popup_act_none;
  TEST_ASSERT_EQ(k_ra8_ok, zoom_popup_tap(&popup, 64, 60, 1000U, &act));
  TEST_ASSERT_EQ(k_zoom_popup_act_redrawn, act);
  TEST_ASSERT_EQ(2U, popup.view.scale);
  TEST_ASSERT_EQ(k_ra8_ok, zoom_popup_status(&popup, status, sizeof status));
  TEST_ASSERT_EQ(0, strcmp(status, "FIG 160x120 2x"));
  TEST_ASSERT_EQ(k_ra8_ok, zoom_popup_present(&popup, &pres));
  TEST_ASSERT(pres.present);
  TEST_ASSERT_EQ(0, pres.rect.y);

  /* Pan left -> chrome stays clean, fast flush only covers viewport dst. */
  TEST_ASSERT_EQ(k_ra8_ok, zoom_popup_tap(&popup, 10, 60, 1100U, &act));
  TEST_ASSERT_EQ(k_zoom_popup_act_redrawn, act);
  TEST_ASSERT_EQ(k_ra8_ok, zoom_popup_present(&popup, &pres));
  TEST_ASSERT(pres.present);
  TEST_ASSERT_EQ(k_zoom_refresh_fast, pres.refresh);
  TEST_ASSERT_EQ((int32_t)k_t_bar_h, pres.rect.y);

  /* Settle timer promotes to quality after 200ms silence. */
  TEST_ASSERT(!zoom_popup_tick(&popup, 1200U));
  TEST_ASSERT(zoom_popup_tick(&popup, 1300U));
  TEST_ASSERT_EQ(k_ra8_ok, zoom_popup_present(&popup, &pres));
  TEST_ASSERT_EQ(k_zoom_refresh_quality, pres.refresh);

  /* Tap top-left Close chip -> dismissed. */
  TEST_ASSERT_EQ(k_ra8_ok, zoom_popup_tap(&popup, 12, 12, 1400U, &act));
  TEST_ASSERT_EQ(k_zoom_popup_act_closed, act);
  TEST_ASSERT(!zoom_popup_active(&popup));
  TEST_END("zoom_popup: PNG gray8 -> render -> tap zoom/pan/settle/close");
}

/** @brief Guard checks for null/closed states and scale_max == 1 no-op centre tap. */
static void test_popup_guards_and_noop_zoom(void)
{
  TEST_BEGIN("zoom_popup: C ABI null/state guards and 1x max tap");
  zoom_popup_t        popup = {};
  zoom_present_t      pres  = {};
  zoom_popup_action_t act   = k_zoom_popup_act_none;
  char                buf[8];

  TEST_ASSERT(!zoom_popup_active(nullptr));
  TEST_ASSERT(!zoom_popup_tick(nullptr, 0U));
  TEST_ASSERT_EQ(k_zoom_popup_zone_none, zoom_popup_zone_hit(nullptr, 0, 0));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, zoom_popup_open(nullptr, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, zoom_popup_close(nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, zoom_popup_tap(nullptr, 0, 0, 0U, &act));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, zoom_popup_present(nullptr, &pres));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, zoom_popup_render(nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, zoom_popup_status(nullptr, buf, sizeof buf));

  TEST_ASSERT_EQ(k_ra8_err_invalid_state, zoom_popup_tap(&popup, 0, 0, 0U, &act));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, zoom_popup_present(&popup, &pres));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, zoom_popup_render(&popup));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, zoom_popup_status(&popup, buf, sizeof buf));

  static uint8_t s_small[16U * 16U];
  (void)memset(s_small, 0x40, sizeof s_small);
  zoom_mem_src_t ms = {};
  zoom_source_t  zs = {};
  TEST_ASSERT_EQ(k_ra8_ok, zoom_mem_src_init(&ms, s_small, 16U, 16U, 0U));
  TEST_ASSERT_EQ(k_ra8_ok, zoom_mem_src_bind(&ms, &zs));
  const zoom_popup_cfg_t cfg = {
    .src       = zs,
    .scratch   = t_scratch(),
    .fb_w      = (uint16_t)k_t_fb_w,
    .fb_h      = (uint16_t)k_t_fb_h,
    .scale     = 1U,
    .scale_max = 1U,
  };
  TEST_ASSERT_EQ(k_ra8_ok, zoom_popup_open(&popup, &cfg));
  /* Small buffer capacity -> no_mem on zoom_popup_status. */
  TEST_ASSERT_EQ(k_ra8_err_no_mem, zoom_popup_status(&popup, buf, 4U));
  /* Centre tap with scale_max == 1 makes no zoom change -> k_zoom_popup_act_none. */
  TEST_ASSERT_EQ(k_ra8_ok, zoom_popup_tap(&popup, 64, 64, 10U, &act));
  TEST_ASSERT_EQ(k_zoom_popup_act_none, act);
  TEST_ASSERT_EQ(k_ra8_ok, zoom_popup_close(&popup));
  TEST_END("zoom_popup: C ABI null/state guards and 1x max tap");
}

/**
 * @brief Test entry point.
 * @return 0 on success; unity macros exit(1) on first failure.
 */
int main(void)
{
  test_mem_src_and_read();
  test_png_to_popup_pan_zoom_close();
  test_popup_guards_and_noop_zoom();
  return 0;
}
