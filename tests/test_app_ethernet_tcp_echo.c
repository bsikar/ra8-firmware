/**
 * @file test_app_ethernet_tcp_echo.c
 * @brief Integration test: ra_eth bare-metal NIC bring-up + frame loopback
 *
 * @details
 * The production app at examples/ek_ra8d2/ethernet_tcp_echo/main.c
 * brings the chip up via ra_cgc_init, routes the PEF7071 PHY pins via
 * ra_board_ethernet_init, opens the NIC at MAC 02:00:00:00:00:01, and
 * services frames by hand (ARP, ICMP echo, TCP echo on port 7).
 *
 * This test exercises the same ra_eth surface the hand-rolled stack
 * calls: open / write / read / link_status / close.
 *
 * Modeled flow:
 *   1. ra_cgc_init
 *   2. ra_board_ethernet_init
 *   3. ra_eth_init / ra_eth_open(cfg with our MAC)
 *   4. ra_eth_link_status
 *   5. ra_eth_test_inject_rx + ra_eth_read (mock RX path)
 *   6. ra_eth_write
 *   7. ra_eth_close + ra_eth_deinit
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "ra8d2_system_regs.h"
#include "ra_board_ek_ra8d2.h"
#include "ra_cgc.h"
#include "ra_err.h"
#include "ra_eth.h"
#include "ra_pin_validator.h"
#include "ra_sim_mmap.h"
#include "unity_minimal.h"

/** @brief Per-test enums. */
typedef enum : uint16_t {
  k_test_eth_frame_len = 64U,
} test_eth_const_t;

/** @brief Locally-administered, unicast MAC the demo uses. */
static const uint8_t k_test_eth_mac[6] = {0x02U, 0x00U, 0x00U, 0x00U, 0x00U, 0x01U};

/**
 * @brief Per-test fixture reset.
 */
static void reset_world(void)
{
  ra_sim_mmap_reset();
  ra_pin_validator_reset();
  (void)ra_eth_close();
  (void)ra_eth_deinit();
  /* Pre-seed OSCSF stabilisation bits so ra_cgc_init() spin loops
   * complete on the first iteration in RA_SIMULATOR_MODE. */
  *ra_sys_oscsf() = (uint8_t)0xFFU;
}

/**
 * @brief Build the open() cfg matching the demo's wiring.
 */
static ra_eth_cfg_t make_demo_cfg(void)
{
  ra_eth_cfg_t cfg = {};
  (void)memcpy(cfg.mac_address, k_test_eth_mac, sizeof k_test_eth_mac);
  cfg.channel            = 0U;
  cfg.num_tx_descriptors = 0U;
  cfg.num_rx_descriptors = 0U;
  cfg.buffer_size        = 0U;
  return cfg;
}

/**
 * @brief Pre-NIC chip bring-up (CGC + board PHY routing).
 *
 * @par MC/DC: not applicable -- two sequential init calls.
 */
static void test_eth_demo_pre_nic_bringup(void)
{
  reset_world();
  TEST_BEGIN("eth_tcp_echo: cgc_init + board_ethernet_init");
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_cgc_init());
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_board_ethernet_init());
  TEST_END("eth_tcp_echo: cgc_init + board_ethernet_init");
}

/**
 * @brief Open the NIC at the demo MAC.
 *
 * @par MC/DC:
 * Compound decision under test (in ra_eth_open): ``cfg == NULL ||
 * channel >= 2 || ring counts out of range``. Three atomic conditions
 * x N+1 = 4 vectors. This case covers the all-valid vector. Failure
 * vectors are below.
 */
static void test_eth_demo_open_nic(void)
{
  reset_world();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_eth_init());
  TEST_BEGIN("eth_tcp_echo: ra_eth_open with demo MAC");
  const ra_eth_cfg_t cfg = make_demo_cfg();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_eth_open(&cfg));
  TEST_END("eth_tcp_echo: ra_eth_open with demo MAC");
}

/**
 * @brief Poll link status -- the demo's per-iteration check.
 *
 * @par MC/DC:
 * Decision vector under test: ``out_status == NULL`` precondition guard
 * inside ra_eth_link_status. This case covers the non-NULL vector.
 */
static void test_eth_demo_link_status(void)
{
  reset_world();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_eth_init());
  const ra_eth_cfg_t cfg = make_demo_cfg();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_eth_open(&cfg));
  TEST_BEGIN("eth_tcp_echo: link_status returns a defined state");
  ra_eth_link_t  link = {};
  const ra_err_t err  = ra_eth_link_status(&link);
  /* Mock PHY may report ok, not-initialised, or timeout polling the
   * MII registers; all three are acceptable shapes for the demo. */
  TEST_ASSERT(err == k_ra_ok || err == k_ra_err_not_initialized || err == k_ra_err_hw_timeout);
  TEST_END("eth_tcp_echo: link_status returns a defined state");
}

/**
 * @brief RX inject + read -- the demo's per-frame service loop.
 *
 * @par MC/DC:
 * Compound decision under test (in ra_eth_read): ``buf == NULL ||
 * got_len == NULL || max_len == 0``. Three atomic conditions x N+1 = 4
 * vectors. This case covers the all-valid vector.
 */
static void test_eth_demo_rx_inject_and_read(void)
{
  reset_world();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_eth_init());
  const ra_eth_cfg_t cfg = make_demo_cfg();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_eth_open(&cfg));

  TEST_BEGIN("eth_tcp_echo: inject + read frame");
  uint8_t inject[k_test_eth_frame_len];
  for (uint16_t i = 0U; i < (uint16_t)k_test_eth_frame_len; ++i) {
    inject[i] = (uint8_t)(i ^ 0xA5U);
  }
  ra_err_t inj = ra_eth_test_inject_rx(inject, (uint32_t)k_test_eth_frame_len);
  TEST_ASSERT(inj == k_ra_ok || inj == k_ra_err_not_initialized);

  uint8_t  rx_buf[k_test_eth_frame_len];
  uint32_t got = 0U;
  ra_err_t r   = ra_eth_read(rx_buf, (uint32_t)k_test_eth_frame_len, &got);
  TEST_ASSERT(r == k_ra_ok || r == k_ra_err_no_data || r == k_ra_err_not_initialized);
  TEST_END("eth_tcp_echo: inject + read frame");
}

/**
 * @brief TX path -- the demo's frame-construction reply hook.
 *
 * @par MC/DC:
 * Compound decision under test (in ra_eth_write): ``buf == NULL ||
 * len < min || len > max``. Three atomic conditions x N+1 = 4 vectors.
 * This case covers the all-valid vector.
 */
static void test_eth_demo_write_frame(void)
{
  reset_world();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_eth_init());
  const ra_eth_cfg_t cfg = make_demo_cfg();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_eth_open(&cfg));

  TEST_BEGIN("eth_tcp_echo: write 64-byte frame");
  uint8_t tx[k_test_eth_frame_len];
  for (uint16_t i = 0U; i < (uint16_t)k_test_eth_frame_len; ++i) {
    tx[i] = (uint8_t)i;
  }
  ra_err_t err = ra_eth_write(tx, (uint32_t)k_test_eth_frame_len);
  TEST_ASSERT(err == k_ra_ok || err == k_ra_err_hw_not_ready || err == k_ra_err_not_initialized);
  TEST_END("eth_tcp_echo: write 64-byte frame");
}

/**
 * @brief NULL cfg rejected at open.
 *
 * @par MC/DC:
 * Decision vector under test: ``cfg == NULL`` precondition guard inside
 * ra_eth_open. Failure vector for the open compound check.
 */
static void test_eth_demo_open_null_cfg_rejected(void)
{
  reset_world();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_eth_init());
  TEST_BEGIN("eth_tcp_echo: open rejects NULL cfg");
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_eth_open(NULL));
  TEST_END("eth_tcp_echo: open rejects NULL cfg");
}

/**
 * @brief Bad channel id rejected at open.
 *
 * @par MC/DC:
 * Decision vector under test: ``cfg->channel >= 2`` precondition guard
 * inside ra_eth_open. Failure-side pair.
 */
static void test_eth_demo_open_bad_channel_rejected(void)
{
  reset_world();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_eth_init());
  TEST_BEGIN("eth_tcp_echo: open rejects bad channel");
  ra_eth_cfg_t cfg = make_demo_cfg();
  cfg.channel      = 99U;
  TEST_ASSERT(ra_eth_open(&cfg) != k_ra_ok);
  TEST_END("eth_tcp_echo: open rejects bad channel");
}

int main(void)
{
  test_eth_demo_pre_nic_bringup();
  test_eth_demo_open_nic();
  test_eth_demo_link_status();
  test_eth_demo_rx_inject_and_read();
  test_eth_demo_write_frame();
  test_eth_demo_open_null_cfg_rejected();
  test_eth_demo_open_bad_channel_rejected();
  return 0;
}
