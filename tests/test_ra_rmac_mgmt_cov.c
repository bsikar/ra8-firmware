/**
 * @file test_ra_rmac_mgmt_cov.c
 * @brief Coverage-focused unit tests for ra_rmac_mgmt.c host-reachable legs
 *
 * @details
 * Companion coverage test to test_ra_rmac.c. It targets the three
 * host-reachable branches of ra_rmac_mgmt.c that the existing suite
 * leaves uncovered, using the same pure-RAM MMIO backing
 * (::ra_sim_mmap) that every RMAC test relies on:
 *
 *   - ::ra_rmac_get_status port-out-of-range rejection.
 *   - ::ra_rmac_clear_status port-out-of-range rejection.
 *   - ::ra_rmac_phy_auto_neg_wait poll-budget clamp when the
 *     ``timeout_ms * iters_per_ms`` product wraps a uint32_t to 0.
 *
 * The remaining uncovered legs of ra_rmac_mgmt.c are the Clause-22 PHY
 * management read-back paths (BMCR/BMSR/ANLPAR responses and MDIO bus
 * errors). Those cannot be driven from the host: ra_rmac.c's
 * internal_mpsm_issue writes the MPSM.PRD field to 0 on every read and
 * unconditionally arms the MMIS1 completion bit, so under
 * RA_SIMULATOR_MODE every ra_rmac_mdio_c22_read returns k_ra_ok with
 * data 0 and no MDIO transaction can fail. Those lines carry
 * GCOVR_EXCL_LINE markers in the source with a per-group rationale.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8d2_rmac_regs.h"
#include "ra_err.h"
#include "ra_mstp.h"
#include "ra_rmac.h"
#include "ra_sim_mmap.h"
#include "unity_minimal.h"

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
 * ra_rmac_phy_auto_neg_wait computes ``cap = timeout_ms * 100``. The
 * multiply is a 32-bit unsigned wrap; 0x40000000 * 100 == 0 (mod 2^32),
 * so this value forces the ``cap == 0 -> cap = 1`` clamp branch.
 */
typedef enum : uint32_t {
  k_test_cap_wrap_ms = 0x40000000U, /**< 2^30; *100 wraps uint32_t to 0. */
} test_rmac_mgmt_cap_t;

/**
 * @brief Reset the MMIO backing and bring port 0 up for each test.
 *
 * @details Mirrors test_ra_rmac.c's fixture so ra_rmac(port) points at
 * a live, zeroed register window before each case.
 */
static void prep(void)
{
  ra_sim_mmap_reset();
  (void)ra_mstp_init();
}

/**
 * @brief Minimal valid init config (GMII / 1000 Mbit / full duplex).
 *
 * @return A config accepted by ra_rmac_init.
 */
static ra_rmac_config_t default_cfg(void)
{
  return (ra_rmac_config_t){
    .rx_filter       = k_ra_rmac_mrafc_unicast_match,
    .err_irq_enable  = 0xCAFEBABEU,
    .mon0_irq_enable = 0x00001FFFU,
    .mon1_irq_enable = 0x0000000FU,
    .mon2_irq_enable = 0x00000007U,
    .phy_interface   = k_ra_rmac_pis_gmii,
    .link_speed      = k_ra_rmac_lsc_1000mbit,
    .duplex          = k_ra_rmac_duplex_full,
  };
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the single-condition
 * ``if (!internal_port_ok(port))`` guard of ra_rmac_get_status; no
 * ``&&`` or ``||`` in the code path this case touches)
 */
static void test_get_status_bad_port(void)
{
  TEST_BEGIN("rmac_get_status rejects out-of-range port");
  prep();
  ra_rmac_status_t snap = {};
  /* Valid out pointer but port == count -> internal_port_ok is false,
   * so the function must reject before touching any register. */
  TEST_ASSERT_EQ(k_ra_err_invalid_arg,
                 ra_rmac_get_status((ra_rmac_port_t)(uint8_t)k_ra_rmac_port_count, &snap));
  TEST_END("rmac_get_status rejects out-of-range port");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the single-condition
 * ``if (!internal_port_ok(port))`` guard of ra_rmac_clear_status; no
 * ``&&`` or ``||`` in the code path this case touches)
 */
static void test_clear_status_bad_port(void)
{
  TEST_BEGIN("rmac_clear_status rejects out-of-range port");
  prep();
  /* clear_status has no null-pointer argument, so the port guard is the
   * only early-return leg; drive it with port == count. */
  TEST_ASSERT_EQ(k_ra_err_invalid_arg,
                 ra_rmac_clear_status((ra_rmac_port_t)(uint8_t)k_ra_rmac_port_count,
                                      0xFFFFFFFFU,
                                      0xFFFFFFFFU,
                                      0xFFFFFFFFU,
                                      0xFFFFFFFFU));
  TEST_END("rmac_clear_status rejects out-of-range port");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the single-condition
 * ``if (cap == 0U)`` poll-budget clamp of ra_rmac_phy_auto_neg_wait.
 * The compound ``internal_phy_args_ok`` guard on entry is held true by
 * a valid (port, phy_addr) pair and is MC/DC-covered by test_ra_rmac.c)
 */
static void test_auto_neg_wait_cap_wrap(void)
{
  TEST_BEGIN("rmac_phy_auto_neg_wait clamps wrapped poll budget to 1");
  prep();
  const ra_rmac_config_t cfg = default_cfg();
  TEST_ASSERT_EQ(k_ra_ok, ra_rmac_init(k_ra_rmac_port_0, &cfg));

  /* timeout_ms is non-zero, so the ternary picks the multiply branch;
   * 0x40000000 * 100 wraps to 0, exercising the ``cap = 1U`` clamp.
   * The host MDIO read returns BMSR = 0 (no link/AN), so the single
   * clamped iteration completes without a match and the call returns
   * hw_timeout with the link marked down and speed unknown. */
  ra_rmac_phy_link_t link = {};
  const ra_err_t     r    = ra_rmac_phy_auto_neg_wait(k_ra_rmac_port_0,
                                                      (uint8_t)k_test_phy_addr,
                                                      (uint32_t)k_test_cap_wrap_ms,
                                                      &link);
  TEST_ASSERT_EQ(k_ra_err_hw_timeout, r);
  TEST_ASSERT(!link.up);
  TEST_ASSERT_EQ(k_ra_rmac_phy_speed_unknown, link.speed);
  TEST_END("rmac_phy_auto_neg_wait clamps wrapped poll budget to 1");
}

int32_t main(void)
{
  test_get_status_bad_port();
  test_clear_status_bad_port();
  test_auto_neg_wait_cap_wrap();
  (void)fprintf(stderr, "[OK  ] test_ra_rmac_mgmt_cov.c\n");
  return 0;
}
