/**
 * @file examples/ek_ra8d2/drw_fill_demo/main.c
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
 * ``"drw: rev=0xNNNNNNNN fill match=Y\r\n"`` on the J-Link OB CDC channel.
 * LED1 toggles on a clean fill; LED2 toggles on a mismatch.
 *
 * Bare EK-RA8D2 only -- no shields or external transceivers. The DRW FB
 * cache is left off (``enable_caches = false``) so the CPU reads the
 * freshly-rasterized pixels directly; with the FB cache on, call
 * ``ra_drw_cache_flush`` before the verify, and mind the Cortex-M85
 * D-cache (the framebuffer lives in cacheable SRAM) -- see the bench plan.
 *
 * @note **Headless-emulator status.** ``tools/board_sim`` models the DRW
 * solid-fill rasterizer (``board_periph_drw.c``): the box fill lands in the
 * emulated framebuffer, so the centre pixel reads back green and the banner
 * reports ``match=Y`` (``g_drw_match = 1``); the ``board_sim_smoke.sh`` gate
 * keys on that line. The app still lives in ``hw_pending/`` until the fill is
 * confirmed on silicon -- the simulator proves the driver register sequence,
 * not the real D/AVE 2D engine. See ``README.md`` for the bench plan.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra_board_ek_ra8d2.h"
#include "ra_cgc.h"
#include "ra_check.h"
#include "ra_drw.h"
#include "ra_err.h"
#include "ra_gpio_constants.h"
#include "ra_isr.h"
#include "ra_mstp.h"
#include "ra_port_constants.h"
#include "ra_port_utils.h"
#include "ra_sci.h"
#include "ra_time.h"

/** @brief Diagnostic / log tag. */
static const char* s_tag = "drw_demo";

/** @brief Compile-time settings. */
typedef enum : uint32_t {
  k_drw_demo_baud        = 115200U,     /**< SCI8 baud rate.             */
  k_drw_demo_period_ms   = 1000U,       /**< Delay between fills.        */
  k_drw_demo_sci_channel = 8U,          /**< J-Link OB CDC is on SCI8.   */
  k_drw_demo_fill_argb   = 0xFF00FF00U, /**< Opaque green fill colour.   */
  k_drw_demo_idle_budget = 200000U,     /**< Bounded wait_idle poll budget. */
} drw_demo_config_t;

/** @brief Framebuffer + rectangle geometry (pixels). */
typedef enum : uint16_t {
  k_drw_demo_fb_dim  = 32U, /**< 32 x 32 framebuffer.            */
  k_drw_demo_rect_xy = 8U,  /**< Rect top-left (8, 8).           */
  k_drw_demo_rect_wh = 16U, /**< Rect 16 x 16 -> covers [8,24).  */
  k_drw_demo_center  = 16U, /**< Centre pixel (16, 16) is inside. */
} drw_demo_geom_t;

/** @brief Pinout for SCI8 on the J-Link OB CDC channel (PD02 / PD03). */
static const ra_port_pin_t k_drw_demo_pin_txd =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << 8) | (uint16_t)k_ra_pin_2);
static const ra_port_pin_t k_drw_demo_pin_rxd =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << 8) | (uint16_t)k_ra_pin_3);

/** @brief Output line tags. */
static const uint8_t k_drw_demo_ok_msg[]  = "drw: fill match=Y\r\n";
static const uint8_t k_drw_demo_bad_msg[] = "drw: fill match=N\r\n";

/**
 * @var g_drw_fb
 * @brief 32 x 32 ARGB8888 framebuffer the DRW rasterizes into.
 * @note 4-byte aligned (ARGB8888 invariant).
 * @since 0.1.0
 */
static uint32_t g_drw_fb[(uint32_t)k_drw_demo_fb_dim * (uint32_t)k_drw_demo_fb_dim]
  __attribute__((aligned(4)));

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

/**
 * @brief Route PD02 / PD03 to SCI8 TXD/RXD via PFS.
 *
 * @return ``ra_err_t`` error code from the underlying PFS routing.
 * @pre IOPORT module reachable.
 * @post On success PD02 + PD03 are in SCI-async mode.
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t drw_demo_pins_init(void)
{
  ra_err_t err = ra_pfs_route_peripheral(k_drw_demo_pin_txd, k_ra_psel_sci_async, "drw_demo.txd8");
  if (err != k_ra_ok) {
    return err;
  }
  return ra_pfs_route_peripheral(k_drw_demo_pin_rxd, k_ra_psel_sci_async, "drw_demo.rxd8");
}

/** @brief Bring CGC + SysTick + SCI8 + LEDs + MSTP up. */
static void drw_demo_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  uint32_t pclka_hz   = 0U;

  if (ra_cgc_init() != k_ra_ok) {
    drw_demo_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_cpuclk0, &cpuclk0_hz) != k_ra_ok) {
    drw_demo_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_pclka, &pclka_hz) != k_ra_ok) {
    drw_demo_panic_halt();
  }
  if (ra_mstp_init() != k_ra_ok) {
    drw_demo_panic_halt();
  }
  if (ra_time_init(cpuclk0_hz) != k_ra_ok) {
    drw_demo_panic_halt();
  }
  if (drw_demo_pins_init() != k_ra_ok) {
    drw_demo_panic_halt();
  }
  const ra_sci_cfg_t sci_cfg = {
    .baud      = k_drw_demo_baud,
    .data_bits = k_ra_sci_data_8,
    .parity    = k_ra_sci_parity_none,
    .stop_bits = k_ra_sci_stop_1,
    .pclk_hz   = pclka_hz,
  };
  if (ra_sci_init((uint8_t)k_drw_demo_sci_channel, &sci_cfg) != k_ra_ok) {
    drw_demo_panic_halt();
  }
  if (ra_board_led_init(k_ra_board_led1) != k_ra_ok) {
    drw_demo_panic_halt();
  }
  if (ra_board_led_init(k_ra_board_led2) != k_ra_ok) {
    drw_demo_panic_halt();
  }
}

/**
 * @brief Bring up the DRW engine pointed at ``g_drw_fb``.
 *
 * @par MC/DC:
 * Single decision ``err != k_ra_ok`` -- no compound condition.
 *
 * @return ``ra_err_t`` from ``ra_drw_init``.
 * @pre CGC + MSTP up; IRQs masked or single-threaded init.
 * @post DRW ORIGIN/PITCH/CONTROL2 target ``g_drw_fb`` (ARGB8888).
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t drw_demo_configure(void)
{
  const ra_drw_config_t cfg = {
    .framebuffer_addr       = (uintptr_t)g_drw_fb,
    .pitch_px               = (uint16_t)k_drw_demo_fb_dim,
    .format                 = k_ra_drw_writefmt_argb8888,
    .enable_caches          = false,
    .enable_buffered_writes = false,
  };
  uint32_t rev = 0U;
  if (ra_drw_get_hwrevision(&rev) == k_ra_ok) {
    g_drw_rev = rev;
  }
  return ra_drw_init(&cfg);
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
 * @return ``ra_err_t`` -- ``k_ra_ok`` once the (bounded) attempt finishes,
 *         or the fill / wait error.
 * @retval k_ra_err_null_ptr ``out_ok`` was NULL.
 * @pre ::drw_demo_configure succeeded.
 * @post ``g_drw_fill_err`` reflects the last DRW call; ``*out_ok`` 0/1.
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t drw_demo_fill_and_check(uint8_t* out_ok)
{
  RA_CHECK_NULL_PTR(out_ok, s_tag, "out_ok must not be nullptr");

  for (uint32_t i = 0U; i < (uint32_t)k_drw_demo_fb_dim * (uint32_t)k_drw_demo_fb_dim; ++i) {
    g_drw_fb[i] = 0U;
  }

  const ra_drw_rect_t rect = {
    .x              = (int16_t)k_drw_demo_rect_xy,
    .y              = (int16_t)k_drw_demo_rect_xy,
    .width_px       = (uint16_t)k_drw_demo_rect_wh,
    .height_px      = (uint16_t)k_drw_demo_rect_wh,
    .color_argb8888 = (uint32_t)k_drw_demo_fill_argb,
  };
  const ra_err_t ferr = ra_drw_fill_rect(&rect);
  if (ferr != k_ra_ok) {
    g_drw_fill_err = (uint32_t)ferr;
    return ferr;
  }
  const ra_err_t werr = ra_drw_wait_idle((uint32_t)k_drw_demo_idle_budget);
  g_drw_fill_err      = (werr == k_ra_ok) ? 0U : (uint32_t)werr;

  const uint32_t centre = g_drw_fb[drw_demo_px(k_drw_demo_center, k_drw_demo_center)];
  const uint32_t tl     = g_drw_fb[drw_demo_px(0U, 0U)];
  const uint32_t br =
    g_drw_fb[drw_demo_px((uint16_t)(k_drw_demo_fb_dim - 1U), (uint16_t)(k_drw_demo_fb_dim - 1U))];
  *out_ok = (centre == (uint32_t)k_drw_demo_fill_argb && tl == 0U && br == 0U) ? 1U : 0U;
  return k_ra_ok;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
int32_t main(void)
{
  drw_demo_setup_or_halt();
  /* Clear PRIMASK so SysTick can dispatch and ra_delay_ms() uses the
   * SysTick path (board_sim does not advance DWT_CYCCNT). No NVIC sources
   * are armed by this demo. */
  ra_isr_globals_enable();

  if (drw_demo_configure() != k_ra_ok) {
    drw_demo_panic_halt();
  }

  while (1) {
    uint8_t        ok   = 0U;
    const ra_err_t err  = drw_demo_fill_and_check(&ok);
    const uint8_t  good = (err == k_ra_ok && ok != 0U) ? 1U : 0U;
    g_drw_match         = (uint32_t)good;
    if (good != 0U) {
      (void)ra_sci_write_polling((uint8_t)k_drw_demo_sci_channel,
                                 k_drw_demo_ok_msg,
                                 (uint32_t)(sizeof(k_drw_demo_ok_msg) - 1U));
      (void)ra_board_led_toggle(k_ra_board_led1);
    } else {
      (void)ra_sci_write_polling((uint8_t)k_drw_demo_sci_channel,
                                 k_drw_demo_bad_msg,
                                 (uint32_t)(sizeof(k_drw_demo_bad_msg) - 1U));
      (void)ra_board_led_toggle(k_ra_board_led2);
    }
    ++g_drw_heartbeat;
    ra_delay_ms(k_drw_demo_period_ms);
  }
  drw_demo_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
