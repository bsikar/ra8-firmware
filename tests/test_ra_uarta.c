/**
 * @file test_ra_uarta.c
 * @brief Unit tests for ra_uarta.c (legacy UART-A placeholder driver)
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_err.h"
#include "ra_sim_mmap.h"
#include "ra_uarta.h"
#include "unity_minimal.h"

typedef enum : uint32_t {
  k_test_baud    = 9600U,
  k_test_baud_lo = 115200U,
} uarta_test_t;

static uint32_t s_cb_count;

static void cb(void* ctx, uint8_t ch)
{
  (void)ctx;
  (void)ch;
  ++s_cb_count;
}

static void test_open_close(void)
{
  TEST_BEGIN("uarta open/close");
  ra_sim_mmap_reset();

  const ra_uarta_cfg_t cfg = {
    .channel      = 0U,
    .baud_rate    = (uint32_t)k_test_baud,
    .data_bits    = 8U,
    .stop_bits    = 1U,
    .parity_en    = false,
    .parity_odd   = false,
    .clock_source = k_ra_uarta_clk_hoco,
  };
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_uarta_open(&cfg));

  uint8_t status = 0U;
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_uarta_get_status(0U, &status));
  TEST_ASSERT((status & 0x1U) != 0U);

  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_uarta_close(0U));
  TEST_END("uarta open/close");
}

static void test_open_null_bad(void)
{
  TEST_BEGIN("uarta open null/bad");
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_uarta_open(nullptr));
  ra_uarta_cfg_t cfg = {.channel = 9U, .baud_rate = (uint32_t)k_test_baud};
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)ra_uarta_open(&cfg));
  cfg.channel   = 0U;
  cfg.baud_rate = 0U;
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)ra_uarta_open(&cfg));
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)ra_uarta_close(9U));
  TEST_END("uarta open null/bad");
}

static void test_loopback(void)
{
  TEST_BEGIN("uarta loopback");
  ra_sim_mmap_reset();
  const ra_uarta_cfg_t cfg = {.channel = 0U, .baud_rate = (uint32_t)k_test_baud_lo};
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_uarta_open(&cfg));

  const uint8_t in[] = {0x10U, 0x20U, 0x30U, 0x40U};
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_uarta_write(0U, in, 4U));

  uint8_t  out[4] = {0U, 0U, 0U, 0U};
  uint16_t got    = 0U;
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_uarta_read(0U, out, 4U, &got));
  TEST_ASSERT_EQ((int)4, (int)got);
  TEST_ASSERT_EQ((int)0x10, (int)out[0]);
  TEST_ASSERT_EQ((int)0x40, (int)out[3]);
  TEST_END("uarta loopback");
}

static void test_write_unopened(void)
{
  TEST_BEGIN("uarta write unopened");
  ra_sim_mmap_reset();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_uarta_close(1U));
  const uint8_t b = 1U;
  TEST_ASSERT_EQ((int)k_ra_err_invalid_state, (int)ra_uarta_write(1U, &b, 1U));
  TEST_END("uarta write unopened");
}

static void test_set_baud(void)
{
  TEST_BEGIN("uarta set_baud");
  ra_sim_mmap_reset();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_uarta_set_baud(0U, 19200U));
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)ra_uarta_set_baud(0U, 0U));
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)ra_uarta_set_baud(9U, 9600U));
  TEST_END("uarta set_baud");
}

static void test_null_args(void)
{
  TEST_BEGIN("uarta null args");
  uint8_t  buf = 0U;
  uint16_t n   = 0U;
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_uarta_write(0U, nullptr, 1U));
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_uarta_read(0U, nullptr, 1U, &n));
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_uarta_read(0U, &buf, 1U, nullptr));
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_uarta_get_status(0U, nullptr));
  TEST_END("uarta null args");
}

static void test_dispatch(void)
{
  TEST_BEGIN("uarta dispatch");
  ra_sim_mmap_reset();
  s_cb_count = 0U;
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_uarta_attach_handler(cb, nullptr));
  ra_uarta_dispatch(0U);
  ra_uarta_dispatch(9U);
  TEST_ASSERT_EQ((int)1, (int)s_cb_count);
  TEST_END("uarta dispatch");
}

int32_t main(void)
{
  test_open_close();
  test_open_null_bad();
  test_loopback();
  test_write_unopened();
  test_set_baud();
  test_null_args();
  test_dispatch();
  (void)fprintf(stderr, "[OK  ] test_ra_uarta.c\n");
  return 0;
}
