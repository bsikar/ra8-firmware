/**
 * @file test_ra_dac_b.c
 * @brief Unit tests for ra_dac_b.c (12-bit DAC_B driver)
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8d2_dac_b_regs.h"
#include "ra_dac_b.h"
#include "ra_err.h"
#include "ra_mstp.h"
#include "ra_sim_mmap.h"
#include "unity_minimal.h"

typedef enum : uint8_t {
  k_ra_dac_b_test_ch_0   = 0U,
  k_ra_dac_b_test_ch_1   = 1U,
  k_ra_dac_b_test_ch_bad = 2U,
  k_ra_dac_b_test_ch_way = 200U,
} ra_dac_b_test_ch_t;

typedef enum : uint16_t {
  k_ra_dac_b_test_mid  = 0x0800U,
  k_ra_dac_b_test_over = 0xFFFFU,
  k_ra_dac_b_test_max  = 0x0FFFU,
} ra_dac_b_test_value_t;

static void test_init_clears_regs(void)
{
  TEST_BEGIN("dac_b init clears regs");
  ra_sim_mmap_reset();

  volatile r_dac_b_regs_t* reg = ra_dac_b();
  reg->DACR                    = 0xFFU;
  reg->DADR0                   = 0xAAAAU;

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_dac_b_init());
  TEST_ASSERT_EQ(0, (int)reg->DACR);
  TEST_ASSERT_EQ(0, (int)reg->DADR0);
  TEST_ASSERT_EQ(0, (int)reg->DADR1);
  TEST_END("dac_b init clears regs");
}

static void test_write_channel_0(void)
{
  TEST_BEGIN("dac_b write channel 0");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_dac_b_init());
  TEST_ASSERT_EQ((int)k_ra_ok,
                 (int)ra_dac_b_write((uint8_t)k_ra_dac_b_test_ch_0, (uint16_t)k_ra_dac_b_test_mid));

  volatile r_dac_b_regs_t* reg = ra_dac_b();
  TEST_ASSERT_EQ((int)k_ra_dac_b_test_mid, (int)reg->DADR0);
  TEST_ASSERT_EQ((int)k_ra_dac_b_mask_dae, (int)(reg->DACR & (uint8_t)k_ra_dac_b_mask_dae));
  TEST_ASSERT_EQ((int)k_ra_dac_b_mask_daoe0, (int)(reg->DACR & (uint8_t)k_ra_dac_b_mask_daoe0));
  TEST_END("dac_b write channel 0");
}

static void test_write_channel_1(void)
{
  TEST_BEGIN("dac_b write channel 1");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_dac_b_init());
  TEST_ASSERT_EQ((int)k_ra_ok,
                 (int)ra_dac_b_write((uint8_t)k_ra_dac_b_test_ch_1, (uint16_t)k_ra_dac_b_test_mid));

  volatile r_dac_b_regs_t* reg = ra_dac_b();
  TEST_ASSERT_EQ((int)k_ra_dac_b_test_mid, (int)reg->DADR1);
  TEST_ASSERT_EQ((int)k_ra_dac_b_mask_daoe1, (int)(reg->DACR & (uint8_t)k_ra_dac_b_mask_daoe1));
  TEST_END("dac_b write channel 1");
}

static void test_write_clamps_over_range(void)
{
  TEST_BEGIN("dac_b write clamps over-range");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ(
    (int)k_ra_ok,
    (int)ra_dac_b_write((uint8_t)k_ra_dac_b_test_ch_0, (uint16_t)k_ra_dac_b_test_over));

  volatile r_dac_b_regs_t* reg = ra_dac_b();
  TEST_ASSERT_EQ((int)k_ra_dac_b_test_max, (int)reg->DADR0);
  TEST_END("dac_b write clamps over-range");
}

static void test_write_bad_channel(void)
{
  TEST_BEGIN("dac_b write bad channel");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ(
    (int)k_ra_err_invalid_arg,
    (int)ra_dac_b_write((uint8_t)k_ra_dac_b_test_ch_bad, (uint16_t)k_ra_dac_b_test_mid));
  TEST_ASSERT_EQ(
    (int)k_ra_err_invalid_arg,
    (int)ra_dac_b_write((uint8_t)k_ra_dac_b_test_ch_way, (uint16_t)k_ra_dac_b_test_mid));
  TEST_END("dac_b write bad channel");
}

/* ---------------------------------------------------------------------------
 * Wave 4.2 -- full build-out
 * ---------------------------------------------------------------------------
 */

static uint32_t s_dac_cb_count;
static uint8_t  s_dac_cb_last_ch;

static void stub_dac_cb(void* ctx, uint8_t ch)
{
  (void)ctx;
  ++s_dac_cb_count;
  s_dac_cb_last_ch = ch;
}

static void prep_w42(void)
{
  ra_sim_mmap_reset();
  (void)ra_mstp_init();
  s_dac_cb_count   = 0U;
  s_dac_cb_last_ch = 0U;
}

static void test_init_configured(void)
{
  TEST_BEGIN("dac_b init configured");
  prep_w42();

  const ra_dac_b_cfg_t cfg = {
    .vref            = k_ra_dac_b_vref_internal,
    .enable_channel0 = true,
    .enable_channel1 = true,
    .sync_with_adc   = true,
  };
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_dac_b_init_configured(&cfg));

  volatile r_dac_b_regs_t* reg = ra_dac_b();
  TEST_ASSERT_EQ((int32_t)k_ra_dac_b_vref_internal, (int32_t)reg->DAVREFCR);
  TEST_ASSERT((reg->DACR & (uint8_t)k_ra_dac_b_mask_dae) != 0U);
  TEST_ASSERT((reg->DACR & (uint8_t)k_ra_dac_b_mask_daoe0) != 0U);
  TEST_ASSERT((reg->DACR & (uint8_t)k_ra_dac_b_mask_daoe1) != 0U);
  TEST_ASSERT((reg->DAADSCR & 0x80U) != 0U);
  TEST_END("dac_b init configured");
}

static void test_init_null(void)
{
  TEST_BEGIN("dac_b init null");
  prep_w42();

  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_dac_b_init_configured(nullptr));
  TEST_END("dac_b init null");
}

static void test_deinit(void)
{
  TEST_BEGIN("dac_b deinit");
  prep_w42();

  const ra_dac_b_cfg_t cfg = {
    .vref            = k_ra_dac_b_vref_avcc0,
    .enable_channel0 = true,
    .enable_channel1 = false,
    .sync_with_adc   = false,
  };
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_dac_b_init_configured(&cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_dac_b_deinit());
  TEST_ASSERT_EQ((int32_t)0, (int32_t)ra_dac_b()->DACR);
  TEST_END("dac_b deinit");
}

static void test_set_vref(void)
{
  TEST_BEGIN("dac_b set_vref");
  prep_w42();

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_dac_b_set_vref(k_ra_dac_b_vref_external));
  TEST_ASSERT_EQ((int32_t)k_ra_dac_b_vref_external, (int32_t)ra_dac_b()->DAVREFCR);
  TEST_END("dac_b set_vref");
}

static void test_output_enable_toggle(void)
{
  TEST_BEGIN("dac_b output_enable toggle");
  prep_w42();

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_dac_b_set_output_enable(0U, true));
  TEST_ASSERT((ra_dac_b()->DACR & (uint8_t)k_ra_dac_b_mask_daoe0) != 0U);
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_dac_b_set_output_enable(0U, false));
  TEST_ASSERT((ra_dac_b()->DACR & (uint8_t)k_ra_dac_b_mask_daoe0) == 0U);
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_dac_b_set_output_enable(1U, true));
  TEST_ASSERT((ra_dac_b()->DACR & (uint8_t)k_ra_dac_b_mask_daoe1) != 0U);
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_dac_b_set_output_enable(1U, false));
  TEST_ASSERT((ra_dac_b()->DACR & (uint8_t)k_ra_dac_b_mask_daoe1) == 0U);
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_dac_b_set_output_enable(9U, true));
  TEST_END("dac_b output_enable toggle");
}

static void test_init_configured_both_disabled(void)
{
  TEST_BEGIN("dac_b init both channels disabled");
  prep_w42();

  const ra_dac_b_cfg_t cfg = {
    .vref            = k_ra_dac_b_vref_avcc0,
    .enable_channel0 = false,
    .enable_channel1 = false,
    .sync_with_adc   = false,
  };
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_dac_b_init_configured(&cfg));
  TEST_ASSERT_EQ((int32_t)0, (int32_t)ra_dac_b()->DACR);
  TEST_END("dac_b init both channels disabled");
}

static void test_init_configured_ch1_only(void)
{
  TEST_BEGIN("dac_b init channel 1 only");
  prep_w42();

  const ra_dac_b_cfg_t cfg = {
    .vref            = k_ra_dac_b_vref_avcc0,
    .enable_channel0 = false,
    .enable_channel1 = true,
    .sync_with_adc   = false,
  };
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_dac_b_init_configured(&cfg));
  TEST_ASSERT((ra_dac_b()->DACR & (uint8_t)k_ra_dac_b_mask_daoe1) != 0U);
  TEST_ASSERT((ra_dac_b()->DACR & (uint8_t)k_ra_dac_b_mask_dae) != 0U);
  TEST_END("dac_b init channel 1 only");
}

static void test_init_configured_ch0_only(void)
{
  TEST_BEGIN("dac_b init channel 0 only");
  prep_w42();

  const ra_dac_b_cfg_t cfg = {
    .vref            = k_ra_dac_b_vref_avcc0,
    .enable_channel0 = true,
    .enable_channel1 = false,
    .sync_with_adc   = false,
  };
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_dac_b_init_configured(&cfg));
  TEST_ASSERT((ra_dac_b()->DACR & (uint8_t)k_ra_dac_b_mask_daoe0) != 0U);
  TEST_END("dac_b init channel 0 only");
}

static void test_status_read_and_clear(void)
{
  TEST_BEGIN("dac_b status read + clear");
  prep_w42();

  ra_dac_b()->DACR = (uint8_t)k_ra_dac_b_mask_dae | (uint8_t)k_ra_dac_b_mask_daoe0;

  uint8_t mask = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_dac_b_get_status(&mask));
  TEST_ASSERT_EQ((int32_t)((uint8_t)k_ra_dac_b_mask_dae | (uint8_t)k_ra_dac_b_mask_daoe0),
                 (int32_t)mask);

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_dac_b_clear_status());
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_dac_b_get_status(&mask));
  TEST_ASSERT_EQ((int32_t)0, (int32_t)mask);
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_dac_b_get_status(nullptr));
  TEST_END("dac_b status read + clear");
}

static void test_attach_and_dispatch(void)
{
  TEST_BEGIN("dac_b attach + dispatch");
  prep_w42();

  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_dac_b_attach_handler(stub_dac_cb, (void*)(uintptr_t)0xFEEDU));
  ra_dac_b_dispatch_update((uint8_t)k_ra_dac_b_test_ch_1);
  TEST_ASSERT_EQ((int32_t)1, (int32_t)s_dac_cb_count);
  TEST_ASSERT_EQ((int32_t)k_ra_dac_b_test_ch_1, (int32_t)s_dac_cb_last_ch);

  ra_dac_b_dispatch_update((uint8_t)k_ra_dac_b_test_ch_bad);
  TEST_ASSERT_EQ((int32_t)1, (int32_t)s_dac_cb_count);
  TEST_END("dac_b attach + dispatch");
}

static void test_power_transition(void)
{
  TEST_BEGIN("dac_b power transition");
  prep_w42();

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_dac_b_init());
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_dac_b_enter_stop());
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_dac_b_exit_stop());
  TEST_END("dac_b power transition");
}

int32_t main(void)
{
  test_init_clears_regs();
  test_write_channel_0();
  test_write_channel_1();
  test_write_clamps_over_range();
  test_write_bad_channel();
  test_init_configured();
  test_init_configured_both_disabled();
  test_init_configured_ch1_only();
  test_init_configured_ch0_only();
  test_init_null();
  test_deinit();
  test_set_vref();
  test_output_enable_toggle();
  test_status_read_and_clear();
  test_attach_and_dispatch();
  test_power_transition();
  (void)fprintf(stderr, "[OK  ] test_ra_dac_b.c\n");
  return 0;
}
