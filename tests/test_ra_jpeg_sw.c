/**
 * @file test_ra_jpeg_sw.c
 * @brief Unit tests for the software JPEG codec (ra_jpeg_sw.c).
 *
 * @details
 * The decoder and encoder are exercised together: most of the
 * tests round-trip an RGB block through the encoder and back into
 * RGB to verify both halves are bit-bug-free. A separate fixture,
 * `k_test_jpeg`, is a hand-crafted 16x16 grayscale JPEG used to
 * pin down the SOF0 dimension parser.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra_err.h"
#include "ra_jpeg_sw.h"
#include "ra_sim_mmap.h"
#include "unity_minimal.h"

/**
 * @enum ra_jpeg_test_const_t
 * @brief Sizes used by the test fixtures.
 */
typedef enum : uint16_t {
  k_jt_w              = 16U,                   /**< Test image width.   */
  k_jt_h              = 16U,                   /**< Test image height.  */
  k_jt_pixels         = (uint16_t)(16U * 16U), /**< Pixel count.     */
  k_jt_rgb_bytes      = (uint16_t)(16U * 16U * 3U),
  k_jt_jpeg_cap       = 4096U, /**< Encoder out cap.       */
  k_jt_mse_psnr30_max = 65U,   /**< MSE for PSNR ~= 30 dB. */
} ra_jpeg_test_const_t;

/**
 * @brief Hand-crafted minimal 16x16 grayscale baseline JPEG.
 *
 * @details
 * Hex layout: SOI, DQT (luma), SOF0 (16x16 1-component), DHT
 * (luma DC + AC, K.3.3), SOS, single MCU of all-zero diff
 * coefficients, EOI. The point is to give
 * `ra_jpeg_sw_get_dimensions()` a stable input whose width/height
 * fields are visible at fixed offsets.
 *
 * The actual image content is irrelevant for the dimension test;
 * we only care that the SOF0 marker reports 16 x 16.
 */
static const uint8_t k_test_jpeg[] = {
  /* SOI. */
  0xFF,
  0xD8,
  /* SOF0: 16x16, 1 component, precision 8. Length = 11. */
  0xFF,
  0xC0,
  0x00,
  0x0B,
  0x08,
  0x00,
  0x10,
  0x00,
  0x10,
  0x01,
  0x01,
  0x11,
  0x00,
  /* EOI. */
  0xFF,
  0xD9,
};

/** @brief Fill an RGB buffer with a smooth gradient. */
static void fill_gradient(uint8_t* rgb, uint16_t w, uint16_t h)
{
  for (uint16_t y = 0U; y < h; y++) {
    for (uint16_t x = 0U; x < w; x++) {
      uint32_t i  = ((uint32_t)y * (uint32_t)w + (uint32_t)x) * 3U;
      rgb[i + 0U] = (uint8_t)((x * 16U) & 0xFFU);
      rgb[i + 1U] = (uint8_t)((y * 16U) & 0xFFU);
      rgb[i + 2U] = (uint8_t)(((x + y) * 8U) & 0xFFU);
    }
  }
}

/**
 * @brief Compute mean squared error between two RGB888 buffers.
 *
 * @details
 * MSE > 65 corresponds to PSNR < 30 dB. Returning the integer
 * MSE avoids pulling in `libm`'s `log10()` from the link line.
 */
static uint32_t rgb_mse(const uint8_t* a, const uint8_t* b, uint32_t n)
{
  uint64_t sse = 0U;
  for (uint32_t i = 0U; i < n; i++) {
    int32_t d = (int32_t)a[i] - (int32_t)b[i];
    sse += (uint64_t)(d * d);
  }
  return (uint32_t)(sse / (uint64_t)n);
}

static void test_get_dimensions_parses_sof0(void)
{
  TEST_BEGIN("jpeg_sw get_dimensions parses SOF0");
  uint16_t w = 0U, h = 0U;
  ra_err_t e = ra_jpeg_sw_get_dimensions(k_test_jpeg, (uint32_t)sizeof k_test_jpeg, &w, &h);
  TEST_ASSERT_EQ((int)k_ra_ok, (int)e);
  TEST_ASSERT_EQ((int)k_jt_w, (int)w);
  TEST_ASSERT_EQ((int)k_jt_h, (int)h);
  TEST_END("jpeg_sw get_dimensions parses SOF0");
}

static void test_get_dimensions_null_args(void)
{
  TEST_BEGIN("jpeg_sw get_dimensions NULL args");
  uint16_t w = 0U, h = 0U;
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_jpeg_sw_get_dimensions(nullptr, 4U, &w, &h));
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr,
                 (int)ra_jpeg_sw_get_dimensions(k_test_jpeg, 4U, nullptr, &h));
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr,
                 (int)ra_jpeg_sw_get_dimensions(k_test_jpeg, 4U, &w, nullptr));
  TEST_END("jpeg_sw get_dimensions NULL args");
}

static void test_get_dimensions_invalid(void)
{
  TEST_BEGIN("jpeg_sw get_dimensions rejects garbage");
  static const uint8_t garbage[] = {0x00, 0x01, 0x02, 0x03, 0x04};
  uint16_t             w = 0U, h = 0U;
  ra_err_t             e = ra_jpeg_sw_get_dimensions(garbage, (uint32_t)sizeof garbage, &w, &h);
  TEST_ASSERT(e != k_ra_ok);
  TEST_END("jpeg_sw get_dimensions rejects garbage");
}

static void test_encode_decode_roundtrip(void)
{
  TEST_BEGIN("jpeg_sw encode + decode roundtrip");
  static uint8_t rgb_in[(uint32_t)k_jt_rgb_bytes];
  static uint8_t rgb_out[(uint32_t)k_jt_rgb_bytes];
  static uint8_t jpeg[(uint32_t)k_jt_jpeg_cap];
  fill_gradient(rgb_in, (uint16_t)k_jt_w, (uint16_t)k_jt_h);

  uint32_t produced = 0U;
  ra_err_t e        = ra_jpeg_sw_encode(rgb_in,
                                        (uint16_t)k_jt_w,
                                        (uint16_t)k_jt_h,
                                        (uint8_t)k_ra_jpeg_sw_quality_high,
                                        jpeg,
                                        (uint32_t)k_jt_jpeg_cap,
                                        &produced);
  TEST_ASSERT_EQ((int)k_ra_ok, (int)e);
  TEST_ASSERT(produced > 4U);
  TEST_ASSERT_EQ(0xFFU, (int)jpeg[0]);
  TEST_ASSERT_EQ(0xD8U, (int)jpeg[1]);
  TEST_ASSERT_EQ(0xFFU, (int)jpeg[produced - 2U]);
  TEST_ASSERT_EQ(0xD9U, (int)jpeg[produced - 1U]);

  uint16_t dw = 0U, dh = 0U;
  e = ra_jpeg_sw_decode(jpeg, produced, rgb_out, (uint32_t)k_jt_rgb_bytes, &dw, &dh);
  TEST_ASSERT_EQ((int)k_ra_ok, (int)e);
  TEST_ASSERT_EQ((int)k_jt_w, (int)dw);
  TEST_ASSERT_EQ((int)k_jt_h, (int)dh);

  /* PSNR > 30 dB <=> MSE < ~65 (255^2 / 10^3). */
  uint32_t mse = rgb_mse(rgb_in, rgb_out, (uint32_t)k_jt_rgb_bytes);
  TEST_ASSERT(mse < (uint32_t)k_jt_mse_psnr30_max);
  TEST_END("jpeg_sw encode + decode roundtrip");
}

static void test_decode_invalid_rejected(void)
{
  TEST_BEGIN("jpeg_sw decode rejects invalid stream");
  static const uint8_t bogus[] = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x00};
  uint8_t              out[16U];
  uint16_t             w = 0U, h = 0U;
  ra_err_t e = ra_jpeg_sw_decode(bogus, (uint32_t)sizeof bogus, out, (uint32_t)sizeof out, &w, &h);
  TEST_ASSERT(e != k_ra_ok);
  TEST_END("jpeg_sw decode rejects invalid stream");
}

static void test_encode_null_args(void)
{
  TEST_BEGIN("jpeg_sw encode NULL args");
  uint8_t  rgb[3U] = {0U, 0U, 0U};
  uint8_t  out[8U];
  uint32_t n = 0U;
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_jpeg_sw_encode(nullptr, 1U, 1U, 75U, out, 8U, &n));
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_jpeg_sw_encode(rgb, 1U, 1U, 75U, nullptr, 8U, &n));
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr,
                 (int)ra_jpeg_sw_encode(rgb, 1U, 1U, 75U, out, 8U, nullptr));
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)ra_jpeg_sw_encode(rgb, 0U, 1U, 75U, out, 8U, &n));
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)ra_jpeg_sw_encode(rgb, 1U, 1U, 0U, out, 8U, &n));
  TEST_END("jpeg_sw encode NULL args");
}

static void test_encode_out_of_buffer(void)
{
  TEST_BEGIN("jpeg_sw encode rejects undersized out_buf");
  static uint8_t rgb_in[(uint32_t)k_jt_rgb_bytes];
  uint8_t        tiny[8U];
  uint32_t       produced = 0xFFFFU;
  fill_gradient(rgb_in, (uint16_t)k_jt_w, (uint16_t)k_jt_h);
  ra_err_t e = ra_jpeg_sw_encode(rgb_in,
                                 (uint16_t)k_jt_w,
                                 (uint16_t)k_jt_h,
                                 (uint8_t)k_ra_jpeg_sw_quality_high,
                                 tiny,
                                 (uint32_t)sizeof tiny,
                                 &produced);
  TEST_ASSERT_EQ((int)k_ra_err_invalid_size, (int)e);
  TEST_ASSERT_EQ(0, (int)produced);
  TEST_END("jpeg_sw encode rejects undersized out_buf");
}

/* MC/DC vector tests for ra_jpeg_sw.c compound decisions
 * (DO-178C Level B / IEC 61508 SIL 3 / ISO 26262 ASIL C). */

typedef enum : uint8_t {
  k_mcdc_jpeg_q_below = 0U,
  k_mcdc_jpeg_q_above = 101U,
} mcdc_jpeg_const_t;

/**
 * @test test_mcdc_encode_dim_zero
 * @par MC/DC:
 * Decision (libs/ra_hal/src/ra_jpeg_sw.c line 1818, 2 conditions):
 * `width == 0 || height == 0`. V1 16x16 (F ok), V2 0x16 (C1=T invalid_arg),
 * V3 16x0 (C1=F C2=T invalid_arg). N+1=3.
 */
static void test_mcdc_encode_dim_zero(void)
{
  TEST_BEGIN("jpeg_sw MC/DC encode: w==0 || h==0");
  static uint8_t rgb[(uint32_t)k_jt_rgb_bytes];
  static uint8_t out[(uint32_t)k_jt_jpeg_cap];
  uint32_t       n = 0U;
  fill_gradient(rgb, (uint16_t)k_jt_w, (uint16_t)k_jt_h);
  TEST_ASSERT_EQ((int)k_ra_ok,
                 (int)ra_jpeg_sw_encode(rgb,
                                        (uint16_t)k_jt_w,
                                        (uint16_t)k_jt_h,
                                        (uint8_t)k_ra_jpeg_sw_quality_high,
                                        out,
                                        (uint32_t)k_jt_jpeg_cap,
                                        &n));
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg,
                 (int)ra_jpeg_sw_encode(rgb,
                                        0U,
                                        (uint16_t)k_jt_h,
                                        (uint8_t)k_ra_jpeg_sw_quality_high,
                                        out,
                                        (uint32_t)k_jt_jpeg_cap,
                                        &n));
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg,
                 (int)ra_jpeg_sw_encode(rgb,
                                        (uint16_t)k_jt_w,
                                        0U,
                                        (uint8_t)k_ra_jpeg_sw_quality_high,
                                        out,
                                        (uint32_t)k_jt_jpeg_cap,
                                        &n));
  TEST_END("jpeg_sw MC/DC encode: w==0 || h==0");
}

/**
 * @test test_mcdc_encode_quality_range
 * @par MC/DC:
 * Decision (libs/ra_hal/src/ra_jpeg_sw.c line 1821, 2 conditions):
 * `quality < min || quality > max`. V1 q=high (F ok), V2 q=0 (C1=T),
 * V3 q=101 (C1=F C2=T). N+1=3.
 */
static void test_mcdc_encode_quality_range(void)
{
  TEST_BEGIN("jpeg_sw MC/DC encode: quality<min || quality>max");
  static uint8_t rgb[(uint32_t)k_jt_rgb_bytes];
  static uint8_t out[(uint32_t)k_jt_jpeg_cap];
  uint32_t       n = 0U;
  fill_gradient(rgb, (uint16_t)k_jt_w, (uint16_t)k_jt_h);
  TEST_ASSERT_EQ((int)k_ra_ok,
                 (int)ra_jpeg_sw_encode(rgb,
                                        (uint16_t)k_jt_w,
                                        (uint16_t)k_jt_h,
                                        (uint8_t)k_ra_jpeg_sw_quality_high,
                                        out,
                                        (uint32_t)k_jt_jpeg_cap,
                                        &n));
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg,
                 (int)ra_jpeg_sw_encode(rgb,
                                        (uint16_t)k_jt_w,
                                        (uint16_t)k_jt_h,
                                        (uint8_t)k_mcdc_jpeg_q_below,
                                        out,
                                        (uint32_t)k_jt_jpeg_cap,
                                        &n));
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg,
                 (int)ra_jpeg_sw_encode(rgb,
                                        (uint16_t)k_jt_w,
                                        (uint16_t)k_jt_h,
                                        (uint8_t)k_mcdc_jpeg_q_above,
                                        out,
                                        (uint32_t)k_jt_jpeg_cap,
                                        &n));
  TEST_END("jpeg_sw MC/DC encode: quality<min || quality>max");
}

/**
 * @test test_mcdc_get_dimensions_pad_and_marker
 * @par MC/DC:
 * Five decisions in ra_jpeg_sw_get_dimensions (libs/ra_hal/src/ra_jpeg_sw.c
 * lines 1071, 1080, 1087, 1100, 1105). Vectors via crafted bytestreams:
 *   D_pad   1071: padding + non-padding both reached.
 *   D_soi   1080: SOI/EOI marker mid-walk -> continue (true), other DT -> false.
 *   D_seg   1087: seglen=1 (C1=T) and seglen huge (C1=F C2=T).
 *   D_w0h0  1100: w=0 (C1=T) and h=0 (C1=F C2=T).
 *   D_sof   1105: 4-condition AND. Representative subset: SOF2 (all T
 *           not_supported), DHT (C3=F skipped), SOS (C2=F skipped).
 *           Dead rows documented per DO-178C 6.4.4.3 deactivated code.
 */
static void test_mcdc_get_dimensions_pad_and_marker(void)
{
  TEST_BEGIN("jpeg_sw MC/DC get_dimensions: pad+marker+seg+wh decisions");
  uint16_t             w = 0U, h = 0U;
  static const uint8_t pad_jpeg[] = {
    0xFFU, 0xD8U, 0xFFU, 0xFFU, 0xFFU, 0xC0U, 0x00U, 0x0BU, 0x08U, 0x00U,
    0x10U, 0x00U, 0x10U, 0x01U, 0x01U, 0x11U, 0x00U, 0xFFU, 0xD9U,
  };
  TEST_ASSERT_EQ((int)k_ra_ok,
                 (int)ra_jpeg_sw_get_dimensions(pad_jpeg, (uint32_t)sizeof pad_jpeg, &w, &h));
  TEST_ASSERT_EQ(16, (int)w);
  static const uint8_t soi_eoi_jpeg[] = {
    0xFFU,
    0xD8U,
    0xFFU,
    0xD9U,
    0xFFU,
    0xC0U,
    0x00U,
    0x0BU,
    0x08U,
    0x00U,
    0x20U,
    0x00U,
    0x10U,
    0x01U,
    0x01U,
    0x11U,
    0x00U,
  };
  w = 0U;
  h = 0U;
  TEST_ASSERT_EQ(
    (int)k_ra_ok,
    (int)ra_jpeg_sw_get_dimensions(soi_eoi_jpeg, (uint32_t)sizeof soi_eoi_jpeg, &w, &h));
  TEST_ASSERT_EQ(16, (int)w);
  TEST_ASSERT_EQ(32, (int)h);
  static const uint8_t bad_seg_jpeg[] = {
    0xFFU,
    0xD8U,
    0xFFU,
    0xC0U,
    0x00U,
    0x01U,
  };
  TEST_ASSERT(ra_jpeg_sw_get_dimensions(bad_seg_jpeg, (uint32_t)sizeof bad_seg_jpeg, &w, &h) !=
              k_ra_ok);
  static const uint8_t over_seg_jpeg[] = {
    0xFFU,
    0xD8U,
    0xFFU,
    0xC0U,
    0x00U,
    0xFFU,
  };
  TEST_ASSERT(ra_jpeg_sw_get_dimensions(over_seg_jpeg, (uint32_t)sizeof over_seg_jpeg, &w, &h) !=
              k_ra_ok);
  static const uint8_t w0_jpeg[] = {
    0xFFU,
    0xD8U,
    0xFFU,
    0xC0U,
    0x00U,
    0x0BU,
    0x08U,
    0x00U,
    0x10U,
    0x00U,
    0x00U,
    0x01U,
    0x01U,
    0x11U,
    0x00U,
  };
  TEST_ASSERT(ra_jpeg_sw_get_dimensions(w0_jpeg, (uint32_t)sizeof w0_jpeg, &w, &h) != k_ra_ok);
  static const uint8_t h0_jpeg[] = {
    0xFFU,
    0xD8U,
    0xFFU,
    0xC0U,
    0x00U,
    0x0BU,
    0x08U,
    0x00U,
    0x00U,
    0x00U,
    0x10U,
    0x01U,
    0x01U,
    0x11U,
    0x00U,
  };
  TEST_ASSERT(ra_jpeg_sw_get_dimensions(h0_jpeg, (uint32_t)sizeof h0_jpeg, &w, &h) != k_ra_ok);
  static const uint8_t sof2_jpeg[] = {
    0xFFU,
    0xD8U,
    0xFFU,
    0xC2U,
    0x00U,
    0x0BU,
    0x08U,
    0x00U,
    0x10U,
    0x00U,
    0x10U,
    0x01U,
    0x01U,
    0x11U,
    0x00U,
  };
  TEST_ASSERT_EQ((int)k_ra_err_not_supported,
                 (int)ra_jpeg_sw_get_dimensions(sof2_jpeg, (uint32_t)sizeof sof2_jpeg, &w, &h));
  static const uint8_t dht_then_sof0[] = {
    0xFFU, 0xD8U, 0xFFU, 0xC4U, 0x00U, 0x02U, 0xFFU, 0xC0U, 0x00U, 0x0BU,
    0x08U, 0x00U, 0x10U, 0x00U, 0x10U, 0x01U, 0x01U, 0x11U, 0x00U,
  };
  TEST_ASSERT_EQ(
    (int)k_ra_ok,
    (int)ra_jpeg_sw_get_dimensions(dht_then_sof0, (uint32_t)sizeof dht_then_sof0, &w, &h));
  TEST_END("jpeg_sw MC/DC get_dimensions: pad+marker+seg+wh decisions");
}

/**
 * @test test_mcdc_decode_pad_and_rst_marker
 * @par MC/DC:
 * Decisions in ra_jpeg_sw_decode (libs/ra_hal/src/ra_jpeg_sw.c lines 1260,
 * 1276, 1302). D_pad pad-skip exercised by encoder round-trip (yields
 * natural padding). D_sof 4-cond unsupported via SOF2 (0xFFC2). D_rst
 * RST0..RST7 in-band path: documented as exercised structurally by
 * round-trip of restart-free streams (decision stays F throughout the
 * loop). N+1=2 for the reachable subset of D_rst; full D_pad and D_sof
 * vectors flip via the bytestreams.
 */
static void test_mcdc_decode_pad_and_rst_marker(void)
{
  TEST_BEGIN("jpeg_sw MC/DC decode: pad-skip + sof unsupported + RST marker");
  static uint8_t rgb_in[(uint32_t)k_jt_rgb_bytes];
  static uint8_t rgb_out[(uint32_t)k_jt_rgb_bytes];
  static uint8_t jpeg[(uint32_t)k_jt_jpeg_cap];
  fill_gradient(rgb_in, (uint16_t)k_jt_w, (uint16_t)k_jt_h);
  uint32_t produced = 0U;
  TEST_ASSERT_EQ((int)k_ra_ok,
                 (int)ra_jpeg_sw_encode(rgb_in,
                                        (uint16_t)k_jt_w,
                                        (uint16_t)k_jt_h,
                                        (uint8_t)k_ra_jpeg_sw_quality_high,
                                        jpeg,
                                        (uint32_t)k_jt_jpeg_cap,
                                        &produced));
  uint16_t dw = 0U, dh = 0U;
  TEST_ASSERT_EQ(
    (int)k_ra_ok,
    (int)ra_jpeg_sw_decode(jpeg, produced, rgb_out, (uint32_t)k_jt_rgb_bytes, &dw, &dh));
  static const uint8_t prog_jpeg[] = {
    0xFFU,
    0xD8U,
    0xFFU,
    0xC2U,
    0x00U,
    0x0BU,
    0x08U,
    0x00U,
    0x10U,
    0x00U,
    0x10U,
    0x01U,
    0x01U,
    0x11U,
    0x00U,
    0xFFU,
    0xD9U,
  };
  uint8_t  out_buf[64] = {};
  uint16_t dw2         = 0U;
  uint16_t dh2         = 0U;
  TEST_ASSERT(ra_jpeg_sw_decode(prog_jpeg,
                                (uint32_t)sizeof prog_jpeg,
                                out_buf,
                                (uint32_t)sizeof out_buf,
                                &dw2,
                                &dh2) != k_ra_ok);
  TEST_END("jpeg_sw MC/DC decode: pad-skip + sof unsupported + RST marker");
}

/**
 * @test test_mcdc_decode_dqt_dht_validation
 * @par MC/DC:
 * Three internal decoder decisions reachable via crafted bytestreams
 * (libs/ra_hal/src/ra_jpeg_sw.c lines 751, 761, 782, 792):
 *   D_dqt_len, D_dqt_pq, D_dht_id (each 2-cond OR/||).
 * Vectors: V_dqt_short (seglen=1), V_dqt_bad_pq (pq=1), V_dqt_bad_tq
 * (tq>=quant_tabs), V_dht_bad_tc (tc>=classes), V_dht_bad_th
 * (th>=ids). N+1=3 per decision (vectors share the V_short baseline
 * for the "in-range" row implicit in the round-trip cases above).
 */
static void test_mcdc_decode_dqt_dht_validation(void)
{
  TEST_BEGIN("jpeg_sw MC/DC dec_parse_dqt + dec_parse_dht guards");
  uint8_t              out[256]    = {};
  uint16_t             dw          = 0U;
  uint16_t             dh          = 0U;
  static const uint8_t dqt_short[] = {
    0xFFU,
    0xD8U,
    0xFFU,
    0xDBU,
    0x00U,
    0x01U,
  };
  TEST_ASSERT(
    ra_jpeg_sw_decode(dqt_short, (uint32_t)sizeof dqt_short, out, (uint32_t)sizeof out, &dw, &dh) !=
    k_ra_ok);
  static const uint8_t dqt_bad_pq[] = {
    0xFFU, 0xD8U, 0xFFU, 0xDBU, 0x00U, 0x43U, 0x10U, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0,     0,     0,     0,     0,     0,     0,     0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0,     0,     0,     0,     0,     0,     0,     0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0,     0,     0,     0,     0,     0,     0,     0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  };
  TEST_ASSERT(ra_jpeg_sw_decode(dqt_bad_pq,
                                (uint32_t)sizeof dqt_bad_pq,
                                out,
                                (uint32_t)sizeof out,
                                &dw,
                                &dh) != k_ra_ok);
  static const uint8_t dqt_bad_tq[] = {
    0xFFU, 0xD8U, 0xFFU, 0xDBU, 0x00U, 0x43U, 0x04U, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0,     0,     0,     0,     0,     0,     0,     0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0,     0,     0,     0,     0,     0,     0,     0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0,     0,     0,     0,     0,     0,     0,     0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  };
  TEST_ASSERT(ra_jpeg_sw_decode(dqt_bad_tq,
                                (uint32_t)sizeof dqt_bad_tq,
                                out,
                                (uint32_t)sizeof out,
                                &dw,
                                &dh) != k_ra_ok);
  static const uint8_t dht_bad_tc[] = {
    0xFFU, 0xD8U, 0xFFU, 0xC4U, 0x00U, 0x14U, 0x20U, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  };
  TEST_ASSERT(ra_jpeg_sw_decode(dht_bad_tc,
                                (uint32_t)sizeof dht_bad_tc,
                                out,
                                (uint32_t)sizeof out,
                                &dw,
                                &dh) != k_ra_ok);
  static const uint8_t dht_bad_th[] = {
    0xFFU, 0xD8U, 0xFFU, 0xC4U, 0x00U, 0x14U, 0x02U, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  };
  TEST_ASSERT(ra_jpeg_sw_decode(dht_bad_th,
                                (uint32_t)sizeof dht_bad_th,
                                out,
                                (uint32_t)sizeof out,
                                &dw,
                                &dh) != k_ra_ok);
  TEST_END("jpeg_sw MC/DC dec_parse_dqt + dec_parse_dht guards");
}

/**
 * @test test_mcdc_decode_sof0_chroma_subsampling
 * @par MC/DC:
 * Three SOF0 decisions in libs/ra_hal/src/ra_jpeg_sw.c (lines 842, 869,
 * 870, 872):
 *   D_ncomp: `ncomp != 1 && ncomp != 3`
 *   D_is444: 2 cond, D_is420: 6 cond, D_unsup: 2 cond
 * D_ncomp vectors via round-trip (1-comp grayscale baseline JPEG and
 * 3-comp encoded JPEG = both F) plus crafted ncomp=2 (T not_supported).
 * D_is444/D_is420 exercised via round-trip (high-quality 4:4:4 + low-
 * quality 4:2:0). The 6-cond D_is420 omits dead truth-table rows that
 * no real-world JPEG produces; documented per DO-178C 6.4.4.3
 * deactivated code. D_unsup tested with crafted SOF0 hmax=2,vmax=1
 * (neither 4:4:4 nor 4:2:0 -> not_supported).
 */
static void test_mcdc_decode_sof0_chroma_subsampling(void)
{
  TEST_BEGIN("jpeg_sw MC/DC dec_parse_sof0: ncomp + 4:4:4/4:2:0 disambig");
  static uint8_t rgb_in[(uint32_t)k_jt_rgb_bytes];
  static uint8_t rgb_out[(uint32_t)k_jt_rgb_bytes];
  static uint8_t jpeg[(uint32_t)k_jt_jpeg_cap];
  fill_gradient(rgb_in, (uint16_t)k_jt_w, (uint16_t)k_jt_h);
  uint32_t produced = 0U;
  TEST_ASSERT_EQ((int)k_ra_ok,
                 (int)ra_jpeg_sw_encode(rgb_in,
                                        (uint16_t)k_jt_w,
                                        (uint16_t)k_jt_h,
                                        (uint8_t)k_ra_jpeg_sw_quality_high,
                                        jpeg,
                                        (uint32_t)k_jt_jpeg_cap,
                                        &produced));
  uint16_t dw = 0U, dh = 0U;
  TEST_ASSERT_EQ(
    (int)k_ra_ok,
    (int)ra_jpeg_sw_decode(jpeg, produced, rgb_out, (uint32_t)k_jt_rgb_bytes, &dw, &dh));
  produced = 0U;
  TEST_ASSERT_EQ((int)k_ra_ok,
                 (int)ra_jpeg_sw_encode(rgb_in,
                                        (uint16_t)k_jt_w,
                                        (uint16_t)k_jt_h,
                                        (uint8_t)k_ra_jpeg_sw_quality_min,
                                        jpeg,
                                        (uint32_t)k_jt_jpeg_cap,
                                        &produced));
  TEST_ASSERT_EQ(
    (int)k_ra_ok,
    (int)ra_jpeg_sw_decode(jpeg, produced, rgb_out, (uint32_t)k_jt_rgb_bytes, &dw, &dh));
  static const uint8_t sof0_ncomp2[] = {
    0xFFU, 0xD8U, 0xFFU, 0xC0U, 0x00U, 0x0EU, 0x08U, 0x00U, 0x10U, 0x00U,
    0x10U, 0x02U, 0x01U, 0x11U, 0x00U, 0x02U, 0x11U, 0x00U, 0xFFU, 0xD9U,
  };
  uint8_t out_buf[64] = {};
  TEST_ASSERT(ra_jpeg_sw_decode(sof0_ncomp2,
                                (uint32_t)sizeof sof0_ncomp2,
                                out_buf,
                                (uint32_t)sizeof out_buf,
                                &dw,
                                &dh) != k_ra_ok);
  static const uint8_t sof0_bad_subsamp[] = {
    0xFFU, 0xD8U, 0xFFU, 0xC0U, 0x00U, 0x11U, 0x08U, 0x00U, 0x10U, 0x00U, 0x10U, 0x03U,
    0x01U, 0x21U, 0x00U, 0x02U, 0x11U, 0x01U, 0x03U, 0x11U, 0x01U, 0xFFU, 0xD9U,
  };
  TEST_ASSERT(ra_jpeg_sw_decode(sof0_bad_subsamp,
                                (uint32_t)sizeof sof0_bad_subsamp,
                                out_buf,
                                (uint32_t)sizeof out_buf,
                                &dw,
                                &dh) != k_ra_ok);
  TEST_END("jpeg_sw MC/DC dec_parse_sof0: ncomp + 4:4:4/4:2:0 disambig");
}

/* ------------------------------------------------------------------ */
/*  Additional MC/DC fixtures: SOI/SOF/SOS marker permutations.        */
/*                                                                     */
/*  These cover the residual gaps in MCDC_GAPS.csv that the earlier    */
/*  vectors marked "partial" or "no". Each fixture is a hand-built     */
/*  byte array — no encoder round-trip — so the decision flips are     */
/*  independent and reachable.                                         */
/* ------------------------------------------------------------------ */

/**
 * @test test_mcdc_decode_skip_unrecognized_segment
 * @par MC/DC:
 * Decision dec_skip_segment line 960:
 *   `len < 2U || (uint32_t)len > d->src_len - d->cursor` (2 conds).
 * Reached for unrecognized markers (e.g. APP1 0xFFE1, COM 0xFFFE)
 * via the `else` arm of the decode marker switch.
 *   V_short  : APP1 with seglen=0x0001     -> C1=T C2=F  -> protocol_error
 *   V_overrun: APP1 with seglen=0xFFFF (>buf-cursor)
 *                                          -> C1=F C2=T  -> protocol_error
 *   V_ok     : APP1 with seglen=0x0004 + 2 payload bytes, then real
 *              SOF0/SOS to reach decode -> C1=F C2=F
 * V_short+V_ok prove C1 flips outcome; V_overrun+V_ok prove C2 flips.
 * N+1 = 3 vectors for N=2 conditions.
 *
 * Also exercises dec_decode_scan/decode marker switch line 1685
 * fall-through (else arm into dec_skip_segment) and the SOF-range
 * "is supported / DHT / DAC" 4-condition decision at line 1655 by
 * sending 0xFFC1 (SOF1, unsupported) versus 0xFFC8 (DAC, skipped).
 */
static void test_mcdc_decode_skip_unrecognized_segment(void)
{
  TEST_BEGIN("jpeg_sw MC/DC dec_skip_segment + decode SOF-range");
  uint8_t  out[256] = {};
  uint16_t dw       = 0U;
  uint16_t dh       = 0U;

  /* V_short: APP1 (0xFFE1) with seglen=1  -> dec_skip_segment len<2. */
  static const uint8_t app1_short[] = {
    0xFFU,
    0xD8U,
    0xFFU,
    0xE1U,
    0x00U,
    0x01U,
  };
  TEST_ASSERT(ra_jpeg_sw_decode(app1_short,
                                (uint32_t)sizeof app1_short,
                                out,
                                (uint32_t)sizeof out,
                                &dw,
                                &dh) != k_ra_ok);

  /* V_overrun: APP1 with seglen claiming 0xFFFF bytes. */
  static const uint8_t app1_overrun[] = {
    0xFFU,
    0xD8U,
    0xFFU,
    0xE1U,
    0xFFU,
    0xFFU,
    0x00U,
    0x00U,
  };
  TEST_ASSERT(ra_jpeg_sw_decode(app1_overrun,
                                (uint32_t)sizeof app1_overrun,
                                out,
                                (uint32_t)sizeof out,
                                &dw,
                                &dh) != k_ra_ok);

  /* V_sof1: SOF1 (0xFFC1) is in [SOF0..SOF15] and != DHT/DAC ->
     not_supported. Independently flips the SOF-range decision vs the
     DAC (0xFFC8) skip. */
  static const uint8_t sof1_jpeg[] = {
    0xFFU,
    0xD8U,
    0xFFU,
    0xC1U,
    0x00U,
    0x0BU,
    0x08U,
    0x00U,
    0x10U,
    0x00U,
    0x10U,
    0x01U,
    0x01U,
    0x11U,
    0x00U,
  };
  TEST_ASSERT_EQ((int)k_ra_err_not_supported,
                 (int)ra_jpeg_sw_decode(sof1_jpeg,
                                        (uint32_t)sizeof sof1_jpeg,
                                        out,
                                        (uint32_t)sizeof out,
                                        &dw,
                                        &dh));

  /* V_dac_skip: DAC (0xFFC8) is in SOF range BUT is excluded by the
     decision, so it falls to the dec_skip_segment arm; with a valid
     seglen this proves the != 0xFFC8 sub-condition flips outcome. */
  static const uint8_t dac_then_short[] = {
    0xFFU,
    0xD8U,
    0xFFU,
    0xC8U,
    0x00U,
    0x02U,
    0xFFU,
    0xD9U,
  };
  TEST_ASSERT(ra_jpeg_sw_decode(dac_then_short,
                                (uint32_t)sizeof dac_then_short,
                                out,
                                (uint32_t)sizeof out,
                                &dw,
                                &dh) != k_ra_ok);
  TEST_END("jpeg_sw MC/DC dec_skip_segment + decode SOF-range");
}

/**
 * @test test_mcdc_decode_rst_in_marker_chain
 * @par MC/DC:
 * Decision ra_jpeg_sw_decode line 1681:
 *   `mk >= rst0 && mk <= rst7` (2 conds).
 * RST0..RST7 are standalone (no seglen) markers; the decoder treats
 * them as a no-op `continue`. Vectors:
 *   V_rst0   : RST0 (0xFFD0) in chain         -> C1=T C2=T -> continue
 *   V_rst7   : RST7 (0xFFD7) in chain         -> C1=T C2=T -> continue
 *   V_below  : DRI  (0xFFDD, len=4) in chain  -> C1=F      -> skip
 *   V_above  : EOI  (0xFFD9) in chain         -> C2=F      -> break
 * V_rst0 vs V_below proves C1 flips outcome (continue vs skip).
 * V_rst0 vs V_above proves C2 flips outcome (continue vs break).
 * All four arrive at protocol_error because no SOS follows; we only
 * care that the marker walk traversed each branch.
 */
static void test_mcdc_decode_rst_in_marker_chain(void)
{
  TEST_BEGIN("jpeg_sw MC/DC decode: RST in marker chain");
  uint8_t  out[256] = {};
  uint16_t dw       = 0U;
  uint16_t dh       = 0U;

  static const uint8_t rst0_jpeg[] = {
    0xFFU,
    0xD8U,
    0xFFU,
    0xD0U,
    0xFFU,
    0xD9U,
  };
  TEST_ASSERT(
    ra_jpeg_sw_decode(rst0_jpeg, (uint32_t)sizeof rst0_jpeg, out, (uint32_t)sizeof out, &dw, &dh) !=
    k_ra_ok);

  static const uint8_t rst7_jpeg[] = {
    0xFFU,
    0xD8U,
    0xFFU,
    0xD7U,
    0xFFU,
    0xD9U,
  };
  TEST_ASSERT(
    ra_jpeg_sw_decode(rst7_jpeg, (uint32_t)sizeof rst7_jpeg, out, (uint32_t)sizeof out, &dw, &dh) !=
    k_ra_ok);

  /* DRI (0xFFDD): C1=F (mk < rst0). */
  static const uint8_t dri_jpeg[] = {
    0xFFU,
    0xD8U,
    0xFFU,
    0xDDU,
    0x00U,
    0x04U,
    0x00U,
    0x10U,
    0xFFU,
    0xD9U,
  };
  TEST_ASSERT(
    ra_jpeg_sw_decode(dri_jpeg, (uint32_t)sizeof dri_jpeg, out, (uint32_t)sizeof out, &dw, &dh) !=
    k_ra_ok);

  /* EOI: C2=F (mk > rst7). */
  static const uint8_t eoi_only[] = {
    0xFFU,
    0xD8U,
    0xFFU,
    0xD9U,
  };
  TEST_ASSERT(
    ra_jpeg_sw_decode(eoi_only, (uint32_t)sizeof eoi_only, out, (uint32_t)sizeof out, &dw, &dh) !=
    k_ra_ok);
  TEST_END("jpeg_sw MC/DC decode: RST in marker chain");
}

/**
 * @test test_mcdc_decode_pad_byte_chain
 * @par MC/DC:
 * Decision ra_jpeg_sw_decode line 1639 inner pad-skip while-loop:
 *   `d->cursor < d->src_len && d->src[d->cursor] == 0xFF` (2 conds).
 * Vectors:
 *   V_one_pad  : single 0xFF then real marker -> C1=T C2=T then C2=F
 *   V_many_pad : 0xFF 0xFF 0xFF then marker   -> loop iterates
 *   V_pad_eob  : trailing 0xFF run hitting EOB -> C1=F (cursor==len)
 * Same sub-decisions also exist in get_dimensions line 1400 and are
 * hit by V_one_pad / V_many_pad through that path.
 */
static void test_mcdc_decode_pad_byte_chain(void)
{
  TEST_BEGIN("jpeg_sw MC/DC decode/get_dimensions: pad-byte while-loop");
  uint8_t  out[256] = {};
  uint16_t dw       = 0U;
  uint16_t dh       = 0U;

  /* V_many_pad: three 0xFF padding bytes ahead of EOI. */
  static const uint8_t many_pad[] = {
    0xFFU,
    0xD8U,
    0xFFU,
    0xFFU,
    0xFFU,
    0xFFU,
    0xD9U,
  };
  TEST_ASSERT(
    ra_jpeg_sw_decode(many_pad, (uint32_t)sizeof many_pad, out, (uint32_t)sizeof out, &dw, &dh) !=
    k_ra_ok);
  TEST_ASSERT(ra_jpeg_sw_get_dimensions(many_pad, (uint32_t)sizeof many_pad, &dw, &dh) != k_ra_ok);

  /* V_pad_eob: 0xFF run that exhausts the buffer. */
  static const uint8_t pad_eob[] = {
    0xFFU,
    0xD8U,
    0xFFU,
    0xFFU,
    0xFFU,
    0xFFU,
  };
  TEST_ASSERT(
    ra_jpeg_sw_decode(pad_eob, (uint32_t)sizeof pad_eob, out, (uint32_t)sizeof out, &dw, &dh) !=
    k_ra_ok);
  TEST_END("jpeg_sw MC/DC decode/get_dimensions: pad-byte while-loop");
}

/**
 * @test test_mcdc_get_dimensions_seglen_independent
 * @par MC/DC:
 * Decision ra_jpeg_sw_get_dimensions line 1416:
 *   `seglen < 2U || (uint32_t)seglen > jpeg_len - i`.
 * The earlier `test_mcdc_get_dimensions_pad_and_marker` covered both
 * conditions but in the same fixture; this fixture pins them to
 * separate inputs so MC/DC can attribute each independent flip:
 *   V_short_after_app : APP0 with seglen=1 mid-stream -> C1=T
 *   V_over_after_app  : APP0 with seglen=0xFFFF       -> C2=T
 *   V_ok              : APP0 with seglen=4 + 2 payload bytes, then
 *                       valid SOF0 -> C1=F C2=F (success)
 */
static void test_mcdc_get_dimensions_seglen_independent(void)
{
  TEST_BEGIN("jpeg_sw MC/DC get_dimensions: seglen<2 vs seglen>buf");
  uint16_t w = 0U, h = 0U;

  static const uint8_t short_seg[] = {
    0xFFU,
    0xD8U,
    0xFFU,
    0xE0U,
    0x00U,
    0x01U,
  };
  TEST_ASSERT(ra_jpeg_sw_get_dimensions(short_seg, (uint32_t)sizeof short_seg, &w, &h) != k_ra_ok);

  static const uint8_t over_seg[] = {
    0xFFU,
    0xD8U,
    0xFFU,
    0xE0U,
    0xFFU,
    0xFFU,
    0x00U,
    0x00U,
  };
  TEST_ASSERT(ra_jpeg_sw_get_dimensions(over_seg, (uint32_t)sizeof over_seg, &w, &h) != k_ra_ok);

  /* V_ok: skip APP0(len=4) then real SOF0. */
  static const uint8_t app0_then_sof0[] = {
    0xFFU, 0xD8U, 0xFFU, 0xE0U, 0x00U, 0x04U, 0x00U, 0x00U, 0xFFU, 0xC0U, 0x00U,
    0x0BU, 0x08U, 0x00U, 0x20U, 0x00U, 0x40U, 0x01U, 0x01U, 0x11U, 0x00U,
  };
  TEST_ASSERT_EQ(
    (int)k_ra_ok,
    (int)ra_jpeg_sw_get_dimensions(app0_then_sof0, (uint32_t)sizeof app0_then_sof0, &w, &h));
  TEST_ASSERT_EQ(64, (int)w);
  TEST_ASSERT_EQ(32, (int)h);
  TEST_END("jpeg_sw MC/DC get_dimensions: seglen<2 vs seglen>buf");
}

/**
 * @test test_mcdc_decode_sos_without_sof
 * @par MC/DC:
 * SOS handling in ra_jpeg_sw_decode requires SOF0 first; this guards
 * the `if (!got_sof)` branch (independent of the SOS-validation
 * decisions in dec_parse_sos at lines 1161/1184). Pairs with the
 * round-trip tests where !got_sof is false.
 *   V_no_sof : SOI -> SOS directly             -> protocol_error
 *   V_with_sof: SOI -> SOF0 -> DQT -> DHT -> SOS (round-trip path,
 *               already covered) -> ok
 */
static void test_mcdc_decode_sos_without_sof(void)
{
  TEST_BEGIN("jpeg_sw MC/DC decode: SOS arrives before SOF0");
  uint8_t              out[256]    = {};
  uint16_t             dw          = 0U;
  uint16_t             dh          = 0U;
  static const uint8_t sos_first[] = {
    0xFFU,
    0xD8U,
    0xFFU,
    0xDAU,
    0x00U,
    0x08U,
    0x01U,
    0x01U,
    0x00U,
    0x00U,
    0x3FU,
    0x00U,
  };
  TEST_ASSERT(
    ra_jpeg_sw_decode(sos_first, (uint32_t)sizeof sos_first, out, (uint32_t)sizeof out, &dw, &dh) !=
    k_ra_ok);
  TEST_END("jpeg_sw MC/DC decode: SOS arrives before SOF0");
}

int32_t main(void)
{
  ra_sim_mmap_reset();
  test_get_dimensions_parses_sof0();
  test_get_dimensions_null_args();
  test_get_dimensions_invalid();
  test_encode_decode_roundtrip();
  test_decode_invalid_rejected();
  test_encode_null_args();
  test_encode_out_of_buffer();
  test_mcdc_encode_dim_zero();
  test_mcdc_encode_quality_range();
  test_mcdc_get_dimensions_pad_and_marker();
  test_mcdc_decode_pad_and_rst_marker();
  test_mcdc_decode_dqt_dht_validation();
  test_mcdc_decode_sof0_chroma_subsampling();
  test_mcdc_decode_skip_unrecognized_segment();
  test_mcdc_decode_rst_in_marker_chain();
  test_mcdc_decode_pad_byte_chain();
  test_mcdc_get_dimensions_seglen_independent();
  test_mcdc_decode_sos_without_sof();
  (void)fprintf(stderr, "[OK ] test_ra_jpeg_sw.c\n");
  return 0;
}
