/**
 * @file test_ra_cgc_eswclk_cov.c
 * @brief Coverage-raising tests for ra_cgc_eswclk.c.
 *
 * @details
 * This file is a dedicated test executable that exercises paths in
 * ra_cgc_eswclk.c not yet reached by test_ra_cgc.c or
 * test_ra_cgc_extended.c. It is auto-discovered by the GLOB in
 * tests/CMakeLists.txt and linked as a separate binary; gcovr merges
 * the resulting .gcda so every covered line adds to the repository-wide
 * line coverage.
 *
 * Paths covered:
 *  - ra_cgc_eswclk_hz(nullptr) -- null-pointer guard (line 350 of source).
 *  - ra_cgc_eswclk_hz before init -- published Hz is 0.
 *  - ra_cgc_eswclk_init happy path -- successful bring-up in sim.
 *  - ra_cgc_eswclk_init with saturated ethphyclk MSTP refcount -- the
 *    ra_mstp_enable step fails, exercising the error-return branch at
 *    the MSTP gate (source lines 334-335).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8d2_system_regs.h"
#include "ra_cgc.h"
#include "ra_err.h"
#include "ra_mstp.h"
#include "ra_sim_mmap.h"
#include "unity_minimal.h"

/* ---------------------------------------------------------------------------
 * ra_cgc_eswclk_hz -- null-pointer guard
 * ---------------------------------------------------------------------------
 */

/**
 * @brief Verify ra_cgc_eswclk_hz rejects a null output pointer.
 *
 * @details
 * Calls ra_cgc_eswclk_hz with out_hz == nullptr and expects
 * k_ra_err_null_ptr. Covers line 350 of ra_cgc_eswclk.c.
 *
 * @pre ra_sim_mmap_reset has been called.
 * @pre ra_cgc_eswclk_init has NOT been called; s_eswclk_hz == 0.
 * @post Returns k_ra_err_null_ptr without touching s_eswclk_hz.
 *
 * @par MC/DC:
 * Decision: ``out_hz == nullptr`` (one condition, no compound logic).
 * - V1: out_hz=nullptr -> true  -> k_ra_err_null_ptr (this test).
 * - V2: out_hz=valid   -> false -> k_ra_ok (covered by other binaries).
 * Single-condition: no MC/DC vector table required.
 *
 * @since 0.1.0
 */
static void test_eswclk_hz_null(void)
{
  TEST_BEGIN("eswclk hz null pointer rejected");
  ra_sim_mmap_reset();

  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_cgc_eswclk_hz(nullptr));

  TEST_END("eswclk hz null pointer rejected");
}

/* ---------------------------------------------------------------------------
 * ra_cgc_eswclk_hz -- query before init
 * ---------------------------------------------------------------------------
 */

/**
 * @brief Verify ra_cgc_eswclk_hz returns 0 Hz before ra_cgc_eswclk_init.
 *
 * @details
 * s_eswclk_hz is static-storage zero at process start. Before a
 * successful ra_cgc_eswclk_init call, the published frequency is 0.
 * This test runs first in the binary, before any init attempt.
 *
 * @pre ra_sim_mmap_reset has been called.
 * @pre ra_cgc_eswclk_init has not been called in this process.
 * @post out_hz == 0 and return value == k_ra_ok.
 *
 * @par MC/DC:
 * (no compound decisions -- single-condition null guard already proven
 * false by this test; the body path is the only executable statement)
 *
 * @since 0.1.0
 */
static void test_eswclk_hz_before_init(void)
{
  TEST_BEGIN("eswclk hz before init returns 0");
  ra_sim_mmap_reset();

  uint32_t hz = 1U;
  TEST_ASSERT_EQ(k_ra_ok, ra_cgc_eswclk_hz(&hz));
  /* s_eswclk_hz starts at 0 (BSS) before the first successful init. */
  TEST_ASSERT_EQ(0U, hz);

  TEST_END("eswclk hz before init returns 0");
}

/* ---------------------------------------------------------------------------
 * ra_cgc_eswclk_init -- happy path, then hz query
 * ---------------------------------------------------------------------------
 */

/**
 * @brief Verify ra_cgc_eswclk_init succeeds and publishes 250 MHz.
 *
 * @details
 * In RA_SIMULATOR_MODE, ra_cgc_ensure_hoco_running_for_usb_ck seeds
 * HOCOSF automatically, the PDCTRESWM power-domain wait clears
 * immediately, and the ESWCKCR / ESWPCKCR SREQ/SRDY handshakes are
 * satisfied by forcing the CKSRDY bits. The successful init sets
 * s_eswclk_hz = 250000000; ra_cgc_eswclk_hz reflects this.
 *
 * @pre ra_sim_mmap_reset and ra_mstp_init have been called.
 * @post ra_cgc_eswclk_hz returns 250000000 Hz.
 *
 * @par MC/DC:
 * (no compound decisions exercised by this test that are not already
 * covered by test_mcdc_eswclk_pdctreswm in test_ra_cgc.c)
 *
 * @since 0.1.0
 */
static void test_eswclk_init_and_hz_ok(void)
{
  TEST_BEGIN("eswclk init happy path and hz query");
  ra_sim_mmap_reset();
  /* Reset all MSTP refcounts to zero before using them. */
  TEST_ASSERT_EQ(k_ra_ok, ra_mstp_init());

  /* HOCO stabilisation is auto-seeded in RA_SIMULATOR_MODE; no manual
   * OSCSF pre-seeding is required for this call. */
  TEST_ASSERT_EQ(k_ra_ok, ra_cgc_eswclk_init());

  uint32_t hz = 0U;
  TEST_ASSERT_EQ(k_ra_ok, ra_cgc_eswclk_hz(&hz));
  TEST_ASSERT_EQ(250000000U, hz);

  TEST_END("eswclk init happy path and hz query");
}

/* ---------------------------------------------------------------------------
 * ra_cgc_eswclk_init -- MSTP refcount saturated (covers lines 334-335)
 * ---------------------------------------------------------------------------
 */

/**
 * @brief Verify ra_cgc_eswclk_init propagates an MSTP saturation error.
 *
 * @details
 * The ethphyclk MSTP refcount (s_refcount[k_ra_mstp_reg_c][28] in
 * ra_mstp.c) is saturated to UINT8_MAX by calling ra_mstp_enable
 * 255 times. When ra_cgc_eswclk_init then calls ra_mstp_enable
 * internally, ra_mstp_enable returns k_ra_err_invalid_state because
 * the refcount would overflow. This causes ra_cgc_eswclk_init to log
 * an error and return the failure code (source lines 334-335).
 *
 * The earlier steps (HOCO ensure, power-domain power-on) always
 * succeed in RA_SIMULATOR_MODE, so the MSTP step is the first failure
 * point.
 *
 * @pre ra_sim_mmap_reset has been called.
 * @pre ra_mstp_init has been called to zero all refcounts.
 * @pre The ethphyclk refcount has been driven to UINT8_MAX.
 * @post ra_cgc_eswclk_init returns k_ra_err_invalid_state.
 * @post s_eswclk_hz is unchanged (still set from a prior successful call).
 *
 * @par MC/DC:
 * Decision at ra_cgc_eswclk_init: ``if (ethphy_mst_err != k_ra_ok)``
 * (one condition, no compound logic).
 * - V1: ethphy_mst_err=k_ra_err_invalid_state -> true  -> error return (this test).
 * - V2: ethphy_mst_err=k_ra_ok               -> false -> proceed (test_eswclk_init_and_hz_ok).
 * Single-condition: no MC/DC vector table required.
 *
 * @since 0.1.0
 */
static void test_eswclk_init_mstp_saturated(void)
{
  TEST_BEGIN("eswclk init fails when ethphyclk mstp refcount saturated");
  ra_sim_mmap_reset();
  /* Reset all MSTP refcounts to 0 (may be non-zero from prior test). */
  TEST_ASSERT_EQ(k_ra_ok, ra_mstp_init());

  /* Saturate the ethphyclk refcount by enabling 255 times. The first
   * call ungates the peripheral (register write + readback); the
   * remaining 254 calls only increment the refcount (fast path). */
  for (uint16_t i = 0U; i < 255U; ++i) {
    TEST_ASSERT_EQ(k_ra_ok, ra_mstp_enable(k_ra_mstp_ethphyclk));
  }

  /* Confirm the refcount is at maximum before the init attempt. */
  uint8_t ref = 0U;
  TEST_ASSERT_EQ(k_ra_ok, ra_mstp_get_refcount(k_ra_mstp_ethphyclk, &ref));
  TEST_ASSERT_EQ(255U, ref);

  /* ra_cgc_eswclk_init steps in RA_SIMULATOR_MODE:
   *   1. ra_cgc_ensure_hoco_running_for_usb_ck -- auto-seeds HOCOSF -> ok.
   *   2. internal_eswclk_power_on_domain       -- sim clears PDCTRESWM -> ok.
   *   3. ra_mstp_enable(k_ra_mstp_ethphyclk)   -- refcount==255 -> invalid_state.
   * Source lines 334-335 (log + return ethphy_mst_err) are reached. */
  TEST_ASSERT_EQ(k_ra_err_invalid_state, ra_cgc_eswclk_init());

  /* Cleanup: restore a clean MSTP state for any code that follows. */
  (void)ra_mstp_init();

  TEST_END("eswclk init fails when ethphyclk mstp refcount saturated");
}

int32_t main(void)
{
  test_eswclk_hz_null();
  test_eswclk_hz_before_init();
  test_eswclk_init_and_hz_ok();
  test_eswclk_init_mstp_saturated();
  (void)fprintf(stderr, "[OK  ] test_ra_cgc_eswclk_cov.c\n");
  return 0;
}
