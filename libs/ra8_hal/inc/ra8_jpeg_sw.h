/**
 * @file ra8_jpeg_sw.h
 * @brief Pure-software baseline JPEG (ISO/IEC 10918-1 / ITU-T T.81) codec
 * @ingroup grp_hal_camera
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * The RA8D2 has no hardware JPEG block. This module implements a
 * software baseline JPEG codec (8-bit precision, sequential DCT,
 * Huffman entropy coding) for two pixel layouts:
 *
 *   - Grayscale (1 component, Y only).
 *   - YCbCr 4:2:0 (3 components, the dominant layout in real-world
 *     JPEG-from-camera files).
 *
 * Decoder input is accepted with both 4:2:0 and 4:4:4 chroma
 * subsampling so that round-trip tests against known files still
 * succeed; encoder output is fixed at 4:2:0 (or grayscale-only when
 * `quality` is negative -- see `ra8_jpeg_sw_encode()`).
 *
 * Out of scope:
 *   - Progressive, hierarchical, lossless and arithmetic-coded modes.
 *   - 12-bit precision.
 *   - JFIF / Exif metadata round-tripping (the encoder emits a minimal
 *     APP0 JFIF header so common viewers accept the output).
 *   - Restart markers (`RST0..RST7`) -- decoder tolerates them, encoder
 *     does not emit them.
 *
 * Spec citations are formatted as `T.81 sec X.Y "..."` and refer to
 * ITU-T Recommendation T.81 (1992) | ISO/IEC 10918-1.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_err.h"

/**
 * @enum ra8_jpeg_sw_pixfmt_t
 * @brief Output pixel layout produced by `ra8_jpeg_sw_decode()`.
 *
 * @details
 * The decoder always emits packed RGB888 (3 bytes per pixel,
 * R-G-B order, no row padding) regardless of the source colour
 * space. This enum exists for symmetry with the encoder and to
 * make future expansion (RGB565, YUV planar) explicit.
 */
typedef enum : uint8_t {
  k_ra8_jpeg_sw_pixfmt_rgb888 = 0U, /**< 24-bit packed R,G,B per pixel. */
} ra8_jpeg_sw_pixfmt_t;

/**
 * @enum ra8_jpeg_sw_quality_t
 * @brief Convenience quality presets for `ra8_jpeg_sw_encode()`.
 *
 * @details
 * The `quality` argument to `ra8_jpeg_sw_encode()` is a plain
 * integer in [1, 100]. These named constants document the
 * recommended operating points; callers may pass any value in
 * range.
 */
typedef enum : uint8_t {
  k_ra8_jpeg_sw_quality_min     = 1U,   /**< Lowest legal quality.        */
  k_ra8_jpeg_sw_quality_low     = 50U,  /**< IJG default lower bound.     */
  k_ra8_jpeg_sw_quality_default = 75U,  /**< IJG `cjpeg` default.         */
  k_ra8_jpeg_sw_quality_high    = 90U,  /**< Visually lossless on photos. */
  k_ra8_jpeg_sw_quality_max     = 100U, /**< Largest legal quality.       */
} ra8_jpeg_sw_quality_t;

/**
 * @brief Parse the SOF0 marker of a JPEG stream and report its
 *        image dimensions without performing entropy decoding.
 *
 * @details
 * Walks the marker chain looking for `T.81 sec B.2.2 "Frame header
 * syntax"` (`SOF0` = `0xFFC0`). On match the 16-bit big-endian Y
 * (height) and X (width) fields are returned. This is the cheap
 * way to size an output buffer before calling
 * `ra8_jpeg_sw_decode()`.
 *
 * Algorithm:
 *   1. Verify the stream begins with the SOI marker (`0xFFD8`).
 *   2. Walk segment headers, skipping their payloads.
 *   3. Stop at SOF0; reject SOF1..SOF15 (non-baseline).
 *   4. Read 8-bit precision, 16-bit Y, 16-bit X.
 *
 * @param[in]  jpeg_buf Pointer to the JPEG byte stream (not NULL).
 * @param[in]  jpeg_len Length of `jpeg_buf` in bytes (must be >= 4).
 * @param[out] out_w    Receives image width in pixels (not NULL).
 * @param[out] out_h    Receives image height in pixels (not NULL).
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok                Dimensions written to `*out_w`, `*out_h`.
 * @retval k_ra8_err_null_ptr      Any pointer argument was NULL.
 * @retval k_ra8_err_invalid_size  `jpeg_len` smaller than the SOI
 *                                marker (2 bytes).
 * @retval k_ra8_err_protocol_error Stream lacks SOI, lacks SOF0, or a
 *                                 marker length field overflows the
 *                                 buffer.
 * @retval k_ra8_err_not_supported SOF marker is non-baseline (e.g.
 *                                progressive `0xFFC2`, lossless
 *                                `0xFFC3`, arithmetic `0xFFC9`+).
 *
 * @pre `jpeg_buf` references at least `jpeg_len` valid bytes.
 * @pre The caller will not modify `jpeg_buf` for the duration of
 *      this call.
 * @post On `k_ra8_ok` both `*out_w` and `*out_h` are non-zero.
 * @post On any error neither output is modified.
 *
 * @note Thread-safe: the function reads only its arguments and has
 *       no internal state.
 *
 * @par Example:
 * @code
 * uint16_t w, h;
 * if (ra8_jpeg_sw_get_dimensions(buf, len, &w, &h) == k_ra8_ok) {
 *   printf("%u x %u\n", w, h);
 * }
 * @endcode
 *
 * @see ra8_jpeg_sw_decode  Full decode that also returns dimensions.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_jpeg_sw_get_dimensions(const uint8_t* jpeg_buf,
                                                   uint32_t       jpeg_len,
                                                   uint16_t*      out_w,
                                                   uint16_t*      out_h);

/**
 * @brief Decode a baseline JPEG stream into packed RGB888.
 *
 * @details
 * Performs full entropy decoding, dequantization, inverse DCT and
 * YCbCr-to-RGB colour conversion (ITU-T T.871 JFIF colour
 * conversion). The output buffer must be at least
 * `width * height * 3` bytes; the actual size is reported via
 * `*out_w` and `*out_h`.
 *
 * Supported source formats:
 *   - Grayscale: 1 component, no subsampling.
 *   - YCbCr: 3 components, 4:4:4 (`H1V1,H1V1,H1V1`),
 *     4:2:0 (`H2V2,H1V1,H1V1`).
 *
 * Algorithm (per MCU):
 *   1. Decode Huffman-coded DC/AC coefficients per block.
 *   2. Dequantize against the per-component quantization table.
 *   3. Apply the IDCT (T.81 Annex A.3.3 8x8 inverse DCT).
 *   4. Upsample chroma to luma resolution where required.
 *   5. YCbCr -> RGB and store row-major into `out_buf`.
 *
 * @param[in]  jpeg_buf     Pointer to the JPEG byte stream.
 * @param[in]  jpeg_len     Length of `jpeg_buf` in bytes.
 * @param[out] out_buf      Destination buffer for RGB888 pixels.
 * @param[in]  out_buf_len  Capacity of `out_buf` in bytes.
 * @param[out] out_w        Receives image width in pixels.
 * @param[out] out_h        Receives image height in pixels.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok                  Decoded successfully.
 * @retval k_ra8_err_null_ptr        Any pointer argument was NULL.
 * @retval k_ra8_err_protocol_error  Malformed JPEG stream.
 * @retval k_ra8_err_not_supported   Progressive / arithmetic / 12-bit /
 *                                  unsupported chroma layout.
 * @retval k_ra8_err_invalid_size    `out_buf_len` < `width*height*3`.
 *
 * @pre `jpeg_buf` and `out_buf` reference disjoint memory.
 * @pre Image dimensions in the SOF0 marker are non-zero.
 * @post On success `*out_w * *out_h * 3 <= out_buf_len`.
 * @post On any error `out_buf` contents are unspecified but the
 *       caller's stack is unaffected.
 *
 * @note Thread-safe (re-entrant): all state lives on the caller's
 *       stack.
 *
 * @warning The decoder does not stream -- the entire JPEG must be
 *          buffered in memory before the call. For typical RA8D2
 *          camera frames (<= 256 KiB) this is fine.
 *
 * @see ra8_jpeg_sw_get_dimensions  Lightweight dimension probe.
 * @see ra8_jpeg_sw_encode          Inverse operation.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_jpeg_sw_decode(const uint8_t* jpeg_buf,
                                           uint32_t       jpeg_len,
                                           uint8_t*       out_buf,
                                           uint32_t       out_buf_len,
                                           uint16_t*      out_w,
                                           uint16_t*      out_h);

/**
 * @brief Encode a packed RGB888 frame as a baseline JPEG.
 *
 * @details
 * Emits an SOI / APP0(JFIF 1.01) / DQT / SOF0 / DHT / SOS /
 * compressed-data / EOI byte stream. Quantization tables are the
 * `T.81 Annex K.1 "Quantization table examples"` luma and chroma
 * tables, scaled by the IJG quality formula (`scale = 5000/q` for
 * `q < 50`, `200 - 2q` for `q >= 50`). Huffman tables are the
 * `T.81 Annex K.3.3 "Typical Huffman tables for 8-bit precision
 * luminance and chrominance"` reference tables, included verbatim
 * via DHT segments rather than being optimized per image.
 *
 * Colour space: input is interpreted as sRGB and converted to
 * YCbCr per ITU-R BT.601, then chroma is averaged 4:1 to produce
 * 4:2:0 subsampling. Width and height are padded to a multiple of
 * 16 internally; the SOF0 marker still records the requested
 * unpadded dimensions so decoders crop correctly.
 *
 * @param[in]  rgb_buf     Source pixels, 3 bytes per pixel
 *                         (R, G, B), `width * height * 3` total.
 * @param[in]  width       Image width in pixels (1..0xFFFF).
 * @param[in]  height      Image height in pixels (1..0xFFFF).
 * @param[in]  quality     Quality factor in
 *                         [`k_ra8_jpeg_sw_quality_min`,
 *                          `k_ra8_jpeg_sw_quality_max`].
 * @param[out] out_buf     Destination buffer for the JPEG bytes.
 * @param[in]  out_buf_len Capacity of `out_buf`.
 * @param[out] out_len     Receives the encoded byte count.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok                Encoded; `*out_len` <= `out_buf_len`.
 * @retval k_ra8_err_null_ptr      Any pointer argument was NULL.
 * @retval k_ra8_err_invalid_arg   `quality` out of range, or
 *                                `width == 0`, or `height == 0`.
 * @retval k_ra8_err_invalid_size  `out_buf_len` too small to hold the
 *                                encoded stream (worst case is
 *                                roughly `width * height * 3`).
 *
 * @pre `rgb_buf`, `out_buf` and `out_len` are non-NULL.
 * @pre `out_buf` capacity is at least `width * height` bytes for
 *      well-compressing inputs (caller should over-provision).
 * @post On success the bytes at `out_buf[0..*out_len)` form a valid
 *       JFIF 1.01 JPEG file.
 * @post On any error `*out_len` is set to 0.
 *
 * @note Thread-safe.
 *
 * @par Example:
 * @code
 * uint32_t produced;
 * ra8_err_t e = ra8_jpeg_sw_encode(rgb, 640, 480,
 *                                k_ra8_jpeg_sw_quality_default,
 *                                jpeg, sizeof jpeg, &produced);
 * @endcode
 *
 * @see ra8_jpeg_sw_decode  Inverse operation.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_jpeg_sw_encode(const uint8_t* rgb_buf,
                                           uint16_t       width,
                                           uint16_t       height,
                                           uint8_t        quality,
                                           uint8_t*       out_buf,
                                           uint32_t       out_buf_len,
                                           uint32_t*      out_len);

#ifdef __cplusplus
}
#endif
