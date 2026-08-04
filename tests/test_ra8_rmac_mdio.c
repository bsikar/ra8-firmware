/**
 * @file test_ra8_rmac_mdio.c
 * @brief Unit tests for the ra8_rmac PHY management side: MDIO C22/C45
 *        transactions, PHY reset / auto-negotiation / link status, and
 *        their MC/DC vector suites
 *
 * @details Split from test_ra8_rmac.c along the test-group seam: this
 * binary owns everything that talks to the PHY through the MPSM MDIO
 * station (including the wait-loop timeout legs and the MC/DC vectors
 * threaded through the PHY helpers); the sibling test_ra8_rmac.c owns
 * the MAC-core public-API contract tests.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_fake_mmio.h"
#include "ra8_mstp.h"
#include "ra8_rmac.h"
#include "ra8_rmac_regs.h"
#include "unity_minimal.h"

/**
 * @enum rmac_mdio_test_lit_t
 * @brief Named constants for the register stamp patterns and literal
 *        test vectors previously inlined in this file's test bodies.
 */
typedef enum : uint32_t {
  k_rmac_mdio_cfg_err_irq_enable  = 0xCAFEBABEU, /**< Rmac mdio config error IRQ enable. */
  k_rmac_mdio_cfg_mon0_irq_enable = 0x00001FFFU, /**< Rmac mdio config mon0 IRQ enable.  */
  k_rmac_mdio_cfg_mon1_irq_enable = 0x0000000FU, /**< Rmac mdio config mon1 IRQ enable.  */
  k_rmac_mdio_cfg_mon2_irq_enable = 0x00000007U, /**< Rmac mdio config mon2 IRQ enable.  */
  k_rmac_mdio_lit_xcafe           = 0xCAFEU,     /**< Rmac mdio literal 0xCAFE.          */
  k_rmac_mdio_cfg_eswclk_hz       = 125000000U,  /**< Rmac mdio config eswclk hz.        */
  k_rmac_mdio_cfg_mdc_hz          = 1000000U,    /**< Rmac mdio config mdc hz.           */
} rmac_mdio_test_lit_t;

/** @brief Per-test fixture reset: fresh peripheral RAM, MMIO seam, MSTP. */
static void prep(void)
{
  ra8_fake_mmap_reset();
  ra8_fake_mmio_reset();
  (void)ra8_mstp_init();
}

static ra8_rmac_config_t default_cfg(void)
{
  return (ra8_rmac_config_t){
    .rx_filter       = k_ra8_rmac_mrafc_unicast_match,
    .err_irq_enable  = k_rmac_mdio_cfg_err_irq_enable,
    .mon0_irq_enable = k_rmac_mdio_cfg_mon0_irq_enable,
    .mon1_irq_enable = k_rmac_mdio_cfg_mon1_irq_enable,
    .mon2_irq_enable = k_rmac_mdio_cfg_mon2_irq_enable,
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
static void test_mdio_c22(void)
{
  TEST_BEGIN("rmac mdio c22");
  prep();
  const ra8_rmac_config_t cfg = default_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rmac_init(k_ra8_rmac_port_0, &cfg));

  /* The MPSM waits consult the unarmed ra8_fake_mmio seam and complete
   * on their first poll, so no register pre-staging is needed. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rmac_mdio_c22_write(k_ra8_rmac_port_0, 0x1FU, 0x1AU, 0xBEEFU));
  /* MPSM should have been written with the encoded transaction. */
  const uint32_t mpsm_w = ra8_rmac(k_ra8_rmac_port_0)->MPSM;
  TEST_ASSERT_EQ(1U, (mpsm_w & 0x1U));      /* PSME */
  TEST_ASSERT_EQ(0xBEEFU, (mpsm_w >> 16U)); /* PRD  */
  TEST_ASSERT_EQ(((uint32_t)k_ra8_rmac_mdio_op_c22_write << 13U),
                 (mpsm_w & (0x3UL << 13U))); /* POP */

  /* Now stage a read: pre-load a PRD value the issue must overwrite. */
  ra8_rmac(k_ra8_rmac_port_0)->MPSM = ((uint32_t)k_rmac_mdio_lit_xcafe << 16U);
  uint16_t v                        = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rmac_mdio_c22_read(k_ra8_rmac_port_0, 0x05U, 0x10U, &v));
  /* The driver issued an MPSM write that overwrote the PRD field with 0,
   * so the reader should see 0 (the freshly-issued read frame). */
  TEST_ASSERT_EQ(0U, v);

  /* Bad-arg / null cases */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_rmac_mdio_c22_read(k_ra8_rmac_port_0, 0U, 0U, nullptr));
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_rmac_mdio_c22_write((ra8_rmac_port_t)(uint8_t)k_ra8_rmac_port_count, 0U, 0U, 0U));
  TEST_END("rmac mdio c22");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_mdio_c45(void)
{
  TEST_BEGIN("rmac mdio c45");
  prep();
  const ra8_rmac_config_t cfg = default_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rmac_init(k_ra8_rmac_port_0, &cfg));

  /* Every MPSM wait (each op's drain AND post-issue PSME wait) consults
   * the ra8_fake_mmio seam; satisfy-after-2 steps each loop twice before
   * it converges, covering the loop-continuation branch (T1-01).
   * Stays armed across the write + read below. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fake_mmio_satisfy_after(&ra8_rmac(k_ra8_rmac_port_0)->MPSM, 2U));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_rmac_mdio_c45_write(k_ra8_rmac_port_0, 0x07U, 0x03U, 0x1234U, 0xABCDU));

  /* Address + read */
  uint16_t v = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rmac_mdio_c45_read(k_ra8_rmac_port_0, 0x07U, 0x03U, 0x1234U, &v));

  /* Bad-arg / null cases */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_rmac_mdio_c45_read(k_ra8_rmac_port_0, 0U, 0U, 0U, nullptr));
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_rmac_mdio_c45_write((ra8_rmac_port_t)(uint8_t)k_ra8_rmac_port_count, 0U, 0U, 0U, 0U));
  TEST_END("rmac mdio c45");
}

/* --- PHY auto-negotiation state machine ---
 *
 * Every MDIO wait consults the ra8_fake_mmio seam and completes on its
 * first poll when unarmed. We pre-stage MPSM so the driver's read
 * fetches a chosen value (the driver clears PRD bits during issue, so
 * reads always return 0 -- we leverage this for negative-path tests).
 */
static void prime_mdio(ra8_rmac_port_t port, uint16_t mpsm_prd)
{
  ra8_rmac(port)->MPSM = ((uint32_t)mpsm_prd << 16U);
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_phy_reset_happy(void)
{
  TEST_BEGIN("rmac phy reset happy");
  prep();
  const ra8_rmac_config_t cfg = default_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rmac_init(k_ra8_rmac_port_0, &cfg));
  /* MPSM read returns 0 (RESET cleared) -> driver returns k_ra8_ok. */
  prime_mdio(k_ra8_rmac_port_0, 0x0000U);
  /* phy_reset issues a BMCR write then polls with a BMCR read; each MDIO op
   * pre-drains MPSM.PSME to CLEAR and the driver set PSME on the prior issue,
   * so arm the seam to satisfy every drain clear-wait (T1-01, pattern B). */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fake_mmio_satisfy_after(&ra8_rmac(k_ra8_rmac_port_0)->MPSM, 2U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rmac_phy_reset(k_ra8_rmac_port_0, 5U));
  TEST_END("rmac phy reset happy");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_phy_reset_bad_args(void)
{
  TEST_BEGIN("rmac phy reset bad args");
  prep();
  const ra8_rmac_config_t cfg = default_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rmac_init(k_ra8_rmac_port_0, &cfg));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_rmac_phy_reset((ra8_rmac_port_t)(uint8_t)k_ra8_rmac_port_count, 0U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_rmac_phy_reset(k_ra8_rmac_port_0, 32U));
  TEST_END("rmac phy reset bad args");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_phy_set_advertise(void)
{
  TEST_BEGIN("rmac phy set_advertise");
  prep();
  const ra8_rmac_config_t cfg = default_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rmac_init(k_ra8_rmac_port_0, &cfg));
  prime_mdio(k_ra8_rmac_port_0, 0U);
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_rmac_phy_set_advertise(k_ra8_rmac_port_0,
                                            7U,
                                            (uint16_t)k_ra8_rmac_phy_advert_100_fd |
                                              (uint16_t)k_ra8_rmac_phy_advert_10_fd));
  /* MPSM PRD field should encode (capabilities | selector_bit). */
  const uint32_t mpsm = ra8_rmac(k_ra8_rmac_port_0)->MPSM;
  const uint16_t prd  = (uint16_t)((mpsm >> 16U) & 0xFFFFU);
  TEST_ASSERT(((prd & (uint16_t)k_ra8_rmac_phy_advert_100_fd) != 0U));
  TEST_ASSERT(((prd & (uint16_t)k_ra8_rmac_phy_advert_10_fd) != 0U));
  TEST_ASSERT(((prd & 0x0001U) != 0U)); /* selector */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_rmac_phy_set_advertise(k_ra8_rmac_port_0, 32U, 0U));
  TEST_END("rmac phy set_advertise");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_phy_auto_neg_start(void)
{
  TEST_BEGIN("rmac phy auto_neg_start");
  prep();
  const ra8_rmac_config_t cfg = default_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rmac_init(k_ra8_rmac_port_0, &cfg));
  prime_mdio(k_ra8_rmac_port_0, 0U);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rmac_phy_auto_neg_start(k_ra8_rmac_port_0, 1U));
  const uint32_t mpsm = ra8_rmac(k_ra8_rmac_port_0)->MPSM;
  const uint16_t prd  = (uint16_t)((mpsm >> 16U) & 0xFFFFU);
  TEST_ASSERT(((prd & 0x1000U) != 0U)); /* AN_ENABLE  */
  TEST_ASSERT(((prd & 0x0200U) != 0U)); /* AN_RESTART */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_rmac_phy_auto_neg_start((ra8_rmac_port_t)(uint8_t)k_ra8_rmac_port_count, 0U));
  TEST_END("rmac phy auto_neg_start");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_phy_auto_neg_wait_happy(void)
{
  TEST_BEGIN("rmac phy auto_neg_wait MPSM transaction shape");
  prep();
  const ra8_rmac_config_t cfg = default_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rmac_init(k_ra8_rmac_port_0, &cfg));
  /* Fake returns 0 from MPSM PRD on every read; AN_COMPLETE will
   * never be observed, so the call returns hw_timeout. We check the
   * MPSM transaction shape (PRA = BMSR = 1, PSME = 1) regardless. */
  prime_mdio(k_ra8_rmac_port_0, 0U);
  /* The AN-wait loop issues a fresh BMSR read each spin; every read pre-drains
   * MPSM.PSME to CLEAR (set by the prior issue), so arm the seam so the loop
   * runs to its full budget and times out on AN_COMPLETE rather than stalling
   * on a pre-drain (T1-01, pattern B). */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fake_mmio_satisfy_after(&ra8_rmac(k_ra8_rmac_port_0)->MPSM, 2U));
  ra8_rmac_phy_link_t link = {};
  const ra8_err_t     r    = ra8_rmac_phy_auto_neg_wait(k_ra8_rmac_port_0, 0U, 1U, &link);
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, r);
  TEST_ASSERT(!link.up);
  const uint32_t mpsm = ra8_rmac(k_ra8_rmac_port_0)->MPSM;
  TEST_ASSERT_EQ(1U, (mpsm & 0x1U));          /* PSME       */
  TEST_ASSERT_EQ(1U, ((mpsm >> 8U) & 0x1FU)); /* PRA = BMSR */
  TEST_END("rmac phy auto_neg_wait MPSM transaction shape");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_phy_auto_neg_wait_timeout(void)
{
  TEST_BEGIN("rmac phy auto_neg_wait timeout");
  prep();
  const ra8_rmac_config_t cfg = default_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rmac_init(k_ra8_rmac_port_0, &cfg));
  /* Each per-read MDIO wait completes via the seam (satisfy-after-2 also
   * steps the loop-continuation branch); the AN loop instead exhausts its
   * own timeout budget because the fake BMSR never asserts
   * AN_COMPLETE (reads deliver PRD = 0). */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fake_mmio_satisfy_after(&ra8_rmac(k_ra8_rmac_port_0)->MPSM, 2U));
  ra8_rmac_phy_link_t link = {.up = true, .speed = k_ra8_rmac_phy_speed_100_fd};
  const ra8_err_t     r    = ra8_rmac_phy_auto_neg_wait(k_ra8_rmac_port_0, 1U, 1U, &link);
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, r);
  TEST_ASSERT(!link.up);
  TEST_ASSERT_EQ(k_ra8_rmac_phy_speed_unknown, link.speed);
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_rmac_phy_auto_neg_wait(k_ra8_rmac_port_0, 0U, 0U, nullptr));
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_rmac_phy_auto_neg_wait((ra8_rmac_port_t)(uint8_t)k_ra8_rmac_port_count, 0U, 0U, &link));
  TEST_END("rmac phy auto_neg_wait timeout");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_phy_link_status(void)
{
  TEST_BEGIN("rmac phy link_status");
  prep();
  const ra8_rmac_config_t cfg = default_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rmac_init(k_ra8_rmac_port_0, &cfg));
  /* Fake returns 0 -> link reads as down. */
  prime_mdio(k_ra8_rmac_port_0, 0U);
  ra8_rmac_phy_link_t link = {.up = true, .speed = k_ra8_rmac_phy_speed_100_fd};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rmac_phy_link_status(k_ra8_rmac_port_0, 3U, &link));
  TEST_ASSERT(!link.up);
  TEST_ASSERT_EQ(k_ra8_rmac_phy_speed_unknown, link.speed);
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_rmac_phy_link_status(k_ra8_rmac_port_0, 0U, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_rmac_phy_link_status(k_ra8_rmac_port_0, 32U, &link));
  TEST_END("rmac phy link_status");
}

/**
 * @test test_mcdc_ra8_rmac
 *
 * @par MC/DC:
 * Decision A: ``internal_phy_args_ok`` line 893,
 * libs/ra8_hal/src/ra8_rmac.c:
 * ``return internal_port_ok(port) && (phy_addr <= 31)``
 * (2 conditions, ``&&``). Threaded through ``ra8_rmac_phy_link_status``.
 * N+1 = 3:
 * - V1: port=0,    phy=5  -> dec T (k_ra8_ok)
 * - V2: port=COUNT,phy=5  -> dec F (invalid_arg)
 * - V3: port=0,    phy=32 -> dec F (invalid_arg)
 *
 * Decision B: ``ra8_rmac_phy_link_status`` line 1047,
 * ``if (out_link->up && ((bmsr & an_done) != 0U))`` (2 conditions, ``&&``).
 * Fake MDIO returns 0 -> only V1 (C1=F short-circuit) is
 * achievable. Representative-subset rationale per DO-178C 6.4.4.3
 * (environment constraint: MDIO fake does not deliver a non-zero
 * BMSR response without an injection mock; documented limitation).
 */
static void test_mcdc_ra8_rmac(void)
{
  TEST_BEGIN("rmac MC/DC: phy_args_ok + link_status decisions");
  prep();
  const ra8_rmac_config_t cfg = default_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rmac_init(k_ra8_rmac_port_0, &cfg));
  prime_mdio(k_ra8_rmac_port_0, 0U);
  /* Each phy_link_status issues one BMSR read whose pre-drain waits MPSM.PSME
   * to CLEAR; the second happy-path read below drains a PSME the first read's
   * issue set, so arm the seam to satisfy every clear-wait (T1-01, pattern B). */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fake_mmio_satisfy_after(&ra8_rmac(k_ra8_rmac_port_0)->MPSM, 2U));
  ra8_rmac_phy_link_t link = {.up = true, .speed = k_ra8_rmac_phy_speed_100_fd};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rmac_phy_link_status(k_ra8_rmac_port_0, 5U, &link));
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_rmac_phy_link_status((ra8_rmac_port_t)(uint8_t)k_ra8_rmac_port_count, 5U, &link));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_rmac_phy_link_status(k_ra8_rmac_port_0, 32U, &link));
  link = (ra8_rmac_phy_link_t){.up = true, .speed = k_ra8_rmac_phy_speed_100_fd};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rmac_phy_link_status(k_ra8_rmac_port_0, 3U, &link));
  TEST_ASSERT(!link.up);
  TEST_ASSERT_EQ(k_ra8_rmac_phy_speed_unknown, link.speed);
  TEST_END("rmac MC/DC: phy_args_ok + link_status decisions");
}

/**
 * @test test_mcdc_ra8_rmac_psmcs_clamp
 *
 * @par MC/DC:
 * Two sequential 1-condition decisions in internal_calc_psmcs
 * (libs/ra8_hal/src/ra8_rmac.c@internal_calc_psmcs):
 *  D1: ``if (eswclk_hz == 0U)`` -- returns clamped PSMCS=127.
 *  D2: ``if (mdc_hz == 0U)``    -- returns clamped PSMCS=127.
 * Each is a 1-condition decision so MC/DC reduces to branch coverage
 * (vectors T + F). Threaded through ``ra8_rmac_init`` ->
 * ``internal_program_mac_config`` -> ``internal_calc_psmcs``.
 *
 * Vectors:
 *  - V1: cfg.eswclk_hz=125M, cfg.mdc_hz=1M -> D1=F, D2=F -> compute
 *        PSMCS=61 (125M/1M/2 - 1).
 *  - V2: cfg.eswclk_hz=0,    cfg.mdc_hz=1M -> D1=T -> PSMCS=127.
 *
 * D2=T is unreachable from the public API because
 * ``internal_program_mac_config`` substitutes the cfg.mdc_hz==0 sentinel
 * with ``k_ra8_rmac_mdc_default_hz`` BEFORE calling internal_calc_psmcs.
 * Documented as callable-API impossibility per DO-178C 6.4.4.3.
 */
static void test_mcdc_ra8_rmac_psmcs_clamp(void)
{
  TEST_BEGIN("rmac MC/DC: psmcs clamp on zero clock inputs");
  prep();
  const uint32_t psmcs_shift = 16U;
  const uint32_t psmcs_mask  = 0x7FUL;
  /* V1: both inputs valid -> compute path, PSMCS = (125M/1M/2)-1 = 61. */
  ra8_rmac_config_t cfg = default_cfg();
  cfg.eswclk_hz         = k_rmac_mdio_cfg_eswclk_hz;
  cfg.mdc_hz            = k_rmac_mdio_cfg_mdc_hz;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rmac_init(k_ra8_rmac_port_0, &cfg));
  uint32_t mpic       = ra8_rmac(k_ra8_rmac_port_0)->MPIC;
  uint32_t psmcs_read = (mpic >> psmcs_shift) & psmcs_mask;
  TEST_ASSERT_EQ(61U, psmcs_read);
  /* V2: eswclk_hz = 0 -> D1=T -> PSMCS clamps to max. */
  prep();
  cfg           = default_cfg();
  cfg.eswclk_hz = 0U;
  cfg.mdc_hz    = k_rmac_mdio_cfg_mdc_hz;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rmac_init(k_ra8_rmac_port_0, &cfg));
  mpic       = ra8_rmac(k_ra8_rmac_port_0)->MPIC;
  psmcs_read = (mpic >> psmcs_shift) & psmcs_mask;
  TEST_ASSERT_EQ(0x7FU, psmcs_read);
  TEST_END("rmac MC/DC: psmcs clamp on zero clock inputs");
}

/**
 * @test test_mdio_wait_timeout_legs
 *
 * @par MC/DC:
 * (no compound decisions under test -- the pre-issue drain in
 * ``internal_mdio_drain`` and the post-issue PSME poll in
 * ``internal_mdio_wait`` are single-condition loop exits; each armed
 * ``ra8_fake_mmio_fail_nth_wait`` vector isolates one MPSM wait-loop's
 * timeout leg while every other loop converges on its first poll)
 *
 * @details Every MDIO op runs two bounded waits on MPSM: the pre-issue
 * PSME drain (wait-loop 2n) and the post-issue PSME completion wait
 * (wait-loop 2n+1). C45 ops chain two such pairs (address, then data).
 * Failing the n-th wait-loop drives each timeout leg -- all previously
 * dead on host while the off-target path accepted a pre-armed MMIS1 bit.
 */
static void test_mdio_wait_timeout_legs(void)
{
  TEST_BEGIN("rmac mdio drain + post-wait timeout legs (c22 + c45 stages)");
  prep();
  const ra8_rmac_config_t cfg = default_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rmac_init(k_ra8_rmac_port_0, &cfg));
  volatile uint32_t* mpsm = &ra8_rmac(k_ra8_rmac_port_0)->MPSM;

  /* Wait-loop 0: the c22 write's pre-issue drain times out. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fake_mmio_fail_nth_wait(mpsm, 0U));
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout,
                 ra8_rmac_mdio_c22_write(k_ra8_rmac_port_0, 0x1FU, 0x1AU, 0xBEEFU));

  /* Wait-loop 1: the c22 write's post-issue PSME wait times out
   * (internal_mdio_wait runs its full budget). */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fake_mmio_fail_nth_wait(mpsm, 1U));
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout,
                 ra8_rmac_mdio_c22_write(k_ra8_rmac_port_0, 0x1FU, 0x1AU, 0xBEEFU));

  /* Wait-loop 2: the c45 read's second (data-phase) drain times out
   * after the address phase completed. */
  uint16_t v = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fake_mmio_fail_nth_wait(mpsm, 2U));
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout,
                 ra8_rmac_mdio_c45_read(k_ra8_rmac_port_0, 0x07U, 0x03U, 0x1234U, &v));

  /* Wait-loop 3: the c45 write's data-phase post-issue wait times out
   * after both drains and the address-phase wait completed. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fake_mmio_fail_nth_wait(mpsm, 3U));
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout,
                 ra8_rmac_mdio_c45_write(k_ra8_rmac_port_0, 0x07U, 0x03U, 0x1234U, 0xABCDU));

  TEST_END("rmac mdio drain + post-wait timeout legs (c22 + c45 stages)");
}

int32_t main(void)
{
  test_mdio_c22();
  test_mdio_c45();
  test_mdio_wait_timeout_legs();
  test_phy_reset_happy();
  test_phy_reset_bad_args();
  test_phy_set_advertise();
  test_phy_auto_neg_start();
  test_phy_auto_neg_wait_happy();
  test_phy_auto_neg_wait_timeout();
  test_phy_link_status();
  test_mcdc_ra8_rmac();
  test_mcdc_ra8_rmac_psmcs_clamp();
  (void)fprintf(stderr, "[OK  ] test_ra8_rmac_mdio.c\n");
  return 0;
}
