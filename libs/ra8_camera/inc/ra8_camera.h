/**
 * @file ra8_camera.h
 * @brief Transport-neutral camera source and image-codec facade.
 * @ingroup grp_camera
 *
 * @par Tag
 * [Ring 4 / Service] {World: NS}
 *
 * @details
 * A camera source captures into storage supplied by its caller and returns an
 * immutable frame view describing the bytes it produced. A codec consumes one
 * such view and either fills another caller-owned buffer or, for a zero-copy
 * backend such as JPEG passthrough, returns a view of the original bytes.
 * Concrete sources and codecs bind opaque vtables through backend-specific
 * `_init()` helpers, following the same pattern as `ra8_io_stream`.
 *
 * No function allocates memory. Handles, backend state, capture buffers, and
 * encoded-output buffers all belong to the caller and must out-live operations
 * that use them.
 *
 * @code
 * uint8_t capture_bytes[640U * 480U * 3U];
 * uint8_t jpeg_bytes[128U * 1024U];
 * ra8_camera_buffer_t capture = {capture_bytes, sizeof capture_bytes};
 * ra8_camera_buffer_t encoded = {jpeg_bytes, sizeof jpeg_bytes};
 * ra8_camera_frame_t frame = {};
 * ra8_camera_frame_t jpeg = {};
 * (void)ra8_camera_source_capture(&source, &capture, &frame);
 * (void)ra8_camera_codec_encode(&codec, &frame, &encoded, &jpeg);
 * @endcode
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_err.h"

/** @brief Opaque source vtable; concrete source backends define its rows. */
typedef struct ra8_camera_source_iface ra8_camera_source_iface_t;

/** @brief Opaque codec vtable; concrete codec backends define its rows. */
typedef struct ra8_camera_codec_iface ra8_camera_codec_iface_t;

/**
 * @enum ra8_camera_format_t
 * @brief Pixel and compressed-image layouts understood by the facade.
 *
 * @details Uncompressed formats are tightly described by `stride_bytes` in a
 *          frame. JPEG is a complete compressed byte stream and has stride 0.
 * @invariant Every value has a stable one-byte representation.
 * @invariant `k_ra8_camera_format_jpeg` denotes a complete SOI-to-EOI stream.
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_ra8_camera_format_rgb888 = 0U, /**< Packed R, G, B bytes per pixel.    */
  k_ra8_camera_format_uyvy422,     /**< Packed Cb, Y0, Cr, Y1 pixel pairs. */
  k_ra8_camera_format_jpeg,        /**< Complete encoded JPEG byte stream. */
} ra8_camera_format_t;

/**
 * @struct ra8_camera_buffer_t
 * @brief Caller-owned writable byte-buffer description.
 *
 * @details The facade never allocates, grows, or frees this storage. A source
 *          writes capture bytes here; a codec backend may use it for encoded
 *          output. A zero-copy codec is allowed to ignore it.
 * @invariant `data` addresses at least `capacity` writable bytes when capacity
 *            is non-zero.
 * @since 0.1.0
 */
typedef struct {
  uint8_t* data;     /**< First writable byte, or NULL when capacity is zero. */
  uint32_t capacity; /**< Writable byte capacity.                             */
} ra8_camera_buffer_t;

/**
 * @struct ra8_camera_frame_t
 * @brief Immutable view of one captured or encoded image.
 *
 * @details `data` is borrowed. For source captures it points at the supplied
 *          capture buffer. For software encoding it points at the supplied
 *          output buffer. A passthrough codec may instead alias its input.
 * @invariant Width and height are non-zero for every valid frame.
 * @invariant JPEG frames have stride 0; uncompressed frames cover every row.
 * @since 0.1.0
 */
typedef struct {
  const uint8_t*      data;         /**< Borrowed immutable image bytes.   */
  uint32_t            bytes;        /**< Valid bytes beginning at `data`.  */
  uint32_t            stride_bytes; /**< Bytes between uncompressed rows.  */
  uint16_t            width;        /**< Image width in pixels.            */
  uint16_t            height;       /**< Image height in pixels.           */
  ra8_camera_format_t format;       /**< Pixel or compressed-image layout. */
} ra8_camera_frame_t;

/**
 * @struct ra8_camera_source_info_t
 * @brief Native geometry and worst-case storage required by a source.
 *
 * @details Query this before allocating a capture buffer. `frame_bytes_max` is
 *          a hard bound: a successful capture never reports more bytes.
 * @invariant Width, height, stride, and `frame_bytes_max` are non-zero.
 * @since 0.1.0
 */
typedef struct {
  uint32_t            frame_bytes_max; /**< Required capture-buffer capacity. */
  uint32_t            stride_bytes;    /**< Native row stride, or zero/JPEG.  */
  uint16_t            width;           /**< Native width in pixels.           */
  uint16_t            height;          /**< Native height in pixels.          */
  ra8_camera_format_t format;          /**< Native output format.             */
} ra8_camera_source_info_t;

/**
 * @struct ra8_camera_source_t
 * @brief Caller-owned handle binding one source backend and its context.
 *
 * @details Zero-initialise, then pass to a source backend's `_init()` helper.
 *          Treat both fields as private and dispatch only through this facade.
 * @invariant `iface` is non-NULL after a successful backend init.
 * @since 0.1.0
 */
typedef struct {
  const ra8_camera_source_iface_t* iface; /**< Bound source vtable (private). */
  void*                            ctx;   /**< Caller-owned backend context.  */
} ra8_camera_source_t;

/**
 * @struct ra8_camera_codec_t
 * @brief Caller-owned handle binding one image codec and its context.
 *
 * @details Zero-initialise, then pass to a codec backend's `_init()` helper.
 *          Treat both fields as private and dispatch only through this facade.
 * @invariant `iface` is non-NULL after a successful backend init.
 * @since 0.1.0
 */
typedef struct {
  const ra8_camera_codec_iface_t* iface; /**< Bound codec vtable (private). */
  void*                           ctx;   /**< Caller-owned backend context. */
} ra8_camera_codec_t;

/**
 * @brief Validate the geometry and storage coverage of a frame view.
 *
 * @param[in] frame Candidate frame.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               Frame is internally consistent.
 * @retval k_ra8_err_null_ptr     `frame` or `frame->data` is NULL.
 * @retval k_ra8_err_invalid_arg  Format is unknown or geometry is invalid.
 * @retval k_ra8_err_invalid_size Byte count or stride cannot cover the frame.
 * @pre `frame` points to readable metadata.
 * @pre When non-NULL, `frame->data` is readable for `frame->bytes` bytes.
 * @post No state is modified.
 * @post Success means a backend may safely read the described bytes.
 * @note Thread-safe; reads only its argument.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_camera_frame_validate(const ra8_camera_frame_t* frame);

/**
 * @brief Query a bound source's native output and capture-buffer bound.
 *
 * @param[in]  source   Bound source handle.
 * @param[out] out_info Receives source information.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                  Information returned.
 * @retval k_ra8_err_null_ptr        An argument was NULL.
 * @retval k_ra8_err_not_initialized No source is bound.
 * @retval other                     Propagated from the source backend.
 * @pre `source` was initialised by a source backend.
 * @pre `out_info` is writable.
 * @post On success `out_info` describes every later capture.
 * @post On error `out_info` is zeroed.
 * @note Not thread-safe with respect to the same source.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_camera_source_get_info(ra8_camera_source_t*      source,
                                                   ra8_camera_source_info_t* out_info);

/**
 * @brief Capture one frame into a caller-owned buffer.
 *
 * @param[in,out] source    Bound source handle.
 * @param[in]     buffer    Writable capture buffer.
 * @param[out]    out_frame Receives an immutable view of captured bytes.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                  One valid frame captured.
 * @retval k_ra8_err_null_ptr        An argument or buffer pointer was NULL.
 * @retval k_ra8_err_not_initialized No source is bound.
 * @retval k_ra8_err_invalid_size    Buffer is too small for the source.
 * @retval other                     Propagated from the source backend.
 * @pre `buffer->data` covers `buffer->capacity` writable bytes.
 * @pre No other call is using `source` or `buffer`.
 * @post On success `out_frame->data == buffer->data` and validates.
 * @post On error `out_frame` is zeroed.
 * @note Not thread-safe with respect to the same source or buffer.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_camera_source_capture(ra8_camera_source_t*       source,
                                                  const ra8_camera_buffer_t* buffer,
                                                  ra8_camera_frame_t*        out_frame);

/**
 * @brief Encode or transform one frame through a bound codec.
 *
 * @details A codec may fill `output_buffer` or return a zero-copy view which
 *          aliases `input`. The returned frame's lifetime is therefore bounded
 *          by both input and output storage.
 * @param[in,out] codec         Bound codec handle.
 * @param[in]     input         Valid source frame.
 * @param[in]     output_buffer Caller-owned output storage; may be empty for a
 *                              backend documented as zero-copy.
 * @param[out]    out_frame     Receives the encoded frame view.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                  Frame produced.
 * @retval k_ra8_err_null_ptr        An argument was NULL.
 * @retval k_ra8_err_not_initialized No codec is bound.
 * @retval k_ra8_err_not_supported   Codec cannot accept `input->format`.
 * @retval other                     Propagated from validation or the backend.
 * @pre `input` passes ::ra8_camera_frame_validate.
 * @pre Output storage does not overlap input unless the backend allows it.
 * @post On success `out_frame` passes ::ra8_camera_frame_validate.
 * @post On error `out_frame` is zeroed.
 * @note Not thread-safe with respect to the same codec or buffers.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_camera_codec_encode(ra8_camera_codec_t*        codec,
                                                const ra8_camera_frame_t*  input,
                                                const ra8_camera_buffer_t* output_buffer,
                                                ra8_camera_frame_t*        out_frame);

#ifdef __cplusplus
}
#endif
