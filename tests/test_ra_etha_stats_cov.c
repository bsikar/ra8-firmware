/**
 * @file test_ra_etha_stats_cov.c
 * @brief Coverage supplement for ra_etha_stats.c.
 *
 * @details
 * Complements the existing ``test_ra_etha.c`` and
 * ``test_ra_etha_rmac_edge_cases.c`` suites by covering the branches
 * in ``ra_etha_stats.c`` that neither prior suite reaches:
 *
 *   - ``internal_etha_to_operation`` early return on success (line 204):
 *     pre-seeding EAMS = OPERATION causes the first poll iteration to
 *     match, returning ``k_ra_ok`` without exhausting the 200000-spin cap.
 *
 *   - The entire PHY sequencing path in ``ra_etha_open`` (lines 228-243):
 *     reachable only after ``internal_etha_to_operation`` returns
 *     ``k_ra_ok``; in ``RA_SIMULATOR_MODE``, ``ra_rmac_phy_reset``,
 *     ``ra_rmac_phy_set_advertise``, and ``ra_rmac_phy_auto_neg_start``
 *     all succeed immediately (MDIO auto-completes via MMIS1 arming),
 *     while ``ra_rmac_phy_auto_neg_wait`` times out because BMSR is 0.
 *
 *   - ``ra_etha_test_inject_rx`` short-frame rejection (line 264):
 *     triggered when ``frame_len`` is below the 14-byte Ethernet header
 *     minimum; the function returns ``k_ra_err_invalid_arg``.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8d2_etha_regs.h"
#include "ra8d2_rmac_regs.h"
#include "ra_err.h"
#include "ra_etha.h"
#include "ra_mstp.h"
#include "ra_rmac.h"
#include "ra_sim_mmap.h"
#include "unity_minimal.h"

/**
 * @brief Reset all simulated peripheral state before each test.
 *
 * @details Wipes every peripheral register window to zero via
 * ``ra_sim_mmap_reset``, then re-gates clocks via ``ra_mstp_init`` so
 * subsequent ``ra_etha_init`` / ``ra_rmac_init`` calls see the expected
 * post-reset baseline.
 *
 * @pre None.
 * @pre None.
 * @post All simulated MMIO registers are zero.
 * @post MSTP clock-gate registers are in their default enabled state.
 * @note Not thread-safe; each test is single-threaded.
 * @since 0.1.0
 */
static void prep(void)
{
  ra_sim_mmap_reset();
  (void)ra_mstp_init();
}

/**
 * @test test_etha_open_covers_phy_seq
 *
 * @brief Cover the PHY sequencing lines inside ``ra_etha_open`` by
 *        pre-seeding EAMS so ``internal_etha_to_operation`` returns
 *        immediately.
 *
 * @details
 * The function ``internal_etha_to_operation`` writes EAMC = OPERATION
 * and then polls EAMS until the OPS field matches.  In the board_sim the
 * hardware state machine that mirrors EAMC into EAMS is absent, so
 * without assistance the poll runs all 200000 iterations and returns
 * ``k_ra_err_hw_timeout``, causing ``ra_etha_open`` to return before it
 * reaches the PHY calls.
 *
 * By writing ``ra_etha(port)->EAMS = k_ra_etha_opc_operation`` before
 * the call, the first poll iteration sees the expected value and exits
 * early (line 204).  Control then passes to the three MDIO calls --
 * ``ra_rmac_phy_reset``, ``ra_rmac_phy_set_advertise``, and
 * ``ra_rmac_phy_auto_neg_start`` -- which all succeed in sim mode
 * (lines 228-239).  ``ra_rmac_phy_auto_neg_wait`` polls BMSR, which
 * reads zero from the sim backing (no auto-neg handshake possible), and
 * times out after the 100-iteration cap imposed by ``timeout_ms = 1``
 * (line 243).
 *
 * @par MC/DC:
 * (no compound decisions in this test -- every guarded return in the
 * code under test is a single condition; no ``&&`` or ``||``; the
 * single-condition guards are exercised via their false branch)
 *
 * @pre The board_sim MMIO backing is zeroed by ``prep()``.
 * @pre ETHA port 0 and RMAC port 0 are initialised before ``ra_etha_open``.
 * @post EAMC register holds the OPERATION encoding.
 * @post ``out_link.up`` is false (auto-neg timed out).
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_etha_open_covers_phy_seq(void)
{
  TEST_BEGIN("etha_open: EAMS pre-seeded -> PHY seq covered, auto_neg times out");
  prep();

  const ra_etha_config_t ecfg = {.initial_mode = k_ra_etha_opc_config,
                                 .eaeie0_mask  = 0U,
                                 .eaeie1_mask  = 0U,
                                 .eaeie2_mask  = 0U};
  TEST_ASSERT_EQ(k_ra_ok, ra_etha_init(k_ra_etha_port_0, &ecfg));

  const ra_rmac_config_t rcfg = {.rx_filter       = k_ra_rmac_mrafc_unicast_match,
                                 .err_irq_enable  = 0U,
                                 .mon0_irq_enable = 0U,
                                 .mon1_irq_enable = 0U,
                                 .mon2_irq_enable = 0U,
                                 .phy_interface   = k_ra_rmac_pis_mii,
                                 .link_speed      = k_ra_rmac_lsc_100mbit,
                                 .duplex          = k_ra_rmac_duplex_full,
                                 .eswclk_hz       = 0U,
                                 .mdc_hz          = 0U};
  TEST_ASSERT_EQ(k_ra_ok, ra_rmac_init(k_ra_rmac_port_0, &rcfg));

  /* Pre-seed EAMS so the first EAMS poll in internal_etha_to_operation
   * sees OPS == OPERATION and returns k_ra_ok at ra_etha_stats.c line 204
   * without spinning the 200000-iteration cap. */
  ra_etha(k_ra_etha_port_0)->EAMS = (uint32_t)k_ra_etha_opc_operation;

  ra_rmac_phy_link_t       lk  = {};
  const ra_etha_phy_open_t phy = {.phy_addr   = 1U,
                                  .advertise  = (uint16_t)k_ra_rmac_phy_advert_100_fd,
                                  .timeout_ms = 1U};

  /* Expected outcome: EAMC -> OPERATION (line 204 hit); phy_reset, set_advertise,
   * auto_neg_start all return k_ra_ok in sim (lines 228-239 hit); auto_neg_wait
   * times out because BMSR reads as 0 (line 243 hit) -> k_ra_err_hw_timeout. */
  TEST_ASSERT_EQ(k_ra_err_hw_timeout, ra_etha_open(k_ra_etha_port_0, &phy, &lk));
  TEST_ASSERT_EQ(k_ra_etha_opc_operation,
                 (ra_etha(k_ra_etha_port_0)->EAMC & (uint32_t)k_ra_etha_mask_opc));
  TEST_ASSERT(!lk.up);

  TEST_END("etha_open: EAMS pre-seeded -> PHY seq covered, auto_neg times out");
}

/**
 * @test test_inject_rx_short_frame
 *
 * @brief Cover the frame-too-short early return in
 *        ``ra_etha_test_inject_rx`` (line 264).
 *
 * @details
 * ``ra_etha_test_inject_rx`` requires at least 14 bytes (a complete
 * Ethernet II header: 6 bytes destination MAC, 6 bytes source MAC,
 * 2 bytes EtherType).  Supplying ``frame_len = 13`` exercises the guard
 * at the top of the function body and returns
 * ``k_ra_err_invalid_arg`` without reading beyond the supplied buffer.
 *
 * @par MC/DC:
 * (no compound decisions in this test -- the ``frame_len`` guard is a
 * single-condition comparison; its true branch is covered here, the
 * false branch (frame is long enough) is covered by the existing
 * test_ra_etha.c happy-path test)
 *
 * @pre The board_sim MMIO backing is zeroed by ``prep()``.
 * @pre A 14-byte buffer is available; only the first 13 bytes are passed.
 * @post No output pointers are written (early return before any copy).
 * @post Return value equals ``k_ra_err_invalid_arg``.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_inject_rx_short_frame(void)
{
  TEST_BEGIN("etha_test_inject_rx: frame_len < 14 -> k_ra_err_invalid_arg");
  prep();

  /* Fourteen-byte Ethernet II header -- we pass only 13 bytes to
   * trigger the short-frame rejection inside ra_etha_test_inject_rx. */
  const uint8_t frame[14] = {
    0xFFU,
    0xFFU,
    0xFFU,
    0xFFU,
    0xFFU,
    0xFFU, /* destination MAC */
    0x00U,
    0x11U,
    0x22U,
    0x33U,
    0x44U,
    0x55U, /* source MAC */
    0x08U,
    0x00U, /* EtherType (IPv4) */
  };
  uint8_t  out_dst[6] = {};
  uint8_t  out_src[6] = {};
  uint16_t out_etype  = 0U;

  TEST_ASSERT_EQ(k_ra_err_invalid_arg,
                 ra_etha_test_inject_rx(frame, 13U, out_dst, out_src, &out_etype));

  TEST_END("etha_test_inject_rx: frame_len < 14 -> k_ra_err_invalid_arg");
}

/**
 * @brief Test-suite entry point.
 *
 * @details Runs all coverage-supplement test functions in sequence.
 * Prints a summary line on success; any assertion failure calls
 * ``exit(1)`` via the ``TEST_FAIL_FMT`` macro.
 *
 * @return int32_t Always 0 on success.
 * @retval 0 All tests passed.
 *
 * @pre The process runs in an environment where ``RA_SIMULATOR_MODE``
 *      is defined (guaranteed by the test build system).
 * @pre Standard I/O is available for test reporting.
 * @post All test functions have completed without assertion failure.
 * @post A summary line is printed to stderr.
 * @note Not thread-safe.
 * @since 0.1.0
 */
int32_t main(void)
{
  test_etha_open_covers_phy_seq();
  test_inject_rx_short_frame();
  (void)fprintf(stderr, "[OK  ] test_ra_etha_stats_cov.c\n");
  return 0;
}
