/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file examples/ek_ra8d2/hw_pending/epaper_refresh/main.c
 * @brief IT8951 e-paper full + partial refresh demo through the display PAL
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * End-to-end example for the e-ink display path (#256): it drives an
 * IT8951-compatible e-paper panel entirely through ``libs/ra8_display_pal``'s
 * e-ink backend (``k_display_backend_eink_it8951``), which sits on the
 * ``ra8_epaper`` HAL driver over an injected ``ra8_io_spi_bus`` seam. The app
 * itself never names ``ra8_epaper_*`` -- it paints a canonical RGB565
 * framebuffer and calls the same six PAL entry points the LCD demo uses.
 *
 * Flow:
 *  1. Bring up clocks, MSTP, SysTick, the SCI console and two LEDs.
 *  2. Configure the panel's /RESET (output) and HRDY (input) GPIOs, bring up
 *     SPI_B channel 0 (mode 0, 10 MHz -- under the IT8951's 24 MHz ceiling),
 *     and bind it into the ``ra8_epaper`` bus seam via ``ra8_io_spi_bus``.
 *  3. ``display_init`` with the e-ink backend + the BSP-style IT8951 descriptor
 *     carried in ``display_cfg_t.panel_timing``.
 *  4. Paint a checkerboard test pattern and FULL-refresh it (GC16 quality),
 *     timing the flush over SysTick and reporting it on the UART.
 *  5. Blacken a sub-region and PARTIAL-refresh just that rectangle (A2 fast),
 *     again timing + reporting.
 *  6. ``display_deinit`` sleeps the panel; print ``epaper: PASS``.
 *
 * ## Hardware / off-target
 *
 * The EK-RA8D2 ships a parallel-RGB TFT, not e-paper, so on-panel HIL is
 * pending an IT8951 carrier on the bench (see README). ``tools/ra8_emulator``
 * models the IT8951 as an SPI device (``--eink``): it answers HRDY, the
 * GET_DEV_INFO drain and the LUTAFSR "LUT idle" poll, so this whole path runs
 * headlessly to its ``epaper: PASS`` banner -- EIL == HIL.
 *
 * The reported refresh times are informational: on silicon a GC16 full refresh
 * is hundreds of milliseconds and an A2 partial tens; under ra8_emulator the
 * modelled controller completes instantly, so the numbers are near zero. The
 * PASS verdict never depends on a timing value.
 *
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_cgc.h"
#include "ra8_display_pal.h"
#include "ra8_display_pal_eink.h"
#include "ra8_epaper.h"
#include "ra8_epd_cal.h"
#include "ra8_err.h"
#include "ra8_io_spi_bus.h"
#include "ra8_io_spi_bus_spi_b.h"
#include "ra8_isr.h"
#include "ra8_mstp.h"
#include "ra8_port_constants.h"
#include "ra8_port_utils.h"
#include "ra8_spi.h"
#include "ra8_time.h"

/* ===========================================================================
 * Compile-time configuration -- typed enums per the no-magic-number rule.
 * =========================================================================== */

/**
 * @enum ep_geom_t
 * @brief Panel + framebuffer geometry.
 *
 * @details A modest 128 x 128 8bpp panel keeps the per-pixel SPI streaming
 *          fast under ra8_emulator while still exercising a full + partial
 *          refresh. RGB565 framebuffer = 128 * 128 * 2 = 32 KiB in SRAM.
 *
 * @since 0.1.0
 */
typedef enum : uint16_t {
  k_ep_panel_w = 128U, /**< Panel + framebuffer width  (px). */
  k_ep_panel_h = 128U, /**< Panel + framebuffer height (px). */
  k_ep_cell    = 16U,  /**< Checkerboard cell size (px).     */
} ep_geom_t;

/**
 * @enum ep_region_t
 * @brief Sub-rectangle updated by the partial refresh.
 *
 * @since 0.1.0
 */
typedef enum : uint16_t {
  k_ep_box_x = 48U, /**< Partial-region left edge (px).  */
  k_ep_box_y = 48U, /**< Partial-region top edge  (px).  */
  k_ep_box_w = 32U, /**< Partial-region width      (px). */
  k_ep_box_h = 32U, /**< Partial-region height     (px). */
} ep_region_t;

/**
 * @enum ep_color_t
 * @brief RGB565 shades used by the test pattern.
 *
 * @since 0.1.0
 */
typedef enum : uint16_t {
  k_ep_white = 0xFFFFU, /**< Full white (maps to 8bpp 255). */
  k_ep_black = 0x0000U, /**< Full black (maps to 8bpp 0).   */
} ep_color_t;

/**
 * @enum ep_bus_t
 * @brief SPI + UART bring-up parameters.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ep_spi_channel  = 0U,        /**< SPI_B channel 0 drives the panel.  */
  k_ep_spi_baud_hz  = 10000000U, /**< 10 MHz (<= IT8951 24 MHz ceiling). */
  k_ep_uart_baud    = 115200U,   /**< SCI console baud.                  */
  k_ep_powerup_ms   = 50U,       /**< PLL / panel settle before init.    */
  k_ep_heartbeat_ms = 500U,      /**< LED heartbeat period after PASS.   */
} ep_bus_t;

/**
 * @enum ep_fmt_t
 * @brief UART line-formatting constants.
 *
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_ep_line_max   = 48U,  /**< Max chars in one console line.     */
  k_ep_dec_digits = 10U,  /**< Max decimal digits for a uint32_t. */
  k_ep_radix      = 10U,  /**< Decimal radix.                     */
  k_ep_cr         = '\r', /**< Ep cr.                             */
  k_ep_lf         = '\n', /**< Ep lf.                             */
} ep_fmt_t;

/**
 * @enum ep_fb_align_t
 * @brief Framebuffer alignment for clean AXI bursts.
 *
 * @since 0.1.0
 */
typedef enum : uint16_t {
  k_ep_fb_align_bytes = 64U, /**< 64-byte AXI-burst alignment. */
} ep_fb_align_t;

/**
 * @enum ep_pin_t
 * @brief IT8951 control GPIOs on the (hypothetical) e-paper carrier.
 *
 * @details These are NOT stock EK-RA8D2 board facts -- the EK ships a parallel
 *          TFT -- so they live in the app, not the board layer. They match the
 *          pins the ra8_emulator IT8951 model drives (P4_00 /RESET, P4_01 HRDY).
 *          The SPI_B COPI/CIPO/SCK routing for the carrier is documented in the
 *          README; ra8_emulator does not require it.
 *
 * @since 0.1.0
 */
typedef enum : uint16_t {
  k_ep_reset_pin = RA8_PIN(k_ra8_port_4, k_ra8_pin_0), /**< IT8951 /RESET (P4_00). */
  k_ep_hrdy_pin  = RA8_PIN(k_ra8_port_4, k_ra8_pin_1), /**< IT8951 HRDY   (P4_01). */
} ep_pin_t;

/* ===========================================================================
 * Static storage
 * =========================================================================== */

/**
 * @var s_framebuffer
 * @brief RGB565 framebuffer in SRAM, 64-byte AXI-burst aligned.
 *
 * @details The PAL hands this pointer to the e-ink backend, which converts each
 *          flushed rectangle to 8bpp greyscale on the way to the IT8951.
 *
 * @note App-owned; only this file writes it.
 * @since 0.1.0
 */
[[gnu::aligned(k_ep_fb_align_bytes)]] static uint16_t
  s_framebuffer[(uint32_t)k_ep_panel_w * (uint32_t)k_ep_panel_h];

/**
 * @var s_spi_bus
 * @brief ra8_io SPI-bus handle bound to SPI_B channel 0.
 *
 * @details Out-lives every ``ra8_epaper`` call: the epaper bus seam's ``ctx``
 *          points at this handle.
 *
 * @note File-scope so it survives past init.
 * @since 0.1.0
 */
static ra8_io_spi_bus_t s_spi_bus;

/**
 * @var s_epaper_cfg
 * @brief IT8951 descriptor handed to the e-ink backend via panel_timing.
 *
 * @details Filled in ``ep_bringup_panel_bus``; carries the injected SPI seam,
 *          the reset / busy GPIOs and the native panel size.
 *
 * @note File-scope so it out-lives ``display_init``.
 * @since 0.1.0
 */
static ra8_epaper_cfg_t s_epaper_cfg;

/** @brief PAL handle returned by ``display_init``. */
static display_handle_t* s_display = nullptr;

/** @brief Cached backend caps + framebuffer descriptor. */
static display_caps_t s_caps;
static display_fb_t   s_fb;

/** @brief PASS / FAIL banners. */
static const uint8_t k_ep_msg_pass[] = "epaper: PASS\r\n";
static const uint8_t k_ep_msg_fail[] = "epaper: FAIL\r\n";

/**
 * @var k_ep_msg_no_vcom
 * @brief Banner for the INV-VCOM-1 refusal.
 *
 * @details Deliberately distinct from the generic FAIL banner: "this panel
 *          has no trusted calibration" is a different fault from "the SPI
 *          bring-up broke", and the operator's next action differs (read
 *          the number off the flex cable and provision it, versus check
 *          the wiring).
 *
 * @note Read-only.
 * @since 0.1.0
 */
static const uint8_t k_ep_msg_no_vcom[] = "epaper: NO TRUSTED VCOM -- panel left dark\r\n";

/**
 * @enum ep_vcom_limits_t
 * @brief The plausible VCOM magnitude window for this panel family.
 *
 * @details
 * A range, never a value: the range says "no sane panel is calibrated
 * outside this", which is enough to reject a controller reporting 0 or
 * 0xFFFF, while carrying none of the per-unit information that makes a
 * hardcoded VCOM damaging. Panels in this family ship labelled around
 * -1.5 V; the window is deliberately wider than that so a legitimately
 * unusual panel is not locked out.
 *
 * @invariant ``min`` is non-zero, so a controller reporting zero can never
 *            pass the range check.
 * @invariant ``min <= max``.
 *
 * @see ra8_epd_cal_limits_mv_t
 * @since 0.1.0
 */
typedef enum : uint16_t {
  k_ep_vcom_min_mv = 200U,  /**< Lowest plausible magnitude, millivolts.  */
  k_ep_vcom_max_mv = 4000U, /**< Highest plausible magnitude, millivolts. */
} ep_vcom_limits_t;

/* ===========================================================================
 * Pure helpers (mirrored by tests/test_app_epaper_refresh.c)
 * =========================================================================== */

/**
 * @brief Checkerboard colour for a framebuffer pixel.
 *
 * @details ``black`` when ``((x/cell) + (y/cell))`` is odd, else ``white`` --
 *          a classic e-ink alignment pattern.
 *
 * @param[in] x Column (0 .. k_ep_panel_w-1).
 * @param[in] y Row    (0 .. k_ep_panel_h-1).
 *
 * @return RGB565 colour for (x, y).
 * @retval k_ep_black Odd cell.
 * @retval k_ep_white Even cell.
 *
 * @pre  x < k_ep_panel_w.
 * @pre  y < k_ep_panel_h.
 * @post No state mutated.
 * @post Return depends solely on (x, y).
 *
 * @note Pure function; thread-safe.
 * @since 0.1.0
 */
static uint16_t ep_checker_px(uint16_t x, uint16_t y)
{
  const uint32_t cell = (uint32_t)k_ep_cell;
  const uint32_t sum  = ((uint32_t)x / cell) + ((uint32_t)y / cell);
  return ((sum & 1U) != 0U) ? (uint16_t)k_ep_black : (uint16_t)k_ep_white;
}

/**
 * @brief Whether a rectangle (w, h >= 1) fits wholly inside the panel.
 *
 * @details Compound bounds guard used before the partial flush so a bad region
 *          is rejected rather than clipped. The right / bottom edge checks
 *          subsume the left / top ones for a non-empty rectangle, so the
 *          decision is the two INDEPENDENT edge tests -- one on the x axis, one
 *          on the y axis.
 *
 * @param[in] x  Rectangle left edge.
 * @param[in] y  Rectangle top edge.
 * @param[in] w  Rectangle width  (>= 1).
 * @param[in] h  Rectangle height (>= 1).
 * @param[in] pw Panel width.
 * @param[in] ph Panel height.
 *
 * @return true iff the rectangle is fully on-panel.
 * @retval true  Both the right and bottom edges are in range.
 * @retval false Either edge steps out of bounds.
 *
 * @pre  None.
 * @pre  None.
 * @post No state mutated.
 * @post Return depends solely on the inputs.
 *
 * @note Pure function; thread-safe.
 * @since 0.1.0
 */
static bool ep_region_fits(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t pw, uint16_t ph)
{
  return ((uint32_t)x + (uint32_t)w <= (uint32_t)pw) && ((uint32_t)y + (uint32_t)h <= (uint32_t)ph);
}

/**
 * @brief PASS verdict: every stage returned ``k_ra8_ok``.
 *
 * @param[in] init_err ``display_init`` result.
 * @param[in] full_err full-refresh ``display_flush`` result.
 * @param[in] part_err partial-refresh ``display_flush`` result.
 *
 * @return true iff all three stages succeeded.
 * @retval true  All three are ``k_ra8_ok``.
 * @retval false Any stage failed.
 *
 * @pre  None.
 * @pre  None.
 * @post No state mutated.
 * @post Return depends solely on the inputs.
 *
 * @note Pure function; thread-safe.
 * @since 0.1.0
 */
static bool ep_flush_ok(ra8_err_t init_err, ra8_err_t full_err, ra8_err_t part_err)
{
  return (init_err == k_ra8_ok) && (full_err == k_ra8_ok) && (part_err == k_ra8_ok);
}

/**
 * @brief Write the decimal digits of ``val`` into ``buf``; return the count.
 *
 * @param[out] buf Destination (>= k_ep_dec_digits bytes).
 * @param[in]  val Value to render.
 *
 * @return Number of digit bytes written (1 .. k_ep_dec_digits).
 * @retval 1 For ``val == 0``.
 *
 * @pre  buf != NULL.
 * @pre  buf has room for k_ep_dec_digits bytes.
 * @post buf[0..n-1] hold the decimal digits.
 * @post No other state mutated.
 *
 * @note Pure aside from writing ``buf``.
 * @since 0.1.0
 */
static uint32_t ep_u32_to_dec(uint8_t* buf, uint32_t val)
{
  if (val == 0U) {
    buf[0] = (uint8_t)'0';
    return 1U;
  }
  uint8_t  tmp[k_ep_dec_digits];
  uint32_t n = 0U;
  uint32_t v = val;
  while ((v != 0U) && (n < (uint32_t)k_ep_dec_digits)) {
    tmp[n] = (uint8_t)('0' + (uint8_t)(v % (uint32_t)k_ep_radix));
    v      = v / (uint32_t)k_ep_radix;
    n++;
  }
  for (uint32_t i = 0U; i < n; i++) {
    buf[i] = tmp[n - 1U - i];
  }
  return n;
}

/* ===========================================================================
 * Bring-up + reporting
 * =========================================================================== */

/**
 * @brief Halt forever with the red board LED on.
 *
 * @pre  None.
 * @pre  None.
 * @post Function never returns; CPU parked in WFI.
 * @post Red LED is on.
 *
 * @note IRQ-safe (no shared state mutated after entry).
 * @since 0.1.0
 */
static void ep_panic_halt(void)
{
  (void)ra8_board_led_on(k_ra8_board_led_red);
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Emit a NUL-free byte span on the SCI console (best effort).
 *
 * @param[in] data Bytes to write.
 * @param[in] len  Byte count.
 *
 * @pre  ``ra8_board_uart_console_init`` succeeded.
 * @pre  data != NULL.
 * @post ``len`` bytes have been queued to the console.
 * @post No app state mutated.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void ep_emit(const uint8_t* data, uint32_t len)
{
  (void)ra8_board_uart_console_write(data, (size_t)len);
}

/**
 * @brief Bring up clocks, MSTP, SysTick, the console and both LEDs.
 *
 * @param[out] out_pclka_hz Receives PCLKA in Hz (SPI clock source).
 *
 * @pre  Reset_Handler ran .data/.bss init.
 * @pre  out_pclka_hz != NULL.
 * @post Clocks, MSTP, SysTick, console and LEDs are live.
 * @post ``*out_pclka_hz`` holds PCLKA in Hz.
 *
 * @note Not thread-safe; single-shot helper. Panic-halts on any failure.
 * @since 0.1.0
 */
static void ep_bringup_core(uint32_t* out_pclka_hz)
{
  uint32_t cpuclk0_hz = 0U;
  if (ra8_cgc_init() != k_ra8_ok) {
    ep_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    ep_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_pclka, out_pclka_hz) != k_ra8_ok) {
    ep_panic_halt();
  }
  if (ra8_mstp_init() != k_ra8_ok) {
    ep_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    ep_panic_halt();
  }
  if (ra8_board_uart_console_init((uint32_t)k_ep_uart_baud) != k_ra8_ok) {
    ep_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led_blue) != k_ra8_ok) {
    ep_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led_red) != k_ra8_ok) {
    ep_panic_halt();
  }
}

/**
 * @brief Configure the panel GPIOs + SPI bus and fill ``s_epaper_cfg``.
 *
 * @details Drives /RESET high (output), configures HRDY as a pulled-up input,
 *          brings up SPI_B channel 0 in mode 0 as controller, binds it into an
 *          ``ra8_io_spi_bus`` and exposes it through the Ring-3 seam the IT8951
 *          descriptor carries.
 *
 * @param[in] pclka_hz PCLKA in Hz (SPI clock source).
 *
 * @pre  ``ep_bringup_core`` has run.
 * @pre  pclka_hz != 0.
 * @post On success ``s_epaper_cfg.bus.xfer8`` is non-NULL and the panel GPIOs
 *       are configured.
 * @post ``s_spi_bus`` is bound to SPI_B channel 0.
 *
 * @note Not thread-safe; single-shot helper. Panic-halts on any failure.
 * @since 0.1.0
 */
static void ep_bringup_panel_bus(uint32_t pclka_hz)
{
  if (ra8_gpio_output_init((ra8_port_pin_t)k_ep_reset_pin, k_ra8_level_high) != k_ra8_ok) {
    ep_panic_halt();
  }
  if (ra8_gpio_input_init((ra8_port_pin_t)k_ep_hrdy_pin, k_ra8_pull_up) != k_ra8_ok) {
    ep_panic_halt();
  }

  const ra8_spi_cfg_t spi_cfg = {
    .baud_hz   = (uint32_t)k_ep_spi_baud_hz,
    .pclka_hz  = pclka_hz,
    .mode      = k_ra8_spi_mode_0,
    .lsb_first = false,
    .loopback  = false,
  };
  if (ra8_spi_init((uint8_t)k_ep_spi_channel, &spi_cfg) != k_ra8_ok) {
    ep_panic_halt();
  }
  if (ra8_io_spi_bus_bind_spi_b(&s_spi_bus, (uint8_t)k_ep_spi_channel) != k_ra8_ok) {
    ep_panic_halt();
  }

  ra8_spi_bus_ops_t ops = {};
  if (ra8_io_spi_bus_as_ops(&s_spi_bus, &ops) != k_ra8_ok) {
    ep_panic_halt();
  }
  s_epaper_cfg.bus          = ops;
  s_epaper_cfg.reset_pin    = (uint16_t)k_ep_reset_pin;
  s_epaper_cfg.busy_pin     = (uint16_t)k_ep_hrdy_pin;
  s_epaper_cfg.panel_width  = (uint16_t)k_ep_panel_w;
  s_epaper_cfg.panel_height = (uint16_t)k_ep_panel_h;
  /* Waveform mode numbers are panel data, not driver data: A2 is mode 4 on
   * the M641 LUT (6 inch / 6 inch HD) and mode 6 elsewhere. The descriptor
   * seeds the M641 map so init has a valid one; an app that reaches real
   * hardware should re-derive it from ``ra8_epaper_dev_info().lut_version``
   * once the controller has answered GET_DEV_INFO. */
  if (ra8_epaper_waveform_cfg_for_lut("M641", &s_epaper_cfg.waveform) != k_ra8_ok) {
    ep_panic_halt();
  }
}

/**
 * @brief ``ra8_epd_cal`` read seam bound to the IT8951 VCOM command.
 *
 * @details Adapts the driver's context-free signature onto the seam's
 *          ``(ctx, out)`` shape. ``ctx`` is unused: the driver keeps one
 *          panel per build.
 *
 * @param[in]  ctx    Unused seam context.
 * @param[out] out_mv Receives the controller's VCOM magnitude; non-NULL.
 *
 * @return ra8_err_t Forwarded from ``ra8_epaper_get_vcom``.
 * @retval k_ra8_ok           Controller answered.
 * @retval k_ra8_err_null_ptr ``out_mv`` is NULL.
 *
 * @pre  ``ra8_epaper_init`` succeeded.
 * @pre  ``out_mv`` is writable.
 * @post No panel pixels are disturbed.
 * @post ``ctx`` is untouched.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static ra8_err_t ep_vcom_get(void* ctx, uint16_t* out_mv)
{
  (void)ctx;
  return ra8_epaper_get_vcom(out_mv);
}

/**
 * @brief ``ra8_epd_cal`` write seam bound to the IT8951 VCOM command.
 *
 * @details The driver's ``ra8_epaper_set_vcom`` verifies its own write by
 *          reading it back, so a ``k_ra8_ok`` from here means the
 *          controller confirmed the value -- not merely that bytes were
 *          clocked out.
 *
 * @param[in] ctx Unused seam context.
 * @param[in] mv  VCOM magnitude to programme, millivolts.
 *
 * @return ra8_err_t Forwarded from ``ra8_epaper_set_vcom``.
 * @retval k_ra8_ok                    Programmed and verified.
 * @retval k_ra8_err_validation_failed Readback disagreed.
 *
 * @pre  ``ra8_epaper_init`` succeeded.
 * @pre  ``mv`` was range-checked by ``ra8_epd_cal``.
 * @post On success the controller reports ``mv``.
 * @post ``ctx`` is untouched.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static ra8_err_t ep_vcom_set(void* ctx, uint16_t mv)
{
  (void)ctx;
  return ra8_epaper_set_vcom(mv);
}

/**
 * @brief Resolve this panel's VCOM and programme it, or refuse to draw.
 *
 * @details
 * The INV-VCOM-1 gate, and the reason this app cannot simply start
 * flushing after ``display_init``. ``ra8_epd_cal_resolve`` walks the
 * controller's own persisted value, then the per-device record, then an
 * operator-supplied value, and fails rather than inventing one;
 * ``ra8_epd_cal_apply`` programmes the winner and confirms it by readback.
 * Only then does the driver permit a refresh at all.
 *
 * The store seams are left unbound on purpose. Reading the per-device
 * record needs a fault-tolerant extra-MRAM probe (a virgin page bus-faults
 * on the first read), which today exists only inside the DFU anti-rollback
 * module and has not been lifted into a shared helper.
 * TODO(shared fault-tolerant extra-MRAM probe helper + per-device config
 * record reader): bind ``store.read`` / ``store.write`` once that lands, at
 * which point a provisioned unit resolves from its own record instead of
 * trusting whatever the controller board happens to hold.
 *
 * Until then this app resolves from the controller's persisted value,
 * which is what makes a vendor-provisioned driver board work out of the
 * box, and refuses to drive anything that cannot supply one.
 *
 * @return ``true`` when a VCOM was resolved, programmed and verified.
 * @retval true  Refresh is permitted.
 * @retval false No trusted VCOM; the caller must leave the panel dark.
 *
 * @pre  ``display_init`` succeeded, so ``ra8_epaper_init`` has run.
 * @pre  The SPI bus seam is bound.
 * @post On ``true`` the controller reports the resolved VCOM.
 * @post On ``false`` no display command has been issued.
 *
 * @note Not thread-safe; boot-path use only.
 * @since 0.1.0
 */
static bool ep_calibrate_vcom(void)
{
  const ra8_epd_cal_cfg_t cal_cfg = {
    .limits          = {.min_mv = (uint16_t)k_ep_vcom_min_mv, .max_mv = (uint16_t)k_ep_vcom_max_mv},
    .panel           = {.get = ep_vcom_get, .set = ep_vcom_set, .ctx = nullptr},
    .store           = {.read = nullptr, .write = nullptr, .ctx = nullptr},
    .has_provisioned = false,
    .provisioned_mv  = 0U,
  };

  ra8_epd_cal_result_t cal = {};
  if (ra8_epd_cal_resolve(&cal_cfg, &cal) != k_ra8_ok) {
    return false;
  }
  return ra8_epd_cal_apply(&cal_cfg, &cal) == k_ra8_ok;
}

/**
 * @brief Paint the whole framebuffer with the checkerboard pattern.
 *
 * @pre  ``display_get_framebuffer`` populated ``s_fb``.
 * @pre  ``s_fb.pixels`` is reachable.
 * @post Every framebuffer pixel holds its checker colour.
 * @post No other state mutated.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void ep_paint_checker(void)
{
  uint16_t* px = (uint16_t*)s_fb.pixels;
  for (uint16_t y = 0U; y < s_fb.height_px; ++y) {
    const uint32_t row = (uint32_t)y * (uint32_t)s_fb.width_px;
    for (uint16_t x = 0U; x < s_fb.width_px; ++x) {
      px[row + (uint32_t)x] = ep_checker_px(x, y);
    }
  }
}

/**
 * @brief Blacken the partial-refresh sub-rectangle in the framebuffer.
 *
 * @pre  ``display_get_framebuffer`` populated ``s_fb``.
 * @pre  The region fits inside the framebuffer.
 * @post Every pixel of the region is ``k_ep_black``.
 * @post Pixels outside the region are untouched.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void ep_paint_region_black(void)
{
  uint16_t*      px      = (uint16_t*)s_fb.pixels;
  const uint16_t y_limit = (uint16_t)((uint16_t)k_ep_box_y + (uint16_t)k_ep_box_h);
  const uint16_t x_limit = (uint16_t)((uint16_t)k_ep_box_x + (uint16_t)k_ep_box_w);
  for (uint16_t y = (uint16_t)k_ep_box_y; y < y_limit; ++y) {
    const uint32_t row = (uint32_t)y * (uint32_t)s_fb.width_px;
    for (uint16_t x = (uint16_t)k_ep_box_x; x < x_limit; ++x) {
      px[row + (uint32_t)x] = (uint16_t)k_ep_black;
    }
  }
}

/**
 * @brief Print ``epaper: <label> ms=<dt> px=<count>`` on the console.
 *
 * @param[in] label  NUL-terminated stage label (e.g. "full-refresh").
 * @param[in] dt_ms  Measured flush time in milliseconds.
 * @param[in] pixels Pixels refreshed this stage.
 *
 * @pre  label != NULL.
 * @pre  ``ra8_board_uart_console_init`` succeeded.
 * @post One formatted line has been queued to the console.
 * @post No app state mutated.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void ep_report_stage(const char* label, uint32_t dt_ms, uint32_t pixels)
{
  static const uint8_t k_prefix[] = "epaper: ";
  static const uint8_t k_ms_sep[] = " ms=";
  static const uint8_t k_px_sep[] = " px=";
  uint8_t              line[k_ep_line_max];
  uint32_t             pos = 0U;
  for (uint32_t i = 0U; i < (sizeof(k_prefix) - 1U); i++) {
    line[pos++] = k_prefix[i];
  }
  for (uint32_t i = 0U; (label[i] != '\0') && (pos < (uint32_t)k_ep_line_max); i++) {
    line[pos++] = (uint8_t)label[i];
  }
  for (uint32_t i = 0U; i < (sizeof(k_ms_sep) - 1U); i++) {
    line[pos++] = k_ms_sep[i];
  }
  pos += ep_u32_to_dec(&line[pos], dt_ms);
  for (uint32_t i = 0U; i < (sizeof(k_px_sep) - 1U); i++) {
    line[pos++] = k_px_sep[i];
  }
  pos += ep_u32_to_dec(&line[pos], pixels);
  line[pos++] = (uint8_t)k_ep_cr;
  line[pos++] = (uint8_t)k_ep_lf;
  ep_emit(line, pos);
}

/**
 * @brief Print ``epaper: init ok panel=<w>x<h>``.
 *
 * @pre  ``display_init`` succeeded.
 * @pre  ``ra8_board_uart_console_init`` succeeded.
 * @post One line queued to the console.
 * @post No app state mutated.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void ep_report_init(void)
{
  static const uint8_t k_prefix[] = "epaper: init ok panel=";
  uint8_t              line[k_ep_line_max];
  uint32_t             pos = 0U;
  for (uint32_t i = 0U; i < (sizeof(k_prefix) - 1U); i++) {
    line[pos++] = k_prefix[i];
  }
  pos += ep_u32_to_dec(&line[pos], (uint32_t)k_ep_panel_w);
  line[pos++] = (uint8_t)'x';
  pos += ep_u32_to_dec(&line[pos], (uint32_t)k_ep_panel_h);
  line[pos++] = (uint8_t)k_ep_cr;
  line[pos++] = (uint8_t)k_ep_lf;
  ep_emit(line, pos);
}

/**
 * @brief Drive the full (checker/GC16) then partial (box/A2) refresh cycle.
 *
 * @details Repaints the framebuffer and flushes it twice through the display
 *          PAL: a full-panel quality refresh, then a bounds-checked partial
 *          fast refresh of the demo box. Each stage's flush time and pixel
 *          count are reported. The partial stage is skipped -- leaving
 *          @p part_err at ``k_ra8_err_invalid_arg`` -- when the box does not
 *          fit the panel.
 *
 * @param[out] full_err Receives the full-refresh flush result.
 * @param[out] part_err Receives the partial-refresh flush result, or
 *                      ``k_ra8_err_invalid_arg`` when the box was out of bounds.
 *
 * @pre  full_err != NULL and part_err != NULL.
 * @pre  ``display_init`` succeeded and s_display is valid.
 * @post ``*full_err`` and ``*part_err`` hold this cycle's flush results.
 * @post The panel has been refreshed at least once.
 *
 * @note Not thread-safe; single-caller boot context.
 * @since 0.1.0
 */
static void ep_run_refresh_cycle(ra8_err_t* full_err, ra8_err_t* part_err)
{
  /* Full refresh: checkerboard + GC16 (quality). */
  ep_paint_checker();
  const uint32_t t0_full = ra8_now_ms();
  *full_err = display_flush(s_display, display_full_rect(s_display), k_display_refresh_quality);
  ep_report_stage("full-refresh",
                  ra8_now_ms() - t0_full,
                  (uint32_t)k_ep_panel_w * (uint32_t)k_ep_panel_h);

  /* Partial refresh: blacken a sub-rect + A2 (fast), bounds-checked first. */
  *part_err = k_ra8_err_invalid_arg;
  if (ep_region_fits((uint16_t)k_ep_box_x,
                     (uint16_t)k_ep_box_y,
                     (uint16_t)k_ep_box_w,
                     (uint16_t)k_ep_box_h,
                     (uint16_t)k_ep_panel_w,
                     (uint16_t)k_ep_panel_h)) {
    ep_paint_region_black();
    const display_rect_t box = {
      .x = (uint16_t)k_ep_box_x,
      .y = (uint16_t)k_ep_box_y,
      .w = (uint16_t)k_ep_box_w,
      .h = (uint16_t)k_ep_box_h,
    };
    const uint32_t t0_part = ra8_now_ms();
    *part_err              = display_flush(s_display, box, k_display_refresh_fast);
    ep_report_stage("partial-refresh",
                    ra8_now_ms() - t0_part,
                    (uint32_t)k_ep_box_w * (uint32_t)k_ep_box_h);
  }
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
/**
 * @brief Application entry: e-ink full + partial refresh through the PAL.
 *
 * @return Never returns.
 *
 * @pre  Reset_Handler copied .data and zeroed .bss.
 * @pre  SystemInit set VTOR / FPU / priority grouping.
 * @post On success the CPU heartbeats after ``epaper: PASS``.
 * @post On any HAL error the console prints ``epaper: FAIL`` and the red LED
 *       latches on.
 *
 * @since 0.1.0
 */
int32_t main(void)
{
  uint32_t pclka_hz = 0U;
  ep_bringup_core(&pclka_hz);
  ra8_isr_globals_enable();
  ra8_delay_ms((uint32_t)k_ep_powerup_ms);
  ep_bringup_panel_bus(pclka_hz);

  const display_cfg_t cfg = {
    .iface             = &k_display_backend_eink_it8951,
    .framebuffer       = s_framebuffer,
    .framebuffer_bytes = sizeof(s_framebuffer),
    .width_px          = (uint16_t)k_ep_panel_w,
    .height_px         = (uint16_t)k_ep_panel_h,
    .pixfmt            = k_display_pixfmt_rgb565,
    .panel_timing      = &s_epaper_cfg,
  };

  const ra8_err_t init_err = display_init(&cfg, &s_display);
  if ((init_err != k_ra8_ok) || (display_get_caps(s_display, &s_caps) != k_ra8_ok) ||
      (display_get_framebuffer(s_display, &s_fb) != k_ra8_ok)) {
    ep_emit(k_ep_msg_fail, (uint32_t)(sizeof(k_ep_msg_fail) - 1U));
    ep_panic_halt();
  }
  ep_report_init();

  /* INV-VCOM-1: nothing gets refreshed until this panel's own bias has
   * been resolved and confirmed. Deliberately a halt with its own banner
   * rather than a best-effort draw -- a blank screen is a support call,
   * a DC-biased panel is landfill. The driver would refuse the refresh
   * anyway; failing here says why. */
  if (!ep_calibrate_vcom()) {
    ep_emit(k_ep_msg_no_vcom, (uint32_t)(sizeof(k_ep_msg_no_vcom) - 1U));
    ep_panic_halt();
  }

  ra8_err_t full_err = k_ra8_err_invalid_arg;
  ra8_err_t part_err = k_ra8_err_invalid_arg;
  ep_run_refresh_cycle(&full_err, &part_err);

  (void)display_deinit(s_display); /* sleep the panel */

  if (!ep_flush_ok(init_err, full_err, part_err)) {
    ep_emit(k_ep_msg_fail, (uint32_t)(sizeof(k_ep_msg_fail) - 1U));
    ep_panic_halt();
  }
  ep_emit(k_ep_msg_pass, (uint32_t)(sizeof(k_ep_msg_pass) - 1U));

  while (1) {
    (void)ra8_board_led_toggle(k_ra8_board_led_blue);
    ra8_delay_ms((uint32_t)k_ep_heartbeat_ms);
  }
  return 0;
}
#pragma GCC diagnostic pop
