/**
 * @file test_ra8_img_imgdec.c
 * @brief Host tests for the stb_image residue bound as an `ra8_imgdec`
 *        backend (RA8FW-308).
 *
 * @details Every case drives the backend through ::ra8_imgdec_decode rather
 * than calling the vtable directly, because the division of labour is the
 * point: the fabric owns the pointer and capability gates, this module owns
 * the container check, the geometry ceiling and the destination fit. The two
 * fixtures are hand-built containers small enough to read in the source, so
 * no binary fixture file and no network input is needed.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_img_arena.h"
#include "ra8_img_imgdec.h"
#include "ra8_imgdec.h"
#include "ra8_imgdec_backend.h"
#include "reflow_image.h"
#include "unity_minimal.h"

/** @brief Fixture sizes (no magic numbers). */
enum : uint32_t {
  k_t_w          = 2U,     /**< Fixture width.          */
  k_t_h          = 2U,     /**< Fixture height.         */
  k_t_rgb        = 3U,     /**< RGB888 bytes/pixel.     */
  k_t_rgba       = 4U,     /**< RGBA8888 bytes/pixel.   */
  k_t_grey       = 1U,     /**< Grey8 bytes/pixel.      */
  k_t_pool       = 65536U, /**< Arena backing a decode. */
  k_t_dst        = 256U,   /**< Destination capacity.   */
  k_t_pad_stride = 10U,    /**< A padded row stride.    */
  k_t_thin_pool  = 8U,     /**< Too small for a decode. */
};

/**
 * @brief A 2x2 24-bit BMP: blue, green on the bottom row; red, white on top.
 * @details Bottom-up rows with the 4-byte row padding a BMP requires, so the
 *          padding path is exercised rather than assumed away.
 */
static const uint8_t s_bmp[] = {
  0x42, 0x4D, 0x46, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x36, 0x00,
  0x00, 0x00, 0x28, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0x00,
  0x00, 0x00, 0x01, 0x00, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
  0x00, 0x00, 0x13, 0x0B, 0x00, 0x00, 0x13, 0x0B, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00, 0xFF, 0x00,
  0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00,
};

/**
 * @brief A 2x2 GIF89a over a four-entry global colour table.
 * @details Indices 0..3 map to red, green, blue, white. Its LZW stream is a
 *          clear code, four literals and an end code, the smallest real GIF
 *          body that still grows the code table.
 */
static const uint8_t s_gif[] = {
  0x47, 0x49, 0x46, 0x38, 0x39, 0x61, 0x02, 0x00, 0x02, 0x00, 0x81, 0x00,
  0x00, 0xFF, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF,
  0xFF, 0x2C, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x02, 0x00, 0x00, 0x02,
  0x03, 0x44, 0x34, 0x05, 0x00,
};

/**
 * @brief A 2x2 8-bit truecolour PNG: red, white on top; blue, green below.
 * @details Both scanlines carry filter type 0 and the IDAT holds one stored
 *          (uncompressed) deflate block, so the whole body is readable in the
 *          source and no encoder is needed to produce it. Same four colours as
 *          the BMP fixture, so a converted consumer can be compared against it.
 */
static const uint8_t s_png[] = {
  0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D,
  0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02,
  0x08, 0x02, 0x00, 0x00, 0x00, 0xFD, 0xD4, 0x9A, 0x73, 0x00, 0x00, 0x00,
  0x19, 0x49, 0x44, 0x41, 0x54, 0x78, 0x01, 0x01, 0x0E, 0x00, 0xF1, 0xFF,
  0x00, 0xFF, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0xFF, 0x00,
  0xFF, 0x00, 0x2D, 0xE0, 0x05, 0xFB, 0xDF, 0xA2, 0xE5, 0x83, 0x00, 0x00,
  0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82,
};

/** @brief Backing store every decode bumps out of. */
static uint8_t s_pool[k_t_pool];

/** @brief Destination the decodes write into. */
static uint8_t s_dst[k_t_dst];

/** @brief The arena handle bound at ::ra8_img_imgdec_bind time. */
static ra8_img_arena_t s_arena;

/**
 * @brief Bind a fresh handle over the fixture arena.
 * @param[out] dec Handle to fill.
 * @return None.
 * @pre @p dec is writable.
 * @post @p dec is bound and the arena is empty.
 * @note File-local helper; no ownership escapes this executable.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_bind(ra8_imgdec_t* dec)
{
  s_arena.base   = s_pool;
  s_arena.cap    = (uint32_t)k_t_pool;
  s_arena.offset = 0U;
  s_arena.live   = 0U;
  (void)memset(s_dst, 0, sizeof(s_dst));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_img_imgdec_bind(dec, &s_arena));
}

/**
 * @brief Build a decode request over one fixture.
 * @param[in] bytes  Encoded container.
 * @param[in] len    Length of @p bytes.
 * @param[in] format Declared container.
 * @param[in] want   Destination layout.
 * @return The request.
 * @pre @p bytes holds @p len bytes.
 * @post No state mutated.
 * @note File-local helper; no ownership escapes this executable.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_imgdec_req_t
internal_req(const uint8_t* bytes, uint32_t len, ra8_imgdec_format_t format, ra8_imgdec_pixel_t want)
{
  ra8_imgdec_req_t req = {};
  req.bytes            = bytes;
  req.byte_count       = len;
  req.arena            = nullptr;
  req.dst              = s_dst;
  req.dst_bytes        = (uint32_t)k_t_dst;
  req.dst_stride       = 0U;
  req.format           = format;
  req.want             = want;
  return req;
}

/**
 * @brief The advertised matrix is PNG, GIF and BMP, never JPEG or TGA.
 * @return None.
 * @pre None.
 * @post No state mutated beyond the local handle.
 * @note File-local case; no ownership escapes this executable.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_caps_are_the_residue(void)
{
  TEST_BEGIN("caps advertise what stb really decodes, never TGA or JPEG");

  ra8_imgdec_t dec = {};
  internal_bind(&dec);

  ra8_imgdec_caps_t caps = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_get_caps(&dec, &caps));

  TEST_ASSERT_EQ((uint32_t)k_ra8_imgdec_format_gif,
                 caps.formats & (uint32_t)k_ra8_imgdec_format_gif);
  TEST_ASSERT_EQ((uint32_t)k_ra8_imgdec_format_bmp,
                 caps.formats & (uint32_t)k_ra8_imgdec_format_bmp);
  /* PNG is advertised because stb is the only bindable PNG decoder in the
   * tree: jof_png.c is RA8_PRIV, pull-based and has no whole-frame entry. A
   * binder refusing PNG could not stand in for the reflow or RABOOK paths,
   * which decode PNG through this very stbi call today. */
  TEST_ASSERT_EQ((uint32_t)k_ra8_imgdec_format_png,
                 caps.formats & (uint32_t)k_ra8_imgdec_format_png);
  /* The first-party codec owns JPEG and nothing is blocked by leaving it
   * there. TGA is not compiled into stb_image_impl.c at all. */
  TEST_ASSERT_EQ(0U, caps.formats & (uint32_t)k_ra8_imgdec_format_jpeg);
  TEST_ASSERT_EQ(0U, caps.formats & (uint32_t)k_ra8_imgdec_format_tga);
  TEST_ASSERT_EQ(0U, caps.formats & (uint32_t)k_ra8_imgdec_format_webp);

  /* The arena came through the binder, so the request's arena is untouched. */
  TEST_ASSERT_EQ(0U, caps.scratch_bytes);
  TEST_ASSERT(caps.streams == false);

  TEST_END("caps advertise what stb really decodes, never TGA or JPEG");
}

/**
 * @brief A BMP decodes to the pixels its rows declare, bottom-up resolved.
 * @return None.
 * @pre None.
 * @post No state mutated beyond the fixture buffers.
 * @note File-local case; no ownership escapes this executable.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_bmp_decodes(void)
{
  TEST_BEGIN("a BMP decodes to RGB888 and reports its surface");

  ra8_imgdec_t dec = {};
  internal_bind(&dec);

  const ra8_imgdec_req_t req =
    internal_req(s_bmp, (uint32_t)sizeof(s_bmp), k_ra8_imgdec_format_bmp, k_ra8_imgdec_pixel_rgb888);
  ra8_imgdec_image_t img = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_decode(&dec, &req, &img));

  TEST_ASSERT_EQ((uint32_t)k_t_w, img.width_px);
  TEST_ASSERT_EQ((uint32_t)k_t_h, img.height_px);
  TEST_ASSERT_EQ((uint32_t)k_t_w * (uint32_t)k_t_rgb, img.stride);
  TEST_ASSERT_EQ((uint32_t)k_t_w * (uint32_t)k_t_h * (uint32_t)k_t_rgb, img.used_bytes);
  TEST_ASSERT_EQ((uint32_t)k_ra8_imgdec_format_bmp, (uint32_t)img.format);
  TEST_ASSERT_EQ((uint32_t)k_ra8_imgdec_pixel_rgb888, (uint32_t)img.pixel);
  TEST_ASSERT(img.had_alpha == false);

  /* Top row first: red then white; then blue and green. */
  TEST_ASSERT_EQ(0xFFU, s_dst[0]);
  TEST_ASSERT_EQ(0x00U, s_dst[1]);
  TEST_ASSERT_EQ(0x00U, s_dst[2]);
  TEST_ASSERT_EQ(0xFFU, s_dst[3]);
  TEST_ASSERT_EQ(0xFFU, s_dst[4]);
  TEST_ASSERT_EQ(0xFFU, s_dst[5]);
  TEST_ASSERT_EQ(0x00U, s_dst[6]);
  TEST_ASSERT_EQ(0x00U, s_dst[7]);
  TEST_ASSERT_EQ(0xFFU, s_dst[8]);

  /* The arena is drained on the way out, not left holding the surface. */
  TEST_ASSERT_EQ(0U, s_arena.offset);
  TEST_ASSERT_EQ(0U, s_arena.live);

  TEST_END("a BMP decodes to RGB888 and reports its surface");
}

/**
 * @brief A GIF decodes even though stb's own header probe refuses it.
 * @details This is the case that pins the choice of geometry probe. An earlier
 *          shape pre-flighted with `stbi_info_from_memory()`, which reports
 *          failure on this fixture while the decoder handles it happily, so
 *          that shape refused a whole advertised format.
 * @return None.
 * @pre None.
 * @post No state mutated beyond the fixture buffers.
 * @note File-local case; no ownership escapes this executable.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_gif_decodes(void)
{
  TEST_BEGIN("a GIF decodes through the shared header probe");

  ra8_imgdec_t dec = {};
  internal_bind(&dec);

  const ra8_imgdec_req_t req =
    internal_req(s_gif, (uint32_t)sizeof(s_gif), k_ra8_imgdec_format_gif, k_ra8_imgdec_pixel_rgba8888);
  ra8_imgdec_image_t img = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_decode(&dec, &req, &img));

  TEST_ASSERT_EQ((uint32_t)k_t_w, img.width_px);
  TEST_ASSERT_EQ((uint32_t)k_t_h, img.height_px);
  TEST_ASSERT_EQ((uint32_t)k_ra8_imgdec_format_gif, (uint32_t)img.format);
  TEST_ASSERT_EQ((uint32_t)k_ra8_imgdec_pixel_rgba8888, (uint32_t)img.pixel);
  /* A GIF carries a transparency slot, so stb reports four source channels. */
  TEST_ASSERT(img.had_alpha == true);

  /* Row 0: red then green, both fully opaque. */
  TEST_ASSERT_EQ(0xFFU, s_dst[0]);
  TEST_ASSERT_EQ(0x00U, s_dst[1]);
  TEST_ASSERT_EQ(0x00U, s_dst[2]);
  TEST_ASSERT_EQ(0xFFU, s_dst[3]);
  TEST_ASSERT_EQ(0x00U, s_dst[4]);
  TEST_ASSERT_EQ(0xFFU, s_dst[5]);

  TEST_END("a GIF decodes through the shared header probe");
}

/**
 * @brief A PNG decodes, because stb is the only bindable PNG decoder in-tree.
 * @details The case that pins the caps decision. RA8FW-308 proposes a separate
 *          `ra8_imgdec_bind_png()` over a promoted `libs/ra8_png`, but
 *          `priv_jof_png_rows()` is RA8_PRIV, pull-based and has no
 *          whole-frame entry, so until that promotion happens this backend is
 *          the seam's only route to a PNG. Refusing one here would leave the
 *          reflow and RABOOK paths unable to move onto the seam at all.
 * @return None.
 * @pre None.
 * @post No state mutated beyond the fixture buffers.
 * @note File-local case; no ownership escapes this executable.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_png_decodes(void)
{
  TEST_BEGIN("a PNG decodes through the advertised backend");

  ra8_imgdec_t dec = {};
  internal_bind(&dec);

  const ra8_imgdec_req_t req =
    internal_req(s_png, (uint32_t)sizeof(s_png), k_ra8_imgdec_format_png, k_ra8_imgdec_pixel_rgb888);
  ra8_imgdec_image_t img = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_decode(&dec, &req, &img));

  TEST_ASSERT_EQ((uint32_t)k_t_w, img.width_px);
  TEST_ASSERT_EQ((uint32_t)k_t_h, img.height_px);
  TEST_ASSERT_EQ((uint32_t)k_ra8_imgdec_format_png, (uint32_t)img.format);
  TEST_ASSERT_EQ((uint32_t)k_ra8_imgdec_pixel_rgb888, (uint32_t)img.pixel);
  /* Colour type 2 carries no alpha channel, so stb reports three. */
  TEST_ASSERT(img.had_alpha == false);

  /* Row 0 is red then white; row 1 is blue then green. PNG rows are top-down,
   * so unlike the BMP fixture no vertical flip is involved. */
  TEST_ASSERT_EQ(0xFFU, s_dst[0]);
  TEST_ASSERT_EQ(0x00U, s_dst[1]);
  TEST_ASSERT_EQ(0x00U, s_dst[2]);
  TEST_ASSERT_EQ(0xFFU, s_dst[3]);
  TEST_ASSERT_EQ(0xFFU, s_dst[4]);
  TEST_ASSERT_EQ(0xFFU, s_dst[5]);

  const uint32_t row1 = (uint32_t)k_t_w * (uint32_t)k_t_rgb;
  TEST_ASSERT_EQ(0x00U, s_dst[row1 + 0U]);
  TEST_ASSERT_EQ(0x00U, s_dst[row1 + 1U]);
  TEST_ASSERT_EQ(0xFFU, s_dst[row1 + 2U]);
  TEST_ASSERT_EQ(0x00U, s_dst[row1 + 3U]);
  TEST_ASSERT_EQ(0xFFU, s_dst[row1 + 4U]);
  TEST_ASSERT_EQ(0x00U, s_dst[row1 + 5U]);

  /* The arena drains on the way out, exactly as the other formats do. */
  TEST_ASSERT_EQ(0U, s_arena.offset);
  TEST_ASSERT_EQ(0U, s_arena.live);

  TEST_END("a PNG decodes through the advertised backend");
}

/**
 * @brief Bytes that are not the declared container are refused.
 * @return None.
 * @pre None.
 * @post No state mutated beyond the fixture buffers.
 * @note File-local case; no ownership escapes this executable.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_container_is_verified(void)
{
  TEST_BEGIN("a PNG declared as a BMP is refused, not decoded");

  ra8_imgdec_t dec = {};
  internal_bind(&dec);

  /* stb_image decodes PNG perfectly well, so nothing but this module's own
   * container check stands between a mis-routed request and a wrong decoder. */
  const ra8_imgdec_req_t as_bmp =
    internal_req(s_png, (uint32_t)sizeof(s_png), k_ra8_imgdec_format_bmp, k_ra8_imgdec_pixel_rgb888);
  ra8_imgdec_image_t img = {};
  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_imgdec_decode(&dec, &as_bmp, &img));

  /* And a GIF declared as a BMP is the same refusal, both being advertised. */
  const ra8_imgdec_req_t swapped =
    internal_req(s_gif, (uint32_t)sizeof(s_gif), k_ra8_imgdec_format_bmp, k_ra8_imgdec_pixel_rgb888);
  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_imgdec_decode(&dec, &swapped, &img));

  TEST_END("a PNG declared as a BMP is refused, not decoded");
}

/**
 * @brief A padded destination stride is honoured, a short one refused.
 * @return None.
 * @pre None.
 * @post No state mutated beyond the fixture buffers.
 * @note File-local case; no ownership escapes this executable.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_stride_requests(void)
{
  TEST_BEGIN("a padded stride is honoured and a short one refused");

  ra8_imgdec_t dec = {};
  internal_bind(&dec);

  ra8_imgdec_req_t padded =
    internal_req(s_bmp, (uint32_t)sizeof(s_bmp), k_ra8_imgdec_format_bmp, k_ra8_imgdec_pixel_rgb888);
  padded.dst_stride      = (uint32_t)k_t_pad_stride;
  ra8_imgdec_image_t img = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_decode(&dec, &padded, &img));
  TEST_ASSERT_EQ((uint32_t)k_t_pad_stride, img.stride);
  /* Row 1 lands at the padded offset, not at one packed row. */
  TEST_ASSERT_EQ(0x00U, s_dst[k_t_pad_stride + 0U]);
  TEST_ASSERT_EQ(0x00U, s_dst[k_t_pad_stride + 1U]);
  TEST_ASSERT_EQ(0xFFU, s_dst[k_t_pad_stride + 2U]);

  ra8_imgdec_req_t thin = padded;
  thin.dst_stride       = 1U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_imgdec_decode(&dec, &thin, &img));

  TEST_END("a padded stride is honoured and a short one refused");
}

/**
 * @brief A destination too small for the surface is refused before any decode.
 * @return None.
 * @pre None.
 * @post No state mutated beyond the fixture buffers.
 * @note File-local case; no ownership escapes this executable.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_destination_too_small(void)
{
  TEST_BEGIN("a destination too small is refused and the arena stays empty");

  ra8_imgdec_t dec = {};
  internal_bind(&dec);

  ra8_imgdec_req_t req =
    internal_req(s_bmp, (uint32_t)sizeof(s_bmp), k_ra8_imgdec_format_bmp, k_ra8_imgdec_pixel_rgb888);
  req.dst_bytes          = ((uint32_t)k_t_w * (uint32_t)k_t_h * (uint32_t)k_t_rgb) - 1U;
  ra8_imgdec_image_t img = {};
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_imgdec_decode(&dec, &req, &img));

  /* Refused on the header, so nothing was ever bumped out of the arena. */
  TEST_ASSERT_EQ(0U, s_arena.offset);
  TEST_ASSERT_EQ(0U, s_arena.live);

  /* A destination sized exactly to the surface is accepted, so the refusal
   * above is a real boundary and not an off-by-one against padding. */
  req.dst_bytes = (uint32_t)k_t_w * (uint32_t)k_t_h * (uint32_t)k_t_rgb;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_decode(&dec, &req, &img));

  TEST_END("a destination too small is refused and the arena stays empty");
}

/**
 * @brief An arena too small to hold the surface reports a shortage, not a
 *        format error, and leaves nothing bound.
 * @return None.
 * @pre None.
 * @post No state mutated beyond the fixture buffers.
 * @note File-local case; no ownership escapes this executable.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_arena_exhaustion(void)
{
  TEST_BEGIN("an arena too small reports a shortage and drains");

  ra8_imgdec_t dec = {};
  internal_bind(&dec);
  s_arena.cap = (uint32_t)k_t_thin_pool;

  const ra8_imgdec_req_t req =
    internal_req(s_bmp, (uint32_t)sizeof(s_bmp), k_ra8_imgdec_format_bmp, k_ra8_imgdec_pixel_rgb888);
  ra8_imgdec_image_t img = {};
  /* A shortage and a corrupt body are one null pointer out of stb; the reason
   * string is what tells them apart, and getting that mapping wrong would
   * report an arena that needs growing as an image that never will. */
  TEST_ASSERT_EQ(k_ra8_err_no_mem, ra8_imgdec_decode(&dec, &req, &img));
  TEST_ASSERT_EQ(0U, s_arena.offset);
  TEST_ASSERT_EQ(0U, s_arena.live);

  TEST_END("an arena too small reports a shortage and drains");
}

/**
 * @brief A layout the backend never advertised is refused by the fabric.
 * @return None.
 * @pre None.
 * @post No state mutated beyond the fixture buffers.
 * @note File-local case; no ownership escapes this executable.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_grey_is_advertised_and_works(void)
{
  TEST_BEGIN("grey8 is advertised and really decodes");

  ra8_imgdec_t dec = {};
  internal_bind(&dec);

  bool ok = false;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_imgdec_supports(&dec, k_ra8_imgdec_format_bmp, k_ra8_imgdec_pixel_grey8, &ok));
  TEST_ASSERT(ok == true);

  const ra8_imgdec_req_t req =
    internal_req(s_bmp, (uint32_t)sizeof(s_bmp), k_ra8_imgdec_format_bmp, k_ra8_imgdec_pixel_grey8);
  ra8_imgdec_image_t img = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_decode(&dec, &req, &img));
  TEST_ASSERT_EQ((uint32_t)k_t_w * (uint32_t)k_t_grey, img.stride);
  TEST_ASSERT_EQ((uint32_t)k_ra8_imgdec_pixel_grey8, (uint32_t)img.pixel);

  /* An unadvertised format is refused by the fabric before the hook runs. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_imgdec_supports(&dec, k_ra8_imgdec_format_jpeg, k_ra8_imgdec_pixel_rgb888, &ok));
  TEST_ASSERT(ok == false);

  TEST_END("grey8 is advertised and really decodes");
}

/**
 * @brief The binder refuses a null handle and a null arena.
 * @return None.
 * @pre None.
 * @post No state mutated beyond the local handle.
 * @note File-local case; no ownership escapes this executable.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_bind_guards(void)
{
  TEST_BEGIN("bind refuses a null handle and a null arena");

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_img_imgdec_bind(nullptr, &s_arena));

  ra8_imgdec_t dec = {};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_img_imgdec_bind(&dec, nullptr));
  /* A refused bind leaves the handle untouched, so it stays unusable. */
  TEST_ASSERT_NULL(dec.iface);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_img_imgdec_bind(&dec, &s_arena));
  TEST_ASSERT_NOT_NULL(dec.iface);
  TEST_ASSERT(dec.ctx == (void*)&s_arena);

  /* One vtable instance, never one per handle. */
  ra8_imgdec_t again = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_img_imgdec_bind(&again, &s_arena));
  TEST_ASSERT(again.iface == dec.iface);

  TEST_END("bind refuses a null handle and a null arena");
}

int main(void)
{
  internal_test_caps_are_the_residue();
  internal_test_bmp_decodes();
  internal_test_gif_decodes();
  internal_test_png_decodes();
  internal_test_container_is_verified();
  internal_test_stride_requests();
  internal_test_destination_too_small();
  internal_test_arena_exhaustion();
  internal_test_grey_is_advertised_and_works();
  internal_test_bind_guards();
  return 0;
}
