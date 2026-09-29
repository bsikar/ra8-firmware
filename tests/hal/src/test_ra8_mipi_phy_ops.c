/**
 * @file test_ra8_mipi_phy_ops.c
 * @brief Unit tests for the MIPI D-PHY observers, pure helpers and dual mode
 *
 * @details
 * Covers the surface implemented in ``libs/ra8_hal/src/ra8_mipi_phy_ops.c``:
 * the derived lifecycle / mode observers, the decoded DPHYSFR snapshot, the
 * PLL-frequency and line-rate arithmetic, the dry-run timing lookup and the
 * dual-mode arbitration shadow. Every prototype exercised here had no
 * definition in the tree before #1430, so no sibling suite reached them.
 *
 * Sibling suites: test_ra8_mipi_phy_init.c (bring-up, status, dispatch) and
 * test_ra8_mipi_phy_lanes.c (lane / clock / PLL setters, MC/DC vectors).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_mipi_phy.h"
#include "ra8_mipi_phy_regs.h"
#include "ra8_mstp_regs.h"
#include "unity_minimal.h"

/**
 * @enum test_phy_ops_const_t
 * @brief Register and argument constants used across the cases.
 *
 * @details
 * ``k_test_ops_mstpc_bit`` mirrors the provisional MSTPCRC slot the driver
 * ungates (HUM Ch 64.4.2 p 3838); the tests set and clear it directly so the
 * observers can be seen switching between the gated and ungated answers.
 */
typedef enum : uint32_t {
  k_test_ops_mstpc_bit    = 13U,        /**< Provisional MIPI PHY MSTPC slot. */
  k_test_ops_mosc_mhz     = 24U,        /**< In-window MOSC frequency.        */
  k_test_ops_mosc_lo      = 7U,         /**< Just below the 8 MHz floor.      */
  k_test_ops_mosc_hi      = 49U,        /**< Just above the 48 MHz ceiling.   */
  k_test_ops_nmul         = 100U,       /**< Mid-range NMUL (40..375).        */
  k_test_ops_pclka_dsi    = 125U,       /**< HUM Table 64.2 PCLKA bucket.     */
  k_test_ops_pclka_csi    = 100U,       /**< HUM Table 64.3 PCLKA bucket.     */
  k_test_ops_pclka_absent = 61U,        /**< No table row carries this PCLKA. */
  k_test_ops_rate_mbps    = 250U,       /**< In-window per-lane line rate.    */
  k_test_ops_rate_lo      = 79U,        /**< Just below the 80 Mbps floor.    */
  k_test_ops_rate_hi      = 721U,       /**< Just above the 720 Mbps ceiling. */
  k_test_ops_mode_bad     = 9U,         /**< Value outside the mode enum.     */
  k_test_ops_dual_bad     = 9U,         /**< Value outside the dual-mode enum.*/
  k_test_ops_hz_100mhz    = 100000000U, /**< 100 MHz expressed in Hz.         */
  k_test_ops_hz_100p9mhz  = 100900000U, /**< 100.9 MHz -- floors to 100.      */
  k_test_ops_hz_sub_mhz   = 999999U,    /**< Under 1 MHz -- floors to 0.      */
  k_test_ops_hz_300mhz    = 300000000U, /**< 300 MHz -- over the RFREQ range. */
} test_phy_ops_const_t;

/**
 * @brief Clear the MIPI PHY module-stop bit so register reads are legal.
 */
static void internal_ungate(void)
{
  volatile uint32_t* mstpc = &ra8_mstp()->MSTPCRC;
  *mstpc                   = *mstpc & ~((uint32_t)1U << (uint32_t)k_test_ops_mstpc_bit);
}

/**
 * @brief Set the MIPI PHY module-stop bit, as it reads after reset.
 */
static void internal_gate(void)
{
  volatile uint32_t* mstpc = &ra8_mstp()->MSTPCRC;
  *mstpc                   = *mstpc | ((uint32_t)1U << (uint32_t)k_test_ops_mstpc_bit);
}

/**
 * @brief A PLL block the arithmetic helpers can be checked against by hand.
 */
static ra8_mipi_phy_pll_t internal_pll(void)
{
  ra8_mipi_phy_pll_t pll = {
    .idiv     = k_ra8_mipi_phy_pll_idiv_1,
    .pmul     = k_ra8_mipi_phy_pll_pmul_1,
    .nfmul    = k_ra8_mipi_phy_pll_nfmul_0,
    .nmul_int = (uint16_t)k_test_ops_nmul,
  };
  return pll;
}

/** @brief The state observer walks the start-up procedure one flag at a time. */
static void test_get_state_walks_the_startup_flags(void)
{
  ra8_fake_mmap_reset();
  internal_gate();
  TEST_ASSERT_EQ(k_ra8_mipi_phy_state_off, ra8_mipi_phy_get_state());

  internal_ungate();
  TEST_ASSERT_EQ(k_ra8_mipi_phy_state_idle, ra8_mipi_phy_get_state());

  *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_sfr) = (uint32_t)k_ra8_mipi_phy_sfr_pwrsf;
  TEST_ASSERT_EQ(k_ra8_mipi_phy_state_ldo_up, ra8_mipi_phy_get_state());

  *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_sfr) = (uint32_t)k_ra8_mipi_phy_sfr_ready_mask;
  TEST_ASSERT_EQ(k_ra8_mipi_phy_state_pll_run, ra8_mipi_phy_get_state());

  *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_ocr) = (uint32_t)k_ra8_mipi_phy_ocr_dphyen;
  TEST_ASSERT_EQ(k_ra8_mipi_phy_state_run, ra8_mipi_phy_get_state());
}

/** @brief The mode observer reads DPHYMDC, and the reset value while gated. */
static void test_get_active_mode_reads_dphymdc(void)
{
  ra8_fake_mmap_reset();
  internal_ungate();
  TEST_ASSERT_EQ(k_ra8_mipi_phy_mode_csi_device, ra8_mipi_phy_get_active_mode());

  *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_mdc) = (uint32_t)k_ra8_mipi_phy_mdc_hosten;
  TEST_ASSERT_EQ(k_ra8_mipi_phy_mode_dsi_host, ra8_mipi_phy_get_active_mode());

  /* Gated: the host bit is still in the backing store, the answer is the
   * documented reset value instead of a read of an unreachable register. */
  internal_gate();
  TEST_ASSERT_EQ(k_ra8_mipi_phy_mode_csi_device, ra8_mipi_phy_get_active_mode());
}

/** @brief Every member of the decoded snapshot is written from DPHYSFR. */
static void test_get_status_decoded(void)
{
  ra8_fake_mmap_reset();
  internal_ungate();
  ra8_mipi_phy_status_decoded_t snap = {0};

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_mipi_phy_get_status_decoded(nullptr));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_phy_get_status_decoded(&snap));
  TEST_ASSERT_EQ(0, (int)snap.raw);
  TEST_ASSERT_EQ(false, snap.ldo_ready);
  TEST_ASSERT_EQ(false, snap.pll_locked);
  TEST_ASSERT_EQ(false, snap.phy_ready);

  *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_sfr) = (uint32_t)k_ra8_mipi_phy_sfr_pwrsf;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_phy_get_status_decoded(&snap));
  TEST_ASSERT_EQ(true, snap.ldo_ready);
  TEST_ASSERT_EQ(false, snap.pll_locked);
  TEST_ASSERT_EQ(false, snap.phy_ready);

  *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_sfr) = (uint32_t)k_ra8_mipi_phy_sfr_ready_mask;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_phy_get_status_decoded(&snap));
  TEST_ASSERT_EQ((int)k_ra8_mipi_phy_sfr_ready_mask, (int)snap.raw);
  TEST_ASSERT_EQ(true, snap.ldo_ready);
  TEST_ASSERT_EQ(true, snap.pll_locked);
  TEST_ASSERT_EQ(true, snap.phy_ready);
}

/** @brief The PLL arithmetic matches the HUM formula and bounds its inputs. */
static void test_compute_pll_freq_and_lane_rate(void)
{
  const ra8_mipi_phy_pll_t pll  = internal_pll();
  uint32_t                 mhz  = 0U;
  uint32_t                 mbps = 0U;

  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_mipi_phy_compute_pll_freq(nullptr, (uint8_t)k_test_ops_mosc_mhz, &mhz));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_mipi_phy_compute_pll_freq(&pll, (uint8_t)k_test_ops_mosc_mhz, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_mipi_phy_compute_pll_freq(&pll, (uint8_t)k_test_ops_mosc_lo, &mhz));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_mipi_phy_compute_pll_freq(&pll, (uint8_t)k_test_ops_mosc_hi, &mhz));

  /* f = 24 MHz * 100 / 1 / 1 = 2400 MHz; line rate = f / 2. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_mipi_phy_compute_pll_freq(&pll, (uint8_t)k_test_ops_mosc_mhz, &mhz));
  TEST_ASSERT_EQ((int)((uint32_t)k_test_ops_mosc_mhz * (uint32_t)k_test_ops_nmul), (int)mhz);

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_mipi_phy_compute_lane_rate_mbps(&pll, (uint8_t)k_test_ops_mosc_hi, &mbps));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_mipi_phy_compute_lane_rate_mbps(&pll, (uint8_t)k_test_ops_mosc_mhz, &mbps));
  TEST_ASSERT_EQ((int)(mhz / 2U), (int)mbps);
}

/** @brief The dry-run lookup returns a row and leaves DPHYTIM1..6 alone. */
static void test_lookup_timing_touches_no_register(void)
{
  ra8_fake_mmap_reset();
  internal_ungate();
  ra8_mipi_phy_timing_t tim = {0};

  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_mipi_phy_lookup_timing(k_ra8_mipi_phy_mode_dsi_host,
                                            (uint8_t)k_test_ops_pclka_dsi,
                                            (uint16_t)k_test_ops_rate_mbps,
                                            nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_mipi_phy_lookup_timing((ra8_mipi_phy_mode_t)k_test_ops_mode_bad,
                                            (uint8_t)k_test_ops_pclka_dsi,
                                            (uint16_t)k_test_ops_rate_mbps,
                                            &tim));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_mipi_phy_lookup_timing(k_ra8_mipi_phy_mode_dsi_host,
                                            (uint8_t)k_test_ops_pclka_dsi,
                                            (uint16_t)k_test_ops_rate_lo,
                                            &tim));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_mipi_phy_lookup_timing(k_ra8_mipi_phy_mode_dsi_host,
                                            (uint8_t)k_test_ops_pclka_dsi,
                                            (uint16_t)k_test_ops_rate_hi,
                                            &tim));
  TEST_ASSERT_EQ(k_ra8_err_not_supported,
                 ra8_mipi_phy_lookup_timing(k_ra8_mipi_phy_mode_dsi_host,
                                            (uint8_t)k_test_ops_pclka_absent,
                                            (uint16_t)k_test_ops_rate_mbps,
                                            &tim));

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_mipi_phy_lookup_timing(k_ra8_mipi_phy_mode_dsi_host,
                                            (uint8_t)k_test_ops_pclka_dsi,
                                            (uint16_t)k_test_ops_rate_mbps,
                                            &tim));
  TEST_ASSERT_NE(0, (int)tim.tinit);
  /* The dry run must not have programmed anything. */
  TEST_ASSERT_EQ(0, *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_tim1));
  TEST_ASSERT_EQ(0, *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_tim6));

  /* The CSI table is reachable through the same entry point. */
  ra8_mipi_phy_timing_t csi = {0};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_mipi_phy_lookup_timing(k_ra8_mipi_phy_mode_csi_device,
                                            (uint8_t)k_test_ops_pclka_csi,
                                            (uint16_t)k_test_ops_rate_mbps,
                                            &csi));
  TEST_ASSERT_NE(0, (int)csi.tinit);
}

/** @brief select_timing still programs the row it shares the matcher with. */
static void test_select_timing_still_programs_the_row(void)
{
  ra8_fake_mmap_reset();
  internal_ungate();
  ra8_mipi_phy_timing_t looked = {0};
  ra8_mipi_phy_timing_t chosen = {0};

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_mipi_phy_lookup_timing(k_ra8_mipi_phy_mode_dsi_host,
                                            (uint8_t)k_test_ops_pclka_dsi,
                                            (uint16_t)k_test_ops_rate_mbps,
                                            &looked));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_mipi_phy_select_timing(k_ra8_mipi_phy_mode_dsi_host,
                                            (uint8_t)k_test_ops_pclka_dsi,
                                            (uint16_t)k_test_ops_rate_mbps,
                                            &chosen));
  TEST_ASSERT_EQ((int)looked.tinit, (int)chosen.tinit);
  TEST_ASSERT_EQ((int)looked.tlpx, (int)chosen.tlpx);
  TEST_ASSERT_EQ((int)chosen.tinit, (int)*ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_tim1));
  TEST_ASSERT_EQ((int)chosen.tlpx, (int)*ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_tim6));
}

/** @brief The dual-mode shadow stores, reads back and arbitrates. */
static void test_dual_mode_policy(void)
{
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_mipi_phy_set_dual_mode((ra8_mipi_phy_dual_mode_t)k_test_ops_dual_bad));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_phy_set_dual_mode(k_ra8_mipi_phy_dual_off));
  TEST_ASSERT_EQ(k_ra8_mipi_phy_dual_off, ra8_mipi_phy_get_dual_mode());
  TEST_ASSERT_EQ(true, ra8_mipi_phy_dual_mode_can_acquire(k_ra8_mipi_phy_mode_dsi_host));
  TEST_ASSERT_EQ(true, ra8_mipi_phy_dual_mode_can_acquire(k_ra8_mipi_phy_mode_csi_device));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_phy_set_dual_mode(k_ra8_mipi_phy_dual_alternate));
  TEST_ASSERT_EQ(k_ra8_mipi_phy_dual_alternate, ra8_mipi_phy_get_dual_mode());
  TEST_ASSERT_EQ(true, ra8_mipi_phy_dual_mode_can_acquire(k_ra8_mipi_phy_mode_dsi_host));
  TEST_ASSERT_EQ(true, ra8_mipi_phy_dual_mode_can_acquire(k_ra8_mipi_phy_mode_csi_device));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_phy_set_dual_mode(k_ra8_mipi_phy_dual_dsi_priority));
  TEST_ASSERT_EQ(k_ra8_mipi_phy_dual_dsi_priority, ra8_mipi_phy_get_dual_mode());
  TEST_ASSERT_EQ(true, ra8_mipi_phy_dual_mode_can_acquire(k_ra8_mipi_phy_mode_dsi_host));
  TEST_ASSERT_EQ(false, ra8_mipi_phy_dual_mode_can_acquire(k_ra8_mipi_phy_mode_csi_device));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_phy_set_dual_mode(k_ra8_mipi_phy_dual_csi_priority));
  TEST_ASSERT_EQ(k_ra8_mipi_phy_dual_csi_priority, ra8_mipi_phy_get_dual_mode());
  TEST_ASSERT_EQ(false, ra8_mipi_phy_dual_mode_can_acquire(k_ra8_mipi_phy_mode_dsi_host));
  TEST_ASSERT_EQ(true, ra8_mipi_phy_dual_mode_can_acquire(k_ra8_mipi_phy_mode_csi_device));

  /* A requestor outside the enum is blocked whatever the policy. */
  TEST_ASSERT_EQ(false,
                 ra8_mipi_phy_dual_mode_can_acquire(
                   (ra8_mipi_phy_mode_t)k_test_ops_mode_bad));

  /* Leave the shadow in its default for any later case. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_phy_set_dual_mode(k_ra8_mipi_phy_dual_off));
}

/** @brief The Hz wrapper floors to MHz and keeps the RFREQ range honest. */
static void test_set_pclka_freq_hz(void)
{
  ra8_fake_mmap_reset();
  internal_ungate();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_phy_set_pclka_freq_hz((uint32_t)k_test_ops_hz_100mhz));
  TEST_ASSERT_EQ((int)((uint32_t)k_test_ops_pclka_csi -
                       (uint32_t)k_ra8_mipi_phy_refcr_rfreq_bias),
                 (int)*ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_refcr));

  /* 100.9 MHz floors onto the same RFREQ word. */
  *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_refcr) = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mipi_phy_set_pclka_freq_hz((uint32_t)k_test_ops_hz_100p9mhz));
  TEST_ASSERT_EQ((int)((uint32_t)k_test_ops_pclka_csi -
                       (uint32_t)k_ra8_mipi_phy_refcr_rfreq_bias),
                 (int)*ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_refcr));

  /* Under 1 MHz floors to 0, which the 40..125 MHz range rejects. */
  *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_refcr) = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_mipi_phy_set_pclka_freq_hz((uint32_t)k_test_ops_hz_sub_mhz));
  TEST_ASSERT_EQ(0, *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_refcr));

  /* 300 MHz would narrow to 44 in a uint8_t; it is rejected before that. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_mipi_phy_set_pclka_freq_hz((uint32_t)k_test_ops_hz_300mhz));
  TEST_ASSERT_EQ(0, *ra8_mipi_phy_reg32(k_ra8_mipi_phy_off_refcr));
}

/**
 * @var s_test_roster
 * @brief Cases in run order; main() walks the table.
 */
static void (*const s_test_roster[])(void) = {
  test_get_state_walks_the_startup_flags,
  test_get_active_mode_reads_dphymdc,
  test_get_status_decoded,
  test_compute_pll_freq_and_lane_rate,
  test_lookup_timing_touches_no_register,
  test_select_timing_still_programs_the_row,
  test_dual_mode_policy,
  test_set_pclka_freq_hz,
};

int main(void)
{
  for (size_t i = 0U; i < (sizeof s_test_roster / sizeof s_test_roster[0]); ++i) {
    s_test_roster[i]();
  }
  return 0;
}
