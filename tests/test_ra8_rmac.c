/**
 * @file test_ra8_rmac.c
 * @brief Unit tests for ra8_rmac.c (per-port Ethernet MAC driver, full coverage)
 *
 * @details MAC-core public-API contract tests (init, filters, link
 * modes, pause/PFC, stats, dispatch, lifecycle). The MDIO/PHY tests and
 * their MC/DC vector suites live in the sibling test_ra8_rmac_mdio.c.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_err.h"
#include "ra8_mstp.h"
#include "ra8_rmac.h"
#include "ra8_rmac_regs.h"
#include "ra8_sim_mmap.h"
#include "ra8_sim_mmio.h"
#include "unity_minimal.h"

static uint32_t        s_cb_count;
static uint32_t        s_cb_last_err;
static uint32_t        s_cb_last_m0;
static uint32_t        s_cb_last_m1;
static uint32_t        s_cb_last_m2;
static ra8_rmac_port_t s_cb_last_port;

static void
stub_cb(void* ctx, ra8_rmac_port_t port, uint32_t err, uint32_t m0, uint32_t m1, uint32_t m2)
{
  (void)ctx;
  ++s_cb_count;
  s_cb_last_err  = err;
  s_cb_last_m0   = m0;
  s_cb_last_m1   = m1;
  s_cb_last_m2   = m2;
  s_cb_last_port = port;
}

static void prep(void)
{
  ra8_sim_mmap_reset();
  ra8_sim_mmio_reset();
  (void)ra8_mstp_init();
  s_cb_count     = 0U;
  s_cb_last_err  = 0U;
  s_cb_last_m0   = 0U;
  s_cb_last_m1   = 0U;
  s_cb_last_m2   = 0U;
  s_cb_last_port = k_ra8_rmac_port_0;
}

static ra8_rmac_config_t default_cfg(void)
{
  return (ra8_rmac_config_t){
    .rx_filter       = k_ra8_rmac_mrafc_unicast_match,
    .err_irq_enable  = 0xCAFEBABEU,
    .mon0_irq_enable = 0x00001FFFU,
    .mon1_irq_enable = 0x0000000FU,
    .mon2_irq_enable = 0x00000007U,
    .phy_interface   = k_ra8_rmac_pis_gmii,
    .link_speed      = k_ra8_rmac_lsc_1000mbit,
    .duplex          = k_ra8_rmac_duplex_full,
  };
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_happy(void)
{
  TEST_BEGIN("rmac init happy");
  prep();
  const ra8_rmac_config_t cfg = default_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rmac_init(k_ra8_rmac_port_0, &cfg));
  TEST_ASSERT_EQ(k_ra8_rmac_mrafc_unicast_match, ra8_rmac(k_ra8_rmac_port_0)->MRAFC);
  TEST_ASSERT_EQ(0xCAFEBABEU, ra8_rmac(k_ra8_rmac_port_0)->MEIE);
  TEST_ASSERT_EQ(0x00001FFFU, ra8_rmac(k_ra8_rmac_port_0)->MMIE0);
  TEST_ASSERT_EQ(0x0000000FU, ra8_rmac(k_ra8_rmac_port_0)->MMIE1);
  TEST_ASSERT_EQ(0x00000007U, ra8_rmac(k_ra8_rmac_port_0)->MMIE2);
  TEST_END("rmac init happy");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_null_cfg(void)
{
  TEST_BEGIN("rmac init null cfg");
  prep();
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_rmac_init(k_ra8_rmac_port_0, nullptr));
  TEST_END("rmac init null cfg");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_bad_port(void)
{
  TEST_BEGIN("rmac init bad port");
  prep();
  const ra8_rmac_config_t cfg = default_cfg();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_rmac_init((ra8_rmac_port_t)(uint8_t)k_ra8_rmac_port_count, &cfg));
  TEST_END("rmac init bad port");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_bad_interface(void)
{
  TEST_BEGIN("rmac init bad iface");
  prep();
  ra8_rmac_config_t cfg = default_cfg();
  /* MPIC.PIS has only MII (0) and GMII (2) defined; 1 is Reserved. */
  cfg.phy_interface = (ra8_rmac_pis_t)(uint8_t)1U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_rmac_init(k_ra8_rmac_port_0, &cfg));
  TEST_END("rmac init bad iface");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_bad_speed(void)
{
  TEST_BEGIN("rmac init bad speed");
  prep();
  ra8_rmac_config_t cfg = default_cfg();
  cfg.link_speed        = (ra8_rmac_lsc_t)(uint8_t)k_ra8_rmac_lsc_count;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_rmac_init(k_ra8_rmac_port_0, &cfg));
  TEST_END("rmac init bad speed");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_mac_address(void)
{
  TEST_BEGIN("rmac set mac address");
  prep();
  const ra8_rmac_config_t cfg = default_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rmac_init(k_ra8_rmac_port_1, &cfg));
  const uint8_t mac[k_ra8_rmac_mac_byte_count] = {0x12U, 0x34U, 0x56U, 0x78U, 0x9AU, 0xBCU};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rmac_set_mac_address(k_ra8_rmac_port_1, mac));

  /* MRMAC0.MAU holds high 16 bits with byte0 in [15:8]. */
  const uint32_t expected_mrmac0 = ((uint32_t)0x12U << 8U) | (uint32_t)0x34U;
  /* MRMAC1.MAL holds low 32 bits with mac[2] in [31:24]. */
  const uint32_t expected_mrmac1 =
    ((uint32_t)0x56U << 24U) | ((uint32_t)0x78U << 16U) | ((uint32_t)0x9AU << 8U) | (uint32_t)0xBCU;
  TEST_ASSERT_EQ(expected_mrmac0, ra8_rmac(k_ra8_rmac_port_1)->MRMAC0);
  TEST_ASSERT_EQ(expected_mrmac1, ra8_rmac(k_ra8_rmac_port_1)->MRMAC1);

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_rmac_set_mac_address(k_ra8_rmac_port_1, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_rmac_set_mac_address((ra8_rmac_port_t)(uint8_t)k_ra8_rmac_port_count, mac));
  TEST_END("rmac set mac address");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_rx_filter(void)
{
  TEST_BEGIN("rmac set rx filter");
  prep();
  const ra8_rmac_config_t cfg = default_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rmac_init(k_ra8_rmac_port_0, &cfg));
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_rmac_set_rx_filter(k_ra8_rmac_port_0,
                           k_ra8_rmac_mrafc_promiscuous | k_ra8_rmac_mrafc_perfect_unicast));
  TEST_ASSERT_EQ((k_ra8_rmac_mrafc_promiscuous | k_ra8_rmac_mrafc_perfect_unicast),
                 ra8_rmac(k_ra8_rmac_port_0)->MRAFC);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_rmac_set_rx_filter((ra8_rmac_port_t)(uint8_t)k_ra8_rmac_port_count,
                                        k_ra8_rmac_mrafc_broadcast));
  TEST_END("rmac set rx filter");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_status_read_and_clear(void)
{
  TEST_BEGIN("rmac status read + clear");
  prep();
  const ra8_rmac_config_t cfg = default_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rmac_init(k_ra8_rmac_port_0, &cfg));
  ra8_rmac(k_ra8_rmac_port_0)->MEIS  = 0xC0FFEE00U;
  ra8_rmac(k_ra8_rmac_port_0)->MMIS0 = 0x00001234U;
  ra8_rmac(k_ra8_rmac_port_0)->MMIS1 = 0x0000000FU;
  ra8_rmac(k_ra8_rmac_port_0)->MMIS2 = 0x00000005U;
  ra8_rmac(k_ra8_rmac_port_0)->MPIM  = (uint32_t)k_ra8_rmac_mpim_pls;

  ra8_rmac_status_t snap = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rmac_get_status(k_ra8_rmac_port_0, &snap));
  TEST_ASSERT_EQ(0xC0FFEE00U, snap.err_status);
  TEST_ASSERT_EQ(0x00001234U, snap.mon_status[0]);
  TEST_ASSERT_EQ(0x0000000FU, snap.mon_status[1]);
  TEST_ASSERT_EQ(0x00000005U, snap.mon_status[2]);
  TEST_ASSERT_EQ(k_ra8_rmac_mpim_pls, snap.phy_monitor);

  TEST_ASSERT_EQ(
    k_ra8_ok,

    ra8_rmac_clear_status(k_ra8_rmac_port_0, 0x00FF00FFU, 0x00000FFFU, 0x0000000FU, 0x00000007U));
  TEST_ASSERT_EQ(0xC000EE00U, ra8_rmac(k_ra8_rmac_port_0)->MEIS);
  TEST_ASSERT_EQ(0x00001000U, ra8_rmac(k_ra8_rmac_port_0)->MMIS0);
  TEST_ASSERT_EQ(0x00000000U, ra8_rmac(k_ra8_rmac_port_0)->MMIS1);
  TEST_ASSERT_EQ(0x00000000U, ra8_rmac(k_ra8_rmac_port_0)->MMIS2);

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_rmac_get_status(k_ra8_rmac_port_0, nullptr));
  TEST_END("rmac status read + clear");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_link_modes(void)
{
  TEST_BEGIN("rmac set link modes");
  prep();
  const ra8_rmac_config_t cfg = default_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rmac_init(k_ra8_rmac_port_0, &cfg));

  /* Walk every (interface, speed, duplex) combination. MPIC.PIS has
   * only MII + GMII defined (HUM Ch 33.4.1.2 p 1707). */
  const ra8_rmac_pis_t ifaces[] = {
    k_ra8_rmac_pis_mii,
    k_ra8_rmac_pis_gmii,
  };
  const ra8_rmac_lsc_t speeds[] = {
    k_ra8_rmac_lsc_10mbit,
    k_ra8_rmac_lsc_100mbit,
    k_ra8_rmac_lsc_1000mbit,
  };
  const ra8_rmac_duplex_t duplexes[] = {k_ra8_rmac_duplex_half, k_ra8_rmac_duplex_full};

  for (uint8_t i = 0; i < (uint8_t)(sizeof(ifaces) / sizeof(ifaces[0])); ++i) {
    for (uint8_t s = 0; s < (uint8_t)(sizeof(speeds) / sizeof(speeds[0])); ++s) {
      for (uint8_t d = 0; d < (uint8_t)(sizeof(duplexes) / sizeof(duplexes[0])); ++d) {
        TEST_ASSERT_EQ(k_ra8_ok,
                       ra8_rmac_set_link(k_ra8_rmac_port_0, ifaces[i], speeds[s], duplexes[d]));
        const uint32_t mpic = ra8_rmac(k_ra8_rmac_port_0)->MPIC;
        TEST_ASSERT_EQ(((uint32_t)ifaces[i] & 0x7U), (mpic & 0x7U));
        TEST_ASSERT_EQ(((uint32_t)speeds[s] & 0x7U), ((mpic >> 3U) & 0x7U));
        const uint32_t pipp_expected = (duplexes[d] == k_ra8_rmac_duplex_full) ? 1U : 0U;
        TEST_ASSERT_EQ(pipp_expected, ((mpic >> 9U) & 0x1U));
      }
    }
  }

  /* Bad-arg paths */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_rmac_set_link(k_ra8_rmac_port_0,
                                   (ra8_rmac_pis_t)(uint8_t)1U, /* 1 = Reserved PIS */
                                   k_ra8_rmac_lsc_10mbit,
                                   k_ra8_rmac_duplex_full));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_rmac_set_link(k_ra8_rmac_port_0,
                                   k_ra8_rmac_pis_mii,
                                   (ra8_rmac_lsc_t)(uint8_t)k_ra8_rmac_lsc_count,
                                   k_ra8_rmac_duplex_full));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_rmac_set_link((ra8_rmac_port_t)(uint8_t)k_ra8_rmac_port_count,
                                   k_ra8_rmac_pis_mii,
                                   k_ra8_rmac_lsc_10mbit,
                                   k_ra8_rmac_duplex_full));
  TEST_END("rmac set link modes");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_frame_size_and_vlan(void)
{
  TEST_BEGIN("rmac frame size + vlan");
  prep();
  const ra8_rmac_config_t cfg = default_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rmac_init(k_ra8_rmac_port_0, &cfg));

  /* Jumbo: min=64 max=9018. E-frame variant first. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rmac_set_frame_size(k_ra8_rmac_port_0, false, 64U, 9018U));
  const uint32_t expected_e = (((uint32_t)9018U) << 0U) | (((uint32_t)64U) << 16U);
  TEST_ASSERT_EQ(expected_e, ra8_rmac(k_ra8_rmac_port_0)->MRFSCE);

  /* P-frame variant */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rmac_set_frame_size(k_ra8_rmac_port_0, true, 60U, 1518U));
  const uint32_t expected_p = (((uint32_t)1518U) << 0U) | (((uint32_t)60U) << 16U);
  TEST_ASSERT_EQ(expected_p, ra8_rmac(k_ra8_rmac_port_0)->MRFSCP);

  /* min > max should reject */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_rmac_set_frame_size(k_ra8_rmac_port_0, false, 9018U, 64U));
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_rmac_set_frame_size((ra8_rmac_port_t)(uint8_t)k_ra8_rmac_port_count, false, 64U, 1518U));

  /* VLAN framing toggles MTFFC */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rmac_set_vlan_framing(k_ra8_rmac_port_0, true, true));
  TEST_ASSERT_EQ((k_ra8_rmac_mtffc_dpad | k_ra8_rmac_mtffc_fcm),
                 ra8_rmac(k_ra8_rmac_port_0)->MTFFC);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rmac_set_vlan_framing(k_ra8_rmac_port_0, false, false));
  TEST_ASSERT_EQ(0U, ra8_rmac(k_ra8_rmac_port_0)->MTFFC);
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_rmac_set_vlan_framing((ra8_rmac_port_t)(uint8_t)k_ra8_rmac_port_count, true, true));
  TEST_END("rmac frame size + vlan");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_pause_and_pfc(void)
{
  TEST_BEGIN("rmac pause + pfc");
  prep();
  const ra8_rmac_config_t cfg = default_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rmac_init(k_ra8_rmac_port_0, &cfg));

  /* Classic pause */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_rmac_set_pause_frame(k_ra8_rmac_port_0,
                                          k_ra8_rmac_pause_mode_802_3x,
                                          0xFFFFU,
                                          0x42U,
                                          0x1FU));
  const uint32_t expected_mtpfc =
    ((uint32_t)0xFFFFU << 0U) | ((uint32_t)0x42U << 16U) | ((uint32_t)0x1FU << 27U);
  TEST_ASSERT_EQ(expected_mtpfc, ra8_rmac(k_ra8_rmac_port_0)->MTPFC);
  TEST_ASSERT_EQ(0U, ra8_rmac(k_ra8_rmac_port_0)->MTPFC2);

  /* PFC mode */
  TEST_ASSERT_EQ(
    k_ra8_ok,

    ra8_rmac_set_pause_frame(k_ra8_rmac_port_0, k_ra8_rmac_pause_mode_pfc, 0x1234U, 0x10U, 0x05U));
  TEST_ASSERT_EQ((1UL << 26U), ra8_rmac(k_ra8_rmac_port_0)->MTPFC2);

  /* PFC priority groups */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_rmac_set_pfc_group(k_ra8_rmac_port_0, k_ra8_rmac_pfc_group_0, 0xAAU));
  TEST_ASSERT_EQ(0xAAU, ra8_rmac(k_ra8_rmac_port_0)->MTPFC3[0]);
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_rmac_set_pfc_group(k_ra8_rmac_port_0, k_ra8_rmac_pfc_group_1, 0x55U));
  TEST_ASSERT_EQ(0x55U, ra8_rmac(k_ra8_rmac_port_0)->MTPFC3[1]);

  /* Bad arg paths */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_rmac_set_pfc_group(k_ra8_rmac_port_0,
                                        (ra8_rmac_pfc_group_t)(uint8_t)k_ra8_rmac_pfc_group_count,
                                        0xFFU));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_rmac_set_pause_frame((ra8_rmac_port_t)(uint8_t)k_ra8_rmac_port_count,
                                          k_ra8_rmac_pause_mode_pfc,
                                          0U,
                                          0U,
                                          0U));
  TEST_END("rmac pause + pfc");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_lpi_and_magic_packet(void)
{
  TEST_BEGIN("rmac lpi + magic packet");
  prep();
  const ra8_rmac_config_t cfg = default_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rmac_init(k_ra8_rmac_port_0, &cfg));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_rmac_set_lpi(k_ra8_rmac_port_0, true));
  TEST_ASSERT_EQ(k_ra8_rmac_meeec_lpitr, ra8_rmac(k_ra8_rmac_port_0)->MEEEC);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rmac_set_lpi(k_ra8_rmac_port_0, false));
  TEST_ASSERT_EQ(0U, ra8_rmac(k_ra8_rmac_port_0)->MEEEC);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_rmac_set_magic_packet(k_ra8_rmac_port_0, true));
  TEST_ASSERT_EQ(k_ra8_rmac_mrgc_mpde,
                 (ra8_rmac(k_ra8_rmac_port_0)->MRGC & (uint32_t)k_ra8_rmac_mrgc_mpde));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rmac_set_magic_packet(k_ra8_rmac_port_0, false));
  TEST_ASSERT_EQ(0U, (ra8_rmac(k_ra8_rmac_port_0)->MRGC & (uint32_t)k_ra8_rmac_mrgc_mpde));

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_rmac_set_lpi((ra8_rmac_port_t)(uint8_t)k_ra8_rmac_port_count, true));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_rmac_set_magic_packet((ra8_rmac_port_t)(uint8_t)k_ra8_rmac_port_count, true));
  TEST_END("rmac lpi + magic packet");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_loopback(void)
{
  TEST_BEGIN("rmac loopback");
  prep();
  const ra8_rmac_config_t cfg = default_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rmac_init(k_ra8_rmac_port_0, &cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rmac_set_loopback(k_ra8_rmac_port_0, true));
  TEST_ASSERT_EQ(k_ra8_rmac_mlbc_lbme, ra8_rmac(k_ra8_rmac_port_0)->MLBC);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rmac_set_loopback(k_ra8_rmac_port_0, false));
  TEST_ASSERT_EQ(0U, ra8_rmac(k_ra8_rmac_port_0)->MLBC);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_rmac_set_loopback((ra8_rmac_port_t)(uint8_t)k_ra8_rmac_port_count, true));
  TEST_END("rmac loopback");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_ptp_filter(void)
{
  TEST_BEGIN("rmac ptp filter");
  prep();
  const ra8_rmac_config_t cfg = default_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rmac_init(k_ra8_rmac_port_0, &cfg));

  /* Program slot 5 -- offset 12, value 0x88, both triggers asserted. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rmac_set_ptp_filter(k_ra8_rmac_port_0, 5U, 12U, 0x88U, true, true));
  const uint32_t expected = (uint32_t)12U | ((uint32_t)0x88U << 8U) | (1UL << 16U) | (1UL << 17U);
  TEST_ASSERT_EQ(expected, ra8_rmac(k_ra8_rmac_port_0)->MPFC[5]);

  /* Bounds */
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,

    ra8_rmac_set_ptp_filter(k_ra8_rmac_port_0, k_ra8_rmac_ptp_filter_count, 0U, 0U, false, false));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_rmac_set_ptp_filter((ra8_rmac_port_t)(uint8_t)k_ra8_rmac_port_count,
                                         0U,
                                         0U,
                                         0U,
                                         false,
                                         false));
  TEST_END("rmac ptp filter");
}

/** @brief Stamp every RMAC statistics counter with a unique pattern so
 *  ::test_read_stats can verify each ra8_rmac_stats_t field is wired to
 *  the right register. */
static void stamp_stats_counters(volatile r_rmac_regs_t* reg)
{
  reg->MMPFTCT     = 0x10U;
  reg->MAPFTCT     = 0x11U;
  reg->MPFRCT      = 0x12U;
  reg->MFCICT      = 0x13U;
  reg->MEEECT      = 0x14U;
  reg->MMPCFTCT[0] = 0x20U;
  reg->MMPCFTCT[1] = 0x21U;
  reg->MAPCFTCT[0] = 0x30U;
  reg->MAPCFTCT[1] = 0x31U;
  for (uint8_t i = 0; i < (uint8_t)k_ra8_rmac_pfc_rx_count; ++i) {
    reg->MPCFRCT[i] = (uint32_t)0x40U + i;
  }
  reg->MROVFC   = 0x50U;
  reg->MRHCRCEC = 0x51U;
  reg->MRGFCE   = 0x52U;
  reg->MRGFCP   = 0x53U;
  reg->MRBFC    = 0x54U;
  reg->MRMFC    = 0x55U;
  reg->MRUFC    = 0x56U;
  reg->MRPEFC   = 0x57U;
  reg->MRNEFC   = 0x58U;
  reg->MRFMEFC  = 0x59U;
  reg->MRFFMEFC = 0x5AU;
  reg->MRCFCEFC = 0x5BU;
  reg->MRFCEFC  = 0x5CU;
  reg->MRRCFEFC = 0x5DU;
  reg->MRFC     = 0x5EU;
  reg->MRGUEFC  = 0x5FU;
  reg->MRBUEFC  = 0x60U;
  reg->MRGOEFC  = 0x61U;
  reg->MRBOEFC  = 0x62U;
  reg->MRXBCEU  = 0x63U;
  reg->MRXBCEL  = 0x64U;
  reg->MRXBCPU  = 0x65U;
  reg->MRXBCPL  = 0x66U;
  reg->MTGFCE   = 0x70U;
  reg->MTGFCP   = 0x71U;
  reg->MTBFC    = 0x72U;
  reg->MTMFC    = 0x73U;
  reg->MTUFC    = 0x74U;
  reg->MTEFC    = 0x75U;
  reg->MTXBCEU  = 0x76U;
  reg->MTXBCEL  = 0x77U;
  reg->MTXBCPU  = 0x78U;
  reg->MTXBCPL  = 0x79U;
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_read_stats(void)
{
  TEST_BEGIN("rmac read stats");
  prep();
  const ra8_rmac_config_t cfg = default_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rmac_init(k_ra8_rmac_port_0, &cfg));

  stamp_stats_counters(ra8_rmac(k_ra8_rmac_port_0));

  ra8_rmac_stats_t s = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rmac_read_stats(k_ra8_rmac_port_0, &s));
  TEST_ASSERT_EQ(0x10U, s.pause_tx_manual);
  TEST_ASSERT_EQ(0x14U, s.eee_count);
  TEST_ASSERT_EQ(0x21U, s.pfc_tx_manual[1]);
  TEST_ASSERT_EQ(0x31U, s.pfc_tx_auto[1]);
  TEST_ASSERT_EQ(0x47U, s.pfc_rx[7]);
  TEST_ASSERT_EQ(0x51U, s.rx_hdr_crc_err);
  TEST_ASSERT_EQ(0x66U, s.rx_bytes_p_lower);
  TEST_ASSERT_EQ(0x79U, s.tx_bytes_p_lower);

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_rmac_read_stats(k_ra8_rmac_port_0, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_rmac_read_stats((ra8_rmac_port_t)(uint8_t)k_ra8_rmac_port_count, &s));
  TEST_END("rmac read stats");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_attach_and_dispatch(void)
{
  TEST_BEGIN("rmac attach + dispatch");
  prep();
  const ra8_rmac_config_t cfg = default_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rmac_init(k_ra8_rmac_port_1, &cfg));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_rmac_attach_handler(k_ra8_rmac_port_1, stub_cb, (void*)(uintptr_t)0xABCDU));

  ra8_rmac(k_ra8_rmac_port_1)->MEIS  = 0x12345678U;
  ra8_rmac(k_ra8_rmac_port_1)->MMIS0 = 0x00000F00U;
  ra8_rmac(k_ra8_rmac_port_1)->MMIS1 = 0x0000000AU;
  ra8_rmac(k_ra8_rmac_port_1)->MMIS2 = 0x00000003U;

  ra8_rmac_dispatch(k_ra8_rmac_port_1);
  TEST_ASSERT_EQ(1, s_cb_count);
  TEST_ASSERT_EQ(0x12345678U, s_cb_last_err);
  TEST_ASSERT_EQ(0x00000F00U, s_cb_last_m0);
  TEST_ASSERT_EQ(0x0000000AU, s_cb_last_m1);
  TEST_ASSERT_EQ(0x00000003U, s_cb_last_m2);
  TEST_ASSERT_EQ(k_ra8_rmac_port_1, s_cb_last_port);

  /* All status registers should now be zero. */
  TEST_ASSERT_EQ(0U, ra8_rmac(k_ra8_rmac_port_1)->MEIS);
  TEST_ASSERT_EQ(0U, ra8_rmac(k_ra8_rmac_port_1)->MMIS0);

  /* Detach */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rmac_attach_handler(k_ra8_rmac_port_1, nullptr, nullptr));
  ra8_rmac(k_ra8_rmac_port_1)->MEIS = 0xDEADBEEFU;
  ra8_rmac_dispatch(k_ra8_rmac_port_1);
  TEST_ASSERT_EQ(1, s_cb_count);

  /* Bad port: dispatch returns early; attach refuses. */
  ra8_rmac_dispatch((ra8_rmac_port_t)(uint8_t)k_ra8_rmac_port_count);
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_rmac_attach_handler((ra8_rmac_port_t)(uint8_t)k_ra8_rmac_port_count, stub_cb, nullptr));
  TEST_END("rmac attach + dispatch");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_stop_and_resume(void)
{
  TEST_BEGIN("rmac stop + resume");
  prep();
  ra8_rmac_config_t cfg = default_cfg();
  cfg.rx_filter         = k_ra8_rmac_mrafc_unicast_match | k_ra8_rmac_mrafc_broadcast;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rmac_init(k_ra8_rmac_port_0, &cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rmac_enter_stop(k_ra8_rmac_port_0));
  TEST_ASSERT_EQ(0U, ra8_rmac(k_ra8_rmac_port_0)->MRAFC);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rmac_exit_stop(k_ra8_rmac_port_0));
  TEST_ASSERT_EQ(cfg.rx_filter, ra8_rmac(k_ra8_rmac_port_0)->MRAFC);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_rmac_enter_stop((ra8_rmac_port_t)(uint8_t)k_ra8_rmac_port_count));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_rmac_exit_stop((ra8_rmac_port_t)(uint8_t)k_ra8_rmac_port_count));
  TEST_END("rmac stop + resume");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_deinit(void)
{
  TEST_BEGIN("rmac deinit");
  prep();
  const ra8_rmac_config_t cfg = default_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rmac_init(k_ra8_rmac_port_0, &cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rmac_deinit(k_ra8_rmac_port_0));
  TEST_ASSERT_EQ(0U, ra8_rmac(k_ra8_rmac_port_0)->MRAFC);
  TEST_ASSERT_EQ(0U, ra8_rmac(k_ra8_rmac_port_0)->MEIE);
  TEST_ASSERT_EQ(0U, ra8_rmac(k_ra8_rmac_port_0)->MMIE0);
  TEST_ASSERT_EQ(0U, ra8_rmac(k_ra8_rmac_port_0)->MMIE1);
  TEST_ASSERT_EQ(0U, ra8_rmac(k_ra8_rmac_port_0)->MMIE2);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_rmac_deinit((ra8_rmac_port_t)(uint8_t)k_ra8_rmac_port_count));
  TEST_END("rmac deinit");
}

int32_t main(void)
{
  test_init_happy();
  test_init_null_cfg();
  test_init_bad_port();
  test_init_bad_interface();
  test_init_bad_speed();
  test_set_mac_address();
  test_set_rx_filter();
  test_status_read_and_clear();
  test_set_link_modes();
  test_frame_size_and_vlan();
  test_pause_and_pfc();
  test_lpi_and_magic_packet();
  test_loopback();
  test_ptp_filter();
  test_read_stats();
  test_attach_and_dispatch();
  test_stop_and_resume();
  test_deinit();
  (void)fprintf(stderr, "[OK  ] test_ra8_rmac.c\n");
  return 0;
}
