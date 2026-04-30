/**
 * @file test_ra_tau.c
 * @brief Unit tests for ra_tau.c (TAU placeholder)
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra_err.h"
#include "ra_sim_mmap.h"
#include "ra_tau.h"
#include "unity_minimal.h"

typedef enum : uint16_t {
  k_test_tau_period = 0x1234U,
} test_tau_const_t;

static void prep(void)
{
  ra_sim_mmap_reset();
  for (uint8_t i = 0U; i < (uint8_t)k_ra_tau_channel_count; ++i) {
    (void)ra_tau_close((ra_tau_channel_t)i);
  }
}

static void test_open_null(void)
{
  TEST_BEGIN("open rejects NULL cfg");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_tau_open(nullptr));
  TEST_END("open rejects NULL cfg");
}

static void test_open_bad_args(void)
{
  TEST_BEGIN("open rejects bad channel and zero period");
  prep();
  ra_tau_cfg_t cfg = {.channel   = (ra_tau_channel_t)9U,
                      .period    = (uint16_t)k_test_tau_period,
                      .prescaler = 0U};
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_tau_open(&cfg));

  cfg.channel = k_ra_tau_channel_0;
  cfg.period  = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_tau_open(&cfg));
  TEST_END("open rejects bad channel and zero period");
}

static void test_lifecycle(void)
{
  TEST_BEGIN("open / close lifecycle");
  prep();
  const ra_tau_cfg_t cfg = {.channel   = k_ra_tau_channel_0,
                            .period    = (uint16_t)k_test_tau_period,
                            .prescaler = 4U};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_tau_open(&cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_err_exists, (int32_t)ra_tau_open(&cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_tau_close(k_ra_tau_channel_0));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_tau_close(k_ra_tau_channel_0));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_tau_close((ra_tau_channel_t)9U));
  TEST_END("open / close lifecycle");
}

static void test_start_stop(void)
{
  TEST_BEGIN("start/stop validate args and update state");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_tau_start((ra_tau_channel_t)9U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_not_initialized, (int32_t)ra_tau_start(k_ra_tau_channel_1));
  TEST_ASSERT_EQ((int32_t)k_ra_err_not_initialized, (int32_t)ra_tau_stop(k_ra_tau_channel_1));

  const ra_tau_cfg_t cfg = {.channel   = k_ra_tau_channel_1,
                            .period    = (uint16_t)k_test_tau_period,
                            .prescaler = 0U};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_tau_open(&cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_tau_start(k_ra_tau_channel_1));

  uint8_t op = 0U;
  uint8_t rn = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_tau_status_get(k_ra_tau_channel_1, &op, &rn));
  TEST_ASSERT_EQ((int32_t)1, (int32_t)rn);

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_tau_stop(k_ra_tau_channel_1));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_tau_status_get(k_ra_tau_channel_1, &op, &rn));
  TEST_ASSERT_EQ((int32_t)0, (int32_t)rn);
  TEST_END("start/stop validate args and update state");
}

static void test_count(void)
{
  TEST_BEGIN("count_get validates");
  prep();
  uint16_t v = 0xFFFFU;
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_tau_count_get(k_ra_tau_channel_0, nullptr));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_tau_count_get((ra_tau_channel_t)9U, &v));
  TEST_ASSERT_EQ((int32_t)k_ra_err_not_initialized,
                 (int32_t)ra_tau_count_get(k_ra_tau_channel_0, &v));

  const ra_tau_cfg_t cfg = {.channel   = k_ra_tau_channel_0,
                            .period    = (uint16_t)k_test_tau_period,
                            .prescaler = 0U};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_tau_open(&cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_tau_count_get(k_ra_tau_channel_0, &v));
  TEST_ASSERT_EQ((int32_t)0, (int32_t)v);
  TEST_END("count_get validates");
}

static void test_status_null(void)
{
  TEST_BEGIN("status_get null + bad channel");
  prep();
  uint8_t op = 0U;
  uint8_t rn = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_tau_status_get(k_ra_tau_channel_0, nullptr, &rn));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_tau_status_get(k_ra_tau_channel_0, &op, nullptr));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_tau_status_get((ra_tau_channel_t)9U, &op, &rn));
  TEST_END("status_get null + bad channel");
}

int32_t main(void)
{
  test_open_null();
  test_open_bad_args();
  test_lifecycle();
  test_start_stop();
  test_count();
  test_status_null();
  (void)fprintf(stderr, "[OK ] test_ra_tau.c\n");
  return 0;
}
