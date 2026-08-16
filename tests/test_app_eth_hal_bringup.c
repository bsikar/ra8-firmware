/**
 * @file test_app_eth_hal_bringup.c
 * @brief Integration test: the eth_hal_bringup example's ESWM/COMA/RGMII flow
 *
 * @details
 * Mirrors the bring-up sequence run by
 * ``examples/ek_ra8d2/hw_pending/eth_hal_bringup/main.c`` (::ehb_run_once):
 *
 *   ra8_cgc_eswclk_init -> ra8_mstp_enable(k_ra8_mstp_eswm)
 *     -> ra8_eth_coma_bringup -> ra8_eth_rgmii_select(k_ra8_eth_mii_port_1)
 *
 * Every register access goes through the host tests/mocks/ra8_fake_mmap.c
 * shim, and the CABPIRM.BPR bounded wait through the ra8_fake_mmio fault
 * seam, so the example's exact call chain is exercised on host with the two
 * new #581 HAL primitives (ra8_eth_coma_bringup + ra8_eth_rgmii_select) doing
 * the real register work. This is the "test the example, not just compile it"
 * counterpart to the compile-gated ARM app: eth is HW-blocked on silicon
 * (#21), so this host integration test is the authoritative functional check.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_eth.h"
#include "ra8_eth_coma.h"
#include "ra8_ether_regs.h"
#include "ra8_fake_mmap.h"
#include "ra8_fake_mmio.h"
#include "ra8_mstp.h"
#include "ra8_mstp_regs.h"
#include "unity_minimal.h"

/**
 * @brief Reset all fake peripheral state before a case.
 *
 * @pre None.
 * @post ra8_fake_mmap register window cleared; fault seam disarmed; MSTP
 *       controller re-initialised.
 *
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
static void reset_world(void)
{
  ra8_fake_mmap_reset();
  ra8_fake_mmio_reset();
  (void)ra8_mstp_init();
}

/**
 * @brief Golden bring-up: eswclk -> ESWM MSTP -> COMA -> RGMII port 1.
 *
 * @details
 * Drives the example's four-call sequence and asserts each ``ra8_err_t`` is
 * ``k_ra8_ok``, then asserts the register effects the two HAL primitives are
 * responsible for: COMA.RCEC fanned out (RCE | ACE[6:0]), CABPIRM.BPIOG
 * kicked, ESWM.MIICR1 = TXCIDE | RGMII, and MIIRR.RGRST1 enabled.
 *
 * @par MC/DC:
 * The example's ::ehb_run_once has four sequential single-condition
 * ``rc != k_ra8_ok`` guards. This case supplies the all-``false`` vector
 * (every step succeeds); ::test_eth_hal_bringup_coma_timeout supplies the
 * ``true`` vector for the COMA guard. The eswclk / MSTP guards cannot fail
 * under RA8_OFF_TARGET (their polls auto-satisfy), matching the board
 * bring-up's GCOVR_EXCL_LINE treatment of those legs.
 */
static void test_eth_hal_bringup_ok(void)
{
  reset_world();
  TEST_BEGIN("eth_hal_bringup: eswclk + coma + rgmii golden");

  TEST_ASSERT_EQ(k_ra8_ok, ra8_cgc_eswclk_init());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mstp_enable(k_ra8_mstp_eswm));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_eth_coma_bringup());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_eth_rgmii_select(k_ra8_eth_mii_port_1));

  /* COMA effects. */
  TEST_ASSERT_EQ(k_ra8_coma_rcec_rce | (uint32_t)k_ra8_coma_rcec_ace_mask, *ra8_coma_rcec());
  TEST_ASSERT_EQ(k_ra8_coma_cabpirm_bpiog,
                 *ra8_coma_cabpirm() & (uint32_t)k_ra8_coma_cabpirm_bpiog);
  /* RGMII media-select effects on port 1. */
  TEST_ASSERT_EQ(k_ra8_eswm_miicr_txcide | (uint32_t)k_ra8_eswm_miicr_miisel_rgmii,
                 *ra8_eswm_miicr1());
  TEST_ASSERT_EQ(k_ra8_eswm_miirr_rgrst1, *ra8_eswm_miirr() & (uint32_t)k_ra8_eswm_miirr_rgrst1);
  /* Port 1 bring-up must not disturb port 0's media control register. */
  TEST_ASSERT_EQ(0U, *ra8_eswm_miicr0());

  TEST_END("eth_hal_bringup: eswclk + coma + rgmii golden");
}

/**
 * @brief Failure vector: the COMA CABPIRM.BPR wait times out.
 *
 * @details
 * Arms the buffer-pool-ready register via ra8_fake_mmio_fail_wait so BPR
 * never asserts. The example returns early from ::ehb_run_once with the
 * hardware-timeout error (and prints "bringup FAIL"); this asserts
 * ra8_eth_coma_bringup surfaces ``k_ra8_err_hw_timeout`` and that the
 * downstream RGMII select never programmed MIICR1.
 *
 * @par MC/DC:
 * Supplies the ``true`` vector of the COMA ``rc != k_ra8_ok`` guard in
 * ::ehb_run_once; ::test_eth_hal_bringup_ok supplies the ``false`` vector.
 */
static void test_eth_hal_bringup_coma_timeout(void)
{
  reset_world();
  TEST_BEGIN("eth_hal_bringup: COMA CABPIRM.BPR timeout aborts the flow");

  TEST_ASSERT_EQ(k_ra8_ok, ra8_cgc_eswclk_init());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mstp_enable(k_ra8_mstp_eswm));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fake_mmio_fail_wait((const volatile void*)ra8_coma_cabpirm()));
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_eth_coma_bringup());

  /* The example returns before ra8_eth_rgmii_select, so MIICR1 stays 0. */
  TEST_ASSERT_EQ(0U, *ra8_eswm_miicr1());

  ra8_fake_mmio_reset();
  TEST_END("eth_hal_bringup: COMA CABPIRM.BPR timeout aborts the flow");
}

int main(void)
{
  test_eth_hal_bringup_ok();
  test_eth_hal_bringup_coma_timeout();
  return 0;
}
