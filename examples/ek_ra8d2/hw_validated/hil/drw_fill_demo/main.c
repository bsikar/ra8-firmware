/**
 * @file examples/ek_ra8d2/hw_validated/hil/drw_fill_demo/main.c
 * @brief DRW 2D-engine framebuffer fill-rect + verify demo (EK-RA8D2)
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * The DRW ("D/AVE 2D") engine is the RA8D2's hardware 2D rasterizer. This
 * demo points it at a small 32x32 ARGB8888 framebuffer in SRAM, asks it to
 * fill a 16x16 green rectangle in the centre, waits for the engine to go
 * idle, and checks the framebuffer byte-exact: the centre pixel must be
 * green and the corners must still be clear.
 *
 * Bring-up: CGC + SysTick + SCI8 + LEDs + MSTP. Once a second the loop
 * clears the framebuffer, issues the fill, and reports
 * ``"drw: fill match=Y\r\n"`` (or ``match=N``) on the J-Link OB CDC channel.
 * LED1 toggles on a clean fill; LED2 toggles on a mismatch. On silicon today
 * the DRW is inert (issue #247), so the fill lands nothing and the banner
 * reads ``match=N``; see the note below.
 *
 * Bare EK-RA8D2 only -- no shields or external transceivers. The DRW FB
 * cache is left off (``enable_caches = false``) so the CPU reads the
 * freshly-rasterized pixels directly; with the FB cache on, call
 * ``ra8_drw_cache_flush`` before the verify, and mind the Cortex-M85
 * D-cache (the framebuffer lives in cacheable SRAM) -- see the bench plan.
 *
 * @note **Headless-emulator status.** ``tools/board_sim`` models the DRW engine
 * as INERT (``board_periph_drw.c``), faithful to real silicon where the D/AVE 2D
 * engine never rasterizes (issue #247): the register sequence completes (writes
 * accepted, ``ra8_drw_wait_idle`` returns), but the box fill lands NO pixel, so
 * the centre pixel stays clear and the banner honestly reports ``match=N``
 * (``g_drw_match = 0``) -- the same result the bench gives. The
 * ``board_sim_smoke.sh`` gate keys on that ``match=N`` line. This demo stays in
 * ``hw_pending/`` and CANNOT report ``match=Y`` in SIL or on hardware until #247
 * brings the engine to life; the simulator proves only the driver register
 * sequence, not a working D/AVE 2D engine. See ``README.md`` for the bench plan.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_cgc.h"
#include "ra8_check.h"
#include "ra8_drw.h"
#include "ra8_err.h"
#include "ra8_isr.h"
#include "ra8_mstp.h"
#include "ra8_time.h"

/** @brief Diagnostic / log tag. */
static const char* s_tag = "drw_demo";

/** @brief Compile-time settings. */
typedef enum : uint32_t {
  k_drw_demo_baud        = 115200U,     /**< SCI8 baud rate.                */
  k_drw_demo_period_ms   = 1000U,       /**< Delay between fills.           */
  k_drw_demo_fill_argb   = 0xFF00FF00U, /**< Opaque green fill colour.      */
  k_drw_demo_idle_budget = 200000U,     /**< Bounded wait_idle poll budget. */
} drw_demo_config_t;

/** @brief Framebuffer + rectangle geometry (pixels). */
typedef enum : uint16_t {
  k_drw_demo_fb_dim  = 32U, /**< 32 x 32 framebuffer.             */
  k_drw_demo_rect_xy = 8U,  /**< Rect top-left (8, 8).            */
  k_drw_demo_rect_wh = 16U, /**< Rect 16 x 16 -> covers [8,24).   */
  k_drw_demo_center  = 16U, /**< Centre pixel (16, 16) is inside. */
} drw_demo_geom_t;

/** @brief Output line tags. */
static const uint8_t k_drw_demo_ok_msg[]  = "drw: fill match=Y\r\n";
static const uint8_t k_drw_demo_bad_msg[] = "drw: fill match=N\r\n";

/**
 * @var g_drw_fb
 * @brief 32 x 32 ARGB8888 framebuffer the DRW rasterizes into.
 * @note 4-byte aligned (ARGB8888 invariant).
 * @since 0.1.0
 */
[[gnu::aligned(
  4)]] static uint32_t g_drw_fb[(uint32_t)k_drw_demo_fb_dim * (uint32_t)k_drw_demo_fb_dim];

/**
 * @var g_drw_match
 * @brief 1 when the filled rectangle read back as expected.
 * @note Read externally only (HIL / board emulator).
 * @since 0.1.0
 */
volatile uint32_t g_drw_match = 0U;

/**
 * @var g_drw_rev
 * @brief DRW HWREVISION register snapshot (confirms the engine is present).
 * @note Read externally only.
 * @since 0.1.0
 */
volatile uint32_t g_drw_rev = 0U;

/**
 * @var g_drw_fill_err
 * @brief Non-zero if a DRW call returned an error.
 * @note Read externally only.
 * @since 0.1.0
 */
volatile uint32_t g_drw_fill_err = 0U;

/**
 * @var g_drw_heartbeat
 * @brief Bumps once per main-loop pass -- liveness for headless probes.
 * @note Read externally only.
 * @since 0.1.0
 */
volatile uint32_t g_drw_heartbeat = 0U;

/** @brief Park forever after a fatal init failure. */
static void drw_demo_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/** @brief Index of pixel (x, y) within the framebuffer. */
static uint32_t drw_demo_px(uint16_t x, uint16_t y)
{
  return ((uint32_t)y * (uint32_t)k_drw_demo_fb_dim) + (uint32_t)x;
}

/** @brief Bring CGC + SysTick + SCI8 + LEDs + MSTP up. */
static void drw_demo_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;

  if (ra8_cgc_init() != k_ra8_ok) {
    drw_demo_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    drw_demo_panic_halt();
  }
  if (ra8_mstp_init() != k_ra8_ok) {
    drw_demo_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    drw_demo_panic_halt();
  }
  if (ra8_board_uart_console_init((uint32_t)k_drw_demo_baud) != k_ra8_ok) {
    drw_demo_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led1) != k_ra8_ok) {
    drw_demo_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led2) != k_ra8_ok) {
    drw_demo_panic_halt();
  }
}

/**
 * @brief Bring up the DRW engine pointed at ``g_drw_fb``.
 *
 * @par MC/DC:
 * Single decision ``err != k_ra8_ok`` -- no compound condition.
 *
 * @return ``ra8_err_t`` from ``ra8_drw_init``.
 * @pre CGC + MSTP up; IRQs masked or single-threaded init.
 * @post DRW ORIGIN/PITCH/CONTROL2 target ``g_drw_fb`` (ARGB8888).
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t drw_demo_configure(void)
{
  const ra8_drw_config_t cfg = {
    .framebuffer_addr       = (uintptr_t)g_drw_fb,
    .pitch_px               = (uint16_t)k_drw_demo_fb_dim,
    .format                 = k_ra8_drw_writefmt_argb8888,
    .enable_caches          = false,
    .enable_buffered_writes = false,
  };
  uint32_t rev = 0U;
  if (ra8_drw_get_hwrevision(&rev) == k_ra8_ok) {
    g_drw_rev = rev;
  }
  return ra8_drw_init(&cfg);
}

/**
 * @brief Clear the framebuffer, fill the centre rectangle, and verify.
 *
 * @param[out] out_ok 1 when the centre pixel is green and both checked
 *                    corners are clear.
 *
 * @par MC/DC:
 * Decision ``ok = centre==green && tl==0 && br==0`` (3 conditions). The
 * host test supplies N+1 = 4 vectors, varying each condition.
 *
 * @return ``ra8_err_t`` -- ``k_ra8_ok`` once the (bounded) attempt finishes,
 *         or the fill / wait error.
 * @retval k_ra8_err_null_ptr ``out_ok`` was NULL.
 * @pre ::drw_demo_configure succeeded.
 * @post ``g_drw_fill_err`` reflects the last DRW call; ``*out_ok`` 0/1.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t drw_demo_fill_and_check(uint8_t* out_ok)
{
  RA8_CHECK_NULL_PTR(out_ok, s_tag, "out_ok must not be nullptr");

  for (uint32_t i = 0U; i < (uint32_t)k_drw_demo_fb_dim * (uint32_t)k_drw_demo_fb_dim; ++i) {
    g_drw_fb[i] = 0U;
  }

  const ra8_drw_rect_t rect = {
    .x              = (int16_t)k_drw_demo_rect_xy,
    .y              = (int16_t)k_drw_demo_rect_xy,
    .width_px       = (uint16_t)k_drw_demo_rect_wh,
    .height_px      = (uint16_t)k_drw_demo_rect_wh,
    .color_argb8888 = (uint32_t)k_drw_demo_fill_argb,
  };
  const ra8_err_t ferr = ra8_drw_fill_rect(&rect);
  if (ferr != k_ra8_ok) {
    g_drw_fill_err = (uint32_t)ferr;
    return ferr;
  }
  const ra8_err_t werr = ra8_drw_wait_idle((uint32_t)k_drw_demo_idle_budget);
  g_drw_fill_err       = (werr == k_ra8_ok) ? 0U : (uint32_t)werr;

  const uint32_t centre = g_drw_fb[drw_demo_px(k_drw_demo_center, k_drw_demo_center)];
  const uint32_t tl     = g_drw_fb[drw_demo_px(0U, 0U)];
  const uint32_t br =
    g_drw_fb[drw_demo_px((uint16_t)(k_drw_demo_fb_dim - 1U), (uint16_t)(k_drw_demo_fb_dim - 1U))];
  *out_ok = (centre == (uint32_t)k_drw_demo_fill_argb && tl == 0U && br == 0U) ? 1U : 0U;
  return k_ra8_ok;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
int32_t main(void)
{
  drw_demo_setup_or_halt();
  /* Clear PRIMASK so SysTick can dispatch and ra8_delay_ms() uses the
   * SysTick path (board_sim does not advance DWT_CYCCNT). No NVIC sources
   * are armed by this demo. */
  ra8_isr_globals_enable();

  if (drw_demo_configure() != k_ra8_ok) {
    drw_demo_panic_halt();
  }

  while (1) {
    uint8_t         ok   = 0U;
    const ra8_err_t err  = drw_demo_fill_and_check(&ok);
    const uint8_t   good = (err == k_ra8_ok && ok != 0U) ? 1U : 0U;
    g_drw_match          = (uint32_t)good;
    if (good != 0U) {
      (void)ra8_board_uart_console_write(k_drw_demo_ok_msg,
                                         (size_t)(sizeof(k_drw_demo_ok_msg) - 1U));
      (void)ra8_board_led_toggle(k_ra8_board_led1);
    } else {
      (void)ra8_board_uart_console_write(k_drw_demo_bad_msg,
                                         (size_t)(sizeof(k_drw_demo_bad_msg) - 1U));
      (void)ra8_board_led_toggle(k_ra8_board_led2);
    }
    ++g_drw_heartbeat;
    ra8_delay_ms(k_drw_demo_period_ms);
  }
  drw_demo_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
