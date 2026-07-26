/**
 * @file examples/ek_ra8d2/hil_needs_revalidation/touch_cal/main.c
 * @brief N-point affine touch-calibration demo + HIL gate (#262).
 *
 * @par Tag
 * [Ring 7 / App] {World: NS}
 *
 * @details
 * `ra8_touch_cal` (the weighted-least-squares affine solver from
 * `libs/ra8_touch_cal`) had no standalone example. This app is that example and
 * its CI gate: it wires the solver end-to-end over the REAL GoodIX GT911 touch
 * driver (`ra8_touch` -> IIC_B) and the REAL GLCDC layer-1 display path
 * (`ra8_display_pal` -> `ra8_glcdc`), and runs the full five-point calibration:
 *
 *   1. Bring up clocks / MSTP / time + the SCI8 J-Link OB console.
 *   2. Bring up the GLCDC panel through the display PAL, scanning a 512x512
 *      RGB565 SRAM framebuffer (the same size `glcdc_render` uses -- the full
 *      1024x600 panel buffer does not fit on-chip and SDRAM is a separate task).
 *   3. Bring up the GT911 over IIC_B (I2C-compat), exactly as `touch_demo`.
 *   4. Drive `ra8_touch_cal_run`: for each of the five built-in targets (four
 *      inset corners + centre) it paints a cross-hair on the panel and blocks
 *      on one settled raw GT911 sample. The two shims (`tc_draw_target`,
 *      `tc_read_raw`) are the SOLID-D seams `ra8_touch_cal` inverts onto: the
 *      draw shim renders into the framebuffer and the read shim polls the real
 *      `ra8_touch_read`. Each shim also records the target / raw pair so the
 *      verify pass can measure the residual.
 *   5. On a successful solve: serialise the matrix with `ra8_touch_cal_save`,
 *      round-trip it back with `ra8_touch_cal_load`, re-apply the matrix to
 *      every captured raw sample with `ra8_touch_cal_apply`, and paint a
 *      corrected cross-hair at each mapped pixel (the "live cross-hair proving
 *      corrected coordinates" -- it lands on its target). The largest
 *      target-to-corrected residual is the fit error.
 *   6. Emit the outcome on the console:
 *
 *        `touchcal: cal=OK verify=OK maxerr=<n>`   (all five taps collected)
 *        `touchcal: cal=SKIP got=<k>`              (no operator / no taps)
 *
 *      then, unconditionally, the finger-free bring-up sentinel:
 *
 *        `touchcal: ready dim=512x512`
 *
 * SIM==HIL discipline (the owner rule): the calibration solve is only reachable
 * once five distinct raw samples arrive. On real silicon a human taps the five
 * cross-hairs. `board_sim` reproduces that by feeding five synthetic raw points
 * through the modelled GT911 (`--touch-seq`, declared in this app's `hil.conf`
 * as `HIL_SIM_ARGS`); those points return through the genuine `ra8_touch_read`
 * decode, so the banner carries a real solved+verified result with no board
 * attached. On a bare automated bench with no finger the read shim times out and
 * the app reports `cal=SKIP` -- so `hil.conf` asserts only the finger-free
 * `touchcal: ready` line (which prints in every environment) and lists
 * `verify=FAIL` / `cal=FAIL` in its negative set, so a solver regression trips
 * the SIL gate. This mirrors `touch_demo`, which likewise gates the finger-free
 * bring-up and lets the simulator inject the touch.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_cgc.h"
#include "ra8_display_pal.h"
#include "ra8_display_pal_lcd.h"
#include "ra8_err.h"
#include "ra8_glcdc.h"
#include "ra8_i2c_bus_ops.h"
#include "ra8_i3c.h"
#include "ra8_io_i2c_bus.h"
#include "ra8_io_i2c_bus_i3c_compat.h"
#include "ra8_isr.h"
#include "ra8_mstp.h"
#include "ra8_panel_timing.h"
#include "ra8_time.h"
#include "ra8_touch.h"
#include "ra8_touch_cal.h"

/**
 * @enum tc_geom_t
 * @brief Framebuffer + calibration geometry (no magic numbers).
 *
 * @details
 * The calibration screen space is the 512x512 SRAM framebuffer, so the affine
 * transform maps GT911 raw units onto this 512x512 pixel grid. ::k_tc_inset_px
 * is the corner-target margin passed to ``ra8_touch_cal_run`` (its four corner
 * targets sit this far in from each edge; the fifth is the panel centre).
 */
typedef enum : uint16_t {
  k_tc_fb_w       = 512U, /**< Framebuffer / screen width, pixels.        */
  k_tc_fb_h       = 512U, /**< Framebuffer / screen height, pixels.       */
  k_tc_inset_px   = 40U,  /**< Corner-target inset from each panel edge.  */
  k_tc_cross_half = 12U,  /**< Cross-hair arm half-length, pixels.        */
  k_tc_verify_tol = 2U,   /**< Max allowed fit residual, pixels.          */
  k_tc_fb_align   = 64U,  /**< AXI-burst alignment for clean GLCDC fetch. */
} tc_geom_t;

/**
 * @enum tc_color_t
 * @brief RGB565 palette for the calibration UI.
 */
typedef enum : uint16_t {
  k_tc_color_bg        = 0x0000U, /**< Black field.                     */
  k_tc_color_target    = 0xFFFFU, /**< White target cross-hair.         */
  k_tc_color_corrected = 0x07E0U, /**< Green corrected-coordinate mark. */
} tc_color_t;

/**
 * @enum tc_consts_t
 * @brief Console / GT911 / poll knobs (no magic numbers).
 */
typedef enum : uint32_t {
  k_tc_uart_baud  = 115200U,   /**< Console baud.                       */
  k_tc_i2c_chan   = 0U,        /**< IIC_B channel 0 (GT911 bus).        */
  k_tc_i2c_bus_hz = 400000U,   /**< Fast-mode I2C clock.                */
  k_tc_pclka_hz   = 60000000U, /**< IIC_B clock-source rate.            */
  k_tc_gt911_addr = 0x5DU,     /**< GT911 default 7-bit address.        */
  k_tc_max_points = 5U,        /**< Read up to the GT911 capacity.      */
  k_tc_poll_max   = 20000U,    /**< Bounded per-target poll (NASA R2).  */
  k_tc_dec_ten    = 10U,       /**< Decimal radix / small-buf cap.      */
  k_tc_blob_cap   = 36U,       /**< ::k_ra8_touch_cal_blob_size mirror. */
} tc_consts_t;

/**
 * @struct tc_capture_t
 * @brief Per-run capture the calibration shims fill for the verify pass.
 *
 * @details
 * ``ra8_touch_cal_run`` owns the target layout and the collect loop; it does
 * not hand the raw samples back. The draw shim records each target and the read
 * shim records each raw sample into this struct (shared as both shim contexts),
 * so once the matrix is solved the app can re-apply it to the same raws and
 * measure the residual.
 *
 * @invariant ``n_drawn`` and ``n_read`` never exceed
 *            ::k_ra8_touch_cal_n_targets.
 */
typedef struct {
  display_handle_t*     disp;                               /**< Live panel handle. */
  ra8_touch_cal_point_t targets[k_ra8_touch_cal_n_targets]; /**< Drawn targets.     */
  ra8_touch_cal_point_t raws[k_ra8_touch_cal_n_targets];    /**< Captured raws.     */
  uint8_t               n_drawn;                            /**< Targets drawn.     */
  uint8_t               n_read;                             /**< Raws captured.     */
} tc_capture_t;

/**
 * @brief Render target in internal SRAM, 64-byte AXI-burst aligned so the
 *        GLCDC's AXI fetches are clean.
 * @note Written by the calibration UI; scanned out continuously by GLCDC GR1.
 * @warning Not thread-safe; single-shot calibration owns it.
 * @since 0.1.0
 */
[[gnu::aligned(
  k_tc_fb_align)]] static uint16_t s_framebuffer[(uint32_t)k_tc_fb_w * (uint32_t)k_tc_fb_h];

/**
 * @var s_touch_bus
 * @brief Bound I2C bus handle the touch driver's injected seam points at.
 * @note Written once during bring-up, then read-only.
 * @warning Do not rebind while the touch driver is open.
 * @since 0.1.0
 */
static ra8_io_i2c_bus_t s_touch_bus;

/**
 * @var s_capture
 * @brief The single calibration capture the shims fill.
 * @note File-scope because the shim ``ctx`` references it for the whole run.
 * @warning Reset at the start of each calibration attempt.
 * @since 0.1.0
 */
static tc_capture_t s_capture;

/**
 * @brief Display PAL config selecting the GLCDC LCD backend.
 * @details The panel timing is the board BSP's EK-RA8D2 descriptor; re-pointing
 *          ``iface`` at another backend is the only change to target it instead.
 * @since 0.1.0
 */
static const display_cfg_t k_tc_display_cfg = {
  .iface             = &k_display_backend_lcd_ra8_glcdc,
  .framebuffer       = s_framebuffer,
  .framebuffer_bytes = sizeof(s_framebuffer),
  .width_px          = (uint16_t)k_tc_fb_w,
  .height_px         = (uint16_t)k_tc_fb_h,
  .pixfmt            = k_display_pixfmt_rgb565,
  .panel_timing      = &k_ra8_panel_ek_ra8d2_timing,
};

static const uint8_t k_tc_msg_boot[]    = "touchcal: boot\r\n";
static const uint8_t k_tc_msg_fail_in[] = "touchcal: FAIL init\r\n";
static const uint8_t k_tc_msg_fail_gl[] = "touchcal: FAIL glcdc\r\n";
static const uint8_t k_tc_msg_fail_op[] = "touchcal: FAIL open\r\n";
static const uint8_t k_tc_msg_tgt[]     = "touchcal: target ";
static const uint8_t k_tc_msg_cal_ok[]  = "touchcal: cal=OK verify=";
static const uint8_t k_tc_msg_ok[]      = "OK";
static const uint8_t k_tc_msg_bad[]     = "FAIL";
static const uint8_t k_tc_msg_maxerr[]  = " maxerr=";
static const uint8_t k_tc_msg_skip[]    = "touchcal: cal=SKIP got=";
static const uint8_t k_tc_msg_ready[]   = "touchcal: ready dim=512x512\r\n";
static const uint8_t k_tc_msg_comma[]   = ",";
static const uint8_t k_tc_msg_eol[]     = "\r\n";

/* ===========================================================================
 * Console
 * ===========================================================================
 */

/** @brief Emit a byte run on the SCI8 console. */
static void tc_print(const uint8_t* msg, uint32_t len)
{
  (void)ra8_board_uart_console_write(msg, (size_t)len);
}

/** @brief Print the fail banner and trap (board_sim halts on the BKPT). */
static void tc_panic_halt(const uint8_t* msg, uint32_t len)
{
  tc_print(msg, len);
  __asm__ volatile("bkpt #0");
  while (1) {
    __asm__ volatile("wfi");
  }
}

/** @brief Print a small unsigned integer in decimal. */
static void tc_print_uint(uint32_t value)
{
  uint8_t  buf[k_tc_dec_ten];
  uint32_t n = 0U;
  if (value == 0U) {
    buf[n] = (uint8_t)'0';
    n++;
  }
  while ((value > 0U) && (n < (uint32_t)k_tc_dec_ten)) {
    buf[n] = (uint8_t)('0' + (value % (uint32_t)k_tc_dec_ten));
    n++;
    value /= (uint32_t)k_tc_dec_ten;
  }
  for (uint32_t i = 0U; i < n; i++) {
    tc_print(&buf[n - 1U - i], 1U);
  }
}

/* ===========================================================================
 * Framebuffer cross-hair painting
 * ===========================================================================
 */

/**
 * @brief Report whether a framebuffer coordinate is inside the panel.
 *
 * @param[in] v    Coordinate under test (may be negative after a transform).
 * @param[in] dim  Panel extent on that axis (width or height).
 * @return true iff ``0 <= v < dim``.
 *
 * @pre ``dim`` is the framebuffer width or height (> 0).
 * @post No state is mutated.
 * @note Pure; safe from any context.
 * @since 0.1.0
 */
static bool tc_pixel_in_bounds(int32_t v, int32_t dim)
{
  return (v >= 0) && (v < dim);
}

/** @brief Write one RGB565 pixel, clipped to the framebuffer rectangle. */
static void tc_put_px(int32_t x, int32_t y, uint16_t color)
{
  if (tc_pixel_in_bounds(x, (int32_t)k_tc_fb_w) && tc_pixel_in_bounds(y, (int32_t)k_tc_fb_h)) {
    s_framebuffer[((uint32_t)y * (uint32_t)k_tc_fb_w) + (uint32_t)x] = color;
  }
}

/** @brief Fill the whole framebuffer with @p color. */
static void tc_fb_fill(uint16_t color)
{
  for (uint32_t i = 0U; i < (uint32_t)k_tc_fb_w * (uint32_t)k_tc_fb_h; i++) {
    s_framebuffer[i] = color;
  }
}

/** @brief Draw a plus-shaped cross-hair centred at (@p cx, @p cy). */
static void tc_draw_cross(int32_t cx, int32_t cy, uint16_t color)
{
  const int32_t half = (int32_t)k_tc_cross_half;
  for (int32_t d = -half; d <= half; d++) {
    tc_put_px(cx + d, cy, color);
    tc_put_px(cx, cy + d, color);
  }
}

/* ===========================================================================
 * GLCDC + touch bring-up
 * ===========================================================================
 */

/** @brief Bring up clocks/MSTP/time + the SCI8 console; halt on failure. */
static void tc_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  if ((ra8_cgc_init() != k_ra8_ok) || (ra8_mstp_init() != k_ra8_ok)) {
    tc_panic_halt(k_tc_msg_fail_in, (uint32_t)sizeof(k_tc_msg_fail_in) - 1U);
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    tc_panic_halt(k_tc_msg_fail_in, (uint32_t)sizeof(k_tc_msg_fail_in) - 1U);
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    tc_panic_halt(k_tc_msg_fail_in, (uint32_t)sizeof(k_tc_msg_fail_in) - 1U);
  }
  if (ra8_board_uart_console_init((uint32_t)k_tc_uart_baud) != k_ra8_ok) {
    tc_panic_halt(k_tc_msg_fail_in, (uint32_t)sizeof(k_tc_msg_fail_in) - 1U);
  }
}

/**
 * @brief Bring the GLCDC layer-1 path up and confirm it is programmed.
 *
 * @param[out] out_disp Receives the live display handle on success.
 * @return true when the panel is up and bound to ::s_framebuffer.
 *
 * @pre ::tc_setup_or_halt has run (clocks, MSTP, console up).
 * @pre ``out_disp`` is non-NULL.
 * @post On true ``*out_disp`` is a live handle scanning ::s_framebuffer.
 * @note Single-shot bring-up helper; not thread-safe.
 * @since 0.1.0
 */
static bool tc_glcdc_bringup(display_handle_t** out_disp)
{
  tc_fb_fill((uint16_t)k_tc_color_bg);
  display_handle_t* d = nullptr;
  if (display_init(&k_tc_display_cfg, &d) != k_ra8_ok) {
    return false;
  }
  if (d == nullptr) {
    return false;
  }
  display_fb_t fb = {};
  if (display_get_framebuffer(d, &fb) != k_ra8_ok) {
    return false;
  }
  if (fb.pixels != (void*)s_framebuffer) {
    return false;
  }
  *out_disp = d;
  return true;
}

/**
 * @brief Bring up IIC_B (I2C-compat) and open the GT911 touch driver.
 *
 * @return true when ``ra8_touch_open`` reported the GT911 alive.
 *
 * @pre ::tc_setup_or_halt has run.
 * @pre IRQs are enabled (``ra8_isr_globals_enable`` called).
 * @post On true the touch driver is open on IIC_B channel 0.
 * @note Mirrors ``touch_demo``: a future board revision that moves the GT911
 *       onto a RIIC channel only swaps the bind call.
 * @since 0.1.0
 */
static bool tc_touch_bringup(void)
{
  const ra8_i3c_cfg_t iic_cfg = {
    .mode     = k_ra8_i3c_mode_i2c,
    .bus_hz   = (uint32_t)k_tc_i2c_bus_hz,
    .pclka_hz = (uint32_t)k_tc_pclka_hz,
  };
  ra8_i2c_bus_ops_t bus_ops = {};
  if (ra8_i3c_init((uint8_t)k_tc_i2c_chan, &iic_cfg) != k_ra8_ok) {
    return false;
  }
  if (ra8_io_i2c_bus_bind_i3c_compat(&s_touch_bus, (uint8_t)k_tc_i2c_chan) != k_ra8_ok) {
    return false;
  }
  if (ra8_io_i2c_bus_as_ops(&s_touch_bus, &bus_ops) != k_ra8_ok) {
    return false;
  }
  const ra8_touch_cfg_t cfg = {.bus        = bus_ops,
                               .target_7b  = (uint8_t)k_tc_gt911_addr,
                               .irq_pin    = (uint8_t)k_ra8_touch_irq_pin_unset,
                               .max_points = (uint8_t)k_tc_max_points};
  return ra8_touch_open(&cfg) == k_ra8_ok;
}

/* ===========================================================================
 * Calibration shims (SOLID-D seams inverted onto by ra8_touch_cal_run)
 * ===========================================================================
 */

/**
 * @brief Draw shim: paint a target cross-hair and record it.
 *
 * @param[in] ctx    ::tc_capture_t (shared draw/read context).
 * @param[in] target Screen-space target centre in framebuffer pixels.
 * @return ``k_ra8_ok`` (rendering never fails for the SRAM framebuffer).
 *
 * @pre ``ctx`` is non-NULL and holds a live display handle.
 * @pre ``ctx->n_drawn`` < ::k_ra8_touch_cal_n_targets.
 * @post One target cross-hair is on the panel and ``target`` is recorded.
 * @note Reports the target over UART so a bench operator knows where to tap.
 * @since 0.1.0
 */
static ra8_err_t tc_draw_target(void* ctx, ra8_touch_cal_point_t target)
{
  tc_capture_t* cap = (tc_capture_t*)ctx;
  if ((cap == nullptr) || (cap->n_drawn >= (uint8_t)k_ra8_touch_cal_n_targets)) {
    return k_ra8_err_invalid_arg;
  }
  cap->targets[cap->n_drawn] = target;
  cap->n_drawn++;
  tc_draw_cross(target.x, target.y, (uint16_t)k_tc_color_target);
  if (cap->disp != nullptr) {
    (void)display_flush(cap->disp, display_full_rect(cap->disp), k_display_refresh_fast);
  }
  tc_print(k_tc_msg_tgt, (uint32_t)sizeof(k_tc_msg_tgt) - 1U);
  tc_print_uint((uint32_t)target.x);
  tc_print(k_tc_msg_comma, (uint32_t)sizeof(k_tc_msg_comma) - 1U);
  tc_print_uint((uint32_t)target.y);
  tc_print(k_tc_msg_eol, (uint32_t)sizeof(k_tc_msg_eol) - 1U);
  return k_ra8_ok;
}

/**
 * @brief Decide whether a touch read yielded a usable contact.
 *
 * @param[in] rc  Return code from ``ra8_touch_read``.
 * @param[in] got Reported contact count.
 * @return true iff the read succeeded AND at least one contact was decoded.
 *
 * @pre ``rc`` is a valid ``ra8_err_t``.
 * @post No state is mutated.
 * @note Pure; safe from any context.
 * @since 0.1.0
 */
static bool tc_sample_usable(ra8_err_t rc, uint8_t got)
{
  return (rc == k_ra8_ok) && (got >= 1U);
}

/**
 * @brief Read shim: block (bounded) on one settled raw GT911 sample.
 *
 * @param[in]  ctx     ::tc_capture_t (shared draw/read context).
 * @param[out] out_raw Receives the raw controller-space coordinate.
 * @return ``k_ra8_ok`` on a captured sample, ``k_ra8_err_hw_error`` on timeout.
 *
 * @pre ``ctx`` and ``out_raw`` are non-NULL.
 * @pre The touch driver is open.
 * @post On ok ``*out_raw`` holds the raw sample and it is recorded in ``ctx``.
 * @note Bounded to ::k_tc_poll_max iterations (NASA P10 Rule 2). A bare
 *       automated bench with no finger exhausts the budget and returns an
 *       error, which ``ra8_touch_cal_run`` surfaces as ``k_ra8_err_hw_error``.
 * @since 0.1.0
 */
static ra8_err_t tc_read_raw(void* ctx, ra8_touch_cal_point_t* out_raw)
{
  tc_capture_t* cap = (tc_capture_t*)ctx;
  if ((cap == nullptr) || (out_raw == nullptr) ||
      (cap->n_read >= (uint8_t)k_ra8_touch_cal_n_targets)) {
    return k_ra8_err_invalid_arg;
  }
  ra8_touch_point_t pts[k_tc_max_points] = {};
  for (uint32_t i = 0U; i < (uint32_t)k_tc_poll_max; i++) {
    uint8_t         got = 0U;
    const ra8_err_t rc  = ra8_touch_read(pts, (uint8_t)k_tc_max_points, &got);
    if (tc_sample_usable(rc, got)) {
      out_raw->x             = (int32_t)pts[0].x;
      out_raw->y             = (int32_t)pts[0].y;
      cap->raws[cap->n_read] = *out_raw;
      cap->n_read++;
      return k_ra8_ok;
    }
  }
  return k_ra8_err_hw_error;
}

/* ===========================================================================
 * Verify pass
 * ===========================================================================
 */

/** @brief Absolute difference of two signed values. */
static int32_t tc_axis_err(int32_t a, int32_t b)
{
  const int32_t d = a - b;
  return (d < 0) ? -d : d;
}

/**
 * @brief Verdict: did the whole calibration pipeline pass?
 *
 * @param[in] run_rc  Return code from ``ra8_touch_cal_run``.
 * @param[in] maxerr  Largest target-to-corrected residual, pixels.
 * @param[in] tol     Allowed residual, pixels.
 * @param[in] blob_ok Whether the save/load round-trip reproduced the matrix.
 * @return true iff the solve succeeded, the residual is within tolerance, and
 *         the serialised matrix round-tripped bit-exactly.
 *
 * @pre ``run_rc`` is a valid ``ra8_err_t``.
 * @post No state is mutated.
 * @note Pure; the compound decision is exercised for MC/DC by the host test.
 * @since 0.1.0
 */
static bool tc_cal_pass(ra8_err_t run_rc, int32_t maxerr, int32_t tol, bool blob_ok)
{
  return (run_rc == k_ra8_ok) && (maxerr <= tol) && blob_ok;
}

/**
 * @brief Round-trip the matrix through ``ra8_touch_cal_save`` /
 *        ``ra8_touch_cal_load`` and report stable reproduction.
 *
 * @details
 * Serialises @p m, loads it back, and re-serialises the loaded matrix; a
 * faithful round-trip yields a byte-identical second blob. Comparing the two
 * byte blobs (not the float structs) both proves stability and sidesteps the
 * ill-defined float object comparison.
 *
 * @param[in] m Matrix to serialise.
 * @return true iff the load-then-save reproduces the original blob byte-for-byte.
 *
 * @pre The six matrix coefficients are finite IEEE-754 floats.
 * @post No global state is mutated.
 * @since 0.1.0
 */
static bool tc_blob_roundtrip(const ra8_touch_cal_matrix_t* m)
{
  uint8_t                blob0[k_tc_blob_cap] = {};
  uint8_t                blob1[k_tc_blob_cap] = {};
  ra8_touch_cal_matrix_t back                 = {};
  if (ra8_touch_cal_save(m, blob0, sizeof(blob0)) != k_ra8_ok) {
    return false;
  }
  if (ra8_touch_cal_load(blob0, sizeof(blob0), &back) != k_ra8_ok) {
    return false;
  }
  if (ra8_touch_cal_save(&back, blob1, sizeof(blob1)) != k_ra8_ok) {
    return false;
  }
  return memcmp(blob0, blob1, sizeof(blob0)) == 0;
}

/**
 * @brief Apply the solved matrix to every captured raw, paint the corrected
 *        cross-hairs, and return the largest residual.
 *
 * @param[in]  m       Solved calibration matrix.
 * @param[in]  cap     Capture holding the target / raw pairs.
 * @param[out] out_max Receives the largest target-to-corrected residual.
 * @return ``k_ra8_ok`` on success, ``k_ra8_err_*`` if an apply failed.
 *
 * @pre ``m``, ``cap`` and ``out_max`` are non-NULL.
 * @pre ``cap->n_read`` == ``cap->n_drawn`` == ::k_ra8_touch_cal_n_targets.
 * @post ``*out_max`` holds the max per-axis residual over all samples.
 * @since 0.1.0
 */
static ra8_err_t
tc_apply_and_measure(const ra8_touch_cal_matrix_t* m, tc_capture_t* cap, int32_t* out_max)
{
  if ((m == nullptr) || (cap == nullptr) || (out_max == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  int32_t worst = 0;
  for (uint8_t i = 0U; i < cap->n_read; i++) {
    ra8_touch_cal_point_t screen = {};
    const ra8_err_t       rc =
      ra8_touch_cal_apply(cap->raws[i], m, (uint16_t)k_tc_fb_w, (uint16_t)k_tc_fb_h, &screen);
    if (rc != k_ra8_ok) {
      return rc;
    }
    const int32_t ex = tc_axis_err(screen.x, cap->targets[i].x);
    const int32_t ey = tc_axis_err(screen.y, cap->targets[i].y);
    worst            = (ex > worst) ? ex : worst;
    worst            = (ey > worst) ? ey : worst;
    tc_draw_cross(screen.x, screen.y, (uint16_t)k_tc_color_corrected);
  }
  if (cap->disp != nullptr) {
    (void)display_flush(cap->disp, display_full_rect(cap->disp), k_display_refresh_fast);
  }
  *out_max = worst;
  return k_ra8_ok;
}

/* ===========================================================================
 * Calibration orchestration + result banner
 * ===========================================================================
 */

/** @brief Reset the capture and run the five-point calibration collect+solve. */
static ra8_err_t tc_run_calibration(display_handle_t* disp, ra8_touch_cal_matrix_t* out_matrix)
{
  s_capture                         = (tc_capture_t){};
  s_capture.disp                    = disp;
  const ra8_touch_cal_run_cfg_t cfg = {
    .screen_width  = (uint16_t)k_tc_fb_w,
    .screen_height = (uint16_t)k_tc_fb_h,
    .inset_px      = (uint16_t)k_tc_inset_px,
    .draw_target   = tc_draw_target,
    .draw_ctx      = &s_capture,
    .read_raw      = tc_read_raw,
    .read_ctx      = &s_capture,
  };
  return ra8_touch_cal_run(&cfg, out_matrix);
}

/** @brief Print the solved-and-verified result banner. */
static void tc_report_ok(int32_t maxerr, bool verify_ok)
{
  tc_print(k_tc_msg_cal_ok, (uint32_t)sizeof(k_tc_msg_cal_ok) - 1U);
  if (verify_ok) {
    tc_print(k_tc_msg_ok, (uint32_t)sizeof(k_tc_msg_ok) - 1U);
  } else {
    tc_print(k_tc_msg_bad, (uint32_t)sizeof(k_tc_msg_bad) - 1U);
  }
  tc_print(k_tc_msg_maxerr, (uint32_t)sizeof(k_tc_msg_maxerr) - 1U);
  tc_print_uint((uint32_t)maxerr);
  tc_print(k_tc_msg_eol, (uint32_t)sizeof(k_tc_msg_eol) - 1U);
}

/** @brief Print the no-taps skip banner (bare automated bench). */
static void tc_report_skip(uint8_t got)
{
  tc_print(k_tc_msg_skip, (uint32_t)sizeof(k_tc_msg_skip) - 1U);
  tc_print_uint((uint32_t)got);
  tc_print(k_tc_msg_eol, (uint32_t)sizeof(k_tc_msg_eol) - 1U);
}

/**
 * @brief Solve, verify, and report; degrade to SKIP if no taps arrived.
 *
 * @param[in] disp Live display handle.
 *
 * @pre ``disp`` is non-NULL (GLCDC up).
 * @pre The touch driver is open.
 * @post Exactly one of the cal=OK / cal=SKIP banners is emitted.
 * @since 0.1.0
 */
static void tc_calibrate_and_report(display_handle_t* disp)
{
  ra8_touch_cal_matrix_t matrix = {};
  const ra8_err_t        run_rc = tc_run_calibration(disp, &matrix);
  if (run_rc != k_ra8_ok) {
    tc_report_skip(s_capture.n_read);
    return;
  }
  int32_t maxerr = 0;
  if (tc_apply_and_measure(&matrix, &s_capture, &maxerr) != k_ra8_ok) {
    tc_report_ok(maxerr, false);
    return;
  }
  const bool blob_ok   = tc_blob_roundtrip(&matrix);
  const bool verify_ok = tc_cal_pass(run_rc, maxerr, (int32_t)k_tc_verify_tol, blob_ok);
  tc_report_ok(maxerr, verify_ok);
}

/* ===========================================================================
 * Boot + main
 * ===========================================================================
 */

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
/**
 * @brief App entry: bring up panel + touch, run calibration, print the banner.
 *
 * @return Never returns.
 *
 * @pre Reset_Handler copied .data and zeroed .bss.
 * @pre SystemInit set VTOR / FPU / priority grouping.
 * @post The cal=OK / cal=SKIP result and the finger-free `touchcal: ready`
 *       sentinel are emitted; the CPU then loops in WFI.
 * @since 0.1.0
 */
int32_t main(void)
{
  tc_setup_or_halt();
  ra8_isr_globals_enable();
  tc_print(k_tc_msg_boot, (uint32_t)sizeof(k_tc_msg_boot) - 1U);

  display_handle_t* disp = nullptr;
  if (!tc_glcdc_bringup(&disp)) {
    tc_panic_halt(k_tc_msg_fail_gl, (uint32_t)sizeof(k_tc_msg_fail_gl) - 1U);
  }
  if (!tc_touch_bringup()) {
    tc_panic_halt(k_tc_msg_fail_op, (uint32_t)sizeof(k_tc_msg_fail_op) - 1U);
  }

  tc_calibrate_and_report(disp);

  /* Finger-free bring-up sentinel: prints in every environment (bench + SIL),
   * so hil.conf asserts this line and lets the negative set catch a solver
   * regression (verify=FAIL / cal=FAIL). */
  tc_print(k_tc_msg_ready, (uint32_t)sizeof(k_tc_msg_ready) - 1U);

  while (1) {
    __asm__ volatile("wfi");
  }
}
#pragma GCC diagnostic pop
