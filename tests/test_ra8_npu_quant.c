/**
 * @file test_ra8_npu_quant.c
 * @brief Unit tests for ra8_npu_quant.c (affine INT8 / UINT8 tensor quantization)
 *
 * @details
 * `ra8_npu_quant.c` is device-agnostic pure math (NOT behind `RA8_HAS_NPU`), so
 * it is compiled into the shared `ra8_core_hal` object library for the default
 * device and this test links it through the normal auto-glob. The tests pin the
 * affine mapping (round-half-away-from-zero, zero-point offset, saturation, and
 * the pre-cast magnitude clamp) plus the argument-validation guards, and confirm
 * a quantize -> dequantize round trip is exact for integer-representable inputs.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_err.h"
#include "ra8_npu_quant.h"
#include "unity_minimal.h"

/**
 * @enum npu_quant_uint8_const_t
 * @brief Named uint8_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint8_t {
  k_npu_quant_o8_42  = 42,
  k_npu_quant_ou8_42 = 42U,
  k_npu_quant_val_10 = 10,
} npu_quant_uint8_const_t;

/** @brief Named float constant used by this file. */
static const float k_npu_quant_f_7p0 = 7.0F;

/** @brief Named float constant used by this file. */
static const float k_npu_quant_ra8_npu_quantize_u8_0p5 = 0.5F;

/** @brief Named float constant used by this file. */
static const float k_npu_quant_val_0p25 = 0.25F;

/** @brief Named float constant used by this file. */
static const float k_npu_quant_val_1000p0 = 1000.0F;

/** @brief Named float constant used by this file. */
static const float k_npu_quant_val_10p0 = 10.0F;

/** @brief Named float constant used by this file. */
static const float k_npu_quant_val_5p0e6 = 5.0e6F;

/**
 * @enum ra8_npu_quant_test_const_t
 * @brief Fixture sizes and quantization parameters for the quant tests.
 */
typedef enum : int32_t {
  k_test_q_count = 8,  /**< Elements per fixture arena.                  */
  k_test_q_zp_i8 = -8, /**< INT8 zero-point (exercises a signed offset). */
  k_test_q_zp_u8 = 16, /**< UINT8 zero-point.                            */
} ra8_npu_quant_test_const_t;

/** @brief Absolute tolerance for the dequantized float comparisons. */
static const float s_q_tol = 0.0001F;

/** @brief Quantization scale shared by the fixtures. */
static const float s_q_scale = 0.5F;

/** @brief |a - b| <= tol without pulling in libm's fabsf. */
static bool q_close(float a, float b)
{
  const float d = (a > b) ? (a - b) : (b - a);
  return d <= s_q_tol;
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- each RA8_CHECK_NULL_PTR guard and the
 * `scale <= 0` guard in the code under test is a single condition, exercised one
 * at a time)
 */
static void test_quantize_rejects_bad_args(void)
{
  TEST_BEGIN("npu quant rejects bad args");
  float   in[k_test_q_count]  = {};
  int8_t  o8[k_test_q_count]  = {};
  uint8_t ou8[k_test_q_count] = {};

  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_npu_quantize_i8(nullptr, o8, (size_t)k_test_q_count, s_q_scale, 0));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_npu_quantize_i8(in, nullptr, (size_t)k_test_q_count, s_q_scale, 0));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_npu_quantize_i8(in, o8, (size_t)k_test_q_count, 0.0F, 0));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_npu_quantize_i8(in, o8, (size_t)k_test_q_count, -1.0F, 0));

  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_npu_quantize_u8(nullptr, ou8, (size_t)k_test_q_count, s_q_scale, 0));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_npu_quantize_u8(in, nullptr, (size_t)k_test_q_count, s_q_scale, 0));
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_npu_quantize_u8(in, ou8, (size_t)k_test_q_count, -k_npu_quant_ra8_npu_quantize_u8_0p5, 0));

  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_npu_dequantize_i8(nullptr, in, (size_t)k_test_q_count, s_q_scale, 0));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_npu_dequantize_i8(o8, nullptr, (size_t)k_test_q_count, s_q_scale, 0));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_npu_dequantize_u8(nullptr, in, (size_t)k_test_q_count, s_q_scale, 0));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_npu_dequantize_u8(ou8, nullptr, (size_t)k_test_q_count, s_q_scale, 0));
  TEST_END("npu quant rejects bad args");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- it exercises the round + zero-point path
 * of the single-condition clamp helper with integer-exact inputs)
 */
static void test_quantize_i8_affine(void)
{
  TEST_BEGIN("npu quant i8 affine round + zero-point");
  /* in[i] = scale * i so round(in/scale) = i exactly; +zp gives i + zp. */
  float  in[k_test_q_count]  = {};
  int8_t out[k_test_q_count] = {};
  for (int32_t i = 0; i < k_test_q_count; i++) {
    in[i] = s_q_scale * (float)i;
  }
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_npu_quantize_i8(in, out, (size_t)k_test_q_count, s_q_scale, k_test_q_zp_i8));
  for (int32_t i = 0; i < k_test_q_count; i++) {
    TEST_ASSERT_EQ((i + k_test_q_zp_i8), out[i]);
  }
  /* Half-away-from-zero rounding: 0.25/0.5 = 0.5 -> 1; -0.25/0.5 = -0.5 -> -1. */
  float  hin[2]  = {k_npu_quant_val_0p25, -k_npu_quant_val_0p25};
  int8_t hout[2] = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_npu_quantize_i8(hin, hout, (size_t)2, s_q_scale, 0));
  TEST_ASSERT_EQ(1, hout[0]);
  TEST_ASSERT_EQ(-1, hout[1]);
  TEST_END("npu quant i8 affine round + zero-point");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- it drives the four independent clamp
 * arms of the helper: q<qmin, q>qmax, rounded<-cap, rounded>cap)
 */
static void test_quantize_saturates(void)
{
  TEST_BEGIN("npu quant saturates + clamps huge magnitudes");
  int8_t  o8[4]  = {};
  uint8_t ou8[4] = {};

  /* INT8: below -128 and above +127 saturate; the cap arms are hit by |in| that
   * makes round(in/scale) exceed 1e6. */
  float i8in[4] = {-k_npu_quant_val_1000p0,
                   k_npu_quant_val_1000p0,
                   -k_npu_quant_val_5p0e6,
                   k_npu_quant_val_5p0e6};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_npu_quantize_i8(i8in, o8, (size_t)4, s_q_scale, 0));
  TEST_ASSERT_EQ(k_ra8_npu_quant_i8_min, o8[0]);
  TEST_ASSERT_EQ(k_ra8_npu_quant_i8_max, o8[1]);
  TEST_ASSERT_EQ(k_ra8_npu_quant_i8_min, o8[2]);
  TEST_ASSERT_EQ(k_ra8_npu_quant_i8_max, o8[3]);

  /* UINT8: below 0 and above 255 saturate. */
  float u8in[4] = {-k_npu_quant_val_10p0,
                   k_npu_quant_val_1000p0,
                   -k_npu_quant_val_5p0e6,
                   k_npu_quant_val_5p0e6};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_npu_quantize_u8(u8in, ou8, (size_t)4, s_q_scale, 0));
  TEST_ASSERT_EQ(k_ra8_npu_quant_u8_min, ou8[0]);
  TEST_ASSERT_EQ(k_ra8_npu_quant_u8_max, ou8[1]);
  TEST_ASSERT_EQ(k_ra8_npu_quant_u8_min, ou8[2]);
  TEST_ASSERT_EQ(k_ra8_npu_quant_u8_max, ou8[3]);
  TEST_END("npu quant saturates + clamps huge magnitudes");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- dequantize is a straight affine map and
 * the round trip pins quantize + dequantize consistency)
 */
static void test_dequantize_round_trip(void)
{
  TEST_BEGIN("npu quant/dequant round trip is exact");
  float   in[k_test_q_count]   = {};
  uint8_t q[k_test_q_count]    = {};
  float   back[k_test_q_count] = {};
  for (int32_t i = 0; i < k_test_q_count; i++) {
    in[i] = s_q_scale * (float)i; /* integer-exact in this scale. */
  }
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_npu_quantize_u8(in, q, (size_t)k_test_q_count, s_q_scale, k_test_q_zp_u8));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_npu_dequantize_u8(q, back, (size_t)k_test_q_count, s_q_scale, k_test_q_zp_u8));
  for (int32_t i = 0; i < k_test_q_count; i++) {
    TEST_ASSERT(q_close(in[i], back[i]));
  }

  /* INT8 dequantize spot-check: scale*(v - zp). */
  int8_t iv[2] = {k_npu_quant_val_10, -4};
  float  fv[2] = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_npu_dequantize_i8(iv, fv, (size_t)2, s_q_scale, k_test_q_zp_i8));
  TEST_ASSERT(q_close(s_q_scale * (float)(10 - k_test_q_zp_i8), fv[0]));
  TEST_ASSERT(q_close(s_q_scale * (float)(-4 - k_test_q_zp_i8), fv[1]));
  TEST_END("npu quant/dequant round trip is exact");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- a zero-count call is a valid no-op that
 * skips the conversion loop and still returns k_ra8_ok)
 */
static void test_zero_count_is_noop(void)
{
  TEST_BEGIN("npu quant zero count is a no-op");
  float   in  = 1.0F;
  int8_t  o8  = k_npu_quant_o8_42;
  uint8_t ou8 = k_npu_quant_ou8_42;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_npu_quantize_i8(&in, &o8, (size_t)0, s_q_scale, 0));
  TEST_ASSERT_EQ(42, o8); /* untouched */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_npu_quantize_u8(&in, &ou8, (size_t)0, s_q_scale, 0));
  TEST_ASSERT_EQ(42, ou8);
  float f = k_npu_quant_f_7p0;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_npu_dequantize_i8(&o8, &f, (size_t)0, s_q_scale, 0));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_npu_dequantize_u8(&ou8, &f, (size_t)0, s_q_scale, 0));
  TEST_END("npu quant zero count is a no-op");
}

int32_t main(void)
{
  test_quantize_rejects_bad_args();
  test_quantize_i8_affine();
  test_quantize_saturates();
  test_dequantize_round_trip();
  test_zero_count_is_noop();
  (void)fprintf(stderr, "[OK ] test_ra8_npu_quant.c\n");
  return 0;
}
