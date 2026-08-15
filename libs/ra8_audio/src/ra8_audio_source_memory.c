/**
 * @file ra8_audio_source_memory.c
 * @brief Fixed PCM replay backend implementation.
 * @ingroup grp_audio
 * @details Copies one immutable caller-owned fixture into each supplied output
 *          buffer and reports its validated PCM metadata without allocation.
 *
 * @par Tag
 * [Ring 4 / Service] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "ra8_audio_source_memory.h"

#include <stddef.h>
#include <string.h>

#include "ra8_audio_internal.h"

/**
 * @brief Report metadata for the fixed memory frame.
 * @details Copies the immutable fixture geometry into the generic source-info structure.
 * @param[in] ctx Memory-source state supplied during initialization.
 * @param[out] out_info Destination for the frame metadata.
 * @return Repository error code.
 * @retval k_ra8_ok Metadata was copied successfully.
 * @retval k_ra8_err_null_ptr A required pointer is null.
 * @pre `ctx` points to an initialized memory-source state.
 * @pre `out_info` points to writable storage.
 * @post On success, `out_info` describes the stored fixture exactly.
 * @post The stored fixture and source state remain unchanged.
 * @note This backend exposes one fixed frame geometry.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_memory_get_info(void* ctx, ra8_audio_source_info_t* out_info)
{
  if (ctx == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (out_info == nullptr) {
    return k_ra8_err_null_ptr;
  }
  const ra8_audio_source_memory_state_t* state = (const ra8_audio_source_memory_state_t*)ctx;
  *out_info                                    = (ra8_audio_source_info_t){
    .frame_bytes       = state->frame.bytes,
    .samples_per_frame = state->frame.sample_count,
    .sample_rate_hz    = state->frame.sample_rate_hz,
    .channels          = state->frame.channels,
    .valid_bits        = state->frame.valid_bits,
    .format            = state->frame.format,
  };
  return k_ra8_ok;
}

/**
 * @brief Copy the fixed memory frame into a caller buffer.
 * @details Replays the configured fixture without allocating or retaining the output buffer.
 * @param[in] ctx Memory-source state supplied during initialization.
 * @param[in] buffer Caller-owned destination buffer descriptor.
 * @param[out] out_frame Descriptor for the copied frame.
 * @return Repository error code.
 * @retval k_ra8_ok The fixture was copied successfully.
 * @retval k_ra8_err_null_ptr A required pointer is null.
 * @retval k_ra8_err_invalid_size The destination is too small.
 * @pre `ctx` points to initialized memory-source state.
 * @pre `buffer->data` points to writable storage of `buffer->capacity` bytes.
 * @post On success, `out_frame->data` aliases `buffer->data`.
 * @post The configured fixture remains unchanged.
 * @note The facade validates the returned descriptor after this callback.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_memory_capture(void* ctx, const ra8_audio_buffer_t* buffer, ra8_audio_frame_t* out_frame)
{
  if (ctx == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (buffer == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (out_frame == nullptr) {
    return k_ra8_err_null_ptr;
  }
  const ra8_audio_source_memory_state_t* state = (const ra8_audio_source_memory_state_t*)ctx;
  if (buffer->capacity < state->frame.bytes) {
    return k_ra8_err_invalid_size;
  }
  (void)memcpy(buffer->data, state->frame.data, (size_t)state->frame.bytes);
  *out_frame      = state->frame;
  out_frame->data = buffer->data;
  return k_ra8_ok;
}

/**
 * @brief Stop the memory replay backend.
 * @details Acknowledges the stop request because the backend owns no active hardware.
 * @param[in] ctx Memory-source state supplied during initialization.
 * @return Repository error code.
 * @retval k_ra8_ok The no-op stop completed.
 * @retval k_ra8_err_null_ptr The state pointer is null.
 * @pre `ctx` points to storage retained by the caller.
 * @pre No concurrent mutation of the source state occurs.
 * @post The state contents remain unchanged.
 * @post No hardware or allocator state is modified.
 * @note The facade clears its source handle after a successful stop.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_memory_stop(void* ctx)
{
  if (ctx == nullptr) {
    return k_ra8_err_null_ptr;
  }
  return k_ra8_ok;
}

static const ra8_audio_source_iface_t s_memory_source_iface = {
  .get_info     = internal_memory_get_info,
  .capture      = internal_memory_capture,
  .stream_start = nullptr,
  .stop         = internal_memory_stop,
};

ra8_err_t ra8_audio_source_memory_init(ra8_audio_source_t*              source,
                                       ra8_audio_source_memory_state_t* state,
                                       const ra8_audio_frame_t*         frame)
{
  if (source == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (state == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (frame == nullptr) {
    return k_ra8_err_null_ptr;
  }
  const ra8_err_t err = ra8_audio_frame_validate(frame);
  if (err != k_ra8_ok) {
    return err;
  }
  state->frame  = *frame;
  source->iface = &s_memory_source_iface;
  source->ctx   = state;
  return k_ra8_ok;
}
