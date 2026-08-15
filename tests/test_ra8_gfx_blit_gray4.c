/**
 * @file test_ra8_gfx_blit_gray4.c
 * @brief Unit tests for ra8_gfx_blit_gray4_zoom() -- the reader-loupe sub-rect blit.
 *
 * @details
 * Binds ra8_gfx to a small ARGB8888 framebuffer (whose in-memory R byte equals
 * the written gray level) and checks the packed-gray4 sub-rectangle blit for:
 * exact 1:1 reproduction, integer 2x magnification (block replication), odd-width
 * nibble-parity handling (the high/low nibble staggers per row), clamping at the
 * source image edges, clipping at the framebuffer edges, and argument validation.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_gfx.h"
#include "unity_minimal.h"

/**
 * @enum gfx_fill_t
 * @brief Value the framebuffer is pre-filled with, so any pixel the blit missed is still recognisable.
 */
typedef enum : uint8_t {
  k_gfx_fill_sentinel =
    0xAA, /**< Framebuffer pre-fill value, so any untouched pixel is still recognisable. */
} gfx_fill_t;

/**
 * @enum test_g4_dim_t
 * @brief Framebuffer + fixture dimensions used across the gray4 blit tests.
 */
typedef enum : uint16_t {
  k_test_g4_fb_w   = 16, /**< Scratch framebuffer width in pixels.              */
  k_test_g4_fb_h   = 16, /**< Scratch framebuffer height in pixels.             */
  k_test_g4_argb   = 4,  /**< ARGB8888 bytes per pixel.                         */
  k_test_g4_r_byte = 2,  /**< R byte offset within an ARGB8888 pixel (B G R A). */
} test_g4_dim_t;

/** @brief 16x16 ARGB8888 scratch framebuffer (1024 B). */
static uint8_t s_g4_fb[k_test_g4_fb_w * k_test_g4_fb_h * k_test_g4_argb];

/**
 * @brief Packed 4x4 gray4 image whose pixel at flat index f has gray nibble f.
 * @details Nibbles 0..15 in row-major order -> gray levels 0x00,0x11,...,0xFF.
 */
static const uint8_t k_g4_ramp4x4[] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF};

/** @brief Packed 3x2 gray4 image, pixels 0..5 (odd width -> per-row nibble stagger). */
static const uint8_t k_g4_odd3x2[] = {0x01, 0x23, 0x45};

/** @brief Re-bind ra8_gfx to a freshly-cleared ARGB8888 scratch framebuffer. */
static void rebind_argb(void)
{
  (void)memset(s_g4_fb, 0, sizeof(s_g4_fb));
  const ra8_err_t err =
    ra8_gfx_init(s_g4_fb, k_test_g4_fb_w, k_test_g4_fb_h, k_ra8_gfx_format_argb8888);
  TEST_ASSERT_EQ(k_ra8_ok, err);
}

/** @brief Fill the whole scratch framebuffer's bytes with a sentinel value. */
static void fill_sentinel(uint8_t v)
{
  (void)memset(s_g4_fb, v, sizeof(s_g4_fb));
}

/** @brief R byte (== written gray level) of framebuffer pixel (x, y). */
static uint8_t px_gray(int32_t x, int32_t y)
{
  const size_t off = (((size_t)y * (size_t)k_test_g4_fb_w) + (size_t)x) * (size_t)k_test_g4_argb;
  return s_g4_fb[off + (size_t)k_test_g4_r_byte];
}

/**
 * @par MC/DC:
 * (no compound decision in the code path this case exercises -- it verifies
 * the 1:1 happy path; no `&&` or `||` under test here)
 */
static void test_blit_gray4_1to1(void)
{
  TEST_BEGIN("gray4 1:1 blit reproduces every pixel");
  rebind_argb();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gfx_blit_gray4_zoom(k_g4_ramp4x4, 4, 4, 0, 0, 4, 4, 1, 0, 0));
  for (int32_t y = 0; y < 4; ++y) {
    for (int32_t x = 0; x < 4; ++x) {
      const uint8_t flat = (uint8_t)((y * 4) + x);
      TEST_ASSERT_EQ(((flat << 4) | flat), px_gray(x, y));
    }
  }
  TEST_END("gray4 1:1 blit reproduces every pixel");
}

/*
 * @par MC/DC:
 * (no compound decision under test -- verifies integer 2x block replication)
 */
/**
 * @brief Assert source pixel (@p sx, @p sy) filled its whole 2x2 output block.
 * @param[in] sx Source column, in source pixels.
 * @param[in] sy Source row, in source pixels.
 * @param[in] g  Gray level the whole block must carry.
 * @pre A 2x zoom blit has just written the framebuffer.
 * @pre The block at (2*@p sx, 2*@p sy) lies inside the framebuffer.
 * @post All four destination pixels of the block compared equal to @p g.
 * @post The framebuffer is unmodified.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void assert_zoom2_block(int32_t sx, int32_t sy, uint8_t g)
{
  for (int32_t dy = 0; dy < 2; ++dy) {
    for (int32_t dx = 0; dx < 2; ++dx) {
      TEST_ASSERT_EQ(g, px_gray((sx * 2) + dx, (sy * 2) + dy));
    }
  }
}

/**
 * @test test_blit_gray4_zoom2
 * @par MC/DC:
 * No compound decision is varied here -- this verifies integer 2x zoom (each
 * source pixel replicated into a 2x2 output block). The one compound guard on
 * the path, `(src_w <= 0) || (src_h <= 0)` in ra8_gfx_blit_gray4_zoom, is
 * traversed both-false as the control vector; its N+1 MC/DC vectors live in
 * test_mcdc_blit_gray4_dims.
 */
static void test_blit_gray4_zoom2(void)
{
  TEST_BEGIN("gray4 2x zoom replicates each pixel into a 2x2 block");
  rebind_argb();
  /* Sample the top-left 2x2 (flats 0,1,4,5 -> 0x00,0x11,0x44,0x55) at 2x. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gfx_blit_gray4_zoom(k_g4_ramp4x4, 4, 4, 0, 0, 2, 2, 2, 0, 0));
  static const uint8_t k_expect[2][2] = {{0x00, 0x11}, {0x44, 0x55}};
  for (int32_t sy = 0; sy < 2; ++sy) {
    for (int32_t sx = 0; sx < 2; ++sx) {
      assert_zoom2_block(sx, sy, k_expect[sy][sx]);
    }
  }
  TEST_END("gray4 2x zoom replicates each pixel into a 2x2 block");
}

/**
 * @par MC/DC:
 * (no compound decision under test -- verifies odd-width nibble parity, i.e.
 * the high/low nibble selection staggers across rows for an odd src_w)
 */
static void test_blit_gray4_nibble_parity(void)
{
  TEST_BEGIN("gray4 odd-width nibble parity is correct per row");
  rebind_argb();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gfx_blit_gray4_zoom(k_g4_odd3x2, 3, 2, 0, 0, 3, 2, 1, 0, 0));
  for (int32_t y = 0; y < 2; ++y) {
    for (int32_t x = 0; x < 3; ++x) {
      const uint8_t flat = (uint8_t)((y * 3) + x); /* 0,1,2 / 3,4,5 -> parity staggers */
      TEST_ASSERT_EQ(((flat << 4) | flat), px_gray(x, y));
    }
  }
  TEST_END("gray4 odd-width nibble parity is correct per row");
}

/**
 * @par MC/DC:
 * (no compound decision under test -- verifies source-edge clamp + framebuffer
 * clip leave off-region pixels untouched)
 */
static void test_blit_gray4_clips_at_edges(void)
{
  TEST_BEGIN("gray4 blit clamps to image + clips to framebuffer");
  /* (a) Sub-rect runs off the RIGHT image edge: sx=2,sw=4 -> only x=2,3 exist. */
  rebind_argb();
  fill_sentinel(k_gfx_fill_sentinel);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gfx_blit_gray4_zoom(k_g4_ramp4x4, 4, 4, 2, 0, 4, 1, 1, 0, 0));
  TEST_ASSERT_EQ(0x22, px_gray(0, 0)); /* source (2,0) = flat 2     */
  TEST_ASSERT_EQ(0x33, px_gray(1, 0)); /* source (3,0) = flat 3     */
  TEST_ASSERT_EQ(0xAA, px_gray(2, 0)); /* off-image: left untouched */

  /* (b) Destination runs off the RIGHT framebuffer edge: only x=14,15 land. */
  rebind_argb();
  fill_sentinel(k_gfx_fill_sentinel);
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_gfx_blit_gray4_zoom(k_g4_ramp4x4, 4, 4, 0, 0, 4, 1, 1, k_test_g4_fb_w - 2, 0));
  TEST_ASSERT_EQ(0x00, px_gray(14, 0)); /* source (0,0) = flat 0                       */
  TEST_ASSERT_EQ(0x11, px_gray(15, 0)); /* source (1,0) = flat 1; x=16,17 clipped away */
  TEST_END("gray4 blit clamps to image + clips to framebuffer");
}

/**
 * @test test_mcdc_blit_gray4_dims
 *
 * @par MC/DC:
 * Decision: `if ((src_w <= 0) || (src_h <= 0))` (2 conditions) in
 * libs/ra8_gfx/src/ra8_gfx_blit_gray4.c@ra8_gfx_blit_gray4_zoom
 * - Vector 1: src_w=4, src_h=4 -> false (both false; control) -> k_ra8_ok
 * - Vector 2: src_w=0, src_h=4 -> true  (varies src_w only)   -> invalid_arg
 * - Vector 3: src_w=4, src_h=0 -> true  (varies src_h only)   -> invalid_arg
 * Vectors 1+2 prove src_w independently drives the outcome; 1+3 prove the same
 * for src_h. N+1 = 3 vectors for N=2 conditions: minimal MC/DC. The scalar
 * not-initialized / NULL-src / zoom guards are covered alongside.
 */
static void test_mcdc_blit_gray4_dims(void)
{
  TEST_BEGIN("gray4 blit MC/DC dimension + scalar guards");
  /* Not-initialized: g_gfx_text_state is zero (this runs before any bind). */
  TEST_ASSERT_EQ(k_ra8_err_not_initialized,
                 ra8_gfx_blit_gray4_zoom(k_g4_ramp4x4, 4, 4, 0, 0, 4, 4, 1, 0, 0));
  rebind_argb();
  /* Scalar guards. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_gfx_blit_gray4_zoom(nullptr, 4, 4, 0, 0, 4, 4, 1, 0, 0));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_gfx_blit_gray4_zoom(k_g4_ramp4x4, 4, 4, 0, 0, 4, 4, 0, 0, 0));
  /* MC/DC vectors for (src_w <= 0) || (src_h <= 0). */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gfx_blit_gray4_zoom(k_g4_ramp4x4, 4, 4, 0, 0, 4, 4, 1, 0, 0));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_gfx_blit_gray4_zoom(k_g4_ramp4x4, 0, 4, 0, 0, 4, 4, 1, 0, 0));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_gfx_blit_gray4_zoom(k_g4_ramp4x4, 4, 0, 0, 0, 4, 4, 1, 0, 0));
  TEST_END("gray4 blit MC/DC dimension + scalar guards");
}

int main(void)
{
  /* Must run first: asserts the not-initialized path before any ra8_gfx_init(). */
  test_mcdc_blit_gray4_dims();
  test_blit_gray4_1to1();
  test_blit_gray4_zoom2();
  test_blit_gray4_nibble_parity();
  test_blit_gray4_clips_at_edges();
  return 0;
}
