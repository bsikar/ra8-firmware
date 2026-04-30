/**
 * @file test_ra_iica_master.c
 * @brief Unit tests for ra_iica_master.c (IICA controller placeholder)
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra_err.h"
#include "ra_iica_master.h"
#include "ra_sim_mmap.h"
#include "unity_minimal.h"

typedef enum : uint16_t {
  k_test_iica_addr   = 0x0050U,
  k_test_iica_addr_x = 0x03FFU, /**< Top of 10-bit range. */
} test_iica_master_const_t;

static void prep(void)
{
  ra_sim_mmap_reset();
  (void)ra_iica_master_close();
}

static void test_open_null(void)
{
  TEST_BEGIN("open rejects NULL cfg");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_iica_master_open(nullptr));
  TEST_END("open rejects NULL cfg");
}

static void test_open_bad_args(void)
{
  TEST_BEGIN("open rejects bad addr_mode and zero rate");
  prep();
  ra_iica_master_cfg_t cfg = {.channel    = 0U,
                              .addr_mode  = (ra_iica_master_addr_t)9U,
                              .rate_hz    = 100000U,
                              .slave_addr = (uint16_t)k_test_iica_addr};
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_iica_master_open(&cfg));

  cfg.addr_mode = k_ra_iica_master_addr_7bit;
  cfg.rate_hz   = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_iica_master_open(&cfg));
  TEST_END("open rejects bad addr_mode and zero rate");
}

static void test_lifecycle(void)
{
  TEST_BEGIN("open / close lifecycle");
  prep();
  const ra_iica_master_cfg_t cfg = {.channel    = 0U,
                                    .addr_mode  = k_ra_iica_master_addr_7bit,
                                    .rate_hz    = 100000U,
                                    .slave_addr = (uint16_t)k_test_iica_addr};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_iica_master_open(&cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_err_exists, (int32_t)ra_iica_master_open(&cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_iica_master_close());
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_iica_master_close());
  TEST_END("open / close lifecycle");
}

static void test_read_write_validation(void)
{
  TEST_BEGIN("read/write validate args, refuse on placeholder");
  prep();
  uint8_t buf[4] = {};
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_iica_master_read(nullptr, 4U, 0U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_iica_master_read(buf, 0U, 0U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_not_initialized, (int32_t)ra_iica_master_read(buf, 4U, 0U));

  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_iica_master_write(nullptr, 4U, 0U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_iica_master_write(buf, 0U, 0U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_not_initialized, (int32_t)ra_iica_master_write(buf, 4U, 0U));

  const ra_iica_master_cfg_t cfg = {.channel    = 0U,
                                    .addr_mode  = k_ra_iica_master_addr_7bit,
                                    .rate_hz    = 100000U,
                                    .slave_addr = (uint16_t)k_test_iica_addr};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_iica_master_open(&cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_err_not_supported, (int32_t)ra_iica_master_read(buf, 4U, 0U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_not_supported, (int32_t)ra_iica_master_write(buf, 4U, 0U));
  TEST_END("read/write validate args, refuse on placeholder");
}

static void test_addr_set(void)
{
  TEST_BEGIN("slave_address_set validates");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_iica_master_slave_address_set(0xFFFFU));
  TEST_ASSERT_EQ((int32_t)k_ra_err_not_initialized,
                 (int32_t)ra_iica_master_slave_address_set((uint16_t)k_test_iica_addr));

  const ra_iica_master_cfg_t cfg = {.channel    = 0U,
                                    .addr_mode  = k_ra_iica_master_addr_10bit,
                                    .rate_hz    = 400000U,
                                    .slave_addr = (uint16_t)k_test_iica_addr};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_iica_master_open(&cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_iica_master_slave_address_set((uint16_t)k_test_iica_addr_x));
  TEST_END("slave_address_set validates");
}

static void test_abort_status(void)
{
  TEST_BEGIN("abort and status_get");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_err_not_initialized, (int32_t)ra_iica_master_abort());
  uint8_t op = 0U;
  uint8_t bs = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_iica_master_status_get(nullptr, &bs));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_iica_master_status_get(&op, nullptr));

  const ra_iica_master_cfg_t cfg = {.channel    = 0U,
                                    .addr_mode  = k_ra_iica_master_addr_7bit,
                                    .rate_hz    = 100000U,
                                    .slave_addr = (uint16_t)k_test_iica_addr};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_iica_master_open(&cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_iica_master_abort());
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_iica_master_status_get(&op, &bs));
  TEST_ASSERT_EQ((int32_t)1, (int32_t)op);
  TEST_END("abort and status_get");
}

int32_t main(void)
{
  test_open_null();
  test_open_bad_args();
  test_lifecycle();
  test_read_write_validation();
  test_addr_set();
  test_abort_status();
  (void)fprintf(stderr, "[OK ] test_ra_iica_master.c\n");
  return 0;
}
