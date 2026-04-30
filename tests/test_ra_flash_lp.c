/**
 * @file test_ra_flash_lp.c
 * @brief Unit tests for ra_flash_lp.c (FLASH_LP placeholder driver)
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra_err.h"
#include "ra_flash_lp.h"
#include "ra_sim_mmap.h"
#include "unity_minimal.h"

typedef enum : uint32_t {
  k_test_flash_lp_fclk_hz = 4000000U, /**< 4 MHz reference. */
  k_test_flash_lp_addr    = 0x40100000U,
  k_test_flash_lp_len     = 64U,
} test_flash_lp_const_t;

static void prep(void)
{
  ra_sim_mmap_reset();
  (void)ra_flash_lp_close();
}

static void test_open_null(void)
{
  TEST_BEGIN("open rejects NULL cfg");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_flash_lp_open(nullptr));
  TEST_END("open rejects NULL cfg");
}

static void test_open_zero_fclk(void)
{
  TEST_BEGIN("open rejects fclk_hz==0");
  prep();
  const ra_flash_lp_cfg_t cfg = {.fclk_hz = 0U, .data_flash = 0U, .bg_operation = 0U};
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_flash_lp_open(&cfg));
  TEST_END("open rejects fclk_hz==0");
}

static void test_lifecycle(void)
{
  TEST_BEGIN("open / close lifecycle");
  prep();
  const ra_flash_lp_cfg_t cfg = {.fclk_hz      = (uint32_t)k_test_flash_lp_fclk_hz,
                                 .data_flash   = 1U,
                                 .bg_operation = 0U};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_flash_lp_open(&cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_err_exists, (int32_t)ra_flash_lp_open(&cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_flash_lp_close());
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state, (int32_t)ra_flash_lp_close());
  TEST_END("open / close lifecycle");
}

static void test_write_validation(void)
{
  TEST_BEGIN("write validates args and refuses on placeholder");
  prep();
  const uint8_t buf[4] = {0xAAU, 0xBBU, 0xCCU, 0xDDU};

  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_flash_lp_write((uint32_t)k_test_flash_lp_addr, nullptr, 4U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_flash_lp_write((uint32_t)k_test_flash_lp_addr, buf, 0U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_not_initialized,
                 (int32_t)ra_flash_lp_write((uint32_t)k_test_flash_lp_addr, buf, 4U));

  const ra_flash_lp_cfg_t cfg = {.fclk_hz      = (uint32_t)k_test_flash_lp_fclk_hz,
                                 .data_flash   = 0U,
                                 .bg_operation = 0U};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_flash_lp_open(&cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_err_not_supported,
                 (int32_t)ra_flash_lp_write((uint32_t)k_test_flash_lp_addr, buf, 4U));
  TEST_END("write validates args and refuses on placeholder");
}

static void test_erase_validation(void)
{
  TEST_BEGIN("erase validates args and refuses on placeholder");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_flash_lp_erase((uint32_t)k_test_flash_lp_addr, 0U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_not_initialized,
                 (int32_t)ra_flash_lp_erase((uint32_t)k_test_flash_lp_addr, 1U));

  const ra_flash_lp_cfg_t cfg = {.fclk_hz      = (uint32_t)k_test_flash_lp_fclk_hz,
                                 .data_flash   = 0U,
                                 .bg_operation = 0U};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_flash_lp_open(&cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_err_not_supported,
                 (int32_t)ra_flash_lp_erase((uint32_t)k_test_flash_lp_addr, 2U));
  TEST_END("erase validates args and refuses on placeholder");
}

static void test_blank_check(void)
{
  TEST_BEGIN("blank_check validates args, returns 0 on placeholder");
  prep();
  uint8_t blank = 0xFFU;
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_flash_lp_blank_check((uint32_t)k_test_flash_lp_addr,
                                                  (uint32_t)k_test_flash_lp_len,
                                                  nullptr));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_flash_lp_blank_check((uint32_t)k_test_flash_lp_addr, 0U, &blank));
  TEST_ASSERT_EQ((int32_t)k_ra_err_not_initialized,
                 (int32_t)ra_flash_lp_blank_check((uint32_t)k_test_flash_lp_addr,
                                                  (uint32_t)k_test_flash_lp_len,
                                                  &blank));

  const ra_flash_lp_cfg_t cfg = {.fclk_hz      = (uint32_t)k_test_flash_lp_fclk_hz,
                                 .data_flash   = 0U,
                                 .bg_operation = 0U};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_flash_lp_open(&cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_flash_lp_blank_check((uint32_t)k_test_flash_lp_addr,
                                                  (uint32_t)k_test_flash_lp_len,
                                                  &blank));
  TEST_ASSERT_EQ(0, (int32_t)blank);
  TEST_END("blank_check validates args, returns 0 on placeholder");
}

static void test_status(void)
{
  TEST_BEGIN("status_get reflects state");
  prep();
  uint8_t op = 0U;
  uint8_t bs = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_flash_lp_status_get(nullptr, &bs));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_flash_lp_status_get(&op, nullptr));

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_flash_lp_status_get(&op, &bs));
  TEST_ASSERT_EQ((int32_t)0, (int32_t)op);

  const ra_flash_lp_cfg_t cfg = {.fclk_hz      = (uint32_t)k_test_flash_lp_fclk_hz,
                                 .data_flash   = 0U,
                                 .bg_operation = 0U};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_flash_lp_open(&cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_flash_lp_status_get(&op, &bs));
  TEST_ASSERT_EQ((int32_t)1, (int32_t)op);
  TEST_END("status_get reflects state");
}

int32_t main(void)
{
  test_open_null();
  test_open_zero_fclk();
  test_lifecycle();
  test_write_validation();
  test_erase_validation();
  test_blank_check();
  test_status();
  (void)fprintf(stderr, "[OK ] test_ra_flash_lp.c\n");
  return 0;
}
