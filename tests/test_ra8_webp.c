/**
 * @file test_ra8_webp.c
 * @brief Host unit + MC/DC tests for the ra8_webp decode facade (#290).
 *
 * @details
 * Exercises `ra8_webp_get_info()` / `ra8_webp_decode_rgba()` (libs/ra8_webp)
 * and the heap-free `ra8_webp_arena` that fronts the vendored libwebp decoder.
 * The golden case decodes a committed 8x8 lossless (VP8L) WebP -- whose RGB is
 * bit-exact -- and compares every pixel against the deterministic source
 * pattern `((x*32)&255, (y*32)&255, ((x+y)*16)&255, 255)`. Error and MC/DC
 * cases drive the validation decisions and the arena allocator.
 *
 * The fixtures are the four committed files under tests/fixtures/webp/; their
 * bytes are embedded inline here (they are tiny) so the test needs no runtime
 * file I/O. See tests/fixtures/webp/README.md for their provenance.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * [Ring 4 / WebP]
 * {World: NS}
 *
 * @since 0.1.0
 */

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "ra8_err.h"
#include "ra8_webp.h"
#include "ra8_webp_arena.h"
#include "unity_minimal.h"

/* ------------------------------------------------------------------------- */
/* Committed fixtures under tests/fixtures/webp/, embedded inline. */
/* ------------------------------------------------------------------------- */

/** 8x8 VP8L (lossless, -exact) -- golden pixel round-trip. */
static const uint8_t k_webp_lossless[] = {
  0x52, 0x49, 0x46, 0x46, 0x2C, 0x00, 0x00, 0x00, 0x57, 0x45, 0x42, 0x50, 0x56,
  0x50, 0x38, 0x4C, 0x1F, 0x00, 0x00, 0x00, 0x2F, 0x07, 0xC0, 0x01, 0x00, 0xCD,
  0x65, 0x44, 0xFF, 0x63, 0x17, 0x85, 0x28, 0x78, 0xFF, 0x03, 0x42, 0x02, 0xC2,
  0x14, 0xFF, 0x77, 0x6A, 0x0E, 0x0C, 0x48, 0xC4, 0x04, 0x80, 0xAD, 0x0D, 0x00,
};

/** 8x8 VP8 (lossy) -- proves the lossy decode path runs. */
static const uint8_t k_webp_lossy[] = {
  0x52, 0x49, 0x46, 0x46, 0x4C, 0x00, 0x00, 0x00, 0x57, 0x45, 0x42, 0x50, 0x56, 0x50,
  0x38, 0x20, 0x40, 0x00, 0x00, 0x00, 0xD0, 0x01, 0x00, 0x9D, 0x01, 0x2A, 0x08, 0x00,
  0x08, 0x00, 0x01, 0x40, 0x26, 0x25, 0xA8, 0x02, 0x74, 0x01, 0x0F, 0x0C, 0x06, 0xC5,
  0xE0, 0x00, 0xFE, 0xFC, 0xD2, 0xFE, 0x92, 0x7B, 0xB2, 0xF7, 0x72, 0xC7, 0x79, 0xA0,
  0xF3, 0x66, 0x26, 0x95, 0xF3, 0x4F, 0xFF, 0xF5, 0x1F, 0xCD, 0x05, 0xC9, 0x25, 0xFE,
  0xFF, 0x9F, 0xB4, 0x46, 0x43, 0xFF, 0x80, 0xFF, 0xA2, 0x97, 0x0C, 0x00, 0x00, 0x00,
};

/** 8200x2 VP8L (solid) -- width exceeds the per-axis dimension cap. */
static const uint8_t k_webp_wide[] = {
  0x52, 0x49, 0x46, 0x46, 0x18, 0x00, 0x00, 0x00, 0x57, 0x45, 0x42, 0x50, 0x56, 0x50, 0x38, 0x4C,
  0x0C, 0x00, 0x00, 0x00, 0x2F, 0x07, 0x60, 0x00, 0x00, 0x28, 0x45, 0x15, 0xEA, 0xD1, 0xFF, 0x00,
};

/** 2x8200 VP8L (solid) -- height exceeds the per-axis dimension cap. */
static const uint8_t k_webp_tall[] = {
  0x52, 0x49, 0x46, 0x46, 0x18, 0x00, 0x00, 0x00, 0x57, 0x45, 0x42, 0x50, 0x56, 0x50, 0x38, 0x4C,
  0x0C, 0x00, 0x00, 0x00, 0x2F, 0x01, 0xC0, 0x01, 0x08, 0x28, 0x45, 0x15, 0xEA, 0xD1, 0xFF, 0x00,
};

/**
 * @enum test_webp_consts_t
 * @brief Geometry + buffer constants for the 8x8 fixtures (no magic numbers).
 */
typedef enum : uint32_t {
  k_dim       = 8U,               /**< Fixture width and height, pixels.    */
  k_bpp       = 4U,               /**< RGBA8888 bytes per pixel.            */
  k_stride    = k_dim * k_bpp,    /**< Tight row stride in bytes (32).      */
  k_fb_bytes  = k_dim * k_stride, /**< 8x8 RGBA framebuffer size (256).     */
  k_alpha_opq = 255U,             /**< Opaque alpha for the golden pattern. */
} test_webp_consts_t;

/** 1 MiB scratch backing store shared by the decode tests. */
alignas(16) static uint8_t s_scratch[1U << 20];

/** @brief Fresh full-size arena over ::s_scratch. */
static ra8_webp_arena_t fresh_arena(void)
{
  return (ra8_webp_arena_t){.base = s_scratch, .cap = sizeof s_scratch, .offset = 0U, .live = 0U};
}

/* ------------------------------------------------------------------------- */
/* Golden decode + arena drain. */
/* ------------------------------------------------------------------------- */

/**
 * @test test_decode_lossless_golden
 * @brief A lossless 8x8 WebP decodes bit-exact to the source pattern, and the
 *        arena fully drains (heap-free path).
 * @since 0.1.0
 */
static void test_decode_lossless_golden(void)
{
  TEST_BEGIN("ra8_webp_decode_rgba: lossless golden + arena drain");
  ra8_webp_arena_t arena          = fresh_arena();
  uint8_t          fb[k_fb_bytes] = {};
  uint32_t         w              = 0U;
  uint32_t         h              = 0U;

  const ra8_err_t e = ra8_webp_decode_rgba(k_webp_lossless,
                                           sizeof k_webp_lossless,
                                           &arena,
                                           fb,
                                           k_stride,
                                           sizeof fb,
                                           &w,
                                           &h);
  TEST_ASSERT_EQ(k_ra8_ok, e);
  TEST_ASSERT_EQ(k_dim, w);
  TEST_ASSERT_EQ(k_dim, h);

  for (uint32_t y = 0U; y < k_dim; ++y) {
    for (uint32_t x = 0U; x < k_dim; ++x) {
      const uint8_t* const p  = &fb[(y * k_stride) + (x * k_bpp)];
      const uint8_t        er = (uint8_t)((x * 32U) & 0xFFU);
      const uint8_t        eg = (uint8_t)((y * 32U) & 0xFFU);
      const uint8_t        eb = (uint8_t)(((x + y) * 16U) & 0xFFU);
      TEST_ASSERT_EQ(er, p[0]);
      TEST_ASSERT_EQ(eg, p[1]);
      TEST_ASSERT_EQ(eb, p[2]);
      TEST_ASSERT_EQ(k_alpha_opq, p[3]);
    }
  }

  /* The bump arena must have fully drained after the decode (NASA P10 R3). */
  TEST_ASSERT_EQ(0, arena.live);
  TEST_ASSERT_EQ(0, arena.offset);
  TEST_END("ra8_webp_decode_rgba: lossless golden + arena drain");
}

/**
 * @test test_decode_lossy_runs
 * @brief A lossy 8x8 WebP decodes without error to the correct geometry.
 * @details Pixels are not bit-compared (lossy re-encoding perturbs them); the
 *          check is that the VP8 + YUV upsampling path completes cleanly.
 * @since 0.1.0
 */
static void test_decode_lossy_runs(void)
{
  TEST_BEGIN("ra8_webp_decode_rgba: lossy path runs");
  ra8_webp_arena_t arena          = fresh_arena();
  uint8_t          fb[k_fb_bytes] = {};
  uint32_t         w              = 0U;
  uint32_t         h              = 0U;

  const ra8_err_t e = ra8_webp_decode_rgba(k_webp_lossy,
                                           sizeof k_webp_lossy,
                                           &arena,
                                           fb,
                                           k_stride,
                                           sizeof fb,
                                           &w,
                                           &h);
  TEST_ASSERT_EQ(k_ra8_ok, e);
  TEST_ASSERT_EQ(k_dim, w);
  TEST_ASSERT_EQ(k_dim, h);
  TEST_ASSERT_EQ(0, arena.live);
  TEST_END("ra8_webp_decode_rgba: lossy path runs");
}

/* ------------------------------------------------------------------------- */
/* ra8_webp_get_info validation. */
/* ------------------------------------------------------------------------- */

/**
 * @test test_get_info_ok_and_null_guards
 * @brief get_info returns dims for a valid header and rejects NULL args / empty.
 * @since 0.1.0
 */
static void test_get_info_ok_and_null_guards(void)
{
  TEST_BEGIN("ra8_webp_get_info: ok + null/empty guards");
  uint32_t w = 0U;
  uint32_t h = 0U;

  TEST_ASSERT_EQ(k_ra8_ok, ra8_webp_get_info(k_webp_lossless, sizeof k_webp_lossless, &w, &h));
  TEST_ASSERT_EQ(k_dim, w);
  TEST_ASSERT_EQ(k_dim, h);

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_webp_get_info(nullptr, 4U, &w, &h));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_webp_get_info(k_webp_lossless, sizeof k_webp_lossless, nullptr, &h));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_webp_get_info(k_webp_lossless, sizeof k_webp_lossless, &w, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_webp_get_info(k_webp_lossless, 0U, &w, &h));
  TEST_END("ra8_webp_get_info: ok + null/empty guards");
}

/**
 * @test test_get_info_garbage
 * @brief Non-WebP bytes are rejected as validation failures.
 * @since 0.1.0
 */
static void test_get_info_garbage(void)
{
  TEST_BEGIN("ra8_webp_get_info: garbage rejected");
  static const uint8_t garbage[] = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x11, 0x22, 0x33};
  uint32_t             w         = 0U;
  uint32_t             h         = 0U;
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, ra8_webp_get_info(garbage, sizeof garbage, &w, &h));
  TEST_END("ra8_webp_get_info: garbage rejected");
}

/**
 * @test test_get_info_dimension_cap_mcdc
 * @brief The per-axis dimension cap rejects oversized WebP headers.
 *
 * @par MC/DC:
 * Decision: `if ((w > k_ra8_webp_max_dim) || (h > k_ra8_webp_max_dim))`
 * (2 conditions, ra8_webp.c ra8_webp_get_info).
 *  - V1: 8x8       -> (F,F) -> k_ra8_ok            (control; both in-range).
 *  - V2: 8200x2    -> (T,F) -> k_ra8_err_not_supported (width varies only).
 *  - V3: 2x8200    -> (F,T) -> k_ra8_err_not_supported (height varies only).
 * V1+V2 prove width independently drives the outcome; V1+V3 prove the same for
 * height. N+1 = 3 vectors for N=2 conditions: minimal MC/DC.
 * @since 0.1.0
 */
static void test_get_info_dimension_cap_mcdc(void)
{
  TEST_BEGIN("ra8_webp_get_info: dimension cap MC/DC");
  uint32_t w = 0U;
  uint32_t h = 0U;

  /* V1 (F,F): both dimensions within the cap. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_webp_get_info(k_webp_lossless, sizeof k_webp_lossless, &w, &h));

  /* V2 (T,F): width 8200 > cap, height 2 <= cap. */
  TEST_ASSERT_EQ(k_ra8_err_not_supported,
                 ra8_webp_get_info(k_webp_wide, sizeof k_webp_wide, &w, &h));

  /* V3 (F,T): width 2 <= cap, height 8200 > cap. */
  TEST_ASSERT_EQ(k_ra8_err_not_supported,
                 ra8_webp_get_info(k_webp_tall, sizeof k_webp_tall, &w, &h));
  TEST_END("ra8_webp_get_info: dimension cap MC/DC");
}

/* ------------------------------------------------------------------------- */
/* ra8_webp_decode_rgba validation. */
/* ------------------------------------------------------------------------- */

/**
 * @test test_decode_null_guards
 * @brief decode_rgba rejects NULL data / arena / output buffer.
 * @since 0.1.0
 */
static void test_decode_null_guards(void)
{
  TEST_BEGIN("ra8_webp_decode_rgba: null guards");
  ra8_webp_arena_t arena          = fresh_arena();
  uint8_t          fb[k_fb_bytes] = {};

  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    ra8_webp_decode_rgba(nullptr, 4U, &arena, fb, k_stride, sizeof fb, nullptr, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_webp_decode_rgba(k_webp_lossless,
                                      sizeof k_webp_lossless,
                                      nullptr,
                                      fb,
                                      k_stride,
                                      sizeof fb,
                                      nullptr,
                                      nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_webp_decode_rgba(k_webp_lossless,
                                      sizeof k_webp_lossless,
                                      &arena,
                                      nullptr,
                                      k_stride,
                                      sizeof fb,
                                      nullptr,
                                      nullptr));
  TEST_END("ra8_webp_decode_rgba: null guards");
}

/**
 * @test test_decode_bad_header_propagates
 * @brief A header error from get_info propagates out of decode_rgba unchanged.
 * @details Feeding an oversized (8200x2) header makes the internal get_info
 *          return k_ra8_err_not_supported, which RA8_RETURN_ON_ERROR forwards.
 * @since 0.1.0
 */
static void test_decode_bad_header_propagates(void)
{
  TEST_BEGIN("ra8_webp_decode_rgba: header error propagates");
  ra8_webp_arena_t arena          = fresh_arena();
  uint8_t          fb[k_fb_bytes] = {};
  TEST_ASSERT_EQ(k_ra8_err_not_supported,
                 ra8_webp_decode_rgba(k_webp_wide,
                                      sizeof k_webp_wide,
                                      &arena,
                                      fb,
                                      k_stride,
                                      sizeof fb,
                                      nullptr,
                                      nullptr));
  TEST_END("ra8_webp_decode_rgba: header error propagates");
}

/**
 * @test test_decode_output_size_mcdc
 * @brief The output-buffer sizing guard rejects too-small stride / capacity.
 *
 * @par MC/DC:
 * Decision: `if ((out_stride < min_stride) || ((h * out_stride) > out_capacity))`
 * (2 conditions, ra8_webp.c ra8_webp_decode_rgba; min_stride = w*4 = 32).
 *  - V1: stride=32, capacity=256 -> (F,F) -> decode proceeds (k_ra8_ok).
 *  - V2: stride=16, capacity=256 -> (T,F) -> k_ra8_err_range_check_failed
 *        (stride < 32, but 8*16=128 <= 256; stride varies only).
 *  - V3: stride=32, capacity=100 -> (F,T) -> k_ra8_err_range_check_failed
 *        (stride ok, but 8*32=256 > 100; capacity varies only).
 * V1+V2 prove stride independently drives the outcome; V1+V3 prove the same for
 * capacity. N+1 = 3 vectors for N=2 conditions: minimal MC/DC.
 * @since 0.1.0
 */
static void test_decode_output_size_mcdc(void)
{
  TEST_BEGIN("ra8_webp_decode_rgba: output-size MC/DC");
  uint8_t fb[k_fb_bytes] = {};

  /* V1 (F,F): correct stride + capacity -> decodes. */
  ra8_webp_arena_t a1 = fresh_arena();
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_webp_decode_rgba(k_webp_lossless,
                                      sizeof k_webp_lossless,
                                      &a1,
                                      fb,
                                      k_stride,
                                      sizeof fb,
                                      nullptr,
                                      nullptr));

  /* V2 (T,F): stride below width*4. */
  ra8_webp_arena_t a2 = fresh_arena();
  TEST_ASSERT_EQ(k_ra8_err_range_check_failed,
                 ra8_webp_decode_rgba(k_webp_lossless,
                                      sizeof k_webp_lossless,
                                      &a2,
                                      fb,
                                      (size_t)k_bpp * 4U,
                                      sizeof fb,
                                      nullptr,
                                      nullptr));

  /* V3 (F,T): capacity below height*stride. */
  ra8_webp_arena_t a3 = fresh_arena();
  TEST_ASSERT_EQ(k_ra8_err_range_check_failed,
                 ra8_webp_decode_rgba(k_webp_lossless,
                                      sizeof k_webp_lossless,
                                      &a3,
                                      fb,
                                      k_stride,
                                      100U,
                                      nullptr,
                                      nullptr));
  TEST_END("ra8_webp_decode_rgba: output-size MC/DC");
}

/**
 * @test test_decode_stride_over_int_max
 * @brief A stride exceeding INT_MAX is rejected before the libwebp `int` cast.
 * @details Passes a dishonestly large capacity so the size guard passes, then a
 *          stride > INT_MAX trips the dedicated `out_stride > INT_MAX` check.
 *          The output buffer is never written (the guard returns first).
 * @since 0.1.0
 */
static void test_decode_stride_over_int_max(void)
{
  TEST_BEGIN("ra8_webp_decode_rgba: stride > INT_MAX rejected");
  ra8_webp_arena_t arena          = fresh_arena();
  uint8_t          fb[k_fb_bytes] = {};
  const size_t     huge_stride    = (size_t)INT_MAX + 1U;
  const size_t     huge_cap       = (size_t)1 << 40; /* > h*huge_stride; never allocated */
  TEST_ASSERT_EQ(k_ra8_err_range_check_failed,
                 ra8_webp_decode_rgba(k_webp_lossless,
                                      sizeof k_webp_lossless,
                                      &arena,
                                      fb,
                                      huge_stride,
                                      huge_cap,
                                      nullptr,
                                      nullptr));
  TEST_END("ra8_webp_decode_rgba: stride > INT_MAX rejected");
}

/**
 * @test test_decode_arena_oom
 * @brief A too-small arena makes libwebp's allocator fail, so the decode fails
 *        cleanly (validation error) rather than corrupting memory.
 * @since 0.1.0
 */
static void test_decode_arena_oom(void)
{
  TEST_BEGIN("ra8_webp_decode_rgba: arena OOM -> clean failure");
  static uint8_t   s_tiny[64U];
  ra8_webp_arena_t arena = {.base = s_tiny, .cap = sizeof s_tiny, .offset = 0U, .live = 0U};
  uint8_t          fb[k_fb_bytes] = {};
  TEST_ASSERT_EQ(k_ra8_err_validation_failed,
                 ra8_webp_decode_rgba(k_webp_lossless,
                                      sizeof k_webp_lossless,
                                      &arena,
                                      fb,
                                      k_stride,
                                      sizeof fb,
                                      nullptr,
                                      nullptr));
  /* Even on the failed decode the arena drains (all partial scratch freed). */
  TEST_ASSERT_EQ(0, arena.live);
  TEST_END("ra8_webp_decode_rgba: arena OOM -> clean failure");
}

/* ------------------------------------------------------------------------- */
/* ra8_webp_arena unit + MC/DC. */
/* ------------------------------------------------------------------------- */

/**
 * @test test_arena_malloc_and_bind
 * @brief Covers the malloc capacity guards and bind(NULL)/unbind paths.
 * @since 0.1.0
 */
static void test_arena_malloc_and_bind(void)
{
  TEST_BEGIN("ra8_webp_arena: malloc guards + bind/unbind");

  /* No arena bound -> malloc/calloc fail. */
  ra8_webp_arena_unbind();
  TEST_ASSERT_NULL(ra8_webp_arena_malloc(16U));
  TEST_ASSERT_NULL(ra8_webp_arena_calloc(4U, 4U));

  /* bind(NULL) exercises the "arena == NULL" arm of bind(). */
  ra8_webp_arena_bind(nullptr);
  TEST_ASSERT_NULL(ra8_webp_arena_malloc(16U));

  static uint8_t   s_buf[32U];
  ra8_webp_arena_t a = {.base = s_buf, .cap = sizeof s_buf, .offset = 0U, .live = 0U};
  ra8_webp_arena_bind(&a);

  TEST_ASSERT_NULL(ra8_webp_arena_malloc(33U)); /* n > cap */

  void* const p1 = ra8_webp_arena_malloc(16U);
  TEST_ASSERT_NOT_NULL(p1);
  TEST_ASSERT_EQ(16, a.offset);
  void* const p2 = ra8_webp_arena_malloc(16U);
  TEST_ASSERT_NOT_NULL(p2);
  TEST_ASSERT_EQ(2, a.live);
  TEST_ASSERT_NULL(ra8_webp_arena_malloc(16U)); /* aligned(16) > remaining(0) */

  /* Free one of two live blocks: live drops to 1 (not 0), so the offset is
   * NOT rewound -- covers the false arm of `if (a->live == 0U)`. */
  ra8_webp_arena_free(p1);
  TEST_ASSERT_EQ(1, a.live);
  TEST_ASSERT_EQ(32, a.offset);
  ra8_webp_arena_free(p2);
  TEST_ASSERT_EQ(0, a.live);
  TEST_ASSERT_EQ(0, a.offset);

  /* Extra free past drain (unbalanced): the `a->live > 0U` guard prevents the
   * live counter from underflowing -- covers that guard's false arm. */
  ra8_webp_arena_free(p2);
  TEST_ASSERT_EQ(0, a.live);

  ra8_webp_arena_unbind();
  TEST_END("ra8_webp_arena: malloc guards + bind/unbind");
}

/**
 * @test test_arena_calloc_overflow_mcdc
 * @brief The calloc overflow guard rejects a wrapping product, else zero-fills.
 *
 * @par MC/DC:
 * Decision: `if ((size != 0U) && (nmemb > (SIZE_MAX / size)))`
 * (2 conditions, ra8_webp_arena.c ra8_webp_arena_calloc).
 *  - V1: size=2,       nmemb=SIZE_MAX -> (T,T) -> NULL (overflow rejected).
 *  - V2: size=4,       nmemb=4        -> (T,F) -> zeroed block (no overflow).
 *  - V3: size=0,       nmemb=8        -> (F,-) -> malloc(0) block (short-circuit).
 * V1+V2 prove the overflow term independently drives the outcome; V2/V3 vary
 * `size != 0`. `&&` short-circuits so V3 leaves the second condition unevaluated.
 * @since 0.1.0
 */
static void test_arena_calloc_overflow_mcdc(void)
{
  TEST_BEGIN("ra8_webp_arena_calloc: overflow MC/DC");
  static uint8_t   s_buf[64U];
  ra8_webp_arena_t a = {.base = s_buf, .cap = sizeof s_buf, .offset = 0U, .live = 0U};
  ra8_webp_arena_bind(&a);

  /* V1 (T,T): product overflows size_t. */
  TEST_ASSERT_NULL(ra8_webp_arena_calloc(SIZE_MAX, 2U));

  /* V2 (T,F): valid product; block must be zeroed. */
  uint8_t* const z = (uint8_t*)ra8_webp_arena_calloc(4U, 4U);
  TEST_ASSERT_NOT_NULL(z);
  for (uint32_t i = 0U; i < 16U; ++i) {
    TEST_ASSERT_EQ(0, z[i]);
  }

  /* V3 (F,-): size == 0 short-circuits; zero-length block, no memset. */
  TEST_ASSERT_NOT_NULL(ra8_webp_arena_calloc(8U, 0U));

  ra8_webp_arena_unbind();
  TEST_END("ra8_webp_arena_calloc: overflow MC/DC");
}

/**
 * @test test_arena_free_mcdc
 * @brief The free null-guard: no-op on NULL pointer or unbound arena.
 *
 * @par MC/DC:
 * Decision: `if ((p == nullptr) || (a == nullptr))`
 * (2 conditions, ra8_webp_arena.c ra8_webp_arena_free).
 *  - V1: valid p, arena bound -> (F,F) -> live decrements.
 *  - V2: p=NULL, arena bound  -> (T,F) -> no-op (p varies only).
 *  - V3: valid p, arena NULL  -> (F,T) -> no-op (bound-arena varies only).
 * V1+V2 prove the pointer term independently drives the outcome; V1+V3 prove
 * the same for the bound-arena term. N+1 = 3 vectors for N=2 conditions.
 * @since 0.1.0
 */
static void test_arena_free_mcdc(void)
{
  TEST_BEGIN("ra8_webp_arena_free: null-guard MC/DC");
  static uint8_t   s_buf[64U];
  ra8_webp_arena_t a = {.base = s_buf, .cap = sizeof s_buf, .offset = 0U, .live = 0U};
  ra8_webp_arena_bind(&a);

  void* const p = ra8_webp_arena_malloc(16U);
  TEST_ASSERT_NOT_NULL(p);
  TEST_ASSERT_EQ(1, a.live);

  /* V1 (F,F): valid pointer + bound arena -> live decrements, offset rewinds. */
  ra8_webp_arena_free(p);
  TEST_ASSERT_EQ(0, a.live);
  TEST_ASSERT_EQ(0, a.offset);

  /* V2 (T,F): NULL pointer + bound arena -> no change. */
  void* const q = ra8_webp_arena_malloc(16U);
  TEST_ASSERT_NOT_NULL(q);
  TEST_ASSERT_EQ(1, a.live);
  ra8_webp_arena_free(nullptr);
  TEST_ASSERT_EQ(1, a.live);

  /* V3 (F,T): valid pointer + no bound arena -> no change to `a`. */
  ra8_webp_arena_unbind();
  ra8_webp_arena_free(q);
  TEST_ASSERT_EQ(1, a.live);
  TEST_END("ra8_webp_arena_free: null-guard MC/DC");
}

/**
 * @brief Test entry point.
 * @return 0 on success; unity macros call exit(1) on the first failure.
 * @since 0.1.0
 */
int32_t main(void)
{
  test_decode_lossless_golden();
  test_decode_lossy_runs();
  test_get_info_ok_and_null_guards();
  test_get_info_garbage();
  test_get_info_dimension_cap_mcdc();
  test_decode_null_guards();
  test_decode_bad_header_propagates();
  test_decode_output_size_mcdc();
  test_decode_stride_over_int_max();
  test_decode_arena_oom();
  test_arena_malloc_and_bind();
  test_arena_calloc_overflow_mcdc();
  test_arena_free_mcdc();
  (void)fprintf(stderr, "[OK ] test_ra8_webp.c\n");
  return 0;
}
