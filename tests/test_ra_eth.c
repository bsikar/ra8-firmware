/**
 * @file test_ra_eth.c
 * @brief Unit tests for ra_eth.c (Ethernet Switch Module scaffold)
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8d2_ether_regs.h"
#include "ra_err.h"
#include "ra_eth.h"
#include "ra_mstp.h"
#include "ra_sim_mmap.h"
#include "unity_minimal.h"

static uint32_t s_eth_cb_count;
static uint32_t s_eth_cb_last_mask;

static void stub_eth_cb(void* ctx, uint32_t mask)
{
  (void)ctx;
  ++s_eth_cb_count;
  s_eth_cb_last_mask = mask;
}

static void prep(void)
{
  ra_sim_mmap_reset();
  (void)ra_mstp_init();
  s_eth_cb_count     = 0U;
  s_eth_cb_last_mask = 0U;
}

static void test_init(void)
{
  TEST_BEGIN("eth init");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_eth_init());
  TEST_END("eth init");
}

static void test_deinit(void)
{
  TEST_BEGIN("eth deinit");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_eth_init());
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_eth_deinit());
  TEST_END("eth deinit");
}

static void test_status_read_and_clear(void)
{
  TEST_BEGIN("eth status read + clear");
  prep();
  ra_eswm()->ESWM_STS = 0xDEADBEEFU;
  uint32_t mask       = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_eth_get_status(&mask));
  TEST_ASSERT_EQ((int32_t)0xDEADBEEFU, (int32_t)mask);
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_eth_clear_status(0xF0F0F0F0U));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_eth_get_status(nullptr));
  TEST_END("eth status read + clear");
}

static void test_attach_and_dispatch(void)
{
  TEST_BEGIN("eth attach + dispatch");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_eth_attach_handler(stub_eth_cb, (void*)(uintptr_t)0xE0U));
  ra_eswm()->ESWM_STS = 0xCAFEU;
  ra_eth_dispatch();
  TEST_ASSERT_EQ((int32_t)1, (int32_t)s_eth_cb_count);
  TEST_ASSERT_EQ((int32_t)0xCAFEU, (int32_t)s_eth_cb_last_mask);

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_eth_attach_handler(nullptr, nullptr));
  ra_eswm()->ESWM_STS = 0xBABEU;
  ra_eth_dispatch();
  TEST_ASSERT_EQ((int32_t)1, (int32_t)s_eth_cb_count);
  TEST_END("eth attach + dispatch");
}

static void test_power_transition(void)
{
  TEST_BEGIN("eth power transition");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_eth_init());
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_eth_enter_stop());
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_eth_exit_stop());
  TEST_END("eth power transition");
}

int32_t main(void)
{
  test_init();
  test_deinit();
  test_status_read_and_clear();
  test_attach_and_dispatch();
  test_power_transition();
  (void)fprintf(stderr, "[OK  ] test_ra_eth.c\n");
  return 0;
}
