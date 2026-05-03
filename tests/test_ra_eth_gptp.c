/**
 * @file test_ra_eth_gptp.c
 * @brief Unit tests for ra_eth_gptp.c (GPTP sub-driver)
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8d2_ether_regs.h"
#include "ra_err.h"
#include "ra_eth_gptp.h"
#include "ra_mstp.h"
#include "ra_sim_mmap.h"
#include "unity_minimal.h"

static uint32_t s_gptp_cb_count;
static uint32_t s_gptp_cb_last_mask;

static void stub_gptp_cb(void* ctx, uint32_t mask)
{
  (void)ctx;
  ++s_gptp_cb_count;
  s_gptp_cb_last_mask = mask;
}

static void prep(void)
{
  ra_sim_mmap_reset();
  (void)ra_mstp_init();
  s_gptp_cb_count     = 0U;
  s_gptp_cb_last_mask = 0U;
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init(void)
{
  TEST_BEGIN("gptp init");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_eth_gptp_init());
  TEST_END("gptp init");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_deinit(void)
{
  TEST_BEGIN("gptp deinit");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_eth_gptp_init());
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_eth_gptp_deinit());
  TEST_END("gptp deinit");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_status_read_and_clear(void)
{
  TEST_BEGIN("gptp status read + clear");
  prep();
  ra_gptp()->GPTP_STS = 0x1EEE1588U;
  uint32_t mask       = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_eth_gptp_get_status(&mask));
  TEST_ASSERT_EQ((int32_t)0x1EEE1588U, (int32_t)mask);
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_eth_gptp_clear_status(0x0000FFFFU));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_eth_gptp_get_status(nullptr));
  TEST_END("gptp status read + clear");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_attach_and_dispatch(void)
{
  TEST_BEGIN("gptp attach + dispatch");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_eth_gptp_attach_handler(stub_gptp_cb, (void*)(uintptr_t)0xF0U));
  ra_gptp()->GPTP_STS = 0xBEEFU;
  ra_eth_gptp_dispatch();
  TEST_ASSERT_EQ((int32_t)1, (int32_t)s_gptp_cb_count);
  TEST_ASSERT_EQ((int32_t)0xBEEFU, (int32_t)s_gptp_cb_last_mask);

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_eth_gptp_attach_handler(nullptr, nullptr));
  ra_gptp()->GPTP_STS = 0xFACEU;
  ra_eth_gptp_dispatch();
  TEST_ASSERT_EQ((int32_t)1, (int32_t)s_gptp_cb_count);
  TEST_END("gptp attach + dispatch");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_power_transition(void)
{
  TEST_BEGIN("gptp power transition");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_eth_gptp_init());
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_eth_gptp_enter_stop());
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_eth_gptp_exit_stop());
  TEST_END("gptp power transition");
}

int32_t main(void)
{
  test_init();
  test_deinit();
  test_status_read_and_clear();
  test_attach_and_dispatch();
  test_power_transition();
  (void)fprintf(stderr, "[OK  ] test_ra_eth_gptp.c\n");
  return 0;
}
