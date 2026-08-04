/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_rabook_gray4.h
 * @brief Grayscale image transcode stage (4-bpp / 8-bpp) for the on-device EPUB compiler (#149).
 * @ingroup grp_ereader
 *
 * @details
 * Converts a decoded grayscale pixel buffer (one byte per pixel, 0-255) into
 * the panel-native 4-bpp nibble format used by @ref ra8_book_image_t.
 * The pipeline is three independently-testable steps:
 *
 *  1. @ref ra8_rabook_gray4_output_dims -- compute the output size that keeps
 *     the longer edge within the caller's opt-in clamp (none by default).
 *  2. @ref ra8_rabook_gray4_downscale -- bilinear-interpolate from the
 *     (possibly large) source buffer into a caller-owned intermediate buffer.
 *  3. @ref ra8_rabook_gray4_encode -- quantise every pixel to 4 bits and pack
 *     two pixels per byte (high nibble = even pixel, low nibble = odd pixel),
 *     matching the byte layout of `tools/epub_compile/epub_compile.py`.
 *
 * @par Quantisation rule
 * Grayscale value v (0-255) maps to nibble n = (v + 8) / 17, clamped to
 * [0, 15]. This is the round-to-nearest equivalent of the desktop palette
 * quantisation (16 evenly-spaced entries at i * 17, i = 0..15).  Downscaled
 * images are byte-identical between host and device (issue #213): the desktop
 * tool resamples and quantises with this exact integer kernel, mirrored in
 * tools/epub_compile/gray4_kernel.py, so both sides emit the same pixels -- no
 * LANCZOS-vs-bilinear exception.  test_ra8_rabook_downscale_parity.c gates that
 * parity against a generated golden (rabook_downscale_parity_fixture.h).
 *
 * @par Zero allocation
 * No malloc.  All working storage (source pixels, intermediate scaled buffer)
 * is caller-owned.
 *
 * @note Not thread-safe.
 * @see ra8_rabook_compile.h  Builder back-end this feeds into.
 * @see ra8_book_image_t      On-disk descriptor stored in the RABOOK1 blob.
 * @since Version 0.1.0
 *
 * [Ring 4 / EPUB Compiler] {World: NS}
 */
#pragma once

#include <stdint.h>

#include "ra8_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/* Public constants */
/* -------------------------------------------------------------------------- */

/**
 * @enum ra8_rabook_gray4_consts_t
 * @brief Compile-time constants for the gray4 transcode stage.
 * @details @ref k_ra8_rabook_gray4_gray_levels and
 *          @ref k_ra8_rabook_gray4_quant_div define the 16-level even palette
 *          (entry i is at grayscale value i * @ref k_ra8_rabook_gray4_quant_div).
 *          There is deliberately NO baked-in max-edge constant: the compile
 *          preserves source resolution by default (the planned zoom loupe
 *          needs full-resolution manga pages), and any downscale clamp is the
 *          caller's opt-in via the pipeline scratch's `max_image_edge` field,
 *          mirroring the desktop tool's opt-in `--max-edge` knob.
 * @invariant k_ra8_rabook_gray4_quant_div * (k_ra8_rabook_gray4_gray_levels - 1) == 255.
 * @code
 *   // Compute output buffer size for an opt-in clamp chosen by the caller:
 *   uint16_t ow, oh;
 *   ra8_rabook_gray4_output_dims(src_w, src_h, caller_max_edge, &ow, &oh);
 *   uint32_t nibble_bytes = ((uint32_t)ow * oh + 1U) / k_ra8_rabook_gray4_nib_per_byte;
 * @endcode
 * @see ra8_rabook_gray4_output_dims
 * @see ra8_rabook_gray4_encode
 * @since Version 0.1.0
 */
typedef enum : uint16_t {
  k_ra8_rabook_gray4_gray_levels  = 16U, /**< Palette depth: 16 evenly-spaced levels.        */
  k_ra8_rabook_gray4_quant_div    = 17U, /**< Palette step: 255 / 15 rounds to 17.           */
  k_ra8_rabook_gray4_round_half   = 8U,  /**< Added before quant divide to round-to-nearest. */
  k_ra8_rabook_gray4_nib_max      = 15U, /**< Maximum nibble value (4 bits unsigned).        */
  k_ra8_rabook_gray4_nib_per_byte = 2U,  /**< Pixels packed per output byte.                 */
} ra8_rabook_gray4_consts_t;

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
void ra8_rabook_gray4_output_dims(uint16_t  src_w,
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
 *          When src_w or src_h is 0 the output is zeroed and k_ra8_ok returned.
 *
 * @param[in]  src    Source buffer: src_w * src_h bytes, one byte per pixel.
 * @param[in]  src_w  Source width in pixels.
 * @param[in]  src_h  Source height in pixels.
 * @param[out] dst    Destination buffer: must hold dst_w * dst_h writable bytes.
 * @param[in]  dst_w  Destination width (> 0).
 * @param[in]  dst_h  Destination height (> 0).
 *
 * @return Error code.
 * @retval k_ra8_ok              Resampled successfully.
 * @retval k_ra8_err_null_ptr    @p src or @p dst is NULL.
 * @retval k_ra8_err_invalid_arg dst_w or dst_h is 0.
 *
 * @pre @p src holds at least src_w * src_h readable bytes.
 * @pre @p dst holds at least dst_w * dst_h writable bytes.
 * @post Every byte in dst[0..dst_w*dst_h) holds the interpolated value.
 * @post The source buffer is unchanged.
 *
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
ra8_err_t ra8_rabook_gray4_downscale(const uint8_t* src,
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
 * @retval k_ra8_ok           Encoded successfully.
 * @retval k_ra8_err_null_ptr @p gray_pixels, @p out, or @p out_size is NULL.
 * @retval k_ra8_err_no_mem   out_cap < (w*h+1)/2.
 *
 * @pre @p gray_pixels holds at least w * h readable bytes.
 * @pre out_cap >= (w * h + 1) / 2.
 * @post *out_size == (w * h + 1) / 2.
 * @post Every output nibble n satisfies 0 <= n <= 15.
 *
 * @note When w or h is 0, *out_size is set to 0 and k_ra8_ok is returned.
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
ra8_err_t ra8_rabook_gray4_encode(const uint8_t* gray_pixels,
                                  uint16_t       w,
                                  uint16_t       h,
                                  uint8_t*       out,
                                  uint32_t       out_cap,
                                  uint32_t*      out_size);

/**
 * @brief Copy a grayscale buffer out verbatim as 8-bpp (one byte per pixel).
 *
 * @details The 8-bpp counterpart of @ref ra8_rabook_gray4_encode, selected when a
 *          device profile wants the lossless grayscale source instead of the
 *          half-size 4-bpp packing. There is no quantise and no packing: the
 *          @ref k_ra8_book_pixfmt_gray8 pool bytes ARE the decoded (and possibly
 *          downscaled) gray pixels, so `out[i] == gray_pixels[i]` for every pixel.
 *          Keeping it a distinct, validated call -- rather than a bare memcpy at
 *          the call site -- means the transcode stage always states the depth it
 *          produced and the capacity is checked once, here.
 *
 *          Output size is w * h bytes (0 when either dimension is 0).
 *
 * @param[in]  gray_pixels  Grayscale source: w * h bytes, 0-255 each.
 * @param[in]  w            Image width in pixels.
 * @param[in]  h            Image height in pixels.
 * @param[out] out          Output buffer; must hold at least w * h bytes.
 * @param[in]  out_cap      Capacity of @p out in bytes.
 * @param[out] out_size     On success: bytes written (== w * h).
 *
 * @return Error code.
 * @retval k_ra8_ok           Copied successfully.
 * @retval k_ra8_err_null_ptr @p gray_pixels, @p out, or @p out_size is NULL.
 * @retval k_ra8_err_no_mem   out_cap < w * h.
 *
 * @pre @p gray_pixels holds at least w * h readable bytes.
 * @pre out_cap >= w * h, and @p out does not overlap @p gray_pixels.
 * @post *out_size == w * h (0 when w or h is 0).
 * @post out[0..w*h) equals gray_pixels[0..w*h) byte for byte.
 *
 * @note When w or h is 0, *out_size is set to 0 and k_ra8_ok is returned.
 * @note Not thread-safe.
 * @see ra8_rabook_gray4_encode  The 4-bpp (quantise + nibble-pack) counterpart.
 * @since Version 0.1.0
 */
ra8_err_t ra8_rabook_gray8_encode(const uint8_t* gray_pixels,
                                  uint16_t       w,
                                  uint16_t       h,
                                  uint8_t*       out,
                                  uint32_t       out_cap,
                                  uint32_t*      out_size);

#ifdef __cplusplus
} /* extern "C" */
#endif
