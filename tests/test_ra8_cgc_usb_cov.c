/**
 * @file test_ra8_cgc_usb_cov.c
 * @brief White-box line-coverage tests for the PLL2 + USB clock driver
 *        (`ra8_cgc_usb.c`).
 *
 * @par Tag
 * [Ring 1 / HAL] {World: S}
 *
 * @details
 * The black-box tests in `test_ra8_cgc_extended.c` link against the production
 * `ra8_cgc_usb.c` and drive it through the sim register mirror. Two families of
 * lines stay unreachable that way:
 *
 *  1. The PLL2 / HOCO / main-XTAL "stabilization poll failed" error legs.
 *     Those legs fire only when ::ra8_cgc_wait_oscsf_set / ::ra8_cgc_wait_oscsf_clear
 *     time out, but in the sim mirror the OSCSF flags are seeded so the polls
 *     always succeed on the happy path -- the failure return is never taken.
 *  2. The USBCKCR / USB60CKCR SREQ->SRDY handshake timeout legs. In
 *     `RA8_SIMULATOR_MODE` the static `internal_wait_usbck*srdy` helpers fake the
 *     SRDY toggle and return success on the first loop iteration, so their
 *     bounded-poll timeout return and the handshake error legs are dead in sim.
 *
 * This TU compiles a second, instrumented copy of `ra8_cgc_usb.c` to reach both
 * families the same way `test_ra8_usb_hmsc_enum_cov.c` reaches its ladder:
 *
 *  - The four exported symbols are renamed with a `_cov` suffix so they do not
 *    collide with the production copy linked from `ra8_core_hal`.
 *  - The two external OSCSF poll helpers (`ra8_cgc_wait_oscsf_set` /
 *    `ra8_cgc_wait_oscsf_clear`) are redirected to deterministic, per-bit
 *    scriptable mocks. Returning failure from a mock hits the corresponding
 *    error return; returning success lets the happy path proceed.
 *  - `RA8_SIMULATOR_MODE` is undefined ONLY for the included `.c` body so the
 *    SRDY poll helpers read the (mmap-backed) register mirror directly. With
 *    the SRDY bit never asserted the bounded poll runs to its cap and returns
 *    the timeout, exercising the handshake error legs. The headers the module
 *    pulls in do not reference `RA8_SIMULATOR_MODE`, so only the driver body is
 *    affected; the `ra8_sim_mmap` backing (a separate TU) is unchanged.
 *
 * No hardware line is bypassed by an exclusion marker. The two USB60CKCR /
 * USBCKCR "SRDY=0" (second-wait) legs remain uncovered because reaching them
 * requires the first SRDY wait to succeed first, which needs a stateful
 * register model that toggles SRDY -- out of scope for this poll-helper mock.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_cgc_regs.h"
#include "ra8_err.h"
#include "ra8_sim_mmap.h"
#include "ra8_system_regs.h"
#include "unity_minimal.h"

/* =============================================================================
 * Scriptable mocks for the two external OSCSF poll helpers.
 * =============================================================================
 */

/** @brief Result ::mock_wait_oscsf_clear returns (PLL2SF stop wait). */
static ra8_err_t s_clear_result = k_ra8_ok;
/**
 * @brief OSCSF bit for which ::mock_wait_oscsf_set reports a timeout.
 * @details -1 means "no bit fails" (every set-poll succeeds).
 */
static int s_set_fail_bit = -1;

/** @brief HOCOCR value with HCSTP set, used to drive the "restart HOCO" branch. */
static const uint8_t k_cov_hococr_hcstp = (uint8_t)(1U << k_ra8_hococr_hcstp);

/** @brief PLL2 USB-path multiplier (matches the driver's k_ra8_pll2_usbfs_mul). */
static const uint8_t k_cov_pll2_mul = 80U;

/** @brief Reset the sim register mirror and the mock scripts to a clean baseline. */
static void cov_reset(void)
{
  ra8_sim_mmap_reset();
  s_clear_result = k_ra8_ok;
  s_set_fail_bit = -1;
}

/* Rename the exported symbols so the instrumented copy does not clash with the
 * production driver in ra8_core_hal, and redirect the OSCSF poll helpers to the
 * scriptable mocks above. */
/* The two OSCSF poll mocks are only DECLARED here and defined below the
 * driver include. `ra8_cgc_internal.h` carries RA8_PRIV on the real
 * declarations, and the #define maps that annotated declaration onto the
 * mock -- clang rejects an attribute on a declaration that follows the
 * definition, so the definition has to come last. The declaration must also
 * be `static` and come first, or the header's expansion introduces the
 * symbol with external linkage and the definition then conflicts with it. */
static ra8_err_t mock_wait_oscsf_clear(uint8_t bit);
static ra8_err_t mock_wait_oscsf_set(uint8_t bit);

ra8_err_t ra8_cgc_pll2_enable_cov(uint8_t mul_int, uint8_t mul_quarters, ra8_plodiv_t p_div_code);
ra8_err_t ra8_cgc_usbfs_clock_enable_cov(void);
ra8_err_t ra8_cgc_usbhs_pll_enable_cov(void);
ra8_err_t ra8_cgc_ensure_hoco_running_for_usb_ck_cov(void);

/** @brief RA8 cgc pll2 enable. */
/* Include-time interposition seam: each macro below must be spelled
 * EXACTLY like the production symbol it replaces, so the project rule
 * that macro names are UPPER_CASE cannot apply here -- renaming one
 * silently un-hooks the mock and the test would exercise the real
 * driver while still passing. */
// NOLINTBEGIN(readability-identifier-naming)
#define ra8_cgc_pll2_enable ra8_cgc_pll2_enable_cov
/** @brief RA8 cgc usbfs clock enable. */
#define ra8_cgc_usbfs_clock_enable ra8_cgc_usbfs_clock_enable_cov
/** @brief RA8 cgc usbhs pll enable. */
#define ra8_cgc_usbhs_pll_enable ra8_cgc_usbhs_pll_enable_cov
/** @brief RA8 cgc ensure hoco running for USB ck. */
#define ra8_cgc_ensure_hoco_running_for_usb_ck ra8_cgc_ensure_hoco_running_for_usb_ck_cov
/** @brief RA8 cgc wait oscsf set. */
#define ra8_cgc_wait_oscsf_set mock_wait_oscsf_set
/** @brief RA8 cgc wait oscsf clear. */
#define ra8_cgc_wait_oscsf_clear mock_wait_oscsf_clear
// NOLINTEND(readability-identifier-naming)

/* Undefine RA8_SIMULATOR_MODE for the driver body ONLY (all of its headers are
 * already free of the macro), so the static SRDY poll helpers read the register
 * mirror directly instead of faking the handshake ack. */
#undef RA8_SIMULATOR_MODE
#include "ra8_cgc_usb.c" // NOLINT(bugprone-suspicious-include) -- white-box copy

/** @brief Mock for `ra8_cgc_wait_oscsf_clear()` -- returns the scripted result. */
static ra8_err_t mock_wait_oscsf_clear(uint8_t bit)
{
  (void)bit;
  return s_clear_result;
}

/** @brief Mock for `ra8_cgc_wait_oscsf_set()` -- fails only for ::s_set_fail_bit. */
static ra8_err_t mock_wait_oscsf_set(uint8_t bit)
{
  if ((int)bit == s_set_fail_bit) {
    return k_ra8_err_hw_timeout;
  }
  return k_ra8_ok;
}

/* =============================================================================
 * White-box tests.
 * =============================================================================
 */

/**
 * @test test_pll2_program_success
 *
 * @brief PLL2 stop/program/lock completes when both OSCSF polls succeed.
 *
 * @details With OSCSF.PLL2SF clear at entry (fresh sim mirror) the idempotency
 * short-circuit is skipped, so the driver programs PLL2CCR/PLL2CCR2, restarts
 * PLL2, and both the stop poll (clear) and the lock poll (set) return ok from
 * the mocks -- covering the "pll2 locked" success return that the black-box
 * timeout test never reaches (it always fails the lock poll).
 *
 * @par MC/DC:
 * (no compound decisions in the code under test -- the idempotency guard and
 * each poll-result check are single-condition `if` statements.)
 */
static void test_pll2_program_success(void)
{
  TEST_BEGIN("pll2: full program + lock succeeds when polls pass");
  cov_reset();

  /* OSCSF.PLL2SF stays clear -> program path; both mocked polls succeed. */
  s_clear_result = k_ra8_ok;
  s_set_fail_bit = -1;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cgc_pll2_enable_cov(k_cov_pll2_mul, 0U, k_ra8_plodiv_div4));

  TEST_END("pll2: full program + lock succeeds when polls pass");
}

/**
 * @test test_pll2_stop_wait_timeout
 *
 * @brief PLL2 stop poll timeout returns the failure code.
 *
 * @details The stop-wait leg (`ra8_cgc_wait_oscsf_clear(PLL2SF)` fails) is the
 * one PLL2 error return the black-box suite cannot reach: its OSCSF=0 timeout
 * test leaves PLL2SF already clear, so the stop poll succeeds and only the lock
 * poll times out. Here the clear mock returns a timeout, hitting the stop-wait
 * log + break and propagating the error out of the enable entry point.
 *
 * @par MC/DC:
 * (no compound decisions -- single-condition `if (err != k_ra8_ok)` after the
 * stop poll.)
 */
static void test_pll2_stop_wait_timeout(void)
{
  TEST_BEGIN("pll2: stop-wait poll timeout propagates");
  cov_reset();

  s_clear_result = k_ra8_err_hw_timeout; /* PLL2SF stop poll never clears. */
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout,
                 ra8_cgc_pll2_enable_cov(k_cov_pll2_mul, 0U, k_ra8_plodiv_div4));

  TEST_END("pll2: stop-wait poll timeout propagates");
}

/**
 * @test test_ensure_hoco_hcstp_and_wait
 *
 * @brief HOCO-restart helper clears HCSTP and honours the stabilization poll.
 *
 * @details Two legs: (1) HOCOCR.HCSTP preseeded set so the "restart HOCO"
 * branch fires and clears the bit, then the (mocked) HOCOSF poll succeeds;
 * (2) the same entry with the HOCOSF set-poll scripted to time out, so the
 * helper returns the failure code.
 *
 * @par MC/DC:
 * (no compound decisions -- the HCSTP guard is a single-condition `if` and the
 * return is the poll result directly.)
 */
static void test_ensure_hoco_hcstp_and_wait(void)
{
  TEST_BEGIN("ensure_hoco: clears HCSTP, returns the stabilization poll result");

  /* HCSTP set + poll ok -> restart branch taken, success returned. */
  cov_reset();
  *ra8_sys_hococr() = k_cov_hococr_hcstp;
  s_set_fail_bit    = -1;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cgc_ensure_hoco_running_for_usb_ck_cov());

  /* Poll times out -> failure returned. */
  cov_reset();
  *ra8_sys_hococr() = k_cov_hococr_hcstp;
  s_set_fail_bit    = (int)k_ra8_oscsf_bit_hocosf;
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_cgc_ensure_hoco_running_for_usb_ck_cov());

  TEST_END("ensure_hoco: clears HCSTP, returns the stabilization poll result");
}

/**
 * @test test_usbfs_hoco_stabilize_timeout
 *
 * @brief USB-FS bring-up aborts when the HOCO stabilization poll times out.
 *
 * @details PLL2 is allowed to lock (clear + PLL2SF set polls pass) but the
 * HOCOSF set poll is scripted to fail, so `ra8_cgc_usbfs_clock_enable` takes the
 * "HOCO stabilization timeout" return before the USBCKCR handshake.
 *
 * @par MC/DC:
 * (no compound decisions -- single-condition `if (hoco_err != k_ra8_ok)`.)
 */
static void test_usbfs_hoco_stabilize_timeout(void)
{
  TEST_BEGIN("usbfs: HOCO stabilization poll timeout aborts before handshake");
  cov_reset();

  /* PLL2SF (bit 6) succeeds; HOCOSF (bit 0) fails. */
  s_set_fail_bit = (int)k_ra8_oscsf_bit_hocosf;
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_cgc_usbfs_clock_enable_cov());

  TEST_END("usbfs: HOCO stabilization poll timeout aborts before handshake");
}

/**
 * @test test_usbfs_handshake_srdy_timeout
 *
 * @brief USB-FS USBCKCR SREQ->SRDY handshake times out on the first poll.
 *
 * @details PLL2 locks and HOCO stabilizes (both mocked ok). HOCOCR.HCSTP is
 * preseeded so the in-line HOCO-restart branch also runs. With the driver body
 * built without `RA8_SIMULATOR_MODE`, the static USBCKSRDY poll reads the plain
 * register mirror (SRDY never asserts), so the bounded poll runs to its cap and
 * returns the timeout, driving the "SRDY=1 timeout" handshake leg and the
 * handshake error return in the enable entry point.
 *
 * @par MC/DC:
 * (no compound decisions -- the poll `if (got == expected)` and the
 * `if (err != k_ra8_ok)` error checks are single-condition.)
 */
static void test_usbfs_handshake_srdy_timeout(void)
{
  TEST_BEGIN("usbfs: USBCKCR SREQ/SRDY handshake times out");
  cov_reset();

  *ra8_sys_hococr() = k_cov_hococr_hcstp; /* Exercise the HOCO-restart branch. */
  s_set_fail_bit    = -1;                 /* PLL2 + HOCO polls all succeed.    */
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_cgc_usbfs_clock_enable_cov());

  TEST_END("usbfs: USBCKCR SREQ/SRDY handshake times out");
}

/**
 * @test test_usbhs_moscsf_timeout
 *
 * @brief USB-HS bring-up aborts when the main-XTAL stabilization poll fails.
 *
 * @details The first step of `ra8_cgc_usbhs_pll_enable` waits OSCSF.MOSCSF; when
 * that set poll is scripted to fail the routine returns immediately with the
 * propagated timeout, before PLL2 is touched.
 *
 * @par MC/DC:
 * (no compound decisions -- single-condition `if (osc_err != k_ra8_ok)`.)
 */
static void test_usbhs_moscsf_timeout(void)
{
  TEST_BEGIN("usbhs: main-XTAL stabilization poll timeout aborts");
  cov_reset();

  s_set_fail_bit = (int)k_ra8_oscsf_bit_moscsf;
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_cgc_usbhs_pll_enable_cov());

  TEST_END("usbhs: main-XTAL stabilization poll timeout aborts");
}

/**
 * @test test_usbhs_pll2_enable_failed
 *
 * @brief USB-HS bring-up aborts when PLL2 enable fails after MOSCSF is stable.
 *
 * @details MOSCSF succeeds, but the PLL2 lock poll (PLL2SF set) is scripted to
 * fail, so `ra8_cgc_pll2_enable` returns a timeout and the USB-HS routine takes
 * its "pll2 enable failed" return -- a leg the black-box USB-HS timeout test
 * never reaches (it fails MOSCSF first).
 *
 * @par MC/DC:
 * (no compound decisions -- single-condition `if (pll2_err != k_ra8_ok)`.)
 */
static void test_usbhs_pll2_enable_failed(void)
{
  TEST_BEGIN("usbhs: PLL2 enable failure propagates");
  cov_reset();

  /* MOSCSF (bit 3) succeeds; PLL2SF (bit 6) lock poll fails. */
  s_set_fail_bit = (int)k_ra8_oscsf_bit_pll2sf;
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_cgc_usbhs_pll_enable_cov());

  TEST_END("usbhs: PLL2 enable failure propagates");
}

/**
 * @test test_usbhs_hoco_stabilize_timeout
 *
 * @brief USB-HS bring-up aborts when the HOCO stabilization poll times out.
 *
 * @details MOSCSF and PLL2 succeed, but the HOCOSF set poll inside the
 * HOCO-restart helper is scripted to fail, so the USB-HS routine takes its
 * "HOCO stabilization timeout" return before the USB60CKCR handshake.
 *
 * @par MC/DC:
 * (no compound decisions -- single-condition `if (hoco_err != k_ra8_ok)`.)
 */
static void test_usbhs_hoco_stabilize_timeout(void)
{
  TEST_BEGIN("usbhs: HOCO stabilization poll timeout aborts before handshake");
  cov_reset();

  /* MOSCSF (3) + PLL2SF (6) succeed; HOCOSF (0) fails. */
  s_set_fail_bit = (int)k_ra8_oscsf_bit_hocosf;
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_cgc_usbhs_pll_enable_cov());

  TEST_END("usbhs: HOCO stabilization poll timeout aborts before handshake");
}

/**
 * @test test_usbhs_handshake_srdy_timeout
 *
 * @brief USB-HS USB60CKCR SREQ->SRDY handshake times out on the first poll.
 *
 * @details Every OSCSF poll succeeds (MOSCSF, PLL2SF, HOCOSF), so the routine
 * reaches the USB60CKCR handshake. With the body built without
 * `RA8_SIMULATOR_MODE`, the static USB60CKSRDY poll reads the plain register
 * mirror (SRDY never asserts) and runs to its cap, driving the "SRDY=1
 * timeout" handshake leg, the diagnostic probe read-back, and the handshake
 * error return.
 *
 * @par MC/DC:
 * (no compound decisions -- the poll `if (got == expected)` and the
 * `if (err != k_ra8_ok)` error checks are single-condition.)
 */
static void test_usbhs_handshake_srdy_timeout(void)
{
  TEST_BEGIN("usbhs: USB60CKCR SREQ/SRDY handshake times out");
  cov_reset();

  s_set_fail_bit = -1; /* All OSCSF polls pass; only the SRDY handshake fails. */
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_cgc_usbhs_pll_enable_cov());

  TEST_END("usbhs: USB60CKCR SREQ/SRDY handshake times out");
}

int32_t main(void)
{
  test_pll2_program_success();
  test_pll2_stop_wait_timeout();
  test_ensure_hoco_hcstp_and_wait();
  test_usbfs_hoco_stabilize_timeout();
  test_usbfs_handshake_srdy_timeout();
  test_usbhs_moscsf_timeout();
  test_usbhs_pll2_enable_failed();
  test_usbhs_hoco_stabilize_timeout();
  test_usbhs_handshake_srdy_timeout();
  (void)fprintf(stderr, "[OK  ] test_ra8_cgc_usb_cov.c\n");
  return 0;
}
