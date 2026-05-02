/**
 * @file test_ra_touch_cal.c
 * @brief Unit tests for the ra_touch_cal calibration utility
 *
 * @details
 * Drives the calibration utility entirely on the host -- no register
 * simulator needed. The tests exercise:
 *
 *   1. ``ra_touch_cal_compute`` against a synthetic linear ground-truth
 *      transform with known coefficients (3 and 5 sample variants).
 *   2. ``ra_touch_cal_apply`` round-trip on the recovered matrix.
 *   3. ``ra_touch_cal_run`` driven by stub LCD/touch shims.
 *   4. ``ra_touch_cal_save`` / ``ra_touch_cal_load`` byte-identical
 *      round-trip plus CRC corruption rejection.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra_err.h"
#include "ra_touch_cal.h"
#include "unity_minimal.h"

/**
 * @enum tc_test_const_t
 * @brief Test-only numeric constants.
 */
typedef enum : uint16_t {
  k_tc_screen_w = 1024U,
  k_tc_screen_h = 600U,
  k_tc_inset    = 32U,
  k_tc_blob     = 36U, /**< Mirrors ::k_ra_touch_cal_blob_size. */
} tc_test_const_t;

/**
 * @enum tc_raw_const_t
 * @brief Raw controller axis ranges used by the synthetic ground truth.
 */
typedef enum : uint16_t {
  k_tc_raw_min    = 100U,
  k_tc_raw_max    = 3900U,
  k_tc_raw_centre = 2000U,
  k_tc_raw_step   = 1000U,
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
  ra_touch_cal_point_t        draws[k_ra_touch_cal_n_targets];
  uint8_t                     n_draws;
  const ra_touch_cal_point_t* reads;
  uint8_t                     reads_idx;
  uint8_t                     n_reads;
  ra_err_t                    forced_err;
} stub_state_t;

/**
 * @brief Stub LCD shim -- records cross-hair coordinates.
 */
static ra_err_t stub_draw(void* ctx, ra_touch_cal_point_t target)
{
  stub_state_t* s = (stub_state_t*)ctx;
  if (s->forced_err != k_ra_ok) {
    return s->forced_err;
  }
  if (s->n_draws < (uint8_t)k_ra_touch_cal_n_targets) {
    s->draws[s->n_draws] = target;
    s->n_draws++;
  }
  return k_ra_ok;
}

/**
 * @brief Stub touch shim -- replays a fixed array of raw samples.
 */
static ra_err_t stub_read(void* ctx, ra_touch_cal_point_t* out_raw)
{
  stub_state_t* s = (stub_state_t*)ctx;
  if (s->reads_idx >= s->n_reads) {
    return k_ra_err_hw_error;
  }
  *out_raw = s->reads[s->reads_idx];
  s->reads_idx++;
  return k_ra_ok;
}

/**
 * @brief Map a raw point through a known ground-truth affine.
 */
static ra_touch_cal_point_t apply_truth(ra_touch_cal_point_t raw, const ra_touch_cal_matrix_t* m)
{
  const float          xf  = (float)raw.x;
  const float          yf  = (float)raw.y;
  const float          u   = (m->a * xf) + (m->b * yf) + m->c;
  const float          v   = (m->d * xf) + (m->e * yf) + m->f;
  ra_touch_cal_point_t out = {
    .x = (int32_t)(u + 0.5F),
    .y = (int32_t)(v + 0.5F),
  };
  return out;
}

/**
 * @brief Test 1 -- 3-point exact fit recovers the ground-truth matrix.
 */
static void test_compute_three_point(void)
{
  const ra_touch_cal_matrix_t truth = {
    .a = 0.25F,
    .b = 0.0F,
    .c = -25.0F,
    .d = 0.0F,
    .e = 0.20F,
    .f = -10.0F,
  };
  const ra_touch_cal_point_t raw[3] = {
    {(int32_t)k_tc_raw_min, (int32_t)k_tc_raw_min},
    {(int32_t)k_tc_raw_max, (int32_t)k_tc_raw_min},
    {(int32_t)k_tc_raw_centre, (int32_t)k_tc_raw_max},
  };
  ra_touch_cal_point_t scr[3] = {{0, 0}, {0, 0}, {0, 0}};
  for (uint8_t i = 0U; i < 3U; i++) {
    scr[i] = apply_truth(raw[i], &truth);
  }

  ra_touch_cal_matrix_t got = {};
  TEST_ASSERT_EQ(k_ra_ok, ra_touch_cal_compute(raw, scr, 3U, &got));

  /* Recovered coefficients should match within 1e-3. */
  TEST_ASSERT(((got.a - truth.a) < 1.0e-3F) && ((truth.a - got.a) < 1.0e-3F));
  TEST_ASSERT(((got.e - truth.e) < 1.0e-3F) && ((truth.e - got.e) < 1.0e-3F));
  TEST_ASSERT(((got.c - truth.c) < 1.0e-1F) && ((truth.c - got.c) < 1.0e-1F));
  TEST_ASSERT(((got.f - truth.f) < 1.0e-1F) && ((truth.f - got.f) < 1.0e-1F));
}

/**
 * @brief Test 2 -- 5-point least-squares fit recovers truth (no noise).
 */
static void test_compute_five_point(void)
{
  /* All five (raw -> screen) samples must remain inside the panel,
   * otherwise ra_touch_cal_apply's clip would silently reshape the
   * residual. The coefficients below produce on-panel u in [0, 1023]
   * and v in [0, 599] for every chosen raw sample. */
  const ra_touch_cal_matrix_t truth = {
    .a = 0.25F,
    .b = 0.005F,
    .c = 10.0F,
    .d = 0.005F,
    .e = 0.14F,
    .f = 20.0F,
  };
  const ra_touch_cal_point_t raw[5] = {
    {100, 100},
    {3800, 200},
    {3700, 3700},
    {300, 3650},
    {2000, 1800},
  };
  ra_touch_cal_point_t scr[5] = {{0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}};
  for (uint8_t i = 0U; i < 5U; i++) {
    scr[i] = apply_truth(raw[i], &truth);
  }

  ra_touch_cal_matrix_t got = {};
  TEST_ASSERT_EQ(k_ra_ok, ra_touch_cal_compute(raw, scr, 5U, &got));

  /* Round-trip: each raw sample must map back to within a small pixel
   * tolerance. Even with an exact-truth dataset, integer rounding of
   * scr at fit time produces a least-squares residual that scales with
   * the raw magnitude (~3800), so a few-pixel tolerance is correct. */
  for (uint8_t i = 0U; i < 5U; i++) {
    ra_touch_cal_point_t mapped = {0, 0};
    TEST_ASSERT_EQ(
      k_ra_ok,
      ra_touch_cal_apply(raw[i], &got, (uint16_t)k_tc_screen_w, (uint16_t)k_tc_screen_h, &mapped));
    const int32_t dx = mapped.x - scr[i].x;
    const int32_t dy = mapped.y - scr[i].y;
    TEST_ASSERT((dx <= 5) && (dx >= -5));
    TEST_ASSERT((dy <= 5) && (dy >= -5));
  }
}

/**
 * @brief Test 3 -- ``ra_touch_cal_compute`` rejects bad inputs.
 */
static void test_compute_bad_inputs(void)
{
  ra_touch_cal_point_t  pts[5] = {{0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}};
  ra_touch_cal_matrix_t m      = {};
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_touch_cal_compute(NULL, pts, 5U, &m));
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_touch_cal_compute(pts, NULL, 5U, &m));
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_touch_cal_compute(pts, pts, 5U, NULL));
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_touch_cal_compute(pts, pts, 2U, &m));
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_touch_cal_compute(pts, pts, 6U, &m));
  /* All-collinear samples -> singular system. */
  ra_touch_cal_point_t coll_raw[3] = {{0, 0}, {100, 100}, {200, 200}};
  ra_touch_cal_point_t coll_scr[3] = {{0, 0}, {10, 10}, {20, 20}};
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_touch_cal_compute(coll_raw, coll_scr, 3U, &m));
}

/**
 * @brief Test 4 -- ``ra_touch_cal_apply`` clips to panel and rejects NULL.
 */
static void test_apply_clip_and_null(void)
{
  ra_touch_cal_matrix_t m = {
    .a = 1.0F,
    .b = 0.0F,
    .c = 0.0F,
    .d = 0.0F,
    .e = 1.0F,
    .f = 0.0F,
  };
  ra_touch_cal_point_t out = {0, 0};

  /* NULL guard. */
  ra_touch_cal_point_t raw = {0, 0};
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_touch_cal_apply(raw, NULL, 100U, 100U, &out));
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_touch_cal_apply(raw, &m, 100U, 100U, NULL));
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_touch_cal_apply(raw, &m, 0U, 100U, &out));

  /* Identity transform: clip negative to 0, clip > w-1. */
  ra_touch_cal_point_t low  = {-50, -50};
  ra_touch_cal_point_t high = {9999, 9999};
  TEST_ASSERT_EQ(k_ra_ok, ra_touch_cal_apply(low, &m, 100U, 100U, &out));
  TEST_ASSERT_EQ(0, out.x);
  TEST_ASSERT_EQ(0, out.y);
  TEST_ASSERT_EQ(k_ra_ok, ra_touch_cal_apply(high, &m, 100U, 100U, &out));
  TEST_ASSERT_EQ(99, out.x);
  TEST_ASSERT_EQ(99, out.y);
}

/**
 * @brief Test 5 -- ``ra_touch_cal_run`` paints 5 targets and recovers a
 *        matrix that round-trips the synthetic raw samples.
 */
static void test_run_full_sequence(void)
{
  const ra_touch_cal_matrix_t truth = {
    .a = 0.25F,
    .b = 0.0F,
    .c = 0.0F,
    .d = 0.0F,
    .e = 0.20F,
    .f = 0.0F,
  };

  /* Compute the expected on-screen targets the utility will paint, in
   * the order it visits them: TL, TR, BR, BL, centre. */
  const int32_t              w          = (int32_t)k_tc_screen_w;
  const int32_t              h          = (int32_t)k_tc_screen_h;
  const int32_t              i          = (int32_t)k_tc_inset;
  const ra_touch_cal_point_t targets[5] = {
    {i, i},
    {w - 1 - i, i},
    {w - 1 - i, h - 1 - i},
    {i, h - 1 - i},
    {w / 2, h / 2},
  };
  /* Synthesise the raw samples the user "would have produced" by
   * inverting truth: raw = (screen - t) / scale. */
  ra_touch_cal_point_t raws[5] = {{0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}};
  for (uint8_t k = 0U; k < 5U; k++) {
    raws[k].x = (int32_t)((float)targets[k].x / truth.a);
    raws[k].y = (int32_t)((float)targets[k].y / truth.e);
  }

  stub_state_t state = {
    .draws      = {{0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}},
    .n_draws    = 0U,
    .reads      = raws,
    .reads_idx  = 0U,
    .n_reads    = 5U,
    .forced_err = k_ra_ok,
  };
  const ra_touch_cal_run_cfg_t cfg = {
    .screen_width  = (uint16_t)k_tc_screen_w,
    .screen_height = (uint16_t)k_tc_screen_h,
    .inset_px      = (uint16_t)k_tc_inset,
    .draw_target   = stub_draw,
    .draw_ctx      = &state,
    .read_raw      = stub_read,
    .read_ctx      = &state,
  };
  ra_touch_cal_matrix_t got = {};
  TEST_ASSERT_EQ(k_ra_ok, ra_touch_cal_run(&cfg, &got));
  TEST_ASSERT_EQ(5, state.n_draws);
  TEST_ASSERT_EQ(5, state.reads_idx);

  /* Verify the order of targets the utility visited. */
  for (uint8_t k = 0U; k < 5U; k++) {
    TEST_ASSERT_EQ(targets[k].x, state.draws[k].x);
    TEST_ASSERT_EQ(targets[k].y, state.draws[k].y);
  }

  /* Recovered matrix must round-trip every raw sample to its target
   * within a small pixel tolerance (see test_compute_five_point for
   * the rationale -- least-squares + integer rounding). */
  for (uint8_t k = 0U; k < 5U; k++) {
    ra_touch_cal_point_t mapped = {0, 0};
    TEST_ASSERT_EQ(
      k_ra_ok,
      ra_touch_cal_apply(raws[k], &got, (uint16_t)k_tc_screen_w, (uint16_t)k_tc_screen_h, &mapped));
    const int32_t dx = mapped.x - targets[k].x;
    const int32_t dy = mapped.y - targets[k].y;
    TEST_ASSERT((dx <= 5) && (dx >= -5));
    TEST_ASSERT((dy <= 5) && (dy >= -5));
  }
}

/**
 * @brief Test 6 -- ``ra_touch_cal_run`` propagates shim errors.
 */
static void test_run_shim_error(void)
{
  ra_touch_cal_point_t dummy[5] = {{0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}};
  stub_state_t         state    = {
    .draws      = {{0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}},
    .n_draws    = 0U,
    .reads      = dummy,
    .reads_idx  = 0U,
    .n_reads    = 5U,
    .forced_err = k_ra_err_hw_error,
  };
  const ra_touch_cal_run_cfg_t cfg = {
    .screen_width  = (uint16_t)k_tc_screen_w,
    .screen_height = (uint16_t)k_tc_screen_h,
    .inset_px      = (uint16_t)k_tc_inset,
    .draw_target   = stub_draw,
    .draw_ctx      = &state,
    .read_raw      = stub_read,
    .read_ctx      = &state,
  };
  ra_touch_cal_matrix_t got = {};
  TEST_ASSERT_EQ(k_ra_err_hw_error, ra_touch_cal_run(&cfg, &got));

  /* NULL guards on run. */
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_touch_cal_run(NULL, &got));
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_touch_cal_run(&cfg, NULL));

  /* Inset >= half panel rejected. */
  ra_touch_cal_run_cfg_t bad = cfg;
  bad.inset_px               = (uint16_t)(k_tc_screen_h);
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_touch_cal_run(&bad, &got));
}

/**
 * @brief Test 7 -- save/load round-trip is bit-identical and CRC-checked.
 */
static void test_save_load_roundtrip(void)
{
  const ra_touch_cal_matrix_t in = {
    .a = 0.25F,
    .b = 0.001F,
    .c = 5.5F,
    .d = -0.01F,
    .e = 0.19F,
    .f = -3.25F,
  };
  uint8_t blob[k_tc_blob] = {};
  TEST_ASSERT_EQ(k_ra_ok, ra_touch_cal_save(&in, blob, sizeof(blob)));

  /* Magic + version sanity. */
  TEST_ASSERT_EQ('T', blob[0]);
  TEST_ASSERT_EQ('C', blob[1]);
  TEST_ASSERT_EQ('A', blob[2]);
  TEST_ASSERT_EQ('L', blob[3]);
  TEST_ASSERT_EQ((int)k_ra_touch_cal_storage_version, blob[4]);

  ra_touch_cal_matrix_t out = {};
  TEST_ASSERT_EQ(k_ra_ok, ra_touch_cal_load(blob, sizeof(blob), &out));
  TEST_ASSERT(memcmp(&in, &out, sizeof(in)) == 0);

  /* Buffer too small. */
  TEST_ASSERT_EQ(k_ra_err_invalid_size, ra_touch_cal_save(&in, blob, (size_t)k_tc_blob - 1U));
  TEST_ASSERT_EQ(k_ra_err_invalid_size, ra_touch_cal_load(blob, (size_t)k_tc_blob - 1U, &out));

  /* NULL guard. */
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_touch_cal_save(NULL, blob, sizeof(blob)));
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_touch_cal_save(&in, NULL, sizeof(blob)));
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_touch_cal_load(NULL, sizeof(blob), &out));
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_touch_cal_load(blob, sizeof(blob), NULL));
}

/**
 * @brief Test 8 -- corrupted blob bytes are rejected.
 */
static void test_load_corruption(void)
{
  const ra_touch_cal_matrix_t in = {
    .a = 1.0F,
    .b = 0.0F,
    .c = 0.0F,
    .d = 0.0F,
    .e = 1.0F,
    .f = 0.0F,
  };
  uint8_t blob[k_tc_blob] = {};
  TEST_ASSERT_EQ(k_ra_ok, ra_touch_cal_save(&in, blob, sizeof(blob)));

  /* Magic byte tamper. */
  uint8_t bad_magic[k_tc_blob] = {};
  (void)memcpy(bad_magic, blob, sizeof(blob));
  bad_magic[0]              = 'X';
  ra_touch_cal_matrix_t out = {};
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_touch_cal_load(bad_magic, sizeof(bad_magic), &out));

  /* Version byte tamper. */
  uint8_t bad_ver[k_tc_blob] = {};
  (void)memcpy(bad_ver, blob, sizeof(blob));
  bad_ver[(size_t)k_ra_touch_cal_off_version] = 0xFFU;
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_touch_cal_load(bad_ver, sizeof(bad_ver), &out));

  /* Reserved byte tamper. */
  uint8_t bad_res[k_tc_blob] = {};
  (void)memcpy(bad_res, blob, sizeof(blob));
  bad_res[(size_t)k_ra_touch_cal_off_reserved] = 0x42U;
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_touch_cal_load(bad_res, sizeof(bad_res), &out));

  /* Coefficient byte tamper -> CRC mismatch. */
  uint8_t bad_coeff[k_tc_blob] = {};
  (void)memcpy(bad_coeff, blob, sizeof(blob));
  bad_coeff[(size_t)k_ra_touch_cal_off_coeffs] ^= 0xFFU;
  TEST_ASSERT_EQ(k_ra_err_crc_mismatch, ra_touch_cal_load(bad_coeff, sizeof(bad_coeff), &out));
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
  return 0;
}
