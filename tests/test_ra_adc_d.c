/**
 * @file test_ra_adc_d.c
 * @brief Unit tests for ra_adc_d.c (ADC-D placeholder driver)
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_adc_d.h"
#include "ra_err.h"
#include "ra_sim_mmap.h"
#include "unity_minimal.h"

typedef enum : uint8_t {
  k_test_mask = 0x07U,
  k_test_bad  = 9U,
} adc_d_test_t;

static uint32_t s_cb_count;

static void cb(void* ctx)
{
  (void)ctx;
  ++s_cb_count;
}

static void test_open_close(void)
{
  TEST_BEGIN("adc_d open/close");
  ra_sim_mmap_reset();

  const ra_adc_d_cfg_t cfg = {
    .channel_mask = (uint8_t)k_test_mask,
    .resolution   = k_ra_adc_d_resolution_24,
    .osr          = 256U,
    .continuous   = false,
  };
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_adc_d_open(&cfg));

  uint8_t status = 0U;
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_adc_d_get_status(&status));
  TEST_ASSERT((status & 0x1U) != 0U);

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_adc_d_close());
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_adc_d_get_status(&status));
  TEST_ASSERT((status & 0x1U) == 0U);
  TEST_END("adc_d open/close");
}

static void test_open_null(void)
{
  TEST_BEGIN("adc_d open null");
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_adc_d_open(nullptr));
  TEST_END("adc_d open null");
}

static void test_open_zero_mask(void)
{
  TEST_BEGIN("adc_d zero mask");
  const ra_adc_d_cfg_t cfg = {.channel_mask = 0U};
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)ra_adc_d_open(&cfg));
  TEST_END("adc_d zero mask");
}

static void test_scan_and_read(void)
{
  TEST_BEGIN("adc_d scan + read");
  ra_sim_mmap_reset();

  const ra_adc_d_cfg_t cfg = {.channel_mask = (uint8_t)k_test_mask,
                              .resolution   = k_ra_adc_d_resolution_16,
                              .osr          = 64U};
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_adc_d_open(&cfg));
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_adc_d_scan_start());

  int32_t s = 0;
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_adc_d_read(0U, &s));
  TEST_ASSERT(s != 0);

  uint8_t status = 0U;
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_adc_d_get_status(&status));
  TEST_ASSERT((status & 0x2U) != 0U);
  TEST_END("adc_d scan + read");
}

static void test_scan_unopened(void)
{
  TEST_BEGIN("adc_d scan when closed");
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_adc_d_close());
  TEST_ASSERT_EQ((int)k_ra_err_invalid_state, (int)ra_adc_d_scan_start());
  TEST_END("adc_d scan when closed");
}

static void test_null_outs(void)
{
  TEST_BEGIN("adc_d null outs");
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_adc_d_read(0U, nullptr));
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_adc_d_get_status(nullptr));
  int32_t s = 0;
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)ra_adc_d_read((uint8_t)k_test_bad, &s));
  TEST_END("adc_d null outs");
}

static void test_dispatch(void)
{
  TEST_BEGIN("adc_d dispatch");
  s_cb_count = 0U;
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_adc_d_attach_handler(cb, nullptr));
  ra_adc_d_dispatch();
  TEST_ASSERT_EQ((int)1, (int)s_cb_count);
  TEST_END("adc_d dispatch");
}

int32_t main(void)
{
  test_open_close();
  test_open_null();
  test_open_zero_mask();
  test_scan_and_read();
  test_scan_unopened();
  test_null_outs();
  test_dispatch();
  (void)fprintf(stderr, "[OK  ] test_ra_adc_d.c\n");
  return 0;
}
