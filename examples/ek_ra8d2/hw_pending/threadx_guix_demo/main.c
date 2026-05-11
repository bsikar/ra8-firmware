/**
 * @file examples/ek_ra8d2/threadx_guix_demo/main.c
 * @brief Eclipse ThreadX + GUIX hello-world demo on the EK-RA8D2
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
 * The framebuffer lives in SRAM as a 256 x 128 RGB565 buffer (64 KiB,
 * comfortably inside the 2 MiB SRAM budget). On real silicon the
 * 1024 x 600 native-resolution framebuffer would have to live in
 * SDRAM because RGB565 at that size is 1.2 MiB; that requires SDRAMC
 * bring-up which is out of scope for this UI smoke test.
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
#include "ra_err.h"
#include "ra_glcdc.h"
#include "ra_gpio_constants.h"
#include "ra_isr.h"
#include "ra_mstp.h"
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
#include "gx_display_driver_ra_glcdc.h"
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
 * 256 x 128 RGB565 = 64 KiB. Picked small on purpose so the buffer
 * lands in on-chip SRAM and the bring-up does not need SDRAMC. The
 * native EK-RA8D2 panel is 1024 x 600; this buffer maps to the
 * top-left corner in the GLCDC viewport.
 */
typedef enum : uint16_t {
  k_demo_fb_width  = 256U, /**< Framebuffer width  (pixels). */
  k_demo_fb_height = 128U, /**< Framebuffer height (pixels). */
} demo_fb_dim_t;

/**
 * @brief Bytes per pixel for RGB565 (used in stride math).
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
 * @brief Identifiers for the two label-colour states.
 */
typedef enum : uint8_t {
  k_demo_color_state_blue   = 0U, /**< Default blue label.       */
  k_demo_color_state_orange = 1U, /**< Highlighted orange label. */
} demo_color_state_t;

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
 * @var s_gx_pool
 * @brief Backing memory for `gx_system_memory_allocator`.
 */
static uint8_t s_gx_pool[k_demo_gx_pool_bytes] __attribute__((aligned(8)));

/* GUIX control blocks. */
static GX_DISPLAY     s_display;
static GX_WINDOW_ROOT s_root;
static GX_WINDOW      s_main_window;
static GX_PROMPT      s_label;
static GX_BUTTON      s_button;
static GX_CANVAS      s_canvas;

/* The text the prompt displays. GUIX does not copy the string. */
static const GX_CHAR s_label_text[] = "Hello from GUIX on RA8D2";

/* ThreadX control blocks + stacks. */
static TX_THREAD s_gui_thread;
static TX_THREAD s_app_thread;
static uint8_t   s_gui_stack[k_demo_thread_stack] __attribute__((aligned(8)));
static uint8_t   s_app_stack[k_demo_thread_stack] __attribute__((aligned(8)));

/**
 * @var s_label_color_state
 * @brief Current colour-cycle phase (`demo_color_state_t`).
 *
 * @note Only written by `app_thread_entry`; read by `gui_thread_entry`
 *       through `gx_system_canvas_refresh`. ThreadX makes the read
 *       atomic on Cortex-M85 (32-bit aligned uint8 read is a single
 *       LDRB).
 */
static volatile uint8_t s_label_color_state = (uint8_t)k_demo_color_state_blue;

/* =============================================================================
 * SysTick override -- routes the 1 ms tick into the ThreadX scheduler
 * (same pattern as examples/threadx_blink/main.c).
 * =============================================================================
 */

/* NOLINTBEGIN(bugprone-reserved-identifier,cert-dcl37-c,readability-identifier-naming) -- ThreadX-supplied symbol. */
extern void _tx_timer_interrupt(void);
/* NOLINTEND(bugprone-reserved-identifier,cert-dcl37-c,readability-identifier-naming) */

/**
 * @brief SysTick exception handler routed to ThreadX.
 *
 * @details
 * Each 1 ms SysTick fires `_tx_timer_interrupt`, which advances the
 * kernel tick counter and pre-empts the running thread when a higher-
 * priority thread becomes ready.
 *
 * @pre Called from exception context (IPSR == 15).
 * @pre `_tx_initialize_low_level` has programmed SysTick.
 * @post One ThreadX tick has been credited.
 *
 * @since 0.1.0
 */
void SysTick_Handler(void);
void SysTick_Handler(void)
{
  _tx_timer_interrupt();
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
 * @brief Pack ARGB components into a GUIX `GX_COLOR` word.
 *
 * @details
 * GUIX colours are `0xAARRGGBB`. Used for both the label-text colour
 * and the GUIX background fill.
 *
 * @param[in] r Red channel 0..255.
 * @param[in] g Green channel 0..255.
 * @param[in] b Blue channel 0..255.
 * @return Packed ARGB.
 *
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_demo_color_shift_a = 24U,
  k_demo_color_shift_r = 16U,
  k_demo_color_shift_g = 8U,
} demo_color_shift_t;

static inline GX_COLOR demo_argb(uint8_t r, uint8_t g, uint8_t b)
{
  return (GX_COLOR)((0xFFUL << k_demo_color_shift_a) | ((uint32_t)r << k_demo_color_shift_r) |
                    ((uint32_t)g << k_demo_color_shift_g) | (uint32_t)b);
}

/**
 * @brief Build a GX_RECTANGLE inline.
 *
 * @param[in] left   Inclusive left edge.
 * @param[in] top    Inclusive top edge.
 * @param[in] right  Inclusive right edge.
 * @param[in] bottom Inclusive bottom edge.
 * @return Initialised rectangle.
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
 * @brief Tear up the GUIX widget tree and start the event loop.
 *
 * @details
 * Called from `gui_thread_entry`. Runs:
 *
 *   1. `ra_guix_display_driver_bind` -- hand the framebuffer base
 *      to the shim.
 *   2. `gx_system_initialize` -- bring up GUIX's internal state.
 *   3. `gx_studio_display_configure`-equivalent: `gx_display_create`
 *      with `ra_guix_display_driver_setup` as the driver-setup
 *      callback.
 *   4. `gx_canvas_create` against the same framebuffer pointer.
 *   5. Build the widget tree: a root window containing the prompt
 *      (label) and a button.
 *   6. `gx_widget_show` on the root, then `gx_system_start` (does
 *      not return).
 *
 * @pre GLCDC is running and scanning out `s_framebuffer`.
 * @pre ThreadX scheduler is up.
 *
 * @post GUIX is in its event loop; the canvas displays the demo.
 *
 * @since 0.1.0
 */
static void demo_gui_setup_and_run(void)
{
  if (ra_guix_display_driver_bind(&s_framebuffer[0][0],
                                  (uint16_t)k_demo_fb_width,
                                  (uint16_t)k_demo_fb_height) != k_ra_ok) {
    demo_panic_halt();
  }

  if (gx_system_memory_allocator_set(demo_gx_alloc, demo_gx_free) != GX_SUCCESS) {
    demo_panic_halt();
  }

  if (gx_system_initialize() != GX_SUCCESS) {
    demo_panic_halt();
  }

  if (gx_display_create(&s_display,
                        "ra8d2-glcdc",
                        ra_guix_display_driver_setup,
                        (GX_VALUE)k_demo_fb_width,
                        (GX_VALUE)k_demo_fb_height) != GX_SUCCESS) {
    demo_panic_halt();
  }

  /* Wire up a tiny static colour table so `gx_display_color_set`
   * has somewhere to land.  Without this the table pointer is NULL
   * and every set call writes into uninitialised memory (or, more
   * commonly, just gets ignored because the table is empty), and
   * all widget paints end up using colour value 0 = black. */
  static GX_COLOR s_color_table[32];
  for (uint32_t i = 0U; i < 32U; i++) {
    s_color_table[i] = demo_argb(0x00U, 0x00U, 0x00U); /* default black */
  }
  s_color_table[GX_COLOR_ID_CANVAS]      = demo_argb(0xFFU, 0xFFU, 0xFFU); /* white */
  s_color_table[GX_COLOR_ID_WINDOW_FILL] = demo_argb(0xFFU, 0xFFU, 0xFFU);
  s_color_table[GX_COLOR_ID_TEXT]        = demo_argb(0x00U, 0x00U, 0x00U);
  s_color_table[GX_COLOR_ID_BTN_UPPER]   = demo_argb(0xE0U, 0xE0U, 0xE0U);
  s_color_table[GX_COLOR_ID_BTN_LOWER]   = demo_argb(0x80U, 0x80U, 0x80U);
  if (gx_display_color_table_set(&s_display, s_color_table, 32) != GX_SUCCESS) {
    demo_panic_halt();
  }

  if (gx_canvas_create(&s_canvas,
                       "ra8d2-canvas",
                       &s_display,
                       GX_CANVAS_MANAGED,
                       (UINT)k_demo_fb_width,
                       (UINT)k_demo_fb_height,
                       (GX_COLOR*)&s_framebuffer[0][0],
                       (ULONG)k_demo_fb_width * (ULONG)k_demo_fb_height *
                         (ULONG)k_demo_bpp_rgb565) != GX_SUCCESS) {
    demo_panic_halt();
  }
  /* GUIX canvases come up with `gx_canvas_status` clear of
   * GX_CANVAS_VISIBLE; without this call the system thread's first
   * canvas refresh decides there is nothing to draw and the panel
   * stays at whatever was in SRAM at boot (black). */
  if (gx_canvas_show(&s_canvas) != GX_SUCCESS) {
    demo_panic_halt();
  }

  const GX_RECTANGLE root_rect =
    demo_rect(0, 0, (GX_VALUE)(k_demo_fb_width - 1U), (GX_VALUE)(k_demo_fb_height - 1U));
  if (gx_window_root_create(&s_root, "ra8d2-root", &s_canvas, GX_STYLE_NONE, 0U, &root_rect) !=
      GX_SUCCESS) {
    demo_panic_halt();
  }

  const GX_RECTANGLE win_rect = root_rect;
  if (gx_window_create(&s_main_window,
                       "ra8d2-main-window",
                       GX_NULL,
                       GX_STYLE_BORDER_THIN | GX_STYLE_ENABLED,
                       0U,
                       &win_rect) != GX_SUCCESS) {
    demo_panic_halt();
  }
  if (gx_widget_attach((GX_WIDGET*)&s_root, (GX_WIDGET*)&s_main_window) != GX_SUCCESS) {
    demo_panic_halt();
  }

  /* Label rectangle: top-half of the canvas, centred horizontally. */
  const GX_RECTANGLE label_rect = demo_rect(8, 16, (GX_VALUE)(k_demo_fb_width - 9U), 48);
  if (gx_prompt_create(&s_label,
                       "ra8d2-label",
                       &s_main_window,
                       0,
                       GX_STYLE_TEXT_LEFT | GX_STYLE_ENABLED,
                       0U,
                       &label_rect) != GX_SUCCESS) {
    demo_panic_halt();
  }
  GX_STRING label_string;
  label_string.gx_string_ptr    = s_label_text;
  label_string.gx_string_length = (UINT)(sizeof(s_label_text) - 1U);
  if (gx_prompt_text_set_ext(&s_label, &label_string) != GX_SUCCESS) {
    demo_panic_halt();
  }

  /* Button rectangle: bottom-centre. */
  const GX_RECTANGLE btn_rect = demo_rect(80, 80, 175, 112);
  if (gx_button_create(&s_button,
                       "ra8d2-button",
                       &s_main_window,
                       GX_STYLE_BORDER_RAISED | GX_STYLE_ENABLED,
                       1U,
                       &btn_rect) != GX_SUCCESS) {
    demo_panic_halt();
  }
  /* The button uses its widget name as on-screen text via the default
   * skin because we did not load any string-table resource. The demo
   * is happy with the empty raised rectangle as a "tap" target. */

  if (gx_widget_show((GX_WIDGET*)&s_root) != GX_SUCCESS) {
    demo_panic_halt();
  }

  /* The demo never loaded a GUIX theme so colour IDs 0/1/2 default
   * to 0x000000 = black -- the GUIX paint pass ends up writing all
   * zeros into the framebuffer and the panel stays solid black even
   * though the widget tree painted successfully (swap_count > 0).
   * Seed a handful of slots with visible colours so the initial
   * paint actually has something to display. */
  (void)gx_display_color_set(&s_display,
                             0U,
                             demo_argb(0xFFU, 0xFFU, 0xFFU)); /* canvas bg = white */
  (void)gx_display_color_set(&s_display,
                             GX_COLOR_ID_WINDOW_FILL,
                             demo_argb(0xFFU, 0xFFU, 0xFFU)); /* window fill white */
  (void)gx_display_color_set(&s_display,
                             GX_COLOR_ID_WINDOW_BORDER,
                             demo_argb(0x00U, 0x00U, 0x00U)); /* black border */
  (void)gx_display_color_set(&s_display,
                             GX_COLOR_ID_TEXT,
                             demo_argb(0x00U, 0x00U, 0x00U)); /* text black */
  (void)gx_display_color_set(&s_display, GX_COLOR_ID_BTN_UPPER, demo_argb(0xE0U, 0xE0U, 0xE0U));
  (void)gx_display_color_set(&s_display, GX_COLOR_ID_BTN_LOWER, demo_argb(0x80U, 0x80U, 0x80U));

  /* Force an explicit initial paint so the first frame lands in the
   * framebuffer before gx_system_start blocks on the event queue. */
  (void)gx_system_canvas_refresh();

  /* Hand control to GUIX. Returns only on internal scheduler error. */
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
 * @brief Application thread: cycles the label colour every 750 ms.
 *
 * @param[in] thread_input Unused (ThreadX cookie).
 *
 * @pre GUIX has finished initialising (the GUI thread is running).
 * @post `s_label_color_state` toggles between blue and orange.
 *
 * @since 0.1.0
 */
static void app_thread_entry(ULONG thread_input)
{
  (void)thread_input;

  while (1) {
    (void)tx_thread_sleep((ULONG)k_demo_app_period_ticks);

    /* Toggle the colour state. */
    if (s_label_color_state == (uint8_t)k_demo_color_state_blue) {
      s_label_color_state = (uint8_t)k_demo_color_state_orange;
    } else {
      s_label_color_state = (uint8_t)k_demo_color_state_blue;
    }

    /* Re-skin the label and ask GUIX for a paint pass. The two
     * built-in colour resources (TEXT and SELECTED_TEXT) take ARGB
     * values straight from `gx_display_color_set`; we drive both
     * slots so the prompt's "selected" highlight follows the cycle
     * if a focus event ever lands on it. */
    GX_COLOR fg = demo_argb(0x20U, 0x60U, 0xE0U); /* default blue. */
    if (s_label_color_state == (uint8_t)k_demo_color_state_orange) {
      fg = demo_argb(0xE0U, 0x80U, 0x10U); /* highlight orange. */
    }
    (void)gx_display_color_set(&s_display, GX_COLOR_ID_TEXT, fg);
    (void)gx_system_canvas_refresh();
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
  /* GLCDC bring-up before tx_kernel_enter, but WITHOUT touching
   * SysTick: ThreadX owns the SysTick vector once linked in, and the
   * handler dereferences kernel state that doesn't exist yet here.
   * Use busy-wait loops (Cortex-M85 nop @ ~1 GHz -- not calibrated;
   * the GLCDC + panel are tolerant of generous delays). */
  uint32_t cpuclk0_hz = 0U;
  (void)ra_cgc_init();
  (void)ra_cgc_get_clock_hz(k_ra_clock_id_cpuclk0, &cpuclk0_hz);
  (void)ra_mstp_init();
  (void)ra_board_led_init(k_ra_board_led1);

  /* ~500 ms PLL / panel-POR settle (1 GHz, 8-cycle nop pair -> ~6 ns). */
  for (volatile uint32_t i = 0U; i < 60000000U; i++) {
    __asm__ volatile("nop");
  }

  /* Re-use the BSP power-on path -- but it calls ra_delay_ms inside,
   * which uses SysTick. Inline the bare GPIO sequence here to avoid
   * the dependency. */
  (void)ra_gpio_output_init(k_ra_pin_lcd_reset_l, k_ra_level_low);
  for (volatile uint32_t i = 0U; i < 6000000U; i++) {
    __asm__ volatile("nop");
  }
  (void)ra_gpio_write(k_ra_pin_lcd_reset_l, k_ra_level_high);
  for (volatile uint32_t i = 0U; i < 6000000U; i++) {
    __asm__ volatile("nop");
  }
  (void)ra_gpio_output_init(k_ra_pin_lcd_blen, k_ra_level_high);

  (void)ra_board_glcdc_init(k_ra_board_glcdc_fmt_rgb888);

  const ra_glcdc_config_t cfg = {
    .framebuffer_addr = (uint32_t)(uintptr_t)s_framebuffer,
    .width_px         = (uint16_t)k_demo_fb_width,
    .height_px        = (uint16_t)k_demo_fb_height,
    .format           = k_ra_glcdc_fmt_rgb565,
  };
  (void)ra_glcdc_init(&cfg);
  (void)ra_glcdc_set_background_color(0x000000U);
  (void)ra_glcdc_start(true);
  (void)ra_glcdc_layer1_show((uintptr_t)s_framebuffer);

  /* Do NOT call ra_isr_globals_enable() here -- ThreadX unmasks
   * globals inside tx_kernel_enter, and unmasking earlier lets the
   * GLCDC VPOS interrupt (enabled by ra_glcdc_start) fire into a
   * not-yet-installed NVIC vector, faulting the chip into reset. */

  tx_kernel_enter();
#endif

  /* Belt + suspenders -- the kernel does not return. */
  while (1) {
    __asm__ volatile("wfi");
  }
}

#pragma GCC diagnostic pop
