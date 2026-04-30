/**
 * @file test_ra_dac.c
 * @brief Unit tests for ra_dac.c (legacy 12-bit DAC placeholder driver)
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_dac.h"
#include "ra_err.h"
#include "ra_sim_mmap.h"
#include "unity_minimal.h"

typedef enum : uint16_t {
  k_test_mid  = 0x800U,
  k_test_over = 0xFFFFU,
  k_test_max  = 4095U,
} dac_test_t;

static void test_open_close(void)
{
  TEST_BEGIN("dac open/close");
  ra_sim_mmap_reset();

  const ra_dac_cfg_t cfg = {
    .channel     = 0U,
    .data_format = k_ra_dac_format_right,
    .ad_da_sync  = false,
    .output_amp  = true,
  };
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_dac_open(&cfg));

  uint8_t mask = 0U;
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_dac_get_status(0U, &mask));
  TEST_ASSERT((mask & 0x1U) != 0U);
  TEST_ASSERT((mask & 0x4U) != 0U);

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_dac_close(0U));
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_dac_get_status(0U, &mask));
  TEST_ASSERT((mask & 0x1U) == 0U);
  TEST_END("dac open/close");
}

static void test_open_null_bad(void)
{
  TEST_BEGIN("dac open null/bad");
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_dac_open(nullptr));
  const ra_dac_cfg_t cfg = {.channel = 9U};
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)ra_dac_open(&cfg));
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)ra_dac_close(9U));
  TEST_END("dac open null/bad");
}

static void test_write_clamps(void)
{
  TEST_BEGIN("dac write clamp");
  ra_sim_mmap_reset();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_dac_write(0U, (uint16_t)k_test_mid));
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_dac_write(1U, (uint16_t)k_test_over));
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)ra_dac_write(9U, 0U));
  TEST_END("dac write clamp");
}

static void test_start_stop(void)
{
  TEST_BEGIN("dac start/stop");
  ra_sim_mmap_reset();
  const ra_dac_cfg_t cfg = {.channel = 1U, .data_format = k_ra_dac_format_left};
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_dac_open(&cfg));
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_dac_start(1U));

  uint8_t mask = 0U;
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_dac_get_status(1U, &mask));
  TEST_ASSERT((mask & 0x2U) != 0U);

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_dac_stop(1U));
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_dac_get_status(1U, &mask));
  TEST_ASSERT((mask & 0x2U) == 0U);
  TEST_END("dac start/stop");
}

static void test_start_unopened(void)
{
  TEST_BEGIN("dac start unopened");
  ra_sim_mmap_reset();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_dac_close(0U));
  TEST_ASSERT_EQ((int)k_ra_err_invalid_state, (int)ra_dac_start(0U));
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)ra_dac_start(9U));
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)ra_dac_stop(9U));
  TEST_END("dac start unopened");
}

static void test_status_null(void)
{
  TEST_BEGIN("dac status null");
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_dac_get_status(0U, nullptr));
  uint8_t m = 0U;
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)ra_dac_get_status(9U, &m));
  TEST_END("dac status null");
}

int32_t main(void)
{
  test_open_close();
  test_open_null_bad();
  test_write_clamps();
  test_start_stop();
  test_start_unopened();
  test_status_null();
  (void)fprintf(stderr, "[OK  ] test_ra_dac.c\n");
  return 0;
}
