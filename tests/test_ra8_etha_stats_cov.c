/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file test_ra8_etha_stats_cov.c
 * @brief Coverage supplement for ra8_etha_stats.c.
 *
 * @details
 * Complements the existing ``test_ra8_etha.c`` and
 * ``test_ra8_etha_rmac_edge_cases.c`` suites by covering the branches
 * in ``ra8_etha_stats.c`` that neither prior suite reaches:
 *
 *   - ``internal_etha_to_operation`` early return on success:
 *     pre-seeding EAMS = OPERATION causes the first poll iteration to
 *     match, returning ``k_ra8_ok`` without exhausting the 200000-spin cap.
 *
 *   - The entire PHY sequencing path in ``ra8_etha_open``: reachable
 *     only after ``internal_etha_to_operation`` returns ``k_ra8_ok``.
 *     On the happy path ``ra8_rmac_phy_reset``,
 *     ``ra8_rmac_phy_set_advertise``, and ``ra8_rmac_phy_auto_neg_start``
 *     succeed (their MPSM waits consult the unarmed ra8_fake_mmio seam),
 *     while ``ra8_rmac_phy_auto_neg_wait`` times out because BMSR is 0.
 *     Each per-step error leg is reached by arming the seam to fail the
 *     matching MPSM wait-loop (fail_wait / fail_nth_wait).
 */

#include "ra8_err.h"
#include "ra8_etha.h"
#include "ra8_etha_regs.h"
#include "ra8_fake_mmap.h"
#include "ra8_fake_mmio.h"
#include "ra8_mstp.h"
#include "ra8_rmac.h"
#include "ra8_rmac_regs.h"
#include "unity_minimal.h"

/**
 * @brief Reset all fake peripheral state before each test.
 *
 * @details Wipes every peripheral register window to zero via
 * ``ra8_fake_mmap_reset``, then re-gates clocks via ``ra8_mstp_init`` so
 * subsequent ``ra8_etha_init`` / ``ra8_rmac_init`` calls see the expected
 * post-reset baseline.
 *
 * @pre None.
 * @pre None.
 * @post All fake MMIO registers are zero.
 * @post MSTP clock-gate registers are in their default enabled state.
 * @note Not thread-safe; each test is single-threaded.
 * @since 0.1.0
 */
static void prep(void)
{
  ra8_fake_mmap_reset();
  ra8_fake_mmio_reset();
  (void)ra8_mstp_init();
}

/**
 * @test test_etha_open_covers_phy_seq
 *
 * @brief Cover the PHY sequencing lines inside ``ra8_etha_open`` by
 *        pre-seeding EAMS so ``internal_etha_to_operation`` returns
 *        immediately.
 *
 * @details
 * The function ``internal_etha_to_operation`` writes EAMC = OPERATION
 * and then polls EAMS until the OPS field matches.  In the ra8_emulator the
 * hardware state machine that mirrors EAMC into EAMS is absent, so
 * without assistance the poll runs all 200000 iterations and returns
 * ``k_ra8_err_hw_timeout``, causing ``ra8_etha_open`` to return before it
 * reaches the PHY calls.
 *
 * By writing ``ra8_etha(port)->EAMS = k_ra8_etha_opc_operation`` before
 * the call, the first poll iteration sees the expected value and exits
 * early.  Control then passes to the three MDIO calls --
 * ``ra8_rmac_phy_reset``, ``ra8_rmac_phy_set_advertise``, and
 * ``ra8_rmac_phy_auto_neg_start`` -- which all succeed off-target mode.
 * ``ra8_rmac_phy_auto_neg_wait`` polls BMSR, which reads zero from the
 * fake backing (no auto-neg handshake possible), and times out after the
 * 100-iteration cap imposed by ``timeout_ms = 1``.
 *
 * @par MC/DC:
 * (no compound decisions in this test -- every guarded return in the
 * code under test is a single condition; no ``&&`` or ``||``; the
 * single-condition guards are exercised via their false branch)
 *
 * @pre The ra8_emulator MMIO backing is zeroed by ``prep()``.
 * @pre ETHA port 0 and RMAC port 0 are initialised before ``ra8_etha_open``.
 * @post EAMC register holds the OPERATION encoding.
 * @post ``out_link.up`` is false (auto-neg timed out).
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_etha_open_covers_phy_seq(void)
{
  TEST_BEGIN("etha_open: EAMS pre-seeded -> PHY seq covered, auto_neg times out");
  prep();

  const ra8_etha_config_t ecfg = {.initial_mode = k_ra8_etha_opc_config,
                                  .eaeie0_mask  = 0U,
                                  .eaeie1_mask  = 0U,
                                  .eaeie2_mask  = 0U};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_etha_init(k_ra8_etha_port_0, &ecfg));

  const ra8_rmac_config_t rcfg = {.rx_filter       = k_ra8_rmac_mrafc_unicast_match,
                                  .err_irq_enable  = 0U,
                                  .mon0_irq_enable = 0U,
                                  .mon1_irq_enable = 0U,
                                  .mon2_irq_enable = 0U,
                                  .phy_interface   = k_ra8_rmac_pis_mii,
                                  .link_speed      = k_ra8_rmac_lsc_100mbit,
                                  .duplex          = k_ra8_rmac_duplex_full,
                                  .eswclk_hz       = 0U,
                                  .mdc_hz          = 0U};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rmac_init(k_ra8_rmac_port_0, &rcfg));

  /* Pre-seed EAMS so the first EAMS poll in internal_etha_to_operation
   * sees OPS == OPERATION and returns k_ra8_ok without spinning the
   * 200000-iteration cap. */
  ra8_etha(k_ra8_etha_port_0)->EAMS = (uint32_t)k_ra8_etha_opc_operation;

  ra8_rmac_phy_link_t       lk  = {};
  const ra8_etha_phy_open_t phy = {.phy_addr   = 1U,
                                   .advertise  = (uint16_t)k_ra8_rmac_phy_advert_100_fd,
                                   .timeout_ms = 1U};

  /* Expected outcome: EAMC -> OPERATION (internal_etha_to_operation converges
   * on its first poll); phy_reset, set_advertise, auto_neg_start all return
   * k_ra8_ok off-target; auto_neg_wait times out because BMSR reads as 0
   * -> k_ra8_err_hw_timeout. */
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_etha_open(k_ra8_etha_port_0, &phy, &lk));
  TEST_ASSERT_EQ(k_ra8_etha_opc_operation,
                 (ra8_etha(k_ra8_etha_port_0)->EAMC & (uint32_t)k_ra8_etha_mask_opc));
  TEST_ASSERT(!lk.up);

  TEST_END("etha_open: EAMS pre-seeded -> PHY seq covered, auto_neg times out");
}

/**
 * @brief Bring ETHA port 0 + RMAC port 0 up and pre-seed EAMS.
 *
 * @details Shared fixture for the ``ra8_etha_open`` PHY-step error-leg
 * tests: resets the fake backing, initialises the ETHA and RMAC ports
 * with the same minimal configs the happy-path case uses, and
 * pre-seeds EAMS so ``internal_etha_to_operation`` converges on its
 * first poll, leaving the MPSM wait-loop count at zero when
 * ``ra8_etha_open`` reaches the PHY sequence.
 *
 * @pre The test binary owns the fake MMIO window (single-threaded).
 * @pre No ra8_fake_mmio fault is armed (prep disarms them).
 * @post ETHA port 0 and RMAC port 0 are initialised.
 * @post EAMS reads OPERATION so the mode poll exits immediately.
 * @note Not thread-safe; call from the test body only.
 * @since 0.1.0
 */
static void open_fixture(void)
{
  prep();
  const ra8_etha_config_t ecfg = {.initial_mode = k_ra8_etha_opc_config,
                                  .eaeie0_mask  = 0U,
                                  .eaeie1_mask  = 0U,
                                  .eaeie2_mask  = 0U};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_etha_init(k_ra8_etha_port_0, &ecfg));
  const ra8_rmac_config_t rcfg = {.rx_filter       = k_ra8_rmac_mrafc_unicast_match,
                                  .err_irq_enable  = 0U,
                                  .mon0_irq_enable = 0U,
                                  .mon1_irq_enable = 0U,
                                  .mon2_irq_enable = 0U,
                                  .phy_interface   = k_ra8_rmac_pis_mii,
                                  .link_speed      = k_ra8_rmac_lsc_100mbit,
                                  .duplex          = k_ra8_rmac_duplex_full,
                                  .eswclk_hz       = 0U,
                                  .mdc_hz          = 0U};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rmac_init(k_ra8_rmac_port_0, &rcfg));
  ra8_etha(k_ra8_etha_port_0)->EAMS = (uint32_t)k_ra8_etha_opc_operation;
}

/**
 * @test test_etha_open_phy_step_error_legs
 *
 * @par MC/DC:
 * (no compound decisions under test -- the three ``err != k_ra8_ok``
 * guards in ra8_etha_open's PHY sequence are single-condition ``if``
 * statements; each armed MPSM wait-loop fault selects exactly one of
 * them while the earlier steps complete on their first poll)
 *
 * @details Each PHY bring-up step is an MDIO op pair on the RMAC MPSM
 * register (drain + post-wait): phy_reset consumes wait-loops 0-3
 * (write pair + one BMCR-poll read pair), set_advertise 4-5, and
 * auto_neg_start 6-7. Failing wait-loop 0 / 4 / 6 therefore lands on
 * the phy_reset / set_advertise / auto_neg_start error leg of
 * ``ra8_etha_open`` -- all three previously dead on host while the fake
 * MDIO path auto-completed via MMIS1 arming.
 */
static void test_etha_open_phy_step_error_legs(void)
{
  TEST_BEGIN("etha_open: each PHY step's MDIO error leg is surfaced");

  ra8_rmac_phy_link_t       lk   = {};
  const ra8_etha_phy_open_t phy  = {.phy_addr   = 1U,
                                    .advertise  = (uint16_t)k_ra8_rmac_phy_advert_100_fd,
                                    .timeout_ms = 1U};
  volatile uint32_t* const  mpsm = &ra8_rmac(k_ra8_rmac_port_0)->MPSM;

  /* Leg 1: phy_reset fails (its BMCR write's drain, wait-loop 0). */
  open_fixture();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fake_mmio_fail_nth_wait(mpsm, 0U));
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_etha_open(k_ra8_etha_port_0, &phy, &lk));
  TEST_ASSERT(!lk.up);

  /* Leg 2: phy_reset completes (loops 0-3), set_advertise's drain
   * (wait-loop 4) fails. */
  open_fixture();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fake_mmio_fail_nth_wait(mpsm, 4U));
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_etha_open(k_ra8_etha_port_0, &phy, &lk));

  /* Leg 3: set_advertise completes (loops 4-5), auto_neg_start's drain
   * (wait-loop 6) fails. */
  open_fixture();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fake_mmio_fail_nth_wait(mpsm, 6U));
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_etha_open(k_ra8_etha_port_0, &phy, &lk));

  TEST_END("etha_open: each PHY step's MDIO error leg is surfaced");
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
 * @pre The process runs in an environment where ``RA8_OFF_TARGET``
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
  test_etha_open_phy_step_error_legs();
  (void)fprintf(stderr, "[OK  ] test_ra8_etha_stats_cov.c\n");
  return 0;
}
