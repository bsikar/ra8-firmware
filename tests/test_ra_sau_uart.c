/**
 * @file test_ra_sau_uart.c
 * @brief Unit tests for ra_sau_uart.c (SAU UART placeholder)
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra_err.h"
#include "ra_sau_uart.h"
#include "ra_sim_mmap.h"
#include "unity_minimal.h"

typedef enum : uint32_t {
  k_test_sau_uart_baud = 115200U,
} test_sau_uart_const_t;

static void prep(void)
{
  ra_sim_mmap_reset();
  (void)ra_sau_uart_close();
}

static void test_open_null(void)
{
  TEST_BEGIN("open rejects NULL cfg");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_sau_uart_open(nullptr));
  TEST_END("open rejects NULL cfg");
}

static void test_open_zero_baud(void)
{
  TEST_BEGIN("open rejects baud==0");
  prep();
  const ra_sau_uart_cfg_t cfg = {.channel   = 0U,
                                 .baud      = 0U,
                                 .data_bits = 8U,
                                 .parity    = 0U,
                                 .stop_bits = 1U};
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_sau_uart_open(&cfg));
  TEST_END("open rejects baud==0");
}

static void test_lifecycle(void)
{
  TEST_BEGIN("open / close lifecycle");
  prep();
  const ra_sau_uart_cfg_t cfg = {.channel   = 0U,
                                 .baud      = (uint32_t)k_test_sau_uart_baud,
                                 .data_bits = 8U,
                                 .parity    = 0U,
                                 .stop_bits = 1U};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_sau_uart_open(&cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_err_exists, (int32_t)ra_sau_uart_open(&cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_sau_uart_close());
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_sau_uart_close());
  TEST_END("open / close lifecycle");
}

static void test_send_recv(void)
{
  TEST_BEGIN("send/recv validate args, refuse on placeholder");
  prep();
  uint8_t buf[4] = {};
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_sau_uart_send(nullptr, 4U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_sau_uart_send(buf, 0U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_not_initialized, (int32_t)ra_sau_uart_send(buf, 4U));

  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_sau_uart_recv(nullptr, 4U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_sau_uart_recv(buf, 0U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_not_initialized, (int32_t)ra_sau_uart_recv(buf, 4U));

  const ra_sau_uart_cfg_t cfg = {.channel   = 0U,
                                 .baud      = (uint32_t)k_test_sau_uart_baud,
                                 .data_bits = 8U,
                                 .parity    = 0U,
                                 .stop_bits = 1U};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_sau_uart_open(&cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_err_not_supported, (int32_t)ra_sau_uart_send(buf, 4U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_not_supported, (int32_t)ra_sau_uart_recv(buf, 4U));
  TEST_END("send/recv validate args, refuse on placeholder");
}

static void test_abort(void)
{
  TEST_BEGIN("abort state-checked");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_err_not_initialized, (int32_t)ra_sau_uart_abort());
  const ra_sau_uart_cfg_t cfg = {.channel   = 0U,
                                 .baud      = (uint32_t)k_test_sau_uart_baud,
                                 .data_bits = 8U,
                                 .parity    = 0U,
                                 .stop_bits = 1U};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_sau_uart_open(&cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_sau_uart_abort());
  TEST_END("abort state-checked");
}

static void test_status(void)
{
  TEST_BEGIN("status_get null + reflects state");
  prep();
  uint8_t op = 0U;
  uint8_t bs = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_sau_uart_status_get(nullptr, &bs));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_sau_uart_status_get(&op, nullptr));

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_sau_uart_status_get(&op, &bs));
  TEST_ASSERT_EQ((int32_t)0, (int32_t)op);
  TEST_END("status_get null + reflects state");
}

int32_t main(void)
{
  test_open_null();
  test_open_zero_baud();
  test_lifecycle();
  test_send_recv();
  test_abort();
  test_status();
  (void)fprintf(stderr, "[OK ] test_ra_sau_uart.c\n");
  return 0;
}
