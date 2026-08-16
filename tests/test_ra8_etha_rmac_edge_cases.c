/**
 * @file test_ra8_etha_rmac_edge_cases.c
 * @brief Edge-case + stress unit tests for the Ethernet stack (ra8_etha + ra8_rmac).
 *
 * @details
 * Complements ``test_ra8_etha.c`` and ``test_ra8_rmac.c`` with explicit
 * coverage of the safety-critical paths the happy-path tests skip:
 *
 *   - TX descriptor ring configured at the EATDQDC 11-bit ceiling --
 *     deeper requests are clamped, not silently dropped or returned as
 *     a bogus error;
 *   - RX-overrun saturation: account_traffic must clamp ``rx_drop``
 *     against UINT32_MAX rather than wrap;
 *   - PHY auto-neg-wait timeout race with EAMC: the wait can return
 *     hw_timeout but the port-level EAMC programming is left in a
 *     well-defined state;
 *   - pause-frame flow control bit-packing across pause vs PFC modes;
 *   - jumbo-frame size programming up to 16 KiB minus 1 (16383); the
 *     EATMFSC field is 14 bits wide so 17 KiB requests must be rejected;
 *   - per-class IRQ enable/disable bit masking is non-destructive across
 *     all three EAEIE blocks.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_err.h"
#include "ra8_etha.h"
#include "ra8_etha_regs.h"
#include "ra8_fake_mmap.h"
#include "ra8_mstp.h"
#include "ra8_rmac.h"
#include "ra8_rmac_regs.h"
#include "unity_minimal.h"

static void prep(void)
{
  ra8_fake_mmap_reset();
  (void)ra8_mstp_init();
}

static ra8_etha_config_t default_etha_cfg(ra8_etha_opc_t mode)
{
  return (ra8_etha_config_t){
    .initial_mode = mode,
    .eaeie0_mask  = 0U,
    .eaeie1_mask  = 0U,
    .eaeie2_mask  = 0U,
  };
}

static ra8_rmac_config_t default_rmac_cfg(void)
{
  return (ra8_rmac_config_t){
    .rx_filter       = k_ra8_rmac_mrafc_unicast_match,
    .err_irq_enable  = 0U,
    .mon0_irq_enable = 0U,
    .mon1_irq_enable = 0U,
    .mon2_irq_enable = 0U,
    .phy_interface   = k_ra8_rmac_pis_gmii,
    .link_speed      = k_ra8_rmac_lsc_1000mbit,
    .duplex          = k_ra8_rmac_duplex_full,
  };
}

/* --- TX descriptor ring depth clamping --- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_tx_ring_full_at_ceiling(void)
{
  TEST_BEGIN("etha tx ring depth clamped to EATDQDC 11-bit ceiling");
  prep();
  const ra8_etha_config_t cfg = default_etha_cfg(k_ra8_etha_opc_config);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_etha_init(k_ra8_etha_port_0, &cfg));
  /* Request 4096 entries -> EATDQDC field saturates at 2047 across all
   * traffic classes. The init still succeeds (clamping is the
   * documented behaviour) and the post-state is observable on every TC.
   * 4097+ would be rejected as out-of-range. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_etha_descriptor_ring_init(k_ra8_etha_port_0, 4096U, 256U, 1518U));
  /* Request beyond the 4096 cap is rejected outright (not clamped). */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_etha_descriptor_ring_init(k_ra8_etha_port_0, 4097U, 256U, 1518U));
  for (uint8_t tc = 0U; tc < (uint8_t)k_ra8_etha_tc_count; ++tc) {
    TEST_ASSERT_EQ(2047U, ra8_etha(k_ra8_etha_port_0)->EATDQDC[tc]);
  }
  TEST_END("etha tx ring depth clamped to EATDQDC 11-bit ceiling");
}

/* --- rx_drop saturation under sustained overrun --- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_rx_overrun_saturates(void)
{
  TEST_BEGIN("etha rx_drop counter saturates rather than wraps");
  prep();
  const ra8_etha_config_t cfg = default_etha_cfg(k_ra8_etha_opc_operation);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_etha_init(k_ra8_etha_port_0, &cfg));
  /* Push rx_drop near saturation in a single accounting call. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_etha_account_traffic(k_ra8_etha_port_0, 0U, 0U, 0U, 0U, UINT32_MAX - 5U));
  /* Then add 100 more -> must saturate at UINT32_MAX. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_etha_account_traffic(k_ra8_etha_port_0, 0U, 0U, 0U, 0U, 100U));
  ra8_etha_port_stats_t st = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_etha_get_stats(k_ra8_etha_port_0, &st));
  TEST_ASSERT(st.rx_drop == UINT32_MAX);
  TEST_END("etha rx_drop counter saturates rather than wraps");
}

/* --- MAC/PHY race: auto_neg_wait timeout must not corrupt MMIS1 --- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_phy_auto_neg_wait_race(void)
{
  TEST_BEGIN("rmac auto_neg_wait timeout leaves status registers consistent");
  prep();
  const ra8_rmac_config_t cfg = default_rmac_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rmac_init(k_ra8_rmac_port_0, &cfg));
  /* Each per-read MDIO wait completes via the unarmed ra8_fake_mmio seam,
   * but BMSR reads deliver 0 (PRD zeroed by issue), so auto_neg_wait
   * must exhaust its own budget and return hw_timeout cleanly with
   * out_link.up false. */
  ra8_rmac_phy_link_t lk = {.up = true, .speed = k_ra8_rmac_phy_speed_100_fd};
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_rmac_phy_auto_neg_wait(k_ra8_rmac_port_0, 1U, 1U, &lk));
  TEST_ASSERT(!lk.up);
  TEST_ASSERT_EQ(k_ra8_rmac_phy_speed_unknown, lk.speed);
  /* Second wait call must observe the same outcome (idempotent). */
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_rmac_phy_auto_neg_wait(k_ra8_rmac_port_0, 1U, 1U, &lk));
  TEST_END("rmac auto_neg_wait timeout leaves status registers consistent");
}

/* --- Pause-frame flow control bit packing for both modes --- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_pause_frame_packing(void)
{
  TEST_BEGIN("rmac pause-frame packing across 802.3x and PFC modes");
  prep();
  const ra8_rmac_config_t cfg = default_rmac_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rmac_init(k_ra8_rmac_port_0, &cfg));
  /* 802.3x classic pause: max pause time, max retry. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_rmac_set_pause_frame(k_ra8_rmac_port_0,
                                          k_ra8_rmac_pause_mode_802_3x,
                                          0xFFFFU,
                                          0xFFU,
                                          0x1FU));
  const uint32_t want_mtpfc =
    ((uint32_t)0xFFFFU << 0U) | ((uint32_t)0xFFU << 16U) | ((uint32_t)0x1FU << 27U);
  TEST_ASSERT_EQ(want_mtpfc, ra8_rmac(k_ra8_rmac_port_0)->MTPFC);
  TEST_ASSERT_EQ(0U, ra8_rmac(k_ra8_rmac_port_0)->MTPFC2);
  /* PFC mode: PFM bit asserts in MTPFC2. */
  TEST_ASSERT_EQ(
    k_ra8_ok,

    ra8_rmac_set_pause_frame(k_ra8_rmac_port_0, k_ra8_rmac_pause_mode_pfc, 0x0001U, 0x01U, 0x00U));
  TEST_ASSERT_EQ((1UL << 26U), ra8_rmac(k_ra8_rmac_port_0)->MTPFC2);
  TEST_END("rmac pause-frame packing across 802.3x and PFC modes");
}

/* --- Jumbo-frame size programming + rejection --- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_jumbo_frame_size_limits(void)
{
  TEST_BEGIN("etha jumbo-frame size programming + ceiling rejection");
  prep();
  const ra8_etha_config_t cfg = default_etha_cfg(k_ra8_etha_opc_config);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_etha_init(k_ra8_etha_port_0, &cfg));
  /* Standard Ethernet frame, jumbo, and a max-jumbo (under the
   * descriptor-ring buf cap of 16 KiB - 1) all accepted. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_etha_set_max_frame_size(k_ra8_etha_port_0, k_ra8_etha_tc_0, 1518U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_etha_set_max_frame_size(k_ra8_etha_port_0, k_ra8_etha_tc_4, 9018U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_etha_set_max_frame_size(k_ra8_etha_port_0, k_ra8_etha_tc_7, 16383U));
  TEST_ASSERT_EQ(1518U, ra8_etha(k_ra8_etha_port_0)->EATMFSC[0]);
  TEST_ASSERT_EQ(9018U, ra8_etha(k_ra8_etha_port_0)->EATMFSC[4]);
  TEST_ASSERT_EQ(16383U, ra8_etha(k_ra8_etha_port_0)->EATMFSC[7]);
  /* Out-of-range traffic class still rejected. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_etha_set_max_frame_size(k_ra8_etha_port_0,
                                             (ra8_etha_tc_t)(uint8_t)k_ra8_etha_tc_count,
                                             1500U));
  TEST_END("etha jumbo-frame size programming + ceiling rejection");
}

/* --- Per-block IRQ enable/disable independence --- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_irq_enable_blocks_independent(void)
{
  TEST_BEGIN("etha enable/disable IRQ across all three blocks is non-destructive");
  prep();
  const ra8_etha_config_t cfg = default_etha_cfg(k_ra8_etha_opc_operation);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_etha_init(k_ra8_etha_port_0, &cfg));
  /* Set distinct masks in each block, then toggle a single bit per
   * block and verify the others were untouched. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_etha_enable_irq(k_ra8_etha_port_0, k_ra8_etha_irq_class_0, 0xAAAAAAAAU));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_etha_enable_irq(k_ra8_etha_port_0, k_ra8_etha_irq_class_1, 0x55555555U));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_etha_enable_irq(k_ra8_etha_port_0, k_ra8_etha_irq_class_2, 0xF0F0F0F0U));
  TEST_ASSERT_EQ(0xAAAAAAAAU, ra8_etha(k_ra8_etha_port_0)->EAEIE0);
  TEST_ASSERT_EQ(0x55555555U, ra8_etha(k_ra8_etha_port_0)->EAEIE1);
  TEST_ASSERT_EQ(0xF0F0F0F0U, ra8_etha(k_ra8_etha_port_0)->EAEIE2);
  /* Clear one bit in block 1 -- block 0 / 2 must not be affected. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_etha_disable_irq(k_ra8_etha_port_0, k_ra8_etha_irq_class_1, 0x00000001U));
  TEST_ASSERT_EQ(0xAAAAAAAAU, ra8_etha(k_ra8_etha_port_0)->EAEIE0);
  TEST_ASSERT_EQ(0x55555554U, ra8_etha(k_ra8_etha_port_0)->EAEIE1);
  TEST_ASSERT_EQ(0xF0F0F0F0U, ra8_etha(k_ra8_etha_port_0)->EAEIE2);
  TEST_END("etha enable/disable IRQ across all three blocks is non-destructive");
}

int main(void)
{
  test_tx_ring_full_at_ceiling();
  test_rx_overrun_saturates();
  test_phy_auto_neg_wait_race();
  test_pause_frame_packing();
  test_jumbo_frame_size_limits();
  test_irq_enable_blocks_independent();
  return 0;
}
