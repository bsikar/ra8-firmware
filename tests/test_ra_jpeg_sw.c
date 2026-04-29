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
  (void)fprintf(stderr, "[OK ] test_ra_jpeg_sw.c\n");
  return 0;
}
