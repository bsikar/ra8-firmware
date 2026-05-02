/**
 * @file test_ra_glcdc.c
 * @brief Unit tests for the GLCDC driver (ra_glcdc.c)
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8d2_glcdc_regs.h"
#include "ra_err.h"
#include "ra_glcdc.h"
#include "ra_mstp.h"
#include "ra_sim_mmap.h"
#include "unity_minimal.h"

typedef enum : uint32_t {
  k_test_glcdc_fb_addr  = 0x68000000UL, /**< Framebuffer at SDRAM base. */
  k_test_glcdc_fb2_addr = 0x68800000UL, /**< Layer-2 framebuffer base.   */
} test_glcdc_fb_t;

typedef enum : uint16_t {
  k_test_glcdc_width  = 1024U,
  k_test_glcdc_height = 600U,
  k_test_layer2_w     = 320U,
  k_test_layer2_h     = 240U,
  k_test_layer2_x     = 64U,
  k_test_layer2_y     = 32U,
  k_test_layer2_strd  = 640U, /**< 320 * 2bpp (RGB565). */
} test_glcdc_dim_t;

typedef enum : uint8_t {
  k_test_alpha_half = 0x80U,
  k_test_layer1     = 0U,
  k_test_layer2     = 1U,
  k_test_clut_small = 4U,
} test_glcdc_byte_t;

typedef enum : uint32_t {
  k_test_bgc_color = 0xFF112233UL,
  k_test_clut_e0   = 0xAA000001UL,
  k_test_clut_e1   = 0xAA000002UL,
  k_test_clut_e2   = 0xAA000003UL,
  k_test_clut_e3   = 0xAA000004UL,
  k_test_clut_dist = 0xDEADBEEFUL,
} test_glcdc_word_t;

static void test_init_happy_path(void)
{
  TEST_BEGIN("ra_glcdc_init happy path");
  ra_sim_mmap_reset();

  const ra_glcdc_config_t cfg = {
    .framebuffer_addr = (uint32_t)k_test_glcdc_fb_addr,
    .width_px         = (uint16_t)k_test_glcdc_width,
    .height_px        = (uint16_t)k_test_glcdc_height,
    .format           = k_ra_glcdc_fmt_rgb565,
  };

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_glcdc_init(&cfg));

  /* The driver writes the configured width into BG_HSIZE. */
  TEST_ASSERT_EQ((int)k_test_glcdc_width, (int)*ra_glcdc_reg32(k_ra_glcdc_off_bg_hsize));
  TEST_ASSERT_EQ((int)k_test_glcdc_height, (int)*ra_glcdc_reg32(k_ra_glcdc_off_bg_vsize));
  TEST_ASSERT_EQ(0, (int)*ra_glcdc_reg32(k_ra_glcdc_off_bg_bgc));
  TEST_ASSERT_EQ((int)k_ra_glcdc_fmt_rgb565, (int)*ra_glcdc_reg32(k_ra_glcdc_off_gr1_fmt));
  TEST_ASSERT_EQ((int)k_test_glcdc_fb_addr, (int)*ra_glcdc_reg32(k_ra_glcdc_off_gr1_saddr));
  TEST_ASSERT_EQ((int)k_test_glcdc_width, (int)*ra_glcdc_reg32(k_ra_glcdc_off_gr1_line));

  TEST_END("ra_glcdc_init happy path");
}

static void test_init_null_cfg_rejected(void)
{
  TEST_BEGIN("ra_glcdc_init rejects NULL cfg");
  ra_sim_mmap_reset();
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_glcdc_init(nullptr));
  TEST_END("ra_glcdc_init rejects NULL cfg");
}

static void test_start_enable(void)
{
  TEST_BEGIN("ra_glcdc_start enables engine");
  ra_sim_mmap_reset();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_glcdc_start(true));
  TEST_ASSERT_EQ(1, (int)*ra_glcdc_reg32(k_ra_glcdc_off_sys_cfg));
  TEST_ASSERT_EQ(1, (int)*ra_glcdc_reg32(k_ra_glcdc_off_bg_en));
  TEST_ASSERT_EQ(1, (int)*ra_glcdc_reg32(k_ra_glcdc_off_gr1_en));
  TEST_END("ra_glcdc_start enables engine");
}

static void test_start_disable(void)
{
  TEST_BEGIN("ra_glcdc_start disables engine");
  ra_sim_mmap_reset();
  /* Prime with something non-zero first. */
  *ra_glcdc_reg32(k_ra_glcdc_off_sys_cfg) = 0xAAU;
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_glcdc_start(false));
  TEST_ASSERT_EQ(0, (int)*ra_glcdc_reg32(k_ra_glcdc_off_sys_cfg));
  TEST_ASSERT_EQ(0, (int)*ra_glcdc_reg32(k_ra_glcdc_off_bg_en));
  TEST_ASSERT_EQ(0, (int)*ra_glcdc_reg32(k_ra_glcdc_off_gr1_en));
  TEST_END("ra_glcdc_start disables engine");
}

/* ---- lifecycle + IRQ + power ---- */

static uint32_t s_glcdc_cb_count;
static uint32_t s_glcdc_cb_last_mask;

static void stub_glcdc_cb(void* ctx, uint32_t mask)
{
  (void)ctx;
  ++s_glcdc_cb_count;
  s_glcdc_cb_last_mask = mask;
}

static void prep_w61(void)
{
  ra_sim_mmap_reset();
  (void)ra_mstp_init();
  s_glcdc_cb_count     = 0U;
  s_glcdc_cb_last_mask = 0U;
}

static void test_deinit(void)
{
  TEST_BEGIN("glcdc deinit");
  prep_w61();
  const ra_glcdc_config_t cfg = {.framebuffer_addr = (uint32_t)k_test_glcdc_fb_addr,
                                 .width_px         = (uint16_t)k_test_glcdc_width,
                                 .height_px        = (uint16_t)k_test_glcdc_height,
                                 .format           = k_ra_glcdc_fmt_rgb565};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_glcdc_init(&cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_glcdc_deinit());
  TEST_END("glcdc deinit");
}

static void test_status_read_and_clear(void)
{
  TEST_BEGIN("glcdc status read + clear");
  prep_w61();
  *ra_glcdc_reg32(k_ra_glcdc_off_sys_stat) = 0xDEADBEEFU;

  uint32_t mask = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_glcdc_get_status(&mask));
  TEST_ASSERT_EQ((int32_t)0xDEADBEEFU, (int32_t)mask);
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_glcdc_clear_status(0xF0F0F0F0U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_glcdc_get_status(nullptr));
  TEST_END("glcdc status read + clear");
}

static void test_attach_and_dispatch(void)
{
  TEST_BEGIN("glcdc attach + dispatch");
  prep_w61();
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_glcdc_attach_handler(stub_glcdc_cb, (void*)(uintptr_t)0x77U));
  *ra_glcdc_reg32(k_ra_glcdc_off_sys_stat) = 0xCAFEU;
  ra_glcdc_dispatch();
  TEST_ASSERT_EQ((int32_t)1, (int32_t)s_glcdc_cb_count);
  TEST_ASSERT_EQ((int32_t)0xCAFEU, (int32_t)s_glcdc_cb_last_mask);
  TEST_END("glcdc attach + dispatch");
}

static void test_power_transition(void)
{
  TEST_BEGIN("glcdc power transition");
  prep_w61();
  const ra_glcdc_config_t cfg = {.framebuffer_addr = (uint32_t)k_test_glcdc_fb_addr,
                                 .width_px         = (uint16_t)k_test_glcdc_width,
                                 .height_px        = (uint16_t)k_test_glcdc_height,
                                 .format           = k_ra_glcdc_fmt_rgb565};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_glcdc_init(&cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_glcdc_enter_stop());
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_glcdc_exit_stop());
  TEST_END("glcdc power transition");
}

/* ---- Layer-2 / blending / output stage / CLUT ---- */

static void test_set_layer2_happy(void)
{
  TEST_BEGIN("glcdc set_layer2 happy path");
  ra_sim_mmap_reset();
  const ra_glcdc_layer2_cfg_t cfg = {
    .framebuffer_addr  = (uint32_t)k_test_glcdc_fb2_addr,
    .line_stride_bytes = (uint32_t)k_test_layer2_strd,
    .width_px          = (uint16_t)k_test_layer2_w,
    .height_px         = (uint16_t)k_test_layer2_h,
    .pos_x             = (uint16_t)k_test_layer2_x,
    .pos_y             = (uint16_t)k_test_layer2_y,
    .format            = k_ra_glcdc_fmt_rgb565,
    .alpha             = (uint8_t)k_test_alpha_half,
  };
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_glcdc_set_layer2(&cfg));

  /* Format -> FLM6. */
  TEST_ASSERT_EQ((int32_t)k_ra_glcdc_fmt_rgb565, (int32_t)*ra_glcdc_reg32(k_ra_glcdc_off_gr2_fmt));
  /* Base address -> FLM2. */
  TEST_ASSERT_EQ((int32_t)k_test_glcdc_fb2_addr,
                 (int32_t)*ra_glcdc_reg32(k_ra_glcdc_off_gr2_saddr));
  /* Line stride -> FLM3 high half. */
  TEST_ASSERT_EQ((int32_t)((uint32_t)k_test_layer2_strd << 16U),
                 (int32_t)*ra_glcdc_reg32(k_ra_glcdc_off_gr2_flm3));
  /* FLM5 = (h<<16) | w. */
  TEST_ASSERT_EQ((int32_t)(((uint32_t)k_test_layer2_h << 16U) | (uint32_t)k_test_layer2_w),
                 (int32_t)*ra_glcdc_reg32(k_ra_glcdc_off_gr2_line));
  /* AB3 (size) = (pos_x<<16) | w. */
  TEST_ASSERT_EQ((int32_t)(((uint32_t)k_test_layer2_x << 16U) | (uint32_t)k_test_layer2_w),
                 (int32_t)*ra_glcdc_reg32(k_ra_glcdc_off_gr2_size));
  /* AB2 = (pos_y<<16) | h. */
  TEST_ASSERT_EQ((int32_t)(((uint32_t)k_test_layer2_y << 16U) | (uint32_t)k_test_layer2_h),
                 (int32_t)*ra_glcdc_reg32(k_ra_glcdc_off_gr2_ab2));
  /* AB5 = (pos_x<<16) | w. */
  TEST_ASSERT_EQ((int32_t)(((uint32_t)k_test_layer2_x << 16U) | (uint32_t)k_test_layer2_w),
                 (int32_t)*ra_glcdc_reg32(k_ra_glcdc_off_gr2_ab5));
  /* AB4 = (pos_y<<16) | h. */
  TEST_ASSERT_EQ((int32_t)(((uint32_t)k_test_layer2_y << 16U) | (uint32_t)k_test_layer2_h),
                 (int32_t)*ra_glcdc_reg32(k_ra_glcdc_off_gr2_ab4));
  /* AB7 = alpha << 16. */
  TEST_ASSERT_EQ((int32_t)((uint32_t)k_test_alpha_half << 16U),
                 (int32_t)*ra_glcdc_reg32(k_ra_glcdc_off_gr2_ab7));
  /* AB1 = DISPSEL=01 (lower-layer blend). */
  TEST_ASSERT_EQ(1, (int32_t)*ra_glcdc_reg32(k_ra_glcdc_off_gr2_ab1));
  /* FLMRD enabled. */
  TEST_ASSERT_EQ(1, (int32_t)*ra_glcdc_reg32(k_ra_glcdc_off_gr2_flmrd));
  /* VEN bit set. */
  TEST_ASSERT_EQ(1, (int32_t)*ra_glcdc_reg32(k_ra_glcdc_off_gr2_en));

  TEST_END("glcdc set_layer2 happy path");
}

static void test_set_layer2_null_cfg(void)
{
  TEST_BEGIN("glcdc set_layer2 rejects NULL");
  ra_sim_mmap_reset();
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_glcdc_set_layer2(nullptr));
  TEST_END("glcdc set_layer2 rejects NULL");
}

static void test_set_blend_alpha(void)
{
  TEST_BEGIN("glcdc set_blend k_ra_blend_alpha");
  ra_sim_mmap_reset();
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_glcdc_set_blend(k_ra_blend_alpha, (uint8_t)k_test_alpha_half));
  /* AB1 should hold DISPSEL=2 (above) | ARCON bit (1 << 12) = 0x1002. */
  TEST_ASSERT_EQ((int32_t)0x1002U, (int32_t)*ra_glcdc_reg32(k_ra_glcdc_off_gr1_ab1));
  /* AB7 = alpha << 16. */
  TEST_ASSERT_EQ((int32_t)((uint32_t)k_test_alpha_half << 16U),
                 (int32_t)*ra_glcdc_reg32(k_ra_glcdc_off_gr1_ab7));
  TEST_END("glcdc set_blend k_ra_blend_alpha");
}

static void test_set_blend_normal(void)
{
  TEST_BEGIN("glcdc set_blend k_ra_blend_normal");
  ra_sim_mmap_reset();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_glcdc_set_blend(k_ra_blend_normal, 0xFFU));
  /* DISPSEL=2 with no ARCON. */
  TEST_ASSERT_EQ((int32_t)2U, (int32_t)*ra_glcdc_reg32(k_ra_glcdc_off_gr1_ab1));
  TEST_END("glcdc set_blend k_ra_blend_normal");
}

static void test_set_blend_overwrite(void)
{
  TEST_BEGIN("glcdc set_blend k_ra_blend_overwrite");
  ra_sim_mmap_reset();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_glcdc_set_blend(k_ra_blend_overwrite, 0U));
  TEST_ASSERT_EQ((int32_t)1U, (int32_t)*ra_glcdc_reg32(k_ra_glcdc_off_gr1_ab1));
  TEST_END("glcdc set_blend k_ra_blend_overwrite");
}

static void test_set_blend_invalid_mode(void)
{
  TEST_BEGIN("glcdc set_blend rejects invalid mode");
  ra_sim_mmap_reset();
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_glcdc_set_blend((ra_glcdc_blend_mode_t)0xFFU, 0U));
  TEST_END("glcdc set_blend rejects invalid mode");
}

static void test_set_background_color(void)
{
  TEST_BEGIN("glcdc set_background_color");
  ra_sim_mmap_reset();
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_glcdc_set_background_color((uint32_t)k_test_bgc_color));
  TEST_ASSERT_EQ((int32_t)k_test_bgc_color, (int32_t)*ra_glcdc_reg32(k_ra_glcdc_off_bg_bgc));
  TEST_END("glcdc set_background_color");
}

/* ---- CLUT double-buffering ---- */

static void test_clut_swap_now_false(void)
{
  TEST_BEGIN("glcdc clut double_buffered swap_now=false");
  ra_sim_mmap_reset();
  /* CLUTINT.SEL starts at 0 -> active plane is 0 -> writes go to plane 1. */
  /* Stamp distinct content into the CURRENTLY active plane (plane 0)
   * so we can prove we did NOT overwrite it. */
  volatile uint32_t* const active0 =
    (volatile uint32_t*)(k_ra_glcdc_base_addr + (uint32_t)k_ra_glcdc_off_gr1_clut0);
  for (uint8_t i = 0U; i < (uint8_t)k_test_clut_small; ++i) {
    active0[i] = (uint32_t)k_test_clut_dist;
  }

  const uint32_t src[k_test_clut_small] = {
    (uint32_t)k_test_clut_e0,
    (uint32_t)k_test_clut_e1,
    (uint32_t)k_test_clut_e2,
    (uint32_t)k_test_clut_e3,
  };
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_glcdc_set_clut_double_buffered((uint8_t)k_test_layer1,
                                                            src,
                                                            (uint32_t)k_test_clut_small,
                                                            false));

  /* Active plane (CLUT0) untouched. */
  TEST_ASSERT_EQ((int32_t)k_test_clut_dist, (int32_t)active0[0]);
  TEST_ASSERT_EQ((int32_t)k_test_clut_dist, (int32_t)active0[3]);
  /* Inactive plane (CLUT1) has the new entries. */
  volatile uint32_t* const inactive1 =
    (volatile uint32_t*)(k_ra_glcdc_base_addr + (uint32_t)k_ra_glcdc_off_gr1_clut1);
  TEST_ASSERT_EQ((int32_t)k_test_clut_e0, (int32_t)inactive1[0]);
  TEST_ASSERT_EQ((int32_t)k_test_clut_e3, (int32_t)inactive1[3]);
  /* CLUTINT.SEL untouched (still 0). */
  TEST_ASSERT_EQ(0, (int32_t)*ra_glcdc_reg32(k_ra_glcdc_off_gr1_clutint));
  TEST_END("glcdc clut double_buffered swap_now=false");
}

static void test_clut_swap_now_true(void)
{
  TEST_BEGIN("glcdc clut double_buffered swap_now=true");
  ra_sim_mmap_reset();
  const uint32_t src[k_test_clut_small] = {
    (uint32_t)k_test_clut_e0,
    (uint32_t)k_test_clut_e1,
    (uint32_t)k_test_clut_e2,
    (uint32_t)k_test_clut_e3,
  };
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_glcdc_set_clut_double_buffered((uint8_t)k_test_layer1,
                                                            src,
                                                            (uint32_t)k_test_clut_small,
                                                            true));
  /* CLUTINT.SEL bit (16) flipped to 1 -- triggers swap on next vsync. */
  TEST_ASSERT_EQ((int32_t)0x10000U, (int32_t)*ra_glcdc_reg32(k_ra_glcdc_off_gr1_clutint));
  /* Verify the second swap toggles SEL back to 0. */
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_glcdc_set_clut_double_buffered((uint8_t)k_test_layer1,
                                                            src,
                                                            (uint32_t)k_test_clut_small,
                                                            true));
  TEST_ASSERT_EQ(0, (int32_t)*ra_glcdc_reg32(k_ra_glcdc_off_gr1_clutint));
  TEST_END("glcdc clut double_buffered swap_now=true");
}

static void test_clut_layer2(void)
{
  TEST_BEGIN("glcdc clut double_buffered layer 2");
  ra_sim_mmap_reset();
  const uint32_t src[k_test_clut_small] = {
    (uint32_t)k_test_clut_e0,
    (uint32_t)k_test_clut_e1,
    (uint32_t)k_test_clut_e2,
    (uint32_t)k_test_clut_e3,
  };
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_glcdc_set_clut_double_buffered((uint8_t)k_test_layer2,
                                                            src,
                                                            (uint32_t)k_test_clut_small,
                                                            false));
  /* New entries live in GR2_CLUT1 (the inactive plane). */
  volatile uint32_t* const inactive1 =
    (volatile uint32_t*)(k_ra_glcdc_base_addr + (uint32_t)k_ra_glcdc_off_gr2_clut1);
  TEST_ASSERT_EQ((int32_t)k_test_clut_e0, (int32_t)inactive1[0]);
  TEST_ASSERT_EQ((int32_t)k_test_clut_e3, (int32_t)inactive1[3]);
  TEST_END("glcdc clut double_buffered layer 2");
}

static void test_clut_null_rejected(void)
{
  TEST_BEGIN("glcdc clut rejects NULL src");
  ra_sim_mmap_reset();
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_glcdc_set_clut_double_buffered((uint8_t)k_test_layer1,
                                                            nullptr,
                                                            (uint32_t)k_test_clut_small,
                                                            false));
  TEST_END("glcdc clut rejects NULL src");
}

static void test_clut_invalid_args(void)
{
  TEST_BEGIN("glcdc clut rejects bad layer / size");
  ra_sim_mmap_reset();
  const uint32_t src[k_test_clut_small] = {0U, 0U, 0U, 0U};
  /* Layer >= 2 is invalid. */
  TEST_ASSERT_EQ(
    (int32_t)k_ra_err_invalid_arg,
    (int32_t)ra_glcdc_set_clut_double_buffered(2U, src, (uint32_t)k_test_clut_small, false));
  /* 0 entries is invalid. */
  TEST_ASSERT_EQ(
    (int32_t)k_ra_err_invalid_arg,
    (int32_t)ra_glcdc_set_clut_double_buffered((uint8_t)k_test_layer1, src, 0U, false));
  /* > 256 entries is invalid. */
  TEST_ASSERT_EQ(
    (int32_t)k_ra_err_invalid_arg,
    (int32_t)ra_glcdc_set_clut_double_buffered((uint8_t)k_test_layer1, src, 257U, false));
  TEST_END("glcdc clut rejects bad layer / size");
}

/* ---- Output stage ---- */

static void test_set_dithering_modes(void)
{
  TEST_BEGIN("glcdc set_dithering modes");
  ra_sim_mmap_reset();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_glcdc_set_dithering(k_ra_dither_off));
  TEST_ASSERT_EQ(0, (int32_t)*ra_glcdc_reg32(k_ra_glcdc_off_panel_dtha));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_glcdc_set_dithering(k_ra_dither_truncate));
  TEST_ASSERT_EQ((int32_t)(2U << 8U), (int32_t)*ra_glcdc_reg32(k_ra_glcdc_off_panel_dtha));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_glcdc_set_dithering(k_ra_dither_2x2));
  TEST_ASSERT_EQ((int32_t)(3U << 8U), (int32_t)*ra_glcdc_reg32(k_ra_glcdc_off_panel_dtha));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_glcdc_set_dithering((ra_glcdc_dither_mode_t)0xFFU));
  TEST_END("glcdc set_dithering modes");
}

static void test_set_brightness(void)
{
  TEST_BEGIN("glcdc set_brightness");
  ra_sim_mmap_reset();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_glcdc_set_brightness(0x11U, 0x22U, 0x33U));
  TEST_ASSERT_EQ((int32_t)0x22U, (int32_t)*ra_glcdc_reg32(k_ra_glcdc_off_out_bright1));
  TEST_ASSERT_EQ((int32_t)((0x33U << 16U) | 0x11U),
                 (int32_t)*ra_glcdc_reg32(k_ra_glcdc_off_out_bright2));
  TEST_END("glcdc set_brightness");
}

static void test_set_contrast(void)
{
  TEST_BEGIN("glcdc set_contrast");
  ra_sim_mmap_reset();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_glcdc_set_contrast(0x40U, 0x80U, 0xC0U));
  TEST_ASSERT_EQ((int32_t)((0x80U << 16U) | (0xC0U << 8U) | 0x40U),
                 (int32_t)*ra_glcdc_reg32(k_ra_glcdc_off_out_contrast));
  TEST_END("glcdc set_contrast");
}

/* ===========================================================================
 * MC/DC vector coverage for compound boolean decisions in ra_glcdc.c.
 * Per docs/MCDC_GAPS.csv there is one decision (line 261, 2 conditions).
 * ===========================================================================
 */

/**
 * @enum test_glcdc_mcdc_t
 * @brief Numeric vectors for the MC/DC tests below.
 */
typedef enum : uint32_t {
  k_test_glcdc_mcdc_clut_zero      = 0U,   /**< Below valid range.        */
  k_test_glcdc_mcdc_clut_in_range  = 4U,   /**< Within 1..256.            */
  k_test_glcdc_mcdc_clut_too_large = 257U, /**< Above k_clut_entries=256. */
} test_glcdc_mcdc_t;

/**
 * @test test_mcdc_set_clut_double_buffered_entries
 *
 * @par MC/DC:
 * Decision: `if ((entries == 0U) || (entries > (uint32_t)k_ra_glcdc_clut_entries))`
 * (2 conditions, libs/ra_hal/src/ra_glcdc.c line 261)
 *
 * - V1: entries=4   -> C1=(4==0)=F, C2=(4>256)=F -> decision F: ok
 * - V2: entries=0   -> C1=T short-circuits        -> decision T: invalid_arg
 * - V3: entries=257 -> C1=F, C2=T                 -> decision T: invalid_arg
 * V1+V2 vary C1; V1+V3 vary C2 (C1 held F). N+1=3 vectors for N=2.
 */
static void test_mcdc_set_clut_double_buffered_entries(void)
{
  TEST_BEGIN("glcdc MC/DC set_clut_double_buffered entries range");
  ra_sim_mmap_reset();
  static const uint32_t clut[(uint32_t)k_test_glcdc_mcdc_clut_in_range] = {
    (uint32_t)k_test_clut_e0,
    (uint32_t)k_test_clut_e1,
    (uint32_t)k_test_clut_e2,
    (uint32_t)k_test_clut_e3,
  };
  /* V1: in-range. */
  TEST_ASSERT_EQ(
    (int32_t)k_ra_ok,
    (int32_t)ra_glcdc_set_clut_double_buffered((uint8_t)k_test_layer1,
                                               clut,
                                               (uint32_t)k_test_glcdc_mcdc_clut_in_range,
                                               false));
  /* V2: zero entries. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_glcdc_set_clut_double_buffered((uint8_t)k_test_layer1,
                                                            clut,
                                                            (uint32_t)k_test_glcdc_mcdc_clut_zero,
                                                            false));
  /* V3: above max. */
  TEST_ASSERT_EQ(
    (int32_t)k_ra_err_invalid_arg,
    (int32_t)ra_glcdc_set_clut_double_buffered((uint8_t)k_test_layer1,
                                               clut,
                                               (uint32_t)k_test_glcdc_mcdc_clut_too_large,
                                               false));
  TEST_END("glcdc MC/DC set_clut_double_buffered entries range");
}

int32_t main(void)
{
  test_init_happy_path();
  test_init_null_cfg_rejected();
  test_start_enable();
  test_start_disable();
  test_deinit();
  test_status_read_and_clear();
  test_attach_and_dispatch();
  test_power_transition();
  test_set_layer2_happy();
  test_set_layer2_null_cfg();
  test_set_blend_alpha();
  test_set_blend_normal();
  test_set_blend_overwrite();
  test_set_blend_invalid_mode();
  test_set_background_color();
  test_clut_swap_now_false();
  test_clut_swap_now_true();
  test_clut_layer2();
  test_clut_null_rejected();
  test_clut_invalid_args();
  test_set_dithering_modes();
  test_set_brightness();
  test_set_contrast();
  test_mcdc_set_clut_double_buffered_entries();
  (void)fprintf(stderr, "[OK ] test_ra_glcdc.c\n");
  return 0;
}
