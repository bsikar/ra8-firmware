/**
 * @file examples/ek_ra8d2/hw_pending/audio_memory_replay_demo/src/main.c
 * @brief ra8_audio demo: bind an app-owned PCM fixture through the in-memory
 *        replay backend and self-check the facade end to end.
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * `ra8_audio` reaches its capture hardware only through the opaque
 * ::ra8_audio_source_iface_t seam, and both backends bind that seam through an
 * `_init()` helper. The contract the facade advertises is therefore that an app
 * can swap the capture source without touching its own logic, and that it can
 * size its own storage from ::ra8_audio_source_get_info rather than hard-coding
 * the frame geometry. Until this app, nothing in the tree did either: the only
 * consumers went through the PDM backend and hard-coded their frame storage.
 *
 * This app is that consumer, and it is the substitutability proof:
 *
 *   1. An app-owned interleaved s16le fixture is validated with
 *      ::ra8_audio_frame_validate, and a deliberately inconsistent copy is
 *      validated too, so the check is proven to reject as well as accept.
 *   2. ::ra8_audio_source_memory_init binds the fixture as a source.
 *   3. ::ra8_audio_source_get_info reports the fixed output contract, and the
 *      app sizes its capture buffer from `frame_bytes` instead of assuming the
 *      geometry it happens to know.
 *   4. A short buffer is offered first: the facade refuses it rather than
 *      overrunning caller storage.
 *   5. A correctly sized capture replays the fixture byte for byte, the
 *      returned view aliases the caller's buffer, and it validates.
 *   6. A second capture returns the same bytes, so replay is deterministic.
 *   7. Streaming is refused, because this backend binds no `stream_start`: that
 *      is the facade dispatching on the seam, not on a backend it assumes.
 *   8. ::ra8_audio_source_stop unbinds the handle, after which the facade
 *      reports the source as uninitialized.
 *
 * `ra8_audio` is mid-migration to Zig behind an unchanged C ABI, so an
 * app that links the C API and self-checks its own results is also the ABI
 * regression check that port wants.
 *
 * Observable over the SCI8 / J-Link OB VCOM console. A good run prints
 * `audio_memory_replay_demo: memory replay PASS`. No audio peripheral is
 * touched and no DMA is armed, so the run is deterministic and ra8_emulator
 * runs it as is; it lives under hw_pending only because it has not been
 * captured on the bench yet.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

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
 * @enum amr_const_t
 * @brief Console and PCM fixture knobs (no magic numbers).
 *
 * @details Collects every literal the app uses so the magic-number gate stays
 *          silent. The fixture is a small stereo s16le frame, which is the
 *          geometry the facade's byte-coverage invariant is most sensitive to.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_amr_uart_chan     = 8U,     /**< SCI8 J-Link OB console.               */
  k_amr_channels      = 2U,     /**< Interleaved stereo.                   */
  k_amr_samples       = 32U,    /**< Sample frames per channel.            */
  k_amr_container     = 2U,     /**< s16le container width in bytes.       */
  k_amr_valid_bits    = 16U,    /**< Significant bits per sample.          */
  k_amr_rate_hz       = 16000U, /**< Fixture sample rate.                  */
  k_amr_timestamp_ms  = 1234U,  /**< Fixture capture-start stamp.          */
  k_amr_fixture_bytes = 128U,   /**< samples * channels * container.       */
  k_amr_short_bytes   = 64U,    /**< Deliberately too small for one frame. */
  k_amr_ramp_step     = 257,    /**< Per-sample ramp step (spans both      */
                                /**< bytes of the container).              */
} amr_const_t;

static int16_t s_fixture[k_amr_samples * k_amr_channels]; /**< App-owned PCM.  */
static uint8_t s_capture[k_amr_fixture_bytes];            /**< Capture target. */

static ra8_io_stream_t            s_uart;       /**< Console stream.       */
static ra8_io_stream_uart_state_t s_uart_state; /**< Console stream state. */

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
 * @brief Fill the app-owned fixture with a deterministic ramp.
 *
 * @return void
 * @post Every fixture sample holds a distinct, reproducible value.
 * @note A ramp that steps by more than 256 moves both container bytes, so a
 *       byte-order or stride mistake in a replay cannot look correct.
 * @since 0.1.0
 */
static void internal_fill_fixture(void)
{
  for (uint32_t i = 0U; i < (uint32_t)(k_amr_samples * k_amr_channels); i++) {
    s_fixture[i] = (int16_t)((int32_t)i * (int32_t)k_amr_ramp_step);
  }
}

/**
 * @brief Describe the app-owned fixture as an immutable PCM frame view.
 *
 * @return ra8_audio_frame_t The fixture's frame view.
 * @post The returned view borrows the app-owned fixture storage.
 * @note `bytes` is computed from the geometry, not written by hand, so the
 *       facade's coverage invariant is asserted rather than assumed.
 * @since 0.1.0
 */
static ra8_audio_frame_t internal_fixture_frame(void)
{
  return (ra8_audio_frame_t){
    .data           = s_fixture,
    .bytes          = (uint32_t)k_amr_samples * (uint32_t)k_amr_channels *
                      (uint32_t)k_amr_container,
    .sample_count   = (uint32_t)k_amr_samples,
    .sample_rate_hz = (uint32_t)k_amr_rate_hz,
    .timestamp_ms   = (uint32_t)k_amr_timestamp_ms,
    .channels       = (uint8_t)k_amr_channels,
    .valid_bits     = (uint8_t)k_amr_valid_bits,
    .format         = k_ra8_audio_format_pcm_s16le,
  };
}

/**
 * @brief Compare a captured byte run against the fixture bytes.
 *
 * @param[in] captured First captured byte.
 * @param[in] len      Bytes to compare.
 * @return bool True iff every byte matches the fixture.
 * @pre `captured` is readable for `len` bytes.
 * @post No state is modified.
 * @note Hand-rolled rather than `memcmp` so the app pulls in no hosted C
 *       library and the loop bound stays visible (NASA Rule 2).
 * @since 0.1.0
 */
static bool internal_bytes_match(const uint8_t* captured, uint32_t len)
{
  const uint8_t* want = (const uint8_t*)s_fixture;
  for (uint32_t i = 0U; i < len; i++) {
    if (captured[i] != want[i]) {
      return false;
    }
  }
  return true;
}

/**
 * @brief Check that the frame validator rejects inconsistent metadata.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              The validator accepted the good frame and
 *                               refused a null view, a null payload, and a
 *                               byte count that does not cover the geometry.
 * @retval k_ra8_err_invalid_state A leg did not behave as documented.
 * @post No source is bound by this leg.
 * @note Proves the accept path is a decision, not a constant.
 * @since 0.1.0
 */
static ra8_err_t internal_check_validate(void)
{
  const ra8_audio_frame_t good = internal_fixture_frame();
  if (ra8_audio_frame_validate(&good) != k_ra8_ok) {
    return k_ra8_err_invalid_state;
  }
  if (ra8_audio_frame_validate(nullptr) != k_ra8_err_null_ptr) {
    return k_ra8_err_invalid_state;
  }

  ra8_audio_frame_t no_data = good;
  no_data.data             = nullptr;
  if (ra8_audio_frame_validate(&no_data) != k_ra8_err_null_ptr) {
    return k_ra8_err_invalid_state;
  }

  /* Byte count one container short of the declared geometry. */
  ra8_audio_frame_t torn = good;
  torn.bytes             = good.bytes - (uint32_t)k_amr_container;
  if (ra8_audio_frame_validate(&torn) != k_ra8_err_invalid_size) {
    return k_ra8_err_invalid_state;
  }
  return k_ra8_ok;
}

/**
 * @brief Bind the fixture and drive the facade through the memory backend.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                One full replay round trip held.
 * @retval k_ra8_err_invalid_state A leg did not behave as documented.
 * @retval other                   Forwarded from the failing facade call.
 * @pre The fixture holds its ramp and validates.
 * @post The source is left unbound.
 * @note The capture buffer is sized from the queried contract, which is the
 *       whole point: the app never hard-codes the source geometry.
 * @since 0.1.0
 */
static ra8_err_t internal_replay_round_trip(void)
{
  const ra8_audio_frame_t         fixture = internal_fixture_frame();
  ra8_audio_source_t              source  = {};
  ra8_audio_source_memory_state_t state   = {};

  ra8_err_t err = ra8_audio_source_memory_init(&source, &state, &fixture);
  if (err != k_ra8_ok) {
    return err;
  }
  if ((source.iface == nullptr) || (source.ctx != &state)) {
    return k_ra8_err_invalid_state;
  }

  /* Leg 3: take the storage requirement from the source, not from what this
   * app happens to know about the fixture. */
  ra8_audio_source_info_t info = {};
  err                          = ra8_audio_source_get_info(&source, &info);
  if (err != k_ra8_ok) {
    return err;
  }
  if ((info.frame_bytes != fixture.bytes) || (info.samples_per_frame != fixture.sample_count) ||
      (info.sample_rate_hz != fixture.sample_rate_hz) || (info.channels != fixture.channels) ||
      (info.valid_bits != fixture.valid_bits) || (info.format != fixture.format)) {
    return k_ra8_err_invalid_state;
  }
  if (info.frame_bytes > (uint32_t)k_amr_fixture_bytes) {
    return k_ra8_err_invalid_size;
  }

  /* Leg 4: a buffer the contract says is too small is refused, not overrun. */
  const ra8_audio_buffer_t short_buf = {.data     = s_capture,
                                        .capacity = (uint32_t)k_amr_short_bytes};
  ra8_audio_frame_t        out       = {};
  if (ra8_audio_source_capture(&source, &short_buf, &out) != k_ra8_err_invalid_size) {
    return k_ra8_err_invalid_state;
  }

  /* Leg 5: the sized capture replays the fixture into caller storage. */
  const ra8_audio_buffer_t buf = {.data = s_capture, .capacity = info.frame_bytes};
  err                          = ra8_audio_source_capture(&source, &buf, &out);
  if (err != k_ra8_ok) {
    return err;
  }
  if ((out.data != s_capture) || (out.bytes != info.frame_bytes) ||
      (out.sample_count != fixture.sample_count) || (out.timestamp_ms != fixture.timestamp_ms)) {
    return k_ra8_err_invalid_state;
  }
  if (!internal_bytes_match(s_capture, out.bytes)) {
    return k_ra8_err_invalid_state;
  }
  if (ra8_audio_frame_validate(&out) != k_ra8_ok) {
    return k_ra8_err_invalid_state;
  }

  /* Leg 6: replay is deterministic, so a second capture matches the first. */
  for (uint32_t i = 0U; i < (uint32_t)k_amr_fixture_bytes; i++) {
    s_capture[i] = 0U;
  }
  err = ra8_audio_source_capture(&source, &buf, &out);
  if (err != k_ra8_ok) {
    return err;
  }
  if (!internal_bytes_match(s_capture, out.bytes)) {
    return k_ra8_err_invalid_state;
  }

  /* Leg 7: this backend binds no stream_start, and the facade says so rather
   * than dispatching into a null operation. */
  if (ra8_audio_source_stream_start(&source, &buf, nullptr, nullptr) !=
      k_ra8_err_not_supported) {
    return k_ra8_err_invalid_state;
  }

  /* Leg 8: stop unbinds the handle, and the facade notices. */
  err = ra8_audio_source_stop(&source);
  if (err != k_ra8_ok) {
    return err;
  }
  if (source.iface != nullptr) {
    return k_ra8_err_invalid_state;
  }
  if (ra8_audio_source_get_info(&source, &info) != k_ra8_err_not_initialized) {
    return k_ra8_err_invalid_state;
  }
  return k_ra8_ok;
}

/**
 * @brief Firmware entry point.
 *
 * @details Brings up the console, checks the frame validator, runs the memory
 *          replay round trip, and parks in an infinite loop.
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
  (void)ra8_io_stream_uart_init(&s_uart, &s_uart_state, (uint8_t)k_amr_uart_chan);
  (void)ra8_io_log_attach(&s_uart);
  internal_print("audio_memory_replay_demo: boot\r\n");

  internal_fill_fixture();

  if (internal_check_validate() != k_ra8_ok) {
    internal_print("audio_memory_replay_demo: frame validate FAIL\r\n");
  } else if (internal_replay_round_trip() != k_ra8_ok) {
    internal_print("audio_memory_replay_demo: memory replay FAIL\r\n");
  } else {
    internal_print("audio_memory_replay_demo: memory replay PASS\r\n");
  }

  (void)ra8_sci_flush((uint8_t)k_amr_uart_chan);
  while (true) {
  }
}
