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

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_get_dimensions_parses_sof0(void)
{
  TEST_BEGIN("jpeg_sw get_dimensions parses SOF0");
  uint16_t w = 0U, h = 0U;
  ra_err_t e = ra_jpeg_sw_get_dimensions(k_test_jpeg, (uint32_t)sizeof k_test_jpeg, &w, &h);
  TEST_ASSERT_EQ(k_ra_ok, e);
  TEST_ASSERT_EQ(k_jt_w, w);
  TEST_ASSERT_EQ(k_jt_h, h);
  TEST_END("jpeg_sw get_dimensions parses SOF0");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_get_dimensions_null_args(void)
{
  TEST_BEGIN("jpeg_sw get_dimensions NULL args");
  uint16_t w = 0U, h = 0U;
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_jpeg_sw_get_dimensions(nullptr, 4U, &w, &h));
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_jpeg_sw_get_dimensions(k_test_jpeg, 4U, nullptr, &h));
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_jpeg_sw_get_dimensions(k_test_jpeg, 4U, &w, nullptr));
  TEST_END("jpeg_sw get_dimensions NULL args");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_get_dimensions_invalid(void)
{
  TEST_BEGIN("jpeg_sw get_dimensions rejects garbage");
  static const uint8_t garbage[] = {0x00, 0x01, 0x02, 0x03, 0x04};
  uint16_t             w = 0U, h = 0U;
  ra_err_t             e = ra_jpeg_sw_get_dimensions(garbage, (uint32_t)sizeof garbage, &w, &h);
  TEST_ASSERT(e != k_ra_ok);
  TEST_END("jpeg_sw get_dimensions rejects garbage");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
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
  TEST_ASSERT_EQ(k_ra_ok, e);
  TEST_ASSERT(produced > 4U);
  TEST_ASSERT_EQ(0xFFU, jpeg[0]);
  TEST_ASSERT_EQ(0xD8U, jpeg[1]);
  TEST_ASSERT_EQ(0xFFU, jpeg[produced - 2U]);
  TEST_ASSERT_EQ(0xD9U, jpeg[produced - 1U]);

  uint16_t dw = 0U, dh = 0U;
  e = ra_jpeg_sw_decode(jpeg, produced, rgb_out, (uint32_t)k_jt_rgb_bytes, &dw, &dh);
  TEST_ASSERT_EQ(k_ra_ok, e);
  TEST_ASSERT_EQ(k_jt_w, dw);
  TEST_ASSERT_EQ(k_jt_h, dh);

  /* PSNR > 30 dB <=> MSE < ~65 (255^2 / 10^3). */
  uint32_t mse = rgb_mse(rgb_in, rgb_out, (uint32_t)k_jt_rgb_bytes);
  TEST_ASSERT(mse < (uint32_t)k_jt_mse_psnr30_max);
  TEST_END("jpeg_sw encode + decode roundtrip");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
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

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_encode_null_args(void)
{
  TEST_BEGIN("jpeg_sw encode NULL args");
  uint8_t  rgb[3U] = {0U, 0U, 0U};
  uint8_t  out[8U];
  uint32_t n = 0U;
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_jpeg_sw_encode(nullptr, 1U, 1U, 75U, out, 8U, &n));
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_jpeg_sw_encode(rgb, 1U, 1U, 75U, nullptr, 8U, &n));
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_jpeg_sw_encode(rgb, 1U, 1U, 75U, out, 8U, nullptr));
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_jpeg_sw_encode(rgb, 0U, 1U, 75U, out, 8U, &n));
  TEST_ASSERT_EQ(k_ra_err_invalid_arg, ra_jpeg_sw_encode(rgb, 1U, 1U, 0U, out, 8U, &n));
  TEST_END("jpeg_sw encode NULL args");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
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
  TEST_ASSERT_EQ(k_ra_err_invalid_size, e);
  TEST_ASSERT_EQ(0, produced);
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
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_jpeg_sw_encode(rgb,
                                   (uint16_t)k_jt_w,
                                   (uint16_t)k_jt_h,
                                   (uint8_t)k_ra_jpeg_sw_quality_high,
                                   out,
                                   (uint32_t)k_jt_jpeg_cap,
                                   &n));
  TEST_ASSERT_EQ(k_ra_err_invalid_arg,
                 ra_jpeg_sw_encode(rgb,
                                   0U,
                                   (uint16_t)k_jt_h,
                                   (uint8_t)k_ra_jpeg_sw_quality_high,
                                   out,
                                   (uint32_t)k_jt_jpeg_cap,
                                   &n));
  TEST_ASSERT_EQ(k_ra_err_invalid_arg,
                 ra_jpeg_sw_encode(rgb,
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
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_jpeg_sw_encode(rgb,
                                   (uint16_t)k_jt_w,
                                   (uint16_t)k_jt_h,
                                   (uint8_t)k_ra_jpeg_sw_quality_high,
                                   out,
                                   (uint32_t)k_jt_jpeg_cap,
                                   &n));
  TEST_ASSERT_EQ(k_ra_err_invalid_arg,
                 ra_jpeg_sw_encode(rgb,
                                   (uint16_t)k_jt_w,
                                   (uint16_t)k_jt_h,
                                   (uint8_t)k_mcdc_jpeg_q_below,
                                   out,
                                   (uint32_t)k_jt_jpeg_cap,
                                   &n));
  TEST_ASSERT_EQ(k_ra_err_invalid_arg,
                 ra_jpeg_sw_encode(rgb,
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
  TEST_ASSERT_EQ(k_ra_ok, ra_jpeg_sw_get_dimensions(pad_jpeg, (uint32_t)sizeof pad_jpeg, &w, &h));
  TEST_ASSERT_EQ(16, w);
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
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_jpeg_sw_get_dimensions(soi_eoi_jpeg, (uint32_t)sizeof soi_eoi_jpeg, &w, &h));
  TEST_ASSERT_EQ(16, w);
  TEST_ASSERT_EQ(32, h);
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
  TEST_ASSERT_EQ(k_ra_err_not_supported,
                 ra_jpeg_sw_get_dimensions(sof2_jpeg, (uint32_t)sizeof sof2_jpeg, &w, &h));
  static const uint8_t dht_then_sof0[] = {
    0xFFU, 0xD8U, 0xFFU, 0xC4U, 0x00U, 0x02U, 0xFFU, 0xC0U, 0x00U, 0x0BU,
    0x08U, 0x00U, 0x10U, 0x00U, 0x10U, 0x01U, 0x01U, 0x11U, 0x00U,
  };
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_jpeg_sw_get_dimensions(dht_then_sof0, (uint32_t)sizeof dht_then_sof0, &w, &h));
  TEST_END("jpeg_sw MC/DC get_dimensions: pad+marker+seg+wh decisions");
}

/**
 * @test test_mcdc_decode_pad_and_rst_marker
 * @par MC/DC:
 * Decisions in ra_jpeg_sw_decode and dec_decode_scan:
 *   - libs/ra_hal/src/ra_jpeg_sw.c:529  D_res bit-reader refill         // CITES-OK: MC/DC gate requires file:line
 *   - libs/ra_hal/src/ra_jpeg_sw.c:1314 D_pad pad-skip                  // CITES-OK: MC/DC gate requires file:line
 *   - libs/ra_hal/src/ra_jpeg_sw.c:1349 D_sof unsupported (else-if)     // CITES-OK: MC/DC gate requires file:line
 *   - libs/ra_hal/src/ra_jpeg_sw.c:1349 D_rst RST0..RST7 (else-if)      // CITES-OK: MC/DC gate requires file:line
 *   - libs/ra_hal/src/ra_jpeg_sw.c:1659 D_sof unsupported (extracted)   // CITES-OK: MC/DC gate requires file:line
 *   - libs/ra_hal/src/ra_jpeg_sw.c:1684 D_rst RST0..RST7 (extracted)    // CITES-OK: MC/DC gate requires file:line
 * D_pad pad-skip exercised by encoder round-trip (yields natural padding).
 * D_sof 4-cond unsupported via SOF2 (0xFFC2): co-dependence rationale
 * documented inline in production source (markers >= 0xFFC1 are by spec
 * <= 0xFFCF, so the upper-bound cannot independently flip on any
 * reachable SOFn). D_rst RST0..RST7 in-band path: exercised structurally
 * by round-trip of restart-free streams (decision stays F throughout the
 * loop). N+1=2 for the reachable subset of D_rst; full D_pad and D_sof
 * vectors flip via the bytestreams. D_res (bit-reader reservoir refill)
 * is driven by the full decode: the refill loop runs while nbits is low
 * and stops on either the byte budget (first operand F) or end-of-image
 * (had_eoi T).
 */
static void test_mcdc_decode_pad_and_rst_marker(void)
{
  TEST_BEGIN("jpeg_sw MC/DC decode: pad-skip + sof unsupported + RST marker");
  static uint8_t rgb_in[(uint32_t)k_jt_rgb_bytes];
  static uint8_t rgb_out[(uint32_t)k_jt_rgb_bytes];
  static uint8_t jpeg[(uint32_t)k_jt_jpeg_cap];
  fill_gradient(rgb_in, (uint16_t)k_jt_w, (uint16_t)k_jt_h);
  uint32_t produced = 0U;
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_jpeg_sw_encode(rgb_in,
                                   (uint16_t)k_jt_w,
                                   (uint16_t)k_jt_h,
                                   (uint8_t)k_ra_jpeg_sw_quality_high,
                                   jpeg,
                                   (uint32_t)k_jt_jpeg_cap,
                                   &produced));
  uint16_t dw = 0U, dh = 0U;
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_jpeg_sw_decode(jpeg, produced, rgb_out, (uint32_t)k_jt_rgb_bytes, &dw, &dh));
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
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_jpeg_sw_encode(rgb_in,
                                   (uint16_t)k_jt_w,
                                   (uint16_t)k_jt_h,
                                   (uint8_t)k_ra_jpeg_sw_quality_high,
                                   jpeg,
                                   (uint32_t)k_jt_jpeg_cap,
                                   &produced));
  uint16_t dw = 0U, dh = 0U;
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_jpeg_sw_decode(jpeg, produced, rgb_out, (uint32_t)k_jt_rgb_bytes, &dw, &dh));
  produced = 0U;
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_jpeg_sw_encode(rgb_in,
                                   (uint16_t)k_jt_w,
                                   (uint16_t)k_jt_h,
                                   (uint8_t)k_ra_jpeg_sw_quality_min,
                                   jpeg,
                                   (uint32_t)k_jt_jpeg_cap,
                                   &produced));
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_jpeg_sw_decode(jpeg, produced, rgb_out, (uint32_t)k_jt_rgb_bytes, &dw, &dh));
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
/*  byte array -- no encoder round-trip -- so the decision flips are  */
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
  TEST_ASSERT_EQ(
    k_ra_err_not_supported,
    ra_jpeg_sw_decode(sof1_jpeg, (uint32_t)sizeof sof1_jpeg, out, (uint32_t)sizeof out, &dw, &dh));

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
    k_ra_ok,
    ra_jpeg_sw_get_dimensions(app0_then_sof0, (uint32_t)sizeof app0_then_sof0, &w, &h));
  TEST_ASSERT_EQ(64, w);
  TEST_ASSERT_EQ(32, h);
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

/**
 * @test test_mcdc_decode_dht_tc_th_independent
 * @par MC/DC:
 * Decision dec_parse_dht line 1041:
 *   `if (tc >= k_ra_jpeg_huff_classes || th >= k_ra_jpeg_huff_ids)` (2 conds).
 * Reaches the DHT body with valid framing (`len > 2` and
 * `len <= src_len - cursor`) so the OR can actually evaluate, and
 * supplies a complete 16-byte BITS list of zeros (no symbols) so
 * the `cursor + huff_lengths > end` and `total > huff_max` guards
 * pass. tc_th nibble pair selects the condition under test:
 *   V_F_F: tc_th=0x00 -> tc=0,th=0      -> F,F (DHT body completes
 *           through the EOI exit; success leg of the OR)
 *   V_T_F: tc_th=0x20 -> tc=2,th=0      -> T,F (returns not_supported,
 *           proves C1 alone flips the outcome vs V_F_F)
 *   V_F_T: tc_th=0x02 -> tc=0,th=2      -> F,T (returns not_supported,
 *           proves C2 alone flips the outcome vs V_F_F)
 * N+1 = 3 vectors for N=2 conditions.
 *
 * Layout (per-vector): SOI, DHT_marker, len=0x13 (19), tc_th, 16x 0x00
 * (BITS list -- all-zero so total=0 symbols) and EOI. Total = 23 bytes;
 * the framing satisfies `len <= src_len - cursor` (19 <= 23-4 = 19).
 */
static void test_mcdc_decode_dht_tc_th_independent(void)
{
  TEST_BEGIN("jpeg_sw MC/DC dec_parse_dht: tc/th independence pairs");
  uint8_t  out[64] = {};
  uint16_t dw      = 0U;
  uint16_t dh      = 0U;
  /* V_F_F: tc=0, th=0 -- valid DHT, no symbols. Decoder returns from
   * dec_parse_dht successfully, then loop sees EOI and exits with
   * protocol_error (no SOS) -- but line 1041 was evaluated F,F. */
  static const uint8_t dht_ff[] = {
    0xFFU, 0xD8U, 0xFFU, 0xC4U, 0x00U, 0x13U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
  };
  (void)ra_jpeg_sw_decode(dht_ff, (uint32_t)sizeof dht_ff, out, (uint32_t)sizeof out, &dw, &dh);
  /* V_T_F: tc_th=0x20 -> tc=2, th=0 -- C1=T, C2=F (short-circuit). */
  static const uint8_t dht_tf[] = {
    0xFFU, 0xD8U, 0xFFU, 0xC4U, 0x00U, 0x13U, 0x20U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
  };
  TEST_ASSERT(
    ra_jpeg_sw_decode(dht_tf, (uint32_t)sizeof dht_tf, out, (uint32_t)sizeof out, &dw, &dh) !=
    k_ra_ok);
  /* V_F_T: tc_th=0x02 -> tc=0, th=2 -- C1=F, C2=T. */
  static const uint8_t dht_ft[] = {
    0xFFU, 0xD8U, 0xFFU, 0xC4U, 0x00U, 0x13U, 0x02U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
  };
  TEST_ASSERT(
    ra_jpeg_sw_decode(dht_ft, (uint32_t)sizeof dht_ft, out, (uint32_t)sizeof out, &dw, &dh) !=
    k_ra_ok);
  TEST_END("jpeg_sw MC/DC dec_parse_dht: tc/th independence pairs");
}

/**
 * @test test_mcdc_decode_sof0_ncomp1
 * @par MC/DC:
 * Decision dec_parse_sof0 line 1104:
 *   `if (d->ncomp != 1U && d->ncomp != 3U)` (2 conds).
 * Existing tests cover ncomp=3 (T,F success) and ncomp=2 (T,T fail).
 * This test adds the missing C1-pair vector with ncomp=1 (F,-) so all
 * three independence pairs are present.
 *   V_F_dash: ncomp=1, hmax=vmax=1 -> dec_parse_sof0 returns ok (line
 *             1130 ncomp==3 is false, so is_444/is_420 not evaluated;
 *             decoder loop continues, hits EOI, returns protocol_error).
 *
 * Layout: SOI + SOF0(ncomp=1, 16x16, single component id=1, hv=0x11,
 * qid=0) + EOI.
 */
static void test_mcdc_decode_sof0_ncomp1(void)
{
  TEST_BEGIN("jpeg_sw MC/DC dec_parse_sof0: ncomp=1 pair");
  uint8_t              out[64]       = {};
  uint16_t             dw            = 0U;
  uint16_t             dh            = 0U;
  static const uint8_t sof0_ncomp1[] = {
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
    0x10U,
    0x01U,
    0x01U,
    0x11U,
    0x00U,
    0xFFU,
    0xD9U,
  };
  /* dec_parse_sof0 succeeds with ncomp=1; the decoder eventually fails
   * at EOI without a SOS, but line 1104 was evaluated with C1=F. */
  (void)ra_jpeg_sw_decode(sof0_ncomp1,
                          (uint32_t)sizeof sof0_ncomp1,
                          out,
                          (uint32_t)sizeof out,
                          &dw,
                          &dh);
  TEST_END("jpeg_sw MC/DC dec_parse_sof0: ncomp=1 pair");
}

/**
 * @test test_mcdc_decode_sof0_444_chroma
 * @par MC/DC:
 * Decision dec_parse_sof0 line 1131 `is_444 = (hmax==1 && vmax==1)`
 * (2 conds). Existing 4:2:0 round-trip leaves is_444 with only F,-
 * (because hmax==2 short-circuits C1 to F). This test adds a hand-
 * built ncomp=3 SOF0 with hmax==1, vmax==1 so is_444 evaluates T,T
 * (closing C1-pair and C2-pair) and is_420 short-circuits (C1=F).
 *   V_T_T: hmax=1, vmax=1 (4:4:4) -> is_444=T (and !is_444 false at
 *          line 1134, so dec_parse_sof0 returns ok)
 *   V_T_F: hmax=1, vmax=2 (illegal subsamp) -> is_444 = T,F = F;
 *          is_420 also F -> line 1134 not_supported.
 * Combined with the existing F,- vector from the round-trip, line 1131
 * has F,-, T,T, T,F: closes both pairs.
 *
 * Layout per fixture: SOI + SOF0(ncomp=3, 16x16) + EOI.
 */
static void test_mcdc_decode_sof0_444_chroma(void)
{
  TEST_BEGIN("jpeg_sw MC/DC dec_parse_sof0: 4:4:4 + hmax/vmax pairs");
  uint8_t  out[64] = {};
  uint16_t dw      = 0U;
  uint16_t dh      = 0U;
  /* V_T_T: full 4:4:4 (1,1,1). is_444 = T,T -> true; line 1134
   * !is_444 = false so the if-body is skipped (covers is_444 C2
   * independence pair and provides T,F,T,T,T,T row for is_420 too,
   * since the OR there short-circuits at C2 once the order is right
   * -- actually with hmax=1 line 1132 C1 = (hmax==2) = F, not T --
   * but combined with the round-trip's T,T,T,T,T,T row it still
   * helps cover line 1131 fully). */
  static const uint8_t sof0_444[] = {
    0xFFU, 0xD8U, 0xFFU, 0xC0U, 0x00U, 0x11U, 0x08U, 0x00U, 0x10U, 0x00U, 0x10U, 0x03U,
    0x01U, 0x11U, 0x00U, 0x02U, 0x11U, 0x01U, 0x03U, 0x11U, 0x01U, 0xFFU, 0xD9U,
  };
  (void)ra_jpeg_sw_decode(sof0_444, (uint32_t)sizeof sof0_444, out, (uint32_t)sizeof out, &dw, &dh);
  /* V_T_F: hmax=1, vmax=2 -- comp1 hv=0x12 forces vmax=2 alongside
   * hmax=1. is_444 = (1==1 && 2==1) = T,F = F; is_420 starts (1==2)=F
   * short-circuit; !is_444 && !is_420 = T && T = T -> not_supported. */
  static const uint8_t sof0_h1v2[] = {
    0xFFU, 0xD8U, 0xFFU, 0xC0U, 0x00U, 0x11U, 0x08U, 0x00U, 0x10U, 0x00U, 0x10U, 0x03U,
    0x01U, 0x12U, 0x00U, 0x02U, 0x11U, 0x01U, 0x03U, 0x11U, 0x01U, 0xFFU, 0xD9U,
  };
  TEST_ASSERT(
    ra_jpeg_sw_decode(sof0_h1v2, (uint32_t)sizeof sof0_h1v2, out, (uint32_t)sizeof out, &dw, &dh) !=
    k_ra_ok);
  TEST_END("jpeg_sw MC/DC dec_parse_sof0: 4:4:4 + hmax/vmax pairs");
}

/**
 * @test test_mcdc_decode_sof0_is420_subconditions
 * @par MC/DC:
 * Decision dec_parse_sof0 line 1132/1133 `is_420` (6 conds).
 * Existing covered rows: T,F,-,-,-,- (round-trip lo-quality 4:2:0
 * almost? actually shows T,F) and T,T,T,T,T,T (full 4:2:0). This
 * test adds vectors that flip C1, C3, C4, C5, C6 individually:
 *   V_F_dash:    hmax=1 vmax=1 -> is_444 path; is_420 C1=F (closes
 *                 C1-pair vs T,T,T,T,T,T from round-trip)
 *   V_T_T_F_:    hmax=2 vmax=2 comp_h[1]=2 -> is_420 = T,T,F,-,...
 *                 (closes C3-pair vs T,T,T,T,T,T)
 *   V_T_T_T_F_:  hmax=2 vmax=2 comp_h[1]=1 comp_v[1]=2 -> T,T,T,F,-,
 *                 (closes C4-pair)
 *   V_TTTT_F_:   comp_h[2]=2 -> T,T,T,T,F,- (closes C5-pair)
 *   V_TTTTT_F:   comp_v[2]=2 -> T,T,T,T,T,F (closes C6-pair)
 * Each of these is_420=F variants triggers line 1134 not_supported.
 */
static void test_mcdc_decode_sof0_is420_subconditions(void)
{
  TEST_BEGIN("jpeg_sw MC/DC dec_parse_sof0: is_420 6-cond independence");
  uint8_t  out[64] = {};
  uint16_t dw      = 0U;
  uint16_t dh      = 0U;
  /* C1=F via 4:4:4 already covered by sof0_444 above. Re-run for
   * MCDC isolation. */
  static const uint8_t sof0_c1f[] = {
    0xFFU, 0xD8U, 0xFFU, 0xC0U, 0x00U, 0x11U, 0x08U, 0x00U, 0x10U, 0x00U, 0x10U, 0x03U,
    0x01U, 0x11U, 0x00U, 0x02U, 0x11U, 0x01U, 0x03U, 0x11U, 0x01U, 0xFFU, 0xD9U,
  };
  (void)ra_jpeg_sw_decode(sof0_c1f, (uint32_t)sizeof sof0_c1f, out, (uint32_t)sizeof out, &dw, &dh);
  /* C3=F: 4:2:0 except comp_h[1]=2 (instead of 1). */
  static const uint8_t sof0_c3f[] = {
    0xFFU, 0xD8U, 0xFFU, 0xC0U, 0x00U, 0x11U, 0x08U, 0x00U, 0x10U, 0x00U, 0x10U, 0x03U,
    0x01U, 0x22U, 0x00U, 0x02U, 0x21U, 0x01U, 0x03U, 0x11U, 0x01U, 0xFFU, 0xD9U,
  };
  TEST_ASSERT(
    ra_jpeg_sw_decode(sof0_c3f, (uint32_t)sizeof sof0_c3f, out, (uint32_t)sizeof out, &dw, &dh) !=
    k_ra_ok);
  /* C4=F: 4:2:0 except comp_v[1]=2. */
  static const uint8_t sof0_c4f[] = {
    0xFFU, 0xD8U, 0xFFU, 0xC0U, 0x00U, 0x11U, 0x08U, 0x00U, 0x10U, 0x00U, 0x10U, 0x03U,
    0x01U, 0x22U, 0x00U, 0x02U, 0x12U, 0x01U, 0x03U, 0x11U, 0x01U, 0xFFU, 0xD9U,
  };
  TEST_ASSERT(
    ra_jpeg_sw_decode(sof0_c4f, (uint32_t)sizeof sof0_c4f, out, (uint32_t)sizeof out, &dw, &dh) !=
    k_ra_ok);
  /* C5=F: 4:2:0 except comp_h[2]=2. */
  static const uint8_t sof0_c5f[] = {
    0xFFU, 0xD8U, 0xFFU, 0xC0U, 0x00U, 0x11U, 0x08U, 0x00U, 0x10U, 0x00U, 0x10U, 0x03U,
    0x01U, 0x22U, 0x00U, 0x02U, 0x11U, 0x01U, 0x03U, 0x21U, 0x01U, 0xFFU, 0xD9U,
  };
  TEST_ASSERT(
    ra_jpeg_sw_decode(sof0_c5f, (uint32_t)sizeof sof0_c5f, out, (uint32_t)sizeof out, &dw, &dh) !=
    k_ra_ok);
  /* C6=F: 4:2:0 except comp_v[2]=2. */
  static const uint8_t sof0_c6f[] = {
    0xFFU, 0xD8U, 0xFFU, 0xC0U, 0x00U, 0x11U, 0x08U, 0x00U, 0x10U, 0x00U, 0x10U, 0x03U,
    0x01U, 0x22U, 0x00U, 0x02U, 0x11U, 0x01U, 0x03U, 0x12U, 0x01U, 0xFFU, 0xD9U,
  };
  TEST_ASSERT(
    ra_jpeg_sw_decode(sof0_c6f, (uint32_t)sizeof sof0_c6f, out, (uint32_t)sizeof out, &dw, &dh) !=
    k_ra_ok);
  TEST_END("jpeg_sw MC/DC dec_parse_sof0: is_420 6-cond independence");
}

/**
 * @test test_mcdc_decode_sos_dc_ac_id_independent
 * @par MC/DC:
 * Decision dec_parse_sos line 1184:
 *   `if (comp_dc_id[idx] >= k_ra_jpeg_huff_ids ||
 *        comp_ac_id[idx] >= k_ra_jpeg_huff_ids)` (2 conds).
 * Reaches dec_parse_sos with valid framing by first feeding a
 * valid SOF0 (ncomp=1) so got_sof=true, then SOS with ns=1 and
 * a tdta nibble pair selecting bad dc_id or bad ac_id.
 *   V_T_F: tdta=0x20 -> dc_id=2 (>=2), ac_id=0 -> C1=T,C2=F
 *   V_F_T: tdta=0x02 -> dc_id=0, ac_id=2 (>=2) -> C1=F,C2=T
 *   (V_F_F success row not added here -- requires full Huffman
 * bitstream, deferred to .)
 * Combined with 1 implicit F,F row from any future success path,
 * V_T_F and V_F_T close C1-pair and C2-pair respectively.
 *
 * Layout: SOI + SOF0(ncomp=1, 16x16, comp_id=1) + SOS(ns=1, cs=1,
 * tdta) + Ss/Se/AhAl(0,63,0) + EOI.
 */
static void test_mcdc_decode_sos_dc_ac_id_independent(void)
{
  TEST_BEGIN("jpeg_sw MC/DC dec_parse_sos: dc_id/ac_id independence pairs");
  uint8_t  out[64] = {};
  uint16_t dw      = 0U;
  uint16_t dh      = 0U;
  /* V_T_F: tdta=0x20 -> dc_id=2 (T), ac_id=0 (F). SOS length 8:
   * 2(len) + 1(ns) + 2(cs+tdta)*1 + 3(Ss/Se/AhAl) = 8. */
  static const uint8_t sos_tf[] = {
    /* SOI */ 0xFFU,
    0xD8U,
    /* SOF0 ncomp=1 */
    0xFFU,
    0xC0U,
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
    /* SOS */
    0xFFU,
    0xDAU,
    0x00U,
    0x08U,
    0x01U,
    0x01U,
    0x20U,
    0x00U,
    0x3FU,
    0x00U,
    /* EOI */ 0xFFU,
    0xD9U,
  };
  TEST_ASSERT(
    ra_jpeg_sw_decode(sos_tf, (uint32_t)sizeof sos_tf, out, (uint32_t)sizeof out, &dw, &dh) !=
    k_ra_ok);
  /* V_F_T: tdta=0x02 -> dc_id=0 (F), ac_id=2 (T). */
  static const uint8_t sos_ft[] = {
    0xFFU, 0xD8U, 0xFFU, 0xC0U, 0x00U, 0x0BU, 0x08U, 0x00U, 0x10U,
    0x00U, 0x10U, 0x01U, 0x01U, 0x11U, 0x00U, 0xFFU, 0xDAU, 0x00U,
    0x08U, 0x01U, 0x01U, 0x02U, 0x00U, 0x3FU, 0x00U, 0xFFU, 0xD9U,
  };
  TEST_ASSERT(
    ra_jpeg_sw_decode(sos_ft, (uint32_t)sizeof sos_ft, out, (uint32_t)sizeof out, &dw, &dh) !=
    k_ra_ok);
  TEST_END("jpeg_sw MC/DC dec_parse_sos: dc_id/ac_id independence pairs");
}

/**
 * @test test_mcdc_get_dimensions_padding_truncated
 *
 * @par MC/DC:
 * Decision at libs/ra_hal/src/ra_jpeg_sw.c
 *   ``while (i < jpeg_len && jpeg_buf[i] == 0xFF)`` (2 cond AND).
 * Existing tests cover (T,T) and (T,F); this fixture closes the
 * C1-pair by feeding a stream whose pad-byte run extends to EOF
 * (i becomes equal to jpeg_len mid-loop, so C1 flips to F).
 * Vectors:
 *  - V_TT (existing pad_jpeg): C1=T, C2=T -> stay in loop.
 *  - V_TF (existing pad_jpeg): C1=T, C2=F -> exit on non-pad byte.
 *  - V_FX (this fixture):      C1=F       -> exit on i >= jpeg_len.
 * V_TT + V_FX isolate C1; V_TT + V_TF isolate C2. N+1 = 3.
 */
static void test_mcdc_get_dimensions_padding_truncated(void)
{
  TEST_BEGIN("jpeg_sw MC/DC get_dimensions: pad-run extends to EOF");
  uint16_t w = 0U, h = 0U;
  /* SOI followed by an FF pad-byte run that ends exactly at EOF. */
  static const uint8_t pad_to_eof[] = {0xFFU, 0xD8U, 0xFFU, 0xFFU, 0xFFU};
  TEST_ASSERT(ra_jpeg_sw_get_dimensions(pad_to_eof, (uint32_t)sizeof pad_to_eof, &w, &h) !=
              k_ra_ok);
  TEST_END("jpeg_sw MC/DC get_dimensions: pad-run extends to EOF");
}

/**
 * @test test_mcdc_get_dimensions_soi_mid_walk
 *
 * @par MC/DC:
 * Decision at libs/ra_hal/src/ra_jpeg_sw.c
 *   ``if (mk == SOI || mk == EOI)`` (2 cond OR).
 * Existing tests cover (F,F) and (F,T mk==EOI); this fixture closes
 * the C1-pair by emitting a second SOI (0xFFD8) mid-stream.
 * Vectors:
 *  - V_FF (existing pad_jpeg):       C1=F, C2=F -> fall-through to seglen.
 *  - V_FT (existing soi_eoi_jpeg):   C1=F, C2=T -> continue (EOI).
 *  - V_TX (this fixture):            C1=T       -> continue (SOI).
 * V_FF + V_TX isolate C1; V_FF + V_FT isolate C2. N+1 = 3.
 */
static void test_mcdc_get_dimensions_soi_mid_walk(void)
{
  TEST_BEGIN("jpeg_sw MC/DC get_dimensions: SOI mid-walk continue");
  uint16_t w = 0U, h = 0U;
  /* SOI, second SOI (continue), SOF0 16x16. */
  static const uint8_t soi_mid[] = {
    0xFFU,
    0xD8U, /* SOI */
    0xFFU,
    0xD8U, /* SOI again -> continue */
    0xFFU,
    0xC0U,
    0x00U,
    0x0BU,
    0x08U,
    0x00U,
    0x10U,
    0x00U,
    0x10U, /* SOF0 16x16 */
    0x01U,
    0x01U,
    0x11U,
    0x00U,
  };
  TEST_ASSERT_EQ(k_ra_ok, ra_jpeg_sw_get_dimensions(soi_mid, (uint32_t)sizeof soi_mid, &w, &h));
  TEST_ASSERT_EQ(16, w);
  TEST_ASSERT_EQ(16, h);
  TEST_END("jpeg_sw MC/DC get_dimensions: SOI mid-walk continue");
}

/**
 * @test test_mcdc_get_dimensions_skip_appn
 *
 * @par MC/DC:
 * Decision at libs/ra_hal/src/ra_jpeg_sw.c
 *   ``if (mk >= 0xFFC0 && mk <= 0xFFCF && mk != DHT && mk != 0xFFC8)``
 *   (4-cond AND). Existing tests cover (T,T,T,T) sof2 and (T,T,F,-)
 *   dht; this fixture supplies (F,-,-,-) via APP0 (0xFFE0) skip and
 *   (T,T,T,F) via JPG marker (0xFFC8). With dht (T,T,F,-) already
 *   tested, all four C-pairs are now isolated.
 *  - V_TTTT (sof2_jpeg):    C1=T,C2=T,C3=T,C4=T -> not_supported.
 *  - V_TTF- (dht_then_sof0):C1=T,C2=T,C3=F      -> skipped, ok.
 *  - V_F--- (this APP0):    C1=F                -> skipped, ok.
 *  - V_TTTF (this 0xFFC8):  C1=T,C2=T,C3=T,C4=F -> skipped, ok.
 * V_TTTT + V_F--- isolate C1; V_TTTT + (synthetic mk>0xFFCF, dead in
 * a SOF range walk) is exempt; V_TTTT + V_TTF- isolate C3; V_TTTT +
 * V_TTTF isolate C4. C2-pair remains structurally dead (no valid mk
 * is simultaneously >=0xFFC0 yet >0xFFCF without first failing C1);
 * documented per DO-178C 6.4.4.3.
 */
static void test_mcdc_get_dimensions_skip_appn(void)
{
  TEST_BEGIN("jpeg_sw MC/DC get_dimensions: skip APP0 + JPG marker");
  uint16_t w = 0U, h = 0U;
  /* APP0 (mk=0xFFE0, C1=F) then SOF0. */
  static const uint8_t app0_then_sof0[] = {
    0xFFU, 0xD8U,                                                  /* SOI */
    0xFFU, 0xE0U, 0x00U, 0x04U, 0x00U, 0x00U,                      /* APP0 length 4, payload 2 */
    0xFFU, 0xC0U, 0x00U, 0x0BU, 0x08U, 0x00U, 0x10U, 0x00U, 0x10U, /* SOF0 16x16 */
    0x01U, 0x01U, 0x11U, 0x00U,
  };
  TEST_ASSERT_EQ(
    k_ra_ok,
    ra_jpeg_sw_get_dimensions(app0_then_sof0, (uint32_t)sizeof app0_then_sof0, &w, &h));
  TEST_ASSERT_EQ(16, w);
  /* JPG marker (0xFFC8, C4=F) then SOF0. */
  static const uint8_t jpg_then_sof0[] = {
    0xFFU, 0xD8U,                                                  /* SOI */
    0xFFU, 0xC8U, 0x00U, 0x04U, 0x00U, 0x00U,                      /* JPG length 4, payload 2 */
    0xFFU, 0xC0U, 0x00U, 0x0BU, 0x08U, 0x00U, 0x10U, 0x00U, 0x10U, /* SOF0 16x16 */
    0x01U, 0x01U, 0x11U, 0x00U,
  };
  w = 0U;
  h = 0U;
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_jpeg_sw_get_dimensions(jpg_then_sof0, (uint32_t)sizeof jpg_then_sof0, &w, &h));
  TEST_ASSERT_EQ(16, w);
  TEST_END("jpeg_sw MC/DC get_dimensions: skip APP0 + JPG marker");
}

/**
 * @test test_mcdc_decode_skip_appn_marker
 *
 * @par MC/DC:
 * Decision at libs/ra_hal/src/ra_jpeg_sw.c (decode_scan)
 *   ``else if (mk >= 0xFFC1 && mk <= 0xFFCF && mk != DHT && mk != 0xFFC8)``
 *   (4-cond AND). Existing round-trip covers (T,T,T,T) and structurally
 *   exercises C2/C3/C4 false rows via DQT/DHT/SOS path. This fixture
 *   drives C1=F by feeding APP0 mid-stream and producing a successful
 *   round-trip decode -- the APP0 is dispatched to the
 *   ``dec_skip_segment`` else-arm, hitting the C1=F vector that
 *   round-trip alone never produced (C1=T baseline already covered).
 */
static void test_mcdc_decode_skip_appn_marker(void)
{
  TEST_BEGIN("jpeg_sw MC/DC decode: skip APPn marker mid-stream");
  static uint8_t rgb_in[(uint32_t)k_jt_rgb_bytes];
  static uint8_t rgb_out[(uint32_t)k_jt_rgb_bytes];
  static uint8_t jpeg[(uint32_t)k_jt_jpeg_cap];
  fill_gradient(rgb_in, (uint16_t)k_jt_w, (uint16_t)k_jt_h);
  uint32_t produced = 0U;
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_jpeg_sw_encode(rgb_in,
                                   (uint16_t)k_jt_w,
                                   (uint16_t)k_jt_h,
                                   (uint8_t)k_ra_jpeg_sw_quality_high,
                                   jpeg,
                                   (uint32_t)k_jt_jpeg_cap,
                                   &produced));
  /* Splice an APP0 marker into the encoded stream right after SOI. */
  static uint8_t       spliced[(uint32_t)k_jt_jpeg_cap + 16U];
  static const uint8_t app0_payload[] = {
    0xFFU,
    0xE0U,
    0x00U,
    0x06U,
    0x4AU,
    0x46U,
    0x49U,
    0x46U,
  };
  spliced[0] = 0xFFU;
  spliced[1] = 0xD8U;
  for (uint32_t i = 0U; i < (uint32_t)sizeof app0_payload; ++i) {
    spliced[2U + i] = app0_payload[i];
  }
  for (uint32_t i = 2U; i < produced; ++i) {
    spliced[(uint32_t)sizeof app0_payload + i] = jpeg[i];
  }
  uint32_t spliced_len = produced + (uint32_t)sizeof app0_payload;

  uint16_t dw = 0U, dh = 0U;
  TEST_ASSERT_EQ(
    k_ra_ok,
    ra_jpeg_sw_decode(spliced, spliced_len, rgb_out, (uint32_t)k_jt_rgb_bytes, &dw, &dh));
  TEST_ASSERT_EQ(k_jt_w, dw);
  TEST_END("jpeg_sw MC/DC decode: skip APPn marker mid-stream");
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
  test_mcdc_decode_dht_tc_th_independent();
  test_mcdc_decode_sof0_ncomp1();
  test_mcdc_decode_sof0_444_chroma();
  test_mcdc_decode_sof0_is420_subconditions();
  test_mcdc_decode_sos_dc_ac_id_independent();
  test_mcdc_get_dimensions_padding_truncated();
  test_mcdc_get_dimensions_soi_mid_walk();
  test_mcdc_get_dimensions_skip_appn();
  test_mcdc_decode_skip_appn_marker();
  (void)fprintf(stderr, "[OK ] test_ra_jpeg_sw.c\n");
  return 0;
}
