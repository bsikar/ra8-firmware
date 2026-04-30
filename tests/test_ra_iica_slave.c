/**
 * @file test_ra_iica_slave.c
 * @brief Unit tests for ra_iica_slave.c (IICA peripheral placeholder)
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra_err.h"
#include "ra_iica_slave.h"
#include "ra_sim_mmap.h"
#include "unity_minimal.h"

typedef enum : uint16_t {
  k_test_iica_slave_addr = 0x0050U,
} test_iica_slave_const_t;

static void prep(void)
{
  ra_sim_mmap_reset();
  (void)ra_iica_slave_close();
}

static void test_open_null(void)
{
  TEST_BEGIN("open rejects NULL cfg");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_iica_slave_open(nullptr));
  TEST_END("open rejects NULL cfg");
}

static void test_open_bad_addr(void)
{
  TEST_BEGIN("open rejects out-of-range own_addr");
  prep();
  const ra_iica_slave_cfg_t cfg = {.channel = 0U, .own_addr = 0xFFFFU, .general_call = 0U};
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_iica_slave_open(&cfg));
  TEST_END("open rejects out-of-range own_addr");
}

static void test_lifecycle(void)
{
  TEST_BEGIN("open / close lifecycle");
  prep();
  const ra_iica_slave_cfg_t cfg = {.channel      = 0U,
                                   .own_addr     = (uint16_t)k_test_iica_slave_addr,
                                   .general_call = 0U};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_iica_slave_open(&cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_err_exists, (int32_t)ra_iica_slave_open(&cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_iica_slave_close());
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_iica_slave_close());
  TEST_END("open / close lifecycle");
}

static void test_read_write(void)
{
  TEST_BEGIN("read/write validate args");
  prep();
  uint8_t buf[4] = {};
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_iica_slave_read(nullptr, 4U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_iica_slave_read(buf, 0U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_not_initialized, (int32_t)ra_iica_slave_read(buf, 4U));

  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_iica_slave_write(nullptr, 4U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_iica_slave_write(buf, 0U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_not_initialized, (int32_t)ra_iica_slave_write(buf, 4U));

  const ra_iica_slave_cfg_t cfg = {.channel      = 0U,
                                   .own_addr     = (uint16_t)k_test_iica_slave_addr,
                                   .general_call = 0U};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_iica_slave_open(&cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_err_not_supported, (int32_t)ra_iica_slave_read(buf, 4U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_not_supported, (int32_t)ra_iica_slave_write(buf, 4U));
  TEST_END("read/write validate args");
}

static void test_status(void)
{
  TEST_BEGIN("status_get null + reflects state");
  prep();
  uint8_t op = 0U;
  uint8_t bs = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_iica_slave_status_get(nullptr, &bs));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_iica_slave_status_get(&op, nullptr));

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_iica_slave_status_get(&op, &bs));
  TEST_ASSERT_EQ((int32_t)0, (int32_t)op);

  const ra_iica_slave_cfg_t cfg = {.channel      = 0U,
                                   .own_addr     = (uint16_t)k_test_iica_slave_addr,
                                   .general_call = 1U};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_iica_slave_open(&cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_iica_slave_status_get(&op, &bs));
  TEST_ASSERT_EQ((int32_t)1, (int32_t)op);
  TEST_END("status_get null + reflects state");
}

int32_t main(void)
{
  test_open_null();
  test_open_bad_addr();
  test_lifecycle();
  test_read_write();
  test_status();
  (void)fprintf(stderr, "[OK ] test_ra_iica_slave.c\n");
  return 0;
}
