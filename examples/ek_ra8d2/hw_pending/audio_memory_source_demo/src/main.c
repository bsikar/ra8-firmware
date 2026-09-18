/**
 * @file examples/ek_ra8d2/hw_pending/audio_memory_source_demo/src/main.c
 * @brief ra8_audio demo: bind an app-owned PCM frame through the in-memory
 *        replay backend and self-check the facade against it.
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * `ra8_audio` had a firmware consumer only through the PDM backend, so the
 * seam the facade exists for -- an app swapping its capture source without
 * touching its own logic -- was exercised nowhere outside the host tests, and
 * ::ra8_audio_source_get_info had no caller at all. This app is that consumer
 * (see issue 1349). It binds a caller-owned interleaved s16le frame through
 * ::ra8_audio_source_memory_init and checks seven legs:
 *
 *   1. The synthetic frame validates, and two deliberately broken copies of it
 *      are refused with the documented codes.
 *   2. The bound handle points at the app's own state, and the reported
 *      contract matches the frame that was bound.
 *   3. A capture lands in the app's buffer byte for byte and the returned view
 *      borrows that buffer rather than the source.
 *   4. Replay repeats: the buffer is wiped and the second capture reproduces
 *      the first.
 *   5. A buffer one byte short is refused and left untouched.
 *   6. The memory backend publishes no streaming operation, so streaming is
 *      refused and the callback never fires.
 *   7. Stop unbinds the handle, and a later query fails rather than reading
 *      stale state.
 *
 * The source is PCM in `.rodata` and the sink is a buffer in `.bss`, so no
 * microphone, DMA channel or codec is involved; only the SCI8 / J-Link OB VCOM
 * console is used, to report the verdict. A good run prints
 * `audio_memory_source_demo: memory source PASS`. It lives under hw_pending
 * because it has not been captured on the bench yet, not because it needs a
 * peripheral.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "ra8_audio.h"
#include "ra8_audio_source_memory.h"
#include "ra8_boot_entry.h"
#include "ra8_err.h"
#include "ra8_io_log.h"
#include "ra8_io_stream.h"
#include "ra8_io_stream_uart.h"
#include "ra8_log.h"
#include "ra8_sci.h"

/**
 * @enum am_const_t
 * @brief Console and PCM-geometry knobs (no magic numbers).
 *
 * @details Collects every literal the app uses so the magic-number gate stays
 *          silent. The geometry is the one the facade's invariant is written
 *          against: `bytes == sample_count * channels * container_bytes`.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_am_uart_chan     = 8U,     /**< SCI8 J-Link OB console.                  */
  k_am_samples       = 64U,    /**< Sample frames per channel in the source. */
  k_am_channels      = 2U,     /**< Interleaved channel count.               */
  k_am_container     = 2U,     /**< Container bytes for s16le.               */
  k_am_valid_bits    = 16U,    /**< Significant bits per sample.             */
  k_am_rate_hz       = 16000U, /**< Sample frames per second.                */
  k_am_timestamp_ms  = 12U,    /**< Fixed capture-start stamp for the frame. */
  k_am_frame_bytes   = 256U,   /**< 64 samples * 2 channels * 2 bytes.       */
  k_am_pcm_words     = 128U,   /**< 64 samples * 2 interleaved channels.     */
  k_am_ramp_step     = 37U,    /**< Deterministic sample-ramp step.          */
  k_am_ramp_mask     = 0x7FFFU, /**< Keeps the ramp inside int16 positives.  */
  k_am_wipe_byte     = 0xA5U,  /**< Sentinel the capture must overwrite.     */
  k_am_bad_bits      = 24U,    /**< Wider than the s16le container allows.   */
  k_am_short_by      = 1U,     /**< Bytes withheld for the refusal leg.      */
} am_const_t;


static int16_t s_source_pcm[k_am_pcm_words];      /**< Source PCM, app-owned.  */
static uint8_t s_capture[k_am_frame_bytes];       /**< Capture sink, app-owned.*/

static ra8_io_stream_t            s_uart;       /**< Console stream.       */
static ra8_io_stream_uart_state_t s_uart_state; /**< Console stream state. */

static bool s_callback_fired = false; /**< Set only if streaming delivered. */

/**
 * @brief Write a NUL-terminated string to the console stream.
 *
 * @param[in] text Message to queue on SCI8.
 * @return void
 * @pre The console stream was initialised.
 * @post The text was queued on the console sink.
 * @note Errors are ignored: the console reports, it does not act.
 * @since 0.1.0
 */
static void internal_print(const char* text)
{
  (void)ra8_io_stream_puts(&s_uart, text);
}

/**
 * @brief Report one failed leg on the console.
 *
 * @param[in] what Short description of the leg that failed.
 * @return void
 * @post One FAIL line naming the leg was queued on the console.
 * @note Called only on the failure path; a good run prints none of these.
 * @since 0.1.0
 */
static void internal_fail(const char* what)
{
  internal_print("audio_memory_source_demo: ");
  internal_print(what);
  internal_print(" FAIL\r\n");
}

/**
 * @brief Fill the app-owned source PCM with a deterministic ramp.
 *
 * @return void
 * @post ::s_source_pcm holds the same content on every run.
 * @note Deterministic by design: the capture comparison depends on it.
 * @since 0.1.0
 */
static void internal_fill_source(void)
{
  for (uint32_t i = 0U; i < (uint32_t)k_am_pcm_words; ++i) {
    const uint32_t value = (i * (uint32_t)k_am_ramp_step) & (uint32_t)k_am_ramp_mask;
    s_source_pcm[i]      = (int16_t)value;
  }
}

/**
 * @brief Build the immutable frame view over the app-owned source PCM.
 *
 * @return ra8_audio_frame_t Frame describing ::s_source_pcm.
 * @post The returned view satisfies the facade's byte-coverage invariant.
 * @note The caller retains ownership of the PCM the view borrows.
 * @since 0.1.0
 */
static ra8_audio_frame_t internal_make_frame(void)
{
  return (ra8_audio_frame_t){
    .data           = s_source_pcm,
    .bytes          = (uint32_t)k_am_frame_bytes,
    .sample_count   = (uint32_t)k_am_samples,
    .sample_rate_hz = (uint32_t)k_am_rate_hz,
    .timestamp_ms   = (uint32_t)k_am_timestamp_ms,
    .channels       = (uint8_t)k_am_channels,
    .valid_bits     = (uint8_t)k_am_valid_bits,
    .format         = k_ra8_audio_format_pcm_s16le,
  };
}

/**
 * @brief Streaming callback that must never run on the memory backend.
 *
 * @param[in] ctx Unused caller context.
 * @param[in] frame Unused delivered frame.
 * @return void
 * @post ::s_callback_fired records that the callback was reached.
 * @note The memory backend publishes no streaming operation, so leg 6 asserts
 *       this stays false.
 * @since 0.1.0
 */
static void internal_stream_cb(void* ctx, const ra8_audio_frame_t* frame)
{
  (void)ctx;
  (void)frame;
  s_callback_fired = true;
}

/**
 * @brief Leg 1: the frame validates and two broken copies are refused.
 *
 * @param[in] frame Valid source frame.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              All three validations answered as documented.
 * @retval k_ra8_err_invalid_state A validation answered the wrong code.
 * @post No state is modified.
 * @since 0.1.0
 */
static ra8_err_t internal_leg_validate(const ra8_audio_frame_t* frame)
{
  if (ra8_audio_frame_validate(frame) != k_ra8_ok) {
    return k_ra8_err_invalid_state;
  }

  ra8_audio_frame_t short_bytes = *frame;
  short_bytes.bytes -= (uint32_t)k_am_short_by;
  if (ra8_audio_frame_validate(&short_bytes) != k_ra8_err_invalid_size) {
    return k_ra8_err_invalid_state;
  }

  ra8_audio_frame_t wide_bits = *frame;
  wide_bits.valid_bits        = (uint8_t)k_am_bad_bits;
  if (ra8_audio_frame_validate(&wide_bits) != k_ra8_err_invalid_arg) {
    return k_ra8_err_invalid_state;
  }
  return k_ra8_ok;
}

/**
 * @brief Leg 2: bind the frame and check the reported source contract.
 *
 * @param[out] source Receives the bound source handle.
 * @param[out] state Caller-owned backend state the handle must point at.
 * @param[in] frame Frame to bind.
 * @param[out] out_info Receives the source's fixed contract.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              Bound, and the contract matches the frame.
 * @retval k_ra8_err_invalid_state Binding or the reported contract disagreed.
 * @retval other                 Propagated facade error.
 * @post On success `source` is bound to `state`.
 * @since 0.1.0
 */
static ra8_err_t internal_leg_bind(ra8_audio_source_t*              source,
                                   ra8_audio_source_memory_state_t* state,
                                   const ra8_audio_frame_t*         frame,
                                   ra8_audio_source_info_t*         out_info)
{
  const ra8_err_t init_err = ra8_audio_source_memory_init(source, state, frame);
  if (init_err != k_ra8_ok) {
    return init_err;
  }
  if (source->ctx != state) {
    return k_ra8_err_invalid_state;
  }

  const ra8_err_t info_err = ra8_audio_source_get_info(source, out_info);
  if (info_err != k_ra8_ok) {
    return info_err;
  }
  if ((out_info->frame_bytes != frame->bytes) ||
      (out_info->samples_per_frame != frame->sample_count) ||
      (out_info->sample_rate_hz != frame->sample_rate_hz) ||
      (out_info->channels != frame->channels) ||
      (out_info->valid_bits != frame->valid_bits) || (out_info->format != frame->format)) {
    return k_ra8_err_invalid_state;
  }
  return k_ra8_ok;
}

/**
 * @brief Legs 3 and 4: capture into app storage, then repeat it.
 *
 * @param[in,out] source Bound source handle.
 * @param[in] buffer Capture sink sized from the reported contract.
 * @param[in] frame Source frame the capture must reproduce.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              Both captures matched the source byte for byte.
 * @retval k_ra8_err_invalid_state A capture borrowed the wrong storage or
 *                                 reproduced the wrong bytes.
 * @retval other                 Propagated facade error.
 * @post ::s_capture holds the source PCM.
 * @since 0.1.0
 */
static ra8_err_t internal_leg_capture(ra8_audio_source_t*       source,
                                      const ra8_audio_buffer_t* buffer,
                                      const ra8_audio_frame_t*  frame)
{
  ra8_audio_frame_t got = {};
  ra8_err_t         err = ra8_audio_source_capture(source, buffer, &got);
  if (err != k_ra8_ok) {
    return err;
  }
  if (got.data != buffer->data) {
    return k_ra8_err_invalid_state;
  }
  if ((got.bytes != frame->bytes) || (got.sample_count != frame->sample_count) ||
      (got.sample_rate_hz != frame->sample_rate_hz) || (got.channels != frame->channels) ||
      (got.valid_bits != frame->valid_bits) || (got.format != frame->format)) {
    return k_ra8_err_invalid_state;
  }
  if (memcmp(s_capture, s_source_pcm, (size_t)k_am_frame_bytes) != 0) {
    return k_ra8_err_invalid_state;
  }

  (void)memset(s_capture, (int)k_am_wipe_byte, (size_t)k_am_frame_bytes);
  ra8_audio_frame_t again = {};
  err                     = ra8_audio_source_capture(source, buffer, &again);
  if (err != k_ra8_ok) {
    return err;
  }
  if (memcmp(s_capture, s_source_pcm, (size_t)k_am_frame_bytes) != 0) {
    return k_ra8_err_invalid_state;
  }
  return k_ra8_ok;
}

/**
 * @brief Leg 5: a buffer one byte short is refused and left untouched.
 *
 * @param[in,out] source Bound source handle.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              Refused with the documented code, no writes.
 * @retval k_ra8_err_invalid_state The refusal or the sentinel check failed.
 * @post ::s_capture still holds the wipe sentinel.
 * @since 0.1.0
 */
static ra8_err_t internal_leg_short_buffer(ra8_audio_source_t* source)
{
  (void)memset(s_capture, (int)k_am_wipe_byte, (size_t)k_am_frame_bytes);
  const ra8_audio_buffer_t small = {
    .data     = s_capture,
    .capacity = (uint32_t)k_am_frame_bytes - (uint32_t)k_am_short_by,
  };
  ra8_audio_frame_t got = {};
  if (ra8_audio_source_capture(source, &small, &got) != k_ra8_err_invalid_size) {
    return k_ra8_err_invalid_state;
  }
  for (uint32_t i = 0U; i < (uint32_t)k_am_frame_bytes; ++i) {
    if (s_capture[i] != (uint8_t)k_am_wipe_byte) {
      return k_ra8_err_invalid_state;
    }
  }
  return k_ra8_ok;
}

/**
 * @brief Leg 6: the backend publishes no streaming operation.
 *
 * @param[in,out] source Bound source handle.
 * @param[in] buffer Capture sink large enough for one frame.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              Refused `not_supported` and nothing delivered.
 * @retval k_ra8_err_invalid_state The refusal was wrong or the callback ran.
 * @post ::s_callback_fired is still false.
 * @since 0.1.0
 */
static ra8_err_t internal_leg_no_stream(ra8_audio_source_t*       source,
                                        const ra8_audio_buffer_t* buffer)
{
  const ra8_err_t err = ra8_audio_source_stream_start(source, buffer, internal_stream_cb, nullptr);
  if (err != k_ra8_err_not_supported) {
    return k_ra8_err_invalid_state;
  }
  return s_callback_fired ? k_ra8_err_invalid_state : k_ra8_ok;
}

/**
 * @brief Leg 7: stop unbinds the handle and later queries fail.
 *
 * @param[in,out] source Bound source handle.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              Handle cleared and the later query refused.
 * @retval k_ra8_err_invalid_state Stop left the handle usable.
 * @retval other                 Propagated facade error.
 * @post `source` is unbound.
 * @since 0.1.0
 */
static ra8_err_t internal_leg_stop(ra8_audio_source_t* source)
{
  const ra8_err_t err = ra8_audio_source_stop(source);
  if (err != k_ra8_ok) {
    return err;
  }
  if ((source->ctx != nullptr) || (source->iface != nullptr)) {
    return k_ra8_err_invalid_state;
  }
  ra8_audio_source_info_t info = {};
  if (ra8_audio_source_get_info(source, &info) != k_ra8_err_not_initialized) {
    return k_ra8_err_invalid_state;
  }
  return k_ra8_ok;
}

/**
 * @brief Run every leg in order, reporting the first that fails.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok All seven legs held.
 * @retval other    The failing leg's error, after its FAIL line was printed.
 * @post On failure exactly one leg line was queued on the console.
 * @since 0.1.0
 */
static ra8_err_t internal_run(void)
{
  internal_fill_source();
  const ra8_audio_frame_t frame = internal_make_frame();

  ra8_err_t err = internal_leg_validate(&frame);
  if (err != k_ra8_ok) {
    internal_fail("leg 1 frame validate");
    return err;
  }

  ra8_audio_source_t              source = {};
  ra8_audio_source_memory_state_t state  = {};
  ra8_audio_source_info_t         info   = {};
  err                                    = internal_leg_bind(&source, &state, &frame, &info);
  if (err != k_ra8_ok) {
    internal_fail("leg 2 bind and contract");
    return err;
  }

  const ra8_audio_buffer_t buffer = {.data = s_capture, .capacity = info.frame_bytes};

  err = internal_leg_capture(&source, &buffer, &frame);
  if (err != k_ra8_ok) {
    internal_fail("leg 3/4 capture and replay");
    return err;
  }

  err = internal_leg_short_buffer(&source);
  if (err != k_ra8_ok) {
    internal_fail("leg 5 short buffer refusal");
    return err;
  }

  err = internal_leg_no_stream(&source, &buffer);
  if (err != k_ra8_ok) {
    internal_fail("leg 6 streaming refusal");
    return err;
  }

  err = internal_leg_stop(&source);
  if (err != k_ra8_ok) {
    internal_fail("leg 7 stop unbinds");
    return err;
  }
  return k_ra8_ok;
}

/**
 * @brief Firmware entry point.
 *
 * @details Brings up the console, runs the in-memory source legs, prints the
 *          verdict, and parks in an infinite loop.
 *
 * @pre SystemInit configured VTOR / FPU / priority grouping.
 * @post A PASS or FAIL verdict is queued on SCI8.
 * @post Control parks in an infinite loop; the function never returns.
 * @note Single-threaded; runs to the park loop on the main stack.
 * @since 0.1.0
 */
void main(void)
{
  ra8_log_init();
  (void)ra8_io_stream_uart_init(&s_uart, &s_uart_state, (uint8_t)k_am_uart_chan);
  (void)ra8_io_log_attach(&s_uart);
  internal_print("audio_memory_source_demo: boot\r\n");

  if (internal_run() == k_ra8_ok) {
    internal_print("audio_memory_source_demo: memory source PASS\r\n");
  } else {
    internal_print("audio_memory_source_demo: memory source FAIL\r\n");
  }

  (void)ra8_sci_flush((uint8_t)k_am_uart_chan);
  while (true) {
  }
}
