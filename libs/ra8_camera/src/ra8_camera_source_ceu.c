/**
 * @file ra8_camera_source_ceu.c
 * @brief RA8 CEU implementation of the camera-source backend.
 * @ingroup grp_camera
 * @details Performs bounded CEU capture and cache maintenance into caller-owned
 *          storage while retaining diagnostic event state for HIL reporting.
 *
 * @par Tag
 * [Ring 4 / Service] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "ra8_camera_source_ceu.h"

#include <stdint.h>

#include "ra8_cache.h"
#include "ra8_camera_internal.h"
#include "ra8_ceu.h"
#include "ra8_err.h"
#include "ra8_time.h"

typedef enum : uintptr_t {
  k_ceu_buffer_alignment_mask = 7U, /**< Eight-byte CEU buffer alignment mask. */
} ra8_camera_ceu_alignment_t;

/** @brief CEU faults that make a pending capture unusable immediately. */
typedef enum : uint32_t {
  k_ceu_fatal_events = (k_ra8_ceu_evt_igrw | k_ra8_ceu_evt_cram_overflow | k_ra8_ceu_evt_vd_error |
                        k_ra8_ceu_evt_firewall), /**< Events that invalidate captured storage. */
} ra8_camera_ceu_event_t;

/**
 * @brief Return validated CEU source metadata.
 * @details Copies immutable metadata from initialized caller-owned state.
 * @param[in] ctx Bound CEU backend state.
 * @param[out] out_info Destination metadata structure.
 * @return Error code.
 * @retval k_ra8_ok Metadata was copied.
 * @retval k_ra8_err_not_initialized Backend state is absent or uninitialized.
 * @pre @p out_info points to writable storage.
 * @pre No concurrent operation mutates @p ctx.
 * @post On success @p out_info equals the configured source metadata.
 * @post CEU hardware and backend state are unchanged.
 * @note Thread-safe only while the caller prevents concurrent reinitialization.
 * @since 0.1.0
 */
static ra8_err_t ceu_get_info(void* ctx, ra8_camera_source_info_t* out_info)
{
  const ra8_camera_source_ceu_state_t* state = (const ra8_camera_source_ceu_state_t*)ctx;
  if (state == nullptr) {
    return k_ra8_err_not_initialized;
  }
  if (!state->initialized) {
    return k_ra8_err_not_initialized;
  }
  *out_info = state->info;
  return k_ra8_ok;
}

/**
 * @brief Poll bounded CEU status until completion, error, or timeout.
 * @details Accumulates diagnostics while waiting for completion. Sync-timing
 * events can precede a valid CPE and therefore do not terminate the capture;
 * illegal writes, CRAM overflow, and firewall faults terminate immediately.
 * A data-enable transfer whose CDSSR is zero returns the configured capacity
 * so a format-aware caller can locate its own end marker within that bound.
 * @param[in,out] state Initialized caller-owned CEU backend state.
 * @param[out] out_bytes Captured byte count on completion.
 * @return Error code.
 * @retval k_ra8_ok A complete frame was observed.
 * @retval k_ra8_err_hw_error A CEU error event was observed.
 * @retval k_ra8_err_hw_timeout The bounded poll expired.
 * @retval other Propagated CEU status error.
 * @pre A capture is armed and @p state is initialized.
 * @pre @p out_bytes points to writable storage.
 * @post @p state retains the last observed raw event bits.
 * @post On error or timeout an in-flight capture is software-reset.
 * @note Blocking and not thread-safe with respect to the CEU.
 * @since 0.1.0
 */
static ra8_err_t ceu_wait_for_frame(ra8_camera_source_ceu_state_t* state, uint32_t* out_bytes)
{
  for (uint32_t attempt = 0U; attempt < state->poll_attempts; attempt += 1U) {
    ra8_ceu_status_t status = {};
    ra8_err_t        err    = ra8_ceu_status_snapshot(&status);
    if (err != k_ra8_ok) {
      return err;
    }
    state->last_events |= status.events;
    if ((status.events & (uint32_t)k_ceu_fatal_events) != 0U) {
      (void)ra8_ceu_clear_status(status.events);
      const ra8_err_t reset_err = ra8_ceu_reset();
      (void)ra8_ceu_clear_status((uint32_t)k_ra8_ceu_evt_mask_all);
      if (reset_err != k_ra8_ok) {
        return reset_err;
      }
      return k_ra8_err_hw_error;
    }
    if ((status.events & (uint32_t)k_ra8_ceu_evt_cpe) != 0U) {
      *out_bytes = state->info.frame_bytes_max;
      if (state->capture_format == k_ra8_ceu_fmt_data_enable) {
        if (status.data_size != 0U) {
          *out_bytes = status.data_size;
        }
      }
      (void)ra8_ceu_clear_status(state->last_events);
      return k_ra8_ok;
    }
    ra8_delay_ms(state->poll_interval_ms);
  }
  const ra8_err_t reset_err = ra8_ceu_reset();
  (void)ra8_ceu_clear_status((uint32_t)k_ra8_ceu_evt_mask_all);
  if (reset_err != k_ra8_ok) {
    return reset_err;
  }
  return k_ra8_err_hw_timeout;
}

/**
 * @brief Capture one CEU frame into caller-owned storage.
 * @details Performs cache maintenance, arms CEU, waits, and returns a frame view.
 * @param[in,out] ctx Bound CEU backend state.
 * @param[in] buffer Caller-owned aligned capture storage.
 * @param[out] out_frame Completed immutable frame view.
 * @return Error code.
 * @retval k_ra8_ok One complete frame was captured.
 * @retval k_ra8_err_not_initialized Backend state is not initialized.
 * @retval k_ra8_err_invalid_size Capture storage or byte count is invalid.
 * @retval k_ra8_err_invalid_arg Capture storage is not eight-byte aligned.
 * @retval other Propagated cache or CEU error.
 * @pre @p buffer and @p out_frame were validated by the camera facade.
 * @pre No other operation uses the CEU or supplied buffer.
 * @post On success @p out_frame views coherent bytes in @p buffer.
 * @post On failure no successful frame view is reported by the facade.
 * @note Not thread-safe with respect to the CEU, state, or buffer.
 * @since 0.1.0
 */
static ra8_err_t
ceu_capture(void* ctx, const ra8_camera_buffer_t* buffer, ra8_camera_frame_t* out_frame)
{
  ra8_camera_source_ceu_state_t* state = (ra8_camera_source_ceu_state_t*)ctx;
  if (state == nullptr) {
    return k_ra8_err_not_initialized;
  }
  if (!state->initialized) {
    return k_ra8_err_not_initialized;
  }
  if (buffer->capacity < state->info.frame_bytes_max) {
    return k_ra8_err_invalid_size;
  }
  if (((uintptr_t)buffer->data & (uintptr_t)k_ceu_buffer_alignment_mask) != 0U) {
    return k_ra8_err_invalid_arg;
  }
  state->last_events = 0U;
  ra8_err_t err =
    ra8_cache_dcache_clean_invalidate_by_addr(buffer->data, state->info.frame_bytes_max);
  if (err != k_ra8_ok) {
    return err;
  }
  const ra8_ceu_buffers_t buffers = {
    .y_top = buffer->data,
  };
  err = ra8_ceu_capture_start_ex(&buffers);
  if (err != k_ra8_ok) {
    return err;
  }
  uint32_t captured_bytes = 0U;
  err                     = ceu_wait_for_frame(state, &captured_bytes);
  if (err != k_ra8_ok) {
    return err;
  }
  if (captured_bytes == 0U) {
    return k_ra8_err_invalid_size;
  }
  if (captured_bytes > buffer->capacity) {
    return k_ra8_err_invalid_size;
  }
  err = ra8_cache_dcache_invalidate_by_addr(buffer->data, captured_bytes);
  if (err != k_ra8_ok) {
    return err;
  }
  *out_frame = (ra8_camera_frame_t){
    .data         = buffer->data,
    .bytes        = captured_bytes,
    .stride_bytes = state->info.stride_bytes,
    .width        = state->info.width,
    .height       = state->info.height,
    .format       = state->info.format,
  };
  return k_ra8_ok;
}

/* See the public header for the documented contract. */
ra8_err_t ra8_camera_source_ceu_get_last_events(const ra8_camera_source_ceu_state_t* state,
                                                uint32_t*                            out_events)
{
  if (state == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (out_events == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (!state->initialized) {
    *out_events = 0U;
    return k_ra8_err_not_initialized;
  }
  *out_events = state->last_events;
  return k_ra8_ok;
}

static const ra8_camera_source_iface_t k_ceu_source_iface = {
  .get_info = ceu_get_info,
  .capture  = ceu_capture,
};

/* See the public header for the documented contract. */
ra8_err_t ra8_camera_source_ceu_init(ra8_camera_source_t*               source,
                                     ra8_camera_source_ceu_state_t*     state,
                                     const ra8_camera_source_ceu_cfg_t* cfg)
{
  if (source == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (state == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (cfg == nullptr) {
    return k_ra8_err_null_ptr;
  }
  *source = (ra8_camera_source_t){};
  *state  = (ra8_camera_source_ceu_state_t){};
  if (cfg->output.frame_bytes_max == 0U) {
    return k_ra8_err_invalid_arg;
  }
  if (cfg->output.width == 0U) {
    return k_ra8_err_invalid_arg;
  }
  if (cfg->output.height == 0U) {
    return k_ra8_err_invalid_arg;
  }
  if (cfg->poll_interval_ms == 0U) {
    return k_ra8_err_invalid_arg;
  }
  if (cfg->poll_attempts == 0U) {
    return k_ra8_err_invalid_arg;
  }
  if ((cfg->ceu.capture_format == k_ra8_ceu_fmt_data_enable) !=
      (cfg->output.format == k_ra8_camera_format_jpeg)) {
    return k_ra8_err_invalid_arg;
  }
  if (cfg->output.format == k_ra8_camera_format_jpeg) {
    if (cfg->output.stride_bytes != 0U) {
      return k_ra8_err_invalid_arg;
    }
    if (cfg->ceu.image_area_size != cfg->output.frame_bytes_max) {
      return k_ra8_err_invalid_arg;
    }
  }
  ra8_err_t err = ra8_ceu_init(&cfg->ceu);
  if (err != k_ra8_ok) {
    return err;
  }
  *state = (ra8_camera_source_ceu_state_t){
    .info             = cfg->output,
    .capture_format   = cfg->ceu.capture_format,
    .poll_interval_ms = cfg->poll_interval_ms,
    .poll_attempts    = cfg->poll_attempts,
    .initialized      = true,
  };
  *source = (ra8_camera_source_t){
    .iface = &k_ceu_source_iface,
    .ctx   = state,
  };
  return k_ra8_ok;
}
