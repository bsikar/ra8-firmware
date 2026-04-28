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
  k_test_glcdc_fb_addr = 0x68000000UL, /**< Framebuffer at SDRAM base. */
} test_glcdc_fb_t;

typedef enum : uint16_t {
  k_test_glcdc_width  = 1024U,
  k_test_glcdc_height = 600U,
} test_glcdc_dim_t;

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
  (void)fprintf(stderr, "[OK ] test_ra_glcdc.c\n");
  return 0;
}
