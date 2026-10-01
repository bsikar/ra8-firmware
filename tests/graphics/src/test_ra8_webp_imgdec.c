/**
 * @file test_ra8_webp_imgdec.c
 * @brief Host unit tests for the WebP `ra8_imgdec` backend binder (RA8FW-308).
 *
 * @details
 * Exercises `ra8_webp_imgdec_bind()` (`apps/shared_libs/webp`) through the
 * public fabric entry points only -- `ra8_imgdec_get_caps()`,
 * `ra8_imgdec_supports()`, `ra8_imgdec_probe()` and `ra8_imgdec_decode()` --
 * so the binder is tested the way a consumer reaches it rather than by
 * calling its vtable hooks directly.
 *
 * The fixtures are the committed files under tests/fixtures/webp/, embedded
 * inline exactly as test_ra8_webp.c embeds them, so the test needs no runtime
 * file I/O. See tests/fixtures/webp/README.md for their provenance.
 *
 *
 * [Ring 4 / WebP]
 * {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_imgdec.h"
#include "ra8_webp.h"
#include "ra8_webp_arena.h"
#include "ra8_webp_imgdec.h"
#include "unity_minimal.h"

/**
 * @enum t_webpdec_t
 * @brief Arena sizing and fixture geometry (no magic numbers).
 */
typedef enum : uint32_t {
  k_t_scratch_log2 = 20U,                      /**< Decode arena size, 1 MiB.     */
  k_t_dim          = 8U,                       /**< Fixture width and height, px. */
  k_t_bpp          = 4U,                       /**< RGBA8888 bytes per pixel.     */
  k_t_stride       = k_t_dim * k_t_bpp,        /**< Tight row stride, bytes.      */
  k_t_fb           = k_t_dim * k_t_stride,     /**< Tight 8x8 RGBA surface.       */
  k_t_pad          = 16U,                      /**< Extra bytes padding a row.    */
  k_t_pad_stride   = k_t_stride + k_t_pad,     /**< A padded destination stride.  */
  k_t_pad_fb       = k_t_dim * k_t_pad_stride, /**< Surface at that stride.       */
  k_t_wide_dim     = 8200U,                    /**< The oversize fixture width.   */
  k_t_opaque       = 255U,                     /**< Alpha of the golden pattern.  */
} t_webpdec_t;

/* ------------------------------------------------------------------------- */
/* Committed fixtures under tests/fixtures/webp/, embedded inline. */
/* ------------------------------------------------------------------------- */

/** 8x8 VP8L (lossless, -exact) -- golden pixel round-trip. */
static const uint8_t s_webp_lossless[] = {
  0x52, 0x49, 0x46, 0x46, 0x2C, 0x00, 0x00, 0x00, 0x57, 0x45, 0x42, 0x50, 0x56,
  0x50, 0x38, 0x4C, 0x1F, 0x00, 0x00, 0x00, 0x2F, 0x07, 0xC0, 0x01, 0x00, 0xCD,
  0x65, 0x44, 0xFF, 0x63, 0x17, 0x85, 0x28, 0x78, 0xFF, 0x03, 0x42, 0x02, 0xC2,
  0x14, 0xFF, 0x77, 0x6A, 0x0E, 0x0C, 0x48, 0xC4, 0x04, 0x80, 0xAD, 0x0D, 0x00,
};

/** 8x8 VP8 (lossy) -- proves the lossy decode path runs through the seam. */
static const uint8_t s_webp_lossy[] = {
  0x52, 0x49, 0x46, 0x46, 0x4C, 0x00, 0x00, 0x00, 0x57, 0x45, 0x42, 0x50, 0x56, 0x50,
  0x38, 0x20, 0x40, 0x00, 0x00, 0x00, 0xD0, 0x01, 0x00, 0x9D, 0x01, 0x2A, 0x08, 0x00,
  0x08, 0x00, 0x01, 0x40, 0x26, 0x25, 0xA8, 0x02, 0x74, 0x01, 0x0F, 0x0C, 0x06, 0xC5,
  0xE0, 0x00, 0xFE, 0xFC, 0xD2, 0xFE, 0x92, 0x7B, 0xB2, 0xF7, 0x72, 0xC7, 0x79, 0xA0,
  0xF3, 0x66, 0x26, 0x95, 0xF3, 0x4F, 0xFF, 0xF5, 0x1F, 0xCD, 0x05, 0xC9, 0x25, 0xFE,
  0xFF, 0x9F, 0xB4, 0x46, 0x43, 0xFF, 0x80, 0xFF, 0xA2, 0x97, 0x0C, 0x00, 0x00, 0x00,
};

/** 8200x2 VP8L (solid) -- width exceeds the per-axis dimension cap. */
static const uint8_t s_webp_wide[] = {
  0x52, 0x49, 0x46, 0x46, 0x18, 0x00, 0x00, 0x00, 0x57, 0x45, 0x42, 0x50, 0x56, 0x50, 0x38, 0x4C,
  0x0C, 0x00, 0x00, 0x00, 0x2F, 0x07, 0x60, 0x00, 0x00, 0x28, 0x45, 0x15, 0xEA, 0xD1, 0xFF, 0x00,
};

/** An 8x8 PNG signature + IHDR: a container this backend must refuse. */
static const uint8_t s_png_head[] = {
  0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52,
  0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x08, 0x08, 0x06, 0x00, 0x00, 0x00, 0x00,
};

/** 1 MiB scratch backing store shared by the decode tests. */
alignas(16) static uint8_t s_scratch[1U << k_t_scratch_log2];

/** Destination surface, sized for the widest stride any case here asks for. */
static uint8_t s_dst[k_t_pad_fb];

/**
 * @brief Construct a fresh full-size arena over @ref s_scratch.
 * @return Reset arena descriptor for one decode operation.
 * @retval ra8_webp_arena_t Descriptor with zero offset and live count.
 * @pre The caller exclusively owns @ref s_scratch for the decode duration.
 * @post The returned descriptor covers the complete scratch buffer.
 * @note Not thread-safe: the scratch buffer is shared module state.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_webp_arena_t internal_fresh_arena(void)
{
  return (ra8_webp_arena_t){.base = s_scratch, .cap = sizeof s_scratch, .offset = 0U, .live = 0U};
}

/**
 * @brief Build a decode request over a fixture, declaring WebP explicitly.
 * @param[in] bytes  Fixture bytes.
 * @param[in] count  Fixture length.
 * @param[in] stride Destination stride, or 0 for tightly packed.
 * @param[in] cap    Writable bytes at the destination.
 * @return ra8_imgdec_req_t The assembled request.
 * @retval ra8_imgdec_req_t Always populated; no failure path.
 * @pre @p bytes spans @p count readable bytes.
 * @post The request names RGBA8888 and the shared destination surface.
 * @note Not thread-safe: the destination is shared module state.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_imgdec_req_t
internal_req(const uint8_t* bytes, uint32_t count, uint32_t stride, uint32_t cap)
{
  return (ra8_imgdec_req_t){
    .bytes      = bytes,
    .byte_count = count,
    .arena      = nullptr,
    .dst        = s_dst,
    .dst_bytes  = cap,
    .dst_stride = stride,
    .format     = k_ra8_imgdec_format_webp,
    .want       = k_ra8_imgdec_pixel_rgba8888,
  };
}

/* ------------------------------------------------------------------------- */
/* Binding and capability reporting. */
/* ------------------------------------------------------------------------- */

/**
 * @test internal_test_bind_guards
 * @brief A null handle or a null scratch is refused, and the handle is left
 *        unbound rather than half-written.
 * @return None.
 * @pre None.
 * @post No handle is left partially bound.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_bind_guards(void)
{
  TEST_BEGIN("bind refuses a null handle and a null scratch");

  ra8_webp_arena_t arena = internal_fresh_arena();
  ra8_imgdec_t     dec   = {};

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_webp_imgdec_bind(nullptr, &arena));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_webp_imgdec_bind(&dec, nullptr));
  TEST_ASSERT(dec.iface == nullptr);
  TEST_ASSERT(dec.ctx == nullptr);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_webp_imgdec_bind(&dec, &arena));
  TEST_ASSERT(dec.iface != nullptr);
  TEST_ASSERT(dec.ctx == (void*)&arena);

  TEST_END("bind refuses a null handle and a null scratch");
}

/**
 * @test internal_test_caps_matrix
 * @brief The advertised matrix is WebP into RGBA8888 alone, with no request
 *        arena and the facade's own dimension cap.
 * @return None.
 * @pre None.
 * @post No state is mutated.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_caps_matrix(void)
{
  TEST_BEGIN("the advertised matrix is WebP into RGBA8888 alone");

  ra8_webp_arena_t  arena = internal_fresh_arena();
  ra8_imgdec_t      dec   = {};
  ra8_imgdec_caps_t caps  = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_webp_imgdec_bind(&dec, &arena));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_get_caps(&dec, &caps));

  TEST_ASSERT_EQ((uint32_t)k_ra8_imgdec_format_webp, caps.formats);
  TEST_ASSERT_EQ((uint32_t)k_ra8_imgdec_pixel_rgba8888, caps.pixels);
  TEST_ASSERT_EQ(0U, caps.scratch_bytes);
  TEST_ASSERT_EQ(0U, caps.scratch_align);
  TEST_ASSERT_EQ((uint32_t)k_ra8_webp_max_dim, caps.dim_max);
  TEST_ASSERT(!(caps.streams));

  TEST_END("the advertised matrix is WebP into RGBA8888 alone");
}

/**
 * @test internal_test_supports_pairs
 * @brief The seam answers yes only to WebP into RGBA8888, and no to the
 *        formats the other backends own.
 * @return None.
 * @pre None.
 * @post No state is mutated.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_supports_pairs(void)
{
  TEST_BEGIN("the seam answers only the pair this backend opens");

  ra8_webp_arena_t arena = internal_fresh_arena();
  ra8_imgdec_t     dec   = {};
  bool             ok    = false;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_webp_imgdec_bind(&dec, &arena));

  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_imgdec_supports(&dec, k_ra8_imgdec_format_webp, k_ra8_imgdec_pixel_rgba8888, &ok));
  TEST_ASSERT(ok);
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_imgdec_supports(&dec, k_ra8_imgdec_format_webp, k_ra8_imgdec_pixel_rgb888, &ok));
  TEST_ASSERT(!(ok));
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_imgdec_supports(&dec, k_ra8_imgdec_format_jpeg, k_ra8_imgdec_pixel_rgba8888, &ok));
  TEST_ASSERT(!(ok));
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_imgdec_supports(&dec, k_ra8_imgdec_format_gif, k_ra8_imgdec_pixel_rgba8888, &ok));
  TEST_ASSERT(!(ok));

  TEST_END("the seam answers only the pair this backend opens");
}

/* ------------------------------------------------------------------------- */
/* Decoding through the fabric. */
/* ------------------------------------------------------------------------- */

/**
 * @test internal_test_decode_lossless_golden
 * @brief A lossless 8x8 WebP decodes bit-exact through the seam to the same
 *        pattern the direct facade test asserts.
 * @return None.
 * @pre The fixture bytes remain valid for the call.
 * @post The result record describes the decoded surface exactly.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_decode_lossless_golden(void)
{
  TEST_BEGIN("a lossless WebP decodes bit-exact through the seam");

  ra8_webp_arena_t arena = internal_fresh_arena();
  ra8_imgdec_t     dec   = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_webp_imgdec_bind(&dec, &arena));

  const ra8_imgdec_req_t req =
    internal_req(s_webp_lossless, (uint32_t)sizeof s_webp_lossless, 0U, (uint32_t)k_t_fb);
  ra8_imgdec_image_t out = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_decode(&dec, &req, &out));

  TEST_ASSERT_EQ((uint32_t)k_t_dim, out.width_px);
  TEST_ASSERT_EQ((uint32_t)k_t_dim, out.height_px);
  TEST_ASSERT_EQ((uint32_t)k_t_stride, out.stride);
  TEST_ASSERT_EQ((uint32_t)k_t_fb, out.used_bytes);
  TEST_ASSERT_EQ(k_ra8_imgdec_format_webp, out.format);
  TEST_ASSERT_EQ(k_ra8_imgdec_pixel_rgba8888, out.pixel);

  for (uint32_t y = 0U; y < (uint32_t)k_t_dim; ++y) {
    for (uint32_t x = 0U; x < (uint32_t)k_t_dim; ++x) {
      const uint8_t* const px = &s_dst[(y * (uint32_t)k_t_stride) + (x * (uint32_t)k_t_bpp)];
      TEST_ASSERT_EQ((uint8_t)((x * 32U) & 255U), px[0]);
      TEST_ASSERT_EQ((uint8_t)((y * 32U) & 255U), px[1]);
      TEST_ASSERT_EQ((uint8_t)(((x + y) * 16U) & 255U), px[2]);
      TEST_ASSERT_EQ((uint8_t)k_t_opaque, px[3]);
    }
  }
  /* The facade drains its arena before returning, on every path. */
  TEST_ASSERT_EQ(0U, arena.live);
  TEST_ASSERT_EQ(0U, (uint32_t)arena.offset);

  TEST_END("a lossless WebP decodes bit-exact through the seam");
}

/**
 * @test internal_test_decode_lossy_runs
 * @brief The lossy (VP8) bitstream reaches the same seam and reports the same
 *        geometry, so the backend is not lossless-only.
 * @return None.
 * @pre The fixture bytes remain valid for the call.
 * @post The arena is drained.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_decode_lossy_runs(void)
{
  TEST_BEGIN("the lossy bitstream decodes through the same seam");

  ra8_webp_arena_t arena = internal_fresh_arena();
  ra8_imgdec_t     dec   = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_webp_imgdec_bind(&dec, &arena));

  const ra8_imgdec_req_t req =
    internal_req(s_webp_lossy, (uint32_t)sizeof s_webp_lossy, 0U, (uint32_t)k_t_fb);
  ra8_imgdec_image_t out = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_decode(&dec, &req, &out));
  TEST_ASSERT_EQ((uint32_t)k_t_dim, out.width_px);
  TEST_ASSERT_EQ((uint32_t)k_t_dim, out.height_px);
  /* A plain lossy VP8 stream carries no alpha chunk, so the flag is false
   * even though the destination layout has an alpha byte. */
  TEST_ASSERT(!(out.had_alpha));
  TEST_ASSERT_EQ(0U, arena.live);

  TEST_END("the lossy bitstream decodes through the same seam");
}

/**
 * @test internal_test_decode_padded_stride
 * @brief A destination wider than the image is honoured: rows land at the
 *        requested stride and the reported span excludes the last row's pad.
 * @return None.
 * @pre The fixture bytes remain valid for the call.
 * @post Row starts hold the same pixels the packed decode produced.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_decode_padded_stride(void)
{
  TEST_BEGIN("a padded stride is honoured and its pad excluded");

  ra8_webp_arena_t arena = internal_fresh_arena();
  ra8_imgdec_t     dec   = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_webp_imgdec_bind(&dec, &arena));

  const ra8_imgdec_req_t req = internal_req(s_webp_lossless,
                                            (uint32_t)sizeof s_webp_lossless,
                                            (uint32_t)k_t_pad_stride,
                                            (uint32_t)k_t_pad_fb);
  ra8_imgdec_image_t     out = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_decode(&dec, &req, &out));

  TEST_ASSERT_EQ((uint32_t)k_t_pad_stride, out.stride);
  /* used_bytes stops at the last real pixel, not at the end of its padded row. */
  TEST_ASSERT_EQ(
    (((uint32_t)k_t_dim - 1U) * (uint32_t)k_t_pad_stride) + (uint32_t)k_t_stride, out.used_bytes);
  for (uint32_t y = 0U; y < (uint32_t)k_t_dim; ++y) {
    const uint8_t* const row = &s_dst[y * (uint32_t)k_t_pad_stride];
    TEST_ASSERT_EQ(0U, row[0]);
    TEST_ASSERT_EQ((uint8_t)((y * 32U) & 255U), row[1]);
  }

  TEST_END("a padded stride is honoured and its pad excluded");
}

/* ------------------------------------------------------------------------- */
/* Refusals. */
/* ------------------------------------------------------------------------- */

/**
 * @test internal_test_wrong_container_refused
 * @brief Bytes that are a PNG, declared as WebP, are refused as unsupported
 *        rather than handed to the facade.
 * @return None.
 * @pre The fixture bytes remain valid for the call.
 * @post No decode is attempted and the arena is untouched.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_wrong_container_refused(void)
{
  TEST_BEGIN("a PNG declared as WebP is refused");

  ra8_webp_arena_t arena = internal_fresh_arena();
  ra8_imgdec_t     dec   = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_webp_imgdec_bind(&dec, &arena));

  const ra8_imgdec_req_t req =
    internal_req(s_png_head, (uint32_t)sizeof s_png_head, 0U, (uint32_t)k_t_fb);
  ra8_imgdec_image_t out = {};
  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_imgdec_decode(&dec, &req, &out));
  TEST_ASSERT_EQ(0U, (uint32_t)arena.offset);

  TEST_END("a PNG declared as WebP is refused");
}

/**
 * @test internal_test_oversize_refused
 * @brief A frame wider than the facade's per-axis cap is refused on the
 *        header, before any scratch is bumped.
 * @return None.
 * @pre The fixture bytes remain valid for the call.
 * @post The arena is untouched.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_oversize_refused(void)
{
  TEST_BEGIN("a frame past the per-axis cap is refused on the header");

  ra8_webp_arena_t arena = internal_fresh_arena();
  ra8_imgdec_t     dec   = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_webp_imgdec_bind(&dec, &arena));

  static_assert((uint32_t)k_t_wide_dim > (uint32_t)k_ra8_webp_max_dim,
                "the wide fixture must exceed the cap it is here to prove");
  const ra8_imgdec_req_t req =
    internal_req(s_webp_wide, (uint32_t)sizeof s_webp_wide, 0U, (uint32_t)k_t_fb);
  ra8_imgdec_image_t out = {};
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_imgdec_decode(&dec, &req, &out));
  TEST_ASSERT_EQ(0U, (uint32_t)arena.offset);

  TEST_END("a frame past the per-axis cap is refused on the header");
}

/**
 * @test internal_test_destination_too_small
 * @brief A destination that cannot hold `height * stride` is refused by the
 *        backend, and a stride narrower than one packed row likewise.
 * @return None.
 * @pre The fixture bytes remain valid for the call.
 * @post Nothing is written to the destination.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_destination_too_small(void)
{
  TEST_BEGIN("a short destination and a thin stride are refused");

  ra8_webp_arena_t arena = internal_fresh_arena();
  ra8_imgdec_t     dec   = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_webp_imgdec_bind(&dec, &arena));
  ra8_imgdec_image_t out = {};

  const ra8_imgdec_req_t short_dst =
    internal_req(s_webp_lossless, (uint32_t)sizeof s_webp_lossless, 0U, (uint32_t)k_t_fb - 1U);
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_imgdec_decode(&dec, &short_dst, &out));

  const ra8_imgdec_req_t thin_stride = internal_req(
    s_webp_lossless, (uint32_t)sizeof s_webp_lossless, (uint32_t)k_t_stride - 1U, (uint32_t)k_t_fb);
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_imgdec_decode(&dec, &thin_stride, &out));

  TEST_END("a short destination and a thin stride are refused");
}

/**
 * @test internal_test_probe_through_seam
 * @brief The fabric's pre-decode query reports the fixture's geometry and
 *        refuses the oversize frame, without touching the arena.
 * @return None.
 * @pre The fixture bytes remain valid for the call.
 * @post No decode is attempted.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_probe_through_seam(void)
{
  TEST_BEGIN("the pre-decode query answers without touching the arena");

  ra8_webp_arena_t  arena = internal_fresh_arena();
  ra8_imgdec_t      dec   = {};
  ra8_imgdec_geom_t geom  = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_webp_imgdec_bind(&dec, &arena));

  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_imgdec_probe(&dec, s_webp_lossless, (uint32_t)sizeof s_webp_lossless, &geom));
  TEST_ASSERT_EQ(k_ra8_imgdec_format_webp, geom.format);
  TEST_ASSERT_EQ((uint32_t)k_t_dim, geom.width_px);
  TEST_ASSERT_EQ((uint32_t)k_t_dim, geom.height_px);

  TEST_ASSERT(k_ra8_ok !=
                   ra8_imgdec_probe(&dec, s_png_head, (uint32_t)sizeof s_png_head, &geom));
  TEST_ASSERT_EQ(0U, (uint32_t)arena.offset);

  TEST_END("the pre-decode query answers without touching the arena");
}

/**
 * @brief Run every case in this suite.
 * @return int 0 when every assertion held.
 * @retval 0 The suite passed.
 * @pre The process owns the shared scratch and destination buffers.
 * @post Every case has run.
 * @note Not thread-safe.
 * @since 0.1.0
 */
int main(void)
{
  internal_test_bind_guards();
  internal_test_caps_matrix();
  internal_test_supports_pairs();
  internal_test_decode_lossless_golden();
  internal_test_decode_lossy_runs();
  internal_test_decode_padded_stride();
  internal_test_wrong_container_refused();
  internal_test_oversize_refused();
  internal_test_destination_too_small();
  internal_test_probe_through_seam();
  return 0;
}
