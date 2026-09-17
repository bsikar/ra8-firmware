/**
 * @file ra8_camera_stream.c
 * @brief `ra8_camera` encoded-frame to `ra8_io_stream` bridge.
 * @ingroup grp_camera
 *
 * @par Tag
 * [Ring 4 / Service] {World: NS}
 *
 * @details Produces a frame view through the selected codec, then forwards its
 * complete byte slice to the selected stream sink without allocation.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "ra8_camera_stream.h"

#include <stddef.h>
#include <stdint.h>

#include "ra8_camera.h"
#include "ra8_err.h"
#include "ra8_io_stream.h"

ra8_err_t ra8_camera_codec_encode_to_stream(ra8_camera_codec_t*        codec,
                                            const ra8_camera_frame_t*  input,
                                            const ra8_camera_buffer_t* output_buffer,
                                            ra8_io_stream_t*           stream,
                                            uint32_t*                  out_written)
{
  if (stream == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (out_written != nullptr) {
    *out_written = 0U;
  }
  ra8_camera_frame_t encoded   = {};
  const ra8_err_t    codec_err = ra8_camera_codec_encode(codec, input, output_buffer, &encoded);
  if (codec_err != k_ra8_ok) {
    return codec_err;
  }
  uint32_t        written    = 0U;
  const ra8_err_t stream_err = ra8_io_stream_write(stream, encoded.data, encoded.bytes, &written);
  if (out_written != nullptr) {
    *out_written = written;
  }
  if (stream_err != k_ra8_ok) {
    return stream_err;
  }
  return (written == encoded.bytes) ? k_ra8_ok : k_ra8_err_invalid_size;
}
