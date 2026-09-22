/**
 * @file ra8_camera.c
 * @brief Validation and opaque-vtable dispatch for `ra8_camera`.
 * @ingroup grp_camera
 *
 * @par Tag
 * [Ring 4 / Service] {World: NS}
 *
 * @details Keeps generic frame rules and backend dispatch in one hardware-free
 *          translation unit. Backends remain responsible only for their source
 *          or codec operation.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "ra8_camera.h"

#include <stddef.h>
#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_camera_internal.h"
#include "ra8_err.h"

/**
 * @brief Return the minimum bytes in one uncompressed row.
 *
 * @details Maps each raw format to its packed row size and rejects compressed,
 *          unknown, or geometrically invalid formats.
 * @param[in]  frame   Frame whose row layout is inspected.
 * @param[out] out_row Receives the minimum packed row size.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok Row size returned.
 * @retval k_ra8_err_invalid_arg Format is unsupported or UYVY width is odd.
 * @pre `frame` and `out_row` are non-NULL.
 * @pre `frame->width` is non-zero.
 * @post On success `out_row` is the packed row size.
 * @post No frame storage is modified.
 * @note This helper does not validate total frame storage.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_frame_row_bytes(const ra8_camera_frame_t* frame,
                                                       uint32_t*                 out_row)
{
  if (frame->format == k_ra8_camera_format_rgb888) {
    *out_row = (uint32_t)frame->width * 3U;
    return k_ra8_ok;
  }
  if (frame->format == k_ra8_camera_format_uyvy422) {
    if ((frame->width & 1U) != 0U) {
      return k_ra8_err_invalid_arg;
    }
    *out_row = (uint32_t)frame->width * 2U;
    return k_ra8_ok;
  }
  return k_ra8_err_invalid_arg;
}

ra8_err_t ra8_camera_frame_validate(const ra8_camera_frame_t* frame)
{
  if (frame == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (frame->data == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (frame->width == 0U) {
    return k_ra8_err_invalid_arg;
  }
  if (frame->height == 0U) {
    return k_ra8_err_invalid_arg;
  }
  if (frame->format == k_ra8_camera_format_jpeg) {
    if (frame->bytes == 0U) {
      return k_ra8_err_invalid_size;
    }
    if (frame->stride_bytes != 0U) {
      return k_ra8_err_invalid_size;
    }
    return k_ra8_ok;
  }
  uint32_t        row_bytes = 0U;
  const ra8_err_t row       = internal_frame_row_bytes(frame, &row_bytes);
  if (row != k_ra8_ok) {
    return row;
  }
  if (frame->stride_bytes < row_bytes) {
    return k_ra8_err_invalid_size;
  }
  if ((uint32_t)frame->height > (UINT32_MAX / frame->stride_bytes)) {
    return k_ra8_err_invalid_size;
  }
  return (frame->bytes >= (frame->stride_bytes * (uint32_t)frame->height)) ? k_ra8_ok
                                                                           : k_ra8_err_invalid_size;
}

/**
 * @brief Validate a source handle and its mandatory vtable rows.
 *
 * @details Centralizes bound-source validation before public dispatch.
 * @param[in] source Candidate source handle.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok Source is ready for dispatch.
 * @retval k_ra8_err_null_ptr Source handle is NULL.
 * @retval k_ra8_err_not_initialized Source or a mandatory row is unbound.
 * @pre No initialization is required.
 * @pre A non-NULL source is readable.
 * @post No source state is modified.
 * @post Success guarantees both mandatory rows are non-NULL.
 * @note Thread-safe because it reads only handle fields.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_source_validate(const ra8_camera_source_t* source)
{
  if (source == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (source->iface == nullptr) {
    return k_ra8_err_not_initialized;
  }
  if (source->iface->get_info == nullptr) {
    return k_ra8_err_not_initialized;
  }
  if (source->iface->capture == nullptr) {
    return k_ra8_err_not_initialized;
  }
  return k_ra8_ok;
}

ra8_err_t ra8_camera_source_get_info(ra8_camera_source_t*      source,
                                     ra8_camera_source_info_t* out_info)
{
  if (out_info == nullptr) {
    return k_ra8_err_null_ptr;
  }
  *out_info              = (ra8_camera_source_info_t){};
  const ra8_err_t handle = internal_source_validate(source);
  if (handle != k_ra8_ok) {
    return handle;
  }
  return source->iface->get_info(source->ctx, out_info);
}

ra8_err_t ra8_camera_source_capture(ra8_camera_source_t*       source,
                                    const ra8_camera_buffer_t* buffer,
                                    ra8_camera_frame_t*        out_frame)
{
  if (buffer == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (out_frame == nullptr) {
    return k_ra8_err_null_ptr;
  }
  *out_frame             = (ra8_camera_frame_t){};
  const ra8_err_t handle = internal_source_validate(source);
  if (handle != k_ra8_ok) {
    return handle;
  }
  if (buffer->data == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (buffer->capacity == 0U) {
    return k_ra8_err_invalid_size;
  }
  const ra8_err_t captured = source->iface->capture(source->ctx, buffer, out_frame);
  if (captured != k_ra8_ok) {
    *out_frame = (ra8_camera_frame_t){};
    return captured;
  }
  if (out_frame->data != buffer->data) {
    *out_frame = (ra8_camera_frame_t){};
    return k_ra8_err_invalid_size;
  }
  if (out_frame->bytes > buffer->capacity) {
    *out_frame = (ra8_camera_frame_t){};
    return k_ra8_err_invalid_size;
  }
  const ra8_err_t valid = ra8_camera_frame_validate(out_frame);
  if (valid != k_ra8_ok) {
    *out_frame = (ra8_camera_frame_t){};
  }
  return valid;
}

ra8_err_t ra8_camera_codec_encode(ra8_camera_codec_t*        codec,
                                  const ra8_camera_frame_t*  input,
                                  const ra8_camera_buffer_t* output_buffer,
                                  ra8_camera_frame_t*        out_frame)
{
  if (codec == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (input == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (output_buffer == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (out_frame == nullptr) {
    return k_ra8_err_null_ptr;
  }
  *out_frame = (ra8_camera_frame_t){};
  if (codec->iface == nullptr) {
    return k_ra8_err_not_initialized;
  }
  if (codec->iface->encode == nullptr) {
    return k_ra8_err_not_initialized;
  }
  const ra8_err_t valid = ra8_camera_frame_validate(input);
  if (valid != k_ra8_ok) {
    return valid;
  }
  const ra8_err_t encoded = codec->iface->encode(codec->ctx, input, output_buffer, out_frame);
  if (encoded != k_ra8_ok) {
    *out_frame = (ra8_camera_frame_t){};
    return encoded;
  }
  const ra8_err_t output_valid = ra8_camera_frame_validate(out_frame);
  if (output_valid != k_ra8_ok) {
    *out_frame = (ra8_camera_frame_t){};
  }
  return output_valid;
}
