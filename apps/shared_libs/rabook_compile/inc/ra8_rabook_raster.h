/**
 * @file ra8_rabook_raster.h
 * @brief Caller-arena raster normalizer for RABOOK image records.
 * @ingroup grp_ereader
 *
 * @details Decodes JPEG, PNG, GIF, BMP, and WebP source bytes, optionally
 * downscales them, and emits the exact gray4 or gray8 payload consumed by
 * ::book_image_t. Every byte of working storage is supplied by the caller;
 * the codec adapters use ::ra8_img_arena_t and ::ra8_webp_arena_t rather than
 * the C heap. The API has no filesystem dependency, so host tools and firmware
 * composition roots can share the same normalization path.
 *
 * [Ring 3 / RABOOK Compiler] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "book.h"
#include "ra8_err.h"
#include "ra8_img_arena.h"
#include "ra8_webp_arena.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @struct ra8_rabook_raster_workspace_t
 * @brief Exclusive caller-owned scratch used by one raster conversion.
 * @details stb_image draws transient allocations from @p stb_arena. WebP
 *          decodes into @p rgba using @p webp_arena. @p gray is used only when
 *          a decoded source must be downscaled; the encoded payload is written
 *          to the separate output supplied to ::ra8_rabook_raster_encode.
 * @invariant Non-NULL buffers span their declared capacities.
 * @invariant @p rgba, @p gray, codec arena backing stores, source, and encoded
 *            destination do not overlap while a conversion is active.
 * @since 0.1.0
 */
typedef struct {
  ra8_img_arena_t*  stb_arena;  /**< JPEG/PNG/GIF/BMP decoder arena. */
  ra8_webp_arena_t* webp_arena; /**< WebP decoder scratch arena.     */
  uint8_t*          rgba;       /**< WebP RGBA frame storage.        */
  size_t            rgba_cap;   /**< Capacity of @p rgba in bytes.   */
  uint8_t*          gray;       /**< Downscaled grayscale storage.   */
  size_t            gray_cap;   /**< Capacity of @p gray in bytes.   */
} ra8_rabook_raster_workspace_t;

/**
 * @struct ra8_rabook_raster_result_t
 * @brief Geometry and byte count of a normalized RABOOK raster payload.
 * @since 0.1.0
 */
typedef struct {
  uint32_t encoded_size; /**< Bytes written to the encoded destination. */
  uint16_t width;        /**< Output width in pixels.                   */
  uint16_t height;       /**< Output height in pixels.                  */
  uint8_t  pixel_format; /**< ::book_image_pixfmt_t emitted.            */
} ra8_rabook_raster_result_t;

/**
 * @brief Decode and normalize one encoded raster into a RABOOK image payload.
 * @details WebP is selected only by its RIFF/WEBP signature; every other source
 *          is passed to stb_image, which validates its own JPEG/PNG/GIF/BMP
 *          structure. RGBA WebP pixels use stb_image's integer luminance rule
 *          `(77R + 150G + 29B) >> 8`, keeping codec results deterministic.
 *          A zero @p max_edge preserves source geometry; otherwise the longer
 *          edge is bounded through ::ra8_rabook_gray4_output_dims.
 * @param[in]  source       Encoded image bytes.
 * @param[in]  source_size  Number of readable source bytes.
 * @param[in]  max_edge     Optional longer-edge clamp; zero preserves size.
 * @param[in]  pixel_format ::k_book_pixfmt_gray4 or
 *                           ::k_book_pixfmt_gray8.
 * @param[in,out] workspace Exclusive initialized scratch descriptor.
 * @param[out] encoded      Destination for normalized gray payload bytes.
 * @param[in]  encoded_cap  Capacity of @p encoded in bytes.
 * @param[out] result       Receives geometry, format, and encoded byte count.
 * @return Conversion status.
 * @retval k_ra8_ok                    Payload produced.
 * @retval k_ra8_err_null_ptr          A required pointer is NULL.
 * @retval k_ra8_err_invalid_arg       Source is empty or pixel format invalid.
 * @retval k_ra8_err_invalid_size      A source/dimension exceeds a bounded API.
 * @retval k_ra8_err_no_mem            Caller workspace/output is too small.
 * @retval k_ra8_err_validation_failed The encoded source is malformed.
 * @return Other WebP facade errors propagate unchanged.
 * @pre All workspace buffers and arena backings remain live for the call.
 * @pre Source, scratch, and destination regions do not overlap.
 * @post On success @p encoded contains exactly `result->encoded_size` bytes.
 * @post On failure @p result is zeroed and neither codec arena remains bound.
 * @note Not thread-safe because the underlying codec arena bindings are global.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rabook_raster_encode(const uint8_t*                 source,
                                                 size_t                         source_size,
                                                 uint16_t                       max_edge,
                                                 uint8_t                        pixel_format,
                                                 ra8_rabook_raster_workspace_t* workspace,
                                                 uint8_t*                       encoded,
                                                 size_t                         encoded_cap,
                                                 ra8_rabook_raster_result_t*    result);

#ifdef __cplusplus
} /* extern "C" */
#endif
