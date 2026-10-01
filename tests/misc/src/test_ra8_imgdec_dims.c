/**
 * @file test_ra8_imgdec_dims.c
 * @brief Host tests for the shared container geometry probe (RA8FW-308).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_imgdec.h"
#include "unity_minimal.h"

/** @brief Fixture sizes (no magic numbers). */
enum {
  k_buf_bytes  = 64, /**< Longest encoded fixture built here.   */
  k_png_bytes  = 24, /**< Signature + length + IHDR + two dims. */
  k_gif_bytes  = 10, /**< Header + logical screen descriptor.   */
  k_bmp_bytes  = 26, /**< File header + 40-byte DIB prefix.     */
  k_webp_bytes = 30  /**< RIFF + chunk header + frame header.   */
};

/** @brief The eight bytes every PNG opens with. */
static const uint8_t k_png_sig[8] = {0x89U, 0x50U, 0x4EU, 0x47U, 0x0DU, 0x0AU, 0x1AU, 0x0AU};

/**
 * @brief Build a minimal PNG head: signature, chunk length, type, geometry.
 *
 * @param[out] buf   Destination, at least ::k_png_bytes.
 * @param[in]  type  Four-byte chunk type to write at offset 12.
 * @param[in]  width Width to store big-endian at offset 16.
 * @param[in]  hgt   Height to store big-endian at offset 20.
 */
RA8_INTERNAL static void internal_make_png(uint8_t*    buf,
                                           const char* type,
                                           uint32_t    width,
                                           uint32_t    hgt) {
  memset(buf, 0, (size_t)k_png_bytes);
  memcpy(&buf[0], k_png_sig, sizeof k_png_sig);
  buf[11] = 13U; /* IHDR payload length, big-endian tail */
  memcpy(&buf[12], type, 4U);
  buf[16] = (uint8_t)((width >> 24U) & 0xFFU);
  buf[17] = (uint8_t)((width >> 16U) & 0xFFU);
  buf[18] = (uint8_t)((width >> 8U) & 0xFFU);
  buf[19] = (uint8_t)(width & 0xFFU);
  buf[20] = (uint8_t)((hgt >> 24U) & 0xFFU);
  buf[21] = (uint8_t)((hgt >> 16U) & 0xFFU);
  buf[22] = (uint8_t)((hgt >> 8U) & 0xFFU);
  buf[23] = (uint8_t)(hgt & 0xFFU);
}

/** @brief Build a RIFF/WEBP head carrying @p chunk as its first chunk. */
RA8_INTERNAL static void internal_make_webp(uint8_t* buf, const char* chunk) {
  memset(buf, 0, (size_t)k_buf_bytes);
  memcpy(&buf[0], "RIFF", 4U);
  memcpy(&buf[8], "WEBP", 4U);
  memcpy(&buf[12], chunk, 4U);
}

RA8_INTERNAL static void internal_test_png_geometry(void) {
  TEST_BEGIN("PNG geometry comes from a checked IHDR");

  uint8_t           buf[k_buf_bytes] = {};
  ra8_imgdec_geom_t got              = {};

  internal_make_png(buf, "IHDR", 1280U, 800U);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_dims(buf, (uint32_t)k_png_bytes, &got));
  TEST_ASSERT_EQ(k_ra8_imgdec_format_png, got.format);
  TEST_ASSERT_EQ(1280U, got.width_px);
  TEST_ASSERT_EQ(800U, got.height_px);

  /* A first chunk that is not IHDR is refused, not read as a size. This is
   * the one behaviour the jof_produce.c copy does not have. */
  internal_make_png(buf, "sRGB", 1280U, 800U);
  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_imgdec_dims(buf, (uint32_t)k_png_bytes, &got));
  TEST_ASSERT_EQ(k_ra8_imgdec_format_none, got.format);
  TEST_ASSERT_EQ(0U, got.width_px);

  /* One byte short of the height field. */
  internal_make_png(buf, "IHDR", 4U, 4U);
  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_imgdec_dims(buf, (uint32_t)k_png_bytes - 1U, &got));

  TEST_END("PNG geometry comes from a checked IHDR");
}

RA8_INTERNAL static void internal_test_gif_and_bmp(void) {
  TEST_BEGIN("GIF and BMP geometry");

  uint8_t           buf[k_buf_bytes] = {};
  ra8_imgdec_geom_t got              = {};

  memcpy(&buf[0], "GIF89a", 6U);
  buf[6] = 0x40U; /* 320 little-endian */
  buf[7] = 0x01U;
  buf[8] = 0xF0U; /* 240 little-endian */
  buf[9] = 0x00U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_dims(buf, (uint32_t)k_gif_bytes, &got));
  TEST_ASSERT_EQ(k_ra8_imgdec_format_gif, got.format);
  TEST_ASSERT_EQ(320U, got.width_px);
  TEST_ASSERT_EQ(240U, got.height_px);

  /* BITMAPINFOHEADER, height negative: a top-down row order, same geometry. */
  memset(buf, 0, sizeof buf);
  memcpy(&buf[0], "BM", 2U);
  buf[14] = 40U; /* DIB size */
  buf[18] = 100U;
  buf[22] = 0x9CU; /* -100 as int32 little-endian */
  buf[23] = 0xFFU;
  buf[24] = 0xFFU;
  buf[25] = 0xFFU;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_dims(buf, (uint32_t)k_bmp_bytes, &got));
  TEST_ASSERT_EQ(k_ra8_imgdec_format_bmp, got.format);
  TEST_ASSERT_EQ(100U, got.width_px);
  TEST_ASSERT_EQ(100U, got.height_px);

  /* BITMAPCOREHEADER: the same fields are 16 bits wide. */
  memset(buf, 0, sizeof buf);
  memcpy(&buf[0], "BM", 2U);
  buf[14] = 12U; /* DIB size */
  buf[18] = 0x20U;
  buf[19] = 0x00U; /* 32 */
  buf[20] = 0x10U;
  buf[21] = 0x00U; /* 16 */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_dims(buf, 22U, &got));
  TEST_ASSERT_EQ(32U, got.width_px);
  TEST_ASSERT_EQ(16U, got.height_px);

  TEST_END("GIF and BMP geometry");
}

RA8_INTERNAL static void internal_test_webp_three_flavours(void) {
  TEST_BEGIN("WebP geometry from VP8, VP8L and VP8X");

  uint8_t           buf[k_buf_bytes] = {};
  ra8_imgdec_geom_t got              = {};

  /* Lossy: three-byte frame tag, the 9D 01 2A start code, then 14-bit dims
   * with two scale bits above each. */
  internal_make_webp(buf, "VP8 ");
  buf[23] = 0x9DU;
  buf[24] = 0x01U;
  buf[25] = 0x2AU;
  buf[26] = 0x40U; /* 320, scale bits zero */
  buf[27] = 0x01U;
  buf[28] = 0xF0U; /* 240 */
  buf[29] = 0x00U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_dims(buf, (uint32_t)k_webp_bytes, &got));
  TEST_ASSERT_EQ(k_ra8_imgdec_format_webp, got.format);
  TEST_ASSERT_EQ(320U, got.width_px);
  TEST_ASSERT_EQ(240U, got.height_px);

  /* The two high bits of each field are an upscale hint, never part of the
   * size, so a fixture with them set must read the same 320x240. */
  buf[27] = 0x01U | 0xC0U;
  buf[29] = 0x00U | 0xC0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_dims(buf, (uint32_t)k_webp_bytes, &got));
  TEST_ASSERT_EQ(320U, got.width_px);
  TEST_ASSERT_EQ(240U, got.height_px);

  /* Lossless: 0x2F, then width-1 and height-1 packed 14 bits each. */
  internal_make_webp(buf, "VP8L");
  buf[20]              = 0x2FU;
  const uint32_t packed = (319U & 0x3FFFU) | ((239U & 0x3FFFU) << 14U);
  buf[21]              = (uint8_t)(packed & 0xFFU);
  buf[22]              = (uint8_t)((packed >> 8U) & 0xFFU);
  buf[23]              = (uint8_t)((packed >> 16U) & 0xFFU);
  buf[24]              = (uint8_t)((packed >> 24U) & 0xFFU);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_dims(buf, 25U, &got));
  TEST_ASSERT_EQ(320U, got.width_px);
  TEST_ASSERT_EQ(240U, got.height_px);

  /* A VP8L payload whose signature byte is wrong is refused. */
  buf[20] = 0x2EU;
  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_imgdec_dims(buf, 25U, &got));

  /* Extended: 24-bit canvas minus one, width then height. */
  internal_make_webp(buf, "VP8X");
  buf[24] = 0x3FU; /* 319 */
  buf[25] = 0x01U;
  buf[26] = 0x00U;
  buf[27] = 0xEFU; /* 239 */
  buf[28] = 0x00U;
  buf[29] = 0x00U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_dims(buf, (uint32_t)k_webp_bytes, &got));
  TEST_ASSERT_EQ(320U, got.width_px);
  TEST_ASSERT_EQ(240U, got.height_px);

  TEST_END("WebP geometry from VP8, VP8L and VP8X");
}

RA8_INTERNAL static void internal_test_webp_unreadable_first_chunk(void) {
  TEST_BEGIN("a recognised WebP with an unreadable first chunk is refused");

  uint8_t           buf[k_buf_bytes] = {};
  ra8_imgdec_geom_t got              = {};

  /* ALPH may open the chunk list only after a VP8X; on its own it carries no
   * canvas size, so it is refused rather than guessed at. */
  internal_make_webp(buf, "ALPH");
  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_imgdec_dims(buf, (uint32_t)k_webp_bytes, &got));
  TEST_ASSERT_EQ(k_ra8_imgdec_format_none, got.format);

  /* The sniff still recognises the container: dims refuses, sniff does not. */
  ra8_imgdec_format_t fmt = k_ra8_imgdec_format_none;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_sniff(buf, (uint32_t)k_webp_bytes, &fmt));
  TEST_ASSERT_EQ(k_ra8_imgdec_format_webp, fmt);

  TEST_END("a recognised WebP with an unreadable first chunk is refused");
}

RA8_INTERNAL static void internal_test_jpeg_marker_walk(void) {
  TEST_BEGIN("JPEG geometry comes from the first real SOF");

  uint8_t           buf[k_buf_bytes] = {};
  ra8_imgdec_geom_t got              = {};
  uint32_t          at               = 0U;

  buf[at++] = 0xFFU; /* SOI */
  buf[at++] = 0xD8U;
  buf[at++] = 0xFFU; /* APP0, length 8, skipped whole */
  buf[at++] = 0xE0U;
  buf[at++] = 0x00U;
  buf[at++] = 0x08U;
  at += 6U;
  buf[at++] = 0xFFU; /* DHT: inside 0xC0..0xCF but not a frame header */
  buf[at++] = 0xC4U;
  buf[at++] = 0x00U;
  buf[at++] = 0x09U;
  at += 7U;
  buf[at++] = 0xFFU; /* SOF0 */
  buf[at++] = 0xC0U;
  buf[at++] = 0x00U;
  buf[at++] = 0x11U;
  /* Sample precision, then height 480 and width 640, each big-endian. */
  buf[at++] = 0x08U;
  buf[at++] = 0x01U;
  buf[at++] = 0xE0U;
  buf[at++] = 0x02U;
  buf[at++] = 0x80U;

  TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_dims(buf, at, &got));
  TEST_ASSERT_EQ(k_ra8_imgdec_format_jpeg, got.format);
  TEST_ASSERT_EQ(640U, got.width_px);
  TEST_ASSERT_EQ(480U, got.height_px);

  TEST_END("JPEG geometry comes from the first real SOF");
}

RA8_INTERNAL static void internal_test_jpeg_without_a_frame(void) {
  TEST_BEGIN("a JPEG whose scan starts before any SOF is refused");

  uint8_t           buf[k_buf_bytes] = {};
  ra8_imgdec_geom_t got              = {};

  buf[0] = 0xFFU; /* SOI */
  buf[1] = 0xD8U;
  /* A marker prefix keeps the sniff calling this a JPEG; SOS then means the
   * entropy data has been reached, so the walk stops with no frame header. */
  buf[2] = 0xFFU;
  buf[3] = 0xDAU;
  buf[4] = 0x00U;
  buf[5] = 0x08U;

  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_imgdec_dims(buf, 12U, &got));
  TEST_ASSERT_EQ(k_ra8_imgdec_format_none, got.format);
  TEST_ASSERT_EQ(0U, got.width_px);

  TEST_END("a JPEG whose scan starts before any SOF is refused");
}

RA8_INTERNAL static void internal_test_refusals(void) {
  TEST_BEGIN("null, empty and unrecognised inputs");

  uint8_t           buf[k_buf_bytes] = {};
  ra8_imgdec_geom_t got              = {.format    = k_ra8_imgdec_format_png,
                                        .width_px  = 7U,
                                        .height_px = 7U};

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_imgdec_dims(nullptr, 8U, &got));
  TEST_ASSERT_EQ(k_ra8_imgdec_format_none, got.format);
  TEST_ASSERT_EQ(0U, got.width_px);
  TEST_ASSERT_EQ(0U, got.height_px);

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_imgdec_dims(buf, 8U, nullptr));

  /* Zero length and an unrecognised buffer come straight from the sniff, so
   * they keep the sniff's own codes rather than gaining new ones. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_imgdec_dims(buf, 0U, &got));
  memcpy(&buf[0], "not an image", 12U);
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_imgdec_dims(buf, 12U, &got));

  TEST_END("null, empty and unrecognised inputs");
}

RA8_INTERNAL static void internal_test_range_check(void) {
  TEST_BEGIN("a declared dimension outside the fabric's bounds is refused");

  uint8_t           buf[k_buf_bytes] = {};
  ra8_imgdec_geom_t got              = {};

  internal_make_png(buf, "IHDR", 0U, 480U);
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_imgdec_dims(buf, (uint32_t)k_png_bytes, &got));

  internal_make_png(buf, "IHDR", 640U, 0U);
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_imgdec_dims(buf, (uint32_t)k_png_bytes, &got));

  internal_make_png(buf, "IHDR", (uint32_t)k_ra8_imgdec_dim_max + 1U, 480U);
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_imgdec_dims(buf, (uint32_t)k_png_bytes, &got));
  TEST_ASSERT_EQ(0U, got.width_px);

  /* Exactly at the bound is accepted. */
  internal_make_png(buf, "IHDR", (uint32_t)k_ra8_imgdec_dim_max, 1U);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_dims(buf, (uint32_t)k_png_bytes, &got));
  TEST_ASSERT_EQ((uint32_t)k_ra8_imgdec_dim_max, got.width_px);

  TEST_END("a declared dimension outside the fabric's bounds is refused");
}

RA8_INTERNAL static void internal_test_window_is_honest(void) {
  TEST_BEGIN("the published read window covers every fixed-offset container");

  /* 30 bytes is the widest fixed-offset read: a RIFF/WEBP lossy frame
   * header. PNG needs 24, BMP 26, GIF 10. */
  TEST_ASSERT_EQ(30U, (uint32_t)k_ra8_imgdec_dims_bytes);

  uint8_t           buf[k_ra8_imgdec_dims_bytes] = {};
  ra8_imgdec_geom_t got                          = {};

  memcpy(&buf[0], "RIFF", 4U);
  memcpy(&buf[8], "WEBP", 4U);
  memcpy(&buf[12], "VP8 ", 4U);
  buf[23] = 0x9DU;
  buf[24] = 0x01U;
  buf[25] = 0x2AU;
  buf[26] = 0x01U;
  buf[28] = 0x01U;

  TEST_ASSERT_EQ(k_ra8_ok, ra8_imgdec_dims(buf, (uint32_t)k_ra8_imgdec_dims_bytes, &got));
  TEST_ASSERT_EQ(1U, got.width_px);
  TEST_ASSERT_EQ(1U, got.height_px);

  TEST_END("the published read window covers every fixed-offset container");
}

int main(void) {
  internal_test_png_geometry();
  internal_test_gif_and_bmp();
  internal_test_webp_three_flavours();
  internal_test_webp_unreadable_first_chunk();
  internal_test_jpeg_marker_walk();
  internal_test_jpeg_without_a_frame();
  internal_test_refusals();
  internal_test_range_check();
  internal_test_window_is_honest();
  return 0;
}
