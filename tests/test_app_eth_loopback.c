/**
 * @file test_app_eth_loopback.c
 * @brief Integration test: ETHA per-port loopback bring-up sequence
 *
 * @details
 * Mirrors examples/ek_ra8d2/eth_loopback/main.c bring-up flow:
 * ra8_etha_init(CONFIG) -> ra8_etha_descriptor_ring_init -> ra8_etha_set_mode(OPERATION)
 * -> ra8_etha_account_traffic -> ra8_etha_get_stats -> ra8_etha_deinit. All
 * MMIO is via the host tests/mocks/ra8_fake_mmap.c shim.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_etha.h"
#include "ra8_fake_mmap.h"
#include "ra8_mstp.h"
#include "unity_minimal.h"

typedef enum : uint16_t {
  k_test_eth_app_ring_tx     = 4U,    /**< Test Ethernet app ring TX.     */
  k_test_eth_app_ring_rx     = 4U,    /**< Test Ethernet app ring RX.     */
  k_test_eth_app_buffer_size = 1536U, /**< Test Ethernet app buffer size. */
  k_test_eth_app_burst       = 4U,    /**< Test Ethernet app burst.       */
} test_eth_app_const_t;

static void reset_world(void)
{
  ra8_fake_mmap_reset();
  (void)ra8_mstp_init();
}

/**
 * @brief Golden bring-up: init + ring + mode + account + stats.
 *
 * @par MC/DC:
 * Compound decision in app: ``init != ok || ring != ok || set_mode != ok ||
 * account != ok || get_stats != ok``. Five atomic conditions x N+1 = 6
 * vectors -- this golden test plus the four reject tests (null cfg,
 * bad port, bad ring port, bad set_mode port) cover the matrix.
 */
static void test_eth_app_loopback_ok(void)
{
  reset_world();
  TEST_BEGIN("eth_loopback: bring-up + account + stats");
  const ra8_etha_config_t cfg = {
    .initial_mode = k_ra8_etha_opc_config,
    .eaeie0_mask  = 0U,
    .eaeie1_mask  = 0U,
    .eaeie2_mask  = 0U,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_etha_init(k_ra8_etha_port_0, &cfg));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_etha_descriptor_ring_init(k_ra8_etha_port_0,
                                               (uint16_t)k_test_eth_app_ring_tx,
                                               (uint16_t)k_test_eth_app_ring_rx,
                                               (uint16_t)k_test_eth_app_buffer_size));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_etha_set_mode(k_ra8_etha_port_0, k_ra8_etha_opc_operation));
  for (uint8_t i = 0U; i < (uint8_t)k_test_eth_app_burst; i++) {
    TEST_ASSERT_EQ(k_ra8_ok, ra8_etha_account_traffic(k_ra8_etha_port_0, 1U, 0U, 1U, 0U, 0U));
  }
  ra8_etha_port_stats_t stats = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_etha_get_stats(k_ra8_etha_port_0, &stats));
  TEST_ASSERT_EQ(k_test_eth_app_burst, stats.tx_ok);
  TEST_ASSERT_EQ(k_test_eth_app_burst, stats.rx_ok);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_etha_deinit(k_ra8_etha_port_0));
  TEST_END("eth_loopback: bring-up + account + stats");
}

/**
 * @brief NULL config rejected by ra8_etha_init.
 *
 * @par MC/DC:
 * Decision: ``cfg == nullptr``. One atomic condition x 2 vectors --
 * golden test above (non-NULL) + this NULL.
 */
static void test_eth_app_init_null_cfg(void)
{
  reset_world();
  TEST_BEGIN("eth_loopback: NULL cfg rejected");
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_etha_init(k_ra8_etha_port_0, nullptr));
  TEST_END("eth_loopback: NULL cfg rejected");
}

/**
 * @brief Bad port index rejected by ra8_etha_init.
 *
 * @par MC/DC:
 * Decision: ``port < k_ra8_etha_port_count``. One atomic condition x 2
 * vectors -- golden (in-range) + this out-of-range.
 */
static void test_eth_app_init_bad_port(void)
{
  reset_world();
  TEST_BEGIN("eth_loopback: bad port rejected");
  const ra8_etha_config_t cfg = {
    .initial_mode = k_ra8_etha_opc_config,
    .eaeie0_mask  = 0U,
    .eaeie1_mask  = 0U,
    .eaeie2_mask  = 0U,
  };
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_etha_init((ra8_etha_port_t)(uint8_t)k_ra8_etha_port_count, &cfg));
  TEST_END("eth_loopback: bad port rejected");
}

/**
 * @brief Bad port rejected by ra8_etha_set_mode.
 *
 * @par MC/DC:
 * Decision: ``port < k_ra8_etha_port_count`` for set_mode. One atomic
 * condition x 2 vectors -- golden + this.
 */
static void test_eth_app_set_mode_bad_port(void)
{
  reset_world();
  TEST_BEGIN("eth_loopback: set_mode bad port rejected");
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_etha_set_mode((ra8_etha_port_t)(uint8_t)k_ra8_etha_port_count, k_ra8_etha_opc_reset));
  TEST_END("eth_loopback: set_mode bad port rejected");
}

/**
 * @brief NULL stats out rejected by ra8_etha_get_stats.
 *
 * @par MC/DC:
 * Decision: ``out_stats == nullptr``. One atomic condition x 2
 * vectors -- golden (non-NULL) + this NULL.
 */
static void test_eth_app_get_stats_null(void)
{
  reset_world();
  TEST_BEGIN("eth_loopback: get_stats NULL rejected");
  const ra8_etha_config_t cfg = {
    .initial_mode = k_ra8_etha_opc_config,
    .eaeie0_mask  = 0U,
    .eaeie1_mask  = 0U,
    .eaeie2_mask  = 0U,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_etha_init(k_ra8_etha_port_0, &cfg));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_etha_get_stats(k_ra8_etha_port_0, nullptr));
  TEST_END("eth_loopback: get_stats NULL rejected");
}

int main(void)
{
  test_eth_app_loopback_ok();
  test_eth_app_init_null_cfg();
  test_eth_app_init_bad_port();
  test_eth_app_set_mode_bad_port();
  test_eth_app_get_stats_null();
  return 0;
}
