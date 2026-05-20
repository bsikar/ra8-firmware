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

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_operation_mode(void)
{
  TEST_BEGIN("gwca set_operation_mode");
  prep();
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_eth_gwca_set_operation_mode((ra_gwmc_opc_t)0xFFU));
  TEST_ASSERT_EQ(k_ra_ok, ra_eth_gwca_set_operation_mode(k_ra_gwmc_opc_reset));
  TEST_ASSERT_EQ(k_ra_ok, ra_eth_gwca_set_operation_mode(k_ra_gwmc_opc_config));
  TEST_ASSERT_EQ(k_ra_ok, ra_eth_gwca_set_operation_mode(k_ra_gwmc_opc_operation));
  TEST_END("gwca set_operation_mode");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_axi_init(void)
{
  TEST_BEGIN("gwca axi_init");
  prep();
  TEST_ASSERT_EQ(k_ra_ok, ra_eth_gwca_axi_init());
  TEST_END("gwca axi_init");
}

/**
 * @test test_mcdc_install_linkfix_count_range
 *
 * @par MC/DC:
 * Decision (libs/ra_hal/src/ra_eth_gwca.c:260): // CITES-OK: MC/DC gate requires file:line
 *   ``if (entry_count == 0U || entry_count > k_ra_gwca_linkfix_max_entries)``
 * Two atomic conditions, N+1 = 3 vectors:
 *   V_F_F: count = 8   -> false || false  (returns ok)
 *   V_T_-: count = 0   -> true  || (n/a)  (varies "count==0" alone)
 *   V_F_T: count = 33  -> false || true   (varies "count>max" alone)
 * Vectors V_F_F + V_T_- prove "count==0" independently affects the
 * outcome; V_F_F + V_F_T prove the same for "count>max". Minimal
 * MC/DC vector set for the validation guard.
 */
static void test_install_linkfix(void)
{
  TEST_BEGIN("gwca install_linkfix");
  prep();
  __attribute__((aligned(16))) static ra_gwca_basic_descriptor_t table[8];

  /* Null table rejected (separate RA_CHECK_NULL_PTR guard). */
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_eth_gwca_install_linkfix(nullptr, 8U));

  /* Vector 2: count = 0 -> invalid_arg. */
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_eth_gwca_install_linkfix(table, 0U));

  /* Vector 3: count = 33 -> invalid_arg. */
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_eth_gwca_install_linkfix(table, 33U));

  /* Vector 1: count = 8 -> ok, every entry LEMPTY. */
  for (uint32_t i = 0U; i < 8U; ++i) {
    table[i].dt = 0xFU;
  }
  TEST_ASSERT_EQ(k_ra_ok, ra_eth_gwca_install_linkfix(table, 8U));
  for (uint32_t i = 0U; i < 8U; ++i) {
    TEST_ASSERT_EQ(k_ra_gwdcc_dt_lempty, table[i].dt);
  }

  TEST_END("gwca install_linkfix");
}

int32_t main(void)
{
  test_init();
  test_deinit();
  test_status_read_and_clear();
  test_attach_and_dispatch();
  test_power_transition();
  test_set_operation_mode();
  test_axi_init();
  test_install_linkfix();
  (void)fprintf(stderr, "[OK  ] test_ra_eth_gwca.c\n");
  return 0;
}
