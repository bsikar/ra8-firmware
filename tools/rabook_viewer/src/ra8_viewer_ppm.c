/**
 * @file ra8_viewer_ppm.c
 * @brief RGB565 <-> 8-bit RGB conversion and the shared binary-PPM (P6) writer.
 *
 * @details
 * Both headless proofs -- the fixed-framebuffer dump (::ra8_viewer_dump_ppm) and
 * the scroll-tile dump in the CLI -- expand the same RGB565 pixels to 8-bit RGB
 * and write the same P6 file. That conversion lives here once so the two paths
 * cannot drift, and so the field shifts / masks are named constants in one place
 * rather than literals repeated at each call site.
 *
 * The 5->8 and 6->8 bit expansions replicate the high bits into the low bits
 * (`(v << 3) | (v >> 2)`), which maps 0x1F to 0xFF exactly -- a plain shift would
 * cap white at 0xF8 and tint every dump.
 *
 * @since 0.1.0
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <stdio.h>

#include "ra8_err.h"
#include "ra8_viewer_reader.h"
#include "ra8_viewer_reader_internal.h"

/**
 * @enum ra8_viewer_rgb565_shift_t
 * @brief Field positions and channel-width conversions for RGB565 packing.
 * @details The `_drop` values narrow 8-bit channels on the way in; the `_fill`
 *          values position the replicated high bits on the way back out.
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_rgb565_r_shift  = 11U, /**< Red field position in the 16-bit word.       */
  k_rgb565_g_shift  = 5U,  /**< Green field position in the 16-bit word.     */
  k_rgb565_r5_drop  = 3U,  /**< 8->5-bit reduction for red/blue.             */
  k_rgb565_g6_drop  = 2U,  /**< 8->6-bit reduction for green.                */
  k_rgb565_r5_fill  = 2U,  /**< 5->8 low-bit replication shift (red/blue).   */
  k_rgb565_g6_fill  = 4U,  /**< 6->8 low-bit replication shift (green).      */
  k_rgb565_hi_shift = 8U,  /**< High-byte position of a little-endian pixel. */
} ra8_viewer_rgb565_shift_t;

/**
 * @enum ra8_viewer_rgb565_mask_t
 * @brief Channel masks used to unpack an RGB565 word.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_rgb565_r5_mask = 0x1FU, /**< 5-bit red field mask.   */
  k_rgb565_g6_mask = 0x3FU, /**< 6-bit green field mask. */
  k_rgb565_b5_mask = 0x1FU, /**< 5-bit blue field mask.  */
} ra8_viewer_rgb565_mask_t;

/**
 * @enum ra8_viewer_ppm_t
 * @brief Layout constants of the binary portable-pixmap (P6) output.
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_ppm_channels = 3U, /**< Bytes per P6 pixel (R, G, B).       */
  k_ppm_idx_r    = 0U, /**< Red byte index within a P6 pixel.   */
  k_ppm_idx_g    = 1U, /**< Green byte index within a P6 pixel. */
  k_ppm_idx_b    = 2U, /**< Blue byte index within a P6 pixel.  */
} ra8_viewer_ppm_t;

uint16_t ra8_viewer_pack565(uint8_t rr, uint8_t gg, uint8_t bb)
{
  const uint16_t r5 = (uint16_t)(rr >> (uint16_t)k_rgb565_r5_drop);
  const uint16_t g6 = (uint16_t)(gg >> (uint16_t)k_rgb565_g6_drop);
  const uint16_t b5 = (uint16_t)(bb >> (uint16_t)k_rgb565_r5_drop);
  return (uint16_t)((r5 << (uint16_t)k_rgb565_r_shift) | (g6 << (uint16_t)k_rgb565_g_shift) | b5);
}

uint16_t ra8_viewer_pack565_le_pair(uint8_t lo, uint8_t hi)
{
  return (uint16_t)((uint16_t)lo | ((uint16_t)hi << (uint16_t)k_rgb565_hi_shift));
}

/**
 * @brief Expand one RGB565 word into three 8-bit channels.
 * @details High-bit replication, so a saturated field maps to 0xFF exactly.
 * @param[in]  px  Packed RGB565 pixel.
 * @param[out] rgb Receives {R, G, B} (non-NULL, ::k_ppm_channels bytes).
 * @pre @p rgb has room for ::k_ppm_channels bytes.
 * @pre @p px is a native-endian RGB565 word.
 * @post @p rgb holds the three expanded 8-bit channels.
 * @post No state other than @p rgb is mutated.
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
static void viewer_unpack565(uint16_t px, uint8_t rgb[k_ppm_channels])
{
  const uint32_t r5 = ((uint32_t)px >> (uint32_t)k_rgb565_r_shift) & (uint32_t)k_rgb565_r5_mask;
  const uint32_t g6 = ((uint32_t)px >> (uint32_t)k_rgb565_g_shift) & (uint32_t)k_rgb565_g6_mask;
  const uint32_t b5 = (uint32_t)px & (uint32_t)k_rgb565_b5_mask;

  rgb[k_ppm_idx_r] =
    (uint8_t)((r5 << (uint32_t)k_rgb565_r5_drop) | (r5 >> (uint32_t)k_rgb565_r5_fill));
  rgb[k_ppm_idx_g] =
    (uint8_t)((g6 << (uint32_t)k_rgb565_g6_drop) | (g6 >> (uint32_t)k_rgb565_g6_fill));
  rgb[k_ppm_idx_b] =
    (uint8_t)((b5 << (uint32_t)k_rgb565_r5_drop) | (b5 >> (uint32_t)k_rgb565_r5_fill));
}

ra8_err_t ra8_viewer_write_ppm565(const uint16_t* px, uint32_t w, uint32_t h, const char* path)
{
  if ((px == nullptr) || (path == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  if ((w == 0U) || (h == 0U)) {
    return k_ra8_err_invalid_size;
  }
  FILE* fp = fopen(path, "wb");
  if (fp == NULL) {
    return k_ra8_err_not_found;
  }
  (void)fprintf(fp, "P6\n%u %u\n255\n", (unsigned)w, (unsigned)h);
  const size_t pixels = (size_t)w * (size_t)h;
  for (size_t i = 0U; i < pixels; ++i) {
    uint8_t rgb[k_ppm_channels];
    viewer_unpack565(px[i], rgb);
    (void)fwrite(rgb, 1U, sizeof(rgb), fp);
  }
  const int closed = fclose(fp);
  return (closed == 0) ? k_ra8_ok : k_ra8_err_not_found;
}

ra8_err_t ra8_viewer_dump_ppm(const ra8_viewer_reader_t* r, const char* path)
{
  if ((r == nullptr) || (path == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  return ra8_viewer_write_ppm565(r->fb,
                                 (uint32_t)k_ra8_viewer_fb_width,
                                 (uint32_t)k_ra8_viewer_fb_height,
                                 path);
}
