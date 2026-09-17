/**
 * @file examples/ek_ra8d2/hw_pending/camera_stream_demo/src/main.c
 * @brief ra8_camera_io bridge demo: encode one frame straight into an
 *        `ra8_io_stream` sink and prove the whole encoded frame landed.
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * `ra8_camera_io` is a deliberately separate library from `ra8_camera` so that
 * capture-only consumers do not inherit `ra8_io`. Until now nothing opted into
 * both, so the bridge had host coverage but no app or example that linked it.
 * This app is that consumer, and it stays honest about what it proves:
 *
 *   1. A deterministic RGB888 test pattern stands in for a captured frame, so
 *      the run needs no sensor, no CEU routing, and no board straps. The frame
 *      view is built exactly as `ra8_camera_source_capture` would hand it over.
 *   2. `ra8_camera_codec_jpeg_sw` binds the software JPEG codec over a
 *      caller-owned RGB workspace and a caller-owned encoded-output buffer.
 *   3. `ra8_io_stream_ram` is the sink, so the accepted bytes stay inspectable
 *      in SRAM instead of disappearing down a UART.
 *   4. `ra8_camera_codec_encode_to_stream` does the one bounded operation the
 *      bridge exists for: encode, then write the complete encoded frame. The
 *      verdict compares the reported written count against the bytes the sink
 *      actually captured, which is the property the bridge promises and the
 *      reason it returns `k_ra8_err_invalid_size` on a short write.
 *
 * The run is headless and observable over the SCI8 / J-Link OB VCOM console. A
 * successful run prints
 * `camera_stream_demo: encode-to-stream bytes <n> PASS`. It lives
 * under hw_pending because it has not been captured on the bench yet; nothing
 * in it touches a peripheral beyond the console, so ra8_emulator runs it as is.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_boot_entry.h"
#include "ra8_camera.h"
#include "ra8_camera_codec_jpeg_sw.h"
#include "ra8_camera_stream.h"
#include "ra8_err.h"
#include "ra8_io_log.h"
#include "ra8_io_stream.h"
#include "ra8_io_stream_ram.h"
#include "ra8_io_stream_uart.h"
#include "ra8_log.h"
#include "ra8_sci.h"

/**
 * @enum cs_const_t
 * @brief Console, image, and buffer knobs (no magic numbers).
 *
 * @details Collects every literal the app uses so the magic-number gate stays
 *          silent and the sizing reads symbolically. The image is kept small so
 *          the RGB workspace, the encoded-output buffer, and the RAM sink all
 *          fit comfortably in internal SRAM with no SDRAM dependency.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_cs_uart_chan    = 8U,     /**< SCI8 J-Link OB console.                  */
  k_cs_img_width    = 64U,    /**< Test-pattern width in pixels.            */
  k_cs_img_height   = 48U,    /**< Test-pattern height in pixels.           */
  k_cs_px_bytes     = 3U,     /**< Packed RGB888 bytes per pixel.           */
  k_cs_rgb_bytes    = 9216U,  /**< 64 * 48 * 3 source and workspace bytes.  */
  k_cs_jpeg_cap     = 16384U, /**< Encoded-output buffer capacity.          */
  k_cs_sink_cap     = 16384U, /**< RAM stream sink capacity.                */
  k_cs_jpeg_quality = 80U,    /**< JPEG quality handed to the encoder.      */
} cs_const_t;

/**
 * @enum cs_pattern_t
 * @brief Deterministic test-pattern coefficients (no magic numbers).
 *
 * @details Each channel is a fixed function of the pixel index, so the encoded
 *          frame is byte-for-byte reproducible across runs and the PASS line
 *          means the same thing every time.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_cs_pat_red_mul   = 5U,  /**< Red ramp multiplier.   */
  k_cs_pat_green_mul = 3U,  /**< Green ramp multiplier. */
  k_cs_pat_blue_add  = 17U, /**< Blue ramp bias.        */
} cs_pattern_t;

static uint8_t s_source_rgb[k_cs_rgb_bytes];    /**< Stand-in captured frame. */
static uint8_t s_rgb_workspace[k_cs_rgb_bytes]; /**< Codec RGB workspace.     */
static uint8_t s_jpeg_buf[k_cs_jpeg_cap];       /**< Encoded-output storage.  */
static uint8_t s_sink_buf[k_cs_sink_cap];       /**< RAM stream sink storage. */

static ra8_io_stream_t            s_uart;       /**< Console stream.       */
static ra8_io_stream_uart_state_t s_uart_state; /**< Console stream state. */

/**
 * @brief Write a NUL-terminated string to the console stream.
 *
 * @param[in] text Message to queue on SCI8.
 * @return void
 * @pre The console stream was initialised.
 * @post The text was queued on the console sink.
 * @note Errors are deliberately ignored: the console is reporting, not acting.
 * @since 0.1.0
 */
static void internal_print(const char* text)
{
  (void)ra8_io_stream_puts(&s_uart, text);
}

/**
 * @brief Fill the stand-in captured frame with the deterministic pattern.
 *
 * @return void
 * @post Every byte of ::s_source_rgb holds its pattern value.
 * @since 0.1.0
 */
static void internal_fill_pattern(void)
{
  for (uint32_t i = 0U; i < (uint32_t)k_cs_rgb_bytes; i += (uint32_t)k_cs_px_bytes) {
    const uint32_t px    = i / (uint32_t)k_cs_px_bytes;
    s_source_rgb[i]      = (uint8_t)(px * (uint32_t)k_cs_pat_red_mul);
    s_source_rgb[i + 1U] = (uint8_t)(px * (uint32_t)k_cs_pat_green_mul);
    s_source_rgb[i + 2U] = (uint8_t)(px + (uint32_t)k_cs_pat_blue_add);
  }
}

/**
 * @brief Run the bridge once and report how many bytes reached the sink.
 *
 * @param[out] out_written Bytes the stream accepted, zero on failure.
 * @param[out] out_used    Bytes the sink actually captured, zero on failure.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok Whole encoded frame was written and accounted for.
 * @retval other    Propagated from the codec, the bridge, or the sink.
 * @pre The pattern buffer holds a valid RGB888 image.
 * @post On success `*out_written` equals `*out_used`.
 * @since 0.1.0
 */
static ra8_err_t internal_run_bridge(uint32_t* out_written, uint32_t* out_used)
{
  *out_written = 0U;
  *out_used    = 0U;

  ra8_camera_codec_t                   codec       = {};
  ra8_camera_codec_jpeg_sw_state_t     codec_state = {};
  const ra8_camera_codec_jpeg_sw_cfg_t codec_cfg   = {
    .rgb_workspace          = s_rgb_workspace,
    .rgb_workspace_capacity = (uint32_t)k_cs_rgb_bytes,
    .output_width           = (uint16_t)k_cs_img_width,
    .output_height          = (uint16_t)k_cs_img_height,
    .quality                = (uint8_t)k_cs_jpeg_quality,
  };
  const ra8_err_t codec_err = ra8_camera_codec_jpeg_sw_init(&codec, &codec_state, &codec_cfg);
  if (codec_err != k_ra8_ok) {
    return codec_err;
  }

  ra8_io_stream_t           sink       = {};
  ra8_io_stream_ram_state_t sink_state = {};
  const ra8_err_t           sink_err =
    ra8_io_stream_ram_init(&sink, &sink_state, s_sink_buf, (uint32_t)k_cs_sink_cap);
  if (sink_err != k_ra8_ok) {
    return sink_err;
  }

  const ra8_camera_frame_t input = {
    .data         = s_source_rgb,
    .bytes        = (uint32_t)k_cs_rgb_bytes,
    .stride_bytes = (uint32_t)k_cs_img_width * (uint32_t)k_cs_px_bytes,
    .width        = (uint16_t)k_cs_img_width,
    .height       = (uint16_t)k_cs_img_height,
    .format       = k_ra8_camera_format_rgb888,
  };
  const ra8_camera_buffer_t output = {
    .data     = s_jpeg_buf,
    .capacity = (uint32_t)k_cs_jpeg_cap,
  };

  const ra8_err_t bridge_err =
    ra8_camera_codec_encode_to_stream(&codec, &input, &output, &sink, out_written);
  if (bridge_err != k_ra8_ok) {
    return bridge_err;
  }
  return ra8_io_stream_ram_used(&sink_state, out_used);
}

/**
 * @brief Firmware entry point.
 *
 * @details Brings up the console, builds the stand-in frame, drives the
 *          `ra8_camera_io` bridge once, and prints one PASS or FAIL verdict
 *          before parking in an infinite loop.
 *
 * @pre SystemInit configured VTOR / FPU / priority grouping.
 * @post A verdict line has been queued on SCI8.
 * @post Control parks in an infinite loop; the function never returns.
 * @note Single-threaded; runs to the park loop on the main stack.
 * @since 0.1.0
 */
void main(void)
{
  ra8_log_init();
  (void)ra8_io_stream_uart_init(&s_uart, &s_uart_state, (uint8_t)k_cs_uart_chan);
  (void)ra8_io_log_attach(&s_uart);
  internal_print("camera_stream_demo: boot\r\n");

  internal_fill_pattern();

  uint32_t        written = 0U;
  uint32_t        used    = 0U;
  const ra8_err_t err     = internal_run_bridge(&written, &used);

  if ((err == k_ra8_ok) && (written > 0U) && (written == used)) {
    internal_print("camera_stream_demo: encode-to-stream bytes ");
    (void)ra8_io_stream_put_u32(&s_uart, written);
    internal_print(" PASS\r\n");
  } else {
    internal_print("camera_stream_demo: encode-to-stream FAIL\r\n");
  }

  (void)ra8_sci_flush((uint8_t)k_cs_uart_chan);
  while (true) {
  }
}
