/**
 * @file examples/ek_ra8d2/common/c6_camera_server/src/c6_cam_audio.c
 * @brief Interrupt-driven onboard PDM microphone recorder for camera servers.
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details The PDM-IF hardware continuously decimates MIC1 into 20-bit PCM.
 * The generic `ra8_audio` PDM backend assembles fixed frames in interrupt
 * context, then this module appends them to a caller-owned one-second SDRAM
 * bank. HTTP requests atomically hand capture to the other SDRAM bank and
 * expose the stable bank as PCM-S16LE WAV without stopping the PDM source,
 * camera capture, or allocating memory.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "c6_camera_server.h"
#include "ra8_attributes.h"
#include "ra8_audio.h"
#include "ra8_audio_source_pdm.h"
#include "ra8_board_ek_ra8d2.h"
#include "ra8_err.h"
#include "ra8_register_guard.h"

/** @brief Audio capture, storage, and WAV constants. */
typedef enum : uint32_t {
  k_c6_cam_audio_rate_hz       = 16000U,  /**< Mono PCM sample rate.             */
  k_c6_cam_audio_frame_samples = 256U,    /**< Samples assembled per IRQ frame.  */
  k_c6_cam_audio_ring_samples  = 16000U,  /**< One-second rolling sample window. */
  k_c6_cam_audio_settle_ms     = 300U,    /**< Microphone/filter settling delay. */
  k_c6_cam_audio_discard       = 1024U,   /**< Startup transient samples.        */
  k_c6_cam_audio_poll_attempts = 500000U, /**< Initialization polling bound.     */
  k_c6_cam_audio_irq_priority  = 5U,      /**< PDM FIFO NVIC priority.           */
  k_c6_cam_audio_bank_count    = 2U,      /**< Ping-pong SDRAM capture banks.    */
  k_c6_cam_audio_gain          = 16U,     /**< WAV digital gain before scaling.  */
  k_c6_cam_wav_header_bytes    = 44U,     /**< Canonical PCM WAV header size.    */
  k_c6_cam_wav_sample_bytes    = 2U,      /**< PCM-S16LE bytes per mono sample.  */
  k_c6_cam_wav_capacity =
    k_c6_cam_wav_header_bytes +
    (k_c6_cam_audio_ring_samples * k_c6_cam_wav_sample_bytes), /**< Complete WAV capacity. */
  k_c6_cam_byte_mask            = 0xFFU, /**< One serialized byte mask.         */
  k_c6_cam_shift_24             = 24U,   /**< Most-significant byte shift.      */
  k_c6_cam_ms_per_second        = 1000U, /**< Milliseconds per second.          */
  k_c6_cam_wav_riff_overhead    = 36U,   /**< RIFF size excluding 8-byte lead.  */
  k_c6_cam_wav_audio_format_off = 20U,   /**< WAV audio-format field offset.    */
  k_c6_cam_wav_channels_off     = 22U,   /**< WAV channel-count field offset.   */
  k_c6_cam_wav_byte_rate_off    = 28U,   /**< WAV byte-rate field offset.       */
  k_c6_cam_wav_bits_off         = 34U,   /**< WAV bits-per-sample field offset. */
  k_c6_cam_wav_data_size_off    = 40U,   /**< WAV data-size field offset.       */
} c6_cam_audio_cfg_t;

/** @brief Writer position and timing metadata for one SDRAM audio bank. */
typedef struct {
  uint32_t write_index;         /**< Next sample index written by the ISR.     */
  uint32_t sample_count;        /**< Valid samples currently retained.         */
  uint32_t oldest_timestamp_ms; /**< Monotonic timestamp of the oldest sample. */
} c6_cam_audio_bank_t;

/** @brief Fixed audio source objects and interrupt assembly scratch. */
static ra8_audio_source_t           s_audio_source;
static ra8_audio_source_pdm_state_t s_audio_state;
alignas(4) static int32_t s_audio_frame[k_c6_cam_audio_frame_samples];

/** @brief Ping-pong PCM rings and generated WAV live in external SDRAM. */
[[gnu::section(".sdram_data"),
  gnu::aligned(
    32)]] static int32_t s_audio_ring[k_c6_cam_audio_bank_count][k_c6_cam_audio_ring_samples];
[[gnu::section(".sdram_data"), gnu::aligned(8)]] static uint8_t s_audio_wav[k_c6_cam_wav_capacity];

/** @brief Active ISR bank and per-bank metadata guarded during handoff. */
static volatile uint8_t    s_audio_active_bank;
static c6_cam_audio_bank_t s_audio_banks[k_c6_cam_audio_bank_count];

/**
 * @brief The three fixed-width chunk tags of a canonical PCM WAV header.
 * @details Byte arrays rather than C strings, because that is what the file
 *          format is: each tag is followed immediately by a binary length or
 *          format field, so the header carries no terminator after any of
 *          them and copying one would corrupt the field that follows.
 * @note Read-only; only ::internal_c6_cam_audio_write_header copies them.
 * @since 0.1.0
 */
static const uint8_t s_wav_tag_riff[]    = {'R', 'I', 'F', 'F'};
static const uint8_t s_wav_tag_wavefmt[] = {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '};
static const uint8_t s_wav_tag_data[]    = {'d', 'a', 't', 'a'};

/**
 * @brief Write one little-endian 16-bit scalar.
 * @details Splits the value into two wire-order WAV bytes.
 * @param[out] out Destination for two bytes.
 * @param[in] value Scalar to serialize.
 * @pre `out` addresses at least two writable bytes.
 * @pre No concurrent writer modifies the same destination bytes.
 * @post `out[0]` contains the least-significant byte.
 * @post `out[1]` contains the most-significant byte.
 * @note The helper performs no alignment-dependent stores.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_c6_cam_audio_put_u16(uint8_t* out, uint16_t value)
{
  out[0] = (uint8_t)(value & (uint16_t)k_c6_cam_byte_mask);
  out[1] = (uint8_t)(value >> 8U);
}

/**
 * @brief Write one little-endian 32-bit scalar.
 * @details Splits the value into four wire-order WAV bytes.
 * @param[out] out Destination for four bytes.
 * @param[in] value Scalar to serialize.
 * @pre `out` addresses at least four writable bytes.
 * @pre No concurrent writer modifies the same destination bytes.
 * @post Destination bytes contain `value` in little-endian order.
 * @post No byte beyond the four-byte destination span is modified.
 * @note The helper performs no alignment-dependent stores.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_c6_cam_audio_put_u32(uint8_t* out, uint32_t value)
{
  out[0] = (uint8_t)(value & (uint32_t)k_c6_cam_byte_mask);
  out[1] = (uint8_t)((value >> 8U) & (uint32_t)k_c6_cam_byte_mask);
  out[2] = (uint8_t)((value >> 16U) & (uint32_t)k_c6_cam_byte_mask);
  out[3] = (uint8_t)(value >> (uint32_t)k_c6_cam_shift_24);
}

/**
 * @brief Convert signed 20-bit PDM PCM to saturated signed 16-bit PCM.
 * @details Applies fixed application gain, removes four low-significance bits,
 *          then saturates to the WAV range.
 * @param[in] sample Signed sample supplied by the PDM backend.
 * @return Converted PCM sample.
 * @retval INT16_MAX The scaled input exceeded positive 16-bit range.
 * @retval INT16_MIN The scaled input exceeded negative 16-bit range.
 * @pre `sample` is a sign-extended PDM sample or another valid signed integer.
 * @pre No mutable audio state is required.
 * @post The result lies in the signed 16-bit range.
 * @post Global ring and WAV storage remain unchanged.
 * @note Integer division intentionally discards four low bits.
 * @since 0.1.0
 */
RA8_INTERNAL static int16_t internal_c6_cam_audio_s16(int32_t sample)
{
  const int64_t amplified = (int64_t)sample * (int64_t)k_c6_cam_audio_gain;
  int32_t       scaled    = (int32_t)(amplified / 16);
  if (scaled > INT16_MAX) {
    scaled = INT16_MAX;
  } else if (scaled < INT16_MIN) {
    scaled = INT16_MIN;
  }
  return (int16_t)scaled;
}

/**
 * @brief Append one complete audio frame to the active SDRAM ring.
 * @details Writes interrupt-delivered PCM into the selected bank and updates bounded metadata.
 * @param[in,out] ctx Unused callback context.
 * @param[in] frame Borrowed PCM-S32LE audio frame.
 * @pre `frame` is null or remains valid for the callback duration.
 * @pre The active bank index selects one of the two static banks.
 * @post Valid samples are appended with circular wraparound.
 * @post Metadata describes retained count, next write index, and oldest timestamp.
 * @note This callback runs in the PDM interrupt path.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_c6_cam_audio_on_frame(void* ctx, const ra8_audio_frame_t* frame)
{
  (void)ctx;
  if ((frame == nullptr) || (frame->data == nullptr) ||
      (frame->format != k_ra8_audio_format_pcm_s32le)) {
    return;
  }
  const uint8_t        bank    = s_audio_active_bank;
  c6_cam_audio_bank_t* meta    = &s_audio_banks[bank];
  const int32_t*       samples = (const int32_t*)frame->data;
  for (uint32_t i = 0U; i < frame->sample_count; ++i) {
    s_audio_ring[bank][meta->write_index] = samples[i];
    meta->write_index += 1U;
    if (meta->write_index == (uint32_t)k_c6_cam_audio_ring_samples) {
      meta->write_index = 0U;
    }
  }
  if (meta->sample_count < (uint32_t)k_c6_cam_audio_ring_samples) {
    const uint32_t room = (uint32_t)k_c6_cam_audio_ring_samples - meta->sample_count;
    meta->sample_count += (frame->sample_count < room) ? frame->sample_count : room;
  }
  const uint32_t retained_ms =
    (meta->sample_count * (uint32_t)k_c6_cam_ms_per_second) / (uint32_t)k_c6_cam_audio_rate_hz;
  const uint32_t frame_end_ms =
    frame->timestamp_ms +
    ((frame->sample_count * (uint32_t)k_c6_cam_ms_per_second) / (uint32_t)k_c6_cam_audio_rate_hz);
  meta->oldest_timestamp_ms = frame_end_ms - retained_ms;
}

/**
 * @brief Serialize the canonical mono PCM-S16LE WAV header.
 * @details Writes RIFF, format, rate, alignment, and data-size fields into static storage.
 * @param[in] samples Number of mono samples following the header.
 * @pre `samples` does not exceed the configured ring capacity.
 * @pre `s_audio_wav` is not concurrently served while the snapshot is built.
 * @post The first 44 bytes contain a self-consistent PCM-S16LE WAV header.
 * @post Payload bytes after the header remain unchanged.
 * @note The sample rate is fixed at 16 kHz mono.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_c6_cam_audio_write_header(uint32_t samples)
{
  const uint32_t data_bytes = samples * (uint32_t)k_c6_cam_wav_sample_bytes;
  (void)memcpy(&s_audio_wav[0], s_wav_tag_riff, sizeof(s_wav_tag_riff));
  internal_c6_cam_audio_put_u32(&s_audio_wav[4], (uint32_t)k_c6_cam_wav_riff_overhead + data_bytes);
  (void)memcpy(&s_audio_wav[8], s_wav_tag_wavefmt, sizeof(s_wav_tag_wavefmt));
  internal_c6_cam_audio_put_u32(&s_audio_wav[16], 16U);
  internal_c6_cam_audio_put_u16(&s_audio_wav[k_c6_cam_wav_audio_format_off], 1U);
  internal_c6_cam_audio_put_u16(&s_audio_wav[k_c6_cam_wav_channels_off], 1U);
  internal_c6_cam_audio_put_u32(&s_audio_wav[24], (uint32_t)k_c6_cam_audio_rate_hz);
  internal_c6_cam_audio_put_u32(&s_audio_wav[k_c6_cam_wav_byte_rate_off],
                                (uint32_t)k_c6_cam_audio_rate_hz *
                                  (uint32_t)k_c6_cam_wav_sample_bytes);
  internal_c6_cam_audio_put_u16(&s_audio_wav[32], (uint16_t)k_c6_cam_wav_sample_bytes);
  internal_c6_cam_audio_put_u16(&s_audio_wav[k_c6_cam_wav_bits_off], 16U);
  (void)memcpy(&s_audio_wav[36], s_wav_tag_data, sizeof(s_wav_tag_data));
  internal_c6_cam_audio_put_u32(&s_audio_wav[k_c6_cam_wav_data_size_off], data_bytes);
}

ra8_err_t c6_cam_audio_start(void)
{
  ra8_err_t err = ra8_board_pdm_mic_route();
  if (err != k_ra8_ok) {
    return err;
  }
  ra8_board_pdm_mic_config_t board = {};
  err                              = ra8_board_pdm_mic_get_config(k_ra8_board_pdm_mic1, &board);
  if (err != k_ra8_ok) {
    return err;
  }
  if (board.sample_rate_hz != (uint32_t)k_c6_cam_audio_rate_hz) {
    return k_ra8_err_invalid_state;
  }
  const ra8_audio_source_pdm_cfg_t cfg = {
    .pdm               = board.pdm,
    .sample_rate_hz    = board.sample_rate_hz,
    .samples_per_frame = (uint32_t)k_c6_cam_audio_frame_samples,
    .settle_ms         = (uint32_t)k_c6_cam_audio_settle_ms,
    .discard_samples   = (uint32_t)k_c6_cam_audio_discard,
    .poll_attempts     = (uint32_t)k_c6_cam_audio_poll_attempts,
    .channel           = board.channel,
    .valid_bits        = board.valid_bits,
    .irq_priority      = (uint8_t)k_c6_cam_audio_irq_priority,
  };
  err = ra8_audio_source_pdm_init(&s_audio_source, &s_audio_state, &cfg);
  if (err != k_ra8_ok) {
    return err;
  }
  const ra8_audio_buffer_t buffer = {
    .data     = s_audio_frame,
    .capacity = (uint32_t)sizeof(s_audio_frame),
  };
  err = ra8_audio_source_stream_start(&s_audio_source,
                                      &buffer,
                                      internal_c6_cam_audio_on_frame,
                                      nullptr);
  if (err != k_ra8_ok) {
    (void)ra8_audio_source_stop(&s_audio_source);
  }
  return err;
}

ra8_err_t
c6_cam_audio_snapshot_wav(const uint8_t** out_wav, uint32_t* out_bytes, uint32_t* out_timestamp_ms)
{
  if ((out_wav == nullptr) || (out_bytes == nullptr) || (out_timestamp_ms == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  *out_wav          = nullptr;
  *out_bytes        = 0U;
  *out_timestamp_ms = 0U;
  ra8_register_guard_t guard;
  ra8_register_guard_enter(&guard);
  const uint8_t  snapshot_bank = s_audio_active_bank;
  const uint32_t samples       = s_audio_banks[snapshot_bank].sample_count;
  if (samples == 0U) {
    ra8_register_guard_exit(&guard);
    return k_ra8_err_no_data;
  }
  const uint32_t write        = s_audio_banks[snapshot_bank].write_index;
  const uint32_t timestamp_ms = s_audio_banks[snapshot_bank].oldest_timestamp_ms;
  const uint8_t  capture_bank = snapshot_bank ^ 1U;
  s_audio_banks[capture_bank] = (c6_cam_audio_bank_t){};
  s_audio_active_bank         = capture_bank;
  ra8_register_guard_exit(&guard);

  const uint32_t oldest = (samples == (uint32_t)k_c6_cam_audio_ring_samples) ? write : 0U;
  internal_c6_cam_audio_write_header(samples);
  for (uint32_t i = 0U; i < samples; ++i) {
    uint32_t ring_index = oldest + i;
    if (ring_index >= (uint32_t)k_c6_cam_audio_ring_samples) {
      ring_index -= (uint32_t)k_c6_cam_audio_ring_samples;
    }
    const uint16_t pcm =
      (uint16_t)internal_c6_cam_audio_s16(s_audio_ring[snapshot_bank][ring_index]);
    internal_c6_cam_audio_put_u16(
      &s_audio_wav[k_c6_cam_wav_header_bytes + (i * (uint32_t)k_c6_cam_wav_sample_bytes)],
      pcm);
  }
  *out_wav = s_audio_wav;
  *out_bytes =
    (uint32_t)k_c6_cam_wav_header_bytes + (samples * (uint32_t)k_c6_cam_wav_sample_bytes);
  *out_timestamp_ms = timestamp_ms;
  return k_ra8_ok;
}
