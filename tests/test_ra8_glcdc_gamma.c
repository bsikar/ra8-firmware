/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file test_ra8_glcdc_gamma.c
 * @brief Unit tests for the GLCDC gamma correction API.
 *
 * @details
 * Exercises `ra8_glcdc_set_gamma` (per-channel LUT + AREA register writes)
 * and `ra8_glcdc_gamma_enable` (GAMSW.GAMON bit).  Every register write is
 * observed through the fake MMIO window provided by `ra8_fake_mmap`.
 */

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_glcdc.h"
#include "ra8_glcdc_regs.h"
#include "unity_minimal.h"

/* =============================================================================
 * Test-local constants
 * =============================================================================
 */

/**
 * @enum test_gamma_ch_t
 * @brief Channel selector values used in the tests.
 */
typedef enum : uint8_t {
  k_test_ch_r   = 0U, /**< Red channel (maps to GAM[0]).   */
  k_test_ch_g   = 1U, /**< Green channel (maps to GAM[1]). */
  k_test_ch_b   = 2U, /**< Blue channel (maps to GAM[2]).  */
  k_test_ch_bad = 3U, /**< One past last valid channel.    */
} test_gamma_ch_t;

/**
 * @enum test_gamma_count_t
 * @brief Count values for set_gamma calls.
 */
typedef enum : uint8_t {
  k_test_count_valid = 16U, /**< Valid LUT depth (k_ra8_glcdc_gamma_lut_depth). */
  k_test_count_zero  = 0U,  /**< Invalid: empty table.                          */
  k_test_count_short = 15U, /**< Invalid: one entry short.                      */
  k_test_count_over  = 17U, /**< Invalid: one entry over.                       */
} test_gamma_count_t;

/**
 * @enum test_gamma_const_t
 * @brief Expected register values and loop bounds for the happy-path checks.
 *
 * @details
 * Fixture gain table: even entries = 0x200, odd entries = 0x1FF.
 * Packed LUT = (0x200 << 16) | 0x1FF = 0x020001FF.
 *
 * Fixture threshold table: even entries = 0x040, odd entries = 0x080.
 * Packed AREA = (0x040 << 16) | 0x080 = 0x00400080.
 */
typedef enum : uint32_t {
  k_test_lut_packed     = 0x020001FFUL, /**< Expected packed LUT register value.   */
  k_test_area_packed    = 0x00400080UL, /**< Expected packed AREA register value.  */
  k_test_gamsw_on       = 0x00000001UL, /**< GAMSW with GAMON=1.                   */
  k_test_gamsw_off      = 0x00000000UL, /**< GAMSW with GAMON=0.                   */
  k_test_lut_reg_count  = 0x00000008UL, /**< LUT registers per channel (depth/2).  */
  k_test_area_reg_count = 0x00000008UL, /**< AREA registers per channel (depth/2). */
} test_gamma_const_t;

/* =============================================================================
 * Test fixtures
 * =============================================================================
 */

/**
 * @var k_gains
 * @brief Gain fixture: even entries = 0x200, odd entries = 0x1FF.
 *
 * @details
 * k_ra8_glcdc_gamma_lut_depth = 16 values (two 11-bit gains per register,
 * packed into each GAMx_LUTn word). Expected packed word: k_test_lut_packed.
 */
static const uint16_t k_gains[16] = {
  0x200U,
  0x1FFU,
  0x200U,
  0x1FFU,
  0x200U,
  0x1FFU,
  0x200U,
  0x1FFU,
  0x200U,
  0x1FFU,
  0x200U,
  0x1FFU,
  0x200U,
  0x1FFU,
  0x200U,
  0x1FFU,
};

/**
 * @var k_thresholds
 * @brief Threshold fixture: even entries = 0x040, odd entries = 0x080.
 *
 * @details
 * k_ra8_glcdc_gamma_lut_depth = 16 values (two 10-bit thresholds per register,
 * packed into each GAMx_AREAn word). Expected packed word: k_test_area_packed.
 */
static const uint16_t k_thresholds[16] = {
  0x040U,
  0x080U,
  0x040U,
  0x080U,
  0x040U,
  0x080U,
  0x040U,
  0x080U,
  0x040U,
  0x080U,
  0x040U,
  0x080U,
  0x040U,
  0x080U,
  0x040U,
  0x080U,
};

/**
 * @brief Reset MMIO and MSTP model before each test.
 */
static void prep(void)
{
  ra8_fake_mmap_reset();
}

/* =============================================================================
 * Happy-path tests: set_gamma per channel
 * =============================================================================
 */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- single happy path for R channel)
 */
static void test_set_gamma_r_writes_lut_and_area(void)
{
  TEST_BEGIN("glcdc set_gamma R: LUT and AREA registers written");
  prep();

  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_glcdc_set_gamma(k_ra8_glcdc_ch_r, k_gains, k_thresholds, (uint8_t)k_test_count_valid));

  volatile ra8_glcdc_gam_t* gam = ra8_glcdc_gam_regs((uint8_t)k_test_ch_r);
  TEST_ASSERT_NOT_NULL((void*)gam);

  /* Verify all 8 LUT registers got the expected packed value. */
  for (uint32_t i = 0U; i < (uint32_t)k_test_lut_reg_count; i++) {
    TEST_ASSERT_EQ(k_test_lut_packed, gam->LUT[i]);
  }

  /* Verify all 8 AREA registers got the expected packed value. */
  for (uint32_t i = 0U; i < (uint32_t)k_test_area_reg_count; i++) {
    TEST_ASSERT_EQ(k_test_area_packed, gam->AREA[i]);
  }

  TEST_END("glcdc set_gamma R: LUT and AREA registers written");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- single happy path for G channel)
 */
static void test_set_gamma_g_writes_lut_and_area(void)
{
  TEST_BEGIN("glcdc set_gamma G: LUT and AREA registers written");
  prep();

  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_glcdc_set_gamma(k_ra8_glcdc_ch_g, k_gains, k_thresholds, (uint8_t)k_test_count_valid));

  volatile ra8_glcdc_gam_t* gam = ra8_glcdc_gam_regs((uint8_t)k_test_ch_g);
  TEST_ASSERT_NOT_NULL((void*)gam);

  for (uint32_t i = 0U; i < (uint32_t)k_test_lut_reg_count; i++) {
    TEST_ASSERT_EQ(k_test_lut_packed, gam->LUT[i]);
  }
  for (uint32_t i = 0U; i < (uint32_t)k_test_area_reg_count; i++) {
    TEST_ASSERT_EQ(k_test_area_packed, gam->AREA[i]);
  }

  TEST_END("glcdc set_gamma G: LUT and AREA registers written");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- single happy path for B channel)
 */
static void test_set_gamma_b_writes_lut_and_area(void)
{
  TEST_BEGIN("glcdc set_gamma B: LUT and AREA registers written");
  prep();

  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_glcdc_set_gamma(k_ra8_glcdc_ch_b, k_gains, k_thresholds, (uint8_t)k_test_count_valid));

  volatile ra8_glcdc_gam_t* gam = ra8_glcdc_gam_regs((uint8_t)k_test_ch_b);
  TEST_ASSERT_NOT_NULL((void*)gam);

  for (uint32_t i = 0U; i < (uint32_t)k_test_lut_reg_count; i++) {
    TEST_ASSERT_EQ(k_test_lut_packed, gam->LUT[i]);
  }
  for (uint32_t i = 0U; i < (uint32_t)k_test_area_reg_count; i++) {
    TEST_ASSERT_EQ(k_test_area_packed, gam->AREA[i]);
  }

  TEST_END("glcdc set_gamma B: LUT and AREA registers written");
}

/**
 * @par MC/DC:
 * (no compound decisions -- channels do not alias; verifies R != G != B)
 */
static void test_set_gamma_channels_do_not_alias(void)
{
  TEST_BEGIN("glcdc set_gamma: R/G/B channels are independent");
  prep();

  /* Write only R; G and B must remain zero. */
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_glcdc_set_gamma(k_ra8_glcdc_ch_r, k_gains, k_thresholds, (uint8_t)k_test_count_valid));

  volatile ra8_glcdc_gam_t* g_gam = ra8_glcdc_gam_regs((uint8_t)k_test_ch_g);
  volatile ra8_glcdc_gam_t* b_gam = ra8_glcdc_gam_regs((uint8_t)k_test_ch_b);
  TEST_ASSERT_NOT_NULL((void*)g_gam);
  TEST_ASSERT_NOT_NULL((void*)b_gam);
  TEST_ASSERT_EQ(0U, g_gam->LUT[0]);
  TEST_ASSERT_EQ(0U, b_gam->LUT[0]);

  TEST_END("glcdc set_gamma: R/G/B channels are independent");
}

/* =============================================================================
 * Happy-path tests: gamma_enable
 * =============================================================================
 */

/**
 * @par MC/DC:
 * (no compound decisions -- enable path sets GAMON=1)
 */
static void test_gamma_enable_sets_gamon(void)
{
  TEST_BEGIN("glcdc gamma_enable true: GAMSW.GAMON=1");
  prep();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_glcdc_gamma_enable(true));

  volatile uint32_t* gamsw = ra8_glcdc_reg32(k_ra8_glcdc_off_out_gamsw);
  TEST_ASSERT_NOT_NULL((void*)gamsw);
  TEST_ASSERT_EQ(k_test_gamsw_on, *gamsw);

  TEST_END("glcdc gamma_enable true: GAMSW.GAMON=1");
}

/**
 * @par MC/DC:
 * (no compound decisions -- disable path clears GAMON=0)
 */
static void test_gamma_disable_clears_gamon(void)
{
  TEST_BEGIN("glcdc gamma_enable false: GAMSW.GAMON=0");
  prep();

  /* First enable, then disable. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_glcdc_gamma_enable(true));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_glcdc_gamma_enable(false));

  volatile uint32_t* gamsw = ra8_glcdc_reg32(k_ra8_glcdc_off_out_gamsw);
  TEST_ASSERT_EQ(k_test_gamsw_off, *gamsw);

  TEST_END("glcdc gamma_enable false: GAMSW.GAMON=0");
}

/* =============================================================================
 * Validation tests: null pointers and bad arguments
 * =============================================================================
 */

/**
 * @par MC/DC:
 * (no compound decisions -- each guard tested individually)
 */
static void test_set_gamma_null_gain_rejected(void)
{
  TEST_BEGIN("glcdc set_gamma: null gain -> k_ra8_err_null_ptr");
  prep();

  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    ra8_glcdc_set_gamma(k_ra8_glcdc_ch_r, nullptr, k_thresholds, (uint8_t)k_test_count_valid));

  TEST_END("glcdc set_gamma: null gain -> k_ra8_err_null_ptr");
}

/**
 * @par MC/DC:
 * (no compound decisions -- each guard tested individually)
 */
static void test_set_gamma_null_threshold_rejected(void)
{
  TEST_BEGIN("glcdc set_gamma: null threshold -> k_ra8_err_null_ptr");
  prep();

  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    ra8_glcdc_set_gamma(k_ra8_glcdc_ch_r, k_gains, nullptr, (uint8_t)k_test_count_valid));

  TEST_END("glcdc set_gamma: null threshold -> k_ra8_err_null_ptr");
}

/**
 * @par MC/DC:
 * (no compound decisions -- each guard tested individually)
 */
static void test_set_gamma_bad_channel_rejected(void)
{
  TEST_BEGIN("glcdc set_gamma: channel >= count -> k_ra8_err_invalid_arg");
  prep();

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_glcdc_set_gamma((ra8_glcdc_color_channel_t)k_test_ch_bad,
                                     k_gains,
                                     k_thresholds,
                                     (uint8_t)k_test_count_valid));

  TEST_END("glcdc set_gamma: channel >= count -> k_ra8_err_invalid_arg");
}

/**
 * @test test_mcdc_set_gamma_count_guard
 *
 * @par MC/DC:
 * Decision: `if (count != (uint8_t)k_ra8_glcdc_gamma_lut_depth)` (1 condition)
 * - Vector 1: count = 16 (valid)  -> false -> ok.
 * - Vector 2: count = 0  (zero)   -> true  -> k_ra8_err_invalid_arg.
 * - Vector 3: count = 15 (short)  -> true  -> k_ra8_err_invalid_arg.
 * - Vector 4: count = 17 (over)   -> true  -> k_ra8_err_invalid_arg.
 * The single condition independently determines the outcome.
 * N+1 = 2 vectors minimum; all four are provided for coverage breadth.
 */
static void test_mcdc_set_gamma_count_guard(void)
{
  TEST_BEGIN("glcdc set_gamma MC/DC: count != lut_depth guard");
  prep();

  /* Vector 1: exact depth -> ok. */
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_glcdc_set_gamma(k_ra8_glcdc_ch_r, k_gains, k_thresholds, (uint8_t)k_test_count_valid));

  /* Vector 2: count == 0 -> invalid_arg. */
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_glcdc_set_gamma(k_ra8_glcdc_ch_r, k_gains, k_thresholds, (uint8_t)k_test_count_zero));

  /* Vector 3: count == 15 -> invalid_arg. */
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_glcdc_set_gamma(k_ra8_glcdc_ch_r, k_gains, k_thresholds, (uint8_t)k_test_count_short));

  /* Vector 4: count == 17 -> invalid_arg. */
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_glcdc_set_gamma(k_ra8_glcdc_ch_r, k_gains, k_thresholds, (uint8_t)k_test_count_over));

  TEST_END("glcdc set_gamma MC/DC: count != lut_depth guard");
}

/* =============================================================================
 * Main
 * =============================================================================
 */

int32_t main(void)
{
  test_set_gamma_r_writes_lut_and_area();
  test_set_gamma_g_writes_lut_and_area();
  test_set_gamma_b_writes_lut_and_area();
  test_set_gamma_channels_do_not_alias();
  test_gamma_enable_sets_gamon();
  test_gamma_disable_clears_gamon();
  test_set_gamma_null_gain_rejected();
  test_set_gamma_null_threshold_rejected();
  test_set_gamma_bad_channel_rejected();
  test_mcdc_set_gamma_count_guard();
  (void)fprintf(stderr, "[OK ] test_ra8_glcdc_gamma.c\n");
  return 0;
}
