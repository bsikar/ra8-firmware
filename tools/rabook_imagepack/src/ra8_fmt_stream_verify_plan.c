/**
 * @file ra8_fmt_stream_verify_plan.c
 * @brief Exact caller-workspace planning for JOF verification.
 * @details Probes encoded JPEG, PNG, and WebP headers through positioned reads,
 * derives decoder channel geometry, and reports exact caller-workspace bounds.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_fmt_stream.h"
#include "ra8_jof_produce.h"

/** @brief Bounded encoded-header probe constants. */
typedef enum : uint32_t {
  k_verify_png_head         = 26U,         /**< Signature, IHDR, and colour type. */
  k_verify_png_chunks       = 33U,         /**< First post-IHDR chunk offset.     */
  k_verify_png_trns         = 0x74524E53U, /**< PNG tRNS chunk code.              */
  k_verify_png_idat         = 0x49444154U, /**< PNG IDAT chunk code.              */
  k_verify_jpeg_sof0        = 0xC0U,       /**< First JPEG SOF code.              */
  k_verify_jpeg_dht         = 0xC4U,       /**< JPEG table code in SOF range.     */
  k_verify_jpeg_jpg         = 0xC8U,       /**< JPEG reserved SOF-range code.     */
  k_verify_jpeg_dac         = 0xCCU,       /**< JPEG arithmetic table code.       */
  k_verify_jpeg_marker      = 0xFFU,       /**< JPEG marker introducer.           */
  k_verify_jpeg_soi         = 0xD8U,       /**< JPEG start-of-image marker.        */
  k_verify_jpeg_sof_last    = 0xCFU,       /**< Last JPEG SOF-range marker.        */
  k_verify_u32_high_shift   = 24U,         /**< Big-endian uint32 high shift.      */
  k_verify_jpeg_components  = 7U,          /**< Component-count offset after len.  */
  k_verify_png_chunk_record = 12U,         /**< PNG chunk framing bytes.           */
  k_verify_png_color_type   = 25U,         /**< PNG IHDR colour-type offset.       */
} verify_plan_const_t;

/**
 * @brief Read one exact positioned span.
 * @details Loops over legal short reads and rejects premature or oversized progress.
 * @param[in] source Bound immutable source.
 * @param[in] offset Absolute offset.
 * @param[out] bytes Exact destination span.
 * @param[in] len Required byte count.
 * @return Callback status or protocol failure.
 * @retval k_ra8_ok Every requested byte was initialized.
 * @retval k_ra8_err_protocol_error The range or callback progress was invalid.
 * @retval other Positioned-source callback status.
 * @pre The requested range is within @p source size.
 * @pre Non-empty @p bytes spans @p len writable bytes.
 * @post Success initializes every requested byte.
 * @post Source position and captured extent remain unchanged.
 * @note Thread safety inherits the source callback.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t
internal_exact(const ra8_fmt_source_t* source, uint64_t offset, uint8_t* bytes, size_t len)
{
  if ((offset > source->size) || ((uint64_t)len > (source->size - offset))) {
    return k_ra8_err_protocol_error;
  }
  size_t done = 0U;
  while (done < len) {
    size_t          got = 0U;
    const ra8_err_t rc =
      source->read_at(source->ctx, offset + done, &bytes[done], len - done, &got);
    if (rc != k_ra8_ok) {
      return rc;
    }
    if ((got == 0U) || (got > (len - done))) {
      return k_ra8_err_protocol_error;
    }
    done += got;
  }
  return k_ra8_ok;
}

/**
 * @brief Decode one big-endian uint16 field.
 * @details Combines exactly two network-order bytes without unaligned access.
 * @param[in] bytes Two encoded bytes.
 * @return Decoded host-order value.
 * @retval UINT16_MAX The encoded field contains all one bits.
 * @pre @p bytes spans exactly two readable bytes.
 * @pre The caller has already bounded the enclosing encoded object.
 * @post No source byte changes.
 * @post The return value depends only on the two input bytes.
 * @note Pure and thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static uint16_t internal_be16(const uint8_t bytes[2])
{
  return (uint16_t)(((uint16_t)bytes[0] << 8U) | bytes[1]);
}

/**
 * @brief Decode one big-endian uint32 field.
 * @details Combines exactly four network-order bytes without unaligned access.
 * @param[in] bytes Four encoded bytes.
 * @return Decoded host-order value.
 * @retval UINT32_MAX The encoded field contains all one bits.
 * @pre @p bytes spans exactly four readable bytes.
 * @pre The caller has already bounded the enclosing encoded object.
 * @post No source byte changes.
 * @post The return value depends only on the four input bytes.
 * @note Pure and thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static uint32_t internal_be32(const uint8_t bytes[4])
{
  return ((uint32_t)bytes[0] << k_verify_u32_high_shift) | ((uint32_t)bytes[1] << 16U) |
         ((uint32_t)bytes[2] << 8U) | bytes[3];
}

/**
 * @brief Classify a JPEG SOF-range marker without table-marker false positives.
 * @details Accepts frame markers while excluding DHT, JPG, and DAC codes.
 * @param[in] marker Candidate marker byte.
 * @return Whether @p marker is a supported start-of-frame marker.
 * @retval true The marker denotes a frame header.
 * @retval false The marker is outside the range or names a table.
 * @pre @p marker is the byte following a JPEG marker introducer.
 * @pre Marker stuffing has already been removed.
 * @post No parser or source state changes.
 * @post Classification is deterministic.
 * @note Pure and thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static bool internal_is_sof(uint8_t marker)
{
  const bool range =
    (marker >= (uint8_t)k_verify_jpeg_sof0) && (marker <= (uint8_t)k_verify_jpeg_sof_last);
  return range && (marker != (uint8_t)k_verify_jpeg_dht) &&
         (marker != (uint8_t)k_verify_jpeg_jpg) && (marker != (uint8_t)k_verify_jpeg_dac);
}

/**
 * @brief Determine baseline JPEG output channels from the first SOF.
 * @details Walks bounded marker segments until a supported frame header is found.
 * @param[in] source JPEG source.
 * @param[out] bpp Receives one or three output channels.
 * @return Probe or unsupported-channel status.
 * @retval k_ra8_ok A supported SOF supplied the channel count.
 * @retval k_ra8_err_not_supported The SOF channel count is unsupported.
 * @retval other Source or malformed-marker status.
 * @pre @p source is non-null with a positioned-read callback.
 * @pre The source begins with a JPEG start-of-image marker.
 * @post Success initializes @p bpp.
 * @post Failure does not publish a channel count.
 * @note Work is bounded by immutable source length.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_jpeg_bpp(const ra8_fmt_source_t* source, uint8_t* bpp)
{
  uint64_t offset = 2U;
  while (offset < source->size) {
    uint8_t byte = 0U;
    do {
      ra8_err_t rc = internal_exact(source, offset++, &byte, 1U);
      if (rc != k_ra8_ok) {
        return rc;
      }
    } while (byte != (uint8_t)k_verify_jpeg_marker);
    do {
      ra8_err_t rc = internal_exact(source, offset++, &byte, 1U);
      if (rc != k_ra8_ok) {
        return rc;
      }
    } while (byte == (uint8_t)k_verify_jpeg_marker);
    uint8_t   length_bytes[2];
    ra8_err_t rc = internal_exact(source, offset, length_bytes, sizeof(length_bytes));
    if (rc != k_ra8_ok) {
      return rc;
    }
    const uint16_t segment = internal_be16(length_bytes);
    if (segment < 2U) {
      return k_ra8_err_protocol_error;
    }
    if (internal_is_sof(byte)) {
      uint8_t components = 0U;
      rc = internal_exact(source, offset + k_verify_jpeg_components, &components, 1U);
      if (rc != k_ra8_ok) {
        return rc;
      }
      if ((components != 1U) && (components != 3U)) {
        return k_ra8_err_not_supported;
      }
      *bpp = components;
      return k_ra8_ok;
    }
    offset += segment;
  }
  return k_ra8_err_protocol_error;
}

/**
 * @brief Scan pre-IDAT PNG chunks for palette transparency.
 * @details Maps IHDR colour type and optional tRNS presence to producer channels.
 * @param[in] source PNG source.
 * @param[in] color_type IHDR colour-type byte.
 * @param[out] bpp Receives the producer output channel count.
 * @return Probe or malformed-window status.
 * @retval k_ra8_ok A supported mapping reached the first IDAT.
 * @retval k_ra8_err_not_supported The colour type is unsupported.
 * @retval other Source or malformed-chunk status.
 * @pre @p source is a bounded PNG positioned source.
 * @pre @p bpp is writable.
 * @post Success initializes @p bpp exactly as the producer does.
 * @post Source bytes and position remain unchanged.
 * @note Work is bounded by the pre-IDAT source bytes.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_png_bpp(const ra8_fmt_source_t* source, uint8_t color_type, uint8_t* bpp)
{
  if (color_type == 0U) {
    *bpp = 1U;
    return k_ra8_ok;
  }
  if ((color_type == 4U) || (color_type == 6U)) {
    *bpp = 4U;
    return k_ra8_ok;
  }
  if (color_type == 2U) {
    *bpp = 3U;
    return k_ra8_ok;
  }
  if (color_type != 3U) {
    return k_ra8_err_not_supported;
  }
  uint64_t offset = k_verify_png_chunks;
  *bpp            = 3U;
  while ((offset + k_verify_png_chunk_record) <= source->size) {
    uint8_t   chunk[8];
    ra8_err_t rc = internal_exact(source, offset, chunk, sizeof(chunk));
    if (rc != k_ra8_ok) {
      return rc;
    }
    const uint32_t length = internal_be32(chunk);
    const uint32_t type   = internal_be32(&chunk[4]);
    if ((uint64_t)length > (source->size - offset - k_verify_png_chunk_record)) {
      return k_ra8_err_protocol_error;
    }
    if (type == k_verify_png_trns) {
      *bpp = 4U;
    }
    if (type == k_verify_png_idat) {
      return k_ra8_ok;
    }
    offset += k_verify_png_chunk_record + length;
  }
  return k_ra8_err_protocol_error;
}

/**
 * @brief Derive exact producer output channels from source headers.
 * @details Dispatches bounded JPEG, PNG, and WebP header probes.
 * @param[in] source Supported encoded image.
 * @param[out] bpp Receives one, three, or four.
 * @return Probe status.
 * @retval k_ra8_ok A supported source supplied its decoder channel count.
 * @retval k_ra8_err_not_supported The source kind is unsupported.
 * @retval other Source or malformed-header status.
 * @pre @p source is non-null with at least the bounded probe prefix.
 * @pre @p bpp is writable.
 * @post Success initializes @p bpp.
 * @post No source byte or callback state is modified.
 * @note Does not decode pixel bodies.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_bpp(const ra8_fmt_source_t* source, uint8_t* bpp)
{
  uint8_t   head[k_verify_png_head];
  ra8_err_t rc = internal_exact(source, 0U, head, sizeof(head));
  if (rc != k_ra8_ok) {
    return rc;
  }
  if ((head[0] == (uint8_t)k_verify_jpeg_marker) && (head[1] == (uint8_t)k_verify_jpeg_soi)) {
    return internal_jpeg_bpp(source, bpp);
  }
  static const uint8_t png[8] = {0x89U, 'P', 'N', 'G', 0x0DU, 0x0AU, 0x1AU, 0x0AU}; /* MAGIC-OK */
  if (memcmp(head, png, sizeof(png)) == 0) {
    return internal_png_bpp(source, head[k_verify_png_color_type], bpp);
  }
  if ((memcmp(head, "RIFF", 4U) == 0) && (memcmp(&head[8], "WEBP", 4U) == 0)) { /* MAGIC-OK */
    *bpp = 4U;
    return k_ra8_ok;
  }
  return k_ra8_err_not_supported;
}

/**
 * @brief Validate a source through its optional stability callback.
 * @details Delegates immutable-view revalidation while preserving optional bindings.
 * @param[in] source Source to validate.
 * @return Stability callback status or success when validation is absent.
 * @retval k_ra8_ok The source is unchanged or has no validator.
 * @retval other The backend observed mutation or validation failure.
 * @pre @p source is non-null.
 * @pre The captured size still describes the intended object.
 * @post No source position changes.
 * @post Success permits continued trust in the captured view.
 * @note Thread safety inherits the backend validator.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_stable(const ra8_fmt_source_t* source)
{
  return (source->validate == nullptr) ? k_ra8_ok : source->validate(source->ctx, source->size);
}

ra8_err_t ra8_fmt_jof_verify_requirements(const ra8_fmt_source_t*            source,
                                          ra8_fmt_jof_verify_requirements_t* out)
{
  if ((source == nullptr) || (source->read_at == nullptr) || (out == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  *out                                       = (ra8_fmt_jof_verify_requirements_t){};
  ra8_fmt_jof_convert_requirements_t convert = {};
  ra8_err_t                          rc      = ra8_fmt_jof_convert_requirements(source, &convert);
  if (rc != k_ra8_ok) {
    return rc;
  }
  rc = internal_bpp(source, &out->bpp);
  if (rc != k_ra8_ok) {
    return rc;
  }
  out->width                = convert.width;
  out->height               = convert.height;
  out->band_height          = convert.tile_height;
  out->reference_work_bytes = ra8_jof_work_bytes(out->width, out->height, out->width, 1U);
  out->banded_work_bytes    = convert.work_bytes;
  out->webp_work_bytes      = convert.webp_work_bytes;
  const uint64_t row        = (uint64_t)out->width * out->bpp;
  const uint64_t band       = row * out->band_height;
  if ((out->reference_work_bytes == 0U) || (band > UINT32_MAX)) {
    return k_ra8_err_invalid_size;
  }
  out->row_bytes       = (uint32_t)row;
  out->band_tile_bytes = (uint32_t)band;
  out->scratch_bytes   = ra8_jof_stored_bound(out->band_tile_bytes);
  return internal_stable(source);
}
