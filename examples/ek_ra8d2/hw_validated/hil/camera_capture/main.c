/**
 * @file examples/ek_ra8d2/hw_validated/hil/camera_capture/main.c
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
 *   7. `ra8_ceu_init` + `ra8_ceu_capture_start_ex` capture one packed frame
 *      into internal SRAM; the driver polls CETCR.CPE for completion.
 *   8. Plausibility stats over the frame, then firmware-side UYVY-to-RGB888
 *      conversion into four cache-cleaned SDRAM buffers at 0/90/180/270.
 *      PASS requires a captured, non-degenerate frame and all four images.
 *
 * The sensor SCCB half (reset / probe / configure + the register table)
 * lives in `cam_ov5640.{c,h}`; the CEU half (DVP pin routing, descriptor,
 * arm/poll + frame buffer) lives in `cam_ceu.{c,h}`. This file owns the
 * app flow, the GPT XCLK, the console, and the banner/verdict logic.
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
#include "cam_ov5640.h"
#include "ra8_board_ek_ra8d2.h"
#include "ra8_cgc.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_gpt.h"
#include "ra8_i2c.h"
#include "ra8_isr.h"
#include "ra8_mstp.h"
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
  k_cam_dec_cap         = 99999999U, /**< Max value cam_put_u32 can render.      */
  k_cam_gpt_period_max  = 0xFFFFU,   /**< Max GPT period (16-bit GTPR).          */
  k_cam_gpt_probe_iters = 64U,       /**< GTCNT samples to prove the timer runs. */
} cam_u32_t;

/** @brief 8-bit app constants: hex/decimal printing, bus scan, U15 expander. */
typedef enum : uint8_t {
  k_cam_nibble_mask = 0x0FU, /**< Low-nibble mask for hex printing.     */
  k_cam_dec_base    = 10U,   /**< Base for decimal printing.            */
  k_cam_byte_max    = 0xFFU, /**< Maximum byte value (min-scan seed).   */
  k_cam_scan_lo     = 0x08U, /**< First 7-bit address in the bus scan.  */
  k_cam_scan_hi     = 0x77U, /**< Last 7-bit address in the bus scan.   */
  k_cam_sw46_output = 0xDFU, /**< U15 latch: SW4-6 ON, other bits high. */
  k_cam_sw46_mask   = 0x20U, /**< Drive SW4-6 only; release all others. */
} cam_u8_t;

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

/* =============================================================================
 * Console helpers
 * =============================================================================
 */

/**
 * @brief Bounded length of a NUL-terminated C string.
 *
 * @param[in] s Non-NULL C string.
 * @return Length in bytes, capped at ::k_cam_frame_bytes.
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
  while ((n < (uint32_t)k_cam_frame_bytes) && (s[n] != '\0')) {
    n += 1U;
  }
  return n;
}

/**
 * @brief Write a NUL-terminated string to the SCI8 console.
 *
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
 *
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
 *
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
 * XCLK generation (GPT12 saw-PWM on P501)
 * =============================================================================
 */

/**
 * @brief Start the ~24 MHz XVCLK on GTIOC12A (P501) via GPT channel 12.
 *
 * @details Programs a saw-wave PWM whose period divides PCLKD down to the
 *          XVCLK target, enables the GTIOC12A output, and routes P501 to
 *          the GPT peripheral. The OV5640 needs this clock before any
 *          SCCB access.
 *
 * @return ra8_err_t; ok when the clock is toggling on P501.
 * @retval k_ra8_ok XVCLK is running.
 * @retval k_ra8_err_invalid_arg PCLKD readback gave an unusable divisor.
 *
 * @pre `ra8_cgc_init` + `ra8_mstp_init` have run.
 * @pre P501 is free (not muxed to the audio codec).
 * @post GPT12 counts and drives a ~24 MHz square wave on P501.
 * @post P501 carries the GTIOC12A function.
 * @note Thread safety: init context only.
 * @since 0.1.0
 */
static ra8_err_t cam_start_xclk(void)
{
  uint32_t  pclkd_hz = 0U;
  ra8_err_t err      = ra8_cgc_get_clock_hz(k_ra8_clock_id_pclkd, &pclkd_hz);
  if (err != k_ra8_ok) {
    return err;
  }
  const uint32_t period_counts = pclkd_hz / (uint32_t)k_cam_xclk_hz;
  RA8_CHECK_RANGE(period_counts, 2U, (uint32_t)k_cam_gpt_period_max, k_ra8_err_invalid_arg);
  const ra8_gpt_cfg_t cfg = {
    .mode       = k_ra8_gpt_mode_saw_pwm,
    .prescaler  = k_ra8_gpt_ps_div_1,
    .period     = period_counts - 1U,
    .duty_a     = period_counts / 2U,
    .duty_b     = 0U,
    .auto_start = true,
  };
  err = ra8_gpt_init((uint8_t)k_cam_xclk_gpt_ch, &cfg);
  if (err != k_ra8_ok) {
    return err;
  }
  const ra8_gpt_pwm_pin_cfg_t pin = {
    .output_enable    = true,
    .polarity         = k_ra8_gpt_pol_active_high,
    .stop_level       = k_ra8_gpt_stop_low,
    .disable_on_fault = k_ra8_gpt_disable_none,
  };
  err = ra8_gpt_pwm_pin_configure((uint8_t)k_cam_xclk_gpt_ch, k_ra8_gpt_pin_a, &pin);
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_pfs_route_peripheral(RA8_PIN(k_ra8_port_5, k_ra8_pin_1), k_ra8_psel_gpt0, "cam.xclk");
  if (err != k_ra8_ok) {
    return err;
  }
  return ra8_pfs_set_drive_strength(RA8_PIN(k_ra8_port_5, k_ra8_pin_1),
                                    k_ra8_pfs_dscr_high_speed_high);
}

/* =============================================================================
 * Frame plausibility
 * =============================================================================
 */

/**
 * @brief Compute min / max / mean over the CEU frame and store to globals.
 *
 * @return true when the frame is non-degenerate (max != min).
 * @retval true  Frame varies, consistent with a live non-degenerate capture.
 * @retval false Frame is all-identical, empty, or the buffer was unavailable.
 *
 * @pre ::cam_capture_one has filled the CEU frame buffer.
 * @pre ::k_cam_frame_bytes is non-zero.
 * @post `g_cam_min` / `g_cam_max` / `g_cam_mean` reflect the frame.
 * @post Return distinguishes a live grab from an all-identical buffer.
 * @note Thread safety: reads shared CEU state; call after capture only.
 * @since 0.1.0
 */
static bool cam_frame_is_plausible(void)
{
  const uint8_t* frame = cam_ceu_frame();
  if (frame == nullptr) {
    return false;
  }
  const uint32_t n = (uint32_t)k_cam_frame_bytes;
  if (n == 0U) {
    return false;
  }
  uint32_t lo  = (uint32_t)k_cam_byte_max;
  uint32_t hi  = 0U;
  uint32_t sum = 0U;
  for (uint32_t i = 0U; i < n; i += 1U) {
    const uint32_t b = (uint32_t)frame[i];
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
    if (ra8_i2c_scan((uint8_t)k_cam_iic_ch, a, &acked) != k_ra8_ok) {
      continue;
    }
    if (!acked) {
      continue;
    }
    (void)cam_put_hex((uint32_t)a, 2U);
    /* Read register 0 (8-bit pointer) as a coarse device-ID fingerprint. */
    const uint8_t reg = 0U;
    uint8_t       v   = 0U;
    if (ra8_i2c_transfer((uint8_t)k_cam_iic_ch, a, &reg, 1U, &v, 1U) == k_ra8_ok) {
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
 *  @pre The verdict banner has been emitted.
 *  @post The CPU idles; only reset or a debugger wakes it.
 *  @since 0.1.0 */
static void cam_park(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Force the Camera Expansion Board into parallel (DVP) mode.
 *
 * @details The board multiplexes the camera FFC (J35) between MIPI-CSI
 *          (SW4-6 OFF, the board default) and parallel DVP (SW4-6 ON) via
 *          the U15 PI4IOE5V6408 SW4-override expander. In MIPI mode the
 *          P501 XCLK is not routed to the sensor, so the OV5640 stays
 *          unclocked and never answers SCCB. This clears U15 output bit 5
 *          (SW4-6 -> ON) so the DVP path -- and the P501 XVCLK -- reach the
 *          sensor, without disturbing the other SW4 overrides.
 *
 * @return ra8_err_t; ok when U15 latched SW4-6 = ON.
 * @retval k_ra8_ok Parallel-camera routing selected.
 * @retval k_ra8_err_nack U15 did not ACK.
 *
 * @pre `ra8_mstp_init` has run and the Camera Expansion Board is on J35.
 * @pre The Camera Expansion Board is on J35.
 * @post SW4-6 reads ON; the DVP data/clock path is live.
 * @post U15's other SW4 overrides are unchanged.
 * @note Thread safety: init context only.
 * @since 0.1.0
 */
static ra8_err_t cam_select_parallel_camera(void)
{
  return ra8_board_io_expander_apply_sw4_mask((uint8_t)k_cam_sw46_output, (uint8_t)k_cam_sw46_mask);
}

/**
 * @brief Bring up clocks, console, SDRAM, XCLK, SCCB and parallel camera.
 *
 * @return ra8_err_t from the first failing bring-up step, or ok.
 * @retval k_ra8_ok Console, SDRAM, XCLK and I2C ch1 are live; SW4-6 = ON.
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
  err = cam_start_xclk();
  if (err != k_ra8_ok) {
    return err;
  }
  /* Force only SW4-6 ON so DVP reaches the sensor without overriding the
     physical settings used by unrelated board peripherals. */
  err = cam_select_parallel_camera();
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
  ra8_err_t err = cam_configure_sensor();
  if (err == k_ra8_ok) {
    cam_ceu_sync_probe_t sync_probe = {};
    err                             = cam_probe_sync_activity(&sync_probe);
    cam_print_sync_probe(&sync_probe);
  }
  if (err == k_ra8_ok) {
    err = cam_route_ceu_pins();
  }
  if (err == k_ra8_ok) {
    err = cam_ceu_setup();
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
    frame_ok = (cam_capture_one() == k_ra8_ok);
  }
  g_cam_frame_ok = frame_ok ? 1U : 0U;
  (void)cam_puts(" bytes=");
  (void)cam_put_u32(cam_ceu_capture_bytes());
  (void)cam_puts(" frame=");
  (void)cam_puts(frame_ok ? "OK" : "TIMEOUT");

  /* Emit CETCR so missing sync and invalid timing remain diagnosable. */
  uint32_t cetcr = 0U;
  (void)cam_ceu_get_status(&cetcr);
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
    plausible = cam_frame_is_plausible();
    if (plausible) {
      rgb_ok = (cam_image_generate(cam_ceu_frame(), cam_ceu_capture_bytes()) == k_ra8_ok);
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
  const ra8_err_t rst_err = cam_reset_sensor();

  cam_bus_scan();

  uint16_t   chip  = 0U;
  const bool id_ok = cam_probe_sensor(&chip);
  g_cam_chipid     = (uint32_t)chip;
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

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
/**
 * @brief Application entry: OV5640 CEU capture plausibility self-test.
 *
 * @return Never returns.
 *
 * @pre Reset_Handler copied .data and zeroed .bss.
 * @pre SystemInit set VTOR, FPU and priority grouping.
 * @post The verdict banner is on SCI8 and the CPU parks.
 * @post The result globals hold the outcome for SWD probing.
 * @since 0.1.0
 */
int32_t main(void)
{
  if (cam_bringup() != k_ra8_ok) {
    (void)cam_puts("cam: bringup ERROR verdict=FAIL\r\n");
    cam_park();
    return 0;
  }
  ra8_isr_globals_enable();
  cam_run();
  cam_park();
  return 0;
}
#pragma GCC diagnostic pop
