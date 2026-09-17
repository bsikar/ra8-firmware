/**
 * @file examples/ek_ra8d2/hw_pending/camera_io_stream_demo/src/main.c
 * @brief ra8_camera_io demo: move one complete encoded frame from a camera
 *        source through a codec into an ra8_io byte-stream sink.
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * `ra8_camera_io` exports exactly one function,
 * ::ra8_camera_codec_encode_to_stream, which bridges an encoded
 * ::ra8_camera_frame_t onto an ::ra8_io_stream_t sink. It was split out of
 * `ra8_camera` so capture-only consumers do not inherit `ra8_io`, and that
 * split left it with no app or example consumer at all: only the host tests
 * ever compiled it, and their fakes stand in for both halves of the bridge.
 * This app is that consumer, and it self-checks rather than merely printing.
 *
 * The pipeline is deterministic and needs no camera module: a synthetic frame
 * (SOI, a fixed byte ramp, EOI) is replayed by ::ra8_camera_source_memory_init,
 * encoded by the zero-copy JPEG passthrough codec, and written to a RAM sink
 * bound through ::ra8_io_stream_ram_init, so every byte that reaches the sink
 * can be compared against the bytes that entered the source.
 *
 * Five legs run, each checked:
 *
 *   1. The source reports the geometry that was bound to it.
 *   2. A capture lands in caller storage, byte-identical to the frame.
 *   3. The bridge writes the whole encoded frame to the RAM sink, reports the
 *      byte count, and the sink holds those exact bytes.
 *   4. A NULL stream is refused with ::k_ra8_err_null_ptr and writes nothing.
 *   5. A sink two bytes too small fails with ::k_ra8_err_no_mem and reports the
 *      accepted prefix, which is the bridge propagating a sink error rather
 *      than reporting a short write as success.
 *
 * Observable over the SCI8 / J-Link OB VCOM console. A good run prints
 * `camera_io_stream_demo: bridge PASS`. It lives under hw_pending because it
 * has not been captured on the bench yet; nothing here touches a peripheral
 * beyond the console, so ra8_emulator runs it as is.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "ra8_boot_entry.h"
#include "ra8_camera.h"
#include "ra8_camera_codec_passthrough.h"
#include "ra8_camera_source_memory.h"
#include "ra8_camera_stream.h"
#include "ra8_err.h"
#include "ra8_io_log.h"
#include "ra8_io_stream.h"
#include "ra8_io_stream_ram.h"
#include "ra8_io_stream_uart.h"
#include "ra8_log.h"
#include "ra8_sci.h"

/**
 * @enum cd_const_t
 * @brief Console, frame, and sink knobs (no magic numbers).
 *
 * @details Collects every literal the app uses. The frame is a complete
 *          SOI-to-EOI byte stream so the passthrough codec accepts it as one
 *          encoded JPEG frame; its payload is a fixed ramp, not real image
 *          data, because nothing here decodes it.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_cd_uart_chan   = 8U,    /**< SCI8 J-Link OB console.          */
  k_cd_frame_bytes = 64U,   /**< Synthetic encoded-frame length.  */
  k_cd_width       = 16U,   /**< Declared frame width in pixels.  */
  k_cd_height      = 16U,   /**< Declared frame height in pixels. */
  k_cd_marker_hi   = 0xFFU, /**< First byte of a JPEG marker.     */
  k_cd_soi_lo      = 0xD8U, /**< Start-of-image marker low byte.  */
  k_cd_eoi_lo      = 0xD9U, /**< End-of-image marker low byte.    */
  k_cd_fill_seed   = 0x41U, /**< First payload byte of the ramp.  */
  k_cd_fill_step   = 7U,    /**< Payload byte step of the ramp.   */
  k_cd_marker_len  = 2U,    /**< Bytes in one JPEG marker.        */
} cd_const_t;

static uint8_t s_frame_store[k_cd_frame_bytes]; /**< Replayed frame bytes.  */
static uint8_t s_capture[k_cd_frame_bytes];     /**< Capture destination.   */
static uint8_t s_sink_store[k_cd_frame_bytes];  /**< RAM sink capture area. */

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
 * @brief Fill the replay buffer with one complete synthetic encoded frame.
 *
 * @return void
 * @post `s_frame_store` opens with SOI, ends with EOI, and carries a fixed
 *       ramp between them.
 * @note Deterministic, so a byte-for-byte comparison is a real assertion.
 * @since 0.1.0
 */
static void internal_build_frame(void)
{
  const uint32_t last = (uint32_t)k_cd_frame_bytes - 1U;
  for (uint32_t i = 0U; i < (uint32_t)k_cd_frame_bytes; i++) {
    s_frame_store[i] = (uint8_t)((uint32_t)k_cd_fill_seed + (i * (uint32_t)k_cd_fill_step));
  }
  s_frame_store[0]         = (uint8_t)k_cd_marker_hi;
  s_frame_store[1]         = (uint8_t)k_cd_soi_lo;
  s_frame_store[last - 1U] = (uint8_t)k_cd_marker_hi;
  s_frame_store[last]      = (uint8_t)k_cd_eoi_lo;
}

/**
 * @brief Describe the replay buffer as an immutable JPEG frame view.
 *
 * @param[out] out_frame Receives the frame view.
 * @return void
 * @pre ::internal_build_frame already populated the replay buffer.
 * @post `out_frame` is a complete JPEG stream view with stride zero.
 * @since 0.1.0
 */
static void internal_frame_view(ra8_camera_frame_t* out_frame)
{
  *out_frame = (ra8_camera_frame_t){
    .data         = s_frame_store,
    .bytes        = (uint32_t)k_cd_frame_bytes,
    .stride_bytes = 0U,
    .width        = (uint16_t)k_cd_width,
    .height       = (uint16_t)k_cd_height,
    .format       = k_ra8_camera_format_jpeg,
  };
}

/**
 * @brief Bind the memory source, check its reported geometry, and capture.
 *
 * @param[out] out_frame Receives the captured frame view.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                One capture matched the replayed bytes.
 * @retval k_ra8_err_invalid_state Geometry or captured bytes disagreed.
 * @retval other                   Propagated from the source facade.
 * @pre ::internal_build_frame already populated the replay buffer.
 * @post On success `out_frame` views `s_capture`.
 * @since 0.1.0
 */
static ra8_err_t internal_capture_leg(ra8_camera_frame_t* out_frame)
{
  ra8_camera_frame_t               replay = {};
  ra8_camera_source_t              source = {};
  ra8_camera_source_memory_state_t state  = {};
  internal_frame_view(&replay);
  const ra8_err_t bind_err = ra8_camera_source_memory_init(&source, &state, &replay);
  if (bind_err != k_ra8_ok) {
    return bind_err;
  }

  ra8_camera_source_info_t info     = {};
  const ra8_err_t          info_err = ra8_camera_source_get_info(&source, &info);
  if (info_err != k_ra8_ok) {
    return info_err;
  }
  if ((info.format != k_ra8_camera_format_jpeg) || (info.width != (uint16_t)k_cd_width) ||
      (info.height != (uint16_t)k_cd_height) ||
      (info.frame_bytes_max < (uint32_t)k_cd_frame_bytes)) {
    return k_ra8_err_invalid_state;
  }

  const ra8_camera_buffer_t capture     = {s_capture, (uint32_t)k_cd_frame_bytes};
  const ra8_err_t           capture_err = ra8_camera_source_capture(&source, &capture, out_frame);
  if (capture_err != k_ra8_ok) {
    return capture_err;
  }
  if ((out_frame->data != s_capture) || (out_frame->bytes != (uint32_t)k_cd_frame_bytes) ||
      (memcmp(s_capture, s_frame_store, sizeof s_capture) != 0)) {
    return k_ra8_err_invalid_state;
  }
  return k_ra8_ok;
}

/**
 * @brief Write one captured frame through the bridge into a RAM sink.
 *
 * @param[in,out] codec Bound passthrough codec.
 * @param[in]     frame Captured frame to forward.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                Sink holds the whole frame, byte for byte.
 * @retval k_ra8_err_invalid_state Reported count or sink content disagreed.
 * @retval other                   Propagated from the bridge.
 * @pre `frame` was produced by ::internal_capture_leg.
 * @post On success `s_sink_store` equals the replayed frame bytes.
 * @since 0.1.0
 */
static ra8_err_t internal_stream_leg(ra8_camera_codec_t* codec, const ra8_camera_frame_t* frame)
{
  ra8_io_stream_t           sink       = {};
  ra8_io_stream_ram_state_t sink_state = {};
  const ra8_err_t           sink_err =
    ra8_io_stream_ram_init(&sink, &sink_state, s_sink_store, (uint32_t)k_cd_frame_bytes);
  if (sink_err != k_ra8_ok) {
    return sink_err;
  }

  const ra8_camera_buffer_t zero_copy = {nullptr, 0U};
  uint32_t                  written   = 0U;
  const ra8_err_t           bridge_err =
    ra8_camera_codec_encode_to_stream(codec, frame, &zero_copy, &sink, &written);
  if (bridge_err != k_ra8_ok) {
    return bridge_err;
  }

  uint32_t        used     = 0U;
  const ra8_err_t used_err = ra8_io_stream_ram_used(&sink_state, &used);
  if (used_err != k_ra8_ok) {
    return used_err;
  }
  if ((written != (uint32_t)k_cd_frame_bytes) || (used != (uint32_t)k_cd_frame_bytes) ||
      (memcmp(s_sink_store, s_frame_store, sizeof s_sink_store) != 0)) {
    return k_ra8_err_invalid_state;
  }
  return k_ra8_ok;
}

/**
 * @brief Check that the bridge refuses a NULL sink and a sink that is too small.
 *
 * @param[in,out] codec Bound passthrough codec.
 * @param[in]     frame Captured frame to forward.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                Both refusals reported the documented code.
 * @retval k_ra8_err_invalid_state A refusal leg returned something else.
 * @retval other                   Propagated from the sink bind.
 * @pre `frame` was produced by ::internal_capture_leg.
 * @post The short sink holds only the prefix it accepted.
 * @note A short write reported as success is the regression this leg catches.
 * @since 0.1.0
 */
static ra8_err_t internal_refusal_legs(ra8_camera_codec_t* codec, const ra8_camera_frame_t* frame)
{
  const ra8_camera_buffer_t zero_copy = {nullptr, 0U};
  uint32_t                  written   = (uint32_t)k_cd_frame_bytes;
  if (ra8_camera_codec_encode_to_stream(codec, frame, &zero_copy, nullptr, &written) !=
      k_ra8_err_null_ptr) {
    return k_ra8_err_invalid_state;
  }

  const uint32_t            short_cap  = (uint32_t)k_cd_frame_bytes - (uint32_t)k_cd_marker_len;
  ra8_io_stream_t           sink       = {};
  ra8_io_stream_ram_state_t sink_state = {};
  const ra8_err_t sink_err = ra8_io_stream_ram_init(&sink, &sink_state, s_sink_store, short_cap);
  if (sink_err != k_ra8_ok) {
    return sink_err;
  }
  written = 0U;
  if (ra8_camera_codec_encode_to_stream(codec, frame, &zero_copy, &sink, &written) !=
      k_ra8_err_no_mem) {
    return k_ra8_err_invalid_state;
  }
  if (written != short_cap) {
    return k_ra8_err_invalid_state;
  }
  return k_ra8_ok;
}

/**
 * @brief Run the whole bridge pipeline once.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok Capture, stream, and both refusal legs agreed.
 * @retval other    First leg that disagreed or failed.
 * @post On success the sink was proven to hold the source bytes exactly.
 * @since 0.1.0
 */
static ra8_err_t internal_run(void)
{
  internal_build_frame();
  (void)memset(s_capture, 0, sizeof s_capture);
  (void)memset(s_sink_store, 0, sizeof s_sink_store);

  ra8_camera_frame_t captured    = {};
  const ra8_err_t    capture_err = internal_capture_leg(&captured);
  if (capture_err != k_ra8_ok) {
    return capture_err;
  }

  ra8_camera_codec_t codec     = {};
  const ra8_err_t    codec_err = ra8_camera_codec_passthrough_init(&codec);
  if (codec_err != k_ra8_ok) {
    return codec_err;
  }

  const ra8_err_t stream_err = internal_stream_leg(&codec, &captured);
  if (stream_err != k_ra8_ok) {
    return stream_err;
  }
  return internal_refusal_legs(&codec, &captured);
}

/**
 * @brief Firmware entry point.
 *
 * @details Brings up the console, runs the camera-to-stream pipeline, reports
 *          one verdict, and parks in an infinite loop.
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
  (void)ra8_io_stream_uart_init(&s_uart, &s_uart_state, (uint8_t)k_cd_uart_chan);
  (void)ra8_io_log_attach(&s_uart);
  internal_print("camera_io_stream_demo: boot\r\n");

  const ra8_err_t err = internal_run();
  if (err == k_ra8_ok) {
    internal_print("camera_io_stream_demo: bridge PASS\r\n");
  } else {
    internal_print("camera_io_stream_demo: bridge FAIL err ");
    (void)ra8_io_stream_put_u32(&s_uart, (uint32_t)err);
    internal_print("\r\n");
  }

  (void)ra8_sci_flush((uint8_t)k_cd_uart_chan);
  while (true) {
  }
}
