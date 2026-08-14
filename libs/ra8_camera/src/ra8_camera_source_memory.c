/**
 * @file ra8_camera_source_memory.c
 * @brief Fixed-frame memory source backend implementation.
 * @ingroup grp_camera
 *
 * @par Tag
 * [Ring 4 / Service] {World: NS}
 *
 * @details Implements the private source vtable with bounds-checked metadata
 * queries and a byte-for-byte copy into caller-owned capture storage.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "ra8_camera_source_memory.h"

#include <stddef.h>
#include <string.h>

#include "ra8_camera_internal.h"
#include "ra8_err.h"

/**
 * @brief Query the fixed frame's native geometry.
 *
 * @details Copies immutable metadata from the memory-source state.
 * @param[in]  ctx      Bound memory-source state.
 * @param[out] out_info Receives source geometry and capacity.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok Information returned.
 * @retval k_ra8_err_null_ptr State or output pointer is NULL.
 * @pre `ctx` was populated by ::ra8_camera_source_memory_init.
 * @pre `out_info` is writable.
 * @post On success `out_info` describes the fixed frame.
 * @post Source state is unchanged.
 * @note Thread-safe because the state is read-only after initialization.
 * @since 0.1.0
 */
static ra8_err_t memory_get_info(void* ctx, ra8_camera_source_info_t* out_info)
{
  if (ctx == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (out_info == nullptr) {
    return k_ra8_err_null_ptr;
  }
  const ra8_camera_source_memory_state_t* state = (const ra8_camera_source_memory_state_t*)ctx;
  *out_info                                     = (ra8_camera_source_info_t){
    .frame_bytes_max = state->frame.bytes,
    .stride_bytes    = state->frame.stride_bytes,
    .width           = state->frame.width,
    .height          = state->frame.height,
    .format          = state->frame.format,
  };
  return k_ra8_ok;
}

/**
 * @brief Copy the fixed frame into the caller's capture buffer.
 *
 * @details Implements source capture as a bounded byte-for-byte copy.
 * @param[in]  ctx       Bound memory-source state.
 * @param[in]  buffer    Caller-owned writable capture storage.
 * @param[out] out_frame Receives a view of copied bytes.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok Frame copied.
 * @retval k_ra8_err_null_ptr State, buffer, or output pointer is NULL.
 * @retval k_ra8_err_invalid_size Buffer cannot hold the fixed frame.
 * @pre `ctx` was populated by ::ra8_camera_source_memory_init.
 * @pre `buffer->data` covers `buffer->capacity` writable bytes.
 * @post On success `out_frame->data == buffer->data`.
 * @post Source state is unchanged.
 * @note Not thread-safe with respect to the capture buffer.
 * @since 0.1.0
 */
static ra8_err_t
memory_capture(void* ctx, const ra8_camera_buffer_t* buffer, ra8_camera_frame_t* out_frame)
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
  const ra8_camera_source_memory_state_t* state = (const ra8_camera_source_memory_state_t*)ctx;
  if (buffer->capacity < state->frame.bytes) {
    return k_ra8_err_invalid_size;
  }
  (void)memcpy(buffer->data, state->frame.data, (size_t)state->frame.bytes);
  *out_frame      = state->frame;
  out_frame->data = buffer->data;
  return k_ra8_ok;
}

/** @brief Fixed-frame source vtable. */
static const ra8_camera_source_iface_t k_memory_source_iface = {
  .get_info = memory_get_info,
  .capture  = memory_capture,
};

ra8_err_t ra8_camera_source_memory_init(ra8_camera_source_t*              source,
                                        ra8_camera_source_memory_state_t* state,
                                        const ra8_camera_frame_t*         frame)
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
  const ra8_err_t valid = ra8_camera_frame_validate(frame);
  if (valid != k_ra8_ok) {
    return valid;
  }
  state->frame  = *frame;
  source->iface = &k_memory_source_iface;
  source->ctx   = state;
  return k_ra8_ok;
}
