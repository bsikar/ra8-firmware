/**
 * @file ra8_audio_source_pdm.c
 * @brief RA8 PDM-IF audio-source backend implementation.
 * @ingroup grp_audio
 * @details Adapts bounded PDM polling and interrupt streams to the generic audio
 *          facade while retaining all buffers and mutable state with callers.
 *
 * @par Tag
 * [Ring 4 / Service] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "ra8_audio_source_pdm.h"

#include <stdint.h>
#include <string.h>

#include "ra8_audio_internal.h"
#include "ra8_time.h"

typedef enum : uintptr_t {
  k_pdm_source_alignment_mask = 3U, /**< PCM-S32LE buffer alignment mask. */
} ra8_audio_pdm_alignment_t;

/**
 * @brief Fill a sample span from the PDM FIFO.
 * @details Performs a bounded number of HAL reads until the requested count is collected.
 * @param[in] channel PDM hardware channel.
 * @param[out] samples Caller-owned destination sample span.
 * @param[in] count Number of samples required.
 * @param[in] attempts Maximum FIFO-read attempts.
 * @return Repository error code.
 * @retval k_ra8_ok The requested sample count was collected.
 * @retval k_ra8_err_hw_timeout The attempt budget expired first.
 * @pre `samples` addresses writable storage for `count` samples.
 * @pre `attempts` is nonzero for a useful acquisition attempt.
 * @post On success, all `count` sample positions are initialized.
 * @post No samples beyond the requested span are written.
 * @note HAL read errors are returned unchanged.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_pdm_fill(uint8_t channel, int32_t* samples, uint32_t count, uint32_t attempts)
{
  uint32_t filled = 0U;
  for (uint32_t attempt = 0U; attempt < attempts; attempt += 1U) {
    if (filled == count) {
      return k_ra8_ok;
    }
    uint32_t  got = 0U;
    ra8_err_t err = ra8_pdm_read(channel, &samples[filled], count - filled, &got);
    if (err != k_ra8_ok) {
      return err;
    }
    filled += got;
  }
  return k_ra8_err_hw_timeout;
}

/**
 * @brief Report the initialized PDM source geometry.
 * @details Copies cached configuration metadata without accessing PDM hardware.
 * @param[in] ctx PDM source state supplied during initialization.
 * @param[out] out_info Destination for audio source metadata.
 * @return Repository error code.
 * @retval k_ra8_ok Metadata was copied successfully.
 * @retval k_ra8_err_not_initialized The source state is absent or inactive.
 * @pre `ctx` is null or points to a PDM source-state object.
 * @pre `out_info` points to writable storage when the source is initialized.
 * @post On success, `out_info` equals the cached source information.
 * @post PDM hardware and state remain unchanged.
 * @note The facade supplies a nonnull output pointer.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_pdm_get_info(void* ctx, ra8_audio_source_info_t* out_info)
{
  const ra8_audio_source_pdm_state_t* state = (const ra8_audio_source_pdm_state_t*)ctx;
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
 * @brief Assemble FIFO chunks into the caller's fixed-size audio frame.
 * @details Copies each interrupt span into persistent caller-owned assembly
 *          storage and publishes frames whenever the configured count is met.
 * @param[in,out] ctx PDM source state.
 * @param[in] samples Borrowed signed samples from the HAL.
 * @param[in] count Sample count in `samples`.
 * @pre Called only while the source stream is active.
 * @pre `samples` addresses at least `count` readable samples.
 * @post Every complete configured frame invokes the caller callback once.
 * @post Partial trailing samples remain buffered for the next interrupt.
 * @note Runs in interrupt context and performs only bounded copies.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_pdm_stream_data(void* ctx, const int32_t* samples, uint32_t count)
{
  ra8_audio_source_pdm_state_t* state = (ra8_audio_source_pdm_state_t*)ctx;
  if (state == nullptr) {
    return;
  }
  if (!state->streaming) {
    return;
  }
  uint32_t consumed = 0U;
  while (consumed < count) {
    if (state->stream_filled == 0U) {
      state->stream_timestamp_ms = ra8_time_ms();
    }
    const uint32_t remaining = state->info.samples_per_frame - state->stream_filled;
    const uint32_t available = count - consumed;
    const uint32_t copy      = (available < remaining) ? available : remaining;
    int32_t*       output    = (int32_t*)state->stream_data;
    (void)memcpy(&output[state->stream_filled], &samples[consumed], copy * sizeof(int32_t));
    state->stream_filled += copy;
    consumed += copy;
    if (state->stream_filled == state->info.samples_per_frame) {
      const ra8_audio_frame_t frame = {
        .data           = state->stream_data,
        .bytes          = state->info.frame_bytes,
        .sample_count   = state->info.samples_per_frame,
        .sample_rate_hz = state->info.sample_rate_hz,
        .timestamp_ms   = state->stream_timestamp_ms,
        .channels       = state->info.channels,
        .valid_bits     = state->info.valid_bits,
        .format         = state->info.format,
      };
      state->stream_filled = 0U;
      state->stream_callback(state->stream_ctx, &frame);
    }
  }
}

/**
 * @brief Capture one fixed-size PDM frame by polling.
 * @details Fills the caller buffer and publishes the configured PCM geometry and timestamp.
 * @param[in,out] ctx Initialized PDM source state.
 * @param[in] buffer Caller-owned aligned output buffer.
 * @param[out] out_frame Descriptor for the captured frame.
 * @return Repository error code.
 * @retval k_ra8_ok One complete frame was captured.
 * @retval k_ra8_err_not_initialized The source is not initialized.
 * @retval k_ra8_err_invalid_size The output buffer is too small.
 * @retval k_ra8_err_invalid_arg The output buffer is misaligned.
 * @pre `buffer->data` remains writable for the duration of capture.
 * @pre The PDM channel was started and read-enabled during initialization.
 * @post On success, `out_frame->data` aliases the caller buffer.
 * @post On failure, no successful frame descriptor is published.
 * @note FIFO and timeout errors are propagated from `internal_pdm_fill`.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_pdm_capture(void* ctx, const ra8_audio_buffer_t* buffer, ra8_audio_frame_t* out_frame)
{
  ra8_audio_source_pdm_state_t* state = (ra8_audio_source_pdm_state_t*)ctx;
  if (state == nullptr) {
    return k_ra8_err_not_initialized;
  }
  if (!state->initialized) {
    return k_ra8_err_not_initialized;
  }
  if (buffer->capacity < state->info.frame_bytes) {
    return k_ra8_err_invalid_size;
  }
  if (((uintptr_t)buffer->data & (uintptr_t)k_pdm_source_alignment_mask) != 0U) {
    return k_ra8_err_invalid_arg;
  }
  const uint32_t  timestamp_ms = ra8_time_ms();
  const ra8_err_t err          = internal_pdm_fill(state->channel,
                                                   (int32_t*)buffer->data,
                                                   state->info.samples_per_frame,
                                                   state->poll_attempts);
  if (err != k_ra8_ok) {
    return err;
  }
  *out_frame = (ra8_audio_frame_t){
    .data           = buffer->data,
    .bytes          = state->info.frame_bytes,
    .sample_count   = state->info.samples_per_frame,
    .sample_rate_hz = state->info.sample_rate_hz,
    .timestamp_ms   = timestamp_ms,
    .channels       = state->info.channels,
    .valid_bits     = state->info.valid_bits,
    .format         = state->info.format,
  };
  return k_ra8_ok;
}

/**
 * @brief Stop and deinitialize the PDM source.
 * @details Stops acquisition before releasing the shared PDM peripheral state.
 * @param[in,out] ctx Initialized PDM source state.
 * @return Repository error code.
 * @retval k_ra8_ok Hardware stopped and deinitialized successfully.
 * @retval k_ra8_err_not_initialized The source is absent or inactive.
 * @pre `ctx` is null or points to retained PDM source state.
 * @pre No caller starts a new stream concurrently.
 * @post On success, the state is marked uninitialized.
 * @post On stop failure, deinitialization is not attempted.
 * @note HAL errors are returned unchanged.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_pdm_stop(void* ctx)
{
  ra8_audio_source_pdm_state_t* state = (ra8_audio_source_pdm_state_t*)ctx;
  if (state == nullptr) {
    return k_ra8_err_not_initialized;
  }
  if (!state->initialized) {
    return k_ra8_err_not_initialized;
  }
  const ra8_err_t stop_err = ra8_pdm_stop(state->channel);
  if (stop_err != k_ra8_ok) {
    return stop_err;
  }
  const ra8_err_t deinit_err = ra8_pdm_deinit();
  if (deinit_err == k_ra8_ok) {
    state->initialized = false;
  }
  return deinit_err;
}

/**
 * @brief Bind persistent scratch and enable PDM FIFO interrupts.
 * @details Records the callback context and delegates bounded FIFO delivery to the HAL.
 * @param[in,out] ctx Initialized PDM source state.
 * @param[in] buffer Caller-owned aligned frame assembly buffer.
 * @param[in] callback Function invoked for each complete audio frame.
 * @param[in,out] callback_ctx Opaque context passed to `callback`.
 * @return Repository error code.
 * @retval k_ra8_ok Interrupt streaming started.
 * @retval k_ra8_err_not_initialized The source is absent or inactive.
 * @retval k_ra8_err_exists Streaming is already active.
 * @retval k_ra8_err_invalid_arg The assembly buffer is misaligned.
 * @pre `buffer->data` remains writable until the source is stopped.
 * @pre `callback` remains callable until the source is stopped.
 * @post On success, the source is marked streaming.
 * @post On failure, retained stream pointers are cleared.
 * @note The callback executes from the PDM interrupt path.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_pdm_stream_start(void*                      ctx,
                                                        const ra8_audio_buffer_t*  buffer,
                                                        ra8_audio_frame_callback_t callback,
                                                        void*                      callback_ctx)
{
  ra8_audio_source_pdm_state_t* state = (ra8_audio_source_pdm_state_t*)ctx;
  if (state == nullptr) {
    return k_ra8_err_not_initialized;
  }
  if (!state->initialized) {
    return k_ra8_err_not_initialized;
  }
  if (state->streaming) {
    return k_ra8_err_exists;
  }
  if (((uintptr_t)buffer->data & (uintptr_t)k_pdm_source_alignment_mask) != 0U) {
    return k_ra8_err_invalid_arg;
  }
  state->stream_data         = buffer->data;
  state->stream_callback     = callback;
  state->stream_ctx          = callback_ctx;
  state->stream_filled       = 0U;
  state->stream_timestamp_ms = 0U;
  state->streaming           = true;
  const ra8_err_t err =
    ra8_pdm_stream_enable(state->channel, internal_pdm_stream_data, state, state->irq_priority);
  if (err != k_ra8_ok) {
    state->streaming       = false;
    state->stream_data     = nullptr;
    state->stream_callback = nullptr;
    state->stream_ctx      = nullptr;
  }
  return err;
}

static const ra8_audio_source_iface_t s_pdm_source_iface = {
  .get_info     = internal_pdm_get_info,
  .capture      = internal_pdm_capture,
  .stream_start = internal_pdm_stream_start,
  .stop         = internal_pdm_stop,
};

/**
 * @brief Validate PDM source geometry and compute its frame size.
 * @details Checks every bounded configuration field and rejects a frame-size
 *          multiplication that cannot fit the public 32-bit byte count.
 * @param[in] cfg Candidate source configuration.
 * @param[out] out_frame_bytes Computed PCM frame size on success.
 * @return Repository error code.
 * @retval k_ra8_ok Configuration geometry is valid.
 * @retval k_ra8_err_invalid_arg One bounded configuration field is invalid.
 * @retval k_ra8_err_invalid_size The frame byte count exceeds 32 bits.
 * @pre `cfg` points to a readable configuration.
 * @pre `out_frame_bytes` points to writable storage.
 * @post On success `out_frame_bytes` contains the exact S32LE frame size.
 * @post The supplied configuration remains unchanged.
 * @note This helper does not access PDM hardware.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_pdm_validate_cfg(const ra8_audio_source_pdm_cfg_t* cfg,
                                                        uint32_t* out_frame_bytes)
{
  if (cfg->channel >= (uint8_t)k_ra8_pdm_ch_count) {
    return k_ra8_err_invalid_arg;
  }
  if (cfg->sample_rate_hz == 0U) {
    return k_ra8_err_invalid_arg;
  }
  if (cfg->samples_per_frame == 0U) {
    return k_ra8_err_invalid_arg;
  }
  if (cfg->poll_attempts == 0U) {
    return k_ra8_err_invalid_arg;
  }
  if (cfg->valid_bits == 0U) {
    return k_ra8_err_invalid_arg;
  }
  if (cfg->valid_bits > 32U) {
    return k_ra8_err_invalid_arg;
  }
  const uint64_t frame_bytes = (uint64_t)cfg->samples_per_frame * sizeof(int32_t);
  if (frame_bytes > UINT32_MAX) {
    return k_ra8_err_invalid_size;
  }
  *out_frame_bytes = (uint32_t)frame_bytes;
  return k_ra8_ok;
}

/**
 * @brief Initialize, settle, and prime one PDM hardware channel.
 * @details Initializes the HAL, applies channel configuration, starts capture,
 *          enables FIFO reads, and discards the requested settling samples.
 * @param[in] cfg Validated source configuration.
 * @return Repository error code.
 * @retval k_ra8_ok The PDM channel is running and ready for capture.
 * @retval other Propagated HAL or bounded-read error.
 * @pre `cfg` passed `internal_pdm_validate_cfg`.
 * @pre No other owner is using the selected PDM channel.
 * @post On success the selected channel remains started with reads enabled.
 * @post On failure initialized PDM hardware is stopped and deinitialized.
 * @note Discard reads use the caller-provided bounded polling budget.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_pdm_prepare_hardware(const ra8_audio_source_pdm_cfg_t* cfg)
{
  ra8_err_t err = ra8_pdm_init();
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_pdm_configure(cfg->channel, &cfg->pdm);
  if (err == k_ra8_ok) {
    err = ra8_pdm_start(cfg->channel);
  }
  if (err != k_ra8_ok) {
    (void)ra8_pdm_deinit();
    return err;
  }
  ra8_delay_ms(cfg->settle_ms);
  err             = ra8_pdm_read_enable(cfg->channel);
  int32_t discard = 0;
  for (uint32_t i = 0U; i < cfg->discard_samples; i += 1U) {
    if (err != k_ra8_ok) {
      break;
    }
    err = internal_pdm_fill(cfg->channel, &discard, 1U, cfg->poll_attempts);
  }
  if (err != k_ra8_ok) {
    (void)ra8_pdm_stop(cfg->channel);
    (void)ra8_pdm_deinit();
  }
  return err;
}

ra8_err_t ra8_audio_source_pdm_init(ra8_audio_source_t*               source,
                                    ra8_audio_source_pdm_state_t*     state,
                                    const ra8_audio_source_pdm_cfg_t* cfg)
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
  *source               = (ra8_audio_source_t){};
  *state                = (ra8_audio_source_pdm_state_t){};
  uint32_t  frame_bytes = 0U;
  ra8_err_t err         = internal_pdm_validate_cfg(cfg, &frame_bytes);
  if (err != k_ra8_ok) {
    return err;
  }
  err = internal_pdm_prepare_hardware(cfg);
  if (err != k_ra8_ok) {
    return err;
  }
  *state = (ra8_audio_source_pdm_state_t){
    .info =
      {
        .frame_bytes       = frame_bytes,
        .samples_per_frame = cfg->samples_per_frame,
        .sample_rate_hz    = cfg->sample_rate_hz,
        .channels          = 1U,
        .valid_bits        = cfg->valid_bits,
        .format            = k_ra8_audio_format_pcm_s32le,
      },
    .poll_attempts = cfg->poll_attempts,
    .channel       = cfg->channel,
    .irq_priority  = cfg->irq_priority,
    .initialized   = true,
  };
  *source = (ra8_audio_source_t){
    .iface = &s_pdm_source_iface,
    .ctx   = state,
  };
  return k_ra8_ok;
}
