/**
 * @file test_ra_acmphs_b.c
 * @brief Unit tests for ra_acmphs_b.c (ACMPHS-B placeholder driver)
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_acmphs_b.h"
#include "ra_err.h"
#include "ra_sim_mmap.h"
#include "unity_minimal.h"

typedef enum : uint8_t {
  k_test_ch_0   = 0U,
  k_test_ch_3   = 3U,
  k_test_ch_bad = 9U,
  k_test_ivp    = 5U,
  k_test_ivref  = 7U,
} acmphs_b_test_t;

static uint32_t s_cb_count;
static uint8_t  s_cb_last;

static void cb(void* ctx, uint8_t ch)
{
  (void)ctx;
  ++s_cb_count;
  s_cb_last = ch;
}

static void test_open_close(void)
{
  TEST_BEGIN("acmphs_b open/close");
  ra_sim_mmap_reset();

  const ra_acmphs_b_cfg_t cfg = {
    .channel    = (uint8_t)k_test_ch_0,
    .ivpsel     = (uint8_t)k_test_ivp,
    .ivrefsel   = (uint8_t)k_test_ivref,
    .edge       = k_ra_acmphs_b_edge_rise,
    .filter_en  = true,
    .invert_out = false,
  };
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_acmphs_b_open(&cfg));

  uint8_t mask = 0U;
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_acmphs_b_get_status((uint8_t)k_test_ch_0, &mask));
  TEST_ASSERT((mask & 0x1U) != 0U);

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_acmphs_b_close((uint8_t)k_test_ch_0));
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_acmphs_b_get_status((uint8_t)k_test_ch_0, &mask));
  TEST_ASSERT((mask & 0x1U) == 0U);
  TEST_END("acmphs_b open/close");
}

static void test_open_null(void)
{
  TEST_BEGIN("acmphs_b open null");
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_acmphs_b_open(nullptr));
  TEST_END("acmphs_b open null");
}

static void test_open_bad_channel(void)
{
  TEST_BEGIN("acmphs_b open bad channel");
  const ra_acmphs_b_cfg_t cfg = {.channel = (uint8_t)k_test_ch_bad};
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)ra_acmphs_b_open(&cfg));
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)ra_acmphs_b_close((uint8_t)k_test_ch_bad));
  bool b = false;
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)ra_acmphs_b_read((uint8_t)k_test_ch_bad, &b));
  uint8_t m = 0U;
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg,
                 (int)ra_acmphs_b_get_status((uint8_t)k_test_ch_bad, &m));
  TEST_END("acmphs_b open bad channel");
}

static void test_read_null(void)
{
  TEST_BEGIN("acmphs_b read null");
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_acmphs_b_read((uint8_t)k_test_ch_0, nullptr));
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr,
                 (int)ra_acmphs_b_get_status((uint8_t)k_test_ch_0, nullptr));
  TEST_END("acmphs_b read null");
}

static void test_dispatch(void)
{
  TEST_BEGIN("acmphs_b attach + dispatch");
  ra_sim_mmap_reset();
  s_cb_count = 0U;
  s_cb_last  = 0U;

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_acmphs_b_attach_handler(cb, (void*)(uintptr_t)0xABCDU));
  ra_acmphs_b_dispatch((uint8_t)k_test_ch_3);
  TEST_ASSERT_EQ((int)1, (int)s_cb_count);
  TEST_ASSERT_EQ((int)k_test_ch_3, (int)s_cb_last);

  ra_acmphs_b_dispatch((uint8_t)k_test_ch_bad);
  TEST_ASSERT_EQ((int)1, (int)s_cb_count);

  bool out = false;
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_acmphs_b_read((uint8_t)k_test_ch_3, &out));
  TEST_ASSERT(out);
  TEST_END("acmphs_b attach + dispatch");
}

int32_t main(void)
{
  test_open_close();
  test_open_null();
  test_open_bad_channel();
  test_read_null();
  test_dispatch();
  (void)fprintf(stderr, "[OK  ] test_ra_acmphs_b.c\n");
  return 0;
}
