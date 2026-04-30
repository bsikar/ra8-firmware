/**
 * @file test_ra_slcdc.c
 * @brief Unit tests for ra_slcdc.c (Segment LCD placeholder driver)
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_err.h"
#include "ra_sim_mmap.h"
#include "ra_slcdc.h"
#include "unity_minimal.h"

static void test_open_close(void)
{
  TEST_BEGIN("slcdc open/close");
  ra_sim_mmap_reset();

  const ra_slcdc_cfg_t cfg = {
    .bias       = k_ra_slcdc_bias_1_3,
    .drive_volt = k_ra_slcdc_drive_internal,
    .time_slice = 4U,
    .frame_rate = 64U,
    .contrast   = 8U,
  };
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_slcdc_open(&cfg));

  uint8_t status = 0U;
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_slcdc_get_status(&status));
  TEST_ASSERT((status & 0x1U) != 0U);

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_slcdc_close());
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_slcdc_get_status(&status));
  TEST_ASSERT((status & 0x1U) == 0U);
  TEST_END("slcdc open/close");
}

static void test_open_null_bad(void)
{
  TEST_BEGIN("slcdc open null/bad");
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_slcdc_open(nullptr));
  const ra_slcdc_cfg_t cfg = {.time_slice = 0U};
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)ra_slcdc_open(&cfg));
  TEST_END("slcdc open null/bad");
}

static void test_on_off(void)
{
  TEST_BEGIN("slcdc on/off");
  ra_sim_mmap_reset();
  const ra_slcdc_cfg_t cfg = {.time_slice = 4U};
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_slcdc_open(&cfg));
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_slcdc_on());

  uint8_t status = 0U;
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_slcdc_get_status(&status));
  TEST_ASSERT((status & 0x2U) != 0U);

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_slcdc_off());
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_slcdc_get_status(&status));
  TEST_ASSERT((status & 0x2U) == 0U);
  TEST_END("slcdc on/off");
}

static void test_on_unopened(void)
{
  TEST_BEGIN("slcdc on unopened");
  ra_sim_mmap_reset();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_slcdc_close());
  TEST_ASSERT_EQ((int)k_ra_err_invalid_state, (int)ra_slcdc_on());
  TEST_END("slcdc on unopened");
}

static void test_write(void)
{
  TEST_BEGIN("slcdc write");
  ra_sim_mmap_reset();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_slcdc_write(0U, 0xA5U));
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_slcdc_write(15U, 0xFFU));
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)ra_slcdc_write(99U, 0U));
  TEST_END("slcdc write");
}

static void test_status_null(void)
{
  TEST_BEGIN("slcdc status null");
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_slcdc_get_status(nullptr));
  TEST_END("slcdc status null");
}

int32_t main(void)
{
  test_open_close();
  test_open_null_bad();
  test_on_off();
  test_on_unopened();
  test_write();
  test_status_null();
  (void)fprintf(stderr, "[OK  ] test_ra_slcdc.c\n");
  return 0;
}
