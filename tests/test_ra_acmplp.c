/**
 * @file test_ra_acmplp.c
 * @brief Unit tests for ra_acmplp.c (ACMPLP placeholder driver)
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_acmplp.h"
#include "ra_err.h"
#include "ra_sim_mmap.h"
#include "unity_minimal.h"

typedef enum : uint8_t {
  k_test_ch_0   = 0U,
  k_test_ch_1   = 1U,
  k_test_ch_bad = 8U,
} acmplp_test_t;

static uint32_t s_cb_count;

static void cb(void* ctx, uint8_t ch)
{
  (void)ctx;
  (void)ch;
  ++s_cb_count;
}

static void test_open_close(void)
{
  TEST_BEGIN("acmplp open/close");
  ra_sim_mmap_reset();

  const ra_acmplp_cfg_t cfg = {
    .channel    = (uint8_t)k_test_ch_1,
    .speed      = k_ra_acmplp_speed_high,
    .ivpsel     = 3U,
    .ivrefsel   = 4U,
    .filter_en  = false,
    .invert_out = true,
  };
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_acmplp_open(&cfg));

  uint8_t mask = 0U;
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_acmplp_get_status((uint8_t)k_test_ch_1, &mask));
  TEST_ASSERT((mask & 0x1U) != 0U);
  TEST_ASSERT((mask & 0x4U) != 0U);

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_acmplp_close((uint8_t)k_test_ch_1));
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_acmplp_get_status((uint8_t)k_test_ch_1, &mask));
  TEST_ASSERT((mask & 0x1U) == 0U);
  TEST_END("acmplp open/close");
}

static void test_open_null(void)
{
  TEST_BEGIN("acmplp open null");
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_acmplp_open(nullptr));
  TEST_END("acmplp open null");
}

static void test_bad_channel(void)
{
  TEST_BEGIN("acmplp bad channel");
  const ra_acmplp_cfg_t cfg = {.channel = (uint8_t)k_test_ch_bad};
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)ra_acmplp_open(&cfg));
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)ra_acmplp_close((uint8_t)k_test_ch_bad));
  bool b = false;
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)ra_acmplp_read((uint8_t)k_test_ch_bad, &b));
  uint8_t m = 0U;
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)ra_acmplp_get_status((uint8_t)k_test_ch_bad, &m));
  TEST_END("acmplp bad channel");
}

static void test_null_outs(void)
{
  TEST_BEGIN("acmplp null outs");
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_acmplp_read((uint8_t)k_test_ch_0, nullptr));
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_acmplp_get_status((uint8_t)k_test_ch_0, nullptr));
  TEST_END("acmplp null outs");
}

static void test_dispatch(void)
{
  TEST_BEGIN("acmplp dispatch");
  ra_sim_mmap_reset();
  s_cb_count = 0U;

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_acmplp_attach_handler(cb, nullptr));
  ra_acmplp_dispatch((uint8_t)k_test_ch_0);
  ra_acmplp_dispatch((uint8_t)k_test_ch_bad);
  TEST_ASSERT_EQ((int)1, (int)s_cb_count);
  TEST_END("acmplp dispatch");
}

int32_t main(void)
{
  test_open_close();
  test_open_null();
  test_bad_channel();
  test_null_outs();
  test_dispatch();
  (void)fprintf(stderr, "[OK  ] test_ra_acmplp.c\n");
  return 0;
}
