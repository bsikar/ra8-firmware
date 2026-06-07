/**
 * @file examples/ek_ra8d2/hw_validated/manual/bedroom_ui_panel/main.c
 * @brief Bare-metal (no ThreadX) GUIX render of the shared bedroom_ui on the
 *        EK-RA8D2 1024x600 panel via GLCDC
 *
 * @par Tag
 * [Ring 5 / APP] {World: NS}
 *
 * @details
 * Completes "one codebase, two targets": the SAME GUIX source that the macOS
 * host preview drives (``examples/host/guix_tabs`` over
 * ``examples/shared/bedroom_ui``) is compiled here for ARM and drawn on the
 * real GLCDC panel. The UI -- a 3-tab smart-room surface -- is byte-for-byte
 * the host's ``bedroom_ui.c``; only the backend (GLCDC vs the macOS window)
 * and the RTOS bind differ.
 *
 * This app is single-threaded GUIX with NO ThreadX. It uses GUIX's generic
 * RTOS surface (``GX_DISABLE_THREADX_BINDING``) bound to a bare-metal shim
 * (``port/guix/gx_generic_rtos_bare.c``) -- the target sibling of the host's
 * ``gx_generic_rtos_host.c``, identical except its time source is the
 * SysTick-backed ``ra_time_ms()`` instead of POSIX ``clock_gettime``. The
 * GUIX init + drive sequence is the one the host preview proves:
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
 * ``display_init``; the GUIX bring-up mirrors ``guix_tabs``. On any init
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
 * Mirrors the host preview's ``guix_build`` (``examples/host/guix_tabs``) but
 * cross-compiled: ask the display PAL for the GLCDC 565RGB setup-fn through
 * the backend-agnostic ``display_bind_guix`` seam (which also binds
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

/**
 * @brief Drive GUIX forever: dispatch events, then refresh the canvas.
 *
 * @details
 * The single-threaded drive the host preview proves: ``gx_host_pump`` drains
 * and dispatches the generic event ring (any future touch / pen events land
 * here), then ``gx_system_canvas_refresh`` repaints dirty widgets into
 * ``s_framebuffer``, which the GLCDC is already scanning out -- so the panel
 * shows the result on the next frame. A short ``ra_delay_ms`` paces the loop
 * (~60 Hz) and keeps SysTick advancing ``ra_time_ms`` for the GUIX timer.
 *
 * On the board emulator this reaches its first full paint within the first
 * few chunks; the emulator then runs the loop until its wall-clock budget and
 * snapshots the framebuffer.
 *
 * @pre ::bedroom_bringup_guix has run.
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
 * @brief Application entry: bring up clocks/SDRAM/GLCDC + GUIX, then loop.
 *
 * @return Never returns (the drive loop runs forever); 0 only to satisfy the
 *         signature.
 *
 * @pre Reset_Handler has copied .data + zeroed .bss; SystemInit has set VTOR,
 *      FPU, caches and priority grouping.
 * @post The panel shows the bedroom UI (tab 0); GUIX is being driven.
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
  bedroom_run_forever();
#endif

  /* Belt + suspenders -- the drive loop above never returns. */
  while (1) {
    __asm__ volatile("wfi");
  }
}

#pragma GCC diagnostic pop
