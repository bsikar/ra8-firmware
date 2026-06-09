/**
 * @file examples/ek_ra8d2/hw_validated/manual/threadx_guix_demo/main.c
 * @brief Eclipse ThreadX + GUIX hello-world demo on the EK-RA8D2
 *
 * @deprecated GUIX retirement -- tracked by issue #81 (replaced by the
 * ra_reflow chrome renderer, issue #80); scheduled for wholesale removal.
 *
 * @par Tag
 * [Ring 6 / APP] {World: S} -- application-layer code that runs in the
 * Secure world.
 *
 * @details
 * Wires up the project's ra_glcdc framebuffer to the vendored Eclipse
 * GUIX UI framework via the `port/guix/` shim, then stands up two
 * ThreadX threads:
 *
 *   - `gui_thread`: calls `gx_system_initialize`, creates a single
 *     `GX_DISPLAY` + `GX_CANVAS`, mounts a window with a label
 *     "Hello from GUIX on RA8D2" and an animated accent rectangle
 *     standing in for a button widget, then enters
 *     `gx_system_start` -- GUIX's event loop runs forever inside it.
 *   - `app_thread`: every 750 ms posts a virtual "tap" by toggling
 *     the demo's `s_label_color_idx` between two predefined GUIX
 *     colours and triggers a canvas refresh through
 *     `gx_system_canvas_refresh`. EK-RA8D2 SW1 wiring is not yet
 *     committed to `docs/reference/`, so the demo animates
 *     automatically rather than reading a real GPIO.
 *
 * This is a GUIX-NATIVE demo: GUIX owns the entire canvas and
 * paints everything on it -- text, progress bars, animations.
 * The application code never writes pixels to the framebuffer
 * directly.  Compare against `lcd_draw_x` / `lcd_color_cycle`
 * which are HAL-only demos that paint pixels by hand; this one
 * is the "use the framework" reference.
 *
 *   - GR1 framebuffer  -- 480 x 270 RGB565 in SRAM.  Lives at the
 *                         panel's top-left back-porch corner.  All
 *                         pixels in this region come from GUIX
 *                         draw calls.
 *   - GR2              -- unused.  No hand-painted overlay layer.
 *   - BG plane         -- solid colour around the GR1 region.
 *
 * Widget tree:
 *   root window
 *     main_window
 *       title prompt          -- "GUIX Native Demo" in ArnoPro 18 pt
 *       4 font sample prompts -- mono / 4bpp / 8bpp / ArnoPro
 *       linear progress bar   -- animated 0->100% by app_thread
 *       radial progress bar   -- animated 0->100% by app_thread
 *
 * The animation thread bumps each progress widget's
 * `current_val` every 50 ms and calls
 * `gx_system_canvas_refresh()` so GUIX redraws.  No
 * framebuffer-by-hand bookkeeping.
 *
 * On real silicon a full 1024 x 600 RGB565 framebuffer is 1.2 MiB
 * and would need SDRAM; once SDRAMC is up, this same widget tree
 * scales to the full panel by changing two enums.
 *
 * @author Brighton Sikarskie
 * @date 2026-04-29
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra_board_ek_ra8d2.h"
#include "ra_cgc.h"
#include "ra_display_pal.h"
#include "ra_display_pal_guix.h"
#include "ra_display_pal_lcd.h"
#include "ra_err.h"
#include "ra_mstp.h"
#include "ra_panel_timing.h"
#include "ra_port_constants.h"
#include "ra_port_utils.h"
#include "ra_time.h"

/*
 * Cross-build-only includes: the host unit-test harness does not link
 * the ThreadX or GUIX vendor trees, so guard those headers in the same
 * way the other ThreadX-aware examples do.
 */
#ifndef RA_SIMULATOR_MODE
#include "gx_api.h"
#include "gx_display.h"
#include "tx_api.h"
#endif

/* =============================================================================
 * Compile-time configuration -- typed enums per the no-magic-number rule.
 * =============================================================================
 */

/**
 * @brief Demo framebuffer dimensions.
 *
 * @details
 * 256 x 128 ARGB8888 = 128 KiB.  ARGB (not RGB565) so that pixels
 * GUIX does NOT paint stay at alpha=0 and let the BG plane show
 * through -- this is what lets the BG-colour cycle still be
 * visible while the GUIX widgets float on top.  The native panel
 * is 1024 x 600; this buffer maps to the top-left corner.
 */
/** @brief Vertical layout (pixels) for the demo's prompt rows and bars. */
typedef enum : uint16_t {
  k_demo_row_title_bottom     = 36U,
  k_demo_row_mono_top         = 46U,
  k_demo_row_mono_bottom      = 74U,
  k_demo_row_4bpp_top         = 78U,
  k_demo_row_4bpp_bottom      = 106U,
  k_demo_row_8bpp_top         = 110U,
  k_demo_row_8bpp_bottom      = 138U,
  k_demo_row_arnopro_top      = 142U,
  k_demo_row_arnopro_bottom   = 170U,
  k_demo_prompt_left          = 12U,
  k_demo_prompt_right_inset   = 13U,
  k_demo_progress_left        = 20U,
  k_demo_progress_top         = 184U,
  k_demo_progress_right_inset = 21U,
  k_demo_progress_bottom      = 210U,
  k_demo_progress_max_val     = 100U,
  k_demo_radial_ycenter       = 234U,
  k_demo_radial_radius        = 26U,
  k_demo_radial_anchor_deg    = 270U, /**< 12 o'clock. */
} demo_layout_t;

/** @brief 8-bit colour-channel intensities used by the palette. */
typedef enum : uint8_t {
  k_chan_0   = 0x00U, /**< 0%   */
  k_chan_25  = 0x40U, /**< ~25% */
  k_chan_38  = 0x60U, /**< ~38% */
  k_chan_50  = 0x80U, /**< ~50% */
  k_chan_75  = 0xC0U, /**< ~75% */
  k_chan_100 = 0xFFU, /**< 100% (also full alpha). */
} demo_chan_t;

/** @brief GUI thread pacing. */
typedef enum : uint16_t {
  k_demo_powerup_ms = 500U, /**< PLL / panel-POR settle. */
} demo_pace_t;

typedef enum : uint16_t {
  k_demo_fb_width  = 480U, /**< Framebuffer width  (pixels). */
  k_demo_fb_height = 270U, /**< Framebuffer height (pixels). */
} demo_fb_dim_t;

/**
 * @brief Bytes per pixel for ARGB8888 (used in stride / canvas-size math).
 */
typedef enum : uint8_t {
  k_demo_bpp_rgb565 = 2U, /**< 2 bytes per RGB565 pixel. */
} demo_bpp_t;

/**
 * @brief Framebuffer alignment for AXI-burst friendliness.
 */
enum : uint8_t {
  k_demo_fb_align = 64U, /**< 64-byte AXI burst alignment. */
};

/**
 * @brief Stack size shared by both ThreadX threads (bytes).
 *
 * @details
 * 4 KiB per thread; the GUIX thread is the heavier of the two because
 * the framework keeps draw context, event queue, and the active
 * canvas's clip stack on the stack.
 */
typedef enum : uint16_t {
  k_demo_thread_stack = 4096U,
} demo_stack_t;

/**
 * @brief Thread priorities. Lower number = higher priority.
 */
typedef enum : uint8_t {
  k_demo_priority_gui = 5U, /**< GUIX runs slightly above the app. */
  k_demo_priority_app = 6U, /**< App thread does the colour cycle. */
} demo_priority_t;

/**
 * @brief Sleep period for the colour-cycle thread (ThreadX ticks).
 *
 * @details
 * The ThreadX tick is 1 ms (set in `port/threadx/tx_user.h`), so
 * 750 ticks == 0.75 s.
 */
typedef enum : uint16_t {
  k_demo_app_period_ticks = 750U,
} demo_period_t;

/**
 * @brief Memory pool size for GUIX's internal allocator.
 *
 * @details
 * GUIX wraps the `_gx_system_memory_allocator` callback over an app-
 * supplied pool. 16 KiB is the upstream guidance for "single screen,
 * a handful of widgets" demos.
 */
typedef enum : uint32_t {
  k_demo_gx_pool_bytes = 16384U,
} demo_pool_t;

/**
 * @brief Font resource IDs registered with `gx_display_font_table_set`.
 *
 * @details Mirrors the order GUIX expects in the font table -- the
 * value is what the widget's `gx_prompt_font_set(id)` looks up.
 */
typedef enum : uint8_t {
  /* Font IDs start at 1 -- some GUIX widget paths treat ID 0 as
   * "no font / use default" and skip rendering entirely. */
  k_demo_font_mono       = 1U, /**< `_gx_system_font_mono`, 1 bpp.        */
  k_demo_font_4bpp       = 2U, /**< `_gx_system_font_4bpp`, 4 bpp AA.     */
  k_demo_font_8bpp       = 3U, /**< `_gx_system_font_8bpp`, 8 bpp AA.     */
  k_demo_font_arnopro    = 4U, /**< `g_font_arnopro_18`, 4 bpp AA, 18 pt. */
  k_demo_font_count      = 4U,
  k_demo_font_table_size = 5U, /**< Slot 0 unused + 4 real fonts.         */
} demo_font_id_t;

/* =============================================================================
 * Cross-build state. Only present on the cross compile -- the host
 * unit-test target skips everything below.
 * =============================================================================
 */

#ifndef RA_SIMULATOR_MODE

/**
 * @var s_framebuffer
 * @brief RGB565 framebuffer scanned out by GLCDC and written by GUIX.
 *
 * @details
 * Lives in `.bss` (zero-init by Reset_Handler). 64-byte aligned so
 * the GLCDC AXI burst never straddles a cache-line boundary.
 */
static uint16_t s_framebuffer[k_demo_fb_height][k_demo_fb_width]
  __attribute__((aligned(k_demo_fb_align)));

/**
 * @brief Display PAL config selecting the LCD backend.
 *
 * @details
 * Bound at startup by ``demo_lcd_panel_bringup``.  To target the
 * IT8951-compatible e-ink panel in the future, change the
 * ``iface`` line to ``&k_display_backend_eink_it8951`` -- the rest
 * of this demo (GUIX widget tree, animation, threads) stays
 * untouched.
 */
static const display_cfg_t k_demo_display_cfg = {
  .iface             = &k_display_backend_lcd_ra_glcdc,
  .framebuffer       = &s_framebuffer[0][0],
  .framebuffer_bytes = sizeof(s_framebuffer),
  .width_px          = (uint16_t)k_demo_fb_width,
  .height_px         = (uint16_t)k_demo_fb_height,
  .pixfmt            = k_display_pixfmt_rgb565,
  .panel_timing      = &k_ra_panel_ek_ra8d2_timing,
};

/** @brief PAL handle returned by ``display_init``. */
static display_handle_t* s_display_pal = nullptr;

/**
 * @var s_gx_pool
 * @brief Backing memory for `gx_system_memory_allocator`.
 */
static uint8_t s_gx_pool[k_demo_gx_pool_bytes] __attribute__((aligned(8)));

/* GUIX control blocks. */
static GX_DISPLAY     s_display;
static GX_WINDOW_ROOT s_root;
static GX_WINDOW      s_main_window;
static GX_CANVAS      s_canvas;

/* One prompt per bundled GUIX system font.  The screen shows the
 * same sample string rendered in each font so the relative size
 * / weight is easy to eyeball.
 *
 * `s_prompt_dummy` is a known-broken workaround.  On this
 * GUIX 6.x / 565RGB / EK-RA8D2 configuration, the FIRST prompt
 * attached to a parent window paints its background but never
 * renders its text.  Every other prompt attached to the same
 * parent renders fine.  We could not reproduce the bug on the
 * GUIX win32 simulation port, and reading
 * `_gx_canvas_drawing_initiate` + `_gx_widget_children_draw` did
 * not surface an obvious cause -- the view-list for the first
 * child appears to have the correct dirty rectangle but the
 * resulting paint just leaves the text region empty.  Until the
 * vendor path is debuggable with a real breakpoint, attaching a
 * zero-size dummy prompt FIRST absorbs the bug so the three real
 * font prompts render. */
static GX_PROMPT s_prompt_dummy;
static GX_PROMPT s_prompt_title;
static GX_PROMPT s_prompt_mono;
static GX_PROMPT s_prompt_4bpp;
static GX_PROMPT s_prompt_8bpp;
static GX_PROMPT s_prompt_arnopro;

/* Animated progress widgets.  The app_thread ticks both of these
 * every 50 ms; GUIX repaints automatically through the dirty-rect
 * mechanism after `gx_progress_bar_info_set` /
 * `gx_radial_progress_bar_info_set` are called. */
static GX_PROGRESS_BAR             s_progress_bar;
static GX_PROGRESS_BAR_INFO        s_progress_bar_info;
static GX_RADIAL_PROGRESS_BAR      s_radial_bar;
static GX_RADIAL_PROGRESS_BAR_INFO s_radial_bar_info;

static const GX_CHAR s_text_title[]   = "GUIX Native Demo";
static const GX_CHAR s_text_mono[]    = "Mono: AaBb 0123";
static const GX_CHAR s_text_4bpp[]    = "4bpp: AaBb 0123";
static const GX_CHAR s_text_8bpp[]    = "8bpp: AaBb 0123";
static const GX_CHAR s_text_arnopro[] = "ArnoPro 18: AaBb";

/* ThreadX control blocks + stacks. */
static TX_THREAD s_gui_thread;
static TX_THREAD s_app_thread;
static uint8_t   s_gui_stack[k_demo_thread_stack] __attribute__((aligned(8)));
static uint8_t   s_app_stack[k_demo_thread_stack] __attribute__((aligned(8)));

/* Colour-cycle state was used by the (now removed) animated app
 * thread.  Kept as a placeholder so future demos can re-add a
 * volatile state byte without rewiring the widget tree. */

/* =============================================================================
 * SysTick override -- routes the 1 ms tick into the ThreadX scheduler
 * (same pattern as examples/threadx_blink/main.c).
 * =============================================================================
 */

/* NOLINTBEGIN(bugprone-reserved-identifier,cert-dcl37-c,readability-identifier-naming) -- ThreadX-supplied symbol. */
extern void _tx_timer_interrupt(void);
/* NOLINTEND(bugprone-reserved-identifier,cert-dcl37-c,readability-identifier-naming) */

/**
 * @brief SysTick exception handler routed to ThreadX and the HAL time base.
 *
 * @details
 * Each 1 ms SysTick fires `_tx_timer_interrupt`, which advances the
 * kernel tick counter and pre-empts the running thread when a higher-
 * priority thread becomes ready.  It ALSO advances the `ra_time`
 * millisecond counter via `ra_time_on_tick`, so `ra_delay_ms` -- used
 * inside HAL bring-up paths such as `ra_board_lcd_panel_power_on`
 * (panel reset/back-light pulses) reached through `display_init` --
 * makes progress under ThreadX instead of busy-waiting forever on a
 * `s_tick_ms` that nothing else moves.
 *
 * @pre Called from exception context (IPSR == 15).
 * @pre `_tx_initialize_low_level` has programmed SysTick.
 * @post One ThreadX tick has been credited.
 * @post The `ra_time` millisecond counter has advanced by one.
 *
 * @note Both callees are interrupt-safe and need no prior init beyond
 *       SysTick being armed.
 * @since 0.1.0
 */
void SysTick_Handler(void);
void SysTick_Handler(void)
{
  _tx_timer_interrupt();
  ra_time_on_tick();
}

/* =============================================================================
 * GUIX memory allocator wrappers.
 * =============================================================================
 */

/**
 * @brief Local malloc-on-pool implementation for GUIX.
 *
 * @details
 * GUIX wants two function pointers (alloc + free). Per NASA Power-of-
 * 10 Rule 3 we forbid runtime malloc, so the "allocator" is a simple
 * bump pointer over `s_gx_pool`. `free` is a no-op; GUIX never
 * actually releases memory in steady-state for the demo's widget set.
 *
 * @param[in] size Bytes requested.
 * @return Aligned pointer into `s_gx_pool`, or NULL on exhaustion.
 *
 * @pre `size` is non-zero.
 * @post Returned pointer (when non-NULL) is 8-byte aligned.
 *
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_demo_pool_align = 8U,
} demo_pool_align_t;

static VOID* demo_gx_alloc(ULONG size)
{
  static ULONG s_used = 0U;
  /* Round up to alignment. */
  const ULONG aligned = (size + (ULONG)(k_demo_pool_align - 1U)) & ~(ULONG)(k_demo_pool_align - 1U);

  if ((s_used + aligned) > (ULONG)k_demo_gx_pool_bytes) {
    return GX_NULL;
  }
  VOID* ptr = &s_gx_pool[s_used];
  s_used += aligned;
  return ptr;
}

/**
 * @brief No-op free callback paired with `demo_gx_alloc`.
 *
 * @param[in] mem Block to free (ignored).
 *
 * @since 0.1.0
 */
static VOID demo_gx_free(VOID* mem)
{
  (void)mem;
}

/* =============================================================================
 * GUIX widget bring-up.
 * =============================================================================
 */

/**
 * @brief Pack RGB components into a GUIX `GX_COLOR` word (opaque).
 *
 * @details
 * `GX_COLOR` is laid out as `0xAARRGGBB`; this demo only paints
 * fully-opaque widgets and the underlying framebuffer is RGB565
 * (no alpha channel), so the alpha byte is hard-coded to `0xFF`
 * and the caller only supplies the three colour channels.
 *
 * @param[in] r Red channel 0..255.
 * @param[in] g Green channel 0..255.
 * @param[in] b Blue channel 0..255.
 * @return Packed `GX_COLOR` with alpha=0xFF.
 *
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_demo_color_shift_a = 24U,
  k_demo_color_shift_r = 16U,
  k_demo_color_shift_g = 8U,
} demo_color_shift_t;

static inline GX_COLOR demo_rgb(uint8_t r, uint8_t g, uint8_t b)
{
  return (GX_COLOR)(((uint32_t)k_chan_100 << k_demo_color_shift_a) |
                    ((uint32_t)r << k_demo_color_shift_r) | ((uint32_t)g << k_demo_color_shift_g) |
                    (uint32_t)b);
}

/**
 * @brief Build a GX_RECTANGLE inline.
 *
 * @param[in] left   Inclusive left edge.
 * @param[in] top    Inclusive top edge.
 * @param[in] right  Inclusive right edge.
 * @param[in] bottom Inclusive bottom edge.
 * @return Initialized rectangle.
 *
 * @since 0.1.0
 */
static inline GX_RECTANGLE demo_rect(GX_VALUE left, GX_VALUE top, GX_VALUE right, GX_VALUE bottom)
{
  GX_RECTANGLE r;
  r.gx_rectangle_left   = left;
  r.gx_rectangle_top    = top;
  r.gx_rectangle_right  = right;
  r.gx_rectangle_bottom = bottom;
  return r;
}

/**
 * @brief Halt forever -- panic stop on init failure.
 *
 * @pre Called only after a fatal error in boot.
 * @post CPU is parked; only a debugger or external reset wakes it.
 *
 * @since 0.1.0
 */
static void demo_panic_halt(void)
{
  (void)ra_board_led_init(k_ra_board_led3); /* red */
  (void)ra_board_led_on(k_ra_board_led3);
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Bring up the LCD panel and GLCDC pipeline for GR1.
 *
 * @details Powers the panel, programmes the GLCDC for the demo
 * framebuffer dimensions (480 x 270 RGB565), sets the BG plane
 * black, starts scan-out, and makes graphics layer 1 visible
 * with `s_framebuffer` as its source.  Panics on any HAL error.
 *
 * @pre Called from the GUI thread (uses `tx_thread_sleep`).
 * @pre `s_framebuffer` is zero-initialized in `.bss`.
 * @post GLCDC is scanning out; GR1 is reading `s_framebuffer`.
 * @post BG plane = black.
 * @note Single-threaded; not safe to call from IRQ.
 * @since 0.1.0
 */
static void demo_lcd_panel_bringup(void)
{
  (void)tx_thread_sleep(k_demo_powerup_ms); /* PLL / panel-POR settle */

  /* Display PAL: bound to the LCD backend by ``k_demo_display_cfg``.
   * Folds the panel power-on (``ra_board_lcd_panel_power_on`` --
   * RESET_L / BLEN strap pulses) + ``ra_board_glcdc_init`` +
   * ``ra_glcdc_init`` + ``set_background_color`` + ``start(true)`` +
   * ``layer1_show`` into a single call.  The panel power-on uses
   * ``ra_delay_ms`` for its reset pulses, which advances here because
   * ``SysTick_Handler`` also drives ``ra_time_on_tick`` -- so the demo
   * no longer has to (and must not, or it double-claims RESET_L) inline
   * the GPIO strap sequence by hand.  Switching to e-ink later is a
   * one-line change in ``k_demo_display_cfg``. */
  if (display_init(&k_demo_display_cfg, &s_display_pal) != k_ra_ok) {
    demo_panic_halt();
  }
}

/**
 * @brief Bind the framebuffer, install the allocator, and bring
 *        up the GUIX system + display objects.
 *
 * @pre `demo_lcd_panel_bringup` has run.
 * @post `s_display` is created and the GUIX system thread is alive.
 * @note Single-threaded; not safe to call from IRQ.
 * @since 0.1.0
 */
static void demo_guix_system_setup(void)
{
  /* Ask the display PAL for the GUIX setup-fn + dimensions.  The PAL
   * routes through the bound backend; the LCD backend forwards to
   * ``port/guix/`` so this code path stays backend-agnostic. */
  display_guix_attach_t attach = {nullptr, 0U, 0U};
  if (display_bind_guix(s_display_pal, &attach) != k_ra_ok) {
    demo_panic_halt();
  }
  if (gx_system_memory_allocator_set(demo_gx_alloc, demo_gx_free) != GX_SUCCESS) {
    demo_panic_halt();
  }
  if (gx_system_initialize() != GX_SUCCESS) {
    demo_panic_halt();
  }
  /* Cast the PAL's opaque setup-fn back to GUIX's typed signature
   * at the call site, the same way ``ra_display_pal_guix.h`` shows
   * in its example. */
  if (gx_display_create(&s_display,
                        "ra8d2-glcdc",
                        (UINT (*)(GX_DISPLAY*))attach.setup_fn,
                        (GX_VALUE)attach.width_px,
                        (GX_VALUE)attach.height_px) != GX_SUCCESS) {
    demo_panic_halt();
  }
}

/**
 * @brief Populate the GUIX colour-resource table.
 *
 * @details `GX_COLOR_ID_*` values are indices into the table
 * supplied to `gx_display_color_table_set`.  Unset slots default
 * to fully transparent so widgets that pick up a stray
 * unconfigured ID do not paint stale colours.  The five named
 * slots cover everything the demo's widgets render with.
 *
 * @pre `demo_guix_system_setup` has run.
 * @post Display has a 32-entry colour resource table installed.
 * @since 0.1.0
 */
static void demo_guix_install_palette(void)
{
  static GX_COLOR s_color_table[32];
  for (uint32_t i = 0U; i < 32U; i++) {
    s_color_table[i] = 0x00000000UL;
  }
  s_color_table[GX_COLOR_ID_CANVAS] = demo_rgb(k_chan_0, k_chan_0, k_chan_38); /* dark navy */
  s_color_table[GX_COLOR_ID_WINDOW_FILL] =
    demo_rgb(k_chan_0, k_chan_50, k_chan_100); /* bright cyan */
  s_color_table[GX_COLOR_ID_TEXT]      = demo_rgb(k_chan_100, k_chan_100, k_chan_0); /* yellow */
  s_color_table[GX_COLOR_ID_BTN_UPPER] = demo_rgb(k_chan_100, k_chan_25, k_chan_0);  /* orange */
  s_color_table[GX_COLOR_ID_BTN_LOWER] = demo_rgb(k_chan_75, k_chan_0, k_chan_0);    /* dark red */
  if (gx_display_color_table_set(&s_display, s_color_table, 32) != GX_SUCCESS) {
    demo_panic_halt();
  }
}

/**
 * @brief Install the font resource table.
 *
 * @details Slot 0 is intentionally NULL because several GUIX
 * widget paths treat `font_id = 0` as "use the system default"
 * and silently skip rendering when no default is installed.
 * Slots 1..4 hold the three GUIX-bundled bitmap fonts plus the
 * in-tree ArnoPro font generated by `scripts/utils/font_to_guix.py`.
 *
 * @pre `demo_guix_system_setup` has run.
 * @post Display has a font resource table with slots 1..4 populated.
 * @since 0.1.0
 */
static void demo_guix_install_fonts(void)
{
  /* NOLINTBEGIN(bugprone-reserved-identifier) -- vendor symbols. */
  extern GX_CONST GX_FONT _gx_system_font_mono;
  extern GX_CONST GX_FONT _gx_system_font_4bpp;
  extern GX_CONST GX_FONT _gx_system_font_8bpp;
  /* NOLINTEND(bugprone-reserved-identifier) */
  extern GX_CONST GX_FONT g_font_arnopro_18;
  static GX_FONT*         s_font_table[k_demo_font_table_size];
  s_font_table[0]                   = GX_NULL;
  s_font_table[k_demo_font_mono]    = (GX_FONT*)&_gx_system_font_mono;
  s_font_table[k_demo_font_4bpp]    = (GX_FONT*)&_gx_system_font_4bpp;
  s_font_table[k_demo_font_8bpp]    = (GX_FONT*)&_gx_system_font_8bpp;
  s_font_table[k_demo_font_arnopro] = (GX_FONT*)&g_font_arnopro_18;
  if (gx_display_font_table_set(&s_display, s_font_table, (UINT)k_demo_font_table_size) !=
      GX_SUCCESS) {
    demo_panic_halt();
  }
}

/**
 * @brief Build the canvas + root window + main window scaffold.
 *
 * @pre `demo_guix_install_palette` and `demo_guix_install_fonts`
 *      have both run.
 * @post `s_canvas`, `s_root`, and `s_main_window` exist; the
 *       main window is attached as a child of the root.
 * @since 0.1.0
 */
static void demo_guix_canvas_setup(void)
{
  if (gx_canvas_create(&s_canvas,
                       "ra8d2-canvas",
                       &s_display,
                       GX_CANVAS_MANAGED_VISIBLE,
                       (UINT)k_demo_fb_width,
                       (UINT)k_demo_fb_height,
                       (GX_COLOR*)&s_framebuffer[0][0],
                       (ULONG)k_demo_fb_width * (ULONG)k_demo_fb_height *
                         (ULONG)k_demo_bpp_rgb565) != GX_SUCCESS) {
    demo_panic_halt();
  }
  /* GUIX canvases come up with GX_CANVAS_VISIBLE clear; without
   * this call the system thread's first refresh decides there is
   * nothing to draw. */
  if (gx_canvas_show(&s_canvas) != GX_SUCCESS) {
    demo_panic_halt();
  }
  const GX_RECTANGLE root_rect =
    demo_rect(0, 0, (GX_VALUE)(k_demo_fb_width - 1U), (GX_VALUE)(k_demo_fb_height - 1U));
  if (gx_window_root_create(&s_root, "ra8d2-root", &s_canvas, GX_STYLE_NONE, 0U, &root_rect) !=
      GX_SUCCESS) {
    demo_panic_halt();
  }
  if (gx_window_create(&s_main_window,
                       "ra8d2-main-window",
                       GX_NULL,
                       GX_STYLE_ENABLED,
                       0U,
                       &root_rect) != GX_SUCCESS) {
    demo_panic_halt();
  }
  if (gx_widget_attach((GX_WIDGET*)&s_root, (GX_WIDGET*)&s_main_window) != GX_SUCCESS) {
    demo_panic_halt();
  }
}

/**
 * @struct demo_prompt_row_t
 * @brief Layout description for one text-prompt row in the canvas.
 *
 * @details Used by `demo_guix_create_prompts` to walk a compile-
 * time table of prompts so the create-loop body stays short.
 */
typedef struct {
  GX_PROMPT*     prompt;   /**< Storage for the widget control block. */
  const char*    name;     /**< Debug name (passed to GUIX).          */
  const GX_CHAR* text;     /**< Text to render.                       */
  UINT           text_len; /**< Length of `text` in bytes.            */
  GX_VALUE       top;      /**< Top edge (px, canvas-relative).       */
  GX_VALUE       bottom;   /**< Bottom edge (px, canvas-relative).    */
  UINT           font_id;  /**< Font resource ID.                     */
} demo_prompt_row_t;

/**
 * @var s_prompts
 * @brief Compile-time table of the prompts the demo renders.
 *
 * @details One row per text widget the demo paints into GR1; the
 * y coordinates pack the rows vertically inside the 270 px-tall
 * canvas with the title row reserved at the top.
 */
static const demo_prompt_row_t s_prompts[5] = {
  {.prompt   = &s_prompt_title,
   .name     = "title",
   .text     = s_text_title,
   .text_len = (UINT)(sizeof(s_text_title) - 1U),
   .top      = 8,
   .bottom   = k_demo_row_title_bottom,
   .font_id  = (UINT)k_demo_font_arnopro},
  {.prompt   = &s_prompt_mono,
   .name     = "font-mono",
   .text     = s_text_mono,
   .text_len = (UINT)(sizeof(s_text_mono) - 1U),
   .top      = k_demo_row_mono_top,
   .bottom   = k_demo_row_mono_bottom,
   .font_id  = (UINT)k_demo_font_mono},
  {.prompt   = &s_prompt_4bpp,
   .name     = "font-4bpp",
   .text     = s_text_4bpp,
   .text_len = (UINT)(sizeof(s_text_4bpp) - 1U),
   .top      = k_demo_row_4bpp_top,
   .bottom   = k_demo_row_4bpp_bottom,
   .font_id  = (UINT)k_demo_font_4bpp},
  {.prompt   = &s_prompt_8bpp,
   .name     = "font-8bpp",
   .text     = s_text_8bpp,
   .text_len = (UINT)(sizeof(s_text_8bpp) - 1U),
   .top      = k_demo_row_8bpp_top,
   .bottom   = k_demo_row_8bpp_bottom,
   .font_id  = (UINT)k_demo_font_8bpp},
  {.prompt   = &s_prompt_arnopro,
   .name     = "font-arnopro",
   .text     = s_text_arnopro,
   .text_len = (UINT)(sizeof(s_text_arnopro) - 1U),
   .top      = k_demo_row_arnopro_top,
   .bottom   = k_demo_row_arnopro_bottom,
   .font_id  = (UINT)k_demo_font_arnopro},
};

static void demo_guix_create_prompts(void)
{
  const GX_RECTANGLE dummy_rect = demo_rect(0, 0, 0, 0);
  if (gx_prompt_create(&s_prompt_dummy,
                       "dummy",
                       &s_main_window,
                       0,
                       GX_STYLE_ENABLED,
                       0xFFFFU,
                       &dummy_rect) != GX_SUCCESS) {
    demo_panic_halt();
  }
  for (uint32_t i = 0U; i < (sizeof(s_prompts) / sizeof(s_prompts[0])); i++) {
    const GX_RECTANGLE r = demo_rect(k_demo_prompt_left,
                                     s_prompts[i].top,
                                     (GX_VALUE)(k_demo_fb_width - k_demo_prompt_right_inset),
                                     s_prompts[i].bottom);
    if (gx_prompt_create(s_prompts[i].prompt,
                         s_prompts[i].name,
                         &s_main_window,
                         0,
                         GX_STYLE_TEXT_LEFT | GX_STYLE_ENABLED,
                         (USHORT)i,
                         &r) != GX_SUCCESS) {
      demo_panic_halt();
    }
    GX_STRING s;
    s.gx_string_ptr    = s_prompts[i].text;
    s.gx_string_length = s_prompts[i].text_len;
    if (gx_prompt_text_set_ext(s_prompts[i].prompt, &s) != GX_SUCCESS) {
      demo_panic_halt();
    }
    if (gx_prompt_font_set(s_prompts[i].prompt, (GX_RESOURCE_ID)s_prompts[i].font_id) !=
        GX_SUCCESS) {
      demo_panic_halt();
    }
  }
}

/**
 * @brief Create the linear + radial progress-bar widgets.
 *
 * @details Both progress bars start at value 0; `app_thread`
 * advances them on a timer and calls the matching `*_info_set`
 * helper, which marks the widget dirty so GUIX repaints at the
 * next event-loop pass.
 *
 * @pre `demo_guix_canvas_setup` has run.
 * @post `s_progress_bar` and `s_radial_bar` are attached to
 *       `s_main_window`.
 * @since 0.1.0
 */
static void demo_guix_create_progress_widgets(void)
{
  s_progress_bar_info.gx_progress_bar_info_min_val        = 0;
  s_progress_bar_info.gx_progress_bar_info_max_val        = k_demo_progress_max_val;
  s_progress_bar_info.gx_progress_bar_info_current_val    = 0;
  s_progress_bar_info.gx_progress_bar_font_id             = (GX_RESOURCE_ID)k_demo_font_4bpp;
  s_progress_bar_info.gx_progress_bar_normal_text_color   = GX_COLOR_ID_TEXT;
  s_progress_bar_info.gx_progress_bar_selected_text_color = GX_COLOR_ID_TEXT;
  s_progress_bar_info.gx_progress_bar_disabled_text_color = GX_COLOR_ID_TEXT;
  s_progress_bar_info.gx_progress_bar_fill_pixelmap       = 0;
  const GX_RECTANGLE progress_rect =
    demo_rect(k_demo_progress_left,
              k_demo_progress_top,
              (GX_VALUE)(k_demo_fb_width - k_demo_progress_right_inset),
              k_demo_progress_bottom);
  if (gx_progress_bar_create(&s_progress_bar,
                             "progress",
                             &s_main_window,
                             &s_progress_bar_info,
                             GX_STYLE_ENABLED | GX_STYLE_PROGRESS_PERCENT,
                             5U,
                             &progress_rect) != GX_SUCCESS) {
    demo_panic_halt();
  }

  s_radial_bar_info.gx_radial_progress_bar_info_xcenter     = (GX_VALUE)(k_demo_fb_width / 2U);
  s_radial_bar_info.gx_radial_progress_bar_info_ycenter     = k_demo_radial_ycenter;
  s_radial_bar_info.gx_radial_progress_bar_info_radius      = k_demo_radial_radius;
  s_radial_bar_info.gx_radial_progress_bar_info_current_val = 0;
  s_radial_bar_info.gx_radial_progress_bar_info_anchor_val =
    k_demo_radial_anchor_deg; /* 12 o'clock */
  s_radial_bar_info.gx_radial_progress_bar_info_font_id = (GX_RESOURCE_ID)k_demo_font_4bpp;
  s_radial_bar_info.gx_radial_progress_bar_info_normal_text_color    = GX_COLOR_ID_TEXT;
  s_radial_bar_info.gx_radial_progress_bar_info_selected_text_color  = GX_COLOR_ID_TEXT;
  s_radial_bar_info.gx_radial_progress_bar_info_disabled_text_color  = GX_COLOR_ID_TEXT;
  s_radial_bar_info.gx_radial_progress_bar_info_normal_brush_width   = 6;
  s_radial_bar_info.gx_radial_progress_bar_info_selected_brush_width = 6;
  s_radial_bar_info.gx_radial_progress_bar_info_normal_brush_color   = GX_COLOR_ID_CANVAS;
  s_radial_bar_info.gx_radial_progress_bar_info_selected_brush_color = GX_COLOR_ID_TEXT;
  if (gx_radial_progress_bar_create(&s_radial_bar,
                                    "radial",
                                    (GX_WIDGET*)&s_main_window,
                                    &s_radial_bar_info,
                                    GX_STYLE_ENABLED,
                                    6U) != GX_SUCCESS) {
    demo_panic_halt();
  }
}

/**
 * @brief Top-level GUI bring-up: chain every setup helper, then
 *        enter the GUIX event loop.
 *
 * @details Each helper panics on failure, so the orchestrator
 * reads top-to-bottom.  Returns only on a GUIX internal scheduler
 * error (which also halts).
 *
 * @pre Called from `gui_thread_entry`.
 * @pre ThreadX scheduler is up; SysTick is routing to `_tx_timer_interrupt`.
 * @post GUIX has finished its first paint; `gx_system_start`
 *       has taken over the calling thread.
 * @since 0.1.0
 */
static void demo_gui_setup_and_run(void)
{
  demo_lcd_panel_bringup();
  demo_guix_system_setup();
  demo_guix_install_palette();
  demo_guix_install_fonts();
  demo_guix_canvas_setup();
  demo_guix_create_prompts();
  demo_guix_create_progress_widgets();

  if (gx_widget_show((GX_WIDGET*)&s_root) != GX_SUCCESS) {
    demo_panic_halt();
  }
  /* Force the first frame to land before gx_system_start blocks. */
  (void)gx_system_canvas_refresh();
  if (gx_system_start() != GX_SUCCESS) {
    demo_panic_halt();
  }
}

/**
 * @brief GUIX worker thread entry point.
 *
 * @param[in] thread_input Unused (ThreadX cookie).
 *
 * @pre `tx_application_define` has created the thread with auto-start.
 * @post Calls `demo_gui_setup_and_run`, which never returns.
 *
 * @since 0.1.0
 */
static void gui_thread_entry(ULONG thread_input)
{
  (void)thread_input;
  demo_gui_setup_and_run();
}

/**
 * @brief Application thread: animates the GUIX progress widgets.
 *
 * @details Every 50 ms, advances the linear and radial progress
 * bars' `current_val` by one step (wrapping back to 0 after 100),
 * then calls `gx_progress_bar_info_set` / `gx_radial_progress_bar_info_set`
 * to tell GUIX the widgets are dirty.  GUIX's system thread picks
 * up the dirty widgets and repaints them at the next event-loop
 * pass.  No framebuffer-by-hand work involved.
 *
 * This shows the GUIX side of the architecture: the app code just
 * publishes new widget state and the framework handles repaint
 * scheduling, dirty-rect tracking, and pixel composition.
 *
 * @param[in] thread_input Unused (ThreadX cookie).
 *
 * @since 0.1.0
 */
static void app_thread_entry(ULONG thread_input)
{
  (void)thread_input;
  enum : uint32_t {
    k_animation_tick_ms = 50U, /**< 50 ms between steps -> ~2 s sweep. */
    k_animation_step    = 2U,  /**< 0..100 in 50 steps.                */
    k_animation_max     = 100U,
  };
  /* Let GUIX finish its first paint before we start firing updates. */
  (void)tx_thread_sleep(k_demo_powerup_ms);
  uint32_t linear_val = 0U;
  uint32_t radial_val = 0U;
  while (1) {
    (void)tx_thread_sleep(k_animation_tick_ms);

    linear_val = (linear_val + k_animation_step) % (k_animation_max + 1U);
    s_progress_bar_info.gx_progress_bar_info_current_val = (INT)linear_val;
    (void)gx_progress_bar_info_set(&s_progress_bar, &s_progress_bar_info);

    /* Radial advances at a different rate so the two widgets
     * don't move in lock-step.  k_animation_step + 1 = 3 -> ~1.7 s sweep. */
    radial_val = (radial_val + (k_animation_step + 1U)) % (k_animation_max + 1U);
    s_radial_bar_info.gx_radial_progress_bar_info_current_val = (GX_VALUE)radial_val;
    (void)gx_radial_progress_bar_info_set(&s_radial_bar, &s_radial_bar_info);
  }
}

/**
 * @brief ThreadX application initialisation callback.
 *
 * @param[in] first_unused_memory Pointer to the first byte of free
 *                                RAM after the kernel's internal
 *                                allocations. Unused -- both threads
 *                                use static stacks.
 *
 * @pre Called from `_tx_initialize_kernel_enter`. IRQs masked.
 * @pre Both stacks declared at file scope.
 *
 * @post `gui_thread` and `app_thread` are created and auto-started.
 *
 * @note On `tx_thread_create` failure the function busy-loops because
 *       there is no log channel up yet and the kernel is about to
 *       take over.
 *
 * @since 0.1.0
 */
/* NOLINTNEXTLINE(misc-use-internal-linkage) -- exported symbol expected by ThreadX. */
void tx_application_define(void* first_unused_memory)
{
  (void)first_unused_memory;

  UINT err = tx_thread_create(&s_gui_thread,
                              "guix",
                              gui_thread_entry,
                              0U,
                              s_gui_stack,
                              (ULONG)k_demo_thread_stack,
                              (UINT)k_demo_priority_gui,
                              (UINT)k_demo_priority_gui,
                              TX_NO_TIME_SLICE,
                              TX_AUTO_START);
  if (err != TX_SUCCESS) {
    demo_panic_halt();
  }

  err = tx_thread_create(&s_app_thread,
                         "app",
                         app_thread_entry,
                         0U,
                         s_app_stack,
                         (ULONG)k_demo_thread_stack,
                         (UINT)k_demo_priority_app,
                         (UINT)k_demo_priority_app,
                         TX_NO_TIME_SLICE,
                         TX_AUTO_START);
  if (err != TX_SUCCESS) {
    demo_panic_halt();
  }
}

#endif /* !RA_SIMULATOR_MODE */

/* =============================================================================
 * main() -- LED indicator + drop into the ThreadX scheduler.
 * =============================================================================
 */

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"

/**
 * @brief Application entry. Configures the heartbeat LED, then hands
 *        control to the ThreadX scheduler.
 *
 * @return Never returns -- `tx_kernel_enter` does not.
 *
 * @pre Reset_Handler has copied .data + zeroed .bss.
 * @pre SystemInit has set VTOR, FPU, and priority grouping.
 *
 * @post LED1 is configured as an output.
 * @post The two demo threads are running.
 * @post On any HAL-init failure the function halts in `__WFI`.
 *
 * @since 0.1.0
 */
int32_t main(void)
{
#ifndef RA_SIMULATOR_MODE
  /* Minimal pre-kernel init: only clocks, MSTP, and the heartbeat
   * LED.  Everything that wants to sleep on SysTick (panel POR
   * settle, GLCDC bring-up, GUIX setup) runs from the gui_thread
   * after `tx_kernel_enter` has installed the SysTick handler. */
  (void)ra_cgc_init();
  (void)ra_mstp_init();
  (void)ra_board_led_init(k_ra_board_led1);

  tx_kernel_enter();
#endif

  /* Belt + suspenders -- the kernel does not return. */
  while (1) {
    __asm__ volatile("wfi");
  }
}

#pragma GCC diagnostic pop
