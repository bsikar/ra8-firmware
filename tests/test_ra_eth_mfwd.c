/**
 * @file test_ra_eth_mfwd.c
 * @brief Unit tests for ra_eth_mfwd.c (MFWD sub-driver)
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8d2_ether_regs.h"
#include "ra_err.h"
#include "ra_eth_mfwd.h"
#include "ra_mstp.h"
#include "ra_sim_mmap.h"
#include "unity_minimal.h"

static uint32_t s_mfwd_cb_count;
static uint32_t s_mfwd_cb_last_mask;

static void stub_mfwd_cb(void* ctx, uint32_t mask)
{
  (void)ctx;
  ++s_mfwd_cb_count;
  s_mfwd_cb_last_mask = mask;
}

static void prep(void)
{
  ra_sim_mmap_reset();
  (void)ra_mstp_init();
  s_mfwd_cb_count     = 0U;
  s_mfwd_cb_last_mask = 0U;
}

static void test_init(void)
{
  TEST_BEGIN("mfwd init");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_eth_mfwd_init());
  TEST_END("mfwd init");
}

static void test_deinit(void)
{
  TEST_BEGIN("mfwd deinit");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_eth_mfwd_init());
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_eth_mfwd_deinit());
  TEST_END("mfwd deinit");
}

static void test_status_read_and_clear(void)
{
  TEST_BEGIN("mfwd status read + clear");
  prep();
  ra_mfwd()->MFWD_STS = 0xFEEDFACEU;
  uint32_t mask       = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_eth_mfwd_get_status(&mask));
  TEST_ASSERT_EQ((int32_t)0xFEEDFACEU, (int32_t)mask);
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_eth_mfwd_clear_status(0x0F0F0F0FU));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_eth_mfwd_get_status(nullptr));
  TEST_END("mfwd status read + clear");
}

static void test_attach_and_dispatch(void)
{
  TEST_BEGIN("mfwd attach + dispatch");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_eth_mfwd_attach_handler(stub_mfwd_cb, (void*)(uintptr_t)0xA0U));
  ra_mfwd()->MFWD_STS = 0xABCDU;
  ra_eth_mfwd_dispatch();
  TEST_ASSERT_EQ((int32_t)1, (int32_t)s_mfwd_cb_count);
  TEST_ASSERT_EQ((int32_t)0xABCDU, (int32_t)s_mfwd_cb_last_mask);

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_eth_mfwd_attach_handler(nullptr, nullptr));
  ra_mfwd()->MFWD_STS = 0xDEADU;
  ra_eth_mfwd_dispatch();
  TEST_ASSERT_EQ((int32_t)1, (int32_t)s_mfwd_cb_count);
  TEST_END("mfwd attach + dispatch");
}

static void test_power_transition(void)
{
  TEST_BEGIN("mfwd power transition");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_eth_mfwd_init());
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_eth_mfwd_enter_stop());
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_eth_mfwd_exit_stop());
  TEST_END("mfwd power transition");
}

int32_t main(void)
{
  test_init();
  test_deinit();
  test_status_read_and_clear();
  test_attach_and_dispatch();
  test_power_transition();
  (void)fprintf(stderr, "[OK  ] test_ra_eth_mfwd.c\n");
  return 0;
}
