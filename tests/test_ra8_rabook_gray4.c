/**
 * @file test_ra8_rabook_gray4.c
 * @brief Host unit tests for the gray4 transcode stage (ra8_rabook_gray4).
 *
 * @details
 * Verifies the independently-testable pieces of the #149 image transcode:
 *
 *  - @ref ra8_rabook_gray4_output_dims -- dimension-clamping arithmetic
 *  - @ref ra8_rabook_gray4_encode      -- quantise + nibble-pack (4-bpp)
 *  - @ref ra8_rabook_gray8_encode      -- verbatim 8-bpp copy (#343)
 *  - @ref ra8_rabook_gray4_downscale   -- Q16.16 bilinear resample
 *
 * @par MC/DC:
 * Decision: `if (src_w == 0 || src_h == 0 || max_edge == 0)` in
 *   ra8_rabook_gray4_output_dims -- 3-condition short-circuit OR. N+1 = 4 vectors
 *   (one all-false control plus one row that flips EACH condition true in turn,
 *   with the others held false so the flipped condition alone drives the outcome
 *   to true). test_dims_no_scale supplies the control; the three single-true rows
 *   below each prove independent influence:
 *   - Vector 1 (control): src_w=100, src_h=50, max_edge=200 -> false  -> (100,50)
 *       [test_dims_no_scale]
 *   - Vector 2 (vary src_w):    src_w=0,   src_h=50,  max_edge=200 -> true -> (0,0)
 *       [test_dims_zero_src]
 *   - Vector 3 (vary src_h):    src_w=100, src_h=0,   max_edge=200 -> true -> (0,0)
 *       [test_dims_zero_src_h]
 *   - Vector 4 (vary max_edge): src_w=100, src_h=50,  max_edge=0   -> true -> (0,0)
 *       [test_dims_zero_max_edge]
 *   1+2 isolate src_w, 1+3 isolate src_h, 1+4 isolate max_edge.
 *
 * Decision: `if (longer <= max_edge)` in ra8_rabook_gray4_output_dims (single).
 *   - Vector 1: src_w=100,  src_h=50,   max_edge=200  -> true  (no scale)
 *       [test_dims_no_scale]
 *   - Vector 2: src_w=3200, src_h=1800, max_edge=1600 -> false (scale down)
 *       [test_dims_scale_landscape]
 *
 * Branch: clamp-to-1 in ra8_rabook_gray4_output_dims -- `if (*out_w == 0)` and
 *   `if (*out_h == 0)`. A pathological aspect ratio rounds one edge to 0 and the
 *   guard rewrites it to 1 (each guard taken on a different vector):
 *   - test_dims_clamp_w_to_1: src 1x10000, max_edge 2 -> width rounds to 0 -> 1.
 *   - test_dims_clamp_h_to_1: src 10000x1, max_edge 2 -> height rounds to 0 -> 1.
 *
 * Decision: `if (dst_w == 0 || dst_h == 0)` in ra8_rabook_gray4_downscale --
 *   2-condition short-circuit OR. N+1 = 3 vectors:
 *   - Vector 1 (control):    dst_w=2, dst_h=2 -> false (proceeds)   [test_downscale_identity]
 *   - Vector 2 (vary dst_w): dst_w=0, dst_h=1 -> true -> invalid_arg [test_downscale_zero_dst]
 *   - Vector 3 (vary dst_h): dst_w=1, dst_h=0 -> true -> invalid_arg [test_downscale_zero_dst_h]
 *   1+2 isolate dst_w, 1+3 isolate dst_h.
 *
 * Decision: `if (src_w == 0 || src_h == 0)` in ra8_rabook_gray4_downscale (the
 *   src guard, reached only with valid dst) -- 2-condition short-circuit OR.
 *   N+1 = 3 vectors:
 *   - Vector 1 (control):    src_w=2, src_h=2 -> false (samples)            [test_downscale_identity]
 *   - Vector 2 (vary src_w): src_w=0, src_h=2 -> true -> ok + zeroed dst    [test_downscale_zero_src]
 *   - Vector 3 (vary src_h): src_w=2, src_h=0 -> true -> ok + zeroed dst    [test_downscale_zero_src_h]
 *   1+2 isolate src_w, 1+3 isolate src_h.
 *
 * Decision: `if ((i & 1U) == 0U)` in ra8_rabook_gray4_encode (even vs odd pixel)
 *   - Vector 1: i=0 (even) -> high nibble path taken
 *   - Vector 2: i=1 (odd)  -> low nibble path taken
 *
 * Build (standalone -- does not link ra8_hal or ra8_sim_mmap):
 * @code
 *   clang -std=c2x -DRA8_SIMULATOR_MODE \
 *     -I libs/ra8_core/inc -I libs/ra8_rabook_compile/inc \
 *     tests/test_ra8_rabook_gray4.c \
 *     libs/ra8_rabook_compile/src/ra8_rabook_gray4.c \
 *     libs/ra8_core/src/ra8_err.c libs/ra8_core/src/ra8_log.c \
 *     -o /tmp/test_ra8_rabook_gray4 && /tmp/test_ra8_rabook_gray4
 * @endcode
 *
 * @since Version 0.1.0
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_log.h"
#include "ra8_rabook_gray4.h"

/**
 * @enum t_g4_dims_t
 * @brief Source geometries and edge caps fed to `ra8_rabook_gray4_output_dims()`.
 *
 * @details
 * Each pair is chosen to land on one branch of the fit-to-max-edge rule: within
 * the cap, exactly on it, over it in landscape, and over it in portrait. The
 * two degenerate values drive the round-to-zero clamps.
 */
typedef enum : uint16_t {
  k_t_src_small_w  = 100U,   /**< Source width already inside the cap.       */
  k_t_src_small_h  = 50U,    /**< Its height.                                */
  k_t_cap_generous = 200U,   /**< Cap larger than the source, so no scaling. */
  k_t_cap_edge     = 1600U,  /**< The e-reader long-edge cap; also the scaled
                                   width every over-cap case must land on.     */
  k_t_fit_h        = 900U,   /**< Height that pairs with the cap at 16:9.    */
  k_t_land_w       = 3200U,  /**< Landscape source width, 2x the cap.        */
  k_t_land_h       = 1800U,  /**< Its height, 2x k_t_fit_h.                  */
  k_t_port_w       = 800U,   /**< Portrait source width.                     */
  k_t_port_h       = 2400U,  /**< Its height, past the cap on the long edge. */
  k_t_extreme_edge = 10000U, /**< Long edge extreme enough that the short edge
                                   rounds to zero and must be clamped to 1.    */
} t_g4_dims_t;

/**
 * @enum t_g4_poison_t
 * @brief Out-parameter seeds proving the callee always writes them.
 */
typedef enum : uint8_t {
  k_t_dim_poison  = 42U, /**< Pre-set width/height; a rejected call must leave it. */
  k_t_size_poison = 99U, /**< Pre-set output size, same purpose.                   */
} t_g4_poison_t;

/**
 * @enum t_g4_packed_t
 * @brief Expected 4-bit-per-pixel packed bytes, two pixels per byte.
 *
 * @details
 * The encoder maps an 8-bit grey `v` to nibble `(v + 8) / 17` and packs the
 * first pixel into the high nibble. Each name states the pixel pair it encodes
 * so a failing assertion identifies the pixel, not just the byte.
 */
typedef enum : uint8_t {
  k_t_pack_0_1    = 0x05U, /**< Nibbles 0 and 5: greys 0 and 85.            */
  k_t_pack_2_3    = 0x23U, /**< Nibbles 2 and 3: the exact-palette pair.    */
  k_t_pack_8_pad  = 0x80U, /**< Nibble 8 plus the zero pad of an odd count. */
  k_t_pack_15_pad = 0xF0U, /**< Nibble 15 plus the zero pad.                */
  k_t_pack_15_15  = 0xFFU, /**< Both nibbles saturated; also the fill a
                               zero-sized downscale must overwrite.        */
} t_g4_packed_t;

/**
 * @enum t_g4_pixel_t
 * @brief Grey levels the identity and 2:1 downscale arms expect back.
 *
 * @details
 * Deliberately unrounded values: an identity downscale must return them
 * verbatim, so any interpolation the resampler wrongly applies shows up.
 */
typedef enum : uint8_t {
  k_t_px_tl      = 10U,  /**< Top-left source pixel.                      */
  k_t_px_tr      = 20U,  /**< Top-right source pixel.                     */
  k_t_px_bl      = 30U,  /**< Bottom-left source pixel.                   */
  k_t_px_br      = 40U,  /**< Bottom-right source pixel.                  */
  k_t_px_sample2 = 128U, /**< The 4x1 -> 2x1 arm's second sample, src[2]. */
} t_g4_pixel_t;

/* -------------------------------------------------------------------------- */
/* Helpers */
/* -------------------------------------------------------------------------- */

typedef enum : uint8_t {
  k_test_pass = 0U, /**< Test pass. */
  k_test_fail = 1U, /**< Test fail. */
} test_result_t;

static uint32_t s_total = 0U;
static uint32_t s_pass  = 0U;

static void check(bool cond, const char* name)
{
  s_total++;
  if (cond) {
    s_pass++;
    printf("[PASS] %s\n", name);
  } else {
    printf("[FAIL] %s\n", name);
  }
}

/* -------------------------------------------------------------------------- */
/* output_dims tests */
/* -------------------------------------------------------------------------- */

/**
 * @test test_dims_no_scale
 * @par MC/DC:
 * Decision: `src_w == 0 || src_h == 0 || max_edge == 0`
 * (3 conditions, function `ra8_rabook_gray4_output_dims`).
 * - V1 src_w=100, src_h=50,  max_edge=200 -> C1=F,C2=F,C3=F -> F (proceeds, returns (100,50)).
 * - V2 src_w=0,   src_h=100, max_edge=200 -> C1=T -> T (returns (0,0)).
 * - V3 src_w=100, src_h=0,   max_edge=200 -> C2=T -> T (returns (0,0)).
 * - V4 src_w=100, src_h=50,  max_edge=0   -> C3=T -> T (returns (0,0)).
 * Vectors 1+2 prove C1 independent; 1+3 prove C2; 1+4 prove C3.
 * N+1 = 4 vectors (this test supplies the all-false control vector V1; the
 * varied-condition vectors V2/V3/V4 are in sibling tests test_dims_zero_src,
 * test_dims_zero_src_h, and test_dims_zero_max_edge of this file).
 */
static void test_dims_no_scale(void)
{
  uint16_t ow = 0U;
  uint16_t oh = 0U;
  ra8_rabook_gray4_output_dims(k_t_src_small_w, k_t_src_small_h, k_t_cap_generous, &ow, &oh);
  check(ow == k_t_src_small_w && oh == k_t_src_small_h, "dims: no scale when within max_edge");
}

/**
 * @test test_dims_exact_edge
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the single-condition
 * fit-to-max-edge branch `if (longer <= max_edge)` at its boundary
 * (longer == max_edge -> no scaling, dimensions pass through unchanged); no
 * `&&` or `||` in the code under test that this case touches)
 */
static void test_dims_exact_edge(void)
{
  uint16_t ow = 0U;
  uint16_t oh = 0U;
  ra8_rabook_gray4_output_dims(k_t_cap_edge, k_t_fit_h, k_t_cap_edge, &ow, &oh);
  check(ow == k_t_cap_edge && oh == k_t_fit_h, "dims: exact max_edge passes unchanged");
}

/**
 * @test test_dims_scale_landscape
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the single-condition
 * fit-to-max-edge branch `if (longer <= max_edge)` on its false arm (a 2x
 * over-cap landscape source is scaled down to the cap); no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_dims_scale_landscape(void)
{
  uint16_t ow = 0U;
  uint16_t oh = 0U;
  ra8_rabook_gray4_output_dims(k_t_land_w, k_t_land_h, k_t_cap_edge, &ow, &oh);
  check(ow == k_t_cap_edge && oh == k_t_fit_h, "dims: 2x landscape scale down");
}

/**
 * @test test_dims_scale_portrait
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the single-condition
 * fit-to-max-edge scale-down branch for a portrait source (longer edge clamped
 * to the cap, width stays >= 1); no `&&` or `||` in the code under test that
 * this case touches)
 */
static void test_dims_scale_portrait(void)
{
  uint16_t ow = 0U;
  uint16_t oh = 0U;
  ra8_rabook_gray4_output_dims(k_t_port_w, k_t_port_h, k_t_cap_edge, &ow, &oh);
  check(oh <= k_t_cap_edge, "dims: portrait longer edge clamped");
  check(ow >= 1U, "dims: portrait width at least 1");
}

/**
 * @test test_dims_null_out
 * @par MC/DC:
 * Decision: `out_w == nullptr || out_h == nullptr`
 * (2 conditions, function `ra8_rabook_gray4_output_dims`).
 * - V1 out_w=&ow,  out_h=&oh  -> C1=F,C2=F -> F (proceeds past the guard).
 * - V2 out_w=NULL, out_h=&oh  -> C1=T -> T (early return, out params untouched).
 * - V3 out_w=&ow,  out_h=NULL -> C2=T -> T (early return, out params untouched).
 * Vectors 1+2 prove C1 (out_w) independent; 1+3 prove C2 (out_h).
 * N+1 = 3 vectors (this test supplies V2 and V3 via its two calls, asserting
 * ow/oh keep their poison value k_t_dim_poison; the all-false control V1 is
 * supplied by the sibling dims tests of this file, which all pass non-NULL out
 * pointers).
 */
static void test_dims_null_out(void)
{
  uint16_t ow = k_t_dim_poison;
  uint16_t oh = k_t_dim_poison;
  ra8_rabook_gray4_output_dims(k_t_src_small_w, k_t_src_small_w, k_t_cap_generous, nullptr, &oh);
  ra8_rabook_gray4_output_dims(k_t_src_small_w, k_t_src_small_w, k_t_cap_generous, &ow, nullptr);
  check(ow == k_t_dim_poison && oh == k_t_dim_poison, "dims: null out ptr leaves values unchanged");
}

/**
 * @test test_dims_zero_src
 * @par MC/DC:
 * Decision: `src_w == 0 || src_h == 0 || max_edge == 0`
 * (3 conditions, function `ra8_rabook_gray4_output_dims`).
 * - V1 src_w=100, src_h=50,  max_edge=200 -> C1=F,C2=F,C3=F -> F (proceeds).
 * - V2 src_w=0,   src_h=100, max_edge=200 -> C1=T -> T (returns (0,0)).
 * - V3 src_w=100, src_h=0,   max_edge=200 -> C2=T -> T (returns (0,0)).
 * - V4 src_w=100, src_h=50,  max_edge=0   -> C3=T -> T (returns (0,0)).
 * Vectors 1+2 prove C1 independent; 1+3 prove C2; 1+4 prove C3.
 * N+1 = 4 vectors (this test supplies V2, flipping only src_w true; the control
 * V1 and the other varied vectors V3/V4 are in sibling tests test_dims_no_scale,
 * test_dims_zero_src_h, and test_dims_zero_max_edge of this file).
 */
static void test_dims_zero_src(void)
{
  uint16_t ow = k_t_size_poison;
  uint16_t oh = k_t_size_poison;
  ra8_rabook_gray4_output_dims(0U, k_t_src_small_w, k_t_cap_generous, &ow, &oh);
  check(ow == 0U && oh == 0U, "dims: zero src_w yields (0,0)");
}

/**
 * @test test_dims_zero_src_h
 * @par MC/DC:
 * Decision: `src_w == 0 || src_h == 0 || max_edge == 0`
 * (3 conditions, function `ra8_rabook_gray4_output_dims`).
 * - V1 src_w=100, src_h=50,  max_edge=200 -> C1=F,C2=F,C3=F -> F (proceeds).
 * - V2 src_w=0,   src_h=100, max_edge=200 -> C1=T -> T (returns (0,0)).
 * - V3 src_w=100, src_h=0,   max_edge=200 -> C2=T -> T (returns (0,0)).
 * - V4 src_w=100, src_h=50,  max_edge=0   -> C3=T -> T (returns (0,0)).
 * Vectors 1+2 prove C1 independent; 1+3 prove C2; 1+4 prove C3.
 * N+1 = 4 vectors (this test supplies V3, flipping only src_h true; the control
 * V1 and the other varied vectors V2/V4 are in sibling tests test_dims_no_scale,
 * test_dims_zero_src, and test_dims_zero_max_edge of this file).
 */
static void test_dims_zero_src_h(void)
{
  /*
   * MC/DC vector 3 for `src_w == 0 || src_h == 0 || max_edge == 0`: only
   * src_h is flipped true (src_w and max_edge stay non-zero), proving src_h
   * independently drives the decision to the (0,0) arm.
   */
  uint16_t ow = k_t_size_poison;
  uint16_t oh = k_t_size_poison;
  ra8_rabook_gray4_output_dims(k_t_src_small_w, 0U, k_t_cap_generous, &ow, &oh);
  check(ow == 0U && oh == 0U, "dims: zero src_h yields (0,0)");
}

/**
 * @test test_dims_zero_max_edge
 * @par MC/DC:
 * Decision: `src_w == 0 || src_h == 0 || max_edge == 0`
 * (3 conditions, function `ra8_rabook_gray4_output_dims`).
 * - V1 src_w=100, src_h=50,  max_edge=200 -> C1=F,C2=F,C3=F -> F (proceeds).
 * - V2 src_w=0,   src_h=100, max_edge=200 -> C1=T -> T (returns (0,0)).
 * - V3 src_w=100, src_h=0,   max_edge=200 -> C2=T -> T (returns (0,0)).
 * - V4 src_w=100, src_h=50,  max_edge=0   -> C3=T -> T (returns (0,0)).
 * Vectors 1+2 prove C1 independent; 1+3 prove C2; 1+4 prove C3.
 * N+1 = 4 vectors (this test supplies V4, flipping only max_edge true; the
 * control V1 and the other varied vectors V2/V3 are in sibling tests
 * test_dims_no_scale, test_dims_zero_src, and test_dims_zero_src_h of this file).
 */
static void test_dims_zero_max_edge(void)
{
  /*
   * MC/DC vector 4 for `src_w == 0 || src_h == 0 || max_edge == 0`: only
   * max_edge is flipped true (both source dims stay non-zero), proving
   * max_edge independently drives the decision to the (0,0) arm.
   */
  uint16_t ow = k_t_size_poison;
  uint16_t oh = k_t_size_poison;
  ra8_rabook_gray4_output_dims(k_t_src_small_w, k_t_src_small_h, 0U, &ow, &oh);
  check(ow == 0U && oh == 0U, "dims: zero max_edge yields (0,0)");
}

/**
 * @test test_dims_clamp_w_to_1
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the single-condition
 * round-to-zero width clamp `if (*out_w == 0) *out_w = 1` via an extreme
 * portrait aspect (src 1x10000, max_edge 2) whose scaled width rounds to 0; no
 * `&&` or `||` in the code under test that this case touches)
 */
static void test_dims_clamp_w_to_1(void)
{
  /*
   * Extreme portrait aspect: the scaled width rounds to 0 and the
   * `if (*out_w == 0) *out_w = 1` guard rewrites it to 1.
   *   longer = 10000, half = 5000
   *   out_w = (1*2 + 5000) / 10000 = 0  -> clamped to 1
   *   out_h = (10000*2 + 5000) / 10000 = 2
   */
  uint16_t ow = 0U;
  uint16_t oh = 0U;
  ra8_rabook_gray4_output_dims(1U, k_t_extreme_edge, 2U, &ow, &oh);
  check(ow == 1U, "dims: degenerate width clamped up to 1");
  check(oh == 2U, "dims: degenerate-width height is 2");
}

/**
 * @test test_dims_clamp_h_to_1
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the single-condition
 * round-to-zero height clamp `if (*out_h == 0) *out_h = 1` via an extreme
 * landscape aspect (src 10000x1, max_edge 2) whose scaled height rounds to 0;
 * no `&&` or `||` in the code under test that this case touches)
 */
static void test_dims_clamp_h_to_1(void)
{
  /*
   * Extreme landscape aspect: the scaled height rounds to 0 and the
   * `if (*out_h == 0) *out_h = 1` guard rewrites it to 1.
   *   longer = 10000, half = 5000
   *   out_w = (10000*2 + 5000) / 10000 = 2
   *   out_h = (1*2 + 5000) / 10000 = 0  -> clamped to 1
   */
  uint16_t ow = 0U;
  uint16_t oh = 0U;
  ra8_rabook_gray4_output_dims(k_t_extreme_edge, 1U, 2U, &ow, &oh);
  check(ow == 2U, "dims: degenerate-height width is 2");
  check(oh == 1U, "dims: degenerate height clamped up to 1");
}

/* -------------------------------------------------------------------------- */
/* encode tests */
/* -------------------------------------------------------------------------- */

/**
 * @test test_encode_4px_exact_palette
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the quantise-and-pack path
 * of `ra8_rabook_gray4_encode` with exact-palette values (v = i*17), covering
 * the single-condition even/odd nibble split `if ((i & 1U) == 0U)` across four
 * pixels; no `&&` or `||` in the code under test that this case touches)
 */
static void test_encode_4px_exact_palette(void)
{
  /*
   * Pixels at exact palette entries: v = i * 17 for i = 0..3.
   * (v + 8) / 17 == i exactly for these values.
   * Expected: nib[0]=0, nib[1]=1, nib[2]=2, nib[3]=3
   * Packed:   byte[0] = (0<<4)|1 = 0x01
   *           byte[1] = (2<<4)|3 = 0x23
   */
  static const uint8_t pixels[] = {0U, 17U, 34U, 51U};
  uint8_t              out[2]   = {};
  uint32_t             out_size = 0U;

  ra8_err_t err = ra8_rabook_gray4_encode(pixels, 4U, 1U, out, sizeof(out), &out_size);
  check(err == k_ra8_ok, "encode: exact palette 4px -- ok");
  check(out_size == 2U, "encode: exact palette 4px -- size");
  check(out[0] == 0x01U, "encode: exact palette 4px -- byte0");
  check(out[1] == k_t_pack_2_3, "encode: exact palette 4px -- byte1");
}

/**
 * @test test_encode_3px_odd
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the odd-pixel-count encode
 * path where the trailing byte is high-nibble only and the low nibble stays
 * zero-padded; no `&&` or `||` in the code under test that this case touches)
 */
static void test_encode_3px_odd(void)
{
  /*
   * 3 pixels: v = 0, 85, 255
   * nib[0] = (0   + 8) / 17 = 0
   * nib[1] = (85  + 8) / 17 = 93/17 = 5
   * nib[2] = (255 + 8) / 17 = 263/17 = 15
   * Packed: byte[0] = (0<<4)|5 = 0x05
   *         byte[1] = (15<<4)  = 0xF0  (low nibble zero-padded)
   */
  static const uint8_t pixels[] = {0U, 85U, 255U};
  uint8_t              out[2]   = {};
  uint32_t             out_size = 0U;

  ra8_err_t err = ra8_rabook_gray4_encode(pixels, 3U, 1U, out, sizeof(out), &out_size);
  check(err == k_ra8_ok, "encode: odd pixel count -- ok");
  check(out_size == 2U, "encode: odd pixel count -- size");
  check(out[0] == k_t_pack_0_1, "encode: odd pixel count -- byte0");
  check(out[1] == k_t_pack_15_pad, "encode: odd pixel count -- byte1 (low nibble zero)");
}

/**
 * @test test_encode_1px
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the single-pixel encode
 * (one high nibble, one output byte); no `&&` or `||` in the code under test
 * that this case touches)
 */
static void test_encode_1px(void)
{
  static const uint8_t pixel    = 128U;
  uint8_t              out[1]   = {};
  uint32_t             out_size = 0U;

  /*
   * nib = (128 + 8) / 17 = 136 / 17 = 8
   * byte[0] = (8 << 4) = 0x80
   */
  ra8_err_t err = ra8_rabook_gray4_encode(&pixel, 1U, 1U, out, sizeof(out), &out_size);
  check(err == k_ra8_ok, "encode: single pixel -- ok");
  check(out_size == 1U, "encode: single pixel -- size");
  check(out[0] == k_t_pack_8_pad, "encode: single pixel -- byte (nib=8)");
}

/**
 * @test test_encode_0px
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the single-condition
 * early-out `if (n_pixels == 0U)` that returns 0 bytes for an empty image; no
 * `&&` or `||` in the code under test that this case touches)
 */
static void test_encode_0px(void)
{
  uint8_t  dummy[1] = {};
  uint32_t out_size = k_t_size_poison;

  ra8_err_t err = ra8_rabook_gray4_encode(dummy, 0U, 0U, dummy, sizeof(dummy), &out_size);
  check(err == k_ra8_ok && out_size == 0U, "encode: 0 pixels returns 0 bytes");
}

/**
 * @test test_encode_buffer_too_small
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the single-condition
 * capacity check `if (out_cap < n_bytes)` returning k_ra8_err_no_mem; no `&&`
 * or `||` in the code under test that this case touches)
 */
static void test_encode_buffer_too_small(void)
{
  static const uint8_t pixels[] = {0U, 255U, 128U, 64U};
  uint8_t              out[1]   = {};
  uint32_t             out_size = 0U;

  ra8_err_t err = ra8_rabook_gray4_encode(pixels, 4U, 1U, out, 1U, &out_size);
  check(err == k_ra8_err_no_mem, "encode: buffer too small returns no_mem");
}

/**
 * @test test_encode_null_src
 * @par MC/DC:
 * (no compound decisions in this test -- a precondition null-guard test that
 * returns early via the single-condition RA8_CHECK_NULL_PTR(gray_pixels) before
 * any compound decision; no `&&` or `||` in the code under test that this case
 * touches)
 */
static void test_encode_null_src(void)
{
  uint8_t   out[4]   = {};
  uint32_t  out_size = 0U;
  ra8_err_t err      = ra8_rabook_gray4_encode(nullptr, 4U, 1U, out, sizeof(out), &out_size);
  check(err == k_ra8_err_null_ptr, "encode: null src returns null_ptr");
}

/**
 * @test test_encode_max_nib
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the single-condition nibble
 * saturation clamp `if (nib > k_ra8_rabook_gray4_nib_max)` where near-white
 * pixels all pin to nibble 15; no `&&` or `||` in the code under test that this
 * case touches)
 */
static void test_encode_max_nib(void)
{
  static const uint8_t pixels[] = {255U, 254U, 253U};
  uint8_t              out[2]   = {};
  uint32_t             out_size = 0U;

  /*
   * 255: (255+8)/17 = 15, clamped 15
   * 254: (254+8)/17 = 15
   * 253: (253+8)/17 = 261/17 = 15
   * All three saturate to nibble 15.
   * byte[0] = (15<<4)|15 = 0xFF
   * byte[1] = (15<<4)    = 0xF0
   */
  ra8_rabook_gray4_encode(pixels, 3U, 1U, out, sizeof(out), &out_size);
  check(out[0] == k_t_pack_15_15 && out[1] == k_t_pack_15_pad,
        "encode: near-white pixels all saturate to 15");
}

/* -------------------------------------------------------------------------- */
/* gray8 encode tests (1 byte per pixel, verbatim copy) */
/* -------------------------------------------------------------------------- */

/**
 * @test test_gray8_encode_copies
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the verbatim gray8 copy
 * path of `ra8_rabook_gray8_encode`, which memcpy's w*h bytes with no `&&` or
 * `||` in the code under test that this case touches)
 */
static void test_gray8_encode_copies(void)
{
  static const uint8_t pixels[] = {0U, 17U, 128U, 255U};
  uint8_t              out[4]   = {};
  uint32_t             out_size = 0U;

  ra8_err_t err = ra8_rabook_gray8_encode(pixels, 4U, 1U, out, sizeof(out), &out_size);
  check(err == k_ra8_ok, "gray8 encode: ok");
  check(out_size == 4U, "gray8 encode: size == w*h");
  check(memcmp(out, pixels, sizeof(pixels)) == 0, "gray8 encode: bytes copied verbatim");
}

/**
 * @test test_gray8_encode_0px
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the single-condition
 * early-out `if (n_bytes == 0U)` that returns 0 bytes for an empty image; the
 * `err == k_ra8_ok && out_size == 0U` in the assertion is a conjoined
 * postcondition of that one path, not a decision under test; no `&&` or `||`
 * in the code under test that this case touches)
 */
static void test_gray8_encode_0px(void)
{
  uint8_t  dummy[1] = {};
  uint32_t out_size = k_t_size_poison;

  ra8_err_t err = ra8_rabook_gray8_encode(dummy, 0U, 0U, dummy, sizeof(dummy), &out_size);
  check(err == k_ra8_ok && out_size == 0U, "gray8 encode: 0 pixels returns 0 bytes");
}

/**
 * @test test_gray8_encode_buffer_too_small
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the single-condition
 * capacity check `if (out_cap < n_bytes)` returning k_ra8_err_no_mem; no `&&`
 * or `||` in the code under test that this case touches)
 */
static void test_gray8_encode_buffer_too_small(void)
{
  static const uint8_t pixels[] = {0U, 255U, 128U, 64U};
  uint8_t              out[1]   = {};
  uint32_t             out_size = 0U;

  ra8_err_t err = ra8_rabook_gray8_encode(pixels, 4U, 1U, out, 1U, &out_size);
  check(err == k_ra8_err_no_mem, "gray8 encode: buffer too small returns no_mem");
}

/**
 * @test test_gray8_encode_null_src
 * @par MC/DC:
 * (no compound decisions in this test -- a precondition null-guard test that
 * returns early via the single-condition RA8_CHECK_NULL_PTR(gray_pixels)
 * before any compound decision; no `&&` or `||` in the code under test that
 * this case touches)
 */
static void test_gray8_encode_null_src(void)
{
  uint8_t   out[4]   = {};
  uint32_t  out_size = 0U;
  ra8_err_t err      = ra8_rabook_gray8_encode(nullptr, 4U, 1U, out, sizeof(out), &out_size);
  check(err == k_ra8_err_null_ptr, "gray8 encode: null src returns null_ptr");
}

/* -------------------------------------------------------------------------- */
/* downscale tests */
/* -------------------------------------------------------------------------- */

/**
 * @test test_downscale_identity
 * @par MC/DC:
 * Decision A: `dst_w == 0 || dst_h == 0`
 * (2 conditions, function `ra8_rabook_gray4_downscale`).
 * - V1 dst_w=2, dst_h=2 -> C1=F,C2=F -> F (proceeds).
 * - V2 dst_w=0, dst_h=1 -> C1=T -> T (invalid_arg).
 * - V3 dst_w=1, dst_h=0 -> C2=T -> T (invalid_arg).
 * Vectors 1+2 prove C1 (dst_w) independent; 1+3 prove C2 (dst_h). N+1 = 3.
 * Decision B: `src_w == 0 || src_h == 0` (the source guard, reached only past
 * decision A; 2 conditions, same function).
 * - V1 src_w=2, src_h=2 -> C1=F,C2=F -> F (samples the bilinear kernel).
 * - V2 src_w=0, src_h=2 -> C1=T -> T (ok, dst zeroed).
 * - V3 src_w=2, src_h=0 -> C2=T -> T (ok, dst zeroed).
 * Vectors 1+2 prove C1 (src_w) independent; 1+3 prove C2 (src_h). N+1 = 3.
 * This test supplies the all-false control vector V1 for BOTH decisions (dst
 * 2x2, src 2x2 -> proceeds and resamples identity). The varied vectors are in
 * sibling tests: test_downscale_zero_dst / test_downscale_zero_dst_h complete
 * decision A, and test_downscale_zero_src / test_downscale_zero_src_h complete
 * decision B.
 */
static void test_downscale_identity(void)
{
  static const uint8_t src[]  = {10U, 20U, 30U, 40U};
  uint8_t              dst[4] = {};

  ra8_err_t err = ra8_rabook_gray4_downscale(src, 2U, 2U, dst, 2U, 2U);
  check(err == k_ra8_ok, "downscale: identity 2x2 -- ok");
  check(dst[0] == k_t_px_tl && dst[1] == k_t_px_tr && dst[2] == k_t_px_bl && dst[3] == k_t_px_br,
        "downscale: identity 2x2 -- pixels unchanged");
}

/**
 * @test test_downscale_2to1_horizontal
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the bilinear resample
 * sampling path (4x1 -> 2x1, left-aligned sample grid) with valid non-zero dst
 * and src, so both compound guards evaluate all-false and are not varied here;
 * no `&&` or `||` in the code under test that this case drives)
 */
static void test_downscale_2to1_horizontal(void)
{
  /*
   * 4x1 -> 2x1 bilinear (left-aligned sample grid):
   * dst[0] <- src at sx_fp = 0 * (4<<16) / 2 = 0     -> sx0=0, fx=0 -> src[0] = 0
   * dst[1] <- src at sx_fp = 1 * (4<<16) / 2 = 2<<16 -> sx0=2, fx=0 -> src[2] = 128
   */
  static const uint8_t src[]  = {0U, 64U, 128U, 192U};
  uint8_t              dst[2] = {};

  ra8_err_t err = ra8_rabook_gray4_downscale(src, 4U, 1U, dst, 2U, 1U);
  check(err == k_ra8_ok, "downscale: 4x1->2x1 -- ok");
  check(dst[0] == 0U && dst[1] == k_t_px_sample2, "downscale: 4x1->2x1 -- sampled at x=0,2");
}

/**
 * @test test_downscale_null_ptr
 * @par MC/DC:
 * (no compound decisions in this test -- two precondition null-guard calls that
 * return early via the separate single-condition RA8_CHECK_NULL_PTR(src) and
 * RA8_CHECK_NULL_PTR(dst) checks before any compound decision; no `&&` or `||`
 * in the code under test that this case touches)
 */
static void test_downscale_null_ptr(void)
{
  uint8_t buf[4] = {};
  check(ra8_rabook_gray4_downscale(nullptr, 2U, 2U, buf, 2U, 2U) == k_ra8_err_null_ptr,
        "downscale: null src");
  check(ra8_rabook_gray4_downscale(buf, 2U, 2U, nullptr, 2U, 2U) == k_ra8_err_null_ptr,
        "downscale: null dst");
}

/**
 * @test test_downscale_zero_dst
 * @par MC/DC:
 * Decision: `dst_w == 0 || dst_h == 0`
 * (2 conditions, function `ra8_rabook_gray4_downscale`).
 * - V1 dst_w=2, dst_h=2 -> C1=F,C2=F -> F (proceeds).
 * - V2 dst_w=0, dst_h=1 -> C1=T -> T (returns k_ra8_err_invalid_arg).
 * - V3 dst_w=1, dst_h=0 -> C2=T -> T (returns k_ra8_err_invalid_arg).
 * Vectors 1+2 prove C1 (dst_w) independent; 1+3 prove C2 (dst_h).
 * N+1 = 3 vectors (this test supplies V2, flipping only dst_w true; the control
 * V1 is in test_downscale_identity and V3 is in test_downscale_zero_dst_h of
 * this file).
 */
static void test_downscale_zero_dst(void)
{
  static const uint8_t src[]  = {1U, 2U, 3U, 4U};
  uint8_t              dst[1] = {};
  ra8_err_t            err    = ra8_rabook_gray4_downscale(src, 2U, 2U, dst, 0U, 1U);
  check(err == k_ra8_err_invalid_arg, "downscale: dst_w=0 returns invalid_arg");
}

/**
 * @test test_downscale_zero_src
 * @par MC/DC:
 * Decision: `src_w == 0 || src_h == 0`
 * (2 conditions, function `ra8_rabook_gray4_downscale`; the source guard,
 * reached only with a valid non-zero dst).
 * - V1 src_w=2, src_h=2 -> C1=F,C2=F -> F (samples).
 * - V2 src_w=0, src_h=2 -> C1=T -> T (returns k_ra8_ok, dst zeroed).
 * - V3 src_w=2, src_h=0 -> C2=T -> T (returns k_ra8_ok, dst zeroed).
 * Vectors 1+2 prove C1 (src_w) independent; 1+3 prove C2 (src_h).
 * N+1 = 3 vectors (this test supplies V2, flipping only src_w true; the control
 * V1 is in test_downscale_identity and V3 is in test_downscale_zero_src_h of
 * this file).
 */
static void test_downscale_zero_src(void)
{
  static const uint8_t src[]  = {1U};
  uint8_t              dst[4] = {k_t_pack_15_15, k_t_pack_15_15, k_t_pack_15_15, k_t_pack_15_15};

  ra8_err_t err = ra8_rabook_gray4_downscale(src, 0U, 2U, dst, 2U, 2U);
  check(err == k_ra8_ok, "downscale: zero src_w returns ok");
  check(dst[0] == 0U && dst[1] == 0U, "downscale: zero src_w zeroes dst");
}

/**
 * @test test_downscale_zero_dst_h
 * @par MC/DC:
 * Decision: `dst_w == 0 || dst_h == 0`
 * (2 conditions, function `ra8_rabook_gray4_downscale`).
 * - V1 dst_w=2, dst_h=2 -> C1=F,C2=F -> F (proceeds).
 * - V2 dst_w=0, dst_h=1 -> C1=T -> T (returns k_ra8_err_invalid_arg).
 * - V3 dst_w=1, dst_h=0 -> C2=T -> T (returns k_ra8_err_invalid_arg).
 * Vectors 1+2 prove C1 (dst_w) independent; 1+3 prove C2 (dst_h).
 * N+1 = 3 vectors (this test supplies V3, flipping only dst_h true; the control
 * V1 is in test_downscale_identity and V2 is in test_downscale_zero_dst of this
 * file).
 */
static void test_downscale_zero_dst_h(void)
{
  /*
   * MC/DC vector 3 for `dst_w == 0 || dst_h == 0`: only dst_h is flipped true
   * (dst_w stays non-zero), proving dst_h independently selects invalid_arg.
   */
  static const uint8_t src[]  = {1U, 2U, 3U, 4U};
  uint8_t              dst[1] = {};
  ra8_err_t            err    = ra8_rabook_gray4_downscale(src, 2U, 2U, dst, 1U, 0U);
  check(err == k_ra8_err_invalid_arg, "downscale: dst_h=0 returns invalid_arg");
}

/**
 * @test test_downscale_zero_src_h
 * @par MC/DC:
 * Decision: `src_w == 0 || src_h == 0`
 * (2 conditions, function `ra8_rabook_gray4_downscale`; the source guard,
 * reached only with a valid non-zero dst).
 * - V1 src_w=2, src_h=2 -> C1=F,C2=F -> F (samples).
 * - V2 src_w=0, src_h=2 -> C1=T -> T (returns k_ra8_ok, dst zeroed).
 * - V3 src_w=2, src_h=0 -> C2=T -> T (returns k_ra8_ok, dst zeroed).
 * Vectors 1+2 prove C1 (src_w) independent; 1+3 prove C2 (src_h).
 * N+1 = 3 vectors (this test supplies V3, flipping only src_h true; the control
 * V1 is in test_downscale_identity and V2 is in test_downscale_zero_src of this
 * file).
 */
static void test_downscale_zero_src_h(void)
{
  /*
   * MC/DC vector 3 for the src guard `src_w == 0 || src_h == 0` (reached only
   * with a valid dst): only src_h is flipped true (src_w stays non-zero),
   * proving src_h independently selects the ok + zeroed-dst arm.
   */
  static const uint8_t src[]  = {1U};
  uint8_t              dst[4] = {k_t_pack_15_15, k_t_pack_15_15, k_t_pack_15_15, k_t_pack_15_15};

  ra8_err_t err = ra8_rabook_gray4_downscale(src, 2U, 0U, dst, 2U, 2U);
  check(err == k_ra8_ok, "downscale: zero src_h returns ok");
  check(dst[0] == 0U && dst[1] == 0U, "downscale: zero src_h zeroes dst");
}

/* -------------------------------------------------------------------------- */
/* main */
/* -------------------------------------------------------------------------- */

static void s_log_sink(void* ctx, uint8_t byte)
{
  (void)ctx;
  (void)fputc((int)byte, stderr);
}

int main(void)
{
  ra8_log_set_byte_sink(s_log_sink, nullptr);
  printf("=== ra8_rabook_gray4 unit tests ===\n");

  test_dims_no_scale();
  test_dims_exact_edge();
  test_dims_scale_landscape();
  test_dims_scale_portrait();
  test_dims_null_out();
  test_dims_zero_src();
  test_dims_zero_src_h();
  test_dims_zero_max_edge();
  test_dims_clamp_w_to_1();
  test_dims_clamp_h_to_1();

  test_encode_4px_exact_palette();
  test_encode_3px_odd();
  test_encode_1px();
  test_encode_0px();
  test_encode_buffer_too_small();
  test_encode_null_src();
  test_encode_max_nib();

  test_gray8_encode_copies();
  test_gray8_encode_0px();
  test_gray8_encode_buffer_too_small();
  test_gray8_encode_null_src();

  test_downscale_identity();
  test_downscale_2to1_horizontal();
  test_downscale_null_ptr();
  test_downscale_zero_dst();
  test_downscale_zero_dst_h();
  test_downscale_zero_src();
  test_downscale_zero_src_h();

  printf("\n%s: %u/%u passed\n",
         (s_pass == s_total) ? "[PASS] ra8_rabook_gray4" : "[FAIL] ra8_rabook_gray4",
         s_pass,
         s_total);

  return (s_pass == s_total) ? 0 : 1;
}
