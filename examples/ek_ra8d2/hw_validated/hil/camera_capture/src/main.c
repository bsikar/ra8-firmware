/**
 * @file examples/ek_ra8d2/hw_validated/hil/camera_capture/src/main.c
 * @brief OV5640 parallel (CEU) camera capture + plausibility self-test
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * End-to-end bring-up of the EK-RA8D2 on-board OV5640 5 MP CMOS sensor
 * over the chip's Capture Engine Unit (CEU) parallel (DVP) path -- the
 * interface the board wires the camera to (connector J35, HUM Ch 60).
 * The app drives the real HAL drivers with no fakes and reports a
 * plausibility verdict over the SCI8 console plus a set of
 * J-Link-probable globals:
 *
 *   1. `ra8_cgc_init` / `ra8_time_init` / `ra8_mstp_init` -- clocks + SysTick.
 *   2. `ra8_board_uart_console_init` -- BSP SCI8 console (PD02 / PD03).
 *   3. XCLK: GPT channel 12 saw-PWM on GTIOC12A (P501) generates the
 *      ~24 MHz sensor input clock (XVCLK). The OV5640 needs XVCLK to
 *      answer even on its SCCB port, so this comes up first.
 *   4. I2C ch1 (SCL1 P512 / SDA1 P511) via the board's validated
 *      bring-up, then force SW4-6 = ON through the U15 expander so the
 *      Camera Expansion Board is in parallel (DVP) mode (the board
 *      default is MIPI). The OV5640 SCCB shares ch1 with U15 + the codec.
 *   5. Confirm the XCLK GPT counter advances, release the sensor RST
 *      strap (P709), then scan the bus and read the OV5640 chip-ID
 *      registers 0x300A/0x300B -- the VERIFY-FIRST proof the sensor is
 *      present (expected 0x5640), trying SCCB 0x3C then 0x3D.
 *   6. Program the proven VGA YUV422 live-scene sequence, sample the active
 *      DVP signals through GPIO, then route VIO_D[7:0], VIO_VD, VIO_HD, and
 *      VIO_CLK to the CEU.
 *   7. `ra8_camera_source_ceu` captures one packed frame into app-owned,
 *      cache-line-aligned internal SRAM; the backend polls CEU completion.
 *   8. Plausibility stats over the frame, then firmware-side UYVY-to-RGB888
 *      conversion into four cache-cleaned SDRAM buffers at 0/90/180/270.
 *      PASS requires a captured, non-degenerate frame and all four images.
 *
 * The reusable `ra8_ov5640` sensor driver owns sensor protocol and tables;
 * the board adapter owns J35 routing/reset/SCCB; `ra8_camera_source_ceu` owns
 * generic capture dispatch. This file owns app policy, caller-supplied source
 * state/storage, console output, and the verdict. `cam_ceu` is limited to the
 * HIL-only pre-routing GPIO activity probe.
 *
 * Banner (scraped by the HIL gate once promoted):
 *   `cam: gpt=RUN scan=3C:56.43:00. chipid=5640 xclk=OK rst=OK sccb=OK`
 *   ` ceu=OK bytes=614400 frame=OK cetcr=........`
 *   ` min=.. max=.. mean=.. verdict=PASS`
 *
 * Hardware: EK-RA8D2 with the OV5640 Camera Expansion Board on the
 * underside FFC port (J35). SW4-6 is driven ON in firmware; no manual
 * jumpers required.
 *
 * @note Bench status: validated on EK-RA8D2 silicon. The sensor ID reads
 *       0x5640, DVP activity is observed on every data/sync signal, CEU
 *       completes a 614400-byte VGA capture, and the host dump decodes to a
 *       clean live image. See the README for still/video capture commands.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "cam_ceu.h"
#include "cam_image.h"
#include "ra8_board_ek_ra8d2.h"
#include "ra8_boot_entry.h"
#include "ra8_camera.h"
#include "ra8_camera_source_ceu.h"
#include "ra8_ceu.h"
#include "ra8_cgc.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_gpt.h"
#include "ra8_i2c.h"
#include "ra8_isr.h"
#include "ra8_mstp.h"
#include "ra8_ov5640.h"
#include "ra8_port_utils.h"
#include "ra8_sdramc.h"
#include "ra8_time.h"

/* =============================================================================
 * Tunable constants (typed enums -- no magic numbers)
 * =============================================================================
 */

/** @brief Scalar app tunables (baud, clocks, GPT limits, decimal cap). */
typedef enum : uint32_t {
  k_cam_baud            = 115200U,   /**< SCI8 console baud.                     */
  k_cam_xclk_hz         = 24000000U, /**< OV5640 XVCLK input-clock target, Hz.   */
  k_cam_mode_settle_ms  = 100U,      /**< Settle after selecting parallel mode.  */
  k_cam_capture_poll_ms = 5U,        /**< CEU completion-poll interval.          */
  k_cam_capture_tries   = 800U,      /**< CEU poll cap (~4 seconds).             */
  k_cam_cache_line      = 32U,       /**< DMA buffer cache-line alignment.       */
  k_cam_string_cap      = 1024U,     /**< Console-string scan safety bound.      */
  k_cam_dec_cap         = 99999999U, /**< Max value cam_put_u32 can render.      */
  k_cam_gpt_probe_iters = 64U,       /**< GTCNT samples to prove the timer runs. */
} cam_u32_t;

/** @brief 8-bit app constants: hex/decimal printing, bus scan, U15 expander. */
typedef enum : uint8_t {
  k_cam_nibble_mask = 0x0FU, /**< Low-nibble mask for hex printing.    */
  k_cam_dec_base    = 10U,   /**< Base for decimal printing.           */
  k_cam_byte_max    = 0xFFU, /**< Maximum byte value (min-scan seed).  */
  k_cam_scan_lo     = 0x08U, /**< First 7-bit address in the bus scan. */
  k_cam_scan_hi     = 0x77U, /**< Last 7-bit address in the bus scan.  */
} cam_u8_t;

/** @brief Packed VGA UYVY CEU geometry. */
typedef enum : uint16_t {
  k_cam_line_bytes = 1280U, /**< 640 pixels x 2 packed bytes. */
} cam_capture_geom_t;

/** @brief Packed VGA UYVY storage layout. */
typedef enum : uint8_t {
  k_cam_bytes_per_pixel = 2U, /**< UYVY bytes per pixel. */
} cam_capture_layout_t;

/** @brief GPT channel that generates XCLK on GTIOC12A (P501). */
typedef enum : uint8_t {
  k_cam_xclk_gpt_ch = 12U, /**< P501 = GTIOC12A per RA8D2 datasheet pinout. */
} cam_gpt_t;

/* =============================================================================
 * J-Link-probable result globals
 * =============================================================================
 */

/** @var g_cam_chipid
 *  @brief Chip ID read over SCCB (0x300A<<8 | 0x300B). 0x5640 = OV5640.
 *  @warning Written once by main(); read via SWD.  @since 0.1.0 */
volatile uint32_t g_cam_chipid = 0U;
/** @var g_cam_frame_ok
 *  @brief 1 when the CEU signalled a completed capture, else 0.
 *  @warning Written once by main().  @since 0.1.0 */
volatile uint32_t g_cam_frame_ok = 0U;
/** @var g_cam_min
 *  @brief Minimum byte value across the captured frame.
 *  @warning Written once by main().  @since 0.1.0 */
volatile uint32_t g_cam_min = 0U;
/** @var g_cam_max
 *  @brief Maximum byte value across the captured frame.
 *  @warning Written once by main().  @since 0.1.0 */
volatile uint32_t g_cam_max = 0U;
/** @var g_cam_mean
 *  @brief Mean byte value across the captured frame.
 *  @warning Written once by main().  @since 0.1.0 */
volatile uint32_t g_cam_mean = 0U;
/** @var g_cam_verdict
 *  @brief 1 = plausible camera frame (PASS), 0 = FAIL.
 *  @warning Written once by main().  @since 0.1.0 */
volatile uint32_t g_cam_verdict = 0U;

/** @brief Console tag prefix for every banner line. */
static const uint8_t k_cam_tag[] = "cam: ";

/** @brief Transport-independent sensor instance bound to the board SCCB adapter. */
static ra8_ov5640_t s_camera_sensor;

/** @brief Generic CEU source handle and caller-owned backend state. */
static ra8_camera_source_t           s_camera_source;
static ra8_camera_source_ceu_state_t s_camera_source_state;

/** @brief Last immutable frame view returned by the generic source. */
static ra8_camera_frame_t s_camera_frame;

/** @brief App-owned, cache-line-aligned target for one packed VGA capture. */
alignas(k_cam_cache_line) static uint8_t s_camera_capture[k_cam_uyvy_frame_bytes];

/**
 * @brief Bind the reusable sensor driver to the EK-RA8D2 SCCB adapter.
 * @details Constructs a transport over board-owned SCCB and delay callbacks.
 * @return Repository error code.
 * @retval k_ra8_ok The sensor instance was initialized.
 * @retval k_ra8_err_null_ptr A required transport callback was unavailable.
 * @pre Board camera support is linked into the application.
 * @pre No capture operation is concurrently using `s_camera_sensor`.
 * @post On success, `s_camera_sensor` is initialized at the primary address.
 * @post No sensor register transfer is performed by the binding itself.
 * @note The board adapter owns the physical I2C channel.
 * @since 0.1.0
 */
static ra8_err_t cam_sensor_bind(void)
{
  const ra8_ov5640_bus_t bus = {
    .read_reg  = ra8_board_camera_sccb_read_reg,
    .write_reg = ra8_board_camera_sccb_write_reg,
    .delay_ms  = ra8_board_camera_delay_ms,
    .ctx       = nullptr,
  };
  return ra8_ov5640_init(&s_camera_sensor, &bus);
}

/**
 * @brief Bind the generic CEU source to app-owned state and VGA metadata.
 * @details Configures a single-shot 8-bit synchronous UYVY capture with fixed
 *          VGA geometry and bounded polling.
 * @return Error from CEU configuration or generic-source binding.
 * @retval k_ra8_ok The generic source is ready.
 * @retval k_ra8_err_null_ptr Internal source bindings were invalid.
 * @pre Clocks and module-stop control are initialized.
 * @pre Parallel camera pins have been routed to the CEU.
 * @post On success `s_camera_source` is ready for one-shot captures.
 * @post App-owned source state retains the configured VGA metadata.
 * @note This function allocates no capture storage.
 * @since 0.1.0
 */
static ra8_err_t cam_capture_source_bind(void)
{
  const ra8_camera_source_ceu_cfg_t cfg = {
    .ceu =
      {
        .width_px        = (uint16_t)k_cam_image_width_px,
        .height_px       = (uint16_t)k_cam_image_height_px,
        .x_start_px      = 0U,
        .y_start_px      = 0U,
        .x_capture_px    = (uint16_t)k_cam_line_bytes,
        .y_capture_lines = (uint16_t)k_cam_image_height_px,
        .dst_stride      = (uint16_t)k_cam_line_bytes,
        .frame_drop      = 0U,
        .bytes_per_pixel = (uint8_t)k_cam_bytes_per_pixel,
        .interrupts      = 0U,
        .capture_format  = k_ra8_ceu_fmt_data_synchronous,
        .capture_mode    = k_ra8_ceu_capture_single,
        .data_bus        = k_ra8_ceu_bus_8_bit,
        .hsync_polarity  = k_ra8_ceu_pol_high_active,
        .vsync_polarity  = k_ra8_ceu_pol_high_active,
        .field_polarity  = k_ra8_ceu_pol_high_active,
        .input_order     = k_ra8_ceu_input_cb0_y0_cr0_y1,
        .output_format   = k_ra8_ceu_output_ycbcr_422,
        .burst_mode      = k_ra8_ceu_burst_32,
        .first_field     = k_ra8_ceu_field_immediate,
        .edge            = {k_ra8_ceu_edge_rising,
                            k_ra8_ceu_edge_rising,
                            k_ra8_ceu_edge_rising,
                            k_ra8_ceu_edge_rising},
        .byte_swap       = {false, true, true},
        .scale = {0U, 0U, 0U, 0U, (uint16_t)k_cam_image_width_px, (uint16_t)k_cam_image_height_px},
        .interlace       = false,
        .one_field_only  = false,
        .bundle_write    = false,
        .low_pass_filter = false,
        .image_area_size = 0U,
      },
    .output =
      {
        .frame_bytes_max = (uint32_t)k_cam_uyvy_frame_bytes,
        .stride_bytes    = (uint32_t)k_cam_line_bytes,
        .width           = (uint16_t)k_cam_image_width_px,
        .height          = (uint16_t)k_cam_image_height_px,
        .format          = k_ra8_camera_format_uyvy422,
      },
    .poll_interval_ms = (uint32_t)k_cam_capture_poll_ms,
    .poll_attempts    = (uint32_t)k_cam_capture_tries,
  };
  return ra8_camera_source_ceu_init(&s_camera_source, &s_camera_source_state, &cfg);
}

/* =============================================================================
 * Console helpers
 * =============================================================================
 */

/**
 * @brief Bounded length of a NUL-terminated C string.
 * @details Scans at most `k_cam_string_cap` bytes to keep console helpers bounded.
 * @param[in] s Non-NULL C string.
 * @return Length in bytes, capped at ::k_cam_string_cap.
 * @retval uint32_t Number of bytes preceding NUL or the configured cap.
 *
 * @pre `s` is non-NULL.
 * @pre The string is NUL-terminated within the cap.
 * @post Return value never exceeds the cap.
 * @post `s` is not modified.
 * @note Thread safety: pure function.
 * @since 0.1.0
 */
static uint32_t cam_strlen(const char* s)
{
  RA8_CHECK_NULL_PTR(s, "cam", "strlen");
  uint32_t n = 0U;
  while ((n < (uint32_t)k_cam_string_cap) && (s[n] != '\0')) {
    n += 1U;
  }
  return n;
}

/**
 * @brief Write a NUL-terminated string to the SCI8 console.
 * @details Measures the bounded string and forwards its bytes to the board console.
 * @param[in] s Non-NULL C string.
 * @return ra8_err_t from the BSP console write.
 * @retval k_ra8_ok String queued.
 * @retval k_ra8_err_null_ptr `s` was NULL.
 *
 * @pre `s` is non-NULL.
 * @pre `ra8_board_uart_console_init` has run.
 * @post The bytes of `s` are handed to the console.
 * @post `s` is not modified.
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
static ra8_err_t cam_puts(const char* s)
{
  RA8_CHECK_NULL_PTR(s, "cam", "puts");
  const uint32_t len = cam_strlen(s);
  return ra8_board_uart_console_write((const uint8_t*)s, (size_t)len);
}

/**
 * @brief Write `width` hex nibbles of `value` (big-endian) to the console.
 * @details Formats uppercase hexadecimal into fixed stack storage before one write.
 * @param[in] value Value to print.
 * @param[in] width Number of nibbles (1..8).
 * @return ra8_err_t from the console write.
 * @retval k_ra8_ok Digits queued.
 * @retval k_ra8_err_invalid_arg `width` out of 1..8.
 *
 * @pre `width` is 1..8.
 * @pre `ra8_board_uart_console_init` has run.
 * @post Exactly `width` hex characters are emitted.
 * @post No other console state changes.
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
static ra8_err_t cam_put_hex(uint32_t value, uint8_t width)
{
  static const char k_hex[] = "0123456789ABCDEF";
  RA8_CHECK_RANGE(width, 1U, 8U, k_ra8_err_invalid_arg);
  char     buf[8];
  uint8_t  i = width;
  uint32_t v = value;
  while (i > 0U) {
    i      = (uint8_t)(i - 1U);
    buf[i] = k_hex[v & (uint32_t)k_cam_nibble_mask];
    v >>= 4U;
  }
  return ra8_board_uart_console_write((const uint8_t*)buf, (size_t)width);
}

/**
 * @brief Write an unsigned decimal integer to the console.
 * @details Formats the value backward into fixed stack storage before one write.
 * @param[in] value Value to print (0..99999999).
 * @return ra8_err_t from the console write.
 * @retval k_ra8_ok Digits queued.
 * @retval k_ra8_err_invalid_arg `value` exceeds 8 decimal digits.
 *
 * @pre `value` fits in 8 decimal digits.
 * @pre `ra8_board_uart_console_init` has run.
 * @post The decimal text of `value` is emitted.
 * @post No other console state changes.
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
static ra8_err_t cam_put_u32(uint32_t value)
{
  if (value > (uint32_t)k_cam_dec_cap) {
    return k_ra8_err_invalid_arg;
  }
  char     buf[8];
  uint8_t  i = (uint8_t)sizeof(buf);
  uint32_t v = value;
  do {
    i      = (uint8_t)(i - 1U);
    buf[i] = (char)('0' + (v % (uint32_t)k_cam_dec_base));
    v /= (uint32_t)k_cam_dec_base;
  } while ((v != 0U) && (i > 0U));
  const uint32_t len = (uint32_t)((uint8_t)sizeof(buf) - i);
  return ra8_board_uart_console_write((const uint8_t*)&buf[i], (size_t)len);
}

/* =============================================================================
 * Frame plausibility
 * =============================================================================
 */

/**
 * @brief Compute min / max / mean over the CEU frame and store to globals.
 * @details Validates the expected VGA UYVY geometry before reducing all bytes
 *          into simple non-degeneracy statistics for the HIL verdict.
 *
 * @return true when the frame is non-degenerate (max != min).
 * @retval true  Frame varies, consistent with a live non-degenerate capture.
 * @retval false Frame is all-identical, empty, or the buffer was unavailable.
 *
 * @param[in] frame Completed generic-camera frame view.
 * @pre @p frame was returned by ::ra8_camera_source_capture.
 * @pre The captured storage remains readable for `frame->bytes` bytes.
 * @post `g_cam_min` / `g_cam_max` / `g_cam_mean` reflect the frame.
 * @post Return distinguishes a live grab from an all-identical buffer.
 * @note Thread safety: reads only the completed frame view and its storage.
 * @since 0.1.0
 */
static bool cam_frame_is_plausible(const ra8_camera_frame_t* frame)
{
  if ((frame == nullptr) || (frame->data == nullptr)) {
    return false;
  }
  if ((frame->bytes != (uint32_t)k_cam_uyvy_frame_bytes) ||
      (frame->stride_bytes != (uint32_t)k_cam_line_bytes)) {
    return false;
  }
  if ((frame->width != (uint16_t)k_cam_image_width_px) ||
      (frame->height != (uint16_t)k_cam_image_height_px)) {
    return false;
  }
  if (frame->format != k_ra8_camera_format_uyvy422) {
    return false;
  }
  const uint32_t n   = frame->bytes;
  uint32_t       lo  = (uint32_t)k_cam_byte_max;
  uint32_t       hi  = 0U;
  uint32_t       sum = 0U;
  for (uint32_t i = 0U; i < n; i += 1U) {
    const uint32_t b = (uint32_t)frame->data[i];
    if (b < lo) {
      lo = b;
    }
    if (b > hi) {
      hi = b;
    }
    sum += b;
  }
  g_cam_min  = lo;
  g_cam_max  = hi;
  g_cam_mean = sum / n;
  return hi != lo;
}

/* =============================================================================
 * Discovery
 * =============================================================================
 */

/**
 * @brief Probe every 7-bit I2C address on ch1 and print those that ACK.
 *
 * @details A discovery aid: emits ` scan=3C.43.` so the bench log shows
 *          exactly which devices answer on the SCCB bus (the OV5640 at
 *          0x3C/0x3D, the U15 expander at 0x43, ...). Address-only probes,
 *          no register access.
 *
 * @pre RIIC ch1 is initialized.
 * @pre The console is up.
 * @post One ` scan=...` field has been written for the run.
 * @post The bus is left idle.
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
static void cam_bus_scan(void)
{
  (void)cam_puts(" scan=");
  for (uint8_t a = (uint8_t)k_cam_scan_lo; a <= (uint8_t)k_cam_scan_hi; a += 1U) {
    bool acked = false;
    if (ra8_i2c_scan((uint8_t)k_ra8_board_camera_i2c_channel, a, &acked) != k_ra8_ok) {
      continue;
    }
    if (!acked) {
      continue;
    }
    (void)cam_put_hex((uint32_t)a, 2U);
    /* Read register 0 (8-bit pointer) as a coarse device-ID fingerprint. */
    const uint8_t reg = 0U;
    uint8_t       v   = 0U;
    if (ra8_i2c_transfer((uint8_t)k_ra8_board_camera_i2c_channel, a, &reg, 1U, &v, 1U) ==
        k_ra8_ok) {
      (void)cam_puts(":");
      (void)cam_put_hex((uint32_t)v, 2U);
    }
    (void)cam_puts(".");
  }
}

/* =============================================================================
 * Orchestration
 * =============================================================================
 */

/** @brief Park forever after the self-test has reported its verdict.
 *  @details Repeatedly enters wait-for-interrupt without returning to startup code.
 *  @pre The verdict banner has been emitted.
 *  @pre Interrupt state is suitable for the application terminal state.
 *  @post The CPU idles; only reset or a debugger wakes it.
 *  @post No camera or console state is modified after entry.
 *  @note This is the intentional terminal state for the HIL image.
 *  @since 0.1.0 */
static void cam_park(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Bring up clocks, console, SDRAM, XCLK, SCCB and parallel camera.
 * @details Initializes dependencies in order, starts the sensor clock, and
 *          selects the U15 parallel-camera route without overwriting unrelated bits.
 * @return ra8_err_t from the first failing bring-up step, or ok.
 * @retval k_ra8_ok Console, SDRAM, XCLK, and the U15 DVP route are live.
 *
 * @pre Reset_Handler + SystemInit ran.
 * @pre Single-threaded init context.
 * @post On ok the console prints, SDRAM responds, and the OV5640 has XVCLK + SCCB.
 * @post On ok the Camera Expansion Board is in parallel (DVP) mode.
 * @note Thread safety: init context only.
 * @since 0.1.0
 */
static ra8_err_t cam_bringup(void)
{
  uint32_t  cpuclk0_hz = 0U;
  ra8_err_t err        = ra8_cgc_init();
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz);
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_mstp_init();
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_time_init(cpuclk0_hz);
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_board_uart_console_init((uint32_t)k_cam_baud);
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_sdramc_init();
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_board_camera_xclk_start((uint32_t)k_cam_xclk_hz);
  if (err != k_ra8_ok) {
    return err;
  }
  /* Force only SW4-6 ON so DVP reaches the sensor without overriding the
     physical settings used by unrelated board peripherals. */
  err = ra8_board_camera_select_parallel();
  if (err != k_ra8_ok) {
    return err;
  }
  ra8_delay_ms((uint32_t)k_cam_mode_settle_ms);
  return k_ra8_ok;
}

/**
 * @brief Print horizontal-sync pulse diagnostics.
 *
 * @param[in] probe Completed DVP sync/data probe.
 * @pre `probe` is non-NULL and the console is initialized.
 * @post HSYNC level and cycle fields are printed.
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
static void cam_print_hsync_probe(const cam_ceu_sync_probe_t* probe)
{
  (void)cam_puts(" h=");
  (void)cam_put_u32(probe->hsync_high_min);
  (void)cam_puts("-");
  (void)cam_put_u32(probe->hsync_high_max);
  (void)cam_puts(" l=");
  (void)cam_put_u32(probe->hsync_low_min);
  (void)cam_puts("-");
  (void)cam_put_u32(probe->hsync_low_max);
  (void)cam_puts(" hc=");
  (void)cam_put_u32(probe->hsync_high_cycles_min);
  (void)cam_puts("-");
  (void)cam_put_u32(probe->hsync_high_cycles_max);
}

/**
 * @brief Print pixel-clock and sampled data diagnostics.
 *
 * @param[in] probe Completed DVP sync/data probe.
 * @pre `probe` is non-NULL and the console is initialized.
 * @post Pixel-clock and data fields are printed.
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
static void cam_print_data_probe(const cam_ceu_sync_probe_t* probe)
{
  (void)cam_puts(" pc=");
  (void)cam_put_u32(probe->pclk_edges);
  (void)cam_puts(" ph=");
  (void)cam_put_u32(probe->pclk_half_cycles_min);
  (void)cam_puts(" data=");
  (void)cam_put_u32(probe->data_samples);
  (void)cam_puts("/");
  (void)cam_put_u32(probe->data_changes);
  (void)cam_puts(" ");
  (void)cam_put_hex((uint32_t)probe->data_min, 2U);
  (void)cam_puts("-");
  (void)cam_put_hex((uint32_t)probe->data_max, 2U);
  (void)cam_puts(" &");
  (void)cam_put_hex((uint32_t)probe->data_and, 2U);
  (void)cam_puts(" |");
  (void)cam_put_hex((uint32_t)probe->data_or, 2U);
}

/**
 * @brief Print per-line pixel-clock diagnostics.
 *
 * @param[in] probe Completed DVP sync/data probe.
 * @pre `probe` is non-NULL and the console is initialized.
 * @post Line-count and line-clock fields are printed.
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
static void cam_print_line_probe(const cam_ceu_sync_probe_t* probe)
{
  (void)cam_puts(" lineclk=");
  (void)cam_put_u32(probe->measured_lines);
  (void)cam_puts("/");
  (void)cam_put_u32(probe->line_pclk_min);
  (void)cam_puts("-");
  (void)cam_put_u32(probe->line_pclk_max);
  (void)cam_puts("/");
  (void)cam_put_u32(probe->line_pclk_mean);
  (void)cam_puts(" long=");
  (void)cam_put_u32(probe->line_pclk_long);
}

/**
 * @brief Print the complete GPIO-observed DVP diagnostic snapshot.
 *
 * @param[in] probe Completed DVP sync/data probe.
 * @pre `probe` is non-NULL.
 * @pre The SCI8 console is initialized.
 * @post Every field in `probe` is represented in the banner.
 * @post `probe` is not modified.
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
static void cam_print_sync_probe(const cam_ceu_sync_probe_t* probe)
{
  (void)cam_puts(" dvp_sync_edges=");
  (void)cam_put_u32(probe->vsync_edges);
  (void)cam_puts("/");
  (void)cam_put_u32(probe->hsync_edges);
  cam_print_hsync_probe(probe);
  cam_print_data_probe(probe);
  cam_print_line_probe(probe);
}

/**
 * @brief Configure the sensor, sample DVP, and prepare the CEU.
 *
 * @return Status of the first failed preparation stage.
 * @retval k_ra8_ok Sensor, pin routing, and CEU are ready.
 * @pre The camera chip ID was confirmed.
 * @pre Camera pins are not owned by another peripheral.
 * @post The CEU readiness field has been printed.
 * @post Success leaves the CEU ready for a single capture.
 * @note Thread safety: init context only.
 * @since 0.1.0
 */
static ra8_err_t cam_prepare_capture(void)
{
  s_camera_frame = (ra8_camera_frame_t){};
  ra8_err_t err  = ra8_ov5640_configure(&s_camera_sensor, k_ra8_ov5640_mode_vga_uyvy);
  if (err == k_ra8_ok) {
    cam_ceu_sync_probe_t sync_probe = {};
    err                             = cam_probe_sync_activity(&sync_probe);
    cam_print_sync_probe(&sync_probe);
  }
  if (err == k_ra8_ok) {
    err = ra8_board_camera_route_parallel_pins();
  }
  if (err == k_ra8_ok) {
    err = cam_capture_source_bind();
  }
  (void)cam_puts(" ceu=");
  (void)cam_puts((err == k_ra8_ok) ? "OK" : "ERR");
  return err;
}

/**
 * @brief Attempt one frame capture and print CEU completion diagnostics.
 *
 * @param[in] ready Whether CEU preparation completed successfully.
 * @return true when one complete frame was captured.
 * @retval true CEU capture completed.
 * @retval false Preparation failed or capture timed out.
 * @pre The console is initialized.
 * @pre `ready` reflects ::cam_prepare_capture.
 * @post Frame byte count and raw CETCR are printed.
 * @post `g_cam_frame_ok` reflects the returned value.
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
static bool cam_capture_frame_and_report(bool ready)
{
  bool frame_ok = false;
  if (ready) {
    const ra8_camera_buffer_t capture = {
      .data     = s_camera_capture,
      .capacity = (uint32_t)sizeof(s_camera_capture),
    };
    frame_ok = (ra8_camera_source_capture(&s_camera_source, &capture, &s_camera_frame) == k_ra8_ok);
  }
  g_cam_frame_ok = frame_ok ? 1U : 0U;
  (void)cam_puts(" bytes=");
  (void)cam_put_u32(s_camera_frame.bytes);
  (void)cam_puts(" frame=");
  (void)cam_puts(frame_ok ? "OK" : "ERR");

  /* Emit CETCR so missing sync and invalid timing remain diagnosable. */
  uint32_t cetcr = 0U;
  (void)ra8_camera_source_ceu_get_last_events(&s_camera_source_state, &cetcr);
  (void)cam_puts(" cetcr=");
  (void)cam_put_hex(cetcr, 8U);
  return frame_ok;
}

/**
 * @brief Validate, convert, and report the completed camera frame.
 *
 * @param[in] frame_ok Whether CEU capture completed.
 * @pre The console is initialized.
 * @pre `frame_ok` reflects ::cam_capture_frame_and_report.
 * @post Frame statistics, RGB status, and verdict are printed.
 * @post `g_cam_verdict` is true only for a plausible converted frame.
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
static void cam_finish_verdict(bool frame_ok)
{
  bool plausible = false;
  bool rgb_ok    = false;
  if (frame_ok) {
    plausible = cam_frame_is_plausible(&s_camera_frame);
    if (plausible) {
      rgb_ok = (cam_image_generate(s_camera_frame.data, s_camera_frame.bytes) == k_ra8_ok);
    }
  }
  (void)cam_puts(" min=");
  (void)cam_put_u32(g_cam_min);
  (void)cam_puts(" max=");
  (void)cam_put_u32(g_cam_max);
  (void)cam_puts(" mean=");
  (void)cam_put_u32(g_cam_mean);
  (void)cam_puts(" rgb=");
  (void)cam_puts(rgb_ok ? "OK" : "ERR");
  (void)cam_puts(" rgb_bytes=");
  (void)cam_put_u32(g_cam_rgb_frame_bytes);
  g_cam_verdict = (plausible && rgb_ok) ? 1U : 0U;
  (void)cam_puts((plausible && rgb_ok) ? " verdict=PASS\r\n" : " verdict=FAIL\r\n");
}

/**
 * @brief Configure the sensor, capture one frame, and print the verdict.
 *
 * @details Called only after the OV5640 chip ID is confirmed. Emits the
 *          the DVP probe, CEU status, byte count, frame statistics, and verdict
 *          banner fields. PASS also requires all four RGB SDRAM views.
 *
 * @pre `cam_probe_sensor` returned true (SCCB reaches the OV5640).
 * @pre The sensor is out of reset; this function probes then routes DVP pins.
 * @post One verdict field has been written to SCI8.
 * @post `g_cam_frame_ok` / `g_cam_verdict` reflect the outcome.
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
static void cam_capture_and_verdict(void)
{
  const ra8_err_t prep_err = cam_prepare_capture();
  const bool      frame_ok = cam_capture_frame_and_report(prep_err == k_ra8_ok);
  cam_finish_verdict(frame_ok);
}

/**
 * @brief Run the capture pipeline and emit the plausibility banner.
 *
 * @details Reports progressively so a single flash reveals exactly which
 *          stage the hardware reached. The verdict is PASS only when the
 *          sensor ID is 0x5640, a frame was captured, it is non-degenerate,
 *          and the four RGB SDRAM views are complete.
 *
 * @pre `cam_bringup` returned ok (console + XCLK + SCCB live).
 * @pre The application owns the camera globals and static capture storage.
 * @post A `cam: ...verdict=...` line has been written to SCI8.
 * @post The result globals reflect the run.
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
static void cam_run(void)
{
  (void)cam_puts((const char*)k_cam_tag);

  /* Confirm the XCLK GPT is actually counting (a proxy that the P501
     XVCLK square wave is live). The GTCNT wraps every few counts at the
     ~24 MHz output rate, so sample it in a tight loop and call it live as
     soon as any read differs from the first; a stuck GTCNT (every sample
     identical) means the sensor never gets a clock. */
  uint32_t cnt_ref     = 0U;
  bool     gpt_running = false;
  (void)ra8_gpt_read((uint8_t)k_cam_xclk_gpt_ch, &cnt_ref);
  for (uint32_t i = 0U; i < (uint32_t)k_cam_gpt_probe_iters; i += 1U) {
    uint32_t cnt = 0U;
    (void)ra8_gpt_read((uint8_t)k_cam_xclk_gpt_ch, &cnt);
    if (cnt != cnt_ref) {
      gpt_running = true;
      break;
    }
  }
  (void)cam_puts(" gpt=");
  (void)cam_puts(gpt_running ? "RUN" : "DEAD");

  /* The OV5640 needs XVCLK (up in cam_bringup) and must be out of
     hardware reset before it answers on SCCB, so release RST (P709)
     before scanning and probing the sensor. */
  ra8_err_t rst_err = ra8_board_camera_reset();
  if (rst_err == k_ra8_ok) {
    rst_err = cam_sensor_bind();
  }

  cam_bus_scan();

  uint16_t   chip = 0U;
  const bool id_ok =
    (rst_err == k_ra8_ok) && (ra8_ov5640_probe(&s_camera_sensor, &chip) == k_ra8_ok);
  g_cam_chipid = (uint32_t)chip;
  (void)cam_puts(" chipid=");
  (void)cam_put_hex((uint32_t)chip, 4U);
  (void)cam_puts(" xclk=OK rst=");
  (void)cam_puts((rst_err == k_ra8_ok) ? "OK" : "ERR");
  (void)cam_puts(" sccb=");
  (void)cam_puts(id_ok ? "OK" : "ERR");

  if (!id_ok) {
    (void)cam_puts(" verdict=FAIL\r\n");
    return;
  }

  cam_capture_and_verdict();
}

/**
 * @brief Application entry: OV5640 CEU capture plausibility self-test.
 *
 * @pre Reset_Handler copied .data and zeroed .bss.
 * @pre SystemInit set VTOR, FPU and priority grouping.
 * @post The verdict banner is on SCI8 and the CPU parks.
 * @post The result globals hold the outcome for SWD probing.
 * @since 0.1.0
 */
void main(void)
{
  if (cam_bringup() != k_ra8_ok) {
    (void)cam_puts("cam: bringup ERROR verdict=FAIL\r\n");
    cam_park();
    return;
  }
  ra8_isr_globals_enable();
  cam_run();
  cam_park();
}
