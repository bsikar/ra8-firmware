/**
 * @file test_ra8_rabook_gray4.c
 * @brief Host unit tests for the gray4 transcode stage (ra8_rabook_gray4).
 *
 * @details
 * Verifies three independently-testable pieces of the #149 image transcode:
 *
 *  - @ref ra8_rabook_gray4_output_dims -- dimension-clamping arithmetic
 *  - @ref ra8_rabook_gray4_encode      -- quantise + nibble-pack
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
 * @enum rabook_gray4_uint8_const_t
 * @brief Named uint8_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint8_t {
  k_rabook_gray4_check_05                         = 0x05U,
  k_rabook_gray4_check_10                         = 10U,
  k_rabook_gray4_check_128                        = 128U,
  k_rabook_gray4_check_20                         = 20U,
  k_rabook_gray4_check_23                         = 0x23U,
  k_rabook_gray4_check_30                         = 30U,
  k_rabook_gray4_check_40                         = 40U,
  k_rabook_gray4_check_80                         = 0x80U,
  k_rabook_gray4_check_f0                         = 0xF0U,
  k_rabook_gray4_check_ff                         = 0xFFU,
  k_rabook_gray4_ow_42                            = 42U,
  k_rabook_gray4_ow_99                            = 99U,
  k_rabook_gray4_ra8_rabook_gray4_output_dims_100 = 100U,
  k_rabook_gray4_ra8_rabook_gray4_output_dims_200 = 200U,
  k_rabook_gray4_ra8_rabook_gray4_output_dims_50  = 50U,
} rabook_gray4_uint8_const_t;

/**
 * @enum rabook_gray4_uint16_const_t
 * @brief Named uint16_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint16_t {
  k_rabook_gray4_ra8_rabook_gray4_output_dims_10000 = 10000U,
  k_rabook_gray4_ra8_rabook_gray4_output_dims_1600  = 1600U,
  k_rabook_gray4_ra8_rabook_gray4_output_dims_1800  = 1800U,
  k_rabook_gray4_ra8_rabook_gray4_output_dims_2400  = 2400U,
  k_rabook_gray4_ra8_rabook_gray4_output_dims_3200  = 3200U,
  k_rabook_gray4_ra8_rabook_gray4_output_dims_800   = 800U,
  k_rabook_gray4_ra8_rabook_gray4_output_dims_900   = 900U,
} rabook_gray4_uint16_const_t;

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

static void test_dims_no_scale(void)
{
  uint16_t ow = 0U;
  uint16_t oh = 0U;
  ra8_rabook_gray4_output_dims(k_rabook_gray4_ra8_rabook_gray4_output_dims_100,
                               k_rabook_gray4_ra8_rabook_gray4_output_dims_50,
                               k_rabook_gray4_ra8_rabook_gray4_output_dims_200,
                               &ow,
                               &oh);
  check(ow == k_rabook_gray4_ra8_rabook_gray4_output_dims_100 &&
          oh == k_rabook_gray4_ra8_rabook_gray4_output_dims_50,
        "dims: no scale when within max_edge");
}

static void test_dims_exact_edge(void)
{
  uint16_t ow = 0U;
  uint16_t oh = 0U;
  ra8_rabook_gray4_output_dims(k_rabook_gray4_ra8_rabook_gray4_output_dims_1600,
                               k_rabook_gray4_ra8_rabook_gray4_output_dims_900,
                               k_rabook_gray4_ra8_rabook_gray4_output_dims_1600,
                               &ow,
                               &oh);
  check(ow == k_rabook_gray4_ra8_rabook_gray4_output_dims_1600 &&
          oh == k_rabook_gray4_ra8_rabook_gray4_output_dims_900,
        "dims: exact max_edge passes unchanged");
}

static void test_dims_scale_landscape(void)
{
  uint16_t ow = 0U;
  uint16_t oh = 0U;
  ra8_rabook_gray4_output_dims(k_rabook_gray4_ra8_rabook_gray4_output_dims_3200,
                               k_rabook_gray4_ra8_rabook_gray4_output_dims_1800,
                               k_rabook_gray4_ra8_rabook_gray4_output_dims_1600,
                               &ow,
                               &oh);
  check(ow == k_rabook_gray4_ra8_rabook_gray4_output_dims_1600 &&
          oh == k_rabook_gray4_ra8_rabook_gray4_output_dims_900,
        "dims: 2x landscape scale down");
}

static void test_dims_scale_portrait(void)
{
  uint16_t ow = 0U;
  uint16_t oh = 0U;
  ra8_rabook_gray4_output_dims(k_rabook_gray4_ra8_rabook_gray4_output_dims_800,
                               k_rabook_gray4_ra8_rabook_gray4_output_dims_2400,
                               k_rabook_gray4_ra8_rabook_gray4_output_dims_1600,
                               &ow,
                               &oh);
  check(oh <= k_rabook_gray4_ra8_rabook_gray4_output_dims_1600,
        "dims: portrait longer edge clamped");
  check(ow >= 1U, "dims: portrait width at least 1");
}

static void test_dims_null_out(void)
{
  uint16_t ow = k_rabook_gray4_ow_42;
  uint16_t oh = k_rabook_gray4_ow_42;
  ra8_rabook_gray4_output_dims(k_rabook_gray4_ra8_rabook_gray4_output_dims_100,
                               k_rabook_gray4_ra8_rabook_gray4_output_dims_100,
                               k_rabook_gray4_ra8_rabook_gray4_output_dims_200,
                               nullptr,
                               &oh);
  ra8_rabook_gray4_output_dims(k_rabook_gray4_ra8_rabook_gray4_output_dims_100,
                               k_rabook_gray4_ra8_rabook_gray4_output_dims_100,
                               k_rabook_gray4_ra8_rabook_gray4_output_dims_200,
                               &ow,
                               nullptr);
  check(ow == k_rabook_gray4_ow_42 && oh == k_rabook_gray4_ow_42,
        "dims: null out ptr leaves values unchanged");
}

static void test_dims_zero_src(void)
{
  uint16_t ow = k_rabook_gray4_ow_99;
  uint16_t oh = k_rabook_gray4_ow_99;
  ra8_rabook_gray4_output_dims(0U,
                               k_rabook_gray4_ra8_rabook_gray4_output_dims_100,
                               k_rabook_gray4_ra8_rabook_gray4_output_dims_200,
                               &ow,
                               &oh);
  check(ow == 0U && oh == 0U, "dims: zero src_w yields (0,0)");
}

static void test_dims_zero_src_h(void)
{
  /*
   * MC/DC vector 3 for `src_w == 0 || src_h == 0 || max_edge == 0`: only
   * src_h is flipped true (src_w and max_edge stay non-zero), proving src_h
   * independently drives the decision to the (0,0) arm.
   */
  uint16_t ow = k_rabook_gray4_ow_99;
  uint16_t oh = k_rabook_gray4_ow_99;
  ra8_rabook_gray4_output_dims(k_rabook_gray4_ra8_rabook_gray4_output_dims_100,
                               0U,
                               k_rabook_gray4_ra8_rabook_gray4_output_dims_200,
                               &ow,
                               &oh);
  check(ow == 0U && oh == 0U, "dims: zero src_h yields (0,0)");
}

static void test_dims_zero_max_edge(void)
{
  /*
   * MC/DC vector 4 for `src_w == 0 || src_h == 0 || max_edge == 0`: only
   * max_edge is flipped true (both source dims stay non-zero), proving
   * max_edge independently drives the decision to the (0,0) arm.
   */
  uint16_t ow = k_rabook_gray4_ow_99;
  uint16_t oh = k_rabook_gray4_ow_99;
  ra8_rabook_gray4_output_dims(k_rabook_gray4_ra8_rabook_gray4_output_dims_100,
                               k_rabook_gray4_ra8_rabook_gray4_output_dims_50,
                               0U,
                               &ow,
                               &oh);
  check(ow == 0U && oh == 0U, "dims: zero max_edge yields (0,0)");
}

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
  ra8_rabook_gray4_output_dims(1U, k_rabook_gray4_ra8_rabook_gray4_output_dims_10000, 2U, &ow, &oh);
  check(ow == 1U, "dims: degenerate width clamped up to 1");
  check(oh == 2U, "dims: degenerate-width height is 2");
}

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
  ra8_rabook_gray4_output_dims(k_rabook_gray4_ra8_rabook_gray4_output_dims_10000, 1U, 2U, &ow, &oh);
  check(ow == 2U, "dims: degenerate-height width is 2");
  check(oh == 1U, "dims: degenerate height clamped up to 1");
}

/* -------------------------------------------------------------------------- */
/* encode tests */
/* -------------------------------------------------------------------------- */

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
  check(out[1] == k_rabook_gray4_check_23, "encode: exact palette 4px -- byte1");
}

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
  check(out[0] == k_rabook_gray4_check_05, "encode: odd pixel count -- byte0");
  check(out[1] == k_rabook_gray4_check_f0, "encode: odd pixel count -- byte1 (low nibble zero)");
}

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
  check(out[0] == k_rabook_gray4_check_80, "encode: single pixel -- byte (nib=8)");
}

static void test_encode_0px(void)
{
  uint8_t  dummy[1] = {};
  uint32_t out_size = k_rabook_gray4_ow_99;

  ra8_err_t err = ra8_rabook_gray4_encode(dummy, 0U, 0U, dummy, sizeof(dummy), &out_size);
  check(err == k_ra8_ok && out_size == 0U, "encode: 0 pixels returns 0 bytes");
}

static void test_encode_buffer_too_small(void)
{
  static const uint8_t pixels[] = {0U, 255U, 128U, 64U};
  uint8_t              out[1]   = {};
  uint32_t             out_size = 0U;

  ra8_err_t err = ra8_rabook_gray4_encode(pixels, 4U, 1U, out, 1U, &out_size);
  check(err == k_ra8_err_no_mem, "encode: buffer too small returns no_mem");
}

static void test_encode_null_src(void)
{
  uint8_t   out[4]   = {};
  uint32_t  out_size = 0U;
  ra8_err_t err      = ra8_rabook_gray4_encode(nullptr, 4U, 1U, out, sizeof(out), &out_size);
  check(err == k_ra8_err_null_ptr, "encode: null src returns null_ptr");
}

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
  check(out[0] == k_rabook_gray4_check_ff && out[1] == k_rabook_gray4_check_f0,
        "encode: near-white pixels all saturate to 15");
}

/* -------------------------------------------------------------------------- */
/* downscale tests */
/* -------------------------------------------------------------------------- */

static void test_downscale_identity(void)
{
  static const uint8_t src[]  = {10U, 20U, 30U, 40U};
  uint8_t              dst[4] = {};

  ra8_err_t err = ra8_rabook_gray4_downscale(src, 2U, 2U, dst, 2U, 2U);
  check(err == k_ra8_ok, "downscale: identity 2x2 -- ok");
  check(dst[0] == k_rabook_gray4_check_10 && dst[1] == k_rabook_gray4_check_20 &&
          dst[2] == k_rabook_gray4_check_30 && dst[3] == k_rabook_gray4_check_40,
        "downscale: identity 2x2 -- pixels unchanged");
}

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
  check(dst[0] == 0U && dst[1] == k_rabook_gray4_check_128,
        "downscale: 4x1->2x1 -- sampled at x=0,2");
}

static void test_downscale_null_ptr(void)
{
  uint8_t buf[4] = {};
  check(ra8_rabook_gray4_downscale(nullptr, 2U, 2U, buf, 2U, 2U) == k_ra8_err_null_ptr,
        "downscale: null src");
  check(ra8_rabook_gray4_downscale(buf, 2U, 2U, nullptr, 2U, 2U) == k_ra8_err_null_ptr,
        "downscale: null dst");
}

static void test_downscale_zero_dst(void)
{
  static const uint8_t src[]  = {1U, 2U, 3U, 4U};
  uint8_t              dst[1] = {};
  ra8_err_t            err    = ra8_rabook_gray4_downscale(src, 2U, 2U, dst, 0U, 1U);
  check(err == k_ra8_err_invalid_arg, "downscale: dst_w=0 returns invalid_arg");
}

static void test_downscale_zero_src(void)
{
  static const uint8_t src[]  = {1U};
  uint8_t              dst[4] = {k_rabook_gray4_check_ff,
                                 k_rabook_gray4_check_ff,
                                 k_rabook_gray4_check_ff,
                                 k_rabook_gray4_check_ff};

  ra8_err_t err = ra8_rabook_gray4_downscale(src, 0U, 2U, dst, 2U, 2U);
  check(err == k_ra8_ok, "downscale: zero src_w returns ok");
  check(dst[0] == 0U && dst[1] == 0U, "downscale: zero src_w zeroes dst");
}

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

static void test_downscale_zero_src_h(void)
{
  /*
   * MC/DC vector 3 for the src guard `src_w == 0 || src_h == 0` (reached only
   * with a valid dst): only src_h is flipped true (src_w stays non-zero),
   * proving src_h independently selects the ok + zeroed-dst arm.
   */
  static const uint8_t src[]  = {1U};
  uint8_t              dst[4] = {k_rabook_gray4_check_ff,
                                 k_rabook_gray4_check_ff,
                                 k_rabook_gray4_check_ff,
                                 k_rabook_gray4_check_ff};

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
