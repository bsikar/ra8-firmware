/**
 * @file test_ra_sau_lin.c
 * @brief Unit tests for ra_sau_lin.c (SAU LIN placeholder)
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra_err.h"
#include "ra_sau_lin.h"
#include "ra_sim_mmap.h"
#include "unity_minimal.h"

typedef enum : uint32_t {
  k_test_sau_lin_baud = 19200U,
} test_sau_lin_const_t;

static void prep(void)
{
  ra_sim_mmap_reset();
  (void)ra_sau_lin_close();
}

static void test_open_null(void)
{
  TEST_BEGIN("open rejects NULL cfg");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_sau_lin_open(nullptr));
  TEST_END("open rejects NULL cfg");
}

static void test_open_zero_baud(void)
{
  TEST_BEGIN("open rejects baud==0");
  prep();
  const ra_sau_lin_cfg_t cfg = {.channel = 0U, .baud = 0U, .break_bits = 13U};
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_sau_lin_open(&cfg));
  TEST_END("open rejects baud==0");
}

static void test_lifecycle(void)
{
  TEST_BEGIN("open / close lifecycle");
  prep();
  const ra_sau_lin_cfg_t cfg = {.channel    = 0U,
                                .baud       = (uint32_t)k_test_sau_lin_baud,
                                .break_bits = 13U};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_sau_lin_open(&cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_err_exists, (int32_t)ra_sau_lin_open(&cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_sau_lin_close());
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_sau_lin_close());
  TEST_END("open / close lifecycle");
}

static void test_send_header(void)
{
  TEST_BEGIN("send_header state-checked");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_err_not_initialized, (int32_t)ra_sau_lin_send_header(0x10U));
  const ra_sau_lin_cfg_t cfg = {.channel    = 0U,
                                .baud       = (uint32_t)k_test_sau_lin_baud,
                                .break_bits = 13U};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_sau_lin_open(&cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_err_not_supported, (int32_t)ra_sau_lin_send_header(0x10U));
  TEST_END("send_header state-checked");
}

static void test_response(void)
{
  TEST_BEGIN("send/recv response validate args");
  prep();
  uint8_t buf[8] = {};
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_sau_lin_send_response(nullptr, 4U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_sau_lin_send_response(buf, 0U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_sau_lin_send_response(buf, 9U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_not_initialized, (int32_t)ra_sau_lin_send_response(buf, 4U));

  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_sau_lin_recv_response(nullptr, 4U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_sau_lin_recv_response(buf, 0U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_sau_lin_recv_response(buf, 9U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_not_initialized, (int32_t)ra_sau_lin_recv_response(buf, 4U));

  const ra_sau_lin_cfg_t cfg = {.channel    = 0U,
                                .baud       = (uint32_t)k_test_sau_lin_baud,
                                .break_bits = 13U};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_sau_lin_open(&cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_err_not_supported, (int32_t)ra_sau_lin_send_response(buf, 4U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_not_supported, (int32_t)ra_sau_lin_recv_response(buf, 4U));
  TEST_END("send/recv response validate args");
}

static void test_status(void)
{
  TEST_BEGIN("status_get null + reflects state");
  prep();
  uint8_t op = 0U;
  uint8_t bs = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_sau_lin_status_get(nullptr, &bs));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_sau_lin_status_get(&op, nullptr));

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_sau_lin_status_get(&op, &bs));
  TEST_ASSERT_EQ((int32_t)0, (int32_t)op);
  TEST_END("status_get null + reflects state");
}

int32_t main(void)
{
  test_open_null();
  test_open_zero_baud();
  test_lifecycle();
  test_send_header();
  test_response();
  test_status();
  (void)fprintf(stderr, "[OK ] test_ra_sau_lin.c\n");
  return 0;
}
