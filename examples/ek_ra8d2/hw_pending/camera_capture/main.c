/**
 * @file examples/ek_ra8d2/hw_pending/camera_capture/main.c
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
 *   5. Confirm the XCLK GPT counter advances, route the CEU DVP pins
 *      (VIO_D[7:0], VIO_VD, VIO_HD, VIO_CLK), release the sensor RST
 *      strap (P709), then scan the bus and read the OV5640 chip-ID
 *      registers 0x300A/0x300B -- the VERIFY-FIRST proof the sensor is
 *      present (expected 0x5640), trying SCCB 0x3C then 0x3D.
 *   6. SCCB register sequence: software reset + a compact DVP YUV422
 *      QVGA config with the built-in colour-bar test pattern enabled,
 *      so the captured frame is deterministic and independent of lens
 *      focus or scene light.
 *   7. `ra8_ceu_init` + `ra8_ceu_capture_arm` capture one frame into an
 *      internal SRAM buffer; the driver polls CETCR.CPE for completion.
 *   8. Plausibility stats over the frame: min / max / mean byte. The
 *      verdict is PASS when the sensor ID is 0x5640, a frame was
 *      captured, and the frame is non-degenerate (max != min) -- a
 *      colour bar spans black..white so a real grab always varies,
 *      while a dead bus reads all-identical bytes.
 *
 * The sensor SCCB half (reset / probe / configure + the register table)
 * lives in `cam_ov5640.{c,h}`; the CEU half (DVP pin routing, descriptor,
 * arm/poll + frame buffer) lives in `cam_ceu.{c,h}`. This file owns the
 * app flow, the GPT XCLK, the console, and the banner/verdict logic.
 *
 * Banner (scraped by the HIL gate once promoted):
 *   `cam: gpt=RUN scan=3C:56.43:00. chipid=5640 xclk=OK rst=OK sccb=OK`
 *   ` ceu=OK frame=OK min=.. max=.. mean=.. verdict=PASS`
 *
 * Hardware: EK-RA8D2 with the OV5640 Camera Expansion Board on the
 * underside FFC port (J35). SW4-6 is driven ON in firmware; no manual
 * jumpers required.
 *
 * @note Bench status (silicon, SWD forensics): SCCB works and the chip
 *       ID reads 0x5640; the OV5640 fully streams over the parallel port
 *       -- VIO_VD, VIO_HD, VIO_CLK and VIO_D[7:0] were all confirmed
 *       toggling on the MCU pins (PORT PIDR sampling). The remaining
 *       blocker is the CEU: every armed frame ends with CETCR IGHS
 *       (bit17, "HD clock-cycle count differs from CMCYR.HCYL") followed
 *       by VBP (bit20, "invalid VD"), and zero bytes are written. No
 *       HCYL value from 2..2560 clears IGHS, so the VIO_HD active-cycle
 *       count the CEU sees is not a stable/matchable width. Resolving
 *       this needs a logic analyzer on VIO_HD / VIO_CLK to measure the
 *       real HREF-active duration and the PCLK/HREF phase; it cannot be
 *       fixed by register config alone. See the README.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "cam_ceu.h"
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
  k_cam_nibble_mask    = 0x0FU, /**< Low-nibble mask for hex printing.       */
  k_cam_dec_base       = 10U,   /**< Base for decimal printing.              */
  k_cam_byte_max       = 0xFFU, /**< Maximum byte value (min-scan seed).     */
  k_cam_scan_lo        = 0x08U, /**< First 7-bit address in the bus scan.    */
  k_cam_scan_hi        = 0x77U, /**< Last 7-bit address in the bus scan.     */
  k_cam_u15_addr       = 0x43U, /**< U15 PI4IOE5V6408 SW4-override address.  */
  k_cam_u15_reg_output = 0x05U, /**< U15 output register (1 = SW4 OFF).      */
  k_cam_sw46_bit       = 5U,    /**< U15 output bit for SW4-6 (camera mode). */
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
  return ra8_pfs_route_peripheral(RA8_PIN(k_ra8_port_5, k_ra8_pin_1), k_ra8_psel_gpt0, "cam.xclk");
}

/* =============================================================================
 * Frame plausibility
 * =============================================================================
 */

/**
 * @brief Compute min / max / mean over the CEU frame and store to globals.
 *
 * @return true when the frame is non-degenerate (max != min).
 * @retval true  Frame varies -- a real colour-bar grab spans black..white.
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
 * @pre RIIC ch1 up and `ra8_board_io_expander_apply_project_sw4_defaults`
 *      has configured U15 as all-outputs.
 * @pre The Camera Expansion Board is on J35.
 * @post SW4-6 reads ON; the DVP data/clock path is live.
 * @post U15's other SW4 overrides are unchanged.
 * @note Thread safety: init context only.
 * @since 0.1.0
 */
static ra8_err_t cam_select_parallel_camera(void)
{
  const uint8_t reg = (uint8_t)k_cam_u15_reg_output;
  uint8_t       cur = 0U;
  ra8_err_t     err =
    ra8_i2c_transfer((uint8_t)k_cam_iic_ch, (uint8_t)k_cam_u15_addr, &reg, 1U, &cur, 1U);
  if (err != k_ra8_ok) {
    return err;
  }
  const uint8_t want       = (uint8_t)(cur & (uint8_t)(~(1U << (uint8_t)k_cam_sw46_bit)));
  const uint8_t payload[2] = {reg, want};
  return ra8_i2c_write((uint8_t)k_cam_iic_ch,
                       (uint8_t)k_cam_u15_addr,
                       payload,
                       (uint32_t)sizeof(payload),
                       true);
}

/**
 * @brief Bring up clocks, console, XCLK, SCCB and select parallel camera.
 *
 * @return ra8_err_t from the first failing bring-up step, or ok.
 * @retval k_ra8_ok Console, XCLK and I2C ch1 are live; SW4-6 = ON.
 *
 * @pre Reset_Handler + SystemInit ran.
 * @pre Single-threaded init context.
 * @post On ok the console prints and the OV5640 has XVCLK + SCCB.
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
  err = cam_start_xclk();
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_board_io_expander_apply_project_sw4_defaults();
  if (err != k_ra8_ok) {
    return err;
  }
  /* The board default is SW4-6 OFF (MIPI camera); flip it ON so the DVP
     path + the P501 XVCLK reach the sensor. */
  err = cam_select_parallel_camera();
  if (err != k_ra8_ok) {
    return err;
  }
  ra8_delay_ms((uint32_t)k_cam_mode_settle_ms);
  return k_ra8_ok;
}

/**
 * @brief Configure the sensor, capture one frame, and print the verdict.
 *
 * @details Called only after the OV5640 chip ID is confirmed. Emits the
 *          ` ceu= frame= min= max= mean= verdict=` banner fields and sets
 *          the result globals. PASS requires a captured, non-degenerate
 *          frame.
 *
 * @pre `cam_probe_sensor` returned true (SCCB reaches the OV5640).
 * @pre The CEU DVP pins are routed and the sensor is out of reset.
 * @post One verdict field has been written to SCI8.
 * @post `g_cam_frame_ok` / `g_cam_verdict` reflect the outcome.
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
static void cam_capture_and_verdict(void)
{
  ra8_err_t err = cam_configure_sensor();
  if (err == k_ra8_ok) {
    err = cam_ceu_setup();
  }
  (void)cam_puts(" ceu=");
  (void)cam_puts((err == k_ra8_ok) ? "OK" : "ERR");

  bool frame_ok = false;
  if (err == k_ra8_ok) {
    frame_ok = (cam_capture_one() == k_ra8_ok);
  }
  g_cam_frame_ok = frame_ok ? 1U : 0U;
  (void)cam_puts(" frame=");
  (void)cam_puts(frame_ok ? "OK" : "TIMEOUT");

  /* Emit the raw CEU event register (CETCR) so a timeout is diagnosable
     from the banner alone: bit17=IGHS (HD cycle-count mismatch), bit20=VBP
     (invalid VD), bit24/25=NHD/NVD (sync missing), bit0=CPE (frame done). */
  uint32_t cetcr = 0U;
  (void)cam_ceu_get_status(&cetcr);
  (void)cam_puts(" cetcr=");
  (void)cam_put_hex(cetcr, 8U);

  bool plausible = false;
  if (frame_ok) {
    plausible = cam_frame_is_plausible();
  }
  (void)cam_puts(" min=");
  (void)cam_put_u32(g_cam_min);
  (void)cam_puts(" max=");
  (void)cam_put_u32(g_cam_max);
  (void)cam_puts(" mean=");
  (void)cam_put_u32(g_cam_mean);

  g_cam_verdict = plausible ? 1U : 0U;
  (void)cam_puts(plausible ? " verdict=PASS\r\n" : " verdict=FAIL\r\n");
}

/**
 * @brief Run the capture pipeline and emit the plausibility banner.
 *
 * @details Reports progressively so a single flash reveals exactly which
 *          stage the hardware reached. The verdict is PASS only when the
 *          sensor ID is 0x5640, a frame was captured, and it is
 *          non-degenerate.
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
     hardware reset before it answers on SCCB, so route the DVP pins and
     release RST (P709) FIRST, then probe. */
  (void)cam_route_ceu_pins();
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
