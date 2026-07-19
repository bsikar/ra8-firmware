/**
 * @file test_ra8_cgc_usb_cov2.c
 * @brief Complementary white-box line-coverage tests for the PLL2 + USB clock
 *        driver (`ra8_cgc_usb.c`) -- the legs `test_ra8_cgc_usb_cov.c` cannot reach.
 *
 * @par Tag
 * [Ring 1 / HAL] {World: S}
 *
 * @details
 * This TU compiles a second instrumented copy of `ra8_cgc_usb.c` (exported
 * symbols renamed `*_cov2`) so gcovr unions its `.gcda` with the sibling
 * `test_ra8_cgc_usb_cov.c` copy. It exists to close the residual gaps that the
 * sibling's poll-helper mock leaves open:
 *
 *  1. The PLL2 argument-validation legs (`mul_int == 0`, `mul_quarters > 3`)
 *     and the "PLL2 already locked -- skip re-program" idempotency short-circuit.
 *     The sibling never drives an invalid argument nor pre-asserts OSCSF.PLL2SF.
 *  2. The USB-FS "PLL2 enable failed" abort inside `ra8_cgc_usbfs_clock_enable`
 *     (the sibling only fails PLL2 for the USB-HS path).
 *  3. The two `USBCKCR` / `USB60CKCR` **second** SRDY waits ("SRDY=0 timeout").
 *     Reaching those requires the FIRST SRDY wait to succeed, which the
 *     sibling's plain register mirror cannot do (SRDY never asserts). Here the
 *     `ra8_sys_usbckcr` / `ra8_sys_usb60ckcr` accessors are redirected to a
 *     stateful, scriptable register model:
 *       - ::k_cov2_srdy_follow  -- SRDY tracks SREQ, so the full handshake
 *         completes (drives the USB-FS success return).
 *       - ::k_cov2_srdy_stuck_high -- SRDY latches high and never clears, so
 *         the first wait (SRDY=1) succeeds and the second wait (SRDY=0) runs to
 *         its bounded cap and times out, driving the "SRDY=0 timeout" legs.
 *  4. The USB-FS success return and the in-line HOCO-restart branch.
 *
 * As in the sibling, the two external OSCSF poll helpers are redirected to
 * deterministic per-bit mocks, and `RA8_SIMULATOR_MODE` is undefined for the
 * driver body only so the static SRDY poll helpers read the (mocked) register
 * model directly. No hardware line is bypassed by an exclusion marker.
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
 * Scriptable mocks for the two external OSCSF poll helpers (mirrors the sibling).
 * =============================================================================
 */

/** @brief Result ::mock_wait_oscsf_clear returns (PLL2SF stop wait). */
static ra8_err_t s_clear_result = k_ra8_ok;
/**
 * @brief OSCSF bit for which ::mock_wait_oscsf_set reports a timeout.
 * @details -1 means "no bit fails" (every set-poll succeeds).
 */
static int s_set_fail_bit = -1;

/* =============================================================================
 * Stateful USBCKCR / USB60CKCR register model.
 *
 * The driver caches `ra8_sys_usbck*()` once per SRDY wait, so the SRDY level a
 * wait observes is fixed at the accessor call that opens it. Injecting SRDY in
 * the accessor therefore fully controls each handshake step without any
 * call-count bookkeeping.
 * =============================================================================
 */

/**
 * @enum cov2_srdy_mode_t
 * @brief How the register model drives the read-only SRDY handshake bit.
 */
typedef enum : uint8_t {
  k_cov2_srdy_follow     = 0U, /**< SRDY tracks SREQ -- full handshake completes. */
  k_cov2_srdy_stuck_high = 1U, /**< SRDY latches high -- second wait times out.   */
} cov2_srdy_mode_t;

/**
 * @enum cov2_pll2_arg_t
 * @brief PLL2 argument values used to drive the validation + idempotency legs.
 */
typedef enum : uint8_t {
  k_cov2_pll2_mul          = 80U, /**< Integer multiplier (matches the driver). */
  k_cov2_pll2_ok_quarters  = 0U,  /**< Valid quarter-step value.                */
  k_cov2_pll2_bad_quarters = 4U,  /**< One past the max quarter-step (3).       */
  k_cov2_pll2_zero_mul     = 0U,  /**< Illegal zero multiplier.                 */
} cov2_pll2_arg_t;

/** @brief HOCOCR value with HCSTP set, used to drive the "restart HOCO" branch. */
static const uint8_t k_cov2_hococr_hcstp = (uint8_t)(1U << k_ra8_hococr_hcstp);

/** @brief Active SRDY drive policy for both USB clock registers. */
static cov2_srdy_mode_t s_srdy_mode = k_cov2_srdy_follow;
/** @brief Scriptable backing byte for USBCKCR (USB-FS source select). */
static volatile uint8_t s_usbfs_ckcr = 0U;
/** @brief Scriptable backing byte for USB60CKCR (USB-HS source select). */
static volatile uint8_t s_usb60ckcr = 0U;

/** @brief Apply the active SRDY policy to @p reg before a read/write dereference. */
static void cov2_apply_srdy(volatile uint8_t* reg)
{
  const uint8_t sreq = (uint8_t)(1U << k_ra8_usbckcr_bit_sreq);
  const uint8_t srdy = (uint8_t)(1U << k_ra8_usbckcr_bit_srdy);
  if (s_srdy_mode == k_cov2_srdy_stuck_high) {
    *reg = (uint8_t)(*reg | srdy);
  } else if ((*reg & sreq) != 0U) {
    *reg = (uint8_t)(*reg | srdy); /* SREQ asserted -> hardware acks SRDY=1. */
  } else {
    *reg = (uint8_t)(*reg & (uint8_t)~srdy); /* SREQ cleared -> SRDY=0. */
  }
}

/** @brief Stateful mock for `ra8_sys_usbckcr()` -- drives the USB-FS handshake. */
static volatile uint8_t* mock_ra8_sys_usbckcr(void)
{
  cov2_apply_srdy(&s_usbfs_ckcr);
  return &s_usbfs_ckcr;
}

/** @brief Stateful mock for `ra8_sys_usb60ckcr()` -- drives the USB-HS handshake. */
static volatile uint8_t* mock_ra8_sys_usb60ckcr(void)
{
  cov2_apply_srdy(&s_usb60ckcr);
  return &s_usb60ckcr;
}

/** @brief Reset the sim mirror, the OSCSF mocks, and the SRDY model to baseline. */
static void cov_reset(void)
{
  ra8_sim_mmap_reset();
  s_clear_result = k_ra8_ok;
  s_set_fail_bit = -1;
  s_srdy_mode    = k_cov2_srdy_follow;
  s_usbfs_ckcr   = 0U;
  s_usb60ckcr    = 0U;
}

/* Rename the exported symbols so the instrumented copy does not clash with the
 * production driver in ra8_core_hal; redirect the OSCSF poll helpers and the two
 * USB clock-register accessors to the scriptable mocks above. */
ra8_err_t ra8_cgc_pll2_enable_cov2(uint8_t mul_int, uint8_t mul_quarters, ra8_plodiv_t p_div_code);
ra8_err_t ra8_cgc_usbfs_clock_enable_cov2(void);
ra8_err_t ra8_cgc_usbhs_pll_enable_cov2(void);
ra8_err_t ra8_cgc_ensure_hoco_running_for_usb_ck_cov2(void);

/** @brief RA8 cgc pll2 enable. */
#define ra8_cgc_pll2_enable ra8_cgc_pll2_enable_cov2
/** @brief RA8 cgc usbfs clock enable. */
#define ra8_cgc_usbfs_clock_enable ra8_cgc_usbfs_clock_enable_cov2
/** @brief RA8 cgc usbhs pll enable. */
#define ra8_cgc_usbhs_pll_enable ra8_cgc_usbhs_pll_enable_cov2
/** @brief RA8 cgc ensure hoco running for USB ck. */
#define ra8_cgc_ensure_hoco_running_for_usb_ck ra8_cgc_ensure_hoco_running_for_usb_ck_cov2
/** @brief RA8 cgc wait oscsf set. */
#define ra8_cgc_wait_oscsf_set mock_wait_oscsf_set
/** @brief RA8 cgc wait oscsf clear. */
#define ra8_cgc_wait_oscsf_clear mock_wait_oscsf_clear
/** @brief RA8 sys usbckcr. */
#define ra8_sys_usbckcr mock_ra8_sys_usbckcr
/** @brief RA8 sys usb60ckcr. */
#define ra8_sys_usb60ckcr mock_ra8_sys_usb60ckcr

/* Undefine RA8_SIMULATOR_MODE for the driver body ONLY so the static SRDY poll
 * helpers read the (mocked) register model instead of faking the handshake. */
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
 * @test test_pll2_arg_and_idempotency
 *
 * @brief PLL2 argument validation + the "already locked" idempotency return.
 *
 * @details Three legs the sibling never drives: `mul_int == 0` and
 * `mul_quarters > 3` both return ::k_ra8_err_invalid_arg, and pre-asserting
 * OSCSF.PLL2SF makes the idempotency guard short-circuit to ::k_ra8_ok without
 * re-programming. A fourth call with clear PLL2SF and passing polls exercises
 * the full program + lock success return.
 *
 * @par MC/DC:
 * (no compound decisions in the code under test -- each guard is a single
 * condition `if`, no `&&`/`||`.)
 */
static void test_pll2_arg_and_idempotency(void)
{
  TEST_BEGIN("pll2: zero-mul + bad-quarters reject; already-locked skips; ok programs");
  cov_reset();

  /* mul_int == 0 -> invalid_arg. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_cgc_pll2_enable_cov2((uint8_t)k_cov2_pll2_zero_mul,
                                          (uint8_t)k_cov2_pll2_ok_quarters,
                                          k_ra8_plodiv_div4));

  /* mul_quarters > 3 -> invalid_arg. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_cgc_pll2_enable_cov2((uint8_t)k_cov2_pll2_mul,
                                          (uint8_t)k_cov2_pll2_bad_quarters,
                                          k_ra8_plodiv_div4));

  /* OSCSF.PLL2SF pre-asserted -> idempotency short-circuit returns ok. */
  cov_reset();
  *ra8_sys_oscsf() = (uint8_t)(1U << k_ra8_oscsf_bit_pll2sf);
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_cgc_pll2_enable_cov2((uint8_t)k_cov2_pll2_mul,
                                          (uint8_t)k_cov2_pll2_ok_quarters,
                                          k_ra8_plodiv_div4));

  /* PLL2SF clear + passing polls -> full program + lock success return. */
  cov_reset();
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_cgc_pll2_enable_cov2((uint8_t)k_cov2_pll2_mul,
                                          (uint8_t)k_cov2_pll2_ok_quarters,
                                          k_ra8_plodiv_div4));

  TEST_END("pll2: zero-mul + bad-quarters reject; already-locked skips; ok programs");
}

/**
 * @test test_usbfs_pll2_enable_failed
 *
 * @brief USB-FS bring-up aborts when the inner PLL2 enable fails.
 *
 * @details PLL2SF starts clear so the program path runs, but its lock poll is
 * scripted to time out. `ra8_cgc_usbfs_clock_enable` then takes its "pll2 enable
 * failed" return -- a leg the sibling never reaches (it only fails PLL2 on the
 * USB-HS path).
 *
 * @par MC/DC:
 * (no compound decisions in the code under test -- single-condition
 * `if (pll2_err != k_ra8_ok)`.)
 */
static void test_usbfs_pll2_enable_failed(void)
{
  TEST_BEGIN("usbfs: inner PLL2 lock timeout aborts before the handshake");
  cov_reset();

  s_set_fail_bit = (int)k_ra8_oscsf_bit_pll2sf; /* PLL2SF lock poll times out. */
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_cgc_usbfs_clock_enable_cov2());

  TEST_END("usbfs: inner PLL2 lock timeout aborts before the handshake");
}

/**
 * @test test_usbfs_hoco_stabilize_timeout
 *
 * @brief USB-FS bring-up aborts when the HOCO stabilization poll times out.
 *
 * @details PLL2 locks (PLL2SF poll passes) but the HOCOSF set poll is scripted
 * to fail, so the routine takes its "HOCO stabilization timeout" return before
 * the USBCKCR handshake.
 *
 * @par MC/DC:
 * (no compound decisions in the code under test -- single-condition
 * `if (hoco_err != k_ra8_ok)`.)
 */
static void test_usbfs_hoco_stabilize_timeout(void)
{
  TEST_BEGIN("usbfs: HOCO stabilization poll timeout aborts before handshake");
  cov_reset();

  s_set_fail_bit = (int)k_ra8_oscsf_bit_hocosf; /* PLL2SF passes; HOCOSF fails. */
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_cgc_usbfs_clock_enable_cov2());

  TEST_END("usbfs: HOCO stabilization poll timeout aborts before handshake");
}

/**
 * @test test_usbfs_hcstp_and_second_srdy_timeout
 *
 * @brief USB-FS HOCO-restart branch + the USBCKCR **second** SRDY (SRDY=0) timeout.
 *
 * @details HOCOCR.HCSTP is pre-asserted so the in-line HOCO-restart branch
 * clears it, and every OSCSF poll passes so the routine reaches the USBCKCR
 * handshake. With the register model in ::k_cov2_srdy_stuck_high the FIRST wait
 * (SRDY=1) is acknowledged immediately, but SRDY never clears, so the SECOND
 * wait (SRDY=0) runs to its bounded cap and times out -- driving the "SRDY=0
 * timeout" leg and the handshake error return.
 *
 * @par MC/DC:
 * (no compound decisions in the code under test -- the HCSTP guard, the poll
 * `if (got == expected)`, and each `if (err != k_ra8_ok)` are single-condition.)
 */
static void test_usbfs_hcstp_and_second_srdy_timeout(void)
{
  TEST_BEGIN("usbfs: HCSTP restart + USBCKCR second-wait (SRDY=0) timeout");
  cov_reset();

  *ra8_sys_hococr() = k_cov2_hococr_hcstp; /* Exercise the HOCO-restart branch. */
  s_set_fail_bit    = -1;                  /* All OSCSF polls pass.             */
  s_srdy_mode       = k_cov2_srdy_stuck_high;
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_cgc_usbfs_clock_enable_cov2());

  TEST_END("usbfs: HCSTP restart + USBCKCR second-wait (SRDY=0) timeout");
}

/**
 * @test test_usbfs_full_success
 *
 * @brief USB-FS bring-up completes end-to-end when the handshake acknowledges.
 *
 * @details Every OSCSF poll passes and the register model is in
 * ::k_cov2_srdy_follow, so SRDY tracks SREQ: the first wait sees SRDY=1 and the
 * second sees SRDY=0. The USBCKCR handshake completes and
 * `ra8_cgc_usbfs_clock_enable` takes its terminal ::k_ra8_ok return -- a line the
 * sibling can never reach (its handshake always times out).
 *
 * @par MC/DC:
 * (no compound decisions in the code under test -- the poll and error checks
 * are single-condition.)
 */
static void test_usbfs_full_success(void)
{
  TEST_BEGIN("usbfs: full bring-up succeeds when SRDY acknowledges");
  cov_reset();

  s_set_fail_bit = -1;                 /* All OSCSF polls pass. */
  s_srdy_mode    = k_cov2_srdy_follow; /* SRDY tracks SREQ.     */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cgc_usbfs_clock_enable_cov2());

  TEST_END("usbfs: full bring-up succeeds when SRDY acknowledges");
}

/**
 * @test test_ensure_hoco_hcstp_restart
 *
 * @brief The HOCO-restart helper clears HCSTP then honours the poll result.
 *
 * @details HOCOCR.HCSTP is pre-asserted so the "restart HOCO" branch fires and
 * clears the bit; with the HOCOSF set poll passing the helper returns ::k_ra8_ok.
 *
 * @par MC/DC:
 * (no compound decisions in the code under test -- the HCSTP guard is a single
 * condition `if` and the return is the poll result directly.)
 */
static void test_ensure_hoco_hcstp_restart(void)
{
  TEST_BEGIN("ensure_hoco: clears HCSTP then returns the stabilization poll result");
  cov_reset();

  *ra8_sys_hococr() = k_cov2_hococr_hcstp;
  s_set_fail_bit    = -1;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cgc_ensure_hoco_running_for_usb_ck_cov2());

  TEST_END("ensure_hoco: clears HCSTP then returns the stabilization poll result");
}

/**
 * @test test_usbhs_pll2_enable_failed
 *
 * @brief USB-HS bring-up aborts when PLL2 enable fails after MOSCSF is stable.
 *
 * @details MOSCSF passes, but the PLL2 lock poll times out, so the USB-HS
 * routine takes its "pll2 enable failed" return.
 *
 * @par MC/DC:
 * (no compound decisions in the code under test -- single-condition
 * `if (pll2_err != k_ra8_ok)`.)
 */
static void test_usbhs_pll2_enable_failed(void)
{
  TEST_BEGIN("usbhs: PLL2 enable failure propagates after MOSCSF is stable");
  cov_reset();

  s_set_fail_bit = (int)k_ra8_oscsf_bit_pll2sf; /* MOSCSF passes; PLL2SF fails. */
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_cgc_usbhs_pll_enable_cov2());

  TEST_END("usbhs: PLL2 enable failure propagates after MOSCSF is stable");
}

/**
 * @test test_usbhs_hoco_stabilize_timeout
 *
 * @brief USB-HS bring-up aborts when the HOCO stabilization poll times out.
 *
 * @details MOSCSF and PLL2 succeed, but the HOCOSF set poll inside the
 * HOCO-restart helper fails, so the routine takes its "HOCO stabilization
 * timeout" return before the USB60CKCR handshake.
 *
 * @par MC/DC:
 * (no compound decisions in the code under test -- single-condition
 * `if (hoco_err != k_ra8_ok)`.)
 */
static void test_usbhs_hoco_stabilize_timeout(void)
{
  TEST_BEGIN("usbhs: HOCO stabilization poll timeout aborts before handshake");
  cov_reset();

  s_set_fail_bit = (int)k_ra8_oscsf_bit_hocosf; /* MOSCSF + PLL2SF pass; HOCOSF fails. */
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_cgc_usbhs_pll_enable_cov2());

  TEST_END("usbhs: HOCO stabilization poll timeout aborts before handshake");
}

/**
 * @test test_usbhs_hcstp_and_second_srdy_timeout
 *
 * @brief USB-HS ensure-HOCO HCSTP restart + USB60CKCR **second** SRDY timeout.
 *
 * @details Every OSCSF poll passes and HOCOCR.HCSTP is pre-asserted so the
 * ensure-HOCO helper clears it, then the routine reaches the USB60CKCR
 * handshake. With the register model in ::k_cov2_srdy_stuck_high the first wait
 * (SRDY=1) is acknowledged but SRDY never clears, so the second wait (SRDY=0)
 * runs to its bounded cap and times out -- driving the "SRDY=0 timeout" leg, the
 * diagnostic probe read-back, and the handshake error return.
 *
 * @par MC/DC:
 * (no compound decisions in the code under test -- the HCSTP guard, the poll
 * `if (got == expected)`, and each `if (err != k_ra8_ok)` are single-condition.)
 */
static void test_usbhs_hcstp_and_second_srdy_timeout(void)
{
  TEST_BEGIN("usbhs: HCSTP restart + USB60CKCR second-wait (SRDY=0) timeout");
  cov_reset();

  *ra8_sys_hococr() = k_cov2_hococr_hcstp; /* Exercise the ensure-HOCO restart. */
  s_set_fail_bit    = -1;                  /* All OSCSF polls pass.             */
  s_srdy_mode       = k_cov2_srdy_stuck_high;
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_cgc_usbhs_pll_enable_cov2());

  TEST_END("usbhs: HCSTP restart + USB60CKCR second-wait (SRDY=0) timeout");
}

int32_t main(void)
{
  test_pll2_arg_and_idempotency();
  test_usbfs_pll2_enable_failed();
  test_usbfs_hoco_stabilize_timeout();
  test_usbfs_hcstp_and_second_srdy_timeout();
  test_usbfs_full_success();
  test_ensure_hoco_hcstp_restart();
  test_usbhs_pll2_enable_failed();
  test_usbhs_hoco_stabilize_timeout();
  test_usbhs_hcstp_and_second_srdy_timeout();
  (void)fprintf(stderr, "[OK  ] test_ra8_cgc_usb_cov2.c\n");
  return 0;
}
