/**
 * @file test_ra8_rabook_raster.c
 * @brief Unit tests for the caller-arena RABOOK raster normalizer.
 *
 * @details Exercises stb/BMP and libwebp decode paths, gray4/gray8 output,
 * geometry clamping, caller-capacity failures, malformed input, and arena drain.
 *
 * [Ring 3 / RABOOK Compiler] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "ra8_book.h"
#include "ra8_err.h"
#include "ra8_rabook_raster.h"
#include "ra8_reflow_image.h"
#include "ra8_webp_arena.h"
#include "unity_minimal.h"

/** @brief Fixed fixture/workspace geometry. */
typedef enum : uint32_t {
  k_test_codec_arena_bytes = 1U << 20U, /**< One MiB per codec arena. */
  k_test_rgba_bytes        = 256U,      /**< 8 x 8 x RGBA fixture.    */
  k_test_gray_bytes        = 64U,       /**< 8 x 8 gray workspace.   */
  k_test_encoded_bytes     = 64U,       /**< Largest gray8 output.   */
  k_test_webp_dim          = 8U,        /**< WebP fixture edge.       */
  k_test_bmp_dim           = 2U,        /**< BMP fixture edge.        */
} test_raster_limits_t;

/** @brief 2x2 24-bit BMP: top row red/green, bottom row blue/white. */
static const uint8_t k_bmp[] = {
  0x42, 0x4D, 0x46, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x36, 0x00, 0x00, 0x00,
  0x28, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x00,
  0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x13, 0x0B, 0x00, 0x00,
  0x13, 0x0B, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x00,
  0x00, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0x00, 0x00,
};

/** @brief 8x8 VP8L fixture with deterministic RGB ramps. */
static const uint8_t k_webp[] = {
  0x52, 0x49, 0x46, 0x46, 0x2C, 0x00, 0x00, 0x00, 0x57, 0x45, 0x42, 0x50, 0x56,
  0x50, 0x38, 0x4C, 0x1F, 0x00, 0x00, 0x00, 0x2F, 0x07, 0xC0, 0x01, 0x00, 0xCD,
  0x65, 0x44, 0xFF, 0x63, 0x17, 0x85, 0x28, 0x78, 0xFF, 0x03, 0x42, 0x02, 0xC2,
  0x14, 0xFF, 0x77, 0x6A, 0x0E, 0x0C, 0x48, 0xC4, 0x04, 0x80, 0xAD, 0x0D, 0x00,
};

alignas(16) static uint8_t s_stb_scratch[k_test_codec_arena_bytes];
alignas(16) static uint8_t s_webp_scratch[k_test_codec_arena_bytes];
static uint8_t s_rgba[k_test_rgba_bytes];
static uint8_t s_gray[k_test_gray_bytes];
static uint8_t s_encoded[k_test_encoded_bytes];

/**
 * @brief Construct fresh codec arenas and one workspace.
 * @details Rebinds both arena descriptors to independent static storage and
 *          exposes the fixed RGBA/downscale buffers used by every test.
 * @param[out] stb Descriptor receiving the stb backing store.
 * @param[out] webp Descriptor receiving the WebP backing store.
 * @return Fully initialized raster workspace referencing both descriptors.
 * @retval ra8_rabook_raster_workspace_t Workspace with maximum test capacity.
 * @pre @p stb and @p webp are non-NULL and exclusively owned by the test.
 * @pre Shared static backing buffers are not concurrently accessed.
 * @post Both arenas are empty and all workspace buffers are live.
 * @post Workspace capacities equal the corresponding fixture buffer sizes.
 * @note Not thread-safe because the fixture buffers are shared statics.
 * @since 0.1.0
 */
static ra8_rabook_raster_workspace_t fresh_workspace(ra8_img_arena_t* stb, ra8_webp_arena_t* webp)
{
  *stb =
    (ra8_img_arena_t){.base = s_stb_scratch, .cap = sizeof s_stb_scratch, .offset = 0U, .live = 0U};
  *webp = (ra8_webp_arena_t){.base   = s_webp_scratch,
                             .cap    = sizeof s_webp_scratch,
                             .offset = 0U,
                             .live   = 0U};
  return (ra8_rabook_raster_workspace_t){.stb_arena  = stb,
                                         .webp_arena = webp,
                                         .rgba       = s_rgba,
                                         .rgba_cap   = sizeof s_rgba,
                                         .gray       = s_gray,
                                         .gray_cap   = sizeof s_gray};
}

/**
 * @test test_bmp_gray4_and_clamp
 * @brief BMP pixels normalize to stb-compatible gray4 and clamp to gray8.
 * @details Checks the exact luminance/nibble bytes at native geometry, then a
 *          one-pixel gray8 clamp and complete stb arena drain.
 * @pre Shared fixture buffers are not in use by another test.
 * @pre The embedded BMP fixture remains byte-identical to its declaration.
 * @post Both conversions succeed and the image arena is empty.
 * @post The exact gray4 and clamped gray8 bytes match their goldens.
 * @note Not thread-safe because the fixture buffers are shared statics.
 * @since 0.1.0
 */
static void test_bmp_gray4_and_clamp(void)
{
  TEST_BEGIN("rabook raster: BMP gray4 + clamp");
  ra8_img_arena_t               stb  = {};
  ra8_webp_arena_t              webp = {};
  ra8_rabook_raster_workspace_t ws   = fresh_workspace(&stb, &webp);
  ra8_rabook_raster_result_t    got  = {};

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_rabook_raster_encode(k_bmp,
                                          sizeof k_bmp,
                                          0U,
                                          (uint8_t)k_ra8_book_pixfmt_gray4,
                                          &ws,
                                          s_encoded,
                                          sizeof s_encoded,
                                          &got));
  TEST_ASSERT_EQ(k_test_bmp_dim, got.width);
  TEST_ASSERT_EQ(k_test_bmp_dim, got.height);
  TEST_ASSERT_EQ(2U, got.encoded_size);
  TEST_ASSERT_EQ(0x49U, s_encoded[0]);
  TEST_ASSERT_EQ(0x2FU, s_encoded[1]);
  TEST_ASSERT_EQ(0U, stb.live);
  TEST_ASSERT_EQ(0U, stb.offset);

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_rabook_raster_encode(k_bmp,
                                          sizeof k_bmp,
                                          1U,
                                          (uint8_t)k_ra8_book_pixfmt_gray8,
                                          &ws,
                                          s_encoded,
                                          sizeof s_encoded,
                                          &got));
  TEST_ASSERT_EQ(1U, got.width);
  TEST_ASSERT_EQ(1U, got.height);
  TEST_ASSERT_EQ(1U, got.encoded_size);
  TEST_ASSERT_EQ(76U, s_encoded[0]);
  TEST_END("rabook raster: BMP gray4 + clamp");
}

/**
 * @test test_webp_gray8_and_arena_drain
 * @brief WebP normalization uses deterministic luminance and drains its arena.
 * @details Verifies geometry, byte count, the first two expected luminance
 *          samples, and the libwebp arena's live/offset postconditions.
 * @pre Shared fixture buffers are not in use by another test.
 * @pre The embedded WebP fixture remains byte-identical to its declaration.
 * @post The lossless fixture has a complete gray8 frame and drained arena.
 * @post Geometry and the first two luminance bytes match their goldens.
 * @note Not thread-safe because the fixture buffers are shared statics.
 * @since 0.1.0
 */
static void test_webp_gray8_and_arena_drain(void)
{
  TEST_BEGIN("rabook raster: WebP gray8 + arena drain");
  ra8_img_arena_t               stb  = {};
  ra8_webp_arena_t              webp = {};
  ra8_rabook_raster_workspace_t ws   = fresh_workspace(&stb, &webp);
  ra8_rabook_raster_result_t    got  = {};

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_rabook_raster_encode(k_webp,
                                          sizeof k_webp,
                                          0U,
                                          (uint8_t)k_ra8_book_pixfmt_gray8,
                                          &ws,
                                          s_encoded,
                                          sizeof s_encoded,
                                          &got));
  TEST_ASSERT_EQ(k_test_webp_dim, got.width);
  TEST_ASSERT_EQ(k_test_webp_dim, got.height);
  TEST_ASSERT_EQ(k_test_gray_bytes, got.encoded_size);
  TEST_ASSERT_EQ(0U, s_encoded[0]);
  TEST_ASSERT_EQ(11U, s_encoded[1]);
  TEST_ASSERT_EQ(0U, webp.live);
  TEST_ASSERT_EQ(0U, webp.offset);
  TEST_END("rabook raster: WebP gray8 + arena drain");
}

/**
 * @test test_validation_and_capacity
 * @brief Invalid parameters, malformed data, and undersized storage fail closed.
 * @details Drives null, empty, invalid-format, malformed-codec, short encoded
 *          output, and short WebP RGBA storage independently.
 * @pre Shared fixture buffers are not in use by another test.
 * @pre The success fixtures are valid controls for the capacity vectors.
 * @post Every error is explicit and the result byte count remains zero.
 * @post No undersized buffer is treated as a successful conversion.
 * @note Not thread-safe because the fixture buffers are shared statics.
 * @since 0.1.0
 */
static void test_validation_and_capacity(void)
{
  TEST_BEGIN("rabook raster: validation + capacity");
  ra8_img_arena_t               stb   = {};
  ra8_webp_arena_t              webp  = {};
  ra8_rabook_raster_workspace_t ws    = fresh_workspace(&stb, &webp);
  ra8_rabook_raster_result_t    got   = {.encoded_size = UINT32_MAX};
  static const uint8_t          bad[] = {0x01U, 0x02U, 0x03U};

  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_rabook_raster_encode(nullptr,
                                          sizeof bad,
                                          0U,
                                          (uint8_t)k_ra8_book_pixfmt_gray4,
                                          &ws,
                                          s_encoded,
                                          sizeof s_encoded,
                                          &got));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_rabook_raster_encode(bad,
                                          0U,
                                          0U,
                                          (uint8_t)k_ra8_book_pixfmt_gray4,
                                          &ws,
                                          s_encoded,
                                          sizeof s_encoded,
                                          &got));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_rabook_raster_encode(bad,
                                          sizeof bad,
                                          0U,
                                          UINT8_MAX,
                                          &ws,
                                          s_encoded,
                                          sizeof s_encoded,
                                          &got));
  TEST_ASSERT_EQ(k_ra8_err_validation_failed,
                 ra8_rabook_raster_encode(bad,
                                          sizeof bad,
                                          0U,
                                          (uint8_t)k_ra8_book_pixfmt_gray4,
                                          &ws,
                                          s_encoded,
                                          sizeof s_encoded,
                                          &got));
  TEST_ASSERT_EQ(k_ra8_err_no_mem,
                 ra8_rabook_raster_encode(k_bmp,
                                          sizeof k_bmp,
                                          0U,
                                          (uint8_t)k_ra8_book_pixfmt_gray4,
                                          &ws,
                                          s_encoded,
                                          1U,
                                          &got));
  TEST_ASSERT_EQ(0U, got.encoded_size);

  ws.rgba_cap = 1U;
  TEST_ASSERT_EQ(k_ra8_err_no_mem,
                 ra8_rabook_raster_encode(k_webp,
                                          sizeof k_webp,
                                          0U,
                                          (uint8_t)k_ra8_book_pixfmt_gray8,
                                          &ws,
                                          s_encoded,
                                          sizeof s_encoded,
                                          &got));
  TEST_END("rabook raster: validation + capacity");
}

int main(void)
{
  test_bmp_gray4_and_clamp();
  test_webp_gray8_and_arena_drain();
  test_validation_and_capacity();
  (void)printf("[OK] test_ra8_rabook_raster.c\n");
  return 0;
}
