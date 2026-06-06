/**
 * @file ra_display_pal_host_macos_pixels.h
 * @brief RGB565 -> ARGB8888 conversion for the host macOS backend
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * Portable, dependency-free pixel conversion shared by the two macOS
 * presentation paths (live window + headless PNG export). Keeping it in
 * a plain C TU -- rather than duplicated inside each Objective-C shim --
 * means the bit-twiddling is held to the project's C rules (typed enums,
 * no magic numbers, validated) and is unit-testable on a CI runner.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra_err.h"

/**
 * @enum ra_macos_pixfmt_const_t
 * @brief Descriptors of the ARGB8888 output this module produces.
 *
 * @details
 * Exposed so the CoreGraphics consumers (``CGBitmapContextCreate``) can
 * name their bit-depth / stride arguments instead of hard-coding them.
 *
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_macos_argb_bits_per_component = 8U, /**< Bits per colour channel.        */
  k_macos_argb_bytes_per_pixel    = 4U, /**< Bytes per output ARGB8888 pixel. */
} ra_macos_pixfmt_const_t;

/**
 * @brief Convert an RGB565 framebuffer to a top-down ARGB8888 buffer.
 *
 * @details
 * Reads the little-endian RGB565 source byte-by-byte (no aligned word
 * casts), expands each channel to 8 bits, and writes opaque
 * ``0xFFRRGGBB`` words row-for-row (no vertical flip). The result is
 * laid out for ``CGBitmapContextCreate`` with
 * ``kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little``.
 *
 * @param[out] dst          Destination ARGB8888 buffer
 *                          (``width_px * height_px`` words).
 * @param[in]  rgb565       Source RGB565 pixel buffer.
 * @param[in]  width_px     Image width in pixels (>= 1).
 * @param[in]  height_px    Image height in pixels (>= 1).
 * @param[in]  stride_bytes Source row stride in bytes (>= width_px * 2).
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok               Conversion complete.
 * @retval k_ra_err_null_ptr     ``dst`` or ``rgb565`` was NULL.
 * @retval k_ra_err_invalid_arg  Zero width / height / stride.
 *
 * @pre ``dst`` holds at least ``width_px * height_px`` 32-bit words.
 * @pre ``rgb565`` holds at least ``stride_bytes * height_px`` bytes.
 * @post On success every ``dst`` word is an opaque ARGB8888 pixel.
 * @post ``rgb565`` is read only.
 *
 * @note Thread-safe (operates only on caller buffers).
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_macos_rgb565_to_argb8888(uint32_t*   dst,
                                                   const void* rgb565,
                                                   uint16_t    width_px,
                                                   uint16_t    height_px,
                                                   uint32_t    stride_bytes);

#ifdef __cplusplus
}
#endif
