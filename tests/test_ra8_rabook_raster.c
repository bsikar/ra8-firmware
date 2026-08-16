/**
 * @file test_ra8_rabook_raster.c
 * @brief Unit tests for the caller-arena RABOOK raster normalizer.
 *
 * @details Exercises stb/BMP and libwebp decode paths, gray4/gray8 output,
 * geometry clamping, caller-capacity failures, malformed input, and arena drain.
 *
 * [Ring 3 / RABOOK_COMPILER] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
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
  k_test_gray_bytes        = 64U,       /**< 8 x 8 gray workspace.    */
  k_test_encoded_bytes     = 64U,       /**< Largest gray8 output.    */
  k_test_webp_dim          = 8U,        /**< WebP fixture edge.       */
  k_test_bmp_dim           = 2U,        /**< BMP fixture edge.        */
} test_raster_limits_t;

/**
 * @brief Geometry constants used only by the decoder-guard fixtures.
 * @details A degenerate edge still decodes: the vendored decoder allocates a
 *          zero-byte frame and reports the declared geometry, which is exactly
 *          the input the raster normalizer's dimension contract must refuse.
 */
typedef enum : uint32_t {
  k_test_bmp_header_bytes = 54U, /**< Fixed BMP file header plus info header.      */
  k_test_bmp_info_bytes   = 40U, /**< BITMAPINFOHEADER size field value.           */
  k_test_bmp_planes       = 1U,  /**< The only plane count stb accepts.            */
  k_test_bmp_bpp_rgb24    = 24U, /**< Uncompressed 24-bit true color.              */
  k_test_dim_single       = 1U,  /**< Single-pixel edge.                           */
  k_test_dim_empty        = 0U,  /**< Degenerate zero edge.                        */
  k_test_cap_overshoot    = 1U,  /**< Bytes past a bounded capacity limit.         */
  k_test_no_bytes         = 0U,  /**< Encoded bytes produced by a rejection.       */
  k_test_arena_drained    = 0U,  /**< Live blocks and offset after a rejection.    */
  k_test_clamp_to_one     = 1U,  /**< Longer-edge clamp forcing a downscale.       */
  k_test_gray4_pair_bytes = 2U,  /**< Gray4 bytes emitted for the 2x2 fixture.     */
} test_raster_geometry_t;

/** @brief Little-endian field offsets and widths inside the fixed BMP header. */
typedef enum : uint8_t {
  k_bmp_off_magic_first  = 0U,  /**< 'B' signature byte.            */
  k_bmp_off_magic_second = 1U,  /**< 'M' signature byte.            */
  k_bmp_off_pixel_data   = 10U, /**< uint32 first-pixel offset.     */
  k_bmp_off_info_size    = 14U, /**< uint32 info-header size.       */
  k_bmp_off_width        = 18U, /**< int32 declared pixel width.    */
  k_bmp_off_height       = 22U, /**< int32 declared pixel height.   */
  k_bmp_off_planes       = 26U, /**< uint16 plane count.            */
  k_bmp_off_bpp          = 28U, /**< uint16 bits per pixel.         */
  k_bmp_u16_bytes        = 2U,  /**< Little-endian uint16 width.    */
  k_bmp_u32_bytes        = 4U,  /**< Little-endian uint32 width.    */
  k_bmp_bits_per_byte    = 8U,  /**< Bits represented by one byte.  */
} test_bmp_field_t;

/** @brief 2x2 24-bit BMP: top row red/green, bottom row blue/white. */
static const uint8_t s_bmp[] = {
  0x42, 0x4D, 0x46, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x36, 0x00, 0x00, 0x00,
  0x28, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x00,
  0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x13, 0x0B, 0x00, 0x00,
  0x13, 0x0B, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x00,
  0x00, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0x00, 0x00,
};

/** @brief 8x8 VP8L fixture with deterministic RGB ramps. */
static const uint8_t s_webp[] = {
  0x52, 0x49, 0x46, 0x46, 0x2C, 0x00, 0x00, 0x00, 0x57, 0x45, 0x42, 0x50, 0x56,
  0x50, 0x38, 0x4C, 0x1F, 0x00, 0x00, 0x00, 0x2F, 0x07, 0xC0, 0x01, 0x00, 0xCD,
  0x65, 0x44, 0xFF, 0x63, 0x17, 0x85, 0x28, 0x78, 0xFF, 0x03, 0x42, 0x02, 0xC2,
  0x14, 0xFF, 0x77, 0x6A, 0x0E, 0x0C, 0x48, 0xC4, 0x04, 0x80, 0xAD, 0x0D, 0x00,
};

/** @brief RIFF/WEBP signature with no image chunk behind it. */
static const uint8_t s_webp_header_only[] =
  {0x52, 0x49, 0x46, 0x46, 0x04, 0x00, 0x00, 0x00, 0x57, 0x45, 0x42, 0x50};

alignas(16) static uint8_t s_stb_scratch[k_test_codec_arena_bytes];
alignas(16) static uint8_t s_webp_scratch[k_test_codec_arena_bytes];
static uint8_t s_rgba[k_test_rgba_bytes];
static uint8_t s_gray[k_test_gray_bytes];
static uint8_t s_encoded[k_test_encoded_bytes];
static uint8_t s_geometry_bmp[k_test_bmp_header_bytes];

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
RA8_INTERNAL static ra8_rabook_raster_workspace_t internal_fresh_workspace(ra8_img_arena_t*  stb,
                                                                           ra8_webp_arena_t* webp)
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
 * @brief Serialize one little-endian header field.
 * @details Writes each byte explicitly so the generated fixture is identical
 *          on either host byte order.
 * @param[out] out Destination spanning @p bytes writable bytes.
 * @param[in] value Field value to serialize.
 * @param[in] bytes Field width in bytes.
 * @pre @p out is non-NULL and spans @p bytes writable bytes.
 * @pre @p bytes is a declared little-endian field width.
 * @post The destination holds the little-endian encoding of @p value.
 * @post No byte outside the field is modified.
 * @note Thread-safe for distinct destinations.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_put_le(uint8_t* out, uint32_t value, uint8_t bytes)
{
  for (uint8_t i = 0U; i < bytes; ++i) {
    out[i] = (uint8_t)(value >> ((uint32_t)i * (uint32_t)k_bmp_bits_per_byte));
  }
}

/**
 * @brief Build a header-only 24-bit BMP declaring the requested geometry.
 * @details A memory-backed decode reads absent pixel bytes as zeros, so the
 *          fixed 54-byte header alone decodes as a complete image of the
 *          declared size. The callers use degenerate edges, so no row of
 *          pixel data is ever consumed.
 * @param[in] width Declared pixel width.
 * @param[in] height Declared pixel height.
 * @pre The static BMP fixture is exclusively owned by the caller.
 * @pre At least one declared edge is zero, so no pixel row is read.
 * @post The fixture is a complete uncompressed 24-bit BMP header.
 * @post The declared first-pixel offset equals the header length.
 * @note Not thread-safe because the fixture buffer is a shared static.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_build_bmp_header(uint32_t width, uint32_t height)
{
  (void)memset(s_geometry_bmp, 0, sizeof s_geometry_bmp);
  s_geometry_bmp[k_bmp_off_magic_first]  = (uint8_t)'B';
  s_geometry_bmp[k_bmp_off_magic_second] = (uint8_t)'M';
  internal_put_le(&s_geometry_bmp[k_bmp_off_pixel_data],
                  (uint32_t)k_test_bmp_header_bytes,
                  (uint8_t)k_bmp_u32_bytes);
  internal_put_le(&s_geometry_bmp[k_bmp_off_info_size],
                  (uint32_t)k_test_bmp_info_bytes,
                  (uint8_t)k_bmp_u32_bytes);
  internal_put_le(&s_geometry_bmp[k_bmp_off_width], width, (uint8_t)k_bmp_u32_bytes);
  internal_put_le(&s_geometry_bmp[k_bmp_off_height], height, (uint8_t)k_bmp_u32_bytes);
  internal_put_le(&s_geometry_bmp[k_bmp_off_planes],
                  (uint32_t)k_test_bmp_planes,
                  (uint8_t)k_bmp_u16_bytes);
  internal_put_le(&s_geometry_bmp[k_bmp_off_bpp],
                  (uint32_t)k_test_bmp_bpp_rgb24,
                  (uint8_t)k_bmp_u16_bytes);
}

/**
 * @brief Decode one declared geometry and require the invalid-size rejection.
 * @details Runs the complete public entry so the decoder allocates, validates
 *          the decoded dimensions, and releases its arena on the reject path.
 * @param[in] width Declared pixel width.
 * @param[in] height Declared pixel height.
 * @pre The static fixture storage is exclusively owned by the caller.
 * @pre The oversized arena covers this geometry's peak decode footprint.
 * @post The conversion returns the invalid-size status.
 * @post The image arena is fully drained and no encoded byte is claimed.
 * @note Test-only orchestration helper; not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_expect_geometry_rejected(uint32_t width, uint32_t height)
{
  ra8_img_arena_t               stb  = {};
  ra8_webp_arena_t              webp = {};
  ra8_rabook_raster_workspace_t ws   = internal_fresh_workspace(&stb, &webp);
  ra8_rabook_raster_result_t    got  = {.encoded_size = UINT32_MAX};
  internal_build_bmp_header(width, height);
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_rabook_raster_encode(s_geometry_bmp,
                                          sizeof s_geometry_bmp,
                                          0U,
                                          (uint8_t)k_ra8_book_pixfmt_gray4,
                                          &ws,
                                          s_encoded,
                                          sizeof s_encoded,
                                          &got));
  TEST_ASSERT_EQ(k_test_no_bytes, got.encoded_size);
  TEST_ASSERT_EQ(k_test_arena_drained, stb.live);
  TEST_ASSERT_EQ(k_test_arena_drained, stb.offset);
}

/** @brief Exercise required pointers, empty input, format, and decoder validation. @details Implements the expect raster validation errors fixture operation used only by this focused test executable. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL
static void internal_expect_raster_validation_errors(void)
{
  ra8_img_arena_t               stb   = {};
  ra8_webp_arena_t              webp  = {};
  ra8_rabook_raster_workspace_t ws    = internal_fresh_workspace(&stb, &webp);
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
}

/** @brief Exercise encoded-output and decoded-WebP workspace capacity guards. @details Implements the expect raster capacity errors fixture operation used only by this focused test executable. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL
static void internal_expect_raster_capacity_errors(void)
{
  ra8_img_arena_t               stb  = {};
  ra8_webp_arena_t              webp = {};
  ra8_rabook_raster_workspace_t ws   = internal_fresh_workspace(&stb, &webp);
  ra8_rabook_raster_result_t    got  = {.encoded_size = UINT32_MAX};
  TEST_ASSERT_EQ(k_ra8_err_no_mem,
                 ra8_rabook_raster_encode(s_bmp,
                                          sizeof s_bmp,
                                          0U,
                                          (uint8_t)k_ra8_book_pixfmt_gray4,
                                          &ws,
                                          s_encoded,
                                          1U,
                                          &got));
  TEST_ASSERT_EQ(0U, got.encoded_size);

  ws.rgba_cap = 1U;
  TEST_ASSERT_EQ(k_ra8_err_no_mem,
                 ra8_rabook_raster_encode(s_webp,
                                          sizeof s_webp,
                                          0U,
                                          (uint8_t)k_ra8_book_pixfmt_gray8,
                                          &ws,
                                          s_encoded,
                                          sizeof s_encoded,
                                          &got));
}

/**
 * @test internal_test_bmp_gray4_and_clamp
 * @brief BMP pixels normalize to stb-compatible gray4 and clamp to gray8.
 * @details Checks the exact luminance/nibble bytes at native geometry, then a
 *          one-pixel gray8 clamp and complete stb arena drain.
 * @pre Shared fixture buffers are not in use by another test.
 * @pre The embedded BMP fixture remains byte-identical to its declaration.
 * @post Both conversions succeed and the image arena is empty.
 * @post The exact gray4 and clamped gray8 bytes match their goldens.
 * @note Not thread-safe because the fixture buffers are shared statics.
 * @since 0.1.0
 * @par MC/DC:
 * WebP signature decision `(size >= 12) && RIFF && WEBP` receives the BMP
 * vector `(T,F,-)->F`. Companion tests supply valid WebP `(T,T,T)->T` and a
 * three-byte malformed input `(F,-,-)->F`; no `(T,T,F)` vector exists here, so
 * independence of the form-tag comparison is not claimed. Scale decision
 * `(source_width == out_width) && (source_height == out_height)` sees
 * `(T,T)->T` without a clamp and `(F,-)->F` for the square 2-to-1 clamp; both
 * dimensions change, so neither equality has a complete independence pair.
 */
RA8_INTERNAL static void internal_test_bmp_gray4_and_clamp(void)
{
  TEST_BEGIN("rabook raster: BMP gray4 + clamp");
  ra8_img_arena_t               stb  = {};
  ra8_webp_arena_t              webp = {};
  ra8_rabook_raster_workspace_t ws   = internal_fresh_workspace(&stb, &webp);
  ra8_rabook_raster_result_t    got  = {};

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_rabook_raster_encode(s_bmp,
                                          sizeof s_bmp,
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
                 ra8_rabook_raster_encode(s_bmp,
                                          sizeof s_bmp,
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
 * @test internal_test_webp_gray8_and_arena_drain
 * @brief WebP normalization uses deterministic luminance and drains its arena.
 * @details Verifies geometry, byte count, the first two expected luminance
 *          samples, and the libwebp arena's live/offset postconditions.
 * @pre Shared fixture buffers are not in use by another test.
 * @pre The embedded WebP fixture remains byte-identical to its declaration.
 * @post The lossless fixture has a complete gray8 frame and drained arena.
 * @post Geometry and the first two luminance bytes match their goldens.
 * @note Not thread-safe because the fixture buffers are shared statics.
 * @since 0.1.0
 * @par MC/DC:
 * The WebP signature decision takes `(size adequate=T, RIFF=T, WEBP=T)->T`.
 * Decode-storage OR `(webp arena null) || (RGBA null) || (frame > capacity)`
 * takes its valid `(F,F,F)->F` control; the short-RGBA vector in the validation
 * test supplies `(F,F,T)->T`, independently varying the capacity condition.
 * Neither pointer condition is varied. With `max_edge == 0`, output dimensions
 * follow the single no-clamp branch and no scale-equality AND is evaluated.
 */
RA8_INTERNAL static void internal_test_webp_gray8_and_arena_drain(void)
{
  TEST_BEGIN("rabook raster: WebP gray8 + arena drain");
  ra8_img_arena_t               stb  = {};
  ra8_webp_arena_t              webp = {};
  ra8_rabook_raster_workspace_t ws   = internal_fresh_workspace(&stb, &webp);
  ra8_rabook_raster_result_t    got  = {};

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_rabook_raster_encode(s_webp,
                                          sizeof s_webp,
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
 * @test internal_test_validation_and_capacity
 * @brief Invalid parameters, malformed data, and undersized storage fail closed.
 * @details Drives null, empty, invalid-format, malformed-codec, short encoded
 *          output, and short WebP RGBA storage independently.
 * @pre Shared fixture buffers are not in use by another test.
 * @pre The success fixtures are valid controls for the capacity vectors.
 * @post Every error is explicit and the result byte count remains zero.
 * @post No undersized buffer is treated as a successful conversion.
 * @note Not thread-safe because the fixture buffers are shared statics.
 * @since 0.1.0
 * @par MC/DC:
 * Required-pointer OR receives source-null `(T,-,-,-)->T` and valid
 * `(F,F,F,F)->F` controls; the other three pointer conditions are not isolated.
 * Pixel-format decision `(format != gray4) && (format != gray8)` has the full
 * N+1 set: gray4 `(F,-)->F`, gray8 `(T,F)->F`, and invalid `(T,T)->T`.
 * WebP storage OR `(arena null) || (RGBA null) || (frame > capacity)` gets
 * `(F,F,T)->T` from the one-byte RGBA buffer and `(F,F,F)->F` from the WebP
 * success test, isolating capacity but not either pointer.
 * The three-byte malformed input gives signature `(F,-,-)->F`; BMP and WebP
 * companion vectors are documented in their tests, without claiming a missing
 * `(T,T,F)` form-tag vector. The undersized encoded buffer flips a separate
 * single encoder-capacity condition.
 */
RA8_INTERNAL static void internal_test_validation_and_capacity(void)
{
  TEST_BEGIN("rabook raster: validation + capacity");
  internal_expect_raster_validation_errors();
  internal_expect_raster_capacity_errors();
  TEST_END("rabook raster: validation + capacity");
}

/**
 * @test internal_test_output_pointer_guards
 * @brief Each remaining required output pointer is rejected on its own.
 * @details The companion validation test covers the source pointer; this one
 *          removes the workspace, the encoded destination, and the result
 *          record in turn while every other argument stays valid.
 * @pre Shared fixture buffers are not in use by another test.
 * @pre The BMP fixture and workspace controls remain valid.
 * @post Every vector returns the null-pointer status.
 * @post No codec arena is bound and no encoded byte is produced.
 * @note These are independent single-condition guards, one per `if`.
 * @since 0.1.0
 * @par MC/DC:
 * The required-pointer chain is four separate single-condition decisions.
 * Each vector here drives exactly one of them true with all earlier ones
 * false; the source-null vector in the validation test drives the first, and
 * every success test supplies the all-false control.
 */
RA8_INTERNAL static void internal_test_output_pointer_guards(void)
{
  TEST_BEGIN("rabook raster: output pointer guards");
  ra8_img_arena_t               stb  = {};
  ra8_webp_arena_t              webp = {};
  ra8_rabook_raster_workspace_t ws   = internal_fresh_workspace(&stb, &webp);
  ra8_rabook_raster_result_t    got  = {.encoded_size = UINT32_MAX};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_rabook_raster_encode(s_bmp,
                                          sizeof s_bmp,
                                          0U,
                                          (uint8_t)k_ra8_book_pixfmt_gray4,
                                          nullptr,
                                          s_encoded,
                                          sizeof s_encoded,
                                          &got));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_rabook_raster_encode(s_bmp,
                                          sizeof s_bmp,
                                          0U,
                                          (uint8_t)k_ra8_book_pixfmt_gray4,
                                          &ws,
                                          nullptr,
                                          sizeof s_encoded,
                                          &got));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_rabook_raster_encode(s_bmp,
                                          sizeof s_bmp,
                                          0U,
                                          (uint8_t)k_ra8_book_pixfmt_gray4,
                                          &ws,
                                          s_encoded,
                                          sizeof s_encoded,
                                          nullptr));
  TEST_ASSERT_EQ(UINT32_MAX, got.encoded_size);
  TEST_ASSERT_EQ(k_test_arena_drained, stb.live);
  TEST_END("rabook raster: output pointer guards");
}

/**
 * @test internal_test_downscale_workspace_faults
 * @brief A missing or undersized gray scratch stops a required downscale.
 * @details Both vectors clamp the longer edge so the decoded geometry really
 *          does have to change, which is the only condition under which the
 *          caller's downscale buffer is consulted at all.
 * @pre Shared fixture buffers are not in use by another test.
 * @pre The BMP fixture decodes successfully before the scale step runs.
 * @post Both vectors return the no-memory status and claim no encoded bytes.
 * @post The stb arena is drained even though the failure follows the decode.
 * @note The unclamped success path proves the same buffers are otherwise fine.
 * @since 0.1.0
 * @par MC/DC:
 * Scratch decision `(gray == NULL) || (pixels > gray_cap)` receives
 * `(T,-)->T` from the absent buffer and `(F,T)->T` from the zero capacity,
 * with `(F,F)->F` supplied by the clamped success vector in the BMP test:
 * the complete N+1 independence set for both conditions.
 */
RA8_INTERNAL static void internal_test_downscale_workspace_faults(void)
{
  TEST_BEGIN("rabook raster: downscale workspace faults");
  ra8_img_arena_t               stb  = {};
  ra8_webp_arena_t              webp = {};
  ra8_rabook_raster_workspace_t ws   = internal_fresh_workspace(&stb, &webp);
  ra8_rabook_raster_result_t    got  = {.encoded_size = UINT32_MAX};
  ws.gray                            = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_no_mem,
                 ra8_rabook_raster_encode(s_bmp,
                                          sizeof s_bmp,
                                          (uint16_t)k_test_clamp_to_one,
                                          (uint8_t)k_ra8_book_pixfmt_gray4,
                                          &ws,
                                          s_encoded,
                                          sizeof s_encoded,
                                          &got));
  TEST_ASSERT_EQ(k_test_no_bytes, got.encoded_size);
  TEST_ASSERT_EQ(k_test_arena_drained, stb.live);

  ws          = internal_fresh_workspace(&stb, &webp);
  ws.gray_cap = 0U;
  TEST_ASSERT_EQ(k_ra8_err_no_mem,
                 ra8_rabook_raster_encode(s_bmp,
                                          sizeof s_bmp,
                                          (uint16_t)k_test_clamp_to_one,
                                          (uint8_t)k_ra8_book_pixfmt_gray4,
                                          &ws,
                                          s_encoded,
                                          sizeof s_encoded,
                                          &got));
  TEST_ASSERT_EQ(k_test_no_bytes, got.encoded_size);
  TEST_ASSERT_EQ(k_test_arena_drained, stb.live);
  TEST_END("rabook raster: downscale workspace faults");
}

/**
 * @test internal_test_encoded_capacity_saturates
 * @brief A host capacity beyond the uint32 encoder bound saturates, not wraps.
 * @details The caller declares one byte more than the encoder's uint32
 *          parameter can express; a truncating cast would present zero
 *          capacity and reject a payload that plainly fits.
 * @pre Shared fixture buffers are not in use by another test.
 * @pre The host `size_t` is wider than the encoder's uint32 capacity field.
 * @post The conversion succeeds and reports the exact gray4 byte count.
 * @post Only the reported prefix of the encoded destination is written.
 * @note The declared capacity is a bound, not a promise of writable bytes.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_encoded_capacity_saturates(void)
{
  TEST_BEGIN("rabook raster: encoded capacity saturates");
  ra8_img_arena_t               stb      = {};
  ra8_webp_arena_t              webp     = {};
  ra8_rabook_raster_workspace_t ws       = internal_fresh_workspace(&stb, &webp);
  ra8_rabook_raster_result_t    got      = {};
  const size_t                  huge_cap = (size_t)UINT32_MAX + (size_t)k_test_cap_overshoot;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_rabook_raster_encode(s_bmp,
                                          sizeof s_bmp,
                                          0U,
                                          (uint8_t)k_ra8_book_pixfmt_gray4,
                                          &ws,
                                          s_encoded,
                                          huge_cap,
                                          &got));
  TEST_ASSERT_EQ(k_test_bmp_dim, got.width);
  TEST_ASSERT_EQ(k_test_bmp_dim, got.height);
  TEST_ASSERT_EQ(k_test_gray4_pair_bytes, got.encoded_size);
  TEST_ASSERT_EQ(k_test_arena_drained, stb.live);
  TEST_END("rabook raster: encoded capacity saturates");
}

/**
 * @test internal_test_stb_entry_guards
 * @brief An unrepresentable source length and a missing arena fail closed.
 * @details The stb entry point takes an `int` length and draws every byte of
 *          scratch from the caller arena, so both are checked before the
 *          decoder is handed anything.
 * @pre Shared fixture buffers are not in use by another test.
 * @pre The BMP fixture spans the bytes the WebP signature probe inspects.
 * @post The oversized length returns invalid-size and the absent arena no-memory.
 * @post Neither vector binds an arena or produces an encoded byte.
 * @note The oversized length is a declared bound, never a readable extent.
 * @since 0.1.0
 * @par MC/DC:
 * Both are single-condition guards: the oversized length flips
 * `source_size > INT_MAX` and the absent descriptor flips
 * `stb_arena == NULL`, with the BMP success test supplying both false arms.
 */
RA8_INTERNAL static void internal_test_stb_entry_guards(void)
{
  TEST_BEGIN("rabook raster: stb entry guards");
  ra8_img_arena_t               stb       = {};
  ra8_webp_arena_t              webp      = {};
  ra8_rabook_raster_workspace_t ws        = internal_fresh_workspace(&stb, &webp);
  ra8_rabook_raster_result_t    got       = {.encoded_size = UINT32_MAX};
  const size_t                  oversized = (size_t)INT_MAX + (size_t)k_test_cap_overshoot;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_rabook_raster_encode(s_bmp,
                                          oversized,
                                          0U,
                                          (uint8_t)k_ra8_book_pixfmt_gray4,
                                          &ws,
                                          s_encoded,
                                          sizeof s_encoded,
                                          &got));
  TEST_ASSERT_EQ(k_test_no_bytes, got.encoded_size);

  ws           = internal_fresh_workspace(&stb, &webp);
  ws.stb_arena = nullptr;
  got          = (ra8_rabook_raster_result_t){.encoded_size = UINT32_MAX};
  TEST_ASSERT_EQ(k_ra8_err_no_mem,
                 ra8_rabook_raster_encode(s_bmp,
                                          sizeof s_bmp,
                                          0U,
                                          (uint8_t)k_ra8_book_pixfmt_gray4,
                                          &ws,
                                          s_encoded,
                                          sizeof s_encoded,
                                          &got));
  TEST_ASSERT_EQ(k_test_no_bytes, got.encoded_size);
  TEST_ASSERT_EQ(k_test_arena_drained, stb.live);
  TEST_END("rabook raster: stb entry guards");
}

/**
 * @test internal_test_decoded_geometry_rejected
 * @brief A decoded image with a degenerate edge is refused, not normalized.
 * @details Two header-only BMP fixtures decode successfully and then fail the
 *          dimension contract: one declares no columns and one declares no
 *          rows. Both produce a live zero-byte decode, which is precisely the
 *          case a contract that only checked the decoder's own status would
 *          pass through to the scaler.
 * @pre Shared fixture buffers are not in use by another test.
 * @pre The decoder accepts a zero edge and reports it back unchanged.
 * @post Both vectors return invalid-size and report no encoded bytes.
 * @post The image arena is drained on every rejection path.
 * @note A rejected decode must free its arena block before unbinding.
 * @since 0.1.0
 * @par MC/DC:
 * The dimension contract is a chain of single-condition guards feeding one
 * validity flag. The zero-width vector flips `width > 0` false and the
 * zero-height vector flips `height <= 0` true, while the BMP success test
 * supplies the all-false control that leaves the flag true. The two
 * `> UINT16_MAX` conditions have no independence vector through this entry
 * point: the vendored decoder caps every axis well below that bound and
 * rejects a larger declaration itself, so they are defensive only.
 */
RA8_INTERNAL static void internal_test_decoded_geometry_rejected(void)
{
  TEST_BEGIN("rabook raster: decoded geometry rejected");
  internal_expect_geometry_rejected(k_test_dim_empty, k_test_dim_single);
  internal_expect_geometry_rejected(k_test_bmp_dim, k_test_dim_empty);
  TEST_END("rabook raster: decoded geometry rejected");
}

/**
 * @test internal_test_webp_decode_faults
 * @brief WebP header, storage, and decode failures are reported distinctly.
 * @details A bare signature has no header to parse, the two storage vectors
 *          remove one required buffer each, and an empty scratch arena lets a
 *          well-formed bitstream fail inside the decoder itself.
 * @pre Shared fixture buffers are not in use by another test.
 * @pre The WebP fixture remains byte-identical to its declaration.
 * @post Header and decode failures report validation-failed.
 * @post Both missing-storage vectors report no-memory.
 * @note The decode vector proves the codec failure propagates unchanged.
 * @since 0.1.0
 * @par MC/DC:
 * Decode-storage OR `(webp arena null) || (RGBA null) || (frame > capacity)`
 * gains `(T,-,-)->T` and `(F,T,-)->T` here; the validation test supplies
 * `(F,F,T)->T` and the WebP success test `(F,F,F)->F`, which completes the
 * N+1 independence set for all three conditions. The header and decode
 * failures each flip a separate single-condition propagation guard.
 */
RA8_INTERNAL static void internal_test_webp_decode_faults(void)
{
  TEST_BEGIN("rabook raster: WebP decode faults");
  ra8_img_arena_t               stb  = {};
  ra8_webp_arena_t              webp = {};
  ra8_rabook_raster_workspace_t ws   = internal_fresh_workspace(&stb, &webp);
  ra8_rabook_raster_result_t    got  = {.encoded_size = UINT32_MAX};
  TEST_ASSERT_EQ(k_ra8_err_validation_failed,
                 ra8_rabook_raster_encode(s_webp_header_only,
                                          sizeof s_webp_header_only,
                                          0U,
                                          (uint8_t)k_ra8_book_pixfmt_gray8,
                                          &ws,
                                          s_encoded,
                                          sizeof s_encoded,
                                          &got));
  TEST_ASSERT_EQ(k_test_no_bytes, got.encoded_size);

  ws            = internal_fresh_workspace(&stb, &webp);
  ws.webp_arena = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_no_mem,
                 ra8_rabook_raster_encode(s_webp,
                                          sizeof s_webp,
                                          0U,
                                          (uint8_t)k_ra8_book_pixfmt_gray8,
                                          &ws,
                                          s_encoded,
                                          sizeof s_encoded,
                                          &got));

  ws      = internal_fresh_workspace(&stb, &webp);
  ws.rgba = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_no_mem,
                 ra8_rabook_raster_encode(s_webp,
                                          sizeof s_webp,
                                          0U,
                                          (uint8_t)k_ra8_book_pixfmt_gray8,
                                          &ws,
                                          s_encoded,
                                          sizeof s_encoded,
                                          &got));

  ws       = internal_fresh_workspace(&stb, &webp);
  webp.cap = 0U;
  TEST_ASSERT_EQ(k_ra8_err_validation_failed,
                 ra8_rabook_raster_encode(s_webp,
                                          sizeof s_webp,
                                          0U,
                                          (uint8_t)k_ra8_book_pixfmt_gray8,
                                          &ws,
                                          s_encoded,
                                          sizeof s_encoded,
                                          &got));
  TEST_ASSERT_EQ(k_test_no_bytes, got.encoded_size);
  TEST_END("rabook raster: WebP decode faults");
}

int main(void)
{
  internal_test_bmp_gray4_and_clamp();
  internal_test_webp_gray8_and_arena_drain();
  internal_test_validation_and_capacity();
  internal_test_output_pointer_guards();
  internal_test_downscale_workspace_faults();
  internal_test_encoded_capacity_saturates();
  internal_test_stb_entry_guards();
  internal_test_decoded_geometry_rejected();
  internal_test_webp_decode_faults();
  return 0;
}
