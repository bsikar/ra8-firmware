/**
 * @file test_ra8_gfx_tone.c
 * @brief Unit tests for ra8_gfx_tone -- per-panel gray-level tone LUT (#479).
 *
 * @details
 * The tone curve exists so the dither can quantise against a panel's real
 * 16-level response instead of an assumed even palette. These tests pin the
 * three things that makes safe: the curve contract (::ra8_gfx_tone_lut_validate
 * rejects every way a stored or bench-supplied curve can be unusable), the
 * preparation arithmetic (::ra8_gfx_tone_prepare brackets each of the 256
 * samples on the right interval, stays in range, and never manufactures a
 * seventeenth level), and the equivalence that keeps the committed dither
 * goldens valid -- prepared from ::k_ra8_gfx_tone_lut_nominal, the curve
 * reproduces ra8_gfx_dither.c's closed-form rule for all 256 x 256 (sample,
 * threshold) pairs, and the NULL-map dither entry points are byte-identical to
 * the nominal ones. A deliberately uneven curve then proves the seam actually
 * does something: it moves levels, and it keeps the flat-field tone unbiased
 * against its own interval widths.
 *
 * Nothing here is a measurement. No panel has been on a bench, so no measured
 * curve is committed and none is inferred; the uneven curve below is a fixture.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_gfx.h"
#include "ra8_gfx_dither.h"
#include "ra8_gfx_tone.h"
#include "unity_minimal.h"

/**
 * @enum tone_test_dim_t
 * @brief Fixture geometry and palette constants for the tone tests.
 */
typedef enum : uint16_t {
  k_tt_domain    = 256, /**< gray8 samples a prepared map covers.    */
  k_tt_levels    = 16,  /**< Panel levels (4 bpp).                   */
  k_tt_max_level = 15,  /**< Maximum 4-bit level.                    */
  k_tt_step      = 17,  /**< Nominal palette step (255/15).          */
  k_tt_tile_w    = 16,  /**< Tile-pack fixture width in pixels.      */
  k_tt_tile_h    = 8,   /**< Tile-pack fixture height in pixels.     */
  k_tt_fb_w      = 32,  /**< Blit fixture framebuffer width.         */
  k_tt_fb_h      = 8,   /**< Blit fixture framebuffer height.        */
  k_tt_argb      = 4,   /**< ARGB8888 bytes per pixel.               */
  k_tt_flat_edge = 64,  /**< Flat-field edge (one full mask period). */
} tone_test_dim_t;

/** @brief Prepared map under test (nominal curve). */
static ra8_gfx_tone_map_t s_tt_nominal;

/** @brief Prepared map under test (uneven fixture curve). */
static ra8_gfx_tone_map_t s_tt_uneven;

/** @brief Row-major gray8 source scratch. */
static uint8_t s_tt_src[k_tt_flat_edge * k_tt_flat_edge];

/** @brief Packed-gray4 output scratch for the nominal path. */
static uint8_t s_tt_pack_a[(k_tt_tile_w * k_tt_tile_h) / 2U];

/** @brief Packed-gray4 output scratch for the tone path. */
static uint8_t s_tt_pack_b[(k_tt_tile_w * k_tt_tile_h) / 2U];

/** @brief ARGB8888 framebuffer for the nominal blit. */
static uint8_t s_tt_fb_a[k_tt_fb_w * k_tt_fb_h * k_tt_argb];

/** @brief ARGB8888 framebuffer for the NULL-map tone blit. */
static uint8_t s_tt_fb_b[k_tt_fb_w * k_tt_fb_h * k_tt_argb];

/**
 * @brief A deliberately uneven 16-knot curve (contract-valid, not a measurement).
 * @details Crushed shadows and a compressed highlight shoulder, the shape a real
 *          e-ink response is expected to have. Strictly increasing, 0 and 255 at
 *          the ends, and no interval equal to the nominal 17 anywhere in the
 *          middle, so any test it passes cannot be passing by accident.
 */
static const ra8_gfx_tone_lut_t k_tt_uneven = {
  .level_gray8 = {0, 6, 14, 25, 40, 58, 80, 104, 128, 152, 175, 197, 216, 232, 245, 255},
};

/** @brief The closed-form nominal rule ra8_gfx_dither.c implements. */
static uint8_t legacy_quantise(uint8_t gray8, uint8_t thr)
{
  const uint8_t base = (uint8_t)(gray8 / (uint8_t)k_tt_step);
  const uint8_t rem  = (uint8_t)(gray8 - (uint8_t)(base * (uint8_t)k_tt_step));
  uint8_t       out  = base;
  if (((uint32_t)thr * (uint32_t)k_tt_step) < ((uint32_t)rem * 256U)) {
    out = (uint8_t)(out + 1U);
  }
  return out;
}

/**
 * @test ra8_gfx_tone_lut_validate contract
 *
 * @details Accepts the committed nominal curve and the uneven fixture; rejects a
 *          NULL pointer, a non-zero black knot, a non-255 white knot, a repeated
 *          knot (zero-width interval) and a descending knot. Every rejection is
 *          a distinct way a stored record could be unusable, so each is asserted
 *          on its own rather than through one malformed blob.
 */
static void test_validate_contract(void)
{
  TEST_BEGIN("tone curve contract");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gfx_tone_lut_validate(&k_ra8_gfx_tone_lut_nominal));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gfx_tone_lut_validate(&k_tt_uneven));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_gfx_tone_lut_validate(nullptr));

  ra8_gfx_tone_lut_t bad = k_ra8_gfx_tone_lut_nominal;
  bad.level_gray8[0]     = 1U;
  TEST_ASSERT_EQ(k_ra8_err_range_check_failed, ra8_gfx_tone_lut_validate(&bad));

  bad                             = k_ra8_gfx_tone_lut_nominal;
  bad.level_gray8[k_tt_max_level] = 254U;
  TEST_ASSERT_EQ(k_ra8_err_range_check_failed, ra8_gfx_tone_lut_validate(&bad));

  bad                = k_ra8_gfx_tone_lut_nominal;
  bad.level_gray8[5] = bad.level_gray8[4];
  TEST_ASSERT_EQ(k_ra8_err_range_check_failed, ra8_gfx_tone_lut_validate(&bad));

  bad                = k_ra8_gfx_tone_lut_nominal;
  bad.level_gray8[9] = (uint8_t)(bad.level_gray8[8] - 1U);
  TEST_ASSERT_EQ(k_ra8_err_range_check_failed, ra8_gfx_tone_lut_validate(&bad));
  TEST_END("tone curve contract");
}

/**
 * @test ra8_gfx_tone_prepare argument and validation guards
 *
 * @details A NULL destination, a NULL curve and an out-of-contract curve each
 *          fail, and the failing curve leaves the destination untouched -- the
 *          post-condition a caller holding a `static` map depends on.
 */
static void test_prepare_guards(void)
{
  TEST_BEGIN("tone prepare guards");
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_gfx_tone_prepare(&k_ra8_gfx_tone_lut_nominal, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_gfx_tone_prepare(nullptr, &s_tt_nominal));

  ra8_gfx_tone_map_t scratch;
  (void)memset(&scratch, 0xA5, sizeof(scratch));
  ra8_gfx_tone_lut_t bad = k_ra8_gfx_tone_lut_nominal;
  bad.level_gray8[3]     = bad.level_gray8[2];
  TEST_ASSERT_EQ(k_ra8_err_range_check_failed, ra8_gfx_tone_prepare(&bad, &scratch));
  TEST_ASSERT_EQ(0xA5U, scratch.level[0]);
  TEST_ASSERT_EQ(0xA5U, scratch.level[k_tt_domain - 1U]);
  TEST_END("tone prepare guards");
}

/**
 * @test Prepared-map invariants over the whole domain, for both curves
 *
 * @details For all 256 samples and both curves: the base level is in range and
 *          non-decreasing, the round-up threshold never exceeds the texture
 *          depth, the bracketing knot really is at or below the sample and the
 *          next knot above it, white pins to level 15 with no round-up (so no
 *          seventeenth level exists), and black pins to level 0.
 */
static void test_prepared_invariants(void)
{
  TEST_BEGIN("prepared map invariants");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gfx_tone_prepare(&k_ra8_gfx_tone_lut_nominal, &s_tt_nominal));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gfx_tone_prepare(&k_tt_uneven, &s_tt_uneven));

  const ra8_gfx_tone_map_t* const maps[2] = {&s_tt_nominal, &s_tt_uneven};
  const ra8_gfx_tone_lut_t* const luts[2] = {&k_ra8_gfx_tone_lut_nominal, &k_tt_uneven};
  for (uint32_t m = 0U; m < 2U; ++m) {
    const ra8_gfx_tone_map_t* const map = maps[m];
    const ra8_gfx_tone_lut_t* const lut = luts[m];
    TEST_ASSERT_EQ(0U, map->level[0]);
    TEST_ASSERT_EQ(k_tt_max_level, map->level[k_tt_domain - 1U]);
    TEST_ASSERT_EQ(0U, map->up_threshold[k_tt_domain - 1U]);
    for (uint32_t v = 0U; v < (uint32_t)k_tt_domain; ++v) {
      const uint8_t n = map->level[v];
      TEST_ASSERT(n <= (uint8_t)k_tt_max_level);
      TEST_ASSERT(map->up_threshold[v] <= 256U);
      TEST_ASSERT((uint32_t)lut->level_gray8[n] <= v);
      if (n < (uint8_t)k_tt_max_level) {
        TEST_ASSERT(v < (uint32_t)lut->level_gray8[n + 1U]);
      }
      if (v > 0U) {
        TEST_ASSERT(map->level[v - 1U] <= n);
      }
      /* Highest threshold plus highest base can never exceed the last level. */
      TEST_ASSERT((uint32_t)ra8_gfx_tone_quantise(map, (uint8_t)v, 0U) <=
                       (uint32_t)k_tt_max_level);
    }
  }
  TEST_END("prepared map invariants");
}

/**
 * @test The nominal curve reproduces the closed-form rule exactly
 *
 * @details The load-bearing assertion of this file: over the full 256 x 256
 *          (sample, threshold) product, quantising through the prepared nominal
 *          curve equals ra8_gfx_dither.c's `thr * 17 < rem * 256` rule. That is
 *          why adding this seam cannot move a committed dither golden.
 */
static void test_nominal_equals_closed_form(void)
{
  TEST_BEGIN("nominal curve == closed-form rule");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gfx_tone_prepare(&k_ra8_gfx_tone_lut_nominal, &s_tt_nominal));
  for (uint32_t v = 0U; v < (uint32_t)k_tt_domain; ++v) {
    for (uint32_t t = 0U; t < (uint32_t)k_tt_domain; ++t) {
      TEST_ASSERT_EQ(legacy_quantise((uint8_t)v, (uint8_t)t),
                     ra8_gfx_tone_quantise(&s_tt_nominal, (uint8_t)v, (uint8_t)t));
    }
  }
  TEST_END("nominal curve == closed-form rule");
}

/**
 * @test A NULL map selects the nominal palette on every dither entry point
 *
 * @details Level, tile pack and framebuffer blit each agree byte for byte with
 *          their nominal siblings when the map is NULL, so the tone variants can
 *          be adopted incrementally without a render change.
 */
static void test_null_map_matches_nominal(void)
{
  TEST_BEGIN("NULL tone map == nominal dither");
  for (uint32_t v = 0U; v < (uint32_t)k_tt_domain; ++v) {
    TEST_ASSERT_EQ(ra8_gfx_dither_gray4_level((uint8_t)v, 3, 5),
                   ra8_gfx_dither_gray4_level_tone(nullptr, (uint8_t)v, 3, 5));
  }

  for (uint32_t i = 0U; i < (uint32_t)(k_tt_tile_w * k_tt_tile_h); ++i) {
    s_tt_src[i] = (uint8_t)(i * 3U);
  }
  uint32_t size_a = 0U;
  uint32_t size_b = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_gfx_dither_gray8_to_gray4(
                   s_tt_src, k_tt_tile_w, k_tt_tile_h, 7, 9, s_tt_pack_a,
                   (uint32_t)sizeof(s_tt_pack_a), &size_a));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_gfx_dither_gray8_to_gray4_tone(
                   nullptr, s_tt_src, k_tt_tile_w, k_tt_tile_h, 7, 9, s_tt_pack_b,
                   (uint32_t)sizeof(s_tt_pack_b), &size_b));
  TEST_ASSERT_EQ(size_a, size_b);
  TEST_ASSERT_EQ(0, memcmp(s_tt_pack_a, s_tt_pack_b, size_a));

  for (uint32_t i = 0U; i < (uint32_t)(k_tt_fb_w * k_tt_fb_h); ++i) {
    s_tt_src[i] = (uint8_t)((i * 7U) & 0xFFU);
  }
  (void)memset(s_tt_fb_a, 0, sizeof(s_tt_fb_a));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_gfx_init(s_tt_fb_a, k_tt_fb_w, k_tt_fb_h, k_ra8_gfx_format_argb8888));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gfx_blit_gray8_dither(s_tt_src, k_tt_fb_w, k_tt_fb_h, 0, 0));
  (void)memset(s_tt_fb_b, 0, sizeof(s_tt_fb_b));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_gfx_init(s_tt_fb_b, k_tt_fb_w, k_tt_fb_h, k_ra8_gfx_format_argb8888));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_gfx_blit_gray8_dither_tone(nullptr, s_tt_src, k_tt_fb_w, k_tt_fb_h, 0, 0));
  TEST_ASSERT_EQ(0, memcmp(s_tt_fb_a, s_tt_fb_b, sizeof(s_tt_fb_a)));
  TEST_END("NULL tone map == nominal dither");
}

/**
 * @test An uneven curve actually re-maps tone, and stays unbiased
 *
 * @details Two halves. First, the seam does something: at least one sample
 *          quantises to a different level under the uneven curve than under the
 *          nominal one, and the level the curve says surrounds a mid-grey is the
 *          level that comes out. Second, it is still unbiased on its own terms:
 *          over one full 64 x 64 mask period of a flat field, the mean
 *          reconstructed tone (taken from the curve's own knots, which is what
 *          the panel would emit) lands within one level-interval of the source.
 */
static void test_uneven_curve_remaps_and_stays_unbiased(void)
{
  TEST_BEGIN("uneven curve remaps tone, unbiased");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gfx_tone_prepare(&k_tt_uneven, &s_tt_uneven));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gfx_tone_prepare(&k_ra8_gfx_tone_lut_nominal, &s_tt_nominal));

  uint32_t differences = 0U;
  for (uint32_t v = 0U; v < (uint32_t)k_tt_domain; ++v) {
    if (s_tt_uneven.level[v] != s_tt_nominal.level[v]) {
      ++differences;
    }
  }
  TEST_ASSERT(differences > 0U);
  /* 40 is knot 4 of the uneven curve, so it must quantise there exactly. */
  TEST_ASSERT_EQ(4U, ra8_gfx_tone_quantise(&s_tt_uneven, 40U, 0U));
  TEST_ASSERT_EQ(4U, ra8_gfx_tone_quantise(&s_tt_uneven, 40U, 255U));

  const uint32_t flat  = 100U; /* between knots 6 (80) and 7 (104). */
  uint32_t       total = 0U;
  for (int32_t y = 0; y < (int32_t)k_tt_flat_edge; ++y) {
    for (int32_t x = 0; x < (int32_t)k_tt_flat_edge; ++x) {
      const uint8_t level = ra8_gfx_dither_gray4_level_tone(&s_tt_uneven, (uint8_t)flat, x, y);
      TEST_ASSERT(level <= (uint8_t)k_tt_max_level);
      total += (uint32_t)k_tt_uneven.level_gray8[level];
    }
  }
  const uint32_t mean = total / ((uint32_t)k_tt_flat_edge * (uint32_t)k_tt_flat_edge);
  TEST_ASSERT(mean >= (flat - 3U));
  TEST_ASSERT(mean <= (flat + 3U));
  TEST_END("uneven curve remaps tone, unbiased");
}

int main(void)
{
  test_validate_contract();
  test_prepare_guards();
  test_prepared_invariants();
  test_nominal_equals_closed_form();
  test_null_map_matches_nominal();
  test_uneven_curve_remaps_and_stays_unbiased();
  return 0;
}
