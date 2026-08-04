/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file test_ra8_touch_cal.c
 * @brief Unit tests for the ra8_touch_cal calibration utility
 *
 * @details
 * Drives the calibration utility entirely on the host -- no register
 * fake needed. The tests exercise:
 *
 *   1. ``ra8_touch_cal_compute`` against a synthetic linear ground-truth
 *      transform with known coefficients (3 and 5 sample variants).
 *   2. ``ra8_touch_cal_apply`` round-trip on the recovered matrix.
 *   3. ``ra8_touch_cal_run`` driven by stub LCD/touch shims.
 *   4. ``ra8_touch_cal_save`` / ``ra8_touch_cal_load`` byte-identical
 *      round-trip plus CRC corruption rejection.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_touch_cal.h"
#include "unity_minimal.h"

/**
 * @enum t_tc_sample_t
 * @brief Calibration sample counts and the point sets fed to the solver.
 *
 * @details
 * The routine calibrates from five touch points (four corners plus centre),
 * so `k_t_cal_points` sizes every point array and is the only accepted count.
 * The collinear triples exist to make the least-squares system singular.
 */
typedef enum : int16_t {
  k_t_collinear_r1 = 100,  /**< Second raw point of the collinear triple.    */
  k_t_collinear_r2 = 200,  /**< Third raw point; 0/100/200 lie on one line.  */
  k_t_collinear_s1 = 10,   /**< Second screen point of the collinear triple. */
  k_t_collinear_s2 = 20,   /**< Third screen point.                          */
  k_t_below_range  = 50,   /**< Magnitude of the negative input the identity
                                transform must clip to 0.                      */
  k_t_above_range  = 9999, /**< Input past the screen edge, clipped to w-1. */
} t_tc_sample_t;

/**
 * @enum t_tc_count_t
 * @brief The fixed calibration-point count.
 *
 * @details
 * Kept out of ::t_tc_sample_t and sized to the smallest type that holds it,
 * rather than inheriting that enum's `int16_t` width from the sample
 * magnitudes it groups. The distinction is load-bearing, not cosmetic: this
 * constant is an array extent and a loop upper bound, so an `int16_t` width
 * makes every `uint8_t` index narrower than the bound it is compared against.
 *
 * @invariant Equals the extent of every `ra8_touch_cal_point_t[]` in this file.
 *
 * @see t_tc_sample_t
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_t_cal_points = 5, /**< Calibration points the solver requires. */
} t_tc_count_t;

/**
 * @enum t_tc_screen_t
 * @brief Screen geometries and target insets the run-config guards reject.
 *
 * @details
 * The guard rejects an inset that leaves no room between opposing targets, so
 * the pairs below straddle it: 10 px is comfortable on any of these screens,
 * 60 px is not on a 100 px edge.
 */
typedef enum : uint16_t {
  k_t_screen_small = 100U, /**< Screen edge small enough that a wide inset fails. */
  k_t_screen_mid   = 200U, /**< A larger screen edge.                             */
  k_t_screen_wide  = 320U, /**< The QVGA width used by the happy-path run.        */
  k_t_inset_ok     = 10U,  /**< An inset that leaves room on every screen above.  */
  k_t_inset_wide   = 60U,  /**< An inset that collapses the 100 px screen.        */
} t_tc_screen_t;

/**
 * @enum t_tc_blob_t
 * @brief Byte values that invalidate a serialised calibration blob.
 */
typedef enum : uint8_t {
  k_t_bad_reserved = 0x42U, /**< Non-zero written into a reserved byte, which
                                 the loader must reject.                       */
  k_t_corrupt_mask = 0xFFU, /**< XOR mask that flips a magic, version or
                                 coefficient byte to force a load failure.      */
} t_tc_blob_t;

/** @brief Rounding bias added before truncating a float coordinate to int. */
static const float k_t_round_half = 0.5F;

/**
 * @brief Tolerance for the affine translation terms (c, f), in pixels.
 *
 * Looser than the scale tolerance because c and f accumulate the rounding of
 * every sampled point rather than a ratio between them.
 */
static const float k_t_tol_translate = 1.0e-1F;

/** @brief Tolerance for the affine scale/shear terms (a, e), dimensionless. */
static const float k_t_tol_scale = 1.0e-3F;

/**
 * @enum tc_test_const_t
 * @brief Test-only numeric constants.
 */
typedef enum : uint16_t {
  k_tc_screen_w = 1024U, /**< Tc screen w.                         */
  k_tc_screen_h = 600U,  /**< Tc screen h.                         */
  k_tc_inset    = 32U,   /**< Tc inset.                            */
  k_tc_blob     = 36U,   /**< Mirrors ::k_ra8_touch_cal_blob_size. */
} tc_test_const_t;

/**
 * @enum tc_raw_const_t
 * @brief Raw controller axis ranges used by the synthetic ground truth.
 */
typedef enum : uint16_t {
  k_tc_raw_min    = 100U,  /**< Tc raw minimum. */
  k_tc_raw_max    = 3900U, /**< Tc raw maximum. */
  k_tc_raw_centre = 2000U, /**< Tc raw centre.  */
  k_tc_raw_step   = 1000U, /**< Tc raw step.    */
} tc_raw_const_t;

/**
 * @struct stub_state_t
 * @brief Mutable state for the LCD/touch shim stubs.
 *
 * @details
 * ``draws`` records every cross-hair coordinate the utility paints,
 * ``reads_idx`` walks a caller-provided array of synthetic raw samples.
 */
typedef struct {
  ra8_touch_cal_point_t        draws[k_ra8_touch_cal_n_targets]; /**< Draws.        */
  uint8_t                      n_draws;                          /**< N draws.      */
  const ra8_touch_cal_point_t* reads;                            /**< Reads.        */
  uint8_t                      reads_idx;                        /**< Reads index.  */
  uint8_t                      n_reads;                          /**< N reads.      */
  ra8_err_t                    forced_err;                       /**< Forced error. */
} stub_state_t;

/**
 * @brief Stub LCD shim -- records cross-hair coordinates.
 */
static ra8_err_t stub_draw(void* ctx, ra8_touch_cal_point_t target)
{
  stub_state_t* s = (stub_state_t*)ctx;
  if (s->forced_err != k_ra8_ok) {
    return s->forced_err;
  }
  if (s->n_draws < (uint8_t)k_ra8_touch_cal_n_targets) {
    s->draws[s->n_draws] = target;
    s->n_draws++;
  }
  return k_ra8_ok;
}

/**
 * @brief Stub touch shim -- replays a fixed array of raw samples.
 */
static ra8_err_t stub_read(void* ctx, ra8_touch_cal_point_t* out_raw)
{
  stub_state_t* s = (stub_state_t*)ctx;
  if (s->reads_idx >= s->n_reads) {
    return k_ra8_err_hw_error;
  }
  *out_raw = s->reads[s->reads_idx];
  s->reads_idx++;
  return k_ra8_ok;
}

/**
 * @brief Map a raw point through a known ground-truth affine.
 */
static ra8_touch_cal_point_t apply_truth(ra8_touch_cal_point_t raw, const ra8_touch_cal_matrix_t* m)
{
  const float           xf  = (float)raw.x;
  const float           yf  = (float)raw.y;
  const float           u   = (m->a * xf) + (m->b * yf) + m->c;
  const float           v   = (m->d * xf) + (m->e * yf) + m->f;
  ra8_touch_cal_point_t out = {
    .x = (int32_t)(u + k_t_round_half),
    .y = (int32_t)(v + k_t_round_half),
  };
  return out;
}

/**
 * @brief Test 1 -- 3-point exact fit recovers the ground-truth matrix.
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */
static void test_compute_three_point(void)
{
  const ra8_touch_cal_matrix_t truth = {
    .a = 0.25F,
    .b = 0.0F,
    .c = -25.0F,
    .d = 0.0F,
    .e = 0.20F,
    .f = -10.0F,
  };
  const ra8_touch_cal_point_t raw[3] = {
    {(int32_t)k_tc_raw_min, (int32_t)k_tc_raw_min},
    {(int32_t)k_tc_raw_max, (int32_t)k_tc_raw_min},
    {(int32_t)k_tc_raw_centre, (int32_t)k_tc_raw_max},
  };
  ra8_touch_cal_point_t scr[3] = {{0, 0}, {0, 0}, {0, 0}};
  for (uint8_t i = 0U; i < 3U; i++) {
    scr[i] = apply_truth(raw[i], &truth);
  }

  ra8_touch_cal_matrix_t got = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_touch_cal_compute(raw, scr, 3U, &got));

  /* Recovered coefficients should match within 1e-3. */
  TEST_ASSERT(((got.a - truth.a) < k_t_tol_scale) && ((truth.a - got.a) < k_t_tol_scale));
  TEST_ASSERT(((got.e - truth.e) < k_t_tol_scale) && ((truth.e - got.e) < k_t_tol_scale));
  TEST_ASSERT(((got.c - truth.c) < k_t_tol_translate) && ((truth.c - got.c) < k_t_tol_translate));
  TEST_ASSERT(((got.f - truth.f) < k_t_tol_translate) && ((truth.f - got.f) < k_t_tol_translate));
}

/**
 * @brief Test 2 -- 5-point least-squares fit recovers truth (no noise).
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */
static void test_compute_five_point(void)
{
  /* All five (raw -> screen) samples must remain inside the panel,
   * otherwise ra8_touch_cal_apply's clip would silently reshape the
   * residual. The coefficients below produce on-panel u in [0, 1023]
   * and v in [0, 599] for every chosen raw sample. */
  const ra8_touch_cal_matrix_t truth = {
    .a = 0.25F,
    .b = 0.005F,
    .c = 10.0F,
    .d = 0.005F,
    .e = 0.14F,
    .f = 20.0F,
  };
  const ra8_touch_cal_point_t raw[5] = {
    {100, 100},
    {3800, 200},
    {3700, 3700},
    {300, 3650},
    {2000, 1800},
  };
  ra8_touch_cal_point_t scr[k_t_cal_points] = {{0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}};
  for (uint8_t i = 0U; i < k_t_cal_points; i++) {
    scr[i] = apply_truth(raw[i], &truth);
  }

  ra8_touch_cal_matrix_t got = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_touch_cal_compute(raw, scr, 5U, &got));

  /* Round-trip: each raw sample must map back to within a small pixel
   * tolerance. Even with an exact-truth dataset, integer rounding of
   * scr at fit time produces a least-squares residual that scales with
   * the raw magnitude (~3800), so a few-pixel tolerance is correct. */
  for (uint8_t i = 0U; i < k_t_cal_points; i++) {
    ra8_touch_cal_point_t mapped = {0, 0};
    TEST_ASSERT_EQ(
      k_ra8_ok,
      ra8_touch_cal_apply(raw[i], &got, (uint16_t)k_tc_screen_w, (uint16_t)k_tc_screen_h, &mapped));
    const int32_t dx = mapped.x - scr[i].x;
    const int32_t dy = mapped.y - scr[i].y;
    TEST_ASSERT((dx <= 5) && (dx >= -5));
    TEST_ASSERT((dy <= 5) && (dy >= -5));
  }
}

/**
 * @brief Test 3 -- ``ra8_touch_cal_compute`` rejects bad inputs.
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */
static void test_compute_bad_inputs(void)
{
  ra8_touch_cal_point_t  pts[k_t_cal_points] = {{0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}};
  ra8_touch_cal_matrix_t m                   = {};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_touch_cal_compute(nullptr, pts, 5U, &m));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_touch_cal_compute(pts, nullptr, 5U, &m));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_touch_cal_compute(pts, pts, 5U, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_touch_cal_compute(pts, pts, 2U, &m));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_touch_cal_compute(pts, pts, 6U, &m));
  /* All-collinear samples -> singular system. */
  ra8_touch_cal_point_t coll_raw[3] = {{0, 0},
                                       {k_t_collinear_r1, k_t_collinear_r1},
                                       {k_t_collinear_r2, k_t_collinear_r2}};
  ra8_touch_cal_point_t coll_scr[3] = {{0, 0},
                                       {k_t_collinear_s1, k_t_collinear_s1},
                                       {k_t_collinear_s2, k_t_collinear_s2}};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_touch_cal_compute(coll_raw, coll_scr, 3U, &m));
}

/**
 * @brief Test 4 -- ``ra8_touch_cal_apply`` clips to panel and rejects NULL.
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */
static void test_apply_clip_and_null(void)
{
  ra8_touch_cal_matrix_t m = {
    .a = 1.0F,
    .b = 0.0F,
    .c = 0.0F,
    .d = 0.0F,
    .e = 1.0F,
    .f = 0.0F,
  };
  ra8_touch_cal_point_t out = {0, 0};

  /* NULL guard. */
  ra8_touch_cal_point_t raw = {0, 0};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_touch_cal_apply(raw, nullptr, 100U, 100U, &out));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_touch_cal_apply(raw, &m, 100U, 100U, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_touch_cal_apply(raw, &m, 0U, 100U, &out));

  /* Identity transform: clip negative to 0, clip > w-1. */
  ra8_touch_cal_point_t low  = {-k_t_below_range, -k_t_below_range};
  ra8_touch_cal_point_t high = {k_t_above_range, k_t_above_range};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_touch_cal_apply(low, &m, 100U, 100U, &out));
  TEST_ASSERT_EQ(0, out.x);
  TEST_ASSERT_EQ(0, out.y);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_touch_cal_apply(high, &m, 100U, 100U, &out));
  TEST_ASSERT_EQ(99, out.x);
  TEST_ASSERT_EQ(99, out.y);
}

/**
 * @brief Assert the recovered matrix maps every raw sample back to its target.
 * @param[in] raws    The five synthesised raw samples.
 * @param[in] targets The five on-screen calibration targets.
 * @param[in] got     The matrix recovered by ra8_touch_cal_run.
 * @pre @p got is a valid calibration matrix for @p raws / @p targets.
 * @post Every sample round-tripped within a 5-pixel tolerance.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void tc_verify_roundtrip(const ra8_touch_cal_point_t   raws[k_t_cal_points],
                                const ra8_touch_cal_point_t   targets[k_t_cal_points],
                                const ra8_touch_cal_matrix_t* got)
{
  for (uint8_t k = 0U; k < k_t_cal_points; k++) {
    ra8_touch_cal_point_t mapped = {0, 0};
    TEST_ASSERT_EQ(
      k_ra8_ok,
      ra8_touch_cal_apply(raws[k], got, (uint16_t)k_tc_screen_w, (uint16_t)k_tc_screen_h, &mapped));
    const int32_t dx = mapped.x - targets[k].x;
    const int32_t dy = mapped.y - targets[k].y;
    TEST_ASSERT((dx <= 5) && (dx >= -5));
    TEST_ASSERT((dy <= 5) && (dy >= -5));
  }
}

/**
 * @brief Synthesise the raw samples by inverting the ground-truth matrix.
 * @param[in]  targets The five on-screen calibration targets.
 * @param[in]  truth   The ground-truth calibration matrix.
 * @param[out] raws    Receives the five inverted raw samples.
 * @pre @p truth has non-zero a/e scale terms.
 * @post @p raws holds raw = screen / scale for each target.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void tc_synth_raws(const ra8_touch_cal_point_t   targets[k_t_cal_points],
                          const ra8_touch_cal_matrix_t* truth,
                          ra8_touch_cal_point_t         raws[k_t_cal_points])
{
  for (uint8_t k = 0U; k < k_t_cal_points; k++) {
    raws[k].x = (int32_t)((float)targets[k].x / truth->a);
    raws[k].y = (int32_t)((float)targets[k].y / truth->e);
  }
}

/**
 * @brief Assert the utility painted the targets in the expected visit order.
 * @param[in] targets The five expected targets in visit order.
 * @param[in] state   The stub state recording draw calls.
 * @pre @p state recorded five draws.
 * @post Every drawn point matched the expected target.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void tc_verify_draw_order(const ra8_touch_cal_point_t targets[k_t_cal_points],
                                 const stub_state_t*         state)
{
  for (uint8_t k = 0U; k < k_t_cal_points; k++) {
    TEST_ASSERT_EQ(targets[k].x, state->draws[k].x);
    TEST_ASSERT_EQ(targets[k].y, state->draws[k].y);
  }
}

/**
 * @brief Test 5 -- ``ra8_touch_cal_run`` paints 5 targets and recovers a
 *        matrix that round-trips the synthetic raw samples.
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */
static void test_run_full_sequence(void)
{
  const ra8_touch_cal_matrix_t truth = {
    .a = 0.25F,
    .b = 0.0F,
    .c = 0.0F,
    .d = 0.0F,
    .e = 0.20F,
    .f = 0.0F,
  };

  /* Compute the expected on-screen targets the utility will paint, in
   * the order it visits them: TL, TR, BR, BL, centre. */
  const int32_t               w          = (int32_t)k_tc_screen_w;
  const int32_t               h          = (int32_t)k_tc_screen_h;
  const int32_t               i          = (int32_t)k_tc_inset;
  const ra8_touch_cal_point_t targets[5] = {
    {i, i},
    {w - 1 - i, i},
    {w - 1 - i, h - 1 - i},
    {i, h - 1 - i},
    {w / 2, h / 2},
  };
  /* Synthesise the raw samples the user "would have produced" by
     inverting truth: raw = (screen - t) / scale. */
  ra8_touch_cal_point_t raws[k_t_cal_points] = {{0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}};
  tc_synth_raws(targets, &truth, raws);

  stub_state_t state = {
    .draws      = {{0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}},
    .n_draws    = 0U,
    .reads      = raws,
    .reads_idx  = 0U,
    .n_reads    = k_t_cal_points,
    .forced_err = k_ra8_ok,
  };
  const ra8_touch_cal_run_cfg_t cfg = {
    .screen_width  = (uint16_t)k_tc_screen_w,
    .screen_height = (uint16_t)k_tc_screen_h,
    .inset_px      = (uint16_t)k_tc_inset,
    .draw_target   = stub_draw,
    .draw_ctx      = &state,
    .read_raw      = stub_read,
    .read_ctx      = &state,
  };
  ra8_touch_cal_matrix_t got = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_touch_cal_run(&cfg, &got));
  TEST_ASSERT_EQ(5, state.n_draws);
  TEST_ASSERT_EQ(5, state.reads_idx);

  /* Verify the order of targets the utility visited. */
  tc_verify_draw_order(targets, &state);

  /* Recovered matrix must round-trip every raw sample to its target
     within a small pixel tolerance (see test_compute_five_point for
     the rationale -- least-squares + integer rounding). */
  tc_verify_roundtrip(raws, targets, &got);
}

/**
 * @brief Test 6 -- ``ra8_touch_cal_run`` propagates shim errors.
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */
static void test_run_shim_error(void)
{
  ra8_touch_cal_point_t dummy[k_t_cal_points] = {{0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}};
  stub_state_t          state                 = {
    .draws      = {{0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}},
    .n_draws    = 0U,
    .reads      = dummy,
    .reads_idx  = 0U,
    .n_reads    = k_t_cal_points,
    .forced_err = k_ra8_err_hw_error,
  };
  const ra8_touch_cal_run_cfg_t cfg = {
    .screen_width  = (uint16_t)k_tc_screen_w,
    .screen_height = (uint16_t)k_tc_screen_h,
    .inset_px      = (uint16_t)k_tc_inset,
    .draw_target   = stub_draw,
    .draw_ctx      = &state,
    .read_raw      = stub_read,
    .read_ctx      = &state,
  };
  ra8_touch_cal_matrix_t got = {};
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_touch_cal_run(&cfg, &got));

  /* NULL guards on run. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_touch_cal_run(nullptr, &got));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_touch_cal_run(&cfg, nullptr));

  /* Inset >= half panel rejected. */
  ra8_touch_cal_run_cfg_t bad = cfg;
  bad.inset_px                = (uint16_t)k_tc_screen_h;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_touch_cal_run(&bad, &got));
}

/**
 * @brief Exact-equality comparison of two calibration matrices.
 *
 * @param[in] lhs First matrix.
 * @param[in] rhs Second matrix.
 * @return true when all six coefficients compare equal.
 *
 * @pre @p lhs is non-null.
 * @pre @p rhs is non-null.
 * @post Neither operand is modified.
 */
static bool cal_matrix_equal(const ra8_touch_cal_matrix_t* lhs, const ra8_touch_cal_matrix_t* rhs)
{
  return (lhs->a == rhs->a) && (lhs->b == rhs->b) && (lhs->c == rhs->c) && (lhs->d == rhs->d) &&
         (lhs->e == rhs->e) && (lhs->f == rhs->f);
}

/**
 * @brief Test 7 -- save/load round-trip is bit-identical and CRC-checked.
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */
static void test_save_load_roundtrip(void)
{
  const ra8_touch_cal_matrix_t in = {
    .a = 0.25F,
    .b = 0.001F,
    .c = 5.5F,
    .d = -0.01F,
    .e = 0.19F,
    .f = -3.25F,
  };
  uint8_t blob[k_tc_blob] = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_touch_cal_save(&in, blob, sizeof(blob)));

  /* Magic + version sanity. */
  TEST_ASSERT_EQ('T', blob[0]);
  TEST_ASSERT_EQ('C', blob[1]);
  TEST_ASSERT_EQ('A', blob[2]);
  TEST_ASSERT_EQ('L', blob[3]);
  TEST_ASSERT_EQ(k_ra8_touch_cal_storage_version, blob[4]);

  ra8_touch_cal_matrix_t out = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_touch_cal_load(blob, sizeof(blob), &out));
  /* Compare the six coefficients rather than the object representation: the
   * struct is all float, so a memcmp would also compare any padding the ABI
   * inserts and would treat -0.0 as different from 0.0. */
  /* The pointer parameters below cannot be const: this mock implements a
 * function-pointer interface (the DI seam under test), so its signature is
 * fixed by the typedef it is assigned to -- adding const changes the
 * function type and the assignment stops compiling. */
  // NOLINTBEGIN(readability-non-const-parameter)
  TEST_ASSERT(cal_matrix_equal(&in, &out));

  /* Buffer too small. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_touch_cal_save(&in, blob, (size_t)k_tc_blob - 1U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_touch_cal_load(blob, (size_t)k_tc_blob - 1U, &out));

  /* NULL guard. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_touch_cal_save(nullptr, blob, sizeof(blob)));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_touch_cal_save(&in, nullptr, sizeof(blob)));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_touch_cal_load(nullptr, sizeof(blob), &out));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_touch_cal_load(blob, sizeof(blob), nullptr));
}

/**
 * @brief Test 8 -- corrupted blob bytes are rejected.
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */
static void test_load_corruption(void)
// NOLINTEND(readability-non-const-parameter)
{
  const ra8_touch_cal_matrix_t in = {
    .a = 1.0F,
    .b = 0.0F,
    .c = 0.0F,
    .d = 0.0F,
    .e = 1.0F,
    .f = 0.0F,
  };
  uint8_t blob[k_tc_blob] = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_touch_cal_save(&in, blob, sizeof(blob)));

  /* Magic byte tamper. */
  uint8_t bad_magic[k_tc_blob] = {};
  (void)memcpy(bad_magic, blob, sizeof(blob));
  bad_magic[0]               = 'X';
  ra8_touch_cal_matrix_t out = {};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_touch_cal_load(bad_magic, sizeof(bad_magic), &out));

  /* Version byte tamper. */
  uint8_t bad_ver[k_tc_blob] = {};
  (void)memcpy(bad_ver, blob, sizeof(blob));
  bad_ver[(size_t)k_ra8_touch_cal_off_version] = k_t_corrupt_mask;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_touch_cal_load(bad_ver, sizeof(bad_ver), &out));

  /* Reserved byte tamper. */
  uint8_t bad_res[k_tc_blob] = {};
  (void)memcpy(bad_res, blob, sizeof(blob));
  bad_res[(size_t)k_ra8_touch_cal_off_reserved] = k_t_bad_reserved;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_touch_cal_load(bad_res, sizeof(bad_res), &out));

  /* Coefficient byte tamper -> CRC mismatch. */
  uint8_t bad_coeff[k_tc_blob] = {};
  (void)memcpy(bad_coeff, blob, sizeof(blob));
  bad_coeff[(size_t)k_ra8_touch_cal_off_coeffs] ^= k_t_corrupt_mask;
  TEST_ASSERT_EQ(k_ra8_err_crc_mismatch, ra8_touch_cal_load(bad_coeff, sizeof(bad_coeff), &out));
}

/**
 * @test test_mcdc_load_magic_and_reserved_byte_pairs
 *
 * @par MC/DC:
 * Two short-circuit ORs in
 * `libs/ra8_touch_cal/src/ra8_touch_cal.c@ra8_touch_cal_load`:
 *   D_magic (line 568, 4-cond OR): magic bytes 0..3 differ from
 *     k_ra8_touch_cal_magic_b0..b3.
 *   D_reserved (line 577, 3-cond OR): reserved bytes 0..2 must all
 *     be zero.
 * Existing test_load_corruption supplies vectors for C1 only (magic[0]
 * tampered, reserved[0] tampered). This test adds the trailing-byte
 * tampers so each condition Cn flips with C1..Cn-1 held at their
 * masking value (F):
 *   D_magic V_C2: tamper magic[1]                   -> F,T,-,-      -> T
 *           V_C3: tamper magic[2]                   -> F,F,T,-      -> T
 *           V_C4: tamper magic[3]                   -> F,F,F,T      -> T
 *   D_reserved V_C2: tamper reserved[1]             -> F,T,-        -> T
 *              V_C3: tamper reserved[2]             -> F,F,T        -> T
 * Combined with the existing C1 vectors and the all-F pass-through
 * from test_save_load_roundtrip, every C-pair is closed (N+1 minimal
 * MC/DC for each OR).
 */
static void test_mcdc_load_magic_and_reserved_byte_pairs(void)
{
  TEST_BEGIN("touch_cal load MC/DC: magic + reserved per-byte independence");
  const ra8_touch_cal_matrix_t in = {
    .a = 1.0F,
    .b = 0.0F,
    .c = 0.0F,
    .d = 0.0F,
    .e = 1.0F,
    .f = 0.0F,
  };
  uint8_t blob[k_tc_blob] = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_touch_cal_save(&in, blob, sizeof(blob)));
  ra8_touch_cal_matrix_t out = {};
  /* D_magic per-byte flips. */
  for (size_t i = 1U; i < 4U; ++i) {
    uint8_t bad[k_tc_blob] = {};
    (void)memcpy(bad, blob, sizeof(bad));
    bad[(size_t)k_ra8_touch_cal_off_magic + i] ^= k_t_corrupt_mask;
    TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_touch_cal_load(bad, sizeof(bad), &out));
  }
  /* D_reserved per-byte flips. */
  for (size_t i = 1U; i < 3U; ++i) {
    uint8_t bad[k_tc_blob] = {};
    (void)memcpy(bad, blob, sizeof(bad));
    bad[(size_t)k_ra8_touch_cal_off_reserved + i] = k_t_bad_reserved;
    TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_touch_cal_load(bad, sizeof(bad), &out));
  }
  TEST_END("touch_cal load MC/DC: magic + reserved per-byte independence");
}

/**
 * @test test_compute_n_range_mcdc
 *
 * @par MC/DC:
 * Decision: `if ((n < (uint8_t)k_ra8_touch_cal_min_targets) ||
 *               (n > (uint8_t)k_ra8_touch_cal_max_targets))`
 * (2 conditions, `libs/ra8_touch_cal/src/ra8_touch_cal.c@ra8_touch_cal_compute`)
 * Standard: DO-178C Table A-7 obj 5; ISO 26262 Part 6 Table 12.
 * - Vector 1: n=2  -> C1=T (short-circuits) -> Decision T (invalid_arg)
 * - Vector 2: n=3  -> C1=F, C2=(3>5)=F -> Decision F (proceeds, returns
 *                     invalid_arg only if the synthetic system is singular)
 * - Vector 3: n=6  -> C1=F, C2=(6>5)=T -> Decision T (invalid_arg)
 * Vectors 1+2 vary C1 (decision flips); vectors 2+3 vary C2 with C1
 * held F (decision flips). N+1 = 3 vectors for N=2 conditions: minimal
 * MC/DC.
 */
static void test_compute_n_range_mcdc(void)
{
  TEST_BEGIN("touch_cal compute MC/DC: n < min || n > max");
  ra8_touch_cal_point_t  pts[6] = {{0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}};
  ra8_touch_cal_matrix_t m      = {};

  /* Vector 1: n=2 (below min=3). C1 short-circuits to T. Decision T. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_touch_cal_compute(pts, pts, 2U, &m));

  /* Vector 2: n=3 (in-range). C1=F, C2=F -> Decision F. The compute
   * proceeds; with all-zero samples the linear system is singular and
   * internal_solve3 reports invalid_arg. The MC/DC obligation here is
   * that C2 is observed F -- the post-decision return code is
   * incidental. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_touch_cal_compute(pts, pts, 3U, &m));

  /* Vector 3: n=6 (above max=5). C1=F, C2=T -> Decision T. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_touch_cal_compute(pts, pts, 6U, &m));
  TEST_END("touch_cal compute MC/DC: n < min || n > max");
}

/**
 * @test test_run_margin_mcdc
 *
 * @par MC/DC:
 * Decision: `if ((margin_total >= (uint32_t)cfg->screen_width) ||
 *               (margin_total >= (uint32_t)cfg->screen_height))`
 * (2 conditions, `libs/ra8_touch_cal/src/ra8_touch_cal.c@ra8_touch_cal_run`)
 * Standard: DO-178C Table A-7 obj 5; IEC 61508-3 SIL 3 Table A.5.
 * - Vector 1: w=100,h=100,inset=60 -> margin=120 >= 100 (C1=T,
 *             short-circuits) -> Decision T (invalid_arg)
 * - Vector 2: w=200,h=200,inset=10 -> margin=20 < 200 (C1=F),
 *             margin=20 < 200 (C2=F) -> Decision F (proceeds; returns
 *             hw_error from stub_read which has reads_idx >= n_reads
 *             on first iteration)
 * - Vector 3: w=200,h=100,inset=60 -> margin=120 < 200 (C1=F),
 *             margin=120 >= 100 (C2=T) -> Decision T (invalid_arg)
 * Vectors 1+2 vary C1; vectors 2+3 vary C2 with C1 held F.
 */
static void test_run_margin_mcdc(void)
{
  TEST_BEGIN("touch_cal run MC/DC: margin >= w || margin >= h");
  /* Stub state with empty read buffer so the post-decision code path
   * returns hw_error (proves the decision was F). */
  ra8_touch_cal_point_t  dummy[1] = {{0, 0}};
  stub_state_t           state    = {.draws      = {{0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}},
                                     .n_draws    = 0U,
                                     .reads      = dummy,
                                     .reads_idx  = 0U,
                                     .n_reads    = 0U,
                                     .forced_err = k_ra8_ok};
  ra8_touch_cal_matrix_t got      = {};

  /* Vector 1: C1=T short-circuits. */
  ra8_touch_cal_run_cfg_t cfg1 = {.screen_width  = k_t_screen_small,
                                  .screen_height = k_t_screen_small,
                                  .inset_px      = k_t_inset_wide,
                                  .draw_target   = stub_draw,
                                  .draw_ctx      = &state,
                                  .read_raw      = stub_read,
                                  .read_ctx      = &state};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_touch_cal_run(&cfg1, &got));

  /* Vector 2: C1=F, C2=F -> proceed; first read fails -> hw_error. */
  ra8_touch_cal_run_cfg_t cfg2 = cfg1;
  cfg2.screen_width            = k_t_screen_mid;
  cfg2.screen_height           = k_t_screen_mid;
  cfg2.inset_px                = k_t_inset_ok;
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_touch_cal_run(&cfg2, &got));

  /* Vector 3: C1=F, C2=T. */
  ra8_touch_cal_run_cfg_t cfg3 = cfg1;
  cfg3.screen_width            = k_t_screen_mid;
  cfg3.screen_height           = k_t_screen_small;
  cfg3.inset_px                = k_t_inset_wide;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_touch_cal_run(&cfg3, &got));
  TEST_END("touch_cal run MC/DC: margin >= w || margin >= h");
}

/**
 * @test test_mcdc_compute_null_or3
 *
 * @par MC/DC:
 * Decision: ``if ((raw == NULL) || (screen == NULL) || (out_mtx == NULL))``
 * (3 conditions, `libs/ra8_touch_cal/src/ra8_touch_cal.c@ra8_touch_cal_compute`).
 *
 * @par DO-178C 6.4.4.3 omission rationale:
 * Full short-circuit MC/DC for N=3 OR requires N+1 = 4 vectors. We use the
 * canonical short-circuit set; each predicate flips with the others held at
 * their masking value (F):
 * - V1: all non-NULL                       -> all F           -> dec F (proceeds)
 * - V2: raw=NULL                           -> C1=T short      -> dec T -> null_ptr
 * - V3: raw=ok, screen=NULL                -> C1=F, C2=T short -> dec T -> null_ptr
 * - V4: raw=ok, screen=ok, out_mtx=NULL    -> C1=F, C2=F, C3=T -> dec T -> null_ptr
 *
 * @note The remaining compound decision in this function,
 * `if (!ok_u || !ok_v)`, is MC/DC-DEACTIVATED at the source (see the
 * rationale comment above that decision). ok_u and ok_v are co-determined by
 * the determinant of the single shared 3x3 calibration matrix -- both solves
 * succeed or both fail together -- so the masking vectors (ok_u=F, ok_v=T)
 * and (ok_u=T, ok_v=F) are unreachable through any input, and per-condition
 * independent influence cannot be demonstrated. Both reachable decision
 * outcomes are still exercised: test_compute_bad_inputs' collinear dataset
 * drives ok_u=ok_v=F (decision T -> invalid_arg) and every well-conditioned
 * dataset drives ok_u=ok_v=T (decision F -> proceeds).
 */
static void test_mcdc_compute_null_or3(void)
{
  TEST_BEGIN("touch_cal compute MC/DC: raw||screen||out NULL");
  const ra8_touch_cal_point_t raw[3] = {
    {(int32_t)k_tc_raw_min, (int32_t)k_tc_raw_min},
    {(int32_t)k_tc_raw_max, (int32_t)k_tc_raw_min},
    {(int32_t)k_tc_raw_centre, (int32_t)k_tc_raw_max},
  };
  const ra8_touch_cal_point_t scr[3] = {{0, 0}, {1023, 0}, {512, 599}};
  ra8_touch_cal_matrix_t      got    = {};
  /* V1 */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_touch_cal_compute(raw, scr, 3U, &got));
  /* V2 */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_touch_cal_compute(nullptr, scr, 3U, &got));
  /* V3 */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_touch_cal_compute(raw, nullptr, 3U, &got));
  /* V4 */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_touch_cal_compute(raw, scr, 3U, nullptr));
  TEST_END("touch_cal compute MC/DC: raw||screen||out NULL");
}

/**
 * @test test_mcdc_run_cb_null_or
 *
 * @par MC/DC:
 * Decision: ``if ((cfg->draw_target == NULL) || (cfg->read_raw == NULL))``
 * (2 conditions, `libs/ra8_touch_cal/src/ra8_touch_cal.c@ra8_touch_cal_run`).
 * N+1 = 3 vectors.
 * - V1: draw=ok, read=ok   -> C1=F, C2=F -> dec F (proceeds)
 * - V2: draw=NULL          -> C1=T short -> dec T -> null_ptr
 * - V3: draw=ok, read=NULL -> C1=F, C2=T -> dec T -> null_ptr
 */
static void test_mcdc_run_cb_null_or(void)
{
  TEST_BEGIN("touch_cal run MC/DC: draw||read NULL");
  ra8_touch_cal_matrix_t        m  = {};
  const ra8_touch_cal_run_cfg_t v1 = {
    .screen_width  = (uint16_t)k_tc_screen_w,
    .screen_height = (uint16_t)k_tc_screen_h,
    .inset_px      = (uint16_t)k_tc_inset,
    .draw_target   = stub_draw,
    .read_raw      = stub_read,
  };
  /* V2: draw=NULL */
  ra8_touch_cal_run_cfg_t v2 = v1;
  v2.draw_target             = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_touch_cal_run(&v2, &m));
  /* V3: read=NULL */
  ra8_touch_cal_run_cfg_t v3 = v1;
  v3.read_raw                = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_touch_cal_run(&v3, &m));
  /* V1 verified by test_run_full_sequence (proceeds path), but we add a
   * lightweight non-null assertion here to make the masking pair explicit:
   * the very-bad-args path is rejected for a *different* reason
   * (zero screen size) -- proving that the cb null guard alone returns ok. */
  ra8_touch_cal_run_cfg_t v1_bad_screen = v1;
  v1_bad_screen.screen_width            = 0U;
  /* draw/read both ok -> C1=F, C2=F (decision F); rejection comes from the
   * subsequent screen-size guard returning invalid_arg, NOT null_ptr. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_touch_cal_run(&v1_bad_screen, &m));
  TEST_END("touch_cal run MC/DC: draw||read NULL");
}

/**
 * @test test_mcdc_run_cfg_out_null_or
 *
 * @par MC/DC:
 * Decision: ``if ((cfg == NULL) || (out_matrix == NULL))``
 * (2 conditions, `libs/ra8_touch_cal/src/ra8_touch_cal.c@ra8_touch_cal_run`).
 * N+1 = 3.
 * - V1: cfg=ok, out=ok -> C1=F, C2=F -> dec F (proceeds)
 * - V2: cfg=NULL       -> C1=T short -> dec T -> null_ptr
 * - V3: cfg=ok, out=NULL -> C1=F, C2=T -> dec T -> null_ptr
 */
static void test_mcdc_run_cfg_out_null_or(void)
{
  TEST_BEGIN("touch_cal run MC/DC: cfg||out NULL");
  ra8_touch_cal_matrix_t        m   = {};
  const ra8_touch_cal_run_cfg_t cfg = {
    .screen_width  = (uint16_t)k_tc_screen_w,
    .screen_height = (uint16_t)k_tc_screen_h,
    .inset_px      = (uint16_t)k_tc_inset,
    .draw_target   = stub_draw,
    .read_raw      = stub_read,
  };
  /* V2 */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_touch_cal_run(nullptr, &m));
  /* V3 */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_touch_cal_run(&cfg, nullptr));
  /* V1 (decision F) verified end-to-end by test_run_full_sequence. */
  TEST_END("touch_cal run MC/DC: cfg||out NULL");
}

/**
 * @test test_mcdc_apply_run_screen_dim_pair
 *
 * @par MC/DC:
 * Decision A (`libs/ra8_touch_cal/src/ra8_touch_cal.c@ra8_touch_cal_apply`):
 *   ``if ((screen_width == 0U) || (screen_height == 0U))``
 * 2-cond OR. test_apply_clip_and_null already supplies V1 (both >0) and
 * V2 (width=0). This adds V3 (height=0) so MC/DC = 100%.
 *
 * Decision B (`libs/ra8_touch_cal/src/ra8_touch_cal.c@ra8_touch_cal_run`):
 *   ``if ((cfg->screen_width == 0U) || (cfg->screen_height == 0U))``
 * Same shape; existing tests cover all-non-zero (V1) and width=0 (V2).
 * This adds V3 (height=0).
 *
 * - V1 (existing): w=100,h=100 -> dec F (proceed).
 * - V2 (existing): w=0,h=100   -> C1=T short -> dec T -> invalid_arg.
 * - V3 (new):      w=100,h=0   -> C1=F,C2=T -> dec T -> invalid_arg.
 * V1+V2 isolate C1; V1+V3 isolate C2.
 */
static void test_mcdc_apply_run_screen_dim_pair(void)
{
  TEST_BEGIN("touch_cal MC/DC: apply+run screen_height==0 (C2 of OR)");
  const ra8_touch_cal_matrix_t m = {
    .a = 1.0F,
    .b = 0.0F,
    .c = 0.0F,
    .d = 0.0F,
    .e = 1.0F,
    .f = 0.0F,
  };
  const ra8_touch_cal_point_t raw = {0, 0};
  ra8_touch_cal_point_t       out = {0, 0};

  /* Decision A V3: screen_height=0. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_touch_cal_apply(raw, &m, 100U, 0U, &out));

  /* Decision B V3: cfg->screen_height=0. */
  ra8_touch_cal_run_cfg_t cfg = {
    .draw_target   = (ra8_touch_cal_draw_target_fn_t)0x1U, /* non-null sentinels */
    .draw_ctx      = nullptr,
    .read_raw      = (ra8_touch_cal_read_raw_fn_t)0x1U,
    .read_ctx      = nullptr,
    .screen_width  = k_t_screen_wide,
    .screen_height = 0U,
    .inset_px      = 8U,
  };
  ra8_touch_cal_matrix_t mat;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_touch_cal_run(&cfg, &mat));
  TEST_END("touch_cal MC/DC: apply+run screen_height==0 (C2 of OR)");
}

/**
 * @brief Test driver.
 */
int main(void)
{
  test_compute_three_point();
  test_compute_five_point();
  test_compute_bad_inputs();
  test_apply_clip_and_null();
  test_run_full_sequence();
  test_run_shim_error();
  test_save_load_roundtrip();
  test_load_corruption();
  test_mcdc_load_magic_and_reserved_byte_pairs();
  test_compute_n_range_mcdc();
  test_run_margin_mcdc();
  test_mcdc_compute_null_or3();
  test_mcdc_run_cb_null_or();
  test_mcdc_run_cfg_out_null_or();
  test_mcdc_apply_run_screen_dim_pair();
  return 0;
}
