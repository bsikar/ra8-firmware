/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file test_app_touch_cal.c
 * @brief Integration test: touch_cal demo logic + end-to-end calibration solve.
 *
 * @details
 * Mirrors examples/ek_ra8d2/hil_needs_revalidation/touch_cal/main.c. The GLCDC panel
 * bring-up is owned + covered by glcdc_render / the display PAL tests, the GT911
 * transport by test_ra8_touch.c, and the affine math by test_ra8_touch_cal.c;
 * this test pins down the app-level decision logic with full MC/DC:
 *
 *  - the framebuffer bounds guard ``tc_pixel_in_bounds`` (2 conditions),
 *  - the usable-sample gate ``tc_sample_usable`` (2 conditions),
 *  - the pass verdict ``tc_cal_pass`` (3 conditions),
 *  - the per-axis residual ``tc_axis_err``,
 *
 * and it closes the loop with an end-to-end solve over the EXACT synthetic raw
 * points the app's hil.conf feeds ra8_emulator via ``--touch-seq``: it builds the
 * five ``ra8_touch_cal_run`` targets (four inset corners + centre of the 512x512
 * screen), distorts them through the same synthetic panel transform the
 * fake replays, solves with ``ra8_touch_cal_compute``, round-trips through
 * ``ra8_touch_cal_save`` / ``ra8_touch_cal_load``, re-applies with
 * ``ra8_touch_cal_apply``, and asserts a zero residual -- proving the EIL feed
 * the HIL gate relies on is mathematically exact.
 *
 * No MMIO is required, so this test runs in-process.
 *
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_touch_cal.h"
#include "unity_minimal.h"

/**
 * @enum t_tc_geom_t
 * @brief Mirror of the app's screen + calibration geometry.
 */
typedef enum : uint16_t {
  k_t_tc_fb_w       = 512U, /**< Screen width, pixels.     */
  k_t_tc_fb_h       = 512U, /**< Screen height, pixels.    */
  k_t_tc_inset_px   = 40U,  /**< Corner-target inset.      */
  k_t_tc_verify_tol = 2U,   /**< Allowed fit residual, px. */
} t_tc_geom_t;

/**
 * @enum t_tc_synth_t
 * @brief Synthetic panel transform the fake replays (raw = screen*g + b).
 *
 * @details
 * The app's hil.conf feeds ra8_emulator these raw points via ``--touch-seq``; the
 * fit must recover the inverse and reproduce each target exactly. Kept in one
 * place so a drift between this test and the HIL feed is a compile-time edit.
 */
typedef enum : uint16_t {
  k_t_tc_gain  = 8U,   /**< Raw-per-screen-pixel gain. */
  k_t_tc_off_x = 100U, /**< Raw X bias.                */
  k_t_tc_off_y = 200U, /**< Raw Y bias.                */
} t_tc_synth_t;

/** @brief Decimal / index sizing (mirror of the app). */
typedef enum : uint8_t {
  k_t_tc_n = 5U, /**< ::k_ra8_touch_cal_n_targets mirror. */
} t_tc_count_t;

/* ===========================================================================
 * Mirrors of the app's pure decision helpers
 * ===========================================================================
 */

/** @brief Mirror of tc_pixel_in_bounds(). */
static bool tc_pixel_in_bounds(int32_t v, int32_t dim)
{
  return (v >= 0) && (v < dim);
}

/** @brief Mirror of tc_sample_usable(). */
static bool tc_sample_usable(ra8_err_t rc, uint8_t got)
{
  return (rc == k_ra8_ok) && (got >= 1U);
}

/** @brief Mirror of tc_cal_pass(). */
static bool tc_cal_pass(ra8_err_t run_rc, int32_t maxerr, int32_t tol, bool blob_ok)
{
  return (run_rc == k_ra8_ok) && (maxerr <= tol) && blob_ok;
}

/** @brief Mirror of tc_axis_err(). */
static int32_t tc_axis_err(int32_t a, int32_t b)
{
  const int32_t d = a - b;
  return (d < 0) ? -d : d;
}

/* ===========================================================================
 * Test-local helpers for the end-to-end solve
 * ===========================================================================
 */

/** @brief Build the five ra8_touch_cal_run targets for a WxH / inset screen. */
static void tc_build_targets(ra8_touch_cal_point_t t[k_t_tc_n])
{
  const int32_t w   = (int32_t)k_t_tc_fb_w;
  const int32_t h   = (int32_t)k_t_tc_fb_h;
  const int32_t ins = (int32_t)k_t_tc_inset_px;
  t[0]              = (ra8_touch_cal_point_t){ins, ins};                 /* top-left     */
  t[1]              = (ra8_touch_cal_point_t){w - 1 - ins, ins};         /* top-right    */
  t[2]              = (ra8_touch_cal_point_t){w - 1 - ins, h - 1 - ins}; /* bottom-right */
  t[3]              = (ra8_touch_cal_point_t){ins, h - 1 - ins};         /* bottom-left  */
  t[4]              = (ra8_touch_cal_point_t){w / 2, h / 2};             /* centre       */
}

/** @brief Distort a screen target into a raw sample via the synthetic panel. */
static ra8_touch_cal_point_t tc_synth_raw(ra8_touch_cal_point_t screen)
{
  ra8_touch_cal_point_t raw = {};
  raw.x                     = (screen.x * (int32_t)k_t_tc_gain) + (int32_t)k_t_tc_off_x;
  raw.y                     = (screen.y * (int32_t)k_t_tc_gain) + (int32_t)k_t_tc_off_y;
  return raw;
}

/* ===========================================================================
 * MC/DC tests of the app decision logic
 * ===========================================================================
 */

/**
 * @brief The bounds guard admits an on-panel pixel and rejects off-panel ones.
 *
 * @par MC/DC:
 * Decision: ``(v >= 0) && (v < dim)`` (2 conditions A and B).
 * - Vector 1: v=100, dim=512 -> true  (control: A=T, B=T)
 * - Vector 2: v=-1,  dim=512 -> false (varies A only: A=F, B=T)
 * - Vector 3: v=512, dim=512 -> false (varies B only: A=T, B=F)
 * Vectors 1+2 prove A independently affects the outcome; 1+3 prove the same
 * for B. N+1 = 3 vectors for N=2 conditions: minimal MC/DC.
 */
static void test_tc_pixel_in_bounds_mcdc(void)
{
  TEST_BEGIN("touch_cal: pixel-in-bounds MC/DC");
  TEST_ASSERT(tc_pixel_in_bounds(100, (int32_t)k_t_tc_fb_w));  /* control      */
  TEST_ASSERT(!tc_pixel_in_bounds(-1, (int32_t)k_t_tc_fb_w));  /* vary A false */
  TEST_ASSERT(!tc_pixel_in_bounds(512, (int32_t)k_t_tc_fb_w)); /* vary B false */
  TEST_END("touch_cal: pixel-in-bounds MC/DC");
}

/**
 * @brief A sample is usable only when the read succeeded AND a contact arrived.
 *
 * @par MC/DC:
 * Decision: ``(rc == k_ra8_ok) && (got >= 1)`` (2 conditions A and B).
 * - Vector 1: rc=ok,  got=1 -> true  (control: A=T, B=T)
 * - Vector 2: rc=err, got=1 -> false (varies A only)
 * - Vector 3: rc=ok,  got=0 -> false (varies B only)
 * Vectors 1+2 prove A independently affects the outcome; 1+3 prove the same
 * for B. N+1 = 3 vectors for N=2 conditions: minimal MC/DC.
 */
static void test_tc_sample_usable_mcdc(void)
{
  TEST_BEGIN("touch_cal: sample-usable MC/DC");
  TEST_ASSERT(tc_sample_usable(k_ra8_ok, 1U));            /* control      */
  TEST_ASSERT(!tc_sample_usable(k_ra8_err_hw_error, 1U)); /* vary A false */
  TEST_ASSERT(!tc_sample_usable(k_ra8_ok, 0U));           /* vary B false */
  TEST_END("touch_cal: sample-usable MC/DC");
}

/**
 * @brief The pass verdict is true only when the solve succeeded, the residual
 *        is within tolerance, and the serialised matrix round-tripped.
 *
 * @par MC/DC:
 * Decision: ``(run_rc == ok) && (maxerr <= tol) && blob_ok`` (3 conditions
 * A/B/C).
 * - Vector 1: ok,  0<=2, true  -> true  (control)
 * - Vector 2: err, 0<=2, true  -> false (varies A only)
 * - Vector 3: ok,  3>2,  true  -> false (varies B only)
 * - Vector 4: ok,  0<=2, false -> false (varies C only)
 * Vectors 1+2 / 1+3 / 1+4 prove A/B/C each independently affect the outcome.
 * N+1 = 4 vectors for N=3 conditions: minimal MC/DC.
 */
static void test_tc_cal_pass_mcdc(void)
{
  TEST_BEGIN("touch_cal: cal-pass verdict MC/DC");
  const int32_t tol = (int32_t)k_t_tc_verify_tol;
  TEST_ASSERT(tc_cal_pass(k_ra8_ok, 0, tol, true));            /* control */
  TEST_ASSERT(!tc_cal_pass(k_ra8_err_hw_error, 0, tol, true)); /* vary A  */
  TEST_ASSERT(!tc_cal_pass(k_ra8_ok, 3, tol, true));           /* vary B  */
  TEST_ASSERT(!tc_cal_pass(k_ra8_ok, 0, tol, false));          /* vary C  */
  TEST_END("touch_cal: cal-pass verdict MC/DC");
}

/**
 * @brief The per-axis residual is the absolute difference, symmetric in sign.
 *
 * @par MC/DC:
 * No compound decision; the single ``(d < 0)`` branch is exercised both ways
 * (mapped below and above the target) plus the zero-residual case.
 */
static void test_tc_axis_err(void)
{
  TEST_BEGIN("touch_cal: axis-error");
  TEST_ASSERT_EQ(0, tc_axis_err(40, 40)); /* exact       */
  TEST_ASSERT_EQ(3, tc_axis_err(43, 40)); /* above (d>0) */
  TEST_ASSERT_EQ(3, tc_axis_err(37, 40)); /* below (d<0) */
  TEST_END("touch_cal: axis-error");
}

/* ===========================================================================
 * End-to-end calibration solve over the app's exact EIL feed
 * ===========================================================================
 */

/**
 * @brief The five synthetic raws the HIL feed replays recover the transform
 *        exactly: every corrected coordinate lands on its target (residual 0).
 *
 * @par MC/DC:
 * No compound decision under test here; this is the integration arm that ties
 * the app's ``--touch-seq`` feed to the real ``ra8_touch_cal`` solver, apply,
 * and save/load round-trip, asserting a zero residual and a bit-exact blob.
 */
static void test_tc_end_to_end_solve(void)
{
  TEST_BEGIN("touch_cal: end-to-end solve residual 0");
  ra8_touch_cal_point_t targets[k_t_tc_n] = {};
  ra8_touch_cal_point_t raws[k_t_tc_n]    = {};
  tc_build_targets(targets);
  for (uint8_t i = 0U; i < (uint8_t)k_t_tc_n; i++) {
    raws[i] = tc_synth_raw(targets[i]);
  }

  /* Pin the raws to the exact points the app's hil.conf feeds ra8_emulator, so a
   * drift between the two is caught here. */
  TEST_ASSERT_EQ(420, raws[0].x);
  TEST_ASSERT_EQ(520, raws[0].y);
  TEST_ASSERT_EQ(3868, raws[1].x);
  TEST_ASSERT_EQ(3968, raws[2].y);
  TEST_ASSERT_EQ(2148, raws[4].x);
  TEST_ASSERT_EQ(2248, raws[4].y);

  ra8_touch_cal_matrix_t m  = {};
  const ra8_err_t        rc = ra8_touch_cal_compute(raws, targets, (uint8_t)k_t_tc_n, &m);
  TEST_ASSERT_EQ(k_ra8_ok, rc);

  /* Round-trip the matrix through the storage blob: load-then-re-save must
   * reproduce the original blob byte-for-byte (comparing bytes, not the float
   * struct, sidesteps the ill-defined float object comparison). */
  uint8_t                blob0[k_ra8_touch_cal_blob_size] = {};
  uint8_t                blob1[k_ra8_touch_cal_blob_size] = {};
  ra8_touch_cal_matrix_t back                             = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_touch_cal_save(&m, blob0, sizeof(blob0)));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_touch_cal_load(blob0, sizeof(blob0), &back));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_touch_cal_save(&back, blob1, sizeof(blob1)));
  TEST_ASSERT_EQ(0, memcmp(blob0, blob1, sizeof(blob0)));

  /* Re-apply the loaded matrix to every raw; the residual must be zero. */
  int32_t worst = 0;
  for (uint8_t i = 0U; i < (uint8_t)k_t_tc_n; i++) {
    ra8_touch_cal_point_t screen = {};
    TEST_ASSERT_EQ(
      k_ra8_ok,
      ra8_touch_cal_apply(raws[i], &back, (uint16_t)k_t_tc_fb_w, (uint16_t)k_t_tc_fb_h, &screen));
    const int32_t ex = tc_axis_err(screen.x, targets[i].x);
    const int32_t ey = tc_axis_err(screen.y, targets[i].y);
    worst            = (ex > worst) ? ex : worst;
    worst            = (ey > worst) ? ey : worst;
  }
  TEST_ASSERT_EQ(0, worst);
  TEST_ASSERT(tc_cal_pass(rc, worst, (int32_t)k_t_tc_verify_tol, true));
  TEST_END("touch_cal: end-to-end solve residual 0");
}

int main(void)
{
  test_tc_pixel_in_bounds_mcdc();
  test_tc_sample_usable_mcdc();
  test_tc_cal_pass_mcdc();
  test_tc_axis_err();
  test_tc_end_to_end_solve();
  return 0;
}
