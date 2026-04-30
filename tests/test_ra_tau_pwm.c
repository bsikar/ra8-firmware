/**
 * @file test_ra_tau_pwm.c
 * @brief Unit tests for ra_tau_pwm.c (TAU PWM placeholder)
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra_err.h"
#include "ra_sim_mmap.h"
#include "ra_tau_pwm.h"
#include "unity_minimal.h"

typedef enum : uint16_t {
  k_test_tau_pwm_period = 1000U,
  k_test_tau_pwm_duty   = 250U,
} test_tau_pwm_const_t;

static void prep(void)
{
  ra_sim_mmap_reset();
  (void)ra_tau_pwm_close();
}

static void test_open_null(void)
{
  TEST_BEGIN("open rejects NULL cfg");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_tau_pwm_open(nullptr));
  TEST_END("open rejects NULL cfg");
}

static void test_open_bad_args(void)
{
  TEST_BEGIN("open rejects zero period or duty>=period");
  prep();
  ra_tau_pwm_cfg_t cfg = {.channel = 0U, .period = 0U, .duty = (uint16_t)k_test_tau_pwm_duty};
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_tau_pwm_open(&cfg));

  cfg.period = (uint16_t)k_test_tau_pwm_period;
  cfg.duty   = (uint16_t)k_test_tau_pwm_period;
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_tau_pwm_open(&cfg));
  TEST_END("open rejects zero period or duty>=period");
}

static void test_lifecycle(void)
{
  TEST_BEGIN("open / close lifecycle");
  prep();
  const ra_tau_pwm_cfg_t cfg = {.channel = 0U,
                                .period  = (uint16_t)k_test_tau_pwm_period,
                                .duty    = (uint16_t)k_test_tau_pwm_duty};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_tau_pwm_open(&cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_err_exists, (int32_t)ra_tau_pwm_open(&cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_tau_pwm_close());
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_tau_pwm_close());
  TEST_END("open / close lifecycle");
}

static void test_start_stop(void)
{
  TEST_BEGIN("start/stop validate state and update");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_err_not_initialized, (int32_t)ra_tau_pwm_start());
  TEST_ASSERT_EQ((int32_t)k_ra_err_not_initialized, (int32_t)ra_tau_pwm_stop());

  const ra_tau_pwm_cfg_t cfg = {.channel = 0U,
                                .period  = (uint16_t)k_test_tau_pwm_period,
                                .duty    = (uint16_t)k_test_tau_pwm_duty};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_tau_pwm_open(&cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_tau_pwm_start());

  uint8_t op = 0U;
  uint8_t rn = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_tau_pwm_status_get(&op, &rn));
  TEST_ASSERT_EQ((int32_t)1, (int32_t)rn);

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_tau_pwm_stop());
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_tau_pwm_status_get(&op, &rn));
  TEST_ASSERT_EQ((int32_t)0, (int32_t)rn);
  TEST_END("start/stop validate state and update");
}

static void test_duty_set(void)
{
  TEST_BEGIN("duty_set validates");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_err_not_initialized, (int32_t)ra_tau_pwm_duty_set(100U));
  const ra_tau_pwm_cfg_t cfg = {.channel = 0U,
                                .period  = (uint16_t)k_test_tau_pwm_period,
                                .duty    = (uint16_t)k_test_tau_pwm_duty};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_tau_pwm_open(&cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_tau_pwm_duty_set((uint16_t)k_test_tau_pwm_period));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_tau_pwm_duty_set(500U));
  TEST_END("duty_set validates");
}

static void test_period_set(void)
{
  TEST_BEGIN("period_set validates");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_err_not_initialized, (int32_t)ra_tau_pwm_period_set(2000U));
  const ra_tau_pwm_cfg_t cfg = {.channel = 0U,
                                .period  = (uint16_t)k_test_tau_pwm_period,
                                .duty    = (uint16_t)k_test_tau_pwm_duty};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_tau_pwm_open(&cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_tau_pwm_period_set(0U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_tau_pwm_period_set((uint16_t)k_test_tau_pwm_duty));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_tau_pwm_period_set(2000U));
  TEST_END("period_set validates");
}

static void test_status_null(void)
{
  TEST_BEGIN("status_get null");
  prep();
  uint8_t op = 0U;
  uint8_t rn = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_tau_pwm_status_get(nullptr, &rn));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_tau_pwm_status_get(&op, nullptr));
  TEST_END("status_get null");
}

int32_t main(void)
{
  test_open_null();
  test_open_bad_args();
  test_lifecycle();
  test_start_stop();
  test_duty_set();
  test_period_set();
  test_status_null();
  (void)fprintf(stderr, "[OK ] test_ra_tau_pwm.c\n");
  return 0;
}
