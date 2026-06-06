/**
 * @file ra_display_pal_host_macos_pixels.c
 * @brief RGB565 -> ARGB8888 conversion for the host macOS backend
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * Portable C implementation shared by the live-window and headless-PNG
 * Objective-C shims. The little-endian RGB565 source is read one byte at
 * a time (no aligned 16-bit word cast, so this stays clean under
 * ``-Wcast-align``), each channel is expanded to 8 bits, and opaque
 * ``0xFFRRGGBB`` words are written top-down.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_display_pal_host_macos_pixels.h"

#include <stddef.h>
#include <stdint.h>

#include "ra_check.h"
#include "ra_err.h"

/** @brief Module log tag. */
static const char* const s_tag = "ra_display_pal_host_macos_pixels";

/**
 * @enum ra_rgb565_layout_t
 * @brief Byte layout of the little-endian RGB565 source.
 *
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_rgb565_bpp = 2U, /**< Bytes per source pixel.          */
  k_byte_lo    = 0U, /**< Little-endian low-byte offset.   */
  k_byte_hi    = 1U, /**< Little-endian high-byte offset.  */
  k_byte_bits  = 8U, /**< Bits per byte.                   */
} ra_rgb565_layout_t;

/**
 * @enum ra_rgb565_bits_t
 * @brief Channel shift positions and 5/6-bit -> 8-bit expansion amounts.
 *
 * @details
 * A 5-bit channel is widened with ``(v << 3) | (v >> 2)`` and a 6-bit
 * channel with ``(v << 2) | (v >> 4)`` so full-scale inputs map to 0xFF.
 *
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_rgb565_red_shift   = 11U, /**< Red occupies bits 11..15.   */
  k_rgb565_green_shift = 5U,  /**< Green occupies bits 5..10.   */
  k_rgb565_blue_shift  = 0U,  /**< Blue occupies bits 0..4.     */
  k_expand5_shl        = 3U,  /**< 5-bit widen: left shift.     */
  k_expand5_shr        = 2U,  /**< 5-bit widen: right fill.     */
  k_expand6_shl        = 2U,  /**< 6-bit widen: left shift.     */
  k_expand6_shr        = 4U,  /**< 6-bit widen: right fill.     */
  k_argb_blue_shift    = 0U,  /**< ARGB blue byte position.     */
  k_argb_green_shift   = 8U,  /**< ARGB green byte position.    */
  k_argb_red_shift     = 16U, /**< ARGB red byte position.      */
} ra_rgb565_bits_t;

/**
 * @enum ra_rgb565_mask_t
 * @brief Per-channel masks and the opaque-alpha fill.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_rgb565_red_mask   = 0x1FU,       /**< 5-bit red mask.          */
  k_rgb565_green_mask = 0x3FU,       /**< 6-bit green mask.        */
  k_rgb565_blue_mask  = 0x1FU,       /**< 5-bit blue mask.         */
  k_argb_opaque       = 0xFF000000U, /**< Alpha = 0xFF in MSB.     */
} ra_rgb565_mask_t;

ra_err_t ra_macos_rgb565_to_argb8888(uint32_t*   dst,
                                     const void* rgb565,
                                     uint16_t    width_px,
                                     uint16_t    height_px,
                                     uint32_t    stride_bytes)
{
  RA_CHECK_NULL_PTR(dst, s_tag, "dst");
  RA_CHECK_NULL_PTR(rgb565, s_tag, "rgb565");
  if (width_px == 0U || height_px == 0U || stride_bytes == 0U) {
    return k_ra_err_invalid_arg;
  }

  const uint8_t* base = (const uint8_t*)rgb565;
  for (uint16_t y = 0U; y < height_px; ++y) {
    const uint8_t* srow = base + ((size_t)y * (size_t)stride_bytes);
    uint32_t*      drow = dst + ((size_t)y * (size_t)width_px);
    for (uint16_t x = 0U; x < width_px; ++x) {
      const size_t   off = (size_t)x * (size_t)k_rgb565_bpp;
      const uint32_t lo  = srow[off + (size_t)k_byte_lo];
      const uint32_t hi  = srow[off + (size_t)k_byte_hi];
      const uint32_t p   = lo | (hi << (uint32_t)k_byte_bits);

      const uint32_t r5 = (p >> (uint32_t)k_rgb565_red_shift) & (uint32_t)k_rgb565_red_mask;
      const uint32_t g6 = (p >> (uint32_t)k_rgb565_green_shift) & (uint32_t)k_rgb565_green_mask;
      const uint32_t b5 = (p >> (uint32_t)k_rgb565_blue_shift) & (uint32_t)k_rgb565_blue_mask;

      const uint32_t r = (r5 << (uint32_t)k_expand5_shl) | (r5 >> (uint32_t)k_expand5_shr);
      const uint32_t g = (g6 << (uint32_t)k_expand6_shl) | (g6 >> (uint32_t)k_expand6_shr);
      const uint32_t b = (b5 << (uint32_t)k_expand5_shl) | (b5 >> (uint32_t)k_expand5_shr);

      drow[x] = (uint32_t)k_argb_opaque | (r << (uint32_t)k_argb_red_shift) |
                (g << (uint32_t)k_argb_green_shift) | (b << (uint32_t)k_argb_blue_shift);
    }
  }
  return k_ra_ok;
}
