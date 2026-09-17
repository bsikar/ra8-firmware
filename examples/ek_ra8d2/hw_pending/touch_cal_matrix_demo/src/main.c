/**
 * @file examples/ek_ra8d2/hw_pending/touch_cal_matrix_demo/src/main.c
 * @brief First consumer of ra8_touch_cal: run, solve, apply, serialise.
 *
 * @details
 * `ra8_touch_cal` is pure math behind two caller-supplied shims, so the whole
 * library can be exercised without a panel or a touch controller. This app
 * stands up a deterministic fake panel and walks the four public entry points
 * end to end:
 *
 *   1. ::ra8_touch_cal_run drives the five built-in targets. The draw shim
 *      records the cross-hair it was asked to paint; the read shim answers
 *      with the raw sample a controller obeying a KNOWN affine map would
 *      report for exactly that cross-hair. Both shims count their calls, so
 *      the run leg also proves the sequence painted before it sampled, five
 *      times.
 *   2. The solved matrix is compared against the ground truth the fake
 *      controller was built from (gain 1/5 on both axes, bias -20 px and
 *      -16 px).
 *   3. ::ra8_touch_cal_apply maps one fresh raw sample through that matrix
 *      and the result is checked against the hand-computed pixel.
 *   4. ::ra8_touch_cal_save serialises the matrix, the 'TCAL' magic and
 *      version byte are checked in place, and ::ra8_touch_cal_load reads it
 *      back for a coefficient-by-coefficient compare.
 *
 * No hardware beyond the console is touched: there is no panel driver, no
 * GT911, and nothing is persisted. The 36-byte blob is exactly the width of
 * the touch-calibration window in a `ra8_devcfg` record, which is where a
 * real product would park it.
 *
 * Observable over the SCI8 / J-Link OB VCOM console. A good run prints
 * `touch_cal_matrix_demo: run+solve PASS`, `apply PASS`, `blob PASS` and a
 * final `ALL PASS` line.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_boot_entry.h"
#include "ra8_err.h"
#include "ra8_io_log.h"
#include "ra8_io_stream.h"
#include "ra8_io_stream_uart.h"
#include "ra8_log.h"
#include "ra8_sci.h"
#include "ra8_touch_cal.h"

/**
 * @enum tc_const_t
 * @brief Console, panel, and fake-controller knobs (no magic numbers).
 *
 * @details The fake controller maps a screen pixel to a raw sample with
 *          `raw = (pixel + offset) * gain`, so the matrix the solver must
 *          recover is `pixel = raw / gain - offset`.
 *
 * @since 0.1.0
 */
typedef enum : int32_t {
  k_tc_uart_chan     = 8,    /**< SCI8 J-Link OB console.                  */
  k_tc_screen_w      = 800,  /**< Fake panel width, pixels.                */
  k_tc_screen_h      = 480,  /**< Fake panel height, pixels.               */
  k_tc_inset_px      = 40,   /**< Corner-target inset from the edge.       */
  k_tc_raw_gain      = 5,    /**< Controller counts per pixel.             */
  k_tc_raw_off_x     = 20,   /**< Controller X origin offset, pixels.      */
  k_tc_raw_off_y     = 16,   /**< Controller Y origin offset, pixels.      */
  k_tc_targets       = 5,    /**< Cross-hairs ra8_touch_cal_run paints.     */
  k_tc_probe_raw_x   = 2100, /**< Fresh raw sample for the apply leg.      */
  k_tc_probe_raw_y   = 1216, /**< Fresh raw sample for the apply leg.      */
  k_tc_probe_pixel_x = 400,  /**< 2100 / 5 - 20, by hand.                  */
  k_tc_probe_pixel_y = 227,  /**< 1216 / 5 - 16, truncated by hand.        */
  k_tc_probe_tol_px  = 2,    /**< Slack on the mapped pixel.               */
} tc_const_t;

static const float k_tc_true_gain   = 0.2F;   /**< 1 / k_tc_raw_gain.      */
static const float k_tc_true_skew   = 0.0F;   /**< Axes are independent.   */
static const float k_tc_true_bias_x = -20.0F; /**< -k_tc_raw_off_x.        */
static const float k_tc_true_bias_y = -16.0F; /**< -k_tc_raw_off_y.        */
static const float k_tc_coeff_tol   = 1.0E-3F; /**< Slack per coefficient. */

/**
 * @struct tc_panel_t
 * @brief State of the fake panel shared by the draw and read shims.
 *
 * @details `last_target` is the hand-off between the two shims: the read shim
 *          answers for whatever cross-hair the draw shim was last asked to
 *          paint, which is what makes the fake controller consistent with the
 *          ground-truth matrix.
 *
 * @since 0.1.0
 */
typedef struct {
  ra8_touch_cal_point_t last_target; /**< Cross-hair most recently painted. */
  int32_t               draws;       /**< Times the draw shim was called.   */
  int32_t               reads;       /**< Times the read shim was called.   */
  bool                  ordered;     /**< Every read followed a fresh draw. */
} tc_panel_t;

static tc_panel_t s_panel = {.ordered = true}; /**< The fake panel.        */

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
 * @brief Absolute value of a float without pulling in libm.
 *
 * @param[in] v Value to fold.
 * @return float Magnitude of @p v.
 * @since 0.1.0
 */
static float internal_absf(float v)
{
  return (v < 0.0F) ? -v : v;
}

/**
 * @brief Compare a solved coefficient against its ground truth.
 *
 * @param[in] got  Coefficient the solver produced.
 * @param[in] want Coefficient the fake controller was built from.
 * @return bool True when the two agree inside ::k_tc_coeff_tol.
 * @since 0.1.0
 */
static bool internal_near(float got, float want)
{
  return internal_absf(got - want) <= k_tc_coeff_tol;
}

/**
 * @brief Draw shim: record the cross-hair the library asked for.
 *
 * @param[in] ctx    Fake panel, as ::tc_panel_t.
 * @param[in] target Cross-hair centre, in screen pixels.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok           Target recorded.
 * @retval k_ra8_err_null_ptr @p ctx was NULL.
 * @post The panel remembers @p target for the next read.
 * @since 0.1.0
 */
static ra8_err_t internal_draw_target(void* ctx, ra8_touch_cal_point_t target)
{
  tc_panel_t* panel = (tc_panel_t*)ctx;

  if (panel == nullptr) {
    return k_ra8_err_null_ptr;
  }

  panel->last_target = target;
  panel->draws += 1;
  return k_ra8_ok;
}

/**
 * @brief Read shim: answer with the raw sample a known controller would give.
 *
 * @param[in]  ctx     Fake panel, as ::tc_panel_t.
 * @param[out] out_raw Destination for the synthetic controller sample.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok           @p out_raw holds the sample.
 * @retval k_ra8_err_null_ptr @p ctx or @p out_raw was NULL.
 * @post A read that did not follow a fresh draw clears `panel->ordered`.
 * @since 0.1.0
 */
static ra8_err_t internal_read_raw(void* ctx, ra8_touch_cal_point_t* out_raw)
{
  tc_panel_t* panel = (tc_panel_t*)ctx;

  if ((panel == nullptr) || (out_raw == nullptr)) {
    return k_ra8_err_null_ptr;
  }

  if (panel->draws != (panel->reads + 1)) {
    panel->ordered = false;
  }

  out_raw->x = (panel->last_target.x + k_tc_raw_off_x) * k_tc_raw_gain;
  out_raw->y = (panel->last_target.y + k_tc_raw_off_y) * k_tc_raw_gain;
  panel->reads += 1;
  return k_ra8_ok;
}

/**
 * @brief Drive the five-target run and check the solved matrix.
 *
 * @param[out] out_mtx Destination for the solved transform.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              Matrix matches the ground truth.
 * @retval k_ra8_err_null_ptr    @p out_mtx was NULL.
 * @retval k_ra8_err_invalid_arg The run misbehaved or the fit is off.
 * @post On success @p out_mtx holds the recovered transform.
 * @since 0.1.0
 */
static ra8_err_t internal_run_and_solve(ra8_touch_cal_matrix_t* out_mtx)
{
  if (out_mtx == nullptr) {
    return k_ra8_err_null_ptr;
  }

  const ra8_touch_cal_run_cfg_t cfg = {
      .screen_width  = (uint16_t)k_tc_screen_w,
      .screen_height = (uint16_t)k_tc_screen_h,
      .inset_px      = (uint16_t)k_tc_inset_px,
      .draw_target   = internal_draw_target,
      .draw_ctx      = &s_panel,
      .read_raw      = internal_read_raw,
      .read_ctx      = &s_panel,
  };

  const ra8_err_t err = ra8_touch_cal_run(&cfg, out_mtx);
  if (err != k_ra8_ok) {
    return err;
  }

  const bool swept = (s_panel.draws == k_tc_targets) && (s_panel.reads == k_tc_targets)
                     && s_panel.ordered;
  const bool fitted = internal_near(out_mtx->a, k_tc_true_gain)
                      && internal_near(out_mtx->b, k_tc_true_skew)
                      && internal_near(out_mtx->c, k_tc_true_bias_x)
                      && internal_near(out_mtx->d, k_tc_true_skew)
                      && internal_near(out_mtx->e, k_tc_true_gain)
                      && internal_near(out_mtx->f, k_tc_true_bias_y);

  return (swept && fitted) ? k_ra8_ok : k_ra8_err_invalid_arg;
}

/**
 * @brief Map one fresh raw sample through the solved matrix.
 *
 * @param[in] mtx Transform from ::internal_run_and_solve.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              Mapped pixel landed inside the tolerance.
 * @retval k_ra8_err_null_ptr    @p mtx was NULL.
 * @retval k_ra8_err_invalid_arg The mapped pixel missed.
 * @since 0.1.0
 */
static ra8_err_t internal_apply_probe(const ra8_touch_cal_matrix_t* mtx)
{
  if (mtx == nullptr) {
    return k_ra8_err_null_ptr;
  }

  const ra8_touch_cal_point_t raw = {.x = k_tc_probe_raw_x, .y = k_tc_probe_raw_y};
  ra8_touch_cal_point_t       pixel = {.x = 0, .y = 0};

  const ra8_err_t err = ra8_touch_cal_apply(raw,
                                            mtx,
                                            (uint16_t)k_tc_screen_w,
                                            (uint16_t)k_tc_screen_h,
                                            &pixel);
  if (err != k_ra8_ok) {
    return err;
  }

  int32_t dx = pixel.x - k_tc_probe_pixel_x;
  int32_t dy = pixel.y - k_tc_probe_pixel_y;
  dx         = (dx < 0) ? -dx : dx;
  dy         = (dy < 0) ? -dy : dy;

  return ((dx <= k_tc_probe_tol_px) && (dy <= k_tc_probe_tol_px)) ? k_ra8_ok
                                                                  : k_ra8_err_invalid_arg;
}

/**
 * @brief Serialise, inspect, and reload the calibration blob.
 *
 * @param[in] mtx Transform from ::internal_run_and_solve.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              Header is right and the round trip agrees.
 * @retval k_ra8_err_null_ptr    @p mtx was NULL.
 * @retval k_ra8_err_invalid_arg Header or a coefficient did not survive.
 * @note Nothing is persisted; the blob lives on the stack of this call.
 * @since 0.1.0
 */
static ra8_err_t internal_blob_round_trip(const ra8_touch_cal_matrix_t* mtx)
{
  if (mtx == nullptr) {
    return k_ra8_err_null_ptr;
  }

  uint8_t   blob[k_ra8_touch_cal_blob_size] = {0};
  ra8_err_t err = ra8_touch_cal_save(mtx, blob, sizeof(blob));
  if (err != k_ra8_ok) {
    return err;
  }

  const bool header_ok = (blob[k_ra8_touch_cal_off_magic] == (uint8_t)k_ra8_touch_cal_magic_b0)
                         && (blob[k_ra8_touch_cal_off_magic + 1U]
                             == (uint8_t)k_ra8_touch_cal_magic_b1)
                         && (blob[k_ra8_touch_cal_off_magic + 2U]
                             == (uint8_t)k_ra8_touch_cal_magic_b2)
                         && (blob[k_ra8_touch_cal_off_magic + 3U]
                             == (uint8_t)k_ra8_touch_cal_magic_b3)
                         && (blob[k_ra8_touch_cal_off_version]
                             == (uint8_t)k_ra8_touch_cal_storage_version);
  if (!header_ok) {
    return k_ra8_err_invalid_arg;
  }

  ra8_touch_cal_matrix_t back = {0};
  err                         = ra8_touch_cal_load(blob, sizeof(blob), &back);
  if (err != k_ra8_ok) {
    return err;
  }

  const bool same = internal_near(back.a, mtx->a) && internal_near(back.b, mtx->b)
                    && internal_near(back.c, mtx->c) && internal_near(back.d, mtx->d)
                    && internal_near(back.e, mtx->e) && internal_near(back.f, mtx->f);

  return same ? k_ra8_ok : k_ra8_err_invalid_arg;
}

/**
 * @brief Entry point: run the three legs and print one verdict each.
 *
 * @return void
 * @pre SystemInit configured VTOR / FPU / priority grouping.
 * @post A verdict per leg and a final summary are queued on SCI8.
 * @post Control parks in an infinite loop; the function never returns.
 * @note Single-threaded; runs to the park loop on the main stack.
 * @since 0.1.0
 */
void main(void)
{
  ra8_log_init();
  (void)ra8_io_stream_uart_init(&s_uart, &s_uart_state, (uint8_t)k_tc_uart_chan);
  (void)ra8_io_log_attach(&s_uart);
  internal_print("touch_cal_matrix_demo: boot\r\n");

  ra8_touch_cal_matrix_t mtx  = {0};
  bool                   pass = true;

  if (internal_run_and_solve(&mtx) == k_ra8_ok) {
    internal_print("touch_cal_matrix_demo: run+solve PASS\r\n");
  } else {
    internal_print("touch_cal_matrix_demo: run+solve FAIL\r\n");
    pass = false;
  }

  if (internal_apply_probe(&mtx) == k_ra8_ok) {
    internal_print("touch_cal_matrix_demo: apply PASS\r\n");
  } else {
    internal_print("touch_cal_matrix_demo: apply FAIL\r\n");
    pass = false;
  }

  if (internal_blob_round_trip(&mtx) == k_ra8_ok) {
    internal_print("touch_cal_matrix_demo: blob PASS\r\n");
  } else {
    internal_print("touch_cal_matrix_demo: blob FAIL\r\n");
    pass = false;
  }

  internal_print(pass ? "touch_cal_matrix_demo: ALL PASS\r\n"
                      : "touch_cal_matrix_demo: ALL FAIL\r\n");

  (void)ra8_sci_flush((uint8_t)k_tc_uart_chan);
  while (true) {
  }
}
