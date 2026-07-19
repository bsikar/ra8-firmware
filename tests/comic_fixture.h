/**
 * @file comic_fixture.h
 * @brief Shared test fixture: build a tiny, real, stb-decodable PNG in memory.
 *
 * @details
 * Both the CBZ (`test_ra8_comic.c`) and CBR (`test_ra8_rar.c`) suites store real
 * page images inside a crafted archive and prove the reader can extract and
 * decode them. This header emits a minimal but fully valid grayscale PNG
 * (signature + IHDR + a zlib-compressed IDAT + IEND, all chunk CRCs computed with
 * miniz's `mz_crc32`), so `stbi_load_from_memory` / `stbi_info_from_memory`
 * decode it exactly. The image bytes are deterministic in the seed, so the two
 * halves of a round-trip (source vs. extracted) compare byte-for-byte.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "miniz.h"
#include "ra8_img_arena.h"
#include "stb_image.h"

/**
 * @enum comic_fixture_uint8_const_t
 * @brief Named uint8_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint8_t {
  k_cf_pixel_col_mul =
    5U, /**< Column multiplier of the page generator, `seed + r * 3 + c * 5`; coprime with the row multiplier so no two pixels on a page collide. */
  k_png_ihdr_data_len = 13U, /**< An IHDR chunk's data is exactly 13 bytes. */
  k_shift_byte3 =
    24, /**< Shift to the most significant byte; PNG fields are big-endian, so this one is written first. */
  k_png_chunk_overhead =
    12U, /**< Bytes a PNG chunk costs beyond its payload: 4 length + 4 type + 4 CRC. */
  k_png_off_type_b1 =
    5, /**< Second byte of a chunk's four-character type field, which starts at offset 4. */
  k_cf_scratch_kib          = 64U, /**< Scratch pool size in KiB. */
  k_png_off_type_b3         = 7,   /**< Its fourth byte. */
  k_png_ihdr_off_color_type = 9,   /**< IHDR's colour-type byte, 9 into the chunk data. */
} comic_fixture_uint8_const_t;

/**
 * @enum comic_fixture_uint16_const_t
 * @brief Named uint16_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint16_t {
  k_cf_bytes_per_kib =
    1024U, /**< Bytes per KiB, so the pool size above reads as a size and not a bare product. */
} comic_fixture_uint16_const_t;

/**
 * @enum cf_dim_t
 * @brief Fixed bounds for the fixture PNG scratch buffers.
 */
typedef enum : uint16_t {
  k_cf_max_dim  = 16U,                                /**< Max image edge, pixels.  */
  k_cf_raw_max  = (k_cf_max_dim + 1U) * k_cf_max_dim, /**< Filtered scanline bytes. */
  k_cf_comp_max = k_cf_raw_max + 64U,                 /**< zlib IDAT scratch.       */
  k_cf_png_max  = k_cf_comp_max + 64U,                /**< Whole-PNG upper bound.   */
} cf_dim_t;

/** @brief Write a big-endian uint32 (PNG chunk lengths, CRCs, IHDR fields). */
static inline void cf_put_be32(uint8_t* p, uint32_t v)
{
  p[0] = (uint8_t)(v >> k_shift_byte3);
  p[1] = (uint8_t)(v >> 16);
  p[2] = (uint8_t)(v >> 8);
  p[3] = (uint8_t)v;
}

/** @brief Emit one PNG chunk (len + type + data + CRC) and return its size. */
static inline size_t
cf_png_chunk(uint8_t* out, const char* type, const uint8_t* data, uint32_t dlen)
{
  cf_put_be32(&out[0], dlen);
  out[4]                 = (uint8_t)type[0];
  out[k_png_off_type_b1] = (uint8_t)type[1];
  out[6]                 = (uint8_t)type[2];
  out[k_png_off_type_b3] = (uint8_t)type[3];
  if (dlen != 0U) {
    memcpy(&out[8], data, dlen);
  }
  const mz_ulong crc = mz_crc32(MZ_CRC32_INIT, &out[4], (size_t)4U + (size_t)dlen);
  cf_put_be32(&out[8 + dlen], (uint32_t)crc);
  return (size_t)k_png_chunk_overhead + (size_t)dlen;
}

/**
 * @brief Build a minimal grayscale PNG of @p w x @p h into @p out.
 * @param[in]  w    Width in pixels (1..k_cf_max_dim).
 * @param[in]  h    Height in pixels (1..k_cf_max_dim).
 * @param[in]  seed Deterministic pixel-pattern seed.
 * @param[out] out  Destination (>= k_cf_png_max bytes).
 * @param[in]  cap  Capacity of @p out.
 * @return PNG byte length, or 0 on any error / capacity shortfall.
 */
static inline size_t cf_make_png(uint16_t w, uint16_t h, uint8_t seed, uint8_t* out, size_t cap)
{
  uint8_t      raw[k_cf_raw_max];
  const size_t rawlen = (size_t)(w + 1U) * (size_t)h;
  if (rawlen > sizeof(raw)) {
    return 0U;
  }
  for (uint16_t r = 0U; r < h; ++r) {
    raw[(size_t)r * (size_t)(w + 1U)] = 0U; /* PNG "none" row filter */
    for (uint16_t c = 0U; c < w; ++c) {
      raw[((size_t)r * (size_t)(w + 1U)) + 1U + c] =
        (uint8_t)(seed + (r * 3U) + (c * k_cf_pixel_col_mul));
    }
  }
  uint8_t  comp[k_cf_comp_max];
  mz_ulong clen = (mz_ulong)sizeof(comp);
  if (mz_compress2(comp, &clen, raw, (mz_ulong)rawlen, MZ_DEFAULT_LEVEL) != MZ_OK) {
    return 0U;
  }
  if (cap < (size_t)k_cf_png_max) {
    return 0U;
  }
  static const uint8_t sig[8] = {137U, 80U, 78U, 71U, 13U, 10U, 26U, 10U};
  memcpy(out, sig, sizeof(sig));
  size_t  pos                       = sizeof(sig);
  uint8_t ihdr[k_png_ihdr_data_len] = {};
  cf_put_be32(&ihdr[0], w);
  cf_put_be32(&ihdr[4], h);
  ihdr[8]                         = 8U; /* bit depth             */
  ihdr[k_png_ihdr_off_color_type] = 0U; /* color type: grayscale */
  pos += cf_png_chunk(&out[pos], "IHDR", ihdr, k_png_ihdr_data_len);
  pos += cf_png_chunk(&out[pos], "IDAT", comp, (uint32_t)clen);
  pos += cf_png_chunk(&out[pos], "IEND", nullptr, 0U);
  return pos;
}

/**
 * @brief Fully decode @p png through the project's arena-backed stb_image and
 *        check its dimensions.
 * @details The test build redirects `STBI_MALLOC` to the reflow bump arena, so a
 *          pixel decode must bind an arena first (this is exactly the device
 *          path). Proves the encoded page bytes decode to real pixels of the
 *          expected size.
 * @param[in] png    Encoded image bytes.
 * @param[in] len    Length of @p png.
 * @param[in] exp_w  Expected decoded width.
 * @param[in] exp_h  Expected decoded height.
 * @return Whether the image decoded to non-NULL pixels of exactly @p exp_w x @p exp_h.
 */
static inline bool cf_decode_ok(const uint8_t* png, size_t len, int exp_w, int exp_h)
{
  static uint8_t  s_scratch[k_cf_scratch_kib * k_cf_bytes_per_kib];
  ra8_img_arena_t arena = {.base = s_scratch, .cap = sizeof(s_scratch), .offset = 0U, .live = 0U};
  ra8_img_arena_bind(&arena);
  int        w  = 0;
  int        h  = 0;
  int        c  = 0;
  stbi_uc*   px = stbi_load_from_memory(png, (int)len, &w, &h, &c, 1);
  const bool ok = (px != nullptr) && (w == exp_w) && (h == exp_h);
  if (px != nullptr) {
    stbi_image_free(px);
  }
  ra8_img_arena_unbind();
  return ok;
}
