/**
 * @file ra8_camera_stream.h
 * @brief Bridge encoded camera frames to `ra8_io_stream` sinks.
 * @ingroup grp_camera
 *
 * @par Tag
 * [Ring 4 / Service] {World: NS}
 *
 * @details Keeps the base `ra8_camera` facade independent of output transports.
 * Consumers opt into this `ra8_camera_io` bridge plus `ra8_camera` and `ra8_io`,
 * then encode and write one complete frame with a single bounded operation.
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

#include "ra8_camera.h"
#include "ra8_err.h"
#include "ra8_io_stream.h"

/**
 * @brief Encode one frame and write the complete result to a byte stream.
 *
 * @details Calls ::ra8_camera_codec_encode, then one
 * ::ra8_io_stream_write. It deliberately does not flush: callers may append a
 * protocol trailer or batch several frames before committing a buffered sink.
 *
 * @param[in,out] codec         Bound camera codec.
 * @param[in]     input         Valid source frame.
 * @param[in]     output_buffer Caller-owned codec output storage.
 * @param[in,out] stream        Bound output stream.
 * @param[out]    out_written   Bytes accepted by the stream, or NULL.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               Whole encoded frame written.
 * @retval k_ra8_err_null_ptr     A required argument was NULL.
 * @retval k_ra8_err_invalid_size A nominally successful sink accepted only a
 *                                prefix of the encoded frame.
 * @retval other                  Propagated from the codec or stream.
 * @pre Input and output storage satisfy ::ra8_camera_codec_encode.
 * @pre `stream` is bound and idle.
 * @post On success exactly one complete encoded frame was written.
 * @post `*out_written`, when provided, reports the accepted byte count.
 * @note Not thread-safe with respect to the same codec, buffers, or stream.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_camera_codec_encode_to_stream(ra8_camera_codec_t*        codec,
                                                          const ra8_camera_frame_t*  input,
                                                          const ra8_camera_buffer_t* output_buffer,
                                                          ra8_io_stream_t*           stream,
                                                          uint32_t*                  out_written);

#ifdef __cplusplus
}
#endif
