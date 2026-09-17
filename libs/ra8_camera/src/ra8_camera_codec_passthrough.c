/**
 * @file ra8_camera_codec_passthrough.c
 * @brief Zero-copy JPEG passthrough backend implementation.
 * @ingroup grp_camera
 *
 * @par Tag
 * [Ring 4 / Service] {World: NS}
 *
 * @details The codec row accepts only `k_ra8_camera_format_jpeg` and aliases
 * the validated input view. It never reads or writes the supplied output
 * buffer.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "ra8_camera_codec_passthrough.h"

#include <stddef.h>

#include "ra8_attributes.h"
#include "ra8_camera_internal.h"
#include "ra8_err.h"

/**
 * @brief Return a JPEG input view unchanged.
 *
 * @details Implements zero-copy encoding by aliasing an already-compressed
 *          JPEG frame and ignoring the output buffer.
 * @param[in]  ctx           Unused stateless backend context.
 * @param[in]  input         Valid input frame.
 * @param[in]  output_buffer Unused caller output buffer.
 * @param[out] out_frame     Receives an alias of `input`.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok JPEG view returned.
 * @retval k_ra8_err_null_ptr Required frame pointer is NULL.
 * @retval k_ra8_err_not_supported Input is not JPEG.
 * @pre `input` passed generic frame validation.
 * @pre `out_frame` is writable.
 * @post On success `out_frame->data == input->data`.
 * @post No input or output storage is modified.
 * @note Thread-safe because the backend has no state.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_passthrough_encode(void*                      ctx,
                                                          const ra8_camera_frame_t*  input,
                                                          const ra8_camera_buffer_t* output_buffer,
                                                          ra8_camera_frame_t*        out_frame)
{
  (void)ctx;
  (void)output_buffer;
  if (input == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (out_frame == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (input->format != k_ra8_camera_format_jpeg) {
    return k_ra8_err_not_supported;
  }
  *out_frame = *input;
  return k_ra8_ok;
}

/** @brief Stateless passthrough codec vtable. */
static const ra8_camera_codec_iface_t s_passthrough_codec_iface = {
  .encode = internal_passthrough_encode,
};

ra8_err_t ra8_camera_codec_passthrough_init(ra8_camera_codec_t* codec)
{
  if (codec == nullptr) {
    return k_ra8_err_null_ptr;
  }
  codec->iface = &s_passthrough_codec_iface;
  codec->ctx   = nullptr;
  return k_ra8_ok;
}
