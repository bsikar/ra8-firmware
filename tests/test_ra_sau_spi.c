/**
 * @file test_ra_sau_spi.c
 * @brief Unit tests for ra_sau_spi.c (SAU SPI placeholder)
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra_err.h"
#include "ra_sau_spi.h"
#include "ra_sim_mmap.h"
#include "unity_minimal.h"

typedef enum : uint32_t {
  k_test_sau_spi_bitrate = 1000000U,
} test_sau_spi_const_t;

static void prep(void)
{
  ra_sim_mmap_reset();
  (void)ra_sau_spi_close();
}

static void test_open_null(void)
{
  TEST_BEGIN("open rejects NULL cfg");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_sau_spi_open(nullptr));
  TEST_END("open rejects NULL cfg");
}

static void test_open_zero_rate(void)
{
  TEST_BEGIN("open rejects bitrate==0");
  prep();
  const ra_sau_spi_cfg_t cfg = {.channel   = 0U,
                                .bitrate   = 0U,
                                .cpol      = 0U,
                                .cpha      = 0U,
                                .msb_first = 1U};
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_sau_spi_open(&cfg));
  TEST_END("open rejects bitrate==0");
}

static void test_lifecycle(void)
{
  TEST_BEGIN("open / close lifecycle");
  prep();
  const ra_sau_spi_cfg_t cfg = {.channel   = 0U,
                                .bitrate   = (uint32_t)k_test_sau_spi_bitrate,
                                .cpol      = 0U,
                                .cpha      = 0U,
                                .msb_first = 1U};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_sau_spi_open(&cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_err_exists, (int32_t)ra_sau_spi_open(&cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_sau_spi_close());
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_sau_spi_close());
  TEST_END("open / close lifecycle");
}

static void test_write_read(void)
{
  TEST_BEGIN("write/read/write_read validate args");
  prep();
  uint8_t buf[4]   = {};
  uint8_t bufrx[4] = {};
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_sau_spi_write(nullptr, 4U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_sau_spi_write(buf, 0U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_not_initialized, (int32_t)ra_sau_spi_write(buf, 4U));

  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_sau_spi_read(nullptr, 4U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_sau_spi_read(bufrx, 0U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_not_initialized, (int32_t)ra_sau_spi_read(bufrx, 4U));

  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_sau_spi_write_read(nullptr, bufrx, 4U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_sau_spi_write_read(buf, nullptr, 4U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_sau_spi_write_read(buf, bufrx, 0U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_not_initialized, (int32_t)ra_sau_spi_write_read(buf, bufrx, 4U));

  const ra_sau_spi_cfg_t cfg = {.channel   = 0U,
                                .bitrate   = (uint32_t)k_test_sau_spi_bitrate,
                                .cpol      = 0U,
                                .cpha      = 0U,
                                .msb_first = 1U};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_sau_spi_open(&cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_err_not_supported, (int32_t)ra_sau_spi_write(buf, 4U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_not_supported, (int32_t)ra_sau_spi_read(bufrx, 4U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_not_supported, (int32_t)ra_sau_spi_write_read(buf, bufrx, 4U));
  TEST_END("write/read/write_read validate args");
}

static void test_abort(void)
{
  TEST_BEGIN("abort state-checked");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_err_not_initialized, (int32_t)ra_sau_spi_abort());
  const ra_sau_spi_cfg_t cfg = {.channel   = 0U,
                                .bitrate   = (uint32_t)k_test_sau_spi_bitrate,
                                .cpol      = 0U,
                                .cpha      = 0U,
                                .msb_first = 1U};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_sau_spi_open(&cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_sau_spi_abort());
  TEST_END("abort state-checked");
}

static void test_status(void)
{
  TEST_BEGIN("status_get");
  prep();
  uint8_t op = 0U;
  uint8_t bs = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_sau_spi_status_get(nullptr, &bs));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_sau_spi_status_get(&op, nullptr));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_sau_spi_status_get(&op, &bs));
  TEST_ASSERT_EQ((int32_t)0, (int32_t)op);
  TEST_END("status_get");
}

int32_t main(void)
{
  test_open_null();
  test_open_zero_rate();
  test_lifecycle();
  test_write_read();
  test_abort();
  test_status();
  (void)fprintf(stderr, "[OK ] test_ra_sau_spi.c\n");
  return 0;
}
