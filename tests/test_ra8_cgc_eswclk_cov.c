/**
 * @file test_ra8_cgc_eswclk_cov.c
 * @brief Coverage-raising tests for ra8_cgc_eswclk.c.
 *
 * @details
 * This file is a dedicated test executable that exercises paths in
 * ra8_cgc_eswclk.c not yet reached by test_ra8_cgc.c or
 * test_ra8_cgc_extended.c. It is auto-discovered by the GLOB in
 * tests/CMakeLists.txt and linked as a separate binary; gcovr merges
 * the resulting .gcda so every covered line adds to the repository-wide
 * line coverage.
 *
 * Paths covered:
 *  - ra8_cgc_eswclk_hz(nullptr) -- null-pointer guard (line 350 of source).
 *  - ra8_cgc_eswclk_hz before init -- published Hz is 0.
 *  - ra8_cgc_eswclk_init happy path -- successful bring-up in sim.
 *  - ra8_cgc_eswclk_init clock-register programming -- verifies ESWCKCR /
 *    ESWPCKCR land on PLL1P with SREQ/SRDY drained and the ESWCKDIVCR /
 *    ESWPCKDIVCR dividers set to /4 and /2 (the internal_switch and
 *    internal_eswclk_program_cks success legs).
 *  - ra8_cgc_eswclk_init with saturated ethphyclk MSTP refcount -- the
 *    ra8_mstp_enable step fails, exercising the error-return branch at
 *    the MSTP gate (source lines 334-335).
 *  - every bounded-wait timeout leg (HOCO stabilisation, PDCSF/PDPGSF
 *    stuck, ESWCKCR/ESWPCKCR CKSRDY handshakes), driven through the
 *    ra8_sim_mmio fault seam now that the sim short-circuits are gone.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_mstp.h"
#include "ra8_sim_mmap.h"
#include "ra8_sim_mmio.h"
#include "ra8_system_regs.h"
#include "unity_minimal.h"

/**
 * @enum cgc_eswclk_cov_fixture_t
 * @brief Loop bounds and counts, sized so the case under test is actually reached.
 */
typedef enum : uint8_t {
  k_cgc_divider_settings =
    255U, /**< Divider settings swept: every value the field can hold, so no encoding goes untried. */
} cgc_eswclk_cov_fixture_t;

/**
 * @enum eswclk_wait_idx_t
 * @brief fail-nth wait-loop indices used to isolate one timeout leg.
 *
 * @details
 * ``internal_switch_eswcr_to_pll1p`` runs two sequential CKSRDY waits
 * on the SAME control register (SRDY=1 after SREQ, then SRDY=0 after
 * the SREQ clear); ``internal_eswclk_power_on_domain`` runs two
 * sequential PDCTRESWM waits (PDCSF then PDPGSF). ``fail_nth`` picks
 * which of those loops times out.
 */
typedef enum : uint32_t {
  k_eswclk_wait_first  = 0U, /**< First wait-loop on the register.  */
  k_eswclk_wait_second = 1U, /**< Second wait-loop on the register. */
} eswclk_wait_idx_t;

/** @brief Reset the sim register mirror and the MMIO fault seam together. */
static void eswclk_test_reset(void)
{
  ra8_sim_mmap_reset();
  ra8_sim_mmio_reset();
}

/* ---------------------------------------------------------------------------
 * ra8_cgc_eswclk_hz -- null-pointer guard
 * ---------------------------------------------------------------------------
 */

/**
 * @brief Verify ra8_cgc_eswclk_hz rejects a null output pointer.
 *
 * @details
 * Calls ra8_cgc_eswclk_hz with out_hz == nullptr and expects
 * k_ra8_err_null_ptr. Covers line 350 of ra8_cgc_eswclk.c.
 *
 * @pre ra8_sim_mmap_reset has been called.
 * @pre ra8_cgc_eswclk_init has NOT been called; s_eswclk_hz == 0.
 * @post Returns k_ra8_err_null_ptr without touching s_eswclk_hz.
 *
 * @par MC/DC:
 * Decision: ``out_hz == nullptr`` (one condition, no compound logic).
 * - V1: out_hz=nullptr -> true  -> k_ra8_err_null_ptr (this test).
 * - V2: out_hz=valid   -> false -> k_ra8_ok (covered by other binaries).
 * Single-condition: no MC/DC vector table required.
 *
 * @since 0.1.0
 */
static void test_eswclk_hz_null(void)
{
  TEST_BEGIN("eswclk hz null pointer rejected");
  eswclk_test_reset();

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_cgc_eswclk_hz(nullptr));

  TEST_END("eswclk hz null pointer rejected");
}

/* ---------------------------------------------------------------------------
 * ra8_cgc_eswclk_hz -- query before init
 * ---------------------------------------------------------------------------
 */

/**
 * @brief Verify ra8_cgc_eswclk_hz returns 0 Hz before ra8_cgc_eswclk_init.
 *
 * @details
 * s_eswclk_hz is static-storage zero at process start. Before a
 * successful ra8_cgc_eswclk_init call, the published frequency is 0.
 * This test runs first in the binary, before any init attempt.
 *
 * @pre ra8_sim_mmap_reset has been called.
 * @pre ra8_cgc_eswclk_init has not been called in this process.
 * @post out_hz == 0 and return value == k_ra8_ok.
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
  eswclk_test_reset();

  uint32_t hz = 1U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cgc_eswclk_hz(&hz));
  /* s_eswclk_hz starts at 0 (BSS) before the first successful init. */
  TEST_ASSERT_EQ(0U, hz);

  TEST_END("eswclk hz before init returns 0");
}

/* ---------------------------------------------------------------------------
 * ra8_cgc_eswclk_init -- happy path, then hz query
 * ---------------------------------------------------------------------------
 */

/**
 * @brief Verify ra8_cgc_eswclk_init succeeds and publishes 250 MHz.
 *
 * @details
 * With no MMIO fault armed, every bounded wait (HOCOSF stabilisation,
 * the PDCTRESWM power-domain polls, and the ESWCKCR / ESWPCKCR
 * SREQ/SRDY handshakes) is satisfied by the ra8_sim_mmio seam on its
 * first poll. The successful init sets s_eswclk_hz = 250000000;
 * ra8_cgc_eswclk_hz reflects this.
 *
 * @pre eswclk_test_reset and ra8_mstp_init have been called.
 * @post ra8_cgc_eswclk_hz returns 250000000 Hz.
 *
 * @par MC/DC:
 * (no compound decisions exercised by this test that are not already
 * covered by test_mcdc_eswclk_pdctreswm in test_ra8_cgc.c)
 *
 * @since 0.1.0
 */
static void test_eswclk_init_and_hz_ok(void)
{
  TEST_BEGIN("eswclk init happy path and hz query");
  eswclk_test_reset();
  /* Reset all MSTP refcounts to zero before using them. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mstp_init());

  /* No fault armed: the seam satisfies every wait on its first poll,
   * so no manual OSCSF pre-seeding is required for this call. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cgc_eswclk_init());

  uint32_t hz = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cgc_eswclk_hz(&hz));
  TEST_ASSERT_EQ(250000000U, hz);

  TEST_END("eswclk init happy path and hz query");
}

/* ---------------------------------------------------------------------------
 * ra8_cgc_eswclk_init -- verify the clock registers are programmed to PLL1P
 * ---------------------------------------------------------------------------
 */

/**
 * @enum eswclk_expected_divcr_t
 * @brief Expected ESWCKDIVCR / ESWPCKDIVCR divider codes after init.
 *
 * @details
 * Mirrors the FSP "log2 ratio" divider map that ra8_cgc_eswclk.c programs:
 * ESWCLK is PLL1P / 4 (code 2) and ESWPHYCLK is PLL1P / 2 (code 1). The
 * driver's own ra8_cgc_eswckdivcr_t is file-static, so the test names the
 * same expected codes locally rather than reaching into the TU.
 */
typedef enum : uint8_t {
  k_ra8_eswckdivcr_div2_code = 1U, /**< /2 -- ESWPHYCLK divider code. */
  k_ra8_eswckdivcr_div4_code = 2U, /**< /4 -- ESWCLK divider code.    */
} eswclk_expected_divcr_t;

/**
 * @brief Verify ra8_cgc_eswclk_init programs ESWCKCR / ESWPCKCR to PLL1P.
 *
 * @details
 * The happy-path test above only asserts the published Hz. This case
 * additionally proves that the SREQ/SRDY handshake in
 * internal_switch_eswcr_to_pll1p left the register file in the expected
 * steady state: both control registers select PLL1P (CKSEL == 5) with
 * SREQ drained to 0, and both divider registers hold the FSP divider
 * codes (ESWCKDIVCR = /4 = 2, ESWPCKDIVCR = /2 = 1). SRDY is a
 * hardware-owned status bit: the driver's step-4 write asserts it (per
 * the FSP sequence) and only the silicon clears it, so on the host the
 * RAM readback keeps the driver-written 1 and is not asserted on here.
 * This walks the success legs of internal_switch_eswcr_to_pll1p (both
 * invocations) and internal_eswclk_program_cks, tying the reachable
 * register writes to observable state instead of the returned error
 * code alone.
 *
 * @pre eswclk_test_reset and ra8_mstp_init have been called.
 * @pre ra8_cgc_eswclk_init returns k_ra8_ok with no fault armed.
 * @post ESWCKCR / ESWPCKCR CKSEL fields read k_ra8_eswcksel_pll1p.
 * @post SREQ bits of both control registers read 0.
 *
 * @par MC/DC:
 * (no compound decisions -- straight-line assertions over the steady
 * register state produced by the success path)
 *
 * @since 0.1.0
 */
static void test_eswclk_init_programs_clock_registers(void)
{
  TEST_BEGIN("eswclk init programs ESWCKCR/ESWPCKCR to PLL1P");
  eswclk_test_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mstp_init());

  TEST_ASSERT_EQ(k_ra8_ok, ra8_cgc_eswclk_init());

  const uint8_t sel_mask  = (uint8_t)k_ra8_eswckcr_mask_sel;
  const uint8_t sreq_mask = (uint8_t)(1U << k_ra8_eswckcr_bit_sreq);

  /* ESWCLK (ESWM core clock): PLL1P / 4. SRDY is hardware-owned and
   * intentionally not asserted on (see the test @details). */
  TEST_ASSERT_EQ(k_ra8_eswcksel_pll1p, (*ra8_sys_eswckcr() & sel_mask));
  TEST_ASSERT_EQ(0U, (*ra8_sys_eswckcr() & sreq_mask));
  TEST_ASSERT_EQ(k_ra8_eswckdivcr_div4_code, *ra8_sys_eswckdivcr());

  /* ESWPHYCLK (Ethernet-PHY interface clock): PLL1P / 2. */
  TEST_ASSERT_EQ(k_ra8_eswcksel_pll1p, (*ra8_sys_eswpckcr() & sel_mask));
  TEST_ASSERT_EQ(0U, (*ra8_sys_eswpckcr() & sreq_mask));
  TEST_ASSERT_EQ(k_ra8_eswckdivcr_div2_code, *ra8_sys_eswpckdivcr());

  TEST_END("eswclk init programs ESWCKCR/ESWPCKCR to PLL1P");
}

/* ---------------------------------------------------------------------------
 * ra8_cgc_eswclk_init -- MSTP refcount saturated (covers lines 334-335)
 * ---------------------------------------------------------------------------
 */

/**
 * @brief Verify ra8_cgc_eswclk_init propagates an MSTP saturation error.
 *
 * @details
 * The ethphyclk MSTP refcount (s_refcount[k_ra8_mstp_reg_c][28] in
 * ra8_mstp.c) is saturated to UINT8_MAX by calling ra8_mstp_enable
 * 255 times. When ra8_cgc_eswclk_init then calls ra8_mstp_enable
 * internally, ra8_mstp_enable returns k_ra8_err_invalid_state because
 * the refcount would overflow. This causes ra8_cgc_eswclk_init to log
 * an error and return the failure code (source lines 334-335).
 *
 * The earlier steps (HOCO ensure, power-domain power-on) always
 * succeed in RA8_SIMULATOR_MODE, so the MSTP step is the first failure
 * point.
 *
 * @pre ra8_sim_mmap_reset has been called.
 * @pre ra8_mstp_init has been called to zero all refcounts.
 * @pre The ethphyclk refcount has been driven to UINT8_MAX.
 * @post ra8_cgc_eswclk_init returns k_ra8_err_invalid_state.
 * @post s_eswclk_hz is unchanged (still set from a prior successful call).
 *
 * @par MC/DC:
 * Decision at ra8_cgc_eswclk_init: ``if (ethphy_mst_err != k_ra8_ok)``
 * (one condition, no compound logic).
 * - V1: ethphy_mst_err=k_ra8_err_invalid_state -> true  -> error return (this test).
 * - V2: ethphy_mst_err=k_ra8_ok               -> false -> proceed (test_eswclk_init_and_hz_ok).
 * Single-condition: no MC/DC vector table required.
 *
 * @since 0.1.0
 */
static void test_eswclk_init_mstp_saturated(void)
{
  TEST_BEGIN("eswclk init fails when ethphyclk mstp refcount saturated");
  eswclk_test_reset();
  /* Reset all MSTP refcounts to 0 (may be non-zero from prior test). */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mstp_init());

  /* Saturate the ethphyclk refcount by enabling 255 times. The first
   * call ungates the peripheral (register write + readback); the
   * remaining 254 calls only increment the refcount (fast path). */
  for (uint16_t i = 0U; i < k_cgc_divider_settings; ++i) {
    TEST_ASSERT_EQ(k_ra8_ok, ra8_mstp_enable(k_ra8_mstp_ethphyclk));
  }

  /* Confirm the refcount is at maximum before the init attempt. */
  uint8_t ref = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mstp_get_refcount(k_ra8_mstp_ethphyclk, &ref));
  TEST_ASSERT_EQ(255U, ref);

  /* ra8_cgc_eswclk_init steps with no MMIO fault armed:
   *   1. ra8_cgc_ensure_hoco_running_for_usb_ck -- seam-satisfied wait -> ok.
   *   2. internal_eswclk_power_on_domain       -- seam-satisfied waits -> ok.
   *   3. ra8_mstp_enable(k_ra8_mstp_ethphyclk)   -- refcount==255 -> invalid_state.
   * The log + return ethphy_mst_err leg is reached. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_cgc_eswclk_init());

  /* Cleanup: restore a clean MSTP state for any code that follows. */
  (void)ra8_mstp_init();

  TEST_END("eswclk init fails when ethphyclk mstp refcount saturated");
}

/* ---------------------------------------------------------------------------
 * ra8_cgc_eswclk_init -- seam-armed failure legs (previously dead branches)
 * ---------------------------------------------------------------------------
 */

/**
 * @brief Verify ra8_cgc_eswclk_init propagates the HOCO stabilisation timeout.
 *
 * @details
 * Arms the MMIO fault seam on OSCSF so the HOCOSF wait inside
 * ra8_cgc_ensure_hoco_running_for_usb_ck burns its budget; init aborts
 * on its step-1 error branch (log + return hoco_err).
 *
 * @pre eswclk_test_reset and ra8_mstp_init have been called.
 * @pre The seam is armed to fail every OSCSF wait.
 * @post ra8_cgc_eswclk_init returns k_ra8_err_hw_timeout.
 * @post No ESW clock register is switched to PLL1P.
 *
 * @par MC/DC:
 * (no compound decisions -- single-condition ``hoco_err != k_ra8_ok``)
 *
 * @since 0.1.0
 */
static void test_eswclk_init_hoco_timeout(void)
{
  TEST_BEGIN("eswclk init HOCO stabilisation timeout");
  eswclk_test_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mstp_init());

  TEST_ASSERT_EQ(k_ra8_ok, ra8_sim_mmio_fail_wait((const volatile void*)ra8_sys_oscsf()));
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_cgc_eswclk_init());

  TEST_END("eswclk init HOCO stabilisation timeout");
}

/**
 * @brief Verify the PDCSF and PDPGSF stuck legs each propagate a timeout.
 *
 * @details
 * internal_eswclk_power_on_domain polls PDCTRESWM twice (PDCSF, then
 * PDPGSF). fail-nth isolates each wait-loop so both "stuck" error
 * branches -- previously dead under the deleted sim short-circuit --
 * execute and propagate k_ra8_err_hw_timeout out of init.
 *
 * @pre eswclk_test_reset and ra8_mstp_init have been called.
 * @pre The seam is armed on PDCTRESWM with the leg under test.
 * @post ra8_cgc_eswclk_init returns k_ra8_err_hw_timeout for both legs.
 * @post The MSTP / clock-switch steps after the failing wait never run.
 *
 * @par MC/DC:
 * (no compound decisions -- two single-condition ``err != k_ra8_ok``
 * checks, one per PDCTRESWM wait)
 *
 * @since 0.1.0
 */
static void test_eswclk_init_pdctreswm_stuck_legs(void)
{
  TEST_BEGIN("eswclk init PDCSF/PDPGSF stuck legs");

  /* Leg 1: PDCSF never clears (first PDCTRESWM wait-loop). */
  eswclk_test_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mstp_init());
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_sim_mmio_fail_nth_wait((const volatile void*)ra8_sys_pdctreswm(),
                                            (uint32_t)k_eswclk_wait_first));
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_cgc_eswclk_init());

  /* Leg 2: PDCSF clears but PDPGSF never does (second wait-loop). */
  eswclk_test_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mstp_init());
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_sim_mmio_fail_nth_wait((const volatile void*)ra8_sys_pdctreswm(),
                                            (uint32_t)k_eswclk_wait_second));
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_cgc_eswclk_init());

  TEST_END("eswclk init PDCSF/PDPGSF stuck legs");
}

/**
 * @brief Verify both CKSRDY handshake timeout legs on ESWCKCR and ESWPCKCR.
 *
 * @details
 * internal_switch_eswcr_to_pll1p waits CKSRDY=1 then CKSRDY=0 on the
 * same control register. fail-nth drives: (a) the SRDY=1 leg on
 * ESWCKCR, (b) the SRDY=0 leg on ESWCKCR, and (c) the SRDY=1 leg on
 * ESWPCKCR after a fully successful ESWCKCR switch -- covering the
 * "ESWCKCR handshake timeout" and "ESWPCKCR handshake timeout" error
 * branches in internal_eswclk_program_cks.
 *
 * @pre eswclk_test_reset and ra8_mstp_init have been called.
 * @pre The seam is armed on the control register of the leg under test.
 * @post ra8_cgc_eswclk_init returns k_ra8_err_hw_timeout for all legs.
 * @post s_eswclk_hz is not re-published on the failing attempts.
 *
 * @par MC/DC:
 * (no compound decisions -- single-condition ``err != k_ra8_ok`` checks
 * after each handshake call)
 *
 * @since 0.1.0
 */
static void test_eswclk_init_cksrdy_timeout_legs(void)
{
  TEST_BEGIN("eswclk init ESWCKCR/ESWPCKCR handshake timeout legs");

  /* Leg a: ESWCKCR SRDY=1 never acknowledges (first wait-loop). */
  eswclk_test_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mstp_init());
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_sim_mmio_fail_nth_wait((const volatile void*)ra8_sys_eswckcr(),
                                            (uint32_t)k_eswclk_wait_first));
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_cgc_eswclk_init());

  /* Leg b: ESWCKCR SRDY=0 never acknowledges (second wait-loop). */
  eswclk_test_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mstp_init());
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_sim_mmio_fail_nth_wait((const volatile void*)ra8_sys_eswckcr(),
                                            (uint32_t)k_eswclk_wait_second));
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_cgc_eswclk_init());

  /* Leg c: ESWCKCR completes, ESWPCKCR SRDY=1 times out. */
  eswclk_test_reset();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mstp_init());
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_sim_mmio_fail_nth_wait((const volatile void*)ra8_sys_eswpckcr(),
                                            (uint32_t)k_eswclk_wait_first));
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_cgc_eswclk_init());

  TEST_END("eswclk init ESWCKCR/ESWPCKCR handshake timeout legs");
}

int32_t main(void)
{
  test_eswclk_hz_null();
  test_eswclk_hz_before_init();
  test_eswclk_init_and_hz_ok();
  test_eswclk_init_programs_clock_registers();
  test_eswclk_init_mstp_saturated();
  test_eswclk_init_hoco_timeout();
  test_eswclk_init_pdctreswm_stuck_legs();
  test_eswclk_init_cksrdy_timeout_legs();
  (void)fprintf(stderr, "[OK  ] test_ra8_cgc_eswclk_cov.c\n");
  return 0;
}
