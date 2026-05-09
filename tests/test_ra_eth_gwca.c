/**
 * @file test_ra_eth_gwca.c
 * @brief Unit tests for ra_eth_gwca.c (GWCA sub-driver)
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8d2_ether_regs.h"
#include "ra_err.h"
#include "ra_eth_gwca.h"
#include "ra_mstp.h"
#include "ra_sim_mmap.h"
#include "unity_minimal.h"

static uint32_t s_gwca_cb_count;
static uint32_t s_gwca_cb_last_mask;

static void stub_gwca_cb(void* ctx, uint32_t mask)
{
  (void)ctx;
  ++s_gwca_cb_count;
  s_gwca_cb_last_mask = mask;
}

static void prep(void)
{
  ra_sim_mmap_reset();
  (void)ra_mstp_init();
  s_gwca_cb_count     = 0U;
  s_gwca_cb_last_mask = 0U;
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init(void)
{
  TEST_BEGIN("gwca init");
  prep();
  TEST_ASSERT_EQ(k_ra_ok, ra_eth_gwca_init());
  TEST_END("gwca init");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_deinit(void)
{
  TEST_BEGIN("gwca deinit");
  prep();
  TEST_ASSERT_EQ(k_ra_ok, ra_eth_gwca_init());
  TEST_ASSERT_EQ(k_ra_ok, ra_eth_gwca_deinit());
  TEST_END("gwca deinit");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_status_read_and_clear(void)
{
  TEST_BEGIN("gwca status read + clear");
  prep();
  ra_gwca()->GWCA_STS = 0x9ABCDEF0U;
  uint32_t mask       = 0U;
  TEST_ASSERT_EQ(k_ra_ok, ra_eth_gwca_get_status(&mask));
  TEST_ASSERT_EQ(0x9ABCDEF0U, mask);
  TEST_ASSERT_EQ(k_ra_ok, ra_eth_gwca_clear_status(0xFF00FF00U));
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_eth_gwca_get_status(nullptr));
  TEST_END("gwca status read + clear");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_attach_and_dispatch(void)
{
  TEST_BEGIN("gwca attach + dispatch");
  prep();
  TEST_ASSERT_EQ(k_ra_ok, ra_eth_gwca_attach_handler(stub_gwca_cb, (void*)(uintptr_t)0xB0U));
  ra_gwca()->GWCA_STS = 0x4321U;
  ra_eth_gwca_dispatch();
  TEST_ASSERT_EQ(1, s_gwca_cb_count);
  TEST_ASSERT_EQ(0x4321U, s_gwca_cb_last_mask);

  TEST_ASSERT_EQ(k_ra_ok, ra_eth_gwca_attach_handler(nullptr, nullptr));
  ra_gwca()->GWCA_STS = 0x8765U;
  ra_eth_gwca_dispatch();
  TEST_ASSERT_EQ(1, s_gwca_cb_count);
  TEST_END("gwca attach + dispatch");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_power_transition(void)
{
  TEST_BEGIN("gwca power transition");
  prep();
  TEST_ASSERT_EQ(k_ra_ok, ra_eth_gwca_init());
  TEST_ASSERT_EQ(k_ra_ok, ra_eth_gwca_enter_stop());
  TEST_ASSERT_EQ(k_ra_ok, ra_eth_gwca_exit_stop());
  TEST_END("gwca power transition");
}

int32_t main(void)
{
  test_init();
  test_deinit();
  test_status_read_and_clear();
  test_attach_and_dispatch();
  test_power_transition();
  (void)fprintf(stderr, "[OK  ] test_ra_eth_gwca.c\n");
  return 0;
}
