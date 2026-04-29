/**
 * @file test_ra_pdc.c
 * @brief Unit tests for ra_pdc.c (Parallel Data Capture driver)
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8d2_pdc_regs.h"
#include "ra_err.h"
#include "ra_pdc.h"
#include "ra_sim_mmap.h"
#include "unity_minimal.h"

typedef enum : uint16_t {
  k_pdc_test_x_start = 16U,
  k_pdc_test_x_size  = 640U,
  k_pdc_test_y_start = 4U,
  k_pdc_test_y_size  = 480U,
  k_pdc_test_max     = 0x0FFFU,
} ra_pdc_test_const_t;

static uint32_t s_pdc_cb_count;
static uint32_t s_pdc_cb_last_mask;
static uint8_t* s_pdc_cb_last_buf;

static void stub_pdc_cb(void* ctx, uint32_t mask, uint8_t* buf)
{
  (void)ctx;
  ++s_pdc_cb_count;
  s_pdc_cb_last_mask = mask;
  s_pdc_cb_last_buf  = buf;
}

static void prep(void)
{
  ra_sim_mmap_reset();
  s_pdc_cb_count     = 0U;
  s_pdc_cb_last_mask = 0U;
  s_pdc_cb_last_buf  = nullptr;
  /* Drop any callback / buffer pointer left over from a prior test. */
  (void)ra_pdc_close();
}

static ra_pdc_config_t baseline_cfg(void)
{
  return (ra_pdc_config_t){
    .clock_div             = k_ra_pdc_div_4,
    .endian                = k_ra_pdc_endian_little,
    .hsync_polarity        = k_ra_pdc_pol_active_high,
    .vsync_polarity        = k_ra_pdc_pol_active_high,
    .pcko_output_enable    = true,
    .enable_frame_end_irq  = true,
    .enable_error_irq      = true,
    .x_capture_start_byte  = (uint16_t)k_pdc_test_x_start,
    .x_capture_bytes       = (uint16_t)k_pdc_test_x_size,
    .y_capture_start_pixel = (uint16_t)k_pdc_test_y_start,
    .y_capture_pixels      = (uint16_t)k_pdc_test_y_size,
  };
}

static void test_open_close(void)
{
  TEST_BEGIN("pdc open + close");
  prep();
  const ra_pdc_config_t cfg = baseline_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_pdc_open(&cfg));

  volatile r_pdc_regs_t* reg = ra_pdc();
  /* PCCR0 should carry FEIE and the four error IE bits. */
  const uint32_t expect_ie =
    (uint32_t)k_ra_pdc_mask_pccr0_feie | (uint32_t)k_ra_pdc_mask_pccr0_ovie |
    (uint32_t)k_ra_pdc_mask_pccr0_udrie | (uint32_t)k_ra_pdc_mask_pccr0_verie |
    (uint32_t)k_ra_pdc_mask_pccr0_herie;
  TEST_ASSERT_EQ((int32_t)expect_ie, (int32_t)(reg->PCCR0 & expect_ie));
  /* PCKO output enable + clock enable. */
  TEST_ASSERT((reg->PCCR0 &
               ((uint32_t)k_ra_pdc_mask_pccr0_pcke | (uint32_t)k_ra_pdc_mask_pccr0_pckoe)) != 0U);
  TEST_ASSERT_EQ(0, (int32_t)reg->PCCR1);

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_pdc_close());
  TEST_ASSERT_EQ(0, (int32_t)reg->PCCR0);
  TEST_ASSERT_EQ(0, (int32_t)reg->PCCR1);
  TEST_END("pdc open + close");
}

static void test_open_null(void)
{
  TEST_BEGIN("pdc open null cfg");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_pdc_open(nullptr));
  TEST_END("pdc open null cfg");
}

static void test_open_bad_window(void)
{
  TEST_BEGIN("pdc open invalid window");
  prep();
  ra_pdc_config_t cfg = baseline_cfg();
  /* HCR overflow: 4090 + 100 > 4095. */
  cfg.x_capture_start_byte = 4090U;
  cfg.x_capture_bytes      = 100U;
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_pdc_open(&cfg));

  cfg                  = baseline_cfg();
  cfg.y_capture_pixels = 0U; /* zero size invalid */
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_pdc_open(&cfg));
  TEST_END("pdc open invalid window");
}

static void test_capture_start_stop(void)
{
  TEST_BEGIN("pdc capture start + stop");
  prep();
  const ra_pdc_config_t cfg = baseline_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_pdc_open(&cfg));

  uint8_t                buf[16] = {};
  volatile r_pdc_regs_t* reg     = ra_pdc();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_pdc_capture_start(buf));
  TEST_ASSERT((reg->PCCR1 & (uint32_t)k_ra_pdc_mask_pccr1_pce) != 0U);

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_pdc_capture_stop());
  TEST_ASSERT_EQ(0, (int32_t)(reg->PCCR1 & (uint32_t)k_ra_pdc_mask_pccr1_pce));
  TEST_END("pdc capture start + stop");
}

static void test_capture_start_null(void)
{
  TEST_BEGIN("pdc capture start null");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_pdc_capture_start(nullptr));
  TEST_END("pdc capture start null");
}

static void test_status_get_clear(void)
{
  TEST_BEGIN("pdc get + clear status");
  prep();
  volatile r_pdc_regs_t* reg = ra_pdc();
  reg->PCSR = (uint32_t)k_ra_pdc_mask_pcsr_fef | (uint32_t)k_ra_pdc_mask_pcsr_ovrf |
              (uint32_t)k_ra_pdc_mask_pcsr_fbsy;

  uint32_t mask = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_pdc_get_status(&mask));
  TEST_ASSERT((mask & (uint32_t)k_ra_pdc_mask_pcsr_fef) != 0U);
  TEST_ASSERT((mask & (uint32_t)k_ra_pdc_mask_pcsr_ovrf) != 0U);

  /* Clear FEF + OVRF, leave FBSY (RO bit, masked out by W1C mask). */
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_pdc_clear_status((uint32_t)k_ra_pdc_mask_pcsr_fef |
                                              (uint32_t)k_ra_pdc_mask_pcsr_ovrf));
  /* Sim model: writing W1C mask just stores the masked value. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_pdc_get_status(nullptr));
  TEST_END("pdc get + clear status");
}

static void test_attach_dispatch(void)
{
  TEST_BEGIN("pdc attach + dispatch");
  prep();
  const ra_pdc_config_t cfg = baseline_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_pdc_open(&cfg));

  uint8_t buf[8] = {};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_pdc_capture_start(buf));
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_pdc_attach_handler(stub_pdc_cb, (void*)(uintptr_t)0xA5U));

  ra_pdc()->PCSR = (uint32_t)k_ra_pdc_mask_pcsr_fef;
  ra_pdc_dispatch();
  TEST_ASSERT_EQ(1, (int32_t)s_pdc_cb_count);
  TEST_ASSERT((s_pdc_cb_last_mask & (uint32_t)k_ra_pdc_mask_pcsr_fef) != 0U);
  TEST_ASSERT_EQ((intptr_t)buf, (intptr_t)s_pdc_cb_last_buf);
  TEST_END("pdc attach + dispatch");
}

static void test_dispatch_no_handler(void)
{
  TEST_BEGIN("pdc dispatch with no handler");
  prep();
  /* No handler attached -- dispatch must not crash. */
  ra_pdc()->PCSR = (uint32_t)k_ra_pdc_mask_pcsr_ovrf;
  ra_pdc_dispatch();
  TEST_ASSERT_EQ(0, (int32_t)s_pdc_cb_count);
  TEST_END("pdc dispatch with no handler");
}

static void test_polarity_programming(void)
{
  TEST_BEGIN("pdc polarity + endian programming");
  prep();
  ra_pdc_config_t cfg = baseline_cfg();
  cfg.hsync_polarity  = k_ra_pdc_pol_active_low;
  cfg.vsync_polarity  = k_ra_pdc_pol_active_low;
  cfg.endian          = k_ra_pdc_endian_big;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_pdc_open(&cfg));

  volatile r_pdc_regs_t* reg = ra_pdc();
  TEST_ASSERT((reg->PCCR0 & (uint32_t)k_ra_pdc_mask_pccr0_hps) != 0U);
  TEST_ASSERT((reg->PCCR0 & (uint32_t)k_ra_pdc_mask_pccr0_vps) != 0U);
  TEST_ASSERT((reg->PCCR0 & (uint32_t)k_ra_pdc_mask_pccr0_eds) != 0U);
  TEST_END("pdc polarity + endian programming");
}

static void test_window_programming(void)
{
  TEST_BEGIN("pdc VCR / HCR window programming");
  prep();
  const ra_pdc_config_t cfg = baseline_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_pdc_open(&cfg));

  volatile r_pdc_regs_t* reg = ra_pdc();
  const uint32_t         vst = reg->VCR & (uint32_t)k_ra_pdc_mask_vcr_vst;
  const uint32_t         vsz =
    (reg->VCR & (uint32_t)k_ra_pdc_mask_vcr_vsz) >> (uint32_t)k_ra_pdc_shift_size;
  const uint32_t hst = reg->HCR & (uint32_t)k_ra_pdc_mask_hcr_hst;
  const uint32_t hsz =
    (reg->HCR & (uint32_t)k_ra_pdc_mask_hcr_hsz) >> (uint32_t)k_ra_pdc_shift_size;

  TEST_ASSERT_EQ((int32_t)k_pdc_test_y_start, (int32_t)vst);
  TEST_ASSERT_EQ((int32_t)k_pdc_test_y_size, (int32_t)vsz);
  TEST_ASSERT_EQ((int32_t)k_pdc_test_x_start, (int32_t)hst);
  TEST_ASSERT_EQ((int32_t)k_pdc_test_x_size, (int32_t)hsz);
  TEST_END("pdc VCR / HCR window programming");
}

static void test_clock_div_field(void)
{
  TEST_BEGIN("pdc PCKDIV programming");
  prep();
  ra_pdc_config_t cfg = baseline_cfg();
  cfg.clock_div       = k_ra_pdc_div_16;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_pdc_open(&cfg));

  volatile r_pdc_regs_t* reg = ra_pdc();
  const uint32_t         div =
    (reg->PCCR0 & (uint32_t)k_ra_pdc_mask_pccr0_pckdiv) >> (uint32_t)k_ra_pdc_shift_pckdiv;
  TEST_ASSERT_EQ((int32_t)k_ra_pdc_div_16, (int32_t)div);
  TEST_END("pdc PCKDIV programming");
}

int32_t main(void)
{
  test_open_close();
  test_open_null();
  test_open_bad_window();
  test_capture_start_stop();
  test_capture_start_null();
  test_status_get_clear();
  test_attach_dispatch();
  test_dispatch_no_handler();
  test_polarity_programming();
  test_window_programming();
  test_clock_div_field();
  (void)fprintf(stderr, "[OK  ] test_ra_pdc.c\n");
  return 0;
}
