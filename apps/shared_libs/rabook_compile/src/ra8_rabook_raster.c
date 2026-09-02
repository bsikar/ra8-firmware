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
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_img_arena.h"
#include "ra8_rabook_gray4.h"
#include "ra8_webp.h"
#include "stb_image.h"

/** @brief Raster signature and color-conversion constants. */
typedef enum : uint16_t {
  k_raster_webp_signature_size = 12U,  /**< RIFF size plus WEBP form tag.  */
  k_raster_webp_form_offset    = 8U,   /**< WEBP form tag byte offset.     */
  k_raster_webp_bytes_per_px   = 4U,   /**< Decoded RGBA bytes per pixel.  */
  k_raster_luma_r              = 77U,  /**< stb-compatible red weight.     */
  k_raster_luma_g              = 150U, /**< stb-compatible green weight.   */
  k_raster_luma_b              = 29U,  /**< stb-compatible blue weight.    */
  k_raster_luma_shift          = 8U,   /**< Luminance normalization shift. */
} ra8_rabook_raster_consts_t;

/**
 * @brief Return true only for a complete RIFF/WEBP signature.
 * @details Checks both the RIFF container tag and WEBP form tag after proving
 *          the fixed header is readable; other RIFF formats remain on the stb
 *          path and fail its decoder normally.
 * @param[in] source Encoded candidate bytes.
 * @param[in] source_size Readable byte count at @p source.
 * @return Whether the candidate carries the WebP signature.
 * @retval true Both required tags match.
 * @retval false The header is short or either tag differs.
 * @pre @p source is non-NULL and spans @p source_size bytes.
 * @pre @p source_size is the complete candidate byte length.
 * @post No input or codec state is modified.
 * @post No byte beyond the twelve-byte signature window is inspected.
 * @note Pure and thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_is_webp(const uint8_t* source, size_t source_size)
{
  static const uint8_t k_riff[] = {'R', 'I', 'F', 'F'};
  static const uint8_t k_webp[] = {'W', 'E', 'B', 'P'};
  if (source_size < (size_t)k_raster_webp_signature_size) {
    return false;
  }
  if (memcmp(source, k_riff, sizeof k_riff) != 0) {
    return false;
  }
  return memcmp(&source[k_raster_webp_form_offset], k_webp, sizeof k_webp) == 0;
}

/**
 * @brief Convert an in-place RGBA frame to a packed grayscale prefix.
 * @details Reads each four-byte pixel before replacing the corresponding byte
 *          in the frame prefix with stb's deterministic integer luminance.
 * @param[in,out] rgba Frame containing @p pixels RGBA8888 pixels.
 * @param[in] pixels Number of pixels to convert.
 * @pre @p rgba is non-NULL and spans `pixels * 4` readable/writable bytes.
 * @pre `pixels * 4` is representable by size_t.
 * @post The first @p pixels bytes hold grayscale samples; later bytes are
 *       unspecified scratch and no byte outside the original frame is written.
 * @post The alpha channel does not influence any grayscale sample.
 * @note Pure with respect to global state; thread-safe for distinct frames.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_rgba_to_gray(uint8_t* rgba, uint32_t pixels)
{
  for (uint32_t i = 0U; i < pixels; ++i) {
    const size_t   p = (size_t)i * (size_t)k_raster_webp_bytes_per_px;
    const uint32_t y = ((uint32_t)rgba[p] * (uint32_t)k_raster_luma_r) +
                       ((uint32_t)rgba[p + 1U] * (uint32_t)k_raster_luma_g) +
                       ((uint32_t)rgba[p + 2U] * (uint32_t)k_raster_luma_b);
    rgba[i]          = (uint8_t)(y >> k_raster_luma_shift);
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
 * @brief Decode WebP into caller RGBA storage and expose its gray prefix.
 * @details Validates bounded dimensions, proves the full RGBA frame fits, runs
 *          the arena-backed WebP facade, then converts the frame prefix in
 *          place to deterministic grayscale.
 * @param[in] source Encoded WebP bytes.
 * @param[in] source_size Readable source length.
 * @param[in,out] workspace Caller WebP arena and RGBA frame storage.
 * @param[out] out_gray Receives the grayscale frame prefix.
 * @param[out] out_width Receives source width.
 * @param[out] out_height Receives source height.
 * @return Decode status.
 * @retval k_ra8_ok A complete gray frame is available.
 * @retval k_ra8_err_invalid_size Dimensions exceed the RABOOK wire fields.
 * @retval k_ra8_err_no_mem Required caller storage is absent or undersized.
 * @return Other WebP validation/decode errors propagate unchanged.
 * @pre All pointer arguments are non-NULL.
 * @pre Non-NULL arena/buffer members span their declared capacities.
 * @post On success the WebP arena is drained and outputs are initialized.
 * @post On error the output pointer/dimensions remain untouched.
 * @note Not thread-safe because the WebP allocator binding is global.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_decode_webp(const uint8_t*                 source,
                                                   size_t                         source_size,
                                                   ra8_rabook_raster_workspace_t* workspace,
                                                   const uint8_t**                out_gray,
                                                   uint16_t*                      out_width,
                                                   uint16_t*                      out_height)
{
  uint32_t  width  = 0U;
  uint32_t  height = 0U;
  ra8_err_t rc     = ra8_webp_get_info(source, source_size, &width, &height);
  if (rc != k_ra8_ok) {
    return rc;
  }
  if (width > UINT16_MAX) {
    return k_ra8_err_invalid_size;
  }
  if (height > UINT16_MAX) {
    return k_ra8_err_invalid_size;
  }
  const uint64_t frame_size =
    (uint64_t)width * (uint64_t)height * (uint64_t)k_raster_webp_bytes_per_px;
  if (workspace->webp_arena == nullptr) {
    return k_ra8_err_no_mem;
  }
  if (workspace->rgba == nullptr) {
    return k_ra8_err_no_mem;
  }
  if (frame_size > (uint64_t)workspace->rgba_cap) {
    return k_ra8_err_no_mem;
  }
  rc = ra8_webp_decode_rgba(source,
                            source_size,
                            workspace->webp_arena,
                            workspace->rgba,
                            (size_t)width * (size_t)k_raster_webp_bytes_per_px,
                            (size_t)frame_size,
                            nullptr,
                            nullptr);
  if (rc != k_ra8_ok) {
    return rc;
  }
  const uint32_t pixels = width * height;
  internal_rgba_to_gray(workspace->rgba, pixels);
  *out_gray   = workspace->rgba;
  *out_width  = (uint16_t)width;
  *out_height = (uint16_t)height;
  return k_ra8_ok;
}

/**
 * @brief Decode an stb-supported raster and expose arena-owned gray pixels.
 * @details Binds the caller arena, asks stb_image for one gray channel, and
 *          validates positive dimensions against the RABOOK uint16 fields. The
 *          successful allocation deliberately remains live for the caller to
 *          scale/encode before it frees and unbinds it.
 * @param[in] source Encoded JPEG/PNG/GIF/BMP bytes.
 * @param[in] source_size Readable source length.
 * @param[in,out] workspace Caller stb arena descriptor.
 * @param[out] out_gray Receives arena-owned gray pixels.
 * @param[out] out_width Receives source width.
 * @param[out] out_height Receives source height.
 * @return Decode status.
 * @retval k_ra8_ok Gray pixels remain live in the bound arena.
 * @retval k_ra8_err_invalid_size Source length or dimensions exceed an API bound.
 * @retval k_ra8_err_no_mem No stb arena was supplied.
 * @retval k_ra8_err_validation_failed stb rejected or could not decode the source.
 * @pre All pointer arguments are non-NULL.
 * @pre @p workspace's stb arena backing spans its declared capacity.
 * @post On success the image arena remains bound until the public caller frees it.
 * @post On failure the image arena is unbound before return.
 * @note Not thread-safe because the stb allocator binding is global.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_decode_stb(const uint8_t*                 source,
                                                  size_t                         source_size,
                                                  ra8_rabook_raster_workspace_t* workspace,
                                                  uint8_t**                      out_gray,
                                                  uint16_t*                      out_width,
                                                  uint16_t*                      out_height)
{
  if (source_size > (size_t)INT_MAX) {
    return k_ra8_err_invalid_size;
  }
  if (workspace->stb_arena == nullptr) {
    return k_ra8_err_no_mem;
  }
  int width      = 0;
  int height     = 0;
  int components = 0;
  ra8_img_arena_bind(workspace->stb_arena);
  uint8_t* pixels = stbi_load_from_memory(source, /* alloc-allow: caller-bound ra8_img_arena */
                                          (int)source_size,
                                          &width,
                                          &height,
                                          &components,
                                          1);
  if (pixels == nullptr) {
    ra8_img_arena_unbind();
    return k_ra8_err_validation_failed;
  }
  bool dimensions_valid = width > 0;
  if (height <= 0) {
    dimensions_valid = false;
  }
  if (width > (int)UINT16_MAX) {
    dimensions_valid = false;
  }
  if (height > (int)UINT16_MAX) {
    dimensions_valid = false;
  }
  if (!dimensions_valid) {
    stbi_image_free(pixels); /* alloc-allow: caller-bound ra8_img_arena */
    ra8_img_arena_unbind();
    return k_ra8_err_invalid_size;
  }
  *out_gray   = pixels;
  *out_width  = (uint16_t)width;
  *out_height = (uint16_t)height;
  return k_ra8_ok;
}

/** @brief One decoded source view and the ownership needed to release it. */
typedef struct internal_decoded_raster_t {
  const uint8_t* gray;       /**< Decoded grayscale samples.              */
  uint8_t*       stb_pixels; /**< stb-owned arena span, or null for WebP. */
  uint16_t       width;      /**< Source width in pixels.                 */
  uint16_t       height;     /**< Source height in pixels.                */
  bool           webp;       /**< Whether the WebP caller arena owns it.  */
} internal_decoded_raster_t;

/**
 * @brief Decode one supported source into a normalized grayscale view.
 * @details Detects WebP by signature and selects the bounded WebP or stb arena
 *          decoder, then normalizes ownership and sample access in @p decoded.
 * @param[in] source Encoded raster bytes.
 * @param[in] source_size Accessible source length in bytes.
 * @param[in,out] workspace Caller-owned decoder arenas.
 * @param[out] decoded Receives normalized pixels, dimensions, and ownership.
 * @return Decoder status.
 * @retval k_ra8_ok The source decoded to a grayscale view.
 * @retval k_ra8_err_validation_failed The encoded source is malformed.
 * @retval k_ra8_err_invalid_size Source or decoded dimensions are unsupported.
 * @retval k_ra8_err_no_mem A selected bounded arena is unavailable or too small.
 * @pre All pointer arguments are non-NULL and capacities describe accessible spans.
 * @pre @p decoded does not alias source bytes or decoder arenas.
 * @post Success initializes every field of @p decoded.
 * @post Failure does not publish a successful normalized view.
 * @note Not thread-safe; decoder arena binding is process-global.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_decode_source(const uint8_t*                 source,
                                        size_t                         source_size,
                                        ra8_rabook_raster_workspace_t* workspace,
                                        internal_decoded_raster_t*     decoded)
{
  decoded->webp          = internal_is_webp(source, source_size);
  const ra8_err_t result = decoded->webp ? internal_decode_webp(source,
                                                                source_size,
                                                                workspace,
                                                                &decoded->gray,
                                                                &decoded->width,
                                                                &decoded->height)
                                         : internal_decode_stb(source,
                                                               source_size,
                                                               workspace,
                                                               &decoded->stb_pixels,
                                                               &decoded->width,
                                                               &decoded->height);
  if (!decoded->webp) {
    decoded->gray = decoded->stb_pixels;
  }
  return result;
}

/**
 * @brief Scale, encode, and release one successfully validated source.
 * @details Decodes to gray, derives aspect-preserving bounded dimensions,
 *          scales when required, packs the requested depth, and releases stb
 *          arena ownership on every post-decode path.
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
 * @post Failure zeros @p result and releases any temporary stb ownership.
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
  if (!decoded.webp) {
    stbi_image_free(decoded.stb_pixels); /* alloc-allow: caller-bound ra8_img_arena */
    ra8_img_arena_unbind();
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
