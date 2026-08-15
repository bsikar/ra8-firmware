/**
 * @file ra8_audio.c
 * @brief Validation and dispatch for the transport-neutral audio facade.
 * @ingroup grp_audio
 * @details Enforces PCM geometry and caller-owned-buffer contracts before
 *          dispatching capture, streaming, and stop operations to a backend.
 *
 * @par Tag
 * [Ring 4 / Service] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "ra8_audio.h"

#include <stdint.h>

#include "ra8_audio_internal.h"

/**
 * @brief Return the PCM container width in bytes.
 * @details Maps each supported PCM storage format to its fixed byte width.
 * @param[in] format Audio sample storage format to inspect.
 * @return Container width in bytes.
 * @retval 0 The format is not supported.
 * @pre `format` is a value representable by `ra8_audio_format_t`.
 * @pre No audio source operation is in progress through this helper.
 * @post The supplied format value is unchanged.
 * @post No global or caller-owned state is modified.
 * @note This reports storage width, not the number of valid sample bits.
 * @since 0.1.0
 */
RA8_INTERNAL static uint32_t internal_audio_container_bytes(ra8_audio_format_t format)
{
  switch (format) {
    case k_ra8_audio_format_pcm_s16le:
      return 2U;
    case k_ra8_audio_format_pcm_s32le:
      return 4U;
    default:
      return 0U;
  }
}

ra8_err_t ra8_audio_frame_validate(const ra8_audio_frame_t* frame)
{
  if (frame == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (frame->data == nullptr) {
    return k_ra8_err_null_ptr;
  }
  const uint32_t container_bytes = internal_audio_container_bytes(frame->format);
  if (container_bytes == 0U) {
    return k_ra8_err_invalid_arg;
  }
  if (frame->sample_count == 0U) {
    return k_ra8_err_invalid_arg;
  }
  if (frame->sample_rate_hz == 0U) {
    return k_ra8_err_invalid_arg;
  }
  if (frame->channels == 0U) {
    return k_ra8_err_invalid_arg;
  }
  if (frame->valid_bits == 0U) {
    return k_ra8_err_invalid_arg;
  }
  if (frame->valid_bits > (container_bytes * 8U)) {
    return k_ra8_err_invalid_arg;
  }
  const uint64_t required =
    (uint64_t)frame->sample_count * (uint64_t)frame->channels * (uint64_t)container_bytes;
  if (required > (uint64_t)UINT32_MAX) {
    return k_ra8_err_invalid_size;
  }
  if (frame->bytes != (uint32_t)required) {
    return k_ra8_err_invalid_size;
  }
  return k_ra8_ok;
}

ra8_err_t ra8_audio_source_get_info(ra8_audio_source_t* source, ra8_audio_source_info_t* out_info)
{
  if (source == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (out_info == nullptr) {
    return k_ra8_err_null_ptr;
  }
  *out_info = (ra8_audio_source_info_t){};
  if (source->iface == nullptr) {
    return k_ra8_err_not_initialized;
  }
  if (source->iface->get_info == nullptr) {
    return k_ra8_err_not_initialized;
  }
  return source->iface->get_info(source->ctx, out_info);
}

/**
 * @brief Verify a captured frame matches its source metadata.
 * @details Compares byte size, sample geometry, rate, channel count, valid-bit
 *          width, and format as independent invariants.
 * @param[in] frame Captured frame descriptor.
 * @param[in] info Source metadata returned before capture.
 * @return Repository error code.
 * @retval k_ra8_ok Every frame field matches the source metadata.
 * @retval k_ra8_err_invalid_state At least one field differs.
 * @pre Both pointers address readable descriptors.
 * @pre The frame already passed `ra8_audio_frame_validate`.
 * @post Both descriptors remain unchanged.
 * @post Success proves the source honored its advertised geometry.
 * @note The caller clears rejected output descriptors.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_audio_frame_matches_info(const ra8_audio_frame_t* frame,
                                                                const ra8_audio_source_info_t* info)
{
  if (frame->bytes != info->frame_bytes) {
    return k_ra8_err_invalid_state;
  }
  if (frame->sample_count != info->samples_per_frame) {
    return k_ra8_err_invalid_state;
  }
  if (frame->sample_rate_hz != info->sample_rate_hz) {
    return k_ra8_err_invalid_state;
  }
  if (frame->channels != info->channels) {
    return k_ra8_err_invalid_state;
  }
  if (frame->valid_bits != info->valid_bits) {
    return k_ra8_err_invalid_state;
  }
  return (frame->format == info->format) ? k_ra8_ok : k_ra8_err_invalid_state;
}

ra8_err_t ra8_audio_source_capture(ra8_audio_source_t*       source,
                                   const ra8_audio_buffer_t* buffer,
                                   ra8_audio_frame_t*        out_frame)
{
  if (source == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (buffer == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (out_frame == nullptr) {
    return k_ra8_err_null_ptr;
  }
  *out_frame = (ra8_audio_frame_t){};
  if (source->iface == nullptr) {
    return k_ra8_err_not_initialized;
  }
  if (source->iface->capture == nullptr) {
    return k_ra8_err_not_initialized;
  }
  if (buffer->data == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (buffer->capacity == 0U) {
    return k_ra8_err_null_ptr;
  }
  ra8_audio_source_info_t info = {};
  ra8_err_t               err  = source->iface->get_info(source->ctx, &info);
  if (err != k_ra8_ok) {
    return err;
  }
  if (buffer->capacity < info.frame_bytes) {
    return k_ra8_err_invalid_size;
  }
  err = source->iface->capture(source->ctx, buffer, out_frame);
  if (err != k_ra8_ok) {
    *out_frame = (ra8_audio_frame_t){};
    return err;
  }
  if (out_frame->data != buffer->data) {
    *out_frame = (ra8_audio_frame_t){};
    return k_ra8_err_invalid_state;
  }
  err = ra8_audio_frame_validate(out_frame);
  if (err != k_ra8_ok) {
    *out_frame = (ra8_audio_frame_t){};
    return err;
  }
  err = internal_audio_frame_matches_info(out_frame, &info);
  if (err != k_ra8_ok) {
    *out_frame = (ra8_audio_frame_t){};
    return err;
  }
  return k_ra8_ok;
}

ra8_err_t ra8_audio_source_stream_start(ra8_audio_source_t*        source,
                                        const ra8_audio_buffer_t*  buffer,
                                        ra8_audio_frame_callback_t callback,
                                        void*                      ctx)
{
  if (source == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (buffer == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (callback == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (buffer->data == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (source->iface == nullptr) {
    return k_ra8_err_not_initialized;
  }
  if (source->iface->get_info == nullptr) {
    return k_ra8_err_not_initialized;
  }
  if (source->iface->stream_start == nullptr) {
    return k_ra8_err_not_supported;
  }
  ra8_audio_source_info_t info = {};
  const ra8_err_t         err  = source->iface->get_info(source->ctx, &info);
  if (err != k_ra8_ok) {
    return err;
  }
  if (buffer->capacity < info.frame_bytes) {
    return k_ra8_err_invalid_size;
  }
  return source->iface->stream_start(source->ctx, buffer, callback, ctx);
}

ra8_err_t ra8_audio_source_stop(ra8_audio_source_t* source)
{
  if (source == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (source->iface == nullptr) {
    return k_ra8_err_not_initialized;
  }
  if (source->iface->stop == nullptr) {
    return k_ra8_err_not_initialized;
  }
  const ra8_err_t err = source->iface->stop(source->ctx);
  if (err == k_ra8_ok) {
    *source = (ra8_audio_source_t){};
  }
  return err;
}
