/**
 * @file examples/ek_ra8d2/hw_validated/manual/bedroom_ui_panel/main.c
 * @brief Bare-metal (no ThreadX) GUIX render of the shared bedroom_ui on the
 *        EK-RA8D2 1024x600 panel via GLCDC
 *
 * @deprecated GUIX retirement -- tracked by issue #81 (replaced by the
 * ra_reflow chrome renderer, issue #80); scheduled for wholesale removal.
 *
 * @par Tag
 * [Ring 5 / APP] {World: NS}
 *
 * @details
 * Draws the shared GUIX UI (``examples/shared/bedroom_ui/bedroom_ui.c``) -- a
 * 3-tab smart-room surface -- on the real GLCDC panel. The same UI source can be
 * previewed without hardware by booting this app in the board emulator
 * (``make sim-bedroom_ui_panel``); only the display backend and RTOS bind are
 * wired per target, the widget code is identical.
 *
 * This app is single-threaded GUIX with NO ThreadX. It uses GUIX's generic
 * RTOS surface (``GX_DISABLE_THREADX_BINDING``) bound to a bare-metal shim
 * (``port/guix/gx_generic_rtos_bare.c``) whose time source is the
 * SysTick-backed ``ra_time_ms()``. The GUIX init + drive sequence:
 *
 *     gx_system_initialize
 *       -> gx_display_create(setup_fn)          (GLCDC 565RGB driver)
 *       -> gx_canvas_create over the SDRAM framebuffer
 *       -> gx_window_root_create
 *       -> bedroom_ui_create
 *       -> gx_widget_show(root)
 *       -> loop { gx_host_pump; gx_system_canvas_refresh }
 *
 * Pipeline coupling: the canvas memory, the GLCDC GR1 scan-out address, and
 * the GUIX display driver's framebuffer base are ALL the same SDRAM pointer
 * (``s_framebuffer``). ``display_init`` programmes GLCDC GR1 to scan that
 * address; ``display_bind_guix`` binds the same pointer into the 565RGB
 * driver; ``gx_canvas_create`` hands it to GUIX as the canvas. GUIX therefore
 * paints straight into the buffer the panel is scanning out -- no copy, no
 * back-buffer flip (the v1 single-buffer driver in
 * ``port/guix/gx_display_driver_ra_glcdc.c``).
 *
 * Why SDRAM: a full 1024x600 RGB565 framebuffer is 1024*600*2 = 1.2 MiB,
 * which does not fit the 2 MiB SRAM alongside .data/.bss/stack. It lives in
 * external SDRAM at 0x68000000 via the linker's ``.sdram_data`` (NOLOAD)
 * section. GUIX clears + paints the whole canvas on its first refresh, so the
 * NOLOAD (uninitialised) backing is fine.
 *
 * Clocks / SDRAM / GLCDC bring-up mirrors ``lcd_color_cycle`` (clocks, MSTP,
 * SysTick, SDRAMC, panel power, GLCDC), folded into the display PAL's
 * ``display_init``. On any init
 * failure the app latches the red LED and parks in WFI (the panic-halt
 * pattern the other panel examples and the board emulator key off).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra_board_ek_ra8d2.h"
#include "ra_cgc.h"
#include "ra_err.h"
#include "ra_isr.h"
#include "ra_mstp.h"
#include "ra_sdramc.h"
#include "ra_time.h"

/*
 * Cross-build-only includes: the host unit-test harness does not link the
 * GUIX vendor tree, so guard those headers the same way the other GUIX-aware
 * examples (threadx_guix_demo) do.
 */
#ifndef RA_SIMULATOR_MODE
#include "bedroom_ui.h"
#include "gx_api.h"
#include "gx_host_run.h"
#include "ra_display_pal.h"
#include "ra_display_pal_guix.h"
#include "ra_display_pal_lcd.h"
#include "ra_panel.h"
#include "ra_panel_timing.h"
#include "ra_touch.h"
#endif

/* =============================================================================
 * Compile-time configuration -- typed enums per the no-magic-number rule.
 * =============================================================================
 */

/**
 * @enum bedroom_panel_dim_t
 * @brief Panel framebuffer dimensions (full native 1024x600 from the BSP).
 *
 * @details
 * Sourced from the BSP panel descriptor (``ra_panel.h``) so the geometry is
 * never hardcoded -- a different panel swaps in its own descriptor through the
 * include path without touching this file.
 *
 * @since 0.1.0
 */
typedef enum : uint16_t {
  k_panel_fb_w = k_panel_width_px,  /**< Framebuffer width  (pixels). */
  k_panel_fb_h = k_panel_height_px, /**< Framebuffer height (pixels). */
} bedroom_panel_dim_t;

/**
 * @enum bedroom_panel_bpp_t
 * @brief Bytes per pixel for RGB565 (used in framebuffer-size math).
 *
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_panel_bpp_rgb565 = 2U, /**< 2 bytes per RGB565 pixel. */
} bedroom_panel_bpp_t;

/**
 * @brief Framebuffer alignment for AXI-burst friendliness.
 *
 * @since 0.1.0
 */
enum : uint8_t {
  k_panel_fb_align = 64U, /**< 64-byte AXI burst alignment. */
};

/**
 * @enum bedroom_pace_t
 * @brief Pre-bring-up settle delay (milliseconds).
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_panel_settle_ms = 500U, /**< PLL / SDRAM / panel-POR settle. */
} bedroom_pace_t;

#ifndef RA_SIMULATOR_MODE

/**
 * @enum bedroom_touch_cfg_t
 * @brief GT911 touch-controller wiring on the EK-RA8D2 carrier.
 *
 * @details
 * The EK-RA8D2 ereader carrier sits the GoodIX GT911 on IIC_B channel 0 at its
 * default 7-bit address. We poll the controller from the render loop rather
 * than taking its INT line, so the IRQ pin is left unset -- ::ra_touch_open
 * then skips ICU pin programming and ::ra_touch_read drains frames on demand.
 * Values live here (the application BSP), never hardcoded inside the driver.
 *
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_touch_i2c_channel = 0U,                                /**< IIC_B channel 0.        */
  k_touch_addr_7b     = 0x5DU,                             /**< GT911 default address.  */
  k_touch_irq_unset   = (uint8_t)k_ra_touch_irq_pin_unset, /**< Polling-only.   */
  k_touch_max_points  = 1U,                                /**< One contact = one tap.  */
} bedroom_touch_cfg_t;

/* =============================================================================
 * Module-static state -- no dynamic allocation (NASA P10 Rule 3).
 * =============================================================================
 */

/**
 * @var s_framebuffer
 * @brief 1024x600 RGB565 framebuffer (1.2 MiB) in external SDRAM.
 *
 * @details
 * Placed in the linker's ``.sdram_data`` (NOLOAD) section so it lands at
 * 0x68000000 (external SDRAM) rather than the 2 MiB SRAM, which has no room
 * for it alongside .data/.bss/stack. 64-byte aligned so a GLCDC AXI burst
 * never straddles a cache line. This single buffer is shared three ways: it
 * is the GLCDC GR1 scan-out source, the display PAL's bound framebuffer, and
 * the GUIX canvas memory.
 */
static uint16_t s_framebuffer[(size_t)k_panel_fb_h * (size_t)k_panel_fb_w]
  __attribute__((section(".sdram_data"), aligned(k_panel_fb_align)));

/**
 * @brief Display PAL config selecting the LCD (GLCDC) backend.
 *
 * @details
 * The framebuffer pointer here is what ``display_init`` programmes into the
 * GLCDC GR1 scan-out address AND what ``display_bind_guix`` binds into the
 * GUIX 565RGB driver -- coupling the panel and GUIX onto one buffer. To
 * target a different panel/backend later, change ``iface`` (and the matching
 * ``panel_timing``); the GUIX widget tree below stays untouched.
 */
static const display_cfg_t k_display_cfg = {
  .iface             = &k_display_backend_lcd_ra_glcdc,
  .framebuffer       = s_framebuffer,
  .framebuffer_bytes = (uint32_t)sizeof(s_framebuffer),
  .width_px          = (uint16_t)k_panel_fb_w,
  .height_px         = (uint16_t)k_panel_fb_h,
  .pixfmt            = k_display_pixfmt_rgb565,
  .panel_timing      = &k_ra_panel_ek_ra8d2_timing,
};

/** @brief PAL handle returned by ``display_init``. */
static display_handle_t* s_display_pal = nullptr;

/* GUIX control blocks -- one display / canvas / root for the whole app. */
static GX_DISPLAY     s_gxdisp;
static GX_CANVAS      s_canvas;
static GX_WINDOW_ROOT s_gxroot;

/**
 * @var s_touch_ready
 * @brief True once ::ra_touch_open has brought the GT911 up successfully.
 *
 * @details
 * The render loop only polls ::ra_touch_read when this is set, so a board
 * without a reachable touch IC still renders the UI (touch is simply inert)
 * instead of churning failing I2C reads every frame.
 */
static bool s_touch_ready = false;

/**
 * @var s_touch_down
 * @brief Tracks whether the previous poll already saw a contact (debounce).
 *
 * @details
 * The GT911 reports a contact on every frame the finger is held, but GUIX
 * wants one PEN_DOWN per press. We emit the down/up pair on the rising edge
 * (no contact -> contact) and ignore subsequent held frames until release, so
 * one physical tap becomes exactly one GUIX click.
 */
static bool s_touch_down = false;

/* =============================================================================
 * Panic-halt
 * =============================================================================
 */

/**
 * @brief Latch the red LED and park the CPU forever.
 *
 * @details
 * Matches the panic-halt pattern in ``lcd_color_cycle`` / ``threadx_guix_demo``
 * and the range the board emulator treats as the failure witness. Used on any
 * boot / GUIX init error -- there is no recovery path for a clock, panel, or
 * framework misconfiguration.
 *
 * @pre Called only after a fatal error during boot.
 * @post The red board LED is on; only a debugger or external reset wakes the CPU.
 *
 * @since 0.1.0
 */
static void bedroom_panic_halt(void)
{
  (void)ra_board_led_init(k_ra_board_led_red);
  (void)ra_board_led_on(k_ra_board_led_red);
  while (1) {
    __asm__ volatile("wfi");
  }
}

/* =============================================================================
 * Phase 1 -- clocks, MSTP, SysTick, SDRAM, GLCDC panel (HAL bring-up)
 * =============================================================================
 */

/**
 * @brief Bring up clocks, MSTP, the 1 kHz SysTick tick, and the board LEDs.
 *
 * @details
 * First boot phase, mirroring ``lcd_color_cycle``'s ``lcd_bringup_clocks``.
 * The SysTick tick started here is what backs ``ra_time_ms()`` -- the GUIX
 * bare-metal bind's time source -- and ``ra_delay_ms`` used in phase 2. Any
 * failure halts in the red-LED panic loop. Global IRQs are enabled on the way
 * out so SysTick fires.
 *
 * @pre Reset_Handler has populated .data / .bss; IRQs still globally disabled.
 * @post Clocks, MSTP, SysTick (1 kHz) and both board LEDs are initialised.
 * @post Global IRQs are enabled.
 *
 * @note Not thread-safe; single-shot startup helper.
 *
 * @since 0.1.0
 */
static void bedroom_bringup_clocks(void)
{
  uint32_t cpuclk0_hz = 0U;
  if (ra_cgc_init() != k_ra_ok) {
    bedroom_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_cpuclk0, &cpuclk0_hz) != k_ra_ok) {
    bedroom_panic_halt();
  }
  if (ra_mstp_init() != k_ra_ok) {
    bedroom_panic_halt();
  }
  if (ra_time_init(cpuclk0_hz) != k_ra_ok) {
    bedroom_panic_halt();
  }
  if (ra_board_led_init(k_ra_board_led_blue) != k_ra_ok) {
    bedroom_panic_halt();
  }
  if (ra_board_led_init(k_ra_board_led_red) != k_ra_ok) {
    bedroom_panic_halt();
  }
  ra_isr_globals_enable();
}

/**
 * @brief Bring up external SDRAM and the GLCDC panel pipeline.
 *
 * @details
 * Second boot phase. A settle delay lets the PLLs, SDRAM, and panel POR
 * stabilise (the GLCDC otherwise sometimes starts before LCDCLK is stable and
 * the panel comes up white on cold boot). ``ra_sdramc_init`` brings up the
 * 0x68000000 region the framebuffer lives in; ``display_init`` then folds the
 * panel power-on + GLCDC routing/clock/controller setup + GR1-show into one
 * call (driving GR1 to scan out ``s_framebuffer``). Any failure halts in the
 * red-LED panic loop.
 *
 * @pre ::bedroom_bringup_clocks has run; IRQs are enabled.
 * @post SDRAM is up; GLCDC is scanning out ``s_framebuffer`` on GR1.
 *
 * @note Not thread-safe; single-shot startup helper.
 *
 * @since 0.1.0
 */
static void bedroom_bringup_panel(void)
{
  /* PLLs / SDRAM / panel all need a few hundred ms after power-on to settle. */
  ra_delay_ms((uint32_t)k_panel_settle_ms);

  if (ra_sdramc_init() != k_ra_ok) {
    bedroom_panic_halt();
  }

  /* display_init runs the full LCD bring-up (panel power-on, GLCDC pin/clock
   * setup, settle, controller init, BG clear, start(true), GR1 show) and
   * programmes GLCDC GR1 to scan out cfg->framebuffer == s_framebuffer. */
  if (display_init(&k_display_cfg, &s_display_pal) != k_ra_ok) {
    bedroom_panic_halt();
  }
}

/* =============================================================================
 * Phase 2 -- GUIX bring-up (single-threaded, bare-metal bind)
 * =============================================================================
 */

/**
 * @brief Stand up GUIX over the SDRAM framebuffer and build the bedroom UI.
 *
 * @details
 * Stands up GUIX on the GLCDC panel: ask the display PAL for the GLCDC 565RGB
 * setup-fn through the backend-agnostic ``display_bind_guix`` seam (which also binds
 * ``s_framebuffer`` into the driver), initialise GUIX, create the display +
 * canvas (canvas memory == ``s_framebuffer`` == GLCDC scan-out buffer) + root
 * window, then populate the shared ``bedroom_ui`` widget tree. No
 * ``gx_system_memory_allocator_set``: GUIX's allocator is only needed by paths
 * this UI does not use (the host preview sets one for symmetry; the shared
 * widget tree itself allocates nothing dynamic). Any failure halts.
 *
 * @pre ::bedroom_bringup_panel has run (GLCDC scanning out s_framebuffer).
 * @post GUIX is initialised; the bedroom UI tree exists; tab 0 is active;
 *       ``gx_widget_show(root)`` has been issued.
 *
 * @note Not thread-safe; single-shot GUIX bring-up.
 *
 * @since 0.1.0
 */
static void bedroom_bringup_guix(void)
{
  /* Ask the PAL for the GUIX setup-fn + dimensions. The LCD backend forwards
   * to port/guix/ and binds s_framebuffer into the 565RGB driver, so the
   * canvas, the driver, and GLCDC GR1 all share one buffer. */
  display_guix_attach_t attach = {nullptr, 0U, 0U};
  if (display_bind_guix(s_display_pal, &attach) != k_ra_ok) {
    bedroom_panic_halt();
  }
  if (gx_system_initialize() != GX_SUCCESS) {
    bedroom_panic_halt();
  }
  /* Cast the PAL's opaque setup-fn back to GUIX's typed signature at the call
   * site (the one place GUIX types meet the PAL), as ra_display_pal_guix.h shows. */
  if (gx_display_create(&s_gxdisp,
                        "ra8d2-glcdc",
                        (UINT (*)(GX_DISPLAY*))attach.setup_fn,
                        (GX_VALUE)attach.width_px,
                        (GX_VALUE)attach.height_px) != GX_SUCCESS) {
    bedroom_panic_halt();
  }
  if (gx_canvas_create(&s_canvas,
                       "ra8d2-canvas",
                       &s_gxdisp,
                       GX_CANVAS_MANAGED_VISIBLE,
                       (UINT)attach.width_px,
                       (UINT)attach.height_px,
                       (GX_COLOR*)s_framebuffer,
                       (ULONG)attach.width_px * (ULONG)attach.height_px *
                         (ULONG)k_panel_bpp_rgb565) != GX_SUCCESS) {
    bedroom_panic_halt();
  }
  /* GUIX canvases come up with GX_CANVAS_VISIBLE clear; without this the first
   * refresh decides there is nothing to draw. */
  if (gx_canvas_show(&s_canvas) != GX_SUCCESS) {
    bedroom_panic_halt();
  }
  GX_RECTANGLE root_rect;
  gx_utility_rectangle_define(&root_rect,
                              0,
                              0,
                              (GX_VALUE)(attach.width_px - 1U),
                              (GX_VALUE)(attach.height_px - 1U));
  if (gx_window_root_create(&s_gxroot, "ra8d2-root", &s_canvas, GX_STYLE_NONE, 0U, &root_rect) !=
      GX_SUCCESS) {
    bedroom_panic_halt();
  }
  /* Build the SAME shared UI the host preview draws. */
  if (bedroom_ui_create(&s_gxdisp, &s_gxroot) != k_ra_ok) {
    bedroom_panic_halt();
  }
  if (gx_widget_show((GX_WIDGET*)&s_gxroot) != GX_SUCCESS) {
    bedroom_panic_halt();
  }
}

/* =============================================================================
 * Phase 3 -- GT911 touch bring-up + per-frame pen-event injection
 * =============================================================================
 */

/**
 * @brief Bring up the GT911 touch controller (non-fatal on failure).
 *
 * @details
 * Opens ::ra_touch against the EK-RA8D2 carrier's GT911 (IIC_B channel 0,
 * polling-only -- see ::bedroom_touch_cfg_t) so the render loop can drain
 * contacts with ::ra_touch_read. Unlike the clock / panel / GUIX phases this
 * is deliberately NOT a panic on failure: a board with no reachable touch IC
 * (or the board emulator, where the I2C transport is not modelled) must still
 * show the UI. ::s_touch_ready gates the loop's polling so the UI renders
 * either way; on real hardware ::ra_touch_open succeeds and taps flow through.
 *
 * @pre ::bedroom_bringup_clocks has run (MSTP up, IRQs enabled).
 * @pre ::bedroom_bringup_guix has run (the widget tree exists to receive taps).
 * @post ::s_touch_ready is true iff the GT911 answered its product-id probe.
 *
 * @note Not thread-safe; single-shot startup helper.
 *
 * @since 0.1.0
 */
static void bedroom_bringup_touch(void)
{
  const ra_touch_cfg_t touch_cfg = {
    .i2c_channel = (uint8_t)k_touch_i2c_channel,
    .target_7b   = (uint8_t)k_touch_addr_7b,
    .irq_pin     = (uint8_t)k_touch_irq_unset,
    .max_points  = (uint8_t)k_touch_max_points,
  };
  s_touch_ready = (ra_touch_open(&touch_cfg) == k_ra_ok);
}

/**
 * @brief Inject a GUIX pen down/up pair at a panel coordinate.
 *
 * @details
 * Injects a GUIX pen event: build a zeroed GX_EVENT, stamp the contact point into
 * ``gx_event_payload.gx_event_pointdata``, then send GX_EVENT_PEN_DOWN
 * followed by GX_EVENT_PEN_UP. GUIX hit-tests the point, routes it to the tab
 * button under it, and that button emits GX_EVENT_CLICKED -- which the shared
 * bedroom_ui background handler turns into a tab switch. A tap is exactly this
 * down-then-up pair, so the same code drives on-panel touch and the host mouse.
 *
 * Unlike the host preview, the event's ``gx_event_display_handle`` MUST be set
 * to this display's ``gx_display_driver_data``: ``_gx_system_top_root_find``
 * routes a pen event only to a root window whose display handle matches, and the
 * GLCDC GUIX driver stores a non-NULL auxiliary record there (the host 565rgb
 * driver leaves it NULL, so the host's zeroed handle matches by luck). Without
 * this the hit-test finds no root and the tap is silently dropped -- so this is
 * required for real on-panel touch, not just the emulator.
 *
 * @param[in] x Contact X in panel pixels (canvas-relative).
 * @param[in] y Contact Y in panel pixels (canvas-relative).
 *
 * @return Nothing.
 *
 * @pre ::bedroom_bringup_guix has run (the GUIX event system is up).
 * @pre ``x`` / ``y`` are within the canvas (GUIX clips out-of-range hits).
 * @post A PEN_DOWN then PEN_UP event has been queued to the GUIX event ring.
 * @post No driver or widget state is mutated directly by this helper.
 *
 * @note Not thread-safe; called only from the single render thread.
 *
 * @since 0.1.0
 */
static void bedroom_send_pen(uint16_t x, uint16_t y)
{
  GX_EVENT ev;
  (void)memset(&ev, 0, sizeof(ev));
  ev.gx_event_payload.gx_event_pointdata.gx_point_x = (GX_VALUE)x;
  ev.gx_event_payload.gx_event_pointdata.gx_point_y = (GX_VALUE)y;
  ev.gx_event_target                                = GX_NULL;
  ev.gx_event_display_handle = (ULONG)(uintptr_t)s_gxdisp.gx_display_driver_data;
  ev.gx_event_type           = GX_EVENT_PEN_DOWN;
  (void)gx_system_event_send(&ev);
  ev.gx_event_type = GX_EVENT_PEN_UP;
  (void)gx_system_event_send(&ev);
}

/**
 * @brief Poll the GT911 once and turn a fresh contact into a GUIX tap.
 *
 * @details
 * Drains one touch frame with ::ra_touch_read (capacity one -- a tab tap is a
 * single contact). On the rising edge of contact (none last frame, one this
 * frame) it injects a pen down/up pair via ::bedroom_send_pen and latches
 * ::s_touch_down so a finger held across frames produces only one click; when
 * the frame comes back empty the latch clears, arming the next tap. A failed
 * read is treated as "no contact" so a transient bus error never wedges the
 * UI. No-op until ::s_touch_ready, so a board without touch just renders.
 *
 * @return Nothing.
 *
 * @pre ::bedroom_bringup_touch has run (::s_touch_ready reflects the IC state).
 * @pre ::bedroom_bringup_guix has run (events have somewhere to go).
 * @post At most one PEN_DOWN/PEN_UP pair is queued per physical tap.
 * @post ::s_touch_down mirrors whether a contact is currently held.
 *
 * @note Not thread-safe; called only from the single render thread.
 *
 * @since 0.1.0
 */
static void bedroom_pump_touch(void)
{
  if (!s_touch_ready) {
    return;
  }
  ra_touch_point_t pt  = {};
  uint8_t          got = 0U;
  if (ra_touch_read(&pt, (uint8_t)k_touch_max_points, &got) != k_ra_ok) {
    s_touch_down = false;
    return;
  }
  if (got > 0U) {
    if (!s_touch_down) {
      bedroom_send_pen(pt.x, pt.y);
      s_touch_down = true;
    }
  } else {
    s_touch_down = false;
  }
}

/**
 * @brief Drive GUIX forever: dispatch events, then refresh the canvas.
 *
 * @details
 * The single-threaded drive the host preview proves: poll the GT911 for a tap
 * (::bedroom_pump_touch, which injects the matching GUIX pen events), then
 * ``gx_host_pump`` drains and dispatches the generic event ring (those pen
 * events land here and become tab switches), then ``gx_system_canvas_refresh``
 * repaints dirty widgets into ``s_framebuffer``, which the GLCDC is already
 * scanning out -- so the panel shows the result on the next frame. A short
 * ``ra_delay_ms`` paces the loop (~60 Hz) and keeps SysTick advancing
 * ``ra_time_ms`` for the GUIX timer.
 *
 * On the board emulator this reaches its first full paint within the first
 * few chunks; the emulator then runs the loop until its wall-clock budget and
 * snapshots the framebuffer.
 *
 * @pre ::bedroom_bringup_guix has run.
 * @pre ::bedroom_bringup_touch has run (touch polling is gated, so safe either way).
 * @post Never returns.
 *
 * @since 0.1.0
 */
static void bedroom_run_forever(void)
{
  enum : uint32_t {
    k_frame_pace_ms = 16U, /**< ~60 Hz cooperative pacing. */
  };
  /* Force the first frame to land immediately. */
  gx_host_pump();
  (void)gx_system_canvas_refresh();
  while (1) {
    bedroom_pump_touch();
    gx_host_pump();
    (void)gx_system_canvas_refresh();
    ra_delay_ms(k_frame_pace_ms);
  }
}

#endif /* !RA_SIMULATOR_MODE */

/* =============================================================================
 * main() -- HAL bring-up, GUIX bring-up, then the bare-metal drive loop.
 * =============================================================================
 */

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"

/**
 * @brief Application entry: bring up clocks/SDRAM/GLCDC + GUIX + touch, then loop.
 *
 * @return Never returns (the drive loop runs forever); 0 only to satisfy the
 *         signature.
 *
 * @pre Reset_Handler has copied .data + zeroed .bss; SystemInit has set VTOR,
 *      FPU, caches and priority grouping.
 * @post The panel shows the bedroom UI (tab 0); GUIX is being driven and GT911
 *       taps switch tabs (touch bring-up is non-fatal, so the UI shows either way).
 * @post On any HAL / GUIX init failure the function halts in the red-LED panic loop.
 *
 * @since 0.1.0
 */
int32_t main(void)
{
#ifndef RA_SIMULATOR_MODE
  bedroom_bringup_clocks();
  bedroom_bringup_panel();
  bedroom_bringup_guix();
  bedroom_bringup_touch();
  bedroom_run_forever();
#endif

  /* Belt + suspenders -- the drive loop above never returns. */
  while (1) {
    __asm__ volatile("wfi");
  }
}

#pragma GCC diagnostic pop
