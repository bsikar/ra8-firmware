/**
 * @file test_ra_dac8.c
 * @brief Unit tests for ra_dac8.c (8-bit DAC placeholder driver)
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_dac8.h"
#include "ra_err.h"
#include "ra_sim_mmap.h"
#include "unity_minimal.h"

static void test_open_close(void)
{
  TEST_BEGIN("dac8 open/close");
  ra_sim_mmap_reset();

  const ra_dac8_cfg_t cfg = {
    .channel     = 0U,
    .mode        = k_ra_dac8_mode_normal,
    .charge_pump = true,
    .output_amp  = true,
  };
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_dac8_open(&cfg));

  uint8_t mask = 0U;
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_dac8_get_status(0U, &mask));
  TEST_ASSERT((mask & 0x1U) != 0U);
  TEST_ASSERT((mask & 0x4U) != 0U);
  TEST_ASSERT((mask & 0x8U) != 0U);

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_dac8_close(0U));
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_dac8_get_status(0U, &mask));
  TEST_ASSERT((mask & 0x1U) == 0U);
  TEST_END("dac8 open/close");
}

static void test_open_null_bad(void)
{
  TEST_BEGIN("dac8 open null/bad");
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_dac8_open(nullptr));
  const ra_dac8_cfg_t cfg = {.channel = 9U};
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)ra_dac8_open(&cfg));
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)ra_dac8_close(9U));
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)ra_dac8_write(9U, 1U));
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)ra_dac8_start(9U));
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)ra_dac8_stop(9U));
  TEST_END("dac8 open null/bad");
}

static void test_write_value(void)
{
  TEST_BEGIN("dac8 write");
  ra_sim_mmap_reset();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_dac8_write(0U, 0xA5U));
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_dac8_write(1U, 0xFFU));
  TEST_END("dac8 write");
}

static void test_start_stop(void)
{
  TEST_BEGIN("dac8 start/stop");
  ra_sim_mmap_reset();
  const ra_dac8_cfg_t cfg = {.channel = 1U, .mode = k_ra_dac8_mode_realtime};
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_dac8_open(&cfg));
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_dac8_start(1U));

  uint8_t mask = 0U;
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_dac8_get_status(1U, &mask));
  TEST_ASSERT((mask & 0x2U) != 0U);

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_dac8_stop(1U));
  TEST_END("dac8 start/stop");
}

static void test_start_unopened(void)
{
  TEST_BEGIN("dac8 start unopened");
  ra_sim_mmap_reset();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_dac8_close(0U));
  TEST_ASSERT_EQ((int)k_ra_err_invalid_state, (int)ra_dac8_start(0U));
  TEST_END("dac8 start unopened");
}

static void test_status_null(void)
{
  TEST_BEGIN("dac8 status null");
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_dac8_get_status(0U, nullptr));
  uint8_t m = 0U;
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)ra_dac8_get_status(9U, &m));
  TEST_END("dac8 status null");
}

int32_t main(void)
{
  test_open_close();
  test_open_null_bad();
  test_write_value();
  test_start_stop();
  test_start_unopened();
  test_status_null();
  (void)fprintf(stderr, "[OK  ] test_ra_dac8.c\n");
  return 0;
}
