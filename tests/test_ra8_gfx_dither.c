/**
 * @file test_ra8_gfx_dither.c
 * @brief Unit tests for ra8_gfx_dither -- void-and-cluster blue-noise dithering (#477).
 *
 * @details
 * Exercises the three dither entry points against the committed blue-noise mask:
 * the atomic quantiser (::ra8_gfx_dither_gray4_level), the packed-gray4 tile
 * transform (::ra8_gfx_dither_gray8_to_gray4), and the framebuffer blit
 * (::ra8_gfx_blit_gray8_dither). Covers the quantiser's endpoint/monotonic/range
 * invariants, the unbiased-tone property that proves the dither removes banding
 * (a flat field averages back to its source value), seamless tiling across a
 * tile boundary, the packed/level cross-check, argument-validation MC/DC on both
 * compound guards, and a pinned rendered-framebuffer golden (FNV-1a). The render
 * is pure integer arithmetic over a `const` table, so this host golden is the
 * byte value ra8_emulator and silicon also produce (the EIL==HIL rule).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_gfx.h"
#include "ra8_gfx_dither.h"
#include "unity_minimal.h"

/**
 * @enum dither_test_dim_t
 * @brief Fixture geometry, framebuffer format, and palette constants for the tests.
 */
typedef enum : uint16_t {
  k_dt_grad_w     = 64,  /**< Gradient / golden image width in pixels.          */
  k_dt_grad_h     = 32,  /**< Gradient / golden image height in pixels.         */
  k_dt_argb       = 4,   /**< ARGB8888 bytes per pixel.                         */
  k_dt_r_byte     = 2,   /**< R byte offset within an ARGB8888 pixel (B G R A). */
  k_dt_mask_dim   = 64,  /**< Blue-noise mask period (== flat-field edge).      */
  k_dt_gray_max   = 255, /**< Peak 8-bit gray value.                            */
  k_dt_step       = 17,  /**< Palette step (255/15); reconstructed gray = n*17. */
  k_dt_seam_w     = 24,  /**< Seam-test image width.                            */
  k_dt_seam_h     = 6,   /**< Seam-test image height.                           */
  k_dt_seam_split = 10,  /**< Column the seam-test image is split at.           */
} dither_test_dim_t;

/**
 * @enum dither_fnv_t
 * @brief 32-bit FNV-1a parameters and the pinned framebuffer golden.
 * @details The golden is the FNV-1a hash of the ARGB8888 framebuffer after a
 *          horizontal 0..255 ramp is blitted through ::ra8_gfx_blit_gray8_dither.
 *          Regenerate it (and re-pin here) only when the mask or the quantiser
 *          rule changes -- a change to either is a deliberate golden update.
 */
typedef enum : uint32_t {
  k_dt_fnv_offset  = 0x811C9DC5U, /**< FNV-1a 32-bit offset basis.        */
  k_dt_fnv_prime   = 0x01000193U, /**< FNV-1a 32-bit prime.               */
  k_dt_blit_golden = 0x84769B0DU, /**< Pinned ramp-blit framebuffer hash. */
} dither_fnv_t;

/** @brief Full-gradient ARGB8888 framebuffer (k_dt_grad_w x k_dt_grad_h). */
static uint8_t s_dt_fb_full[k_dt_grad_w * k_dt_grad_h * k_dt_argb];

/** @brief Second ARGB8888 framebuffer used to prove tiled == whole rendering. */
static uint8_t s_dt_fb_tiled[k_dt_seam_w * k_dt_seam_h * k_dt_argb];

/** @brief First ARGB8888 framebuffer used to prove tiled == whole rendering. */
static uint8_t s_dt_fb_whole[k_dt_seam_w * k_dt_seam_h * k_dt_argb];

/** @brief Row-major gray8 source scratch (large enough for every fixture). */
static uint8_t s_dt_src[k_dt_grad_w * k_dt_grad_h];

/** @brief Packed-gray4 output scratch (>= (grad_w*grad_h + 1)/2 bytes). */
static uint8_t s_dt_pack[(k_dt_grad_w * k_dt_grad_h) / 2U];

/** @brief FNV-1a 32-bit hash of `len` bytes at `buf`. */
static uint32_t fnv1a(const uint8_t* buf, size_t len)
{
  uint32_t hash = (uint32_t)k_dt_fnv_offset;
  for (size_t i = 0U; i < len; ++i) {
    hash ^= (uint32_t)buf[i];
    hash *= (uint32_t)k_dt_fnv_prime;
  }
  return hash;
}

/** @brief Fill `s_dt_src` (w x h) with a horizontal 0..255 ramp (banding source). */
static void fill_hramp(int32_t w, int32_t h)
{
  for (int32_t y = 0; y < h; ++y) {
    for (int32_t x = 0; x < w; ++x) {
      s_dt_src[(y * w) + x] =
        (uint8_t)(((uint32_t)x * (uint32_t)k_dt_gray_max) / (uint32_t)(w - 1));
    }
  }
}

/**
 * @test level quantiser invariants (endpoints, range, monotonic in value)
 *
 * @details Independent of the exact mask contents: black stays 0 and white
 * stays 15 at every position, every level is in [0,15], and for a fixed
 * position the level never decreases as the source value rises.
 *
 * @par MC/DC:
 * No compound decisions in this test -- it sweeps the pure quantiser and
 * asserts scalar invariants; the quantiser's own guards carry no `&&` / `||`.
 */
static void test_level_invariants(void)
{
  TEST_BEGIN("dither level endpoint/range/monotonic invariants");
  for (int32_t y = 0; y < k_dt_mask_dim; ++y) {
    for (int32_t x = 0; x < k_dt_mask_dim; ++x) {
      TEST_ASSERT_EQ(0, ra8_gfx_dither_gray4_level(0, x, y));
      TEST_ASSERT_EQ(k_ra8_gfx_dither_max_level,
                     ra8_gfx_dither_gray4_level((uint8_t)k_dt_gray_max, x, y));
      uint8_t prev = 0U;
      for (int32_t v = 0; v <= k_dt_gray_max; ++v) {
        const uint8_t lvl = ra8_gfx_dither_gray4_level((uint8_t)v, x, y);
        TEST_ASSERT(lvl <= (uint8_t)k_ra8_gfx_dither_max_level);
        TEST_ASSERT(lvl >= prev);
        prev = lvl;
      }
    }
  }
  TEST_END("dither level endpoint/range/monotonic invariants");
}

/**
 * @test unbiased tone -- a flat field averages back to its source value
 *
 * @details The core anti-banding guarantee: over one full mask period a flat
 * gray v dithers to levels whose reconstructed grays (level * 17) average to v.
 * Asserts the rounded average equals v within one gray level for every v.
 *
 * @par MC/DC:
 * No compound decisions in this test -- a straight statistical average over the
 * pure quantiser; the only branch is the scalar abs-value ternary.
 */
static void test_unbiased_flat_field(void)
{
  TEST_BEGIN("dither reproduces flat tone unbiased (no banding)");
  const int32_t n = k_dt_mask_dim * k_dt_mask_dim;
  for (int32_t v = 0; v <= k_dt_gray_max; ++v) {
    uint32_t sum_gray = 0U;
    for (int32_t y = 0; y < k_dt_mask_dim; ++y) {
      for (int32_t x = 0; x < k_dt_mask_dim; ++x) {
        sum_gray += (uint32_t)ra8_gfx_dither_gray4_level((uint8_t)v, x, y) * (uint32_t)k_dt_step;
      }
    }
    const int32_t avg = (int32_t)((sum_gray + (uint32_t)(n / 2)) / (uint32_t)n);
    const int32_t err = (avg > v) ? (avg - v) : (v - avg);
    TEST_ASSERT(err <= 1);
  }
  TEST_END("dither reproduces flat tone unbiased (no banding)");
}

/**
 * @test packed gray4 output equals the per-pixel level, nibble-packed
 *
 * @details Cross-checks the bulk transform against the atomic quantiser: every
 * packed nibble must equal ::ra8_gfx_dither_gray4_level for that pixel, with the
 * even/odd high/low nibble parity, and the reported size must be ceil(w*h/2).
 *
 * @par MC/DC:
 * No compound decisions in this test -- the even/odd nibble select is a scalar
 * ternary, not a boolean decision; the packed guards' MC/DC lives in
 * test_mcdc_pack_guards.
 */
static void test_packed_matches_level(void)
{
  TEST_BEGIN("packed gray4 == per-pixel level, correct size");
  fill_hramp(k_dt_grad_w, k_dt_grad_h);
  uint32_t        size = 0U;
  const ra8_err_t err  = ra8_gfx_dither_gray8_to_gray4(s_dt_src,
                                                       k_dt_grad_w,
                                                       k_dt_grad_h,
                                                       0,
                                                       0,
                                                       s_dt_pack,
                                                       sizeof(s_dt_pack),
                                                       &size);
  TEST_ASSERT_EQ(k_ra8_ok, err);
  TEST_ASSERT_EQ(((k_dt_grad_w * k_dt_grad_h + 1) / 2), size);
  for (int32_t y = 0; y < k_dt_grad_h; ++y) {
    for (int32_t x = 0; x < k_dt_grad_w; ++x) {
      const uint32_t i      = (uint32_t)((y * k_dt_grad_w) + x);
      const uint8_t  expect = ra8_gfx_dither_gray4_level(s_dt_src[i], x, y);
      const uint8_t  byte   = s_dt_pack[i / 2U];
      const uint8_t  nib = ((i & 1U) == 0U) ? (uint8_t)(byte >> (uint8_t)k_ra8_gfx_dither_nib_shift)
                                            : (uint8_t)(byte & 0x0FU);
      TEST_ASSERT_EQ(expect, nib);
    }
  }
  TEST_END("packed gray4 == per-pixel level, correct size");
}

/**
 * @test seamless tiling -- tiled render byte-equals a whole render
 *
 * @details Blits a ramp whole, then as a left tile at (0,0) and a right tile at
 * (split,0); because the mask is indexed at absolute coordinates the two
 * framebuffers must be byte-identical, proving no tile-boundary seam.
 *
 * @par MC/DC:
 * No compound decisions in this test -- it compares two rendered framebuffers
 * byte for byte; the blit's own compound guard is covered in
 * test_mcdc_blit_guards.
 */
static void test_seamless_tiling(void)
{
  TEST_BEGIN("tiled dither render == whole render (no seam)");
  fill_hramp(k_dt_seam_w, k_dt_seam_h);
  uint8_t       left[k_dt_seam_split * k_dt_seam_h];
  uint8_t       right[(k_dt_seam_w - k_dt_seam_split) * k_dt_seam_h];
  const int32_t rw = k_dt_seam_w - k_dt_seam_split;
  for (int32_t y = 0; y < k_dt_seam_h; ++y) {
    for (int32_t x = 0; x < k_dt_seam_split; ++x) {
      left[(y * k_dt_seam_split) + x] = s_dt_src[(y * k_dt_seam_w) + x];
    }
    for (int32_t x = 0; x < rw; ++x) {
      right[(y * rw) + x] = s_dt_src[(y * k_dt_seam_w) + (k_dt_seam_split + x)];
    }
  }

  (void)memset(s_dt_fb_whole, 0, sizeof(s_dt_fb_whole));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_gfx_init(s_dt_fb_whole, k_dt_seam_w, k_dt_seam_h, k_ra8_gfx_format_argb8888));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gfx_blit_gray8_dither(s_dt_src, k_dt_seam_w, k_dt_seam_h, 0, 0));

  (void)memset(s_dt_fb_tiled, 0, sizeof(s_dt_fb_tiled));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_gfx_init(s_dt_fb_tiled, k_dt_seam_w, k_dt_seam_h, k_ra8_gfx_format_argb8888));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gfx_blit_gray8_dither(left, k_dt_seam_split, k_dt_seam_h, 0, 0));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gfx_blit_gray8_dither(right, rw, k_dt_seam_h, k_dt_seam_split, 0));

  TEST_ASSERT_EQ(0, memcmp(s_dt_fb_whole, s_dt_fb_tiled, sizeof(s_dt_fb_whole)));
  TEST_END("tiled dither render == whole render (no seam)");
}

/**
 * @test rendered-framebuffer golden (FNV-1a) pins the dither output
 *
 * @details Blits a 0..255 horizontal ramp through ::ra8_gfx_blit_gray8_dither
 * into an ARGB8888 framebuffer and asserts the FNV-1a hash equals the committed
 * golden. Deterministic integer arithmetic over a const mask, so the host hash
 * is byte-identical to ra8_emulator and silicon (EIL==HIL).
 *
 * @par MC/DC:
 * No compound decisions in this test -- it hashes one rendered framebuffer and
 * compares against a pinned constant.
 */
static void test_blit_golden(void)
{
  TEST_BEGIN("dithered ramp blit matches committed framebuffer golden");
  fill_hramp(k_dt_grad_w, k_dt_grad_h);
  (void)memset(s_dt_fb_full, 0, sizeof(s_dt_fb_full));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_gfx_init(s_dt_fb_full, k_dt_grad_w, k_dt_grad_h, k_ra8_gfx_format_argb8888));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gfx_blit_gray8_dither(s_dt_src, k_dt_grad_w, k_dt_grad_h, 0, 0));
  TEST_ASSERT_EQ(k_dt_blit_golden, fnv1a(s_dt_fb_full, sizeof(s_dt_fb_full)));
  TEST_END("dithered ramp blit matches committed framebuffer golden");
}

/**
 * @test ra8_gfx_dither_gray8_to_gray4 argument-validation MC/DC
 *
 * @par MC/DC:
 * Decision: `if (w <= 0 || h <= 0)` (2 conditions)
 * - Vector 1: w=grad_w, h=grad_h -> false (both conditions false: encodes)
 * - Vector 2: w=0,      h=grad_h -> true  (varies w only)
 * - Vector 3: w=grad_w, h=0      -> true  (varies h only)
 * Vectors 1+2 prove w independently affects the outcome; 1+3 prove the same for
 * h. N+1 = 3 vectors for N=2 conditions: minimal MC/DC. The NULL-pointer guards
 * (src / out / out_size) and the capacity (no_mem) guard are covered alongside.
 */
static void test_mcdc_pack_guards(void)
{
  TEST_BEGIN("packed-dither MC/DC dimension + pointer + capacity guards");
  uint32_t size = 0U;
  /* NULL guards. */
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    ra8_gfx_dither_gray8_to_gray4(nullptr, 4, 4, 0, 0, s_dt_pack, sizeof(s_dt_pack), &size));
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    ra8_gfx_dither_gray8_to_gray4(s_dt_src, 4, 4, 0, 0, nullptr, sizeof(s_dt_pack), &size));
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    ra8_gfx_dither_gray8_to_gray4(s_dt_src, 4, 4, 0, 0, s_dt_pack, sizeof(s_dt_pack), nullptr));
  /* MC/DC vectors for (w <= 0) || (h <= 0). */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_gfx_dither_gray8_to_gray4(s_dt_src,
                                               k_dt_grad_w,
                                               k_dt_grad_h,
                                               0,
                                               0,
                                               s_dt_pack,
                                               sizeof(s_dt_pack),
                                               &size));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_gfx_dither_gray8_to_gray4(s_dt_src,
                                               0,
                                               k_dt_grad_h,
                                               0,
                                               0,
                                               s_dt_pack,
                                               sizeof(s_dt_pack),
                                               &size));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_gfx_dither_gray8_to_gray4(s_dt_src,
                                               k_dt_grad_w,
                                               0,
                                               0,
                                               0,
                                               s_dt_pack,
                                               sizeof(s_dt_pack),
                                               &size));
  /* Capacity guard: one byte short of ceil(4*4/2) == 8. */
  TEST_ASSERT_EQ(k_ra8_err_no_mem,
                 ra8_gfx_dither_gray8_to_gray4(s_dt_src, 4, 4, 0, 0, s_dt_pack, 7U, &size));
  TEST_END("packed-dither MC/DC dimension + pointer + capacity guards");
}

/**
 * @test ra8_gfx_blit_gray8_dither argument-validation MC/DC
 *
 * @par MC/DC:
 * Decision: `if (src == nullptr || w <= 0 || h <= 0)` (3 conditions)
 * - Vector 1: src=valid, w=8, h=8 -> false (all three false: blits)
 * - Vector 2: src=NULL,  w=8, h=8 -> true  (varies src only)
 * - Vector 3: src=valid, w=0, h=8 -> true  (varies w only)
 * - Vector 4: src=valid, w=8, h=0 -> true  (varies h only)
 * Vectors 1+2, 1+3, 1+4 each prove one condition independently affects the
 * outcome. N+1 = 4 vectors for N=3 conditions: minimal MC/DC. The separate
 * not-initialized guard is asserted first, before any ra8_gfx_init().
 */
static void test_mcdc_blit_guards(void)
{
  TEST_BEGIN("blit-dither MC/DC not-init + src/dim guards");
  /* Not-initialized: s_gfx_text_state is zero (this runs before any bind). */
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_gfx_blit_gray8_dither(s_dt_src, 8, 8, 0, 0));
  (void)memset(s_dt_fb_full, 0, sizeof(s_dt_fb_full));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_gfx_init(s_dt_fb_full, k_dt_grad_w, k_dt_grad_h, k_ra8_gfx_format_argb8888));
  /* MC/DC vectors for (src == nullptr) || (w <= 0) || (h <= 0). */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gfx_blit_gray8_dither(s_dt_src, 8, 8, 0, 0));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_gfx_blit_gray8_dither(nullptr, 8, 8, 0, 0));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_gfx_blit_gray8_dither(s_dt_src, 0, 8, 0, 0));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_gfx_blit_gray8_dither(s_dt_src, 8, 0, 0, 0));
  TEST_END("blit-dither MC/DC not-init + src/dim guards");
}

int main(void)
{
  /* Must run first: asserts the not-initialized path before any ra8_gfx_init(). */
  test_mcdc_blit_guards();
  test_level_invariants();
  test_unbiased_flat_field();
  test_packed_matches_level();
  test_seamless_tiling();
  test_blit_golden();
  test_mcdc_pack_guards();
  return 0;
}
