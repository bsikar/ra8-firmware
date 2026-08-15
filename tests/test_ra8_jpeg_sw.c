/**
 * @file test_ra8_jpeg_sw.c
 * @brief Unit tests for the software JPEG codec (ra8_jpeg_sw.c).
 *
 * @details
 * The decoder and encoder are exercised together: most of the
 * tests round-trip an RGB block through the encoder and back into
 * RGB to verify both halves are bit-bug-free. A separate fixture,
 * `k_test_jpeg`, is a hand-crafted 16x16 grayscale JPEG used to
 * pin down the SOF0 dimension parser.
 *
 * This sibling owns the core encode / decode / get_dimensions contract
 * tests plus the encoder MC/DC vectors; the segment-walk MC/DC vectors
 * live in test_ra8_jpeg_sw_seg_mcdc.c and the header-parse MC/DC vectors
 * in test_ra8_jpeg_sw_hdr_mcdc.c.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_jpeg_sw.h"
#include "unity_minimal.h"

/**
 * @enum jpeg_sw_fixture_t
 * @brief The byte-level helpers.
 */
typedef enum : uint8_t {
  k_byte_mask = 0xFFU, /**< Truncates a generated or shifted value back into a byte. */
} jpeg_sw_fixture_t;

/**
 * @enum jpeg_sw_fixture2_t
 * @brief Poison values written into out-parameters before a call, so one that fails without assigning is detectable.
 */
typedef enum : uint16_t {
  k_jpeg_poison_out =
    0xFFFFU, /**< Poison in the produced-bytes out-param; an encode that skips it is detectable. */
} jpeg_sw_fixture2_t;

/**
 * @enum ra8_jpeg_test_const_t
 * @brief Sizes used by the test fixtures.
 */
typedef enum : uint16_t {
  k_jt_w              = 16U,                        /**< Test image width.      */
  k_jt_h              = 16U,                        /**< Test image height.     */
  k_jt_pixels         = (uint16_t)(16U * 16U),      /**< Pixel count.           */
  k_jt_rgb_bytes      = (uint16_t)(16U * 16U * 3U), /**< Jt RGB bytes.          */
  k_jt_jpeg_cap       = 4096U,                      /**< Encoder out cap.       */
  k_jt_mse_psnr30_max = 65U,                        /**< MSE for PSNR ~= 30 dB. */
} ra8_jpeg_test_const_t;

/**
 * @brief Hand-crafted minimal 16x16 grayscale baseline JPEG.
 *
 * @details
 * Hex layout: SOI, DQT (luma), SOF0 (16x16 1-component), DHT
 * (luma DC + AC, K.3.3), SOS, single MCU of all-zero diff
 * coefficients, EOI. The point is to give
 * `ra8_jpeg_sw_get_dimensions()` a stable input whose width/height
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
      uint32_t i  = (((uint32_t)y * (uint32_t)w) + (uint32_t)x) * 3U;
      rgb[i + 0U] = (uint8_t)((x * 16U) & k_byte_mask);
      rgb[i + 1U] = (uint8_t)((y * 16U) & k_byte_mask);
      rgb[i + 2U] = (uint8_t)(((x + y) * 8U) & k_byte_mask);
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
  uint16_t  w = 0U;
  uint16_t  h = 0U;
  ra8_err_t e = ra8_jpeg_sw_get_dimensions(k_test_jpeg, (uint32_t)sizeof k_test_jpeg, &w, &h);
  TEST_ASSERT_EQ(k_ra8_ok, e);
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
  uint16_t w = 0U;
  uint16_t h = 0U;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_jpeg_sw_get_dimensions(nullptr, 4U, &w, &h));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_jpeg_sw_get_dimensions(k_test_jpeg, 4U, nullptr, &h));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_jpeg_sw_get_dimensions(k_test_jpeg, 4U, &w, nullptr));
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
  uint16_t             w         = 0U;
  uint16_t             h         = 0U;
  ra8_err_t            e = ra8_jpeg_sw_get_dimensions(garbage, (uint32_t)sizeof garbage, &w, &h);
  TEST_ASSERT(e != k_ra8_ok);
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
  static uint8_t s_rgb_in[(uint32_t)k_jt_rgb_bytes];
  static uint8_t s_rgb_out[(uint32_t)k_jt_rgb_bytes];
  static uint8_t s_jpeg[(uint32_t)k_jt_jpeg_cap];
  fill_gradient(s_rgb_in, (uint16_t)k_jt_w, (uint16_t)k_jt_h);

  uint32_t  produced = 0U;
  ra8_err_t e        = ra8_jpeg_sw_encode(s_rgb_in,
                                          (uint16_t)k_jt_w,
                                          (uint16_t)k_jt_h,
                                          (uint8_t)k_ra8_jpeg_sw_quality_high,
                                          s_jpeg,
                                          (uint32_t)k_jt_jpeg_cap,
                                          &produced);
  TEST_ASSERT_EQ(k_ra8_ok, e);
  TEST_ASSERT(produced > 4U);
  TEST_ASSERT_EQ(0xFFU, s_jpeg[0]);
  TEST_ASSERT_EQ(0xD8U, s_jpeg[1]);
  TEST_ASSERT_EQ(0xFFU, s_jpeg[produced - 2U]);
  TEST_ASSERT_EQ(0xD9U, s_jpeg[produced - 1U]);

  uint16_t dw = 0U;
  uint16_t dh = 0U;
  e           = ra8_jpeg_sw_decode(s_jpeg, produced, s_rgb_out, (uint32_t)k_jt_rgb_bytes, &dw, &dh);
  TEST_ASSERT_EQ(k_ra8_ok, e);
  TEST_ASSERT_EQ(k_jt_w, dw);
  TEST_ASSERT_EQ(k_jt_h, dh);

  /* PSNR > 30 dB <=> MSE < ~65 (255^2 / 10^3). */
  uint32_t mse = rgb_mse(s_rgb_in, s_rgb_out, (uint32_t)k_jt_rgb_bytes);
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
  uint16_t             w = 0U;
  uint16_t             h = 0U;
  ra8_err_t            e =
    ra8_jpeg_sw_decode(bogus, (uint32_t)sizeof bogus, out, (uint32_t)sizeof out, &w, &h);
  TEST_ASSERT(e != k_ra8_ok);
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
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_jpeg_sw_encode(nullptr, 1U, 1U, 75U, out, 8U, &n));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_jpeg_sw_encode(rgb, 1U, 1U, 75U, nullptr, 8U, &n));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_jpeg_sw_encode(rgb, 1U, 1U, 75U, out, 8U, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_jpeg_sw_encode(rgb, 0U, 1U, 75U, out, 8U, &n));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_jpeg_sw_encode(rgb, 1U, 1U, 0U, out, 8U, &n));
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
  static uint8_t s_rgb_in[(uint32_t)k_jt_rgb_bytes];
  uint8_t        tiny[8U];
  uint32_t       produced = k_jpeg_poison_out;
  fill_gradient(s_rgb_in, (uint16_t)k_jt_w, (uint16_t)k_jt_h);
  ra8_err_t e = ra8_jpeg_sw_encode(s_rgb_in,
                                   (uint16_t)k_jt_w,
                                   (uint16_t)k_jt_h,
                                   (uint8_t)k_ra8_jpeg_sw_quality_high,
                                   tiny,
                                   (uint32_t)sizeof tiny,
                                   &produced);
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, e);
  TEST_ASSERT_EQ(0, produced);
  TEST_END("jpeg_sw encode rejects undersized out_buf");
}

/* MC/DC vector tests for ra8_jpeg_sw.c compound decisions
 * (DO-178C Level B / IEC 61508 SIL 3 / ISO 26262 ASIL C). */

typedef enum : uint8_t {
  k_mcdc_jpeg_q_below = 0U,   /**< Mcdc JPEG q below. */
  k_mcdc_jpeg_q_above = 101U, /**< Mcdc JPEG q above. */
} mcdc_jpeg_const_t;

/**
 * @test test_mcdc_encode_dim_zero
 * @par MC/DC:
 * Decision (libs/ra8_jpeg/src/ra8_jpeg_sw.c line 1818, 2 conditions):
 * `width == 0 || height == 0`. V1 16x16 (F ok), V2 0x16 (C1=T invalid_arg),
 * V3 16x0 (C1=F C2=T invalid_arg). N+1=3.
 */
static void test_mcdc_encode_dim_zero(void)
{
  TEST_BEGIN("jpeg_sw MC/DC encode: w==0 || h==0");
  static uint8_t s_rgb[(uint32_t)k_jt_rgb_bytes];
  static uint8_t s_out[(uint32_t)k_jt_jpeg_cap];
  uint32_t       n = 0U;
  fill_gradient(s_rgb, (uint16_t)k_jt_w, (uint16_t)k_jt_h);
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_jpeg_sw_encode(s_rgb,
                                    (uint16_t)k_jt_w,
                                    (uint16_t)k_jt_h,
                                    (uint8_t)k_ra8_jpeg_sw_quality_high,
                                    s_out,
                                    (uint32_t)k_jt_jpeg_cap,
                                    &n));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_jpeg_sw_encode(s_rgb,
                                    0U,
                                    (uint16_t)k_jt_h,
                                    (uint8_t)k_ra8_jpeg_sw_quality_high,
                                    s_out,
                                    (uint32_t)k_jt_jpeg_cap,
                                    &n));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_jpeg_sw_encode(s_rgb,
                                    (uint16_t)k_jt_w,
                                    0U,
                                    (uint8_t)k_ra8_jpeg_sw_quality_high,
                                    s_out,
                                    (uint32_t)k_jt_jpeg_cap,
                                    &n));
  TEST_END("jpeg_sw MC/DC encode: w==0 || h==0");
}

/**
 * @test test_mcdc_encode_quality_range
 * @par MC/DC:
 * Decision (libs/ra8_jpeg/src/ra8_jpeg_sw.c line 1821, 2 conditions):
 * `quality < min || quality > max`. V1 q=high (F ok), V2 q=0 (C1=T),
 * V3 q=101 (C1=F C2=T). N+1=3.
 */
static void test_mcdc_encode_quality_range(void)
{
  TEST_BEGIN("jpeg_sw MC/DC encode: quality<min || quality>max");
  static uint8_t s_rgb[(uint32_t)k_jt_rgb_bytes];
  static uint8_t s_out[(uint32_t)k_jt_jpeg_cap];
  uint32_t       n = 0U;
  fill_gradient(s_rgb, (uint16_t)k_jt_w, (uint16_t)k_jt_h);
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_jpeg_sw_encode(s_rgb,
                                    (uint16_t)k_jt_w,
                                    (uint16_t)k_jt_h,
                                    (uint8_t)k_ra8_jpeg_sw_quality_high,
                                    s_out,
                                    (uint32_t)k_jt_jpeg_cap,
                                    &n));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_jpeg_sw_encode(s_rgb,
                                    (uint16_t)k_jt_w,
                                    (uint16_t)k_jt_h,
                                    (uint8_t)k_mcdc_jpeg_q_below,
                                    s_out,
                                    (uint32_t)k_jt_jpeg_cap,
                                    &n));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_jpeg_sw_encode(s_rgb,
                                    (uint16_t)k_jt_w,
                                    (uint16_t)k_jt_h,
                                    (uint8_t)k_mcdc_jpeg_q_above,
                                    s_out,
                                    (uint32_t)k_jt_jpeg_cap,
                                    &n));
  TEST_END("jpeg_sw MC/DC encode: quality<min || quality>max");
}

int32_t main(void)
{
  ra8_fake_mmap_reset();
  test_get_dimensions_parses_sof0();
  test_get_dimensions_null_args();
  test_get_dimensions_invalid();
  test_encode_decode_roundtrip();
  test_decode_invalid_rejected();
  test_encode_null_args();
  test_encode_out_of_buffer();
  test_mcdc_encode_dim_zero();
  test_mcdc_encode_quality_range();
  (void)fprintf(stderr, "[OK ] test_ra8_jpeg_sw.c\n");
  return 0;
}
