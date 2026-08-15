/**
 * @file test_ra8_rmac_mgmt_cov.c
 * @brief Coverage-focused unit tests for ra8_rmac_mgmt.c host-reachable legs
 *
 * @details
 * Companion coverage test to test_ra8_rmac.c. It targets the
 * host-reachable branches of ra8_rmac_mgmt.c that the existing suite
 * leaves uncovered, using the same pure-RAM MMIO backing
 * (::ra8_fake_mmap) that every RMAC test relies on:
 *
 *   - ::ra8_rmac_get_status port-out-of-range rejection.
 *   - ::ra8_rmac_clear_status port-out-of-range rejection.
 *   - ::ra8_rmac_phy_auto_neg_wait poll-budget clamp when the
 *     ``timeout_ms * iters_per_ms`` product wraps a uint32_t to 0.
 *   - The MDIO bus-error legs of ::ra8_rmac_phy_reset,
 *     ::ra8_rmac_phy_auto_neg_wait and ::ra8_rmac_phy_link_status,
 *     driven by arming the ra8_fake_mmio fault seam on the RMAC MPSM
 *     register so a chosen MDIO wait-loop times out.
 *
 * The remaining excluded legs of ra8_rmac_mgmt.c are the Clause-22 PHY
 * read-back DATA paths (a non-zero BMCR/BMSR/ANLPAR response):
 * ra8_rmac.c's internal_mpsm_issue writes the MPSM.PRD field to 0 on
 * every read, so host MDIO reads always deliver 0 -- read DATA the
 * wait seam cannot synthesize. Those lines carry GCOVR exclusion
 * markers in the source with a per-group rationale.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_fake_mmio.h"
#include "ra8_mstp.h"
#include "ra8_rmac.h"
#include "ra8_rmac_regs.h"
#include "unity_minimal.h"

/**
 * @enum t_mgmt_irq_t
 * @brief Interrupt-enable masks written into the Ethernet management block.
 *
 * @details
 * The three monitor masks are the full width of each monitor's defined
 * interrupt set, so every implemented bit is exercised; the error mask is a
 * distinctive pattern instead, since the driver must pass it through verbatim
 * rather than interpret it.
 */
typedef enum : uint32_t {
  k_t_mon0_irq_all    = 0x00001FFFU, /**< Monitor 0: all 13 defined sources.   */
  k_t_mon1_irq_all    = 0x0000000FU, /**< Monitor 1: all 4 defined sources.    */
  k_t_mon2_irq_all    = 0x00000007U, /**< Monitor 2: all 3 defined sources.    */
  k_t_err_irq_pattern = 0xCAFEBABEU, /**< Error mask passed through unchanged. */
} t_mgmt_irq_t;

/**
 * @enum test_rmac_mgmt_addr_t
 * @brief Small PHY address used by the reachable poll-budget test.
 */
typedef enum : uint8_t {
  k_test_phy_addr = 1U, /**< In-range 5-bit MDIO PHY address. */
} test_rmac_mgmt_addr_t;

/**
 * @enum test_rmac_mgmt_cap_t
 * @brief Timeout value that overflows the internal poll-budget product.
 *
 * @details
 * ra8_rmac_phy_auto_neg_wait computes ``cap = timeout_ms * 100``. The
 * multiply is a 32-bit unsigned wrap; 0x40000000 * 100 == 0 (mod 2^32),
 * so this value forces the ``cap == 0 -> cap = 1`` clamp branch.
 */
typedef enum : uint32_t {
  k_test_cap_wrap_ms = 0x40000000U, /**< 2^30; *100 wraps uint32_t to 0. */
} test_rmac_mgmt_cap_t;

/**
 * @brief Reset the MMIO backing and bring port 0 up for each test.
 *
 * @details Mirrors test_ra8_rmac.c's fixture so ra8_rmac(port) points at
 * a live, zeroed register window before each case.
 */
static void prep(void)
{
  ra8_fake_mmap_reset();
  ra8_fake_mmio_reset();
  (void)ra8_mstp_init();
}

/**
 * @brief Minimal valid init config (GMII / 1000 Mbit / full duplex).
 *
 * @return A config accepted by ra8_rmac_init.
 */
static ra8_rmac_config_t default_cfg(void)
{
  return (ra8_rmac_config_t){
    .rx_filter       = k_ra8_rmac_mrafc_unicast_match,
    .err_irq_enable  = k_t_err_irq_pattern,
    .mon0_irq_enable = k_t_mon0_irq_all,
    .mon1_irq_enable = k_t_mon1_irq_all,
    .mon2_irq_enable = k_t_mon2_irq_all,
    .phy_interface   = k_ra8_rmac_pis_gmii,
    .link_speed      = k_ra8_rmac_lsc_1000mbit,
    .duplex          = k_ra8_rmac_duplex_full,
  };
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the single-condition
 * ``if (!internal_port_ok(port))`` guard of ra8_rmac_get_status; no
 * ``&&`` or ``||`` in the code path this case touches)
 */
static void test_get_status_bad_port(void)
{
  TEST_BEGIN("rmac_get_status rejects out-of-range port");
  prep();
  ra8_rmac_status_t snap = {};
  /* Valid out pointer but port == count -> internal_port_ok is false,
   * so the function must reject before touching any register. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_rmac_get_status((ra8_rmac_port_t)(uint8_t)k_ra8_rmac_port_count, &snap));
  TEST_END("rmac_get_status rejects out-of-range port");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the single-condition
 * ``if (!internal_port_ok(port))`` guard of ra8_rmac_clear_status; no
 * ``&&`` or ``||`` in the code path this case touches)
 */
static void test_clear_status_bad_port(void)
{
  TEST_BEGIN("rmac_clear_status rejects out-of-range port");
  prep();
  /* clear_status has no null-pointer argument, so the port guard is the
   * only early-return leg; drive it with port == count. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_rmac_clear_status((ra8_rmac_port_t)(uint8_t)k_ra8_rmac_port_count,
                                       0xFFFFFFFFU,
                                       0xFFFFFFFFU,
                                       0xFFFFFFFFU,
                                       0xFFFFFFFFU));
  TEST_END("rmac_clear_status rejects out-of-range port");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the single-condition
 * ``if (cap == 0U)`` poll-budget clamp of ra8_rmac_phy_auto_neg_wait.
 * The compound ``internal_phy_args_ok`` guard on entry is held true by
 * a valid (port, phy_addr) pair and is MC/DC-covered by test_ra8_rmac.c)
 */
static void test_auto_neg_wait_cap_wrap(void)
{
  TEST_BEGIN("rmac_phy_auto_neg_wait clamps wrapped poll budget to 1");
  prep();
  const ra8_rmac_config_t cfg = default_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rmac_init(k_ra8_rmac_port_0, &cfg));

  /* timeout_ms is non-zero, so the ternary picks the multiply branch;
   * 0x40000000 * 100 wraps to 0, exercising the ``cap = 1U`` clamp.
   * The host MDIO read returns BMSR = 0 (no link/AN), so the single
   * clamped iteration completes without a match and the call returns
   * hw_timeout with the link marked down and speed unknown. */
  ra8_rmac_phy_link_t link = {};
  const ra8_err_t     r    = ra8_rmac_phy_auto_neg_wait(k_ra8_rmac_port_0,
                                                        (uint8_t)k_test_phy_addr,
                                                        (uint32_t)k_test_cap_wrap_ms,
                                                        &link);
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, r);
  TEST_ASSERT(!link.up);
  TEST_ASSERT_EQ(k_ra8_rmac_phy_speed_unknown, link.speed);
  TEST_END("rmac_phy_auto_neg_wait clamps wrapped poll budget to 1");
}

/**
 * @test test_phy_reset_mdio_error_legs
 *
 * @par MC/DC:
 * (no compound decisions under test -- the ``w != k_ra8_ok`` and
 * ``r != k_ra8_ok`` guards in ra8_rmac_phy_reset are single-condition
 * ``if`` statements; each armed MPSM wait-loop fault drives exactly one
 * of them)
 *
 * @details ra8_rmac_phy_reset issues a BMCR write (MPSM wait-loops 0-1)
 * then polls BMCR via reads (wait-loops 2-3, ...). Failing wait-loop 0
 * fails the write's drain and lands on the bmcr-write-error leg;
 * failing wait-loop 2 fails the first read's drain and lands on the
 * bmcr-read-error leg. Both legs were host-dead while the fake MDIO
 * path auto-completed via MMIS1 arming.
 */
static void test_phy_reset_mdio_error_legs(void)
{
  TEST_BEGIN("rmac_phy_reset surfaces the BMCR write- and read-error legs");
  prep();
  const ra8_rmac_config_t cfg = default_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rmac_init(k_ra8_rmac_port_0, &cfg));
  volatile uint32_t* mpsm = &ra8_rmac(k_ra8_rmac_port_0)->MPSM;

  /* Write leg: the BMCR write's drain (wait-loop 0) times out. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fake_mmio_fail_nth_wait(mpsm, 0U));
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout,
                 ra8_rmac_phy_reset(k_ra8_rmac_port_0, (uint8_t)k_test_phy_addr));

  /* Read leg: the write completes (loops 0-1), the first BMCR read's
   * drain (wait-loop 2) times out. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fake_mmio_fail_nth_wait(mpsm, 2U));
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout,
                 ra8_rmac_phy_reset(k_ra8_rmac_port_0, (uint8_t)k_test_phy_addr));

  TEST_END("rmac_phy_reset surfaces the BMCR write- and read-error legs");
}

/**
 * @test test_auto_neg_wait_mdio_error_leg
 *
 * @par MC/DC:
 * (no compound decisions under test -- the ``r != k_ra8_ok`` guard in
 * ra8_rmac_phy_auto_neg_wait's poll loop is a single-condition ``if``,
 * driven by failing the BMSR read's MPSM drain)
 *
 * @details Arms the ra8_fake_mmio seam on MPSM to fail so the first BMSR
 * read inside the auto-negotiation poll loop reports an MDIO error,
 * which the function must propagate instead of spinning its budget.
 */
static void test_auto_neg_wait_mdio_error_leg(void)
{
  TEST_BEGIN("rmac_phy_auto_neg_wait propagates an MDIO read error");
  prep();
  const ra8_rmac_config_t cfg = default_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rmac_init(k_ra8_rmac_port_0, &cfg));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fake_mmio_fail_wait(&ra8_rmac(k_ra8_rmac_port_0)->MPSM));
  ra8_rmac_phy_link_t link = {.up = true, .speed = k_ra8_rmac_phy_speed_100_fd};
  TEST_ASSERT_EQ(
    k_ra8_err_hw_timeout,
    ra8_rmac_phy_auto_neg_wait(k_ra8_rmac_port_0, (uint8_t)k_test_phy_addr, 1U, &link));
  /* The error surfaced before any link state could be read back. */
  TEST_ASSERT(!link.up);
  TEST_ASSERT_EQ(k_ra8_rmac_phy_speed_unknown, link.speed);

  TEST_END("rmac_phy_auto_neg_wait propagates an MDIO read error");
}

/**
 * @test test_link_status_mdio_error_leg
 *
 * @par MC/DC:
 * (no compound decisions under test -- the ``r != k_ra8_ok`` guard in
 * ra8_rmac_phy_link_status is a single-condition ``if``, driven by
 * failing the BMSR read's MPSM drain)
 *
 * @details Arms the ra8_fake_mmio seam on MPSM to fail so the BMSR read
 * inside ra8_rmac_phy_link_status reports an MDIO error, which the
 * function must propagate with the link left marked down.
 */
static void test_link_status_mdio_error_leg(void)
{
  TEST_BEGIN("rmac_phy_link_status propagates an MDIO read error");
  prep();
  const ra8_rmac_config_t cfg = default_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rmac_init(k_ra8_rmac_port_0, &cfg));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fake_mmio_fail_wait(&ra8_rmac(k_ra8_rmac_port_0)->MPSM));
  ra8_rmac_phy_link_t link = {.up = true, .speed = k_ra8_rmac_phy_speed_100_fd};
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout,
                 ra8_rmac_phy_link_status(k_ra8_rmac_port_0, (uint8_t)k_test_phy_addr, &link));
  TEST_ASSERT(!link.up);
  TEST_ASSERT_EQ(k_ra8_rmac_phy_speed_unknown, link.speed);

  TEST_END("rmac_phy_link_status propagates an MDIO read error");
}

int32_t main(void)
{
  test_get_status_bad_port();
  test_clear_status_bad_port();
  test_auto_neg_wait_cap_wrap();
  test_phy_reset_mdio_error_legs();
  test_auto_neg_wait_mdio_error_leg();
  test_link_status_mdio_error_leg();
  return 0;
}
