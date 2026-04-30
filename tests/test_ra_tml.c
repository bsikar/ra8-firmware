/**
 * @file test_ra_tml.c
 * @brief Unit tests for ra_tml.c (TML placeholder)
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra_err.h"
#include "ra_sim_mmap.h"
#include "ra_tml.h"
#include "unity_minimal.h"

typedef enum : uint32_t {
  k_test_tml_period = 0x10000U,
} test_tml_const_t;

static void prep(void)
{
  ra_sim_mmap_reset();
  (void)ra_tml_close();
}

static void test_open_null(void)
{
  TEST_BEGIN("open rejects NULL cfg");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_tml_open(nullptr));
  TEST_END("open rejects NULL cfg");
}

static void test_open_zero_period(void)
{
  TEST_BEGIN("open rejects period==0");
  prep();
  const ra_tml_cfg_t cfg = {.channel = 0U, .period = 0U, .one_shot = 0U};
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_tml_open(&cfg));
  TEST_END("open rejects period==0");
}

static void test_lifecycle(void)
{
  TEST_BEGIN("open / close lifecycle");
  prep();
  const ra_tml_cfg_t cfg = {.channel = 0U, .period = (uint32_t)k_test_tml_period, .one_shot = 1U};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_tml_open(&cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_err_exists, (int32_t)ra_tml_open(&cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_tml_close());
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_tml_close());
  TEST_END("open / close lifecycle");
}

static void test_start_stop(void)
{
  TEST_BEGIN("start/stop validate state");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_err_not_initialized, (int32_t)ra_tml_start());
  TEST_ASSERT_EQ((int32_t)k_ra_err_not_initialized, (int32_t)ra_tml_stop());

  const ra_tml_cfg_t cfg = {.channel = 0U, .period = (uint32_t)k_test_tml_period, .one_shot = 0U};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_tml_open(&cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_tml_start());

  uint8_t op = 0U;
  uint8_t rn = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_tml_status_get(&op, &rn));
  TEST_ASSERT_EQ((int32_t)1, (int32_t)rn);

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_tml_stop());
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_tml_status_get(&op, &rn));
  TEST_ASSERT_EQ((int32_t)0, (int32_t)rn);
  TEST_END("start/stop validate state");
}

static void test_count(void)
{
  TEST_BEGIN("count_get null + state");
  prep();
  uint32_t v = 0xFFFFFFFFU;
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_tml_count_get(nullptr));
  TEST_ASSERT_EQ((int32_t)k_ra_err_not_initialized, (int32_t)ra_tml_count_get(&v));

  const ra_tml_cfg_t cfg = {.channel = 0U, .period = (uint32_t)k_test_tml_period, .one_shot = 0U};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_tml_open(&cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_tml_count_get(&v));
  TEST_ASSERT_EQ((int32_t)0, (int32_t)v);
  TEST_END("count_get null + state");
}

static void test_status_null(void)
{
  TEST_BEGIN("status_get null");
  prep();
  uint8_t op = 0U;
  uint8_t rn = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_tml_status_get(nullptr, &rn));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_tml_status_get(&op, nullptr));
  TEST_END("status_get null");
}

int32_t main(void)
{
  test_open_null();
  test_open_zero_period();
  test_lifecycle();
  test_start_stop();
  test_count();
  test_status_null();
  (void)fprintf(stderr, "[OK ] test_ra_tml.c\n");
  return 0;
}
