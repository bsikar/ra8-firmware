/**
 * @file ra_rabook_gray4.h
 * @brief 4-bpp grayscale image transcode stage for the on-device EPUB compiler (#149).
 *
 * @details
 * Converts a decoded grayscale pixel buffer (one byte per pixel, 0-255) into
 * the panel-native 4-bpp nibble format used by @ref ra_book_image_t.
 * The pipeline is three independently-testable steps:
 *
 *  1. @ref ra_rabook_gray4_output_dims -- compute the output size that keeps
 *     the longer edge within @ref k_ra_rabook_gray4_max_edge.
 *  2. @ref ra_rabook_gray4_downscale -- bilinear-interpolate from the
 *     (possibly large) source buffer into a caller-owned intermediate buffer.
 *  3. @ref ra_rabook_gray4_encode -- quantise every pixel to 4 bits and pack
 *     two pixels per byte (high nibble = even pixel, low nibble = odd pixel),
 *     matching the byte layout of `tools/epub_compile/epub_compile.py`.
 *
 * @par Quantisation rule
 * Grayscale value v (0-255) maps to nibble n = (v + 8) / 17, clamped to
 * [0, 15]. This is the round-to-nearest equivalent of the desktop palette
 * quantisation (16 evenly-spaced entries at i * 17, i = 0..15).  For images
 * that do not require downscaling the output is byte-identical to the desktop
 * tool.  For downscaled images the bilinear kernel differs from PIL LANCZOS so
 * pixel-level byte-identity is not guaranteed; the RABOOK1 format semantics
 * are identical.
 *
 * @par Zero allocation
 * No malloc.  All working storage (source pixels, intermediate scaled buffer)
 * is caller-owned.
 *
 * @note Not thread-safe.
 * @see ra_rabook_compile.h  Builder back-end this feeds into.
 * @see ra_book_image_t      On-disk descriptor stored in the RABOOK1 blob.
 * @since Version 0.1.0
 *
 * [Ring 4 / EPUB Compiler] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdint.h>

#include "ra_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/* Public constants */
/* -------------------------------------------------------------------------- */

/**
 * @enum ra_rabook_gray4_consts_t
 * @brief Compile-time constants for the gray4 transcode stage.
 * @details @ref k_ra_rabook_gray4_max_edge matches MAX_IMAGE_EDGE in
 *          `epub_compile.py`. @ref k_ra_rabook_gray4_gray_levels and
 *          @ref k_ra_rabook_gray4_quant_div define the 16-level even palette
 *          (entry i is at grayscale value i * @ref k_ra_rabook_gray4_quant_div).
 * @invariant k_ra_rabook_gray4_quant_div * (k_ra_rabook_gray4_gray_levels - 1) == 255.
 * @code
 *   // Compute output buffer size:
 *   uint16_t ow, oh;
 *   ra_rabook_gray4_output_dims(src_w, src_h, k_ra_rabook_gray4_max_edge, &ow, &oh);
 *   uint32_t nibble_bytes = ((uint32_t)ow * oh + 1U) / k_ra_rabook_gray4_nib_per_byte;
 * @endcode
 * @see ra_rabook_gray4_output_dims
 * @see ra_rabook_gray4_encode
 * @since Version 0.1.0
 */
typedef enum : uint16_t {
  k_ra_rabook_gray4_max_edge     = 1600U, /**< Max length of the longer edge after downscale. */
  k_ra_rabook_gray4_gray_levels  = 16U,   /**< Palette depth: 16 evenly-spaced levels.        */
  k_ra_rabook_gray4_quant_div    = 17U,   /**< Palette step: 255 / 15 rounds to 17.           */
  k_ra_rabook_gray4_round_half   = 8U,    /**< Added before quant divide to round-to-nearest. */
  k_ra_rabook_gray4_nib_max      = 15U,   /**< Maximum nibble value (4 bits unsigned).        */
  k_ra_rabook_gray4_nib_per_byte = 2U,    /**< Pixels packed per output byte.                 */
} ra_rabook_gray4_consts_t;

/* -------------------------------------------------------------------------- */
/* API */
/* -------------------------------------------------------------------------- */

/**
 * @brief Compute scaled output dimensions keeping the longer edge within @p max_edge.
 *
 * @details If max(src_w, src_h) <= max_edge the source dimensions are returned
 *          unchanged.  Otherwise both dimensions are scaled by
 *          max_edge / longer_edge (rounded to nearest, minimum 1 each) so the
 *          image fits the panel class without distorting the aspect ratio.  The
 *          result is the exact size the caller must allocate for the
 *          intermediate downscale buffer.
 *
 * @param[in]  src_w    Source width in pixels.
 * @param[in]  src_h    Source height in pixels.
 * @param[in]  max_edge Maximum allowed length of the longer edge.
 * @param[out] out_w    Scaled output width (>= 1 when src_w, src_h and max_edge
 *                      are all > 0; 0 otherwise, per the @note below).
 * @param[out] out_h    Scaled output height (>= 1 when src_w, src_h and max_edge
 *                      are all > 0; 0 otherwise, per the @note below).
 *
 * @pre @p out_w is non-NULL.
 * @pre @p out_h is non-NULL.
 * @post If max(src_w, src_h) <= max_edge then *out_w == src_w and *out_h == src_h.
 * @post If scaling is required then max(*out_w, *out_h) <= max_edge.
 *
 * @note Not thread-safe.
 * @note When src_w, src_h, or max_edge is 0 both outputs are set to 0.
 * @since Version 0.1.0
 */
void ra_rabook_gray4_output_dims(uint16_t  src_w,
                                 uint16_t  src_h,
                                 uint16_t  max_edge,
                                 uint16_t* out_w,
                                 uint16_t* out_h);

/**
 * @brief Bilinear-interpolate a grayscale image from (src_w x src_h) to (dst_w x dst_h).
 *
 * @details Each output pixel is the bilinear interpolation of the four nearest
 *          source pixels at the corresponding sample point, computed in Q16.16
 *          fixed-point arithmetic (no floating-point, no malloc).  The mapping
 *          places the sample for output pixel dx at source x = dx * src_w /
 *          dst_w (and similarly for y); this is a left-aligned sample grid that
 *          is deterministic for any src/dst size pair.
 *
 *          When dst_w == src_w and dst_h == src_h the output is an exact copy.
 *          When src_w or src_h is 0 the output is zeroed and k_ra_ok returned.
 *
 * @param[in]  src    Source buffer: src_w * src_h bytes, one byte per pixel.
 * @param[in]  src_w  Source width in pixels.
 * @param[in]  src_h  Source height in pixels.
 * @param[out] dst    Destination buffer: must hold dst_w * dst_h writable bytes.
 * @param[in]  dst_w  Destination width (> 0).
 * @param[in]  dst_h  Destination height (> 0).
 *
 * @return Error code.
 * @retval k_ra_ok              Resampled successfully.
 * @retval k_ra_err_null_ptr    @p src or @p dst is NULL.
 * @retval k_ra_err_invalid_arg dst_w or dst_h is 0.
 *
 * @pre @p src holds at least src_w * src_h readable bytes.
 * @pre @p dst holds at least dst_w * dst_h writable bytes.
 * @post Every byte in dst[0..dst_w*dst_h) holds the interpolated value.
 * @post The source buffer is unchanged.
 *
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
ra_err_t ra_rabook_gray4_downscale(const uint8_t* src,
                                   uint16_t       src_w,
                                   uint16_t       src_h,
                                   uint8_t*       dst,
                                   uint16_t       dst_w,
                                   uint16_t       dst_h);

/**
 * @brief Quantise a grayscale buffer to 16 levels and pack as 4-bpp nibbles.
 *
 * @details For each source pixel v the nibble is n = (v + 8) / 17, clamped
 *          to [0, 15] (round-to-nearest quantisation to the 16-level palette
 *          at {0, 17, 34, ..., 255}).  Pairs of nibbles are packed one per
 *          byte: byte[i] = (nib[2i] << 4) | nib[2i+1].  For an odd pixel
 *          count the last byte holds the final nibble in its high half and
 *          its low half is zero.
 *
 *          Output size is ceil(w * h / 2) == (w * h + 1) / 2 bytes.
 *
 *          The packing order is byte-identical to `epub_compile.py` for the
 *          same input pixel values.
 *
 * @param[in]  gray_pixels  Grayscale source: w * h bytes, 0-255 each.
 * @param[in]  w            Image width in pixels.
 * @param[in]  h            Image height in pixels.
 * @param[out] out          Output nibble buffer; must hold at least (w*h+1)/2 bytes.
 * @param[in]  out_cap      Capacity of @p out in bytes.
 * @param[out] out_size     On success: bytes written (== (w*h+1)/2).
 *
 * @return Error code.
 * @retval k_ra_ok           Encoded successfully.
 * @retval k_ra_err_null_ptr @p gray_pixels, @p out, or @p out_size is NULL.
 * @retval k_ra_err_no_mem   out_cap < (w*h+1)/2.
 *
 * @pre @p gray_pixels holds at least w * h readable bytes.
 * @pre out_cap >= (w * h + 1) / 2.
 * @post *out_size == (w * h + 1) / 2.
 * @post Every output nibble n satisfies 0 <= n <= 15.
 *
 * @note When w or h is 0, *out_size is set to 0 and k_ra_ok is returned.
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
ra_err_t ra_rabook_gray4_encode(const uint8_t* gray_pixels,
                                uint16_t       w,
                                uint16_t       h,
                                uint8_t*       out,
                                uint32_t       out_cap,
                                uint32_t*      out_size);

#ifdef __cplusplus
} /* extern "C" */
#endif
