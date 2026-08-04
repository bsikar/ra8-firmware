/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_gfx_dither.c
 * @brief Void-and-cluster blue-noise dither: gray8 -> 16-level e-ink panel (#477).
 *
 * @details
 * Implements the three ra8_gfx_dither.h entry points over one shared pair of
 * primitives: ::internal_mask_index maps absolute panel coordinates onto the
 * committed blue-noise mask (::s_ra8_gfx_dither_mask, baked by
 * scripts/gen/gen_bluenoise_mask.py), and ::internal_quantise turns a gray8
 * sample plus its threshold into a 4-bit level. Because the threshold depends on
 * no neighbour state, a large image split into tiles dithers byte-for-byte the
 * same as the whole image, and the transform is pure integer arithmetic over a
 * `const` table -- host, ra8_emulator, and silicon emit identical bytes (the
 * EIL==HIL rule). The single-pixel (::ra8_gfx_dither_gray4_level), bulk-pack
 * (::internal_pack_tile) and render-blit (::ra8_gfx_blit_gray8_dither) paths each
 * compose those primitives directly, so the hot loops carry no per-pixel call
 * through the public API. Scalar-first; a Helium/MVE lane-wise pass can replace
 * the inner loops later without moving the mask or the quantise rule.
 *
 *
 * @since 0.1.0
 */

#include "ra8_gfx_dither.h"

#include <stddef.h>
#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_gfx_dither_mask_internal.h"
#include "ra8_gfx_internal.h"
#include "ra8_log.h"

/* The runtime index arithmetic assumes the generated mask is exactly one
 * mask_dim x mask_dim texture; assert it here so a regenerate that changed the
 * geometry cannot silently mis-index. */
static_assert(sizeof(s_ra8_gfx_dither_mask) == (size_t)k_ra8_gfx_dither_mask_len,
              "blue-noise mask must be mask_dim * mask_dim bytes");
static_assert((int)k_ra8_gfx_dither_mask_index_mask == (int)k_ra8_gfx_dither_mask_dim - 1,
              "toroidal index bitmask must be mask_dim - 1");

/**
 * @brief Toroidal blue-noise mask index for absolute panel coordinate (@p x, @p y).
 *
 * @details Reduces the coordinates onto the mask edge with a bitmask (the edge is
 *          a power of two): `(y & (dim - 1)) * dim + (x & (dim - 1))`. AND with
 *          `dim - 1` yields the mathematically-correct non-negative modulo for
 *          negative coordinates too, so a lens window drawn at a negative offset
 *          still lands on the same continuous mask phase (seamless tiling).
 *
 * @param[in] x Absolute column (any int32).
 * @param[in] y Absolute row (any int32).
 * @return Flat mask index in [0, @ref k_ra8_gfx_dither_mask_len).
 * @retval 0 Both coordinates land on the mask origin column and row.
 * @pre  @ref k_ra8_gfx_dither_mask_dim is a power of two (asserted above).
 * @pre  The mask index bitmask equals `dim - 1` (asserted above).
 * @post The result is a valid subscript into ::s_ra8_gfx_dither_mask.
 * @post No memory is modified (pure function).
 * @note Thread-safe; reads only its arguments and compile-time constants.
 * @since 0.1.0
 */
RA8_INTERNAL
static uint32_t internal_mask_index(int32_t x, int32_t y)
{
  const uint32_t mx = (uint32_t)x & (uint32_t)k_ra8_gfx_dither_mask_index_mask;
  const uint32_t my = (uint32_t)y & (uint32_t)k_ra8_gfx_dither_mask_index_mask;
  return (my * (uint32_t)k_ra8_gfx_dither_mask_dim) + mx;
}

/**
 * @brief Quantise a gray8 sample to a 4-bit level given its blue-noise threshold.
 *
 * @details The base level is `gray8 / step` and the fractional distance to the
 *          next level is `(gray8 % step) / step`; the pixel rounds up when the
 *          threshold falls below that fraction. Written as the exact integer test
 *          `thr * step < rem * byte_levels`, the round-up probability is exactly
 *          `rem / step` over a uniform mask -- unbiased, so flat regions
 *          reproduce their tone with no banding. No clamp is needed and none is
 *          added (it would be unreachable dead code): the base equals the maximum
 *          level only when gray8 == 255, which forces rem == 0 and hence no
 *          round-up, so the result is in [0, max_level] by construction.
 *
 * @param[in] gray8 Source luminance sample, 0 (black) .. 255 (white).
 * @param[in] thr   Blue-noise threshold for the pixel, 0 .. 255.
 * @return The dithered 4-bit level, 0 .. @ref k_ra8_gfx_dither_max_level.
 * @retval 0  The pixel quantised to black.
 * @retval 15 The pixel quantised to white.
 * @pre  @p thr is a mask byte in [0, 255].
 * @pre  The palette step and byte-level scale are valid compile-time constants.
 * @post The result is in [0, @ref k_ra8_gfx_dither_max_level].
 * @post No memory is modified (pure function).
 * @note Thread-safe; reads only its arguments and compile-time constants.
 * @since 0.1.0
 */
RA8_INTERNAL
static uint8_t internal_quantise(uint8_t gray8, uint8_t thr)
{
  const uint8_t base  = (uint8_t)(gray8 / (uint8_t)k_ra8_gfx_dither_step);
  const uint8_t rem   = (uint8_t)(gray8 - (uint8_t)(base * (uint8_t)k_ra8_gfx_dither_step));
  uint8_t       level = base;
  if (((uint32_t)thr * (uint32_t)k_ra8_gfx_dither_step) <
      ((uint32_t)rem * (uint32_t)k_ra8_gfx_dither_byte_levels)) {
    level = (uint8_t)(level + 1U);
  }
  return level;
}

/**
 * @brief Expand a 4-bit panel level to a 0x00RRGGBB gray colour.
 *
 * @details Replicates the nibble into an 8-bit gray (`(n << 4) | n == n * 17`)
 *          and broadcasts it across R, G and B -- byte-identical to the gray
 *          level ::ra8_gfx_blit_gray8 and ::ra8_gfx_blit_gray4_zoom produce for
 *          the same level, so the dithered blit down-converts the same way. The
 *          nibble is widened to `uint32_t` before shifting so no cast is applied
 *          to a composite expression.
 *
 * @param[in] level 4-bit panel level, 0 .. @ref k_ra8_gfx_dither_max_level.
 * @return 0x00RRGGBB gray colour with R == G == B.
 * @retval 0x00000000 @p level is 0 (black).
 * @retval 0x00FFFFFF @p level is 15 (white).
 * @pre  @p level <= @ref k_ra8_gfx_dither_max_level (caller guarantees).
 * @pre  The shift constants are valid compile-time literals.
 * @post The returned colour has equal R, G and B channels.
 * @post No memory is modified (pure function).
 * @note Thread-safe; reads only its argument and compile-time constants.
 * @since 0.1.0
 */
RA8_INTERNAL
static uint32_t internal_level_to_color(uint8_t level)
{
  const uint32_t g    = (uint32_t)level;
  const uint32_t gray = (g << (uint32_t)k_ra8_gfx_dither_nib_shift) | g;
  return (gray << (uint32_t)k_ra8_gfx_dither_rgb_r_shift) |
         (gray << (uint32_t)k_ra8_gfx_dither_rgb_g_shift) | gray;
}

/**
 * @brief Dither a gray8 tile to packed 4-bpp nibbles, mask-phased at (@p ox, @p oy).
 *
 * @details The bulk packer behind ::ra8_gfx_dither_gray8_to_gray4: for every
 *          pixel it thresholds the sample against the blue-noise mask at absolute
 *          coordinates (@p ox + col, @p oy + row) and packs the level two per
 *          byte (high nibble even, low nibble odd). Even indices assign the byte
 *          (clearing the low nibble) and odd indices OR into it, so no pre-zeroing
 *          is required even for an odd pixel count.
 *
 * @param[in]  src Row-major gray8 tile of @p w * @p h bytes.
 * @param[in]  w   Tile width in pixels (> 0; caller-checked).
 * @param[in]  h   Tile height in pixels (> 0; caller-checked).
 * @param[in]  ox  Absolute panel column of the tile's left edge (mask phase).
 * @param[in]  oy  Absolute panel row of the tile's top edge (mask phase).
 * @param[out] out Packed-gray4 output; >= `(w * h + 1) / 2` writable bytes.
 * @pre  @p src and @p out are non-NULL and do not overlap (caller-checked).
 * @pre  @p w > 0 and @p h > 0 (caller-checked).
 * @post Every packed nibble is in [0, @ref k_ra8_gfx_dither_max_level].
 * @post @p out[0 .. (w*h+1)/2) holds the dithered, packed tile.
 * @note Not thread-safe only in that it writes @p out; holds no shared state.
 * @since 0.1.0
 */
RA8_INTERNAL
static void
internal_pack_tile(const uint8_t* src, int32_t w, int32_t h, int32_t ox, int32_t oy, uint8_t* out)
{
  for (int32_t row = 0; row < h; ++row) {
    for (int32_t col = 0; col < w; ++col) {
      const uint32_t i = ((uint32_t)row * (uint32_t)w) + (uint32_t)col;
      const uint8_t  level =
        internal_quantise(src[i], s_ra8_gfx_dither_mask[internal_mask_index(ox + col, oy + row)]);
      const uint32_t byte_idx = i / (uint32_t)k_ra8_gfx_dither_ppb;
      if ((i & 1U) == 0U) {
        out[byte_idx] = (uint8_t)(level << (uint8_t)k_ra8_gfx_dither_nib_shift);
      } else {
        out[byte_idx] = (uint8_t)(out[byte_idx] | level);
      }
    }
  }
}

uint8_t ra8_gfx_dither_gray4_level(uint8_t gray8, int32_t x, int32_t y)
{
  return internal_quantise(gray8, s_ra8_gfx_dither_mask[internal_mask_index(x, y)]);
}

ra8_err_t ra8_gfx_dither_gray8_to_gray4(const uint8_t* src,
                                        int32_t        w,
                                        int32_t        h,
                                        int32_t        origin_x,
                                        int32_t        origin_y,
                                        uint8_t*       out,
                                        uint32_t       out_cap,
                                        uint32_t*      out_size)
{
  static const char* const s_tag = "ra8_gfx_dither";
  RA8_CHECK_NULL_PTR(src, s_tag, "src");
  RA8_CHECK_NULL_PTR(out, s_tag, "out");
  RA8_CHECK_NULL_PTR(out_size, s_tag, "out_size");

  if ((w <= 0) || (h <= 0)) {
    ra8_log_error(s_tag, "w or h is non-positive");
    return k_ra8_err_invalid_arg;
  }

  const uint32_t n_pixels = (uint32_t)w * (uint32_t)h;
  const uint32_t n_bytes  = (n_pixels + 1U) / (uint32_t)k_ra8_gfx_dither_ppb;
  if (out_cap < n_bytes) {
    ra8_log_error(s_tag, "output buffer too small");
    return k_ra8_err_no_mem;
  }

  internal_pack_tile(src, w, h, origin_x, origin_y, out);
  *out_size = n_bytes;
  return k_ra8_ok;
}

ra8_err_t
ra8_gfx_blit_gray8_dither(const uint8_t* src, int32_t w, int32_t h, int32_t dst_x, int32_t dst_y)
{
  if (!s_gfx_text_state.initialized) {
    return k_ra8_err_not_initialized;
  }
  if ((src == nullptr) || (w <= 0) || (h <= 0)) {
    return k_ra8_err_invalid_arg;
  }

  for (int32_t row = 0; row < h; ++row) {
    for (int32_t col = 0; col < w; ++col) {
      const uint8_t level =
        internal_quantise(src[((size_t)row * (size_t)w) + (size_t)col],
                          s_ra8_gfx_dither_mask[internal_mask_index(dst_x + col, dst_y + row)]);
      s_gfx_text_plot(dst_x + col, dst_y + row, internal_level_to_color(level));
    }
  }
  return k_ra8_ok;
}
