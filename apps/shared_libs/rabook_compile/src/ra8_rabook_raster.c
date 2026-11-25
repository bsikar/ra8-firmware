/**
 * @file ra8_rabook_raster.c
 * @brief Caller-arena raster normalizer for RABOOK image records.
 *
 * @details
 * Decodes JPEG, PNG, GIF, BMP, or WebP bytes through caller-supplied codec
 * arenas, optionally bounds the longer edge, and emits deterministic gray4 or
 * gray8 payloads. Codec bindings are released on every path, and no filesystem
 * service or heap allocation is required by the normalization pipeline.
 *
 * @since 0.1.0
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_rabook_raster.h"

#include <limits.h>
#include <stddef.h>
#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_img_arena.h"
#include "ra8_img_imgdec.h"
#include "ra8_imgdec.h"
#include "ra8_imgdec_mux.h"
#include "ra8_imgdec_scratch.h"
#include "ra8_jpeg_imgdec.h"
#include "ra8_rabook_gray4.h"
#include "ra8_webp_imgdec.h"

/** @brief Raster color-conversion constants. */
typedef enum : uint16_t {
  k_raster_rgb_bytes_per_px  = 3U,   /**< Decoded RGB888 bytes per pixel. */
  k_raster_rgba_bytes_per_px = 4U,   /**< Decoded RGBA bytes per pixel.   */
  k_raster_luma_r            = 77U,  /**< stb-compatible red weight.      */
  k_raster_luma_g            = 150U, /**< stb-compatible green weight.    */
  k_raster_luma_b            = 29U,  /**< stb-compatible blue weight.     */
  k_raster_luma_shift        = 8U,   /**< Luminance normalization shift.  */
} ra8_rabook_raster_consts_t;

/**
 * @brief Convert an in-place RGB/RGBA frame to a packed grayscale prefix.
 * @details Reads each multi-byte pixel before replacing the corresponding byte
 *          in the frame prefix with BT.601 integer luminance.
 * @param[in,out] frame     Frame containing @p pixels packed RGB or RGBA pixels.
 * @param[in]     pixels    Number of pixels to convert.
 * @param[in]     step      Bytes per source pixel (3 for RGB888, 4 for RGBA8888).
 * @pre @p frame is non-NULL and spans `pixels * step` readable/writable bytes.
 * @post The first @p pixels bytes hold grayscale samples.
 * @note Pure with respect to global state; thread-safe for distinct frames.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_packed_to_gray(uint8_t* frame, uint32_t pixels, uint32_t step)
{
  for (uint32_t i = 0U; i < pixels; ++i) {
    const size_t   p = (size_t)i * (size_t)step;
    const uint32_t y = ((uint32_t)frame[p] * (uint32_t)k_raster_luma_r) +
                       ((uint32_t)frame[p + 1U] * (uint32_t)k_raster_luma_g) +
                       ((uint32_t)frame[p + 2U] * (uint32_t)k_raster_luma_b);
    frame[i]         = (uint8_t)(y >> k_raster_luma_shift);
  }
}

/**
 * @brief Compute output geometry, preserving dimensions when unclamped.
 * @details A zero clamp copies the source dimensions. A nonzero clamp delegates
 *          to the shared fixed-point RABOOK dimension helper.
 * @param[in] source_width Source width in pixels.
 * @param[in] source_height Source height in pixels.
 * @param[in] max_edge Optional longer-edge limit; zero disables scaling.
 * @param[out] out_width Receives the output width.
 * @param[out] out_height Receives the output height.
 * @pre Output pointers are non-NULL and source dimensions are nonzero.
 * @pre @p max_edge is zero or a valid nonzero pixel edge.
 * @post Outputs preserve the source or fit within @p max_edge.
 * @post Both output dimensions are nonzero.
 * @note Pure and thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_output_dims(uint16_t  source_width,
                                              uint16_t  source_height,
                                              uint16_t  max_edge,
                                              uint16_t* out_width,
                                              uint16_t* out_height)
{
  if (max_edge == 0U) {
    *out_width  = source_width;
    *out_height = source_height;
    return;
  }
  ra8_rabook_gray4_output_dims(source_width, source_height, max_edge, out_width, out_height);
}

/**
 * @brief Select the source or caller downscale buffer for final encoding.
 * @details Reuses @p source_gray when geometry is unchanged; otherwise checks
 *          the caller's gray capacity and fills it through the shared bilinear
 *          kernel before returning that buffer.
 * @param[in] source_gray Source grayscale pixels.
 * @param[in] source_width Source width in pixels.
 * @param[in] source_height Source height in pixels.
 * @param[in] out_width Requested output width.
 * @param[in] out_height Requested output height.
 * @param[in,out] workspace Exclusive caller scratch descriptor.
 * @param[out] out_gray Receives the selected grayscale buffer.
 * @return Scaling status.
 * @retval k_ra8_ok Source or scaled pixels are ready.
 * @retval k_ra8_err_no_mem The gray scratch is absent or undersized.
 * @return Other downscale errors propagate unchanged.
 * @pre Pointer arguments are non-NULL and geometries are nonzero.
 * @pre @p source_gray spans `source_width * source_height` readable bytes.
 * @post On success @p out_gray spans `out_width * out_height` readable bytes.
 * @post The source buffer is not modified.
 * @note Thread-safe for distinct workspaces.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_scale(const uint8_t*                 source_gray,
                                             uint16_t                       source_width,
                                             uint16_t                       source_height,
                                             uint16_t                       out_width,
                                             uint16_t                       out_height,
                                             ra8_rabook_raster_workspace_t* workspace,
                                             const uint8_t**                out_gray)
{
  if (source_width == out_width) {
    if (source_height == out_height) {
      *out_gray = source_gray;
      return k_ra8_ok;
    }
  }
  const uint32_t pixels = (uint32_t)out_width * (uint32_t)out_height;
  if (workspace->gray == nullptr) {
    return k_ra8_err_no_mem;
  }
  if ((size_t)pixels > workspace->gray_cap) {
    return k_ra8_err_no_mem;
  }
  const ra8_err_t rc = ra8_rabook_gray4_downscale(source_gray,
                                                  source_width,
                                                  source_height,
                                                  workspace->gray,
                                                  out_width,
                                                  out_height);
  if (rc == k_ra8_ok) {
    *out_gray = workspace->gray;
  }
  return rc;
}

/**
 * @brief Encode scaled grayscale pixels at the requested device depth.
 * @details Saturates a host-sized capacity to the uint32 firmware API and
 *          dispatches to gray8 copy or gray4 nibble packing.
 * @param[in] gray Scaled grayscale pixels.
 * @param[in] width Pixel width.
 * @param[in] height Pixel height.
 * @param[in] pixel_format Valid RABOOK gray depth.
 * @param[out] encoded Encoded destination bytes.
 * @param[in] encoded_cap Writable destination capacity.
 * @param[out] out_size Receives the exact encoded byte count.
 * @return Encoder status.
 * @retval k_ra8_ok Payload encoded.
 * @retval k_ra8_err_no_mem Destination is too small.
 * @return Other encoder argument errors propagate unchanged.
 * @pre Pointers are non-NULL and @p gray spans `width * height` bytes.
 * @pre @p pixel_format is gray4 or gray8.
 * @post On success @p encoded contains exactly @p *out_size payload bytes.
 * @post The input grayscale buffer is not modified.
 * @note Thread-safe for distinct buffers.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_encode(const uint8_t* gray,
                                              uint16_t       width,
                                              uint16_t       height,
                                              uint8_t        pixel_format,
                                              uint8_t*       encoded,
                                              size_t         encoded_cap,
                                              uint32_t*      out_size)
{
  const uint32_t bounded_cap = (encoded_cap > UINT32_MAX) ? UINT32_MAX : (uint32_t)encoded_cap;
  if (pixel_format == (uint8_t)k_book_pixfmt_gray8) {
    return ra8_rabook_gray8_encode(gray, width, height, encoded, bounded_cap, out_size);
  }
  return ra8_rabook_gray4_encode(gray, width, height, encoded, bounded_cap, out_size);
}

/**
 * @brief Bind available raster decoders into @p mux in priority order.
 * @details Always registers the stateless baseline JPEG backend first, then
 *          binds WebP and residual stb backends whenever their caller arenas
 *          are present.
 * @param[out]    mux       Mux initialized with all bound backends.
 * @param[in,out] workspace Caller arenas for WebP and stb backends.
 * @return Initialization status.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_init_mux(ra8_imgdec_mux_t*              mux,
                                                ra8_rabook_raster_workspace_t* workspace)
{
  ra8_err_t rc = ra8_imgdec_mux_init(mux);
  if (rc != k_ra8_ok) {
    return rc;
  }

  ra8_imgdec_t dec = {};
  rc               = ra8_jpeg_imgdec_bind(&dec);
  if (rc != k_ra8_ok) {
    return rc;
  }
  rc = ra8_imgdec_mux_add(mux, &dec);
  if (rc != k_ra8_ok) {
    return rc;
  }

  if (workspace->webp_arena != nullptr) {
    rc = ra8_webp_imgdec_bind(&dec, workspace->webp_arena);
    if (rc != k_ra8_ok) {
      return rc;
    }
    rc = ra8_imgdec_mux_add(mux, &dec);
    if (rc != k_ra8_ok) {
      return rc;
    }
  }

  if (workspace->stb_arena != nullptr) {
    rc = ra8_img_imgdec_bind(&dec, workspace->stb_arena);
    if (rc != k_ra8_ok) {
      return rc;
    }
    rc = ra8_imgdec_mux_add(mux, &dec);
    if (rc != k_ra8_ok) {
      return rc;
    }
  }

  return k_ra8_ok;
}

/**
 * @brief Select the preferred decoded pixel layout for @p format.
 */
RA8_INTERNAL static ra8_imgdec_pixel_t internal_want_for_format(ra8_imgdec_format_t format)
{
  if (format == k_ra8_imgdec_format_webp) {
    return k_ra8_imgdec_pixel_rgba8888;
  }
  if (format == k_ra8_imgdec_format_jpeg) {
    return k_ra8_imgdec_pixel_rgb888;
  }
  return k_ra8_imgdec_pixel_grey8;
}

/**
 * @brief Probe @p source through @p mux and map geometry/container errors.
 */
RA8_INTERNAL static ra8_err_t internal_probe_raster(const ra8_imgdec_mux_t* mux,
                                                    const uint8_t*          source,
                                                    uint32_t                source_size,
                                                    ra8_imgdec_pixel_t      want,
                                                    ra8_imgdec_geom_t*      out_geom)
{
  const ra8_err_t rc = ra8_imgdec_mux_probe(mux, source, source_size, want, out_geom, nullptr);
  if (rc == k_ra8_err_invalid_size) {
    return k_ra8_err_invalid_size;
  }
  if (rc != k_ra8_ok) {
    return k_ra8_err_validation_failed;
  }
  if ((out_geom->width_px == 0U) || (out_geom->height_px == 0U) ||
      (out_geom->width_px > (uint32_t)UINT16_MAX) || (out_geom->height_px > (uint32_t)UINT16_MAX)) {
    return k_ra8_err_invalid_size;
  }
  return k_ra8_ok;
}

/**
 * @brief Decode WebP into caller RGBA storage and expose its gray prefix.
 */
RA8_INTERNAL static ra8_err_t internal_decode_webp(const ra8_imgdec_mux_t*        mux,
                                                   const uint8_t*                 source,
                                                   uint32_t                       source_size,
                                                   const ra8_imgdec_geom_t*       geom,
                                                   ra8_rabook_raster_workspace_t* workspace,
                                                   const uint8_t**                out_gray)
{
  const uint64_t frame_size =
    (uint64_t)geom->width_px * (uint64_t)geom->height_px * (uint64_t)k_raster_rgba_bytes_per_px;
  if (frame_size > (uint64_t)workspace->rgba_cap) {
    return k_ra8_err_no_mem;
  }

  const ra8_imgdec_req_t req = {.bytes      = source,
                                .byte_count = source_size,
                                .arena      = nullptr,
                                .dst        = workspace->rgba,
                                .dst_bytes  = (uint32_t)frame_size,
                                .dst_stride = 0U,
                                .format     = geom->format,
                                .want       = k_ra8_imgdec_pixel_rgba8888};
  ra8_imgdec_image_t     img = {};
  const ra8_err_t        rc  = ra8_imgdec_mux_decode(mux, &req, &img);
  if (rc != k_ra8_ok) {
    return rc;
  }
  internal_packed_to_gray(workspace->rgba,
                          img.width_px * img.height_px,
                          (uint32_t)k_raster_rgba_bytes_per_px);
  *out_gray = workspace->rgba;
  return k_ra8_ok;
}

/**
 * @brief Decode JPEG/PNG/GIF/BMP into caller stb_arena prefix storage.
 */
RA8_INTERNAL static ra8_err_t internal_decode_arena_prefix(const ra8_imgdec_mux_t*  mux,
                                                           const uint8_t*           source,
                                                           uint32_t                 source_size,
                                                           const ra8_imgdec_geom_t* geom,
                                                           ra8_imgdec_pixel_t       want,
                                                           ra8_rabook_raster_workspace_t* workspace,
                                                           uint8_t** out_pixels)
{
  const uint32_t bpp      = ra8_imgdec_pixel_bytes(want);
  const uint64_t px_count = (uint64_t)geom->width_px * (uint64_t)geom->height_px;
  const uint64_t frame    = px_count * (uint64_t)bpp;
  if ((bpp == 0U) || (frame == 0ULL) || (frame > (uint64_t)UINT32_MAX) ||
      (frame > (uint64_t)SIZE_MAX)) {
    return k_ra8_err_invalid_size;
  }

  if (ra8_imgdec_scratch_init(workspace->stb_arena,
                              workspace->stb_arena->base,
                              workspace->stb_arena->cap) != k_ra8_ok) {
    return k_ra8_err_no_mem;
  }
  uint8_t* const pixels = (uint8_t*)ra8_imgdec_scratch_alloc(workspace->stb_arena, (size_t)frame);
  if (pixels == nullptr) {
    return k_ra8_err_no_mem;
  }

  const ra8_imgdec_req_t req = {.bytes      = source,
                                .byte_count = source_size,
                                .arena      = nullptr,
                                .dst        = pixels,
                                .dst_bytes  = (uint32_t)frame,
                                .dst_stride = 0U,
                                .format     = geom->format,
                                .want       = want};
  ra8_imgdec_image_t     img = {};
  const ra8_err_t        rc  = ra8_imgdec_mux_decode(mux, &req, &img);
  if (rc != k_ra8_ok) {
    ra8_imgdec_scratch_free(workspace->stb_arena, pixels);
    if ((rc == k_ra8_err_no_mem) || (rc == k_ra8_err_invalid_size)) {
      return rc;
    }
    return k_ra8_err_validation_failed;
  }
  if (want == k_ra8_imgdec_pixel_rgb888) {
    internal_packed_to_gray(pixels,
                            img.width_px * img.height_px,
                            (uint32_t)k_raster_rgb_bytes_per_px);
  }
  *out_pixels = pixels;
  return k_ra8_ok;
}

/** @brief One decoded source view and the ownership needed to release it. */
typedef struct internal_decoded_raster_t {
  const uint8_t* gray;       /**< Decoded grayscale samples.               */
  uint8_t*       stb_pixels; /**< Arena-owned span, or null for WebP.      */
  uint16_t       width;      /**< Source width in pixels.                  */
  uint16_t       height;     /**< Source height in pixels.                 */
  bool           webp;       /**< Whether the WebP caller storage owns it. */
} internal_decoded_raster_t;

/**
 * @brief Decode one supported source into a normalized grayscale view.
 * @details Sniffs the container signature, validates required caller arenas,
 *          routes JPEG/PNG/GIF/BMP/WebP through `ra8_imgdec_mux`, and exposes
 *          the resulting gray samples in @p decoded.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_decode_source(const uint8_t*                 source,
                                        size_t                         source_size,
                                        ra8_rabook_raster_workspace_t* workspace,
                                        internal_decoded_raster_t*     decoded)
{
  if (source_size > (size_t)INT_MAX) {
    return k_ra8_err_invalid_size;
  }
  const uint32_t      window = (source_size > (size_t)k_ra8_imgdec_sniff_bytes)
                                 ? (uint32_t)k_ra8_imgdec_sniff_bytes
                                 : (uint32_t)source_size;
  ra8_imgdec_format_t format = k_ra8_imgdec_format_none;
  if (ra8_imgdec_sniff(source, window, &format) != k_ra8_ok) {
    return k_ra8_err_validation_failed;
  }

  const bool is_webp = (format == k_ra8_imgdec_format_webp);
  decoded->webp      = is_webp;
  if (is_webp) {
    if ((workspace->webp_arena == nullptr) || (workspace->rgba == nullptr)) {
      return k_ra8_err_no_mem;
    }
  } else if (workspace->stb_arena == nullptr) {
    return k_ra8_err_no_mem;
  }

  ra8_imgdec_mux_t mux = {};
  ra8_err_t        rc  = internal_init_mux(&mux, workspace);
  if (rc != k_ra8_ok) {
    return rc;
  }

  const ra8_imgdec_pixel_t want = internal_want_for_format(format);
  ra8_imgdec_geom_t        geom = {};
  rc = internal_probe_raster(&mux, source, (uint32_t)source_size, want, &geom);
  if (rc != k_ra8_ok) {
    return rc;
  }

  decoded->width  = (uint16_t)geom.width_px;
  decoded->height = (uint16_t)geom.height_px;
  if (is_webp) {
    decoded->stb_pixels = nullptr;
    return internal_decode_webp(&mux,
                                source,
                                (uint32_t)source_size,
                                &geom,
                                workspace,
                                &decoded->gray);
  }
  rc = internal_decode_arena_prefix(&mux,
                                    source,
                                    (uint32_t)source_size,
                                    &geom,
                                    want,
                                    workspace,
                                    &decoded->stb_pixels);
  if (rc == k_ra8_ok) {
    decoded->gray = decoded->stb_pixels;
  }
  return rc;
}

/**
 * @brief Scale, encode, and release one successfully validated source.
 * @details Decodes to gray, derives aspect-preserving bounded dimensions,
 *          scales when required, packs the requested depth, and releases arena
 *          ownership on every post-decode path.
 * @param[in] source Encoded raster bytes.
 * @param[in] source_size Accessible source length in bytes.
 * @param[in] max_edge Maximum output width or height in pixels.
 * @param[in] pixel_format Requested gray4 or gray8 output format.
 * @param[in,out] workspace Caller-owned decode and scale arenas.
 * @param[out] encoded Destination for packed output bytes.
 * @param[in] encoded_cap Writable bytes at @p encoded.
 * @param[out] result Receives dimensions, format, and encoded length on success.
 * @return Complete raster pipeline status.
 * @retval k_ra8_ok The raster was decoded, scaled, and encoded.
 * @retval k_ra8_err_validation_failed Source decoding failed.
 * @retval k_ra8_err_invalid_size Geometry or destination capacity is invalid.
 * @retval k_ra8_err_no_mem A caller-owned arena cannot satisfy the operation.
 * @pre Pointer arguments and workspace arenas passed public validation.
 * @pre @p encoded addresses @p encoded_cap writable bytes disjoint from source.
 * @post Success fully initializes @p result and the reported encoded prefix.
 * @post Failure zeros @p result and releases any temporary arena ownership.
 * @note Not thread-safe; decoder arena binding is process-global.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_encode_source(const uint8_t*                 source,
                                        size_t                         source_size,
                                        uint16_t                       max_edge,
                                        uint8_t                        pixel_format,
                                        ra8_rabook_raster_workspace_t* workspace,
                                        uint8_t*                       encoded,
                                        size_t                         encoded_cap,
                                        ra8_rabook_raster_result_t*    result)
{
  internal_decoded_raster_t decoded = {};
  ra8_err_t                 rc = internal_decode_source(source, source_size, workspace, &decoded);
  if (rc != k_ra8_ok) {
    return rc;
  }
  uint16_t out_width  = 0U;
  uint16_t out_height = 0U;
  internal_output_dims(decoded.width, decoded.height, max_edge, &out_width, &out_height);
  const uint8_t* scaled = nullptr;
  rc                    = internal_scale(decoded.gray,
                                         decoded.width,
                                         decoded.height,
                                         out_width,
                                         out_height,
                                         workspace,
                                         &scaled);
  if (rc == k_ra8_ok) {
    rc = internal_encode(scaled,
                         out_width,
                         out_height,
                         pixel_format,
                         encoded,
                         encoded_cap,
                         &result->encoded_size);
  }
  if (!decoded.webp && (decoded.stb_pixels != nullptr)) {
    ra8_imgdec_scratch_free(workspace->stb_arena, decoded.stb_pixels);
  }
  if (rc != k_ra8_ok) {
    *result = (ra8_rabook_raster_result_t){0};
    return rc;
  }
  result->width        = out_width;
  result->height       = out_height;
  result->pixel_format = pixel_format;
  return k_ra8_ok;
}

ra8_err_t ra8_rabook_raster_encode(const uint8_t*                 source,
                                   size_t                         source_size,
                                   uint16_t                       max_edge,
                                   uint8_t                        pixel_format,
                                   ra8_rabook_raster_workspace_t* workspace,
                                   uint8_t*                       encoded,
                                   size_t                         encoded_cap,
                                   ra8_rabook_raster_result_t*    result)
{
  if (source == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (workspace == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (encoded == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (result == nullptr) {
    return k_ra8_err_null_ptr;
  }
  *result = (ra8_rabook_raster_result_t){0};
  if (source_size == 0U) {
    return k_ra8_err_invalid_arg;
  }
  if (pixel_format != (uint8_t)k_book_pixfmt_gray4) {
    if (pixel_format != (uint8_t)k_book_pixfmt_gray8) {
      return k_ra8_err_invalid_arg;
    }
  }

  return internal_encode_source(source,
                                source_size,
                                max_edge,
                                pixel_format,
                                workspace,
                                encoded,
                                encoded_cap,
                                result);
}
