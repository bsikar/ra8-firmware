/**
 * @file test_ra_rabook_gray4.c
 * @brief Host unit tests for the gray4 transcode stage (ra_rabook_gray4).
 *
 * @details
 * Verifies three independently-testable pieces of the #149 image transcode:
 *
 *  - @ref ra_rabook_gray4_output_dims -- dimension-clamping arithmetic
 *  - @ref ra_rabook_gray4_encode      -- quantise + nibble-pack
 *  - @ref ra_rabook_gray4_downscale   -- Q16.16 bilinear resample
 *
 * @par MC/DC:
 * Decision: `if (longer <= max_edge)` in ra_rabook_gray4_output_dims
 *   - Vector 1: src_w=100, src_h=50, max_edge=200  -> no scale (false: both conditions true for pass-through)
 *   - Vector 2: src_w=200, src_h=100, max_edge=100 -> scale down (true: longer > max_edge, triggers scale)
 *   Vectors 1+2 independently demonstrate the scale vs. no-scale branch.
 *
 * Decision: `if ((i & 1U) == 0U)` in ra_rabook_gray4_encode (even vs odd pixel)
 *   - Vector 1: i=0 (even) -> high nibble path taken
 *   - Vector 2: i=1 (odd)  -> low nibble path taken
 *
 * Build (standalone -- does not link ra_hal or ra_sim_mmap):
 * @code
 *   clang -std=c2x -DRA_SIMULATOR_MODE \
 *     -I libs/ra_core/inc -I libs/ra_rabook_compile/inc \
 *     tests/test_ra_rabook_gray4.c \
 *     libs/ra_rabook_compile/src/ra_rabook_gray4.c \
 *     libs/ra_core/src/ra_err.c libs/ra_core/src/ra_log.c \
 *     -o /tmp/test_ra_rabook_gray4 && /tmp/test_ra_rabook_gray4
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

#include "ra_err.h"
#include "ra_log.h"
#include "ra_rabook_gray4.h"

/* -------------------------------------------------------------------------- */
/* Helpers */
/* -------------------------------------------------------------------------- */

typedef enum : uint8_t {
  k_test_pass = 0U,
  k_test_fail = 1U,
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
  ra_rabook_gray4_output_dims(100U, 50U, 200U, &ow, &oh);
  check(ow == 100U && oh == 50U, "dims: no scale when within max_edge");
}

static void test_dims_exact_edge(void)
{
  uint16_t ow = 0U;
  uint16_t oh = 0U;
  ra_rabook_gray4_output_dims(1600U, 900U, 1600U, &ow, &oh);
  check(ow == 1600U && oh == 900U, "dims: exact max_edge passes unchanged");
}

static void test_dims_scale_landscape(void)
{
  uint16_t ow = 0U;
  uint16_t oh = 0U;
  ra_rabook_gray4_output_dims(3200U, 1800U, 1600U, &ow, &oh);
  check(ow == 1600U && oh == 900U, "dims: 2x landscape scale down");
}

static void test_dims_scale_portrait(void)
{
  uint16_t ow = 0U;
  uint16_t oh = 0U;
  ra_rabook_gray4_output_dims(800U, 2400U, 1600U, &ow, &oh);
  check(oh <= 1600U, "dims: portrait longer edge clamped");
  check(ow >= 1U, "dims: portrait width at least 1");
}

static void test_dims_null_out(void)
{
  uint16_t ow = 42U;
  uint16_t oh = 42U;
  ra_rabook_gray4_output_dims(100U, 100U, 200U, nullptr, &oh);
  ra_rabook_gray4_output_dims(100U, 100U, 200U, &ow, nullptr);
  check(ow == 42U && oh == 42U, "dims: null out ptr leaves values unchanged");
}

static void test_dims_zero_src(void)
{
  uint16_t ow = 99U;
  uint16_t oh = 99U;
  ra_rabook_gray4_output_dims(0U, 100U, 200U, &ow, &oh);
  check(ow == 0U && oh == 0U, "dims: zero src_w yields (0,0)");
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

  ra_err_t err = ra_rabook_gray4_encode(pixels, 4U, 1U, out, sizeof(out), &out_size);
  check(err == k_ra_ok, "encode: exact palette 4px -- ok");
  check(out_size == 2U, "encode: exact palette 4px -- size");
  check(out[0] == 0x01U, "encode: exact palette 4px -- byte0");
  check(out[1] == 0x23U, "encode: exact palette 4px -- byte1");
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

  ra_err_t err = ra_rabook_gray4_encode(pixels, 3U, 1U, out, sizeof(out), &out_size);
  check(err == k_ra_ok, "encode: odd pixel count -- ok");
  check(out_size == 2U, "encode: odd pixel count -- size");
  check(out[0] == 0x05U, "encode: odd pixel count -- byte0");
  check(out[1] == 0xF0U, "encode: odd pixel count -- byte1 (low nibble zero)");
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
  ra_err_t err = ra_rabook_gray4_encode(&pixel, 1U, 1U, out, sizeof(out), &out_size);
  check(err == k_ra_ok, "encode: single pixel -- ok");
  check(out_size == 1U, "encode: single pixel -- size");
  check(out[0] == 0x80U, "encode: single pixel -- byte (nib=8)");
}

static void test_encode_0px(void)
{
  uint8_t  dummy[1] = {};
  uint32_t out_size = 99U;

  ra_err_t err = ra_rabook_gray4_encode(dummy, 0U, 0U, dummy, sizeof(dummy), &out_size);
  check(err == k_ra_ok && out_size == 0U, "encode: 0 pixels returns 0 bytes");
}

static void test_encode_buffer_too_small(void)
{
  static const uint8_t pixels[] = {0U, 255U, 128U, 64U};
  uint8_t              out[1]   = {};
  uint32_t             out_size = 0U;

  ra_err_t err = ra_rabook_gray4_encode(pixels, 4U, 1U, out, 1U, &out_size);
  check(err == k_ra_err_no_mem, "encode: buffer too small returns no_mem");
}

static void test_encode_null_src(void)
{
  uint8_t  out[4]   = {};
  uint32_t out_size = 0U;
  ra_err_t err      = ra_rabook_gray4_encode(nullptr, 4U, 1U, out, sizeof(out), &out_size);
  check(err == k_ra_err_null_ptr, "encode: null src returns null_ptr");
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
  ra_rabook_gray4_encode(pixels, 3U, 1U, out, sizeof(out), &out_size);
  check(out[0] == 0xFFU && out[1] == 0xF0U, "encode: near-white pixels all saturate to 15");
}

/* -------------------------------------------------------------------------- */
/* downscale tests */
/* -------------------------------------------------------------------------- */

static void test_downscale_identity(void)
{
  static const uint8_t src[]  = {10U, 20U, 30U, 40U};
  uint8_t              dst[4] = {};

  ra_err_t err = ra_rabook_gray4_downscale(src, 2U, 2U, dst, 2U, 2U);
  check(err == k_ra_ok, "downscale: identity 2x2 -- ok");
  check(dst[0] == 10U && dst[1] == 20U && dst[2] == 30U && dst[3] == 40U,
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

  ra_err_t err = ra_rabook_gray4_downscale(src, 4U, 1U, dst, 2U, 1U);
  check(err == k_ra_ok, "downscale: 4x1->2x1 -- ok");
  check(dst[0] == 0U && dst[1] == 128U, "downscale: 4x1->2x1 -- sampled at x=0,2");
}

static void test_downscale_null_ptr(void)
{
  uint8_t buf[4] = {};
  check(ra_rabook_gray4_downscale(nullptr, 2U, 2U, buf, 2U, 2U) == k_ra_err_null_ptr,
        "downscale: null src");
  check(ra_rabook_gray4_downscale(buf, 2U, 2U, nullptr, 2U, 2U) == k_ra_err_null_ptr,
        "downscale: null dst");
}

static void test_downscale_zero_dst(void)
{
  static const uint8_t src[]  = {1U, 2U, 3U, 4U};
  uint8_t              dst[1] = {};
  ra_err_t             err    = ra_rabook_gray4_downscale(src, 2U, 2U, dst, 0U, 1U);
  check(err == k_ra_err_invalid_arg, "downscale: dst_w=0 returns invalid_arg");
}

static void test_downscale_zero_src(void)
{
  static const uint8_t src[]  = {1U};
  uint8_t              dst[4] = {0xFFU, 0xFFU, 0xFFU, 0xFFU};

  ra_err_t err = ra_rabook_gray4_downscale(src, 0U, 2U, dst, 2U, 2U);
  check(err == k_ra_ok, "downscale: zero src_w returns ok");
  check(dst[0] == 0U && dst[1] == 0U, "downscale: zero src_w zeroes dst");
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
  ra_log_set_byte_sink(s_log_sink, nullptr);
  printf("=== ra_rabook_gray4 unit tests ===\n");

  test_dims_no_scale();
  test_dims_exact_edge();
  test_dims_scale_landscape();
  test_dims_scale_portrait();
  test_dims_null_out();
  test_dims_zero_src();

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
  test_downscale_zero_src();

  printf("\n%s: %u/%u passed\n",
         (s_pass == s_total) ? "[PASS] ra_rabook_gray4" : "[FAIL] ra_rabook_gray4",
         s_pass,
         s_total);

  return (s_pass == s_total) ? 0 : 1;
}
