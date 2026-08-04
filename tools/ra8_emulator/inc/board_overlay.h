/**
 * @file board_overlay.h
 * @brief Composite board-view renderer: panel framebuffer + a status sidebar
 *
 * @details
 * Builds the graphical BOARD VIEW the emulator presents: the emulated GLCDC
 * panel framebuffer on the left, plus a Cocoa-blitted status sidebar on the
 * right that makes a NON-display example observable -- three LED indicators
 * that light in the real GPIO LED colour, and text lines for the live USB /
 * UART / timer-IRQ / touch state. The whole composite is rendered into one
 * RGB565 buffer in portable C (a tiny embedded 5x7 ASCII font draws the text
 * and filled rectangles draw the LED dots), so the exact same pixels the macOS
 * window shows are what @c --ppm writes out -- the status overlay is therefore
 * verifiable headlessly with a region/pixel check, not just visible on screen.
 *
 * This module owns no Unicorn engine and no AppKit dependency: main.c reads the
 * peripheral state out of board_periph / board_usb, fills a ::board_status_t,
 * and calls ::board_overlay_compose; board_view.m only blits the result. Plain
 * C so it builds and tests on any host.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Number of board LED indicators shown in the status sidebar. */
typedef enum : uint32_t {
  k_overlay_led_count = 3U, /**< LED1 / LED2 / LED3 (mirrors the BSP). */
} board_overlay_dims_t;

/** @brief Console-panel sizing for the board view. */
typedef enum : uint32_t {
  k_overlay_console_rows     = 18U,  /**< Console lines shown (newest at bottom). */
  k_overlay_line_cap         = 128U, /**< Max chars copied per console line.      */
  k_overlay_console_tabs_max = 16U,  /**< Tab-bar capacity (channels = tabs).     */
} board_overlay_console_t;

/**
 * @brief On-screen sidebar control a click landed on.
 *
 * @details ::board_overlay_hit_button classifies a composite-space click into
 * one of the sidebar's interactive controls (or none), so the caller can route
 * a window / @c --click tap to the user-switch model instead of the touch panel.
 */
typedef enum : uint32_t {
  k_board_overlay_btn_none     = 0U, /**< Click did not land on a control.   */
  k_board_overlay_btn_sw1      = 1U, /**< On-screen SW1 (P009) push-button.  */
  k_board_overlay_btn_sw2      = 2U, /**< On-screen SW2 (P008) push-button.  */
  k_board_overlay_btn_console  = 3U, /**< Console panel: toggle autoscroll.  */
  k_board_overlay_btn_battery  = 4U, /**< Battery slider track: set SOC.     */
  k_board_overlay_btn_batt_chg = 5U, /**< Battery CHG button: toggle charge. */
  k_board_overlay_btn_lowpower = 6U, /**< CORE low-power toggle button.      */
} board_overlay_btn_t;

/**
 * @brief One board LED's live state for the status sidebar.
 *
 * @details A lit indicator is filled with @c color when @c on is true and drawn
 * dark otherwise; @c label is the short caption printed beside the dot.
 */
typedef struct {
  bool        on;    /**< True when the GPIO pin is driven high.  */
  uint16_t    color; /**< RGB565 lit colour (blue / green / red). */
  const char* label; /**< Short ASCII caption (e.g. "LED1").      */
} board_led_status_t;

/**
 * @brief Live peripheral snapshot the status sidebar renders each frame.
 *
 * @details main.c fills this from the read-only board_periph / board_usb
 * getters once per present. String fields are borrowed pointers valid only for
 * the duration of the ::board_overlay_compose call; the overlay copies nothing.
 */
typedef struct {
  board_led_status_t leds[k_overlay_led_count]; /**< Per-LED indicator state.      */
  const char*        usb_state;                 /**< USB device state string.      */
  const char*        uart_line;                 /**< Last captured UART line.      */
  uint32_t           irq_total;                 /**< Total NVIC IRQs taken.        */
  uint32_t           irq0;                      /**< IRQ0 taken count.             */
  uint32_t           irq1;                      /**< IRQ1 taken count.             */
  bool               has_touch;                 /**< A touch has been drained.     */
  uint16_t           touch_x;                   /**< Last touch X (if any).        */
  uint16_t           touch_y;                   /**< Last touch Y (if any).        */
  bool               sw1_pressed;               /**< On-screen SW1 held down.      */
  bool               sw2_pressed;               /**< On-screen SW2 held down.      */
  uint8_t            battery_soc;               /**< Fuel-gauge SOC percent.       */
  bool               battery_charging;          /**< Fuel gauge reports charge.    */
  bool               low_power;                 /**< Low-power (M33 4:1 clock) on. */
  bool               core_is_m33;               /**< Primary core is the M33.      */
  const char*        app_name;                  /**< App / window title.           */
  bool               sd_attached;               /**< A microSD card is attached.   */
  uint64_t           sd_bytes;                  /**< SD card size in bytes (>4GB). */
  uint8_t            sd_fat_bits;               /**< 12/16/32 if --sd-new, else 0. */
  const char*        sd_label;                  /**< SD volume label (--sd-new).   */
  /* Console window: console[0] is the newest *visible* line (the firmware ring
   * is deeper -- see console_total -- and console_scroll says how far back this
   * window starts). Unused rows are empty strings. */
  /** Visible lines (active tab). */
  char     console[k_overlay_console_rows][k_overlay_line_cap];
  uint32_t console_count;      /**< Visible lines populated (<= rows).        */
  uint32_t console_scroll;     /**< Lines scrolled back from newest (0=live). */
  uint32_t console_total;      /**< Total lines in the active channel ring.   */
  bool     console_autoscroll; /**< True = following live tail; false=paused. */
  /* Tabbed-console bar: one tab per board_console channel. console_active_ch is
   * the channel the console body shows; the names + per-tab line counts caption
   * the bar. Borrowed name pointers valid for the compose call only. */
  /** Tabs to draw (<= max). */
  uint32_t console_ch_count;
  /** Active tab index. */
  uint32_t console_active_ch;
  /** Per-tab caption. */
  const char* console_ch_name[k_overlay_console_tabs_max];
  /** Per-tab line count. */
  uint32_t console_ch_count_lines[k_overlay_console_tabs_max];
  uint32_t pc;            /**< Current emulated program counter.             */
  uint32_t chunks;        /**< Emulation chunks run so far.                  */
  uint32_t mmio_reads;    /**< Modelled-peripheral MMIO reads served.        */
  uint32_t mmio_writes;   /**< Modelled-peripheral MMIO writes served.       */
  uint32_t uart_tx_total; /**< Total UART TX bytes the firmware emitted.     */
  bool     running;       /**< True while the run loop is live (not parked). */
} board_status_t;

/**
 * @brief Width in pixels the status sidebar adds to the right of the panel.
 *
 * @return Fixed sidebar width (the composite width is panel width + this).
 * @since 0.1.0
 */
uint16_t board_overlay_sidebar_width(void);

/**
 * @brief Total composite width for a panel of width @p panel_w.
 *
 * @param[in] panel_w Panel framebuffer width in pixels.
 * @return @p panel_w plus the sidebar width.
 * @since 0.1.0
 */
uint16_t board_overlay_total_width(uint16_t panel_w);

/**
 * @brief Total composite height for a panel of height @p panel_h.
 *
 * @details At least a minimum so the sidebar's text always fits even when the
 * panel is short (a non-display example may have a tiny / blank panel region).
 *
 * @param[in] panel_h Panel framebuffer height in pixels.
 * @return The larger of @p panel_h and the sidebar's minimum height.
 * @since 0.1.0
 */
uint16_t board_overlay_total_height(uint16_t panel_h);

/**
 * @brief Render the full composite (panel region + status sidebar) into @p out.
 *
 * @details Clears @p out to the sidebar background, blits the @p panel pixels
 * into the top-left region, draws a divider, then paints the three LED
 * indicators and the USB / UART / IRQ / touch text lines from @p st. The
 * output is exactly what board_view blits and what @c --ppm writes, so a
 * headless pixel check over a sidebar region verifies the overlay.
 *
 * @param[out] out     Composite RGB565 buffer, ::board_overlay_total_width by
 *                     ::board_overlay_total_height pixels (row-major, no gap).
 * @param[in]  panel   Panel framebuffer (RGB565), or NULL for a blank panel.
 * @param[in]  panel_w Panel width in pixels.
 * @param[in]  panel_h Panel height in pixels.
 * @param[in]  st      Live peripheral snapshot to render (NULL draws no status).
 * @return Nothing.
 * @since 0.1.0
 */
void board_overlay_compose(uint16_t*             out,
                           const uint16_t*       panel,
                           uint16_t              panel_w,
                           uint16_t              panel_h,
                           const board_status_t* st);

/**
 * @brief Classify a composite-space click against the sidebar's on-screen buttons.
 *
 * @details The interactive board view draws SW1 / SW2 push-buttons in the status
 * sidebar at a fixed layout (the same constants ::board_overlay_compose draws
 * them with). Given a click in composite pixels and the @p panel_w used to
 * compose the frame, this reports which button the click hit so the caller can
 * drive the user-switch model rather than injecting a panel touch. Out-of-band
 * clicks (the panel, sidebar text) return ::k_board_overlay_btn_none.
 *
 * @param[in] x       Click column in composite pixels (top-left origin).
 * @param[in] y       Click row in composite pixels (top-left origin).
 * @param[in] panel_w Panel width the composite was built with (sidebar origin).
 * @return The button hit, or ::k_board_overlay_btn_none.
 * @since 0.1.0
 */
board_overlay_btn_t board_overlay_hit_button(uint16_t x, uint16_t y, uint16_t panel_w);

/**
 * @brief Map a click column on the battery slider track to a 0..100 SOC percent.
 *
 * @details The POWER section draws a horizontal slider whose fill mirrors the
 * battery SOC. ::board_overlay_hit_button reports ::k_board_overlay_btn_battery
 * when a click lands on that track's row; this then converts the click column to
 * the percent the user dragged to, so the caller can drive the fuel-gauge model.
 * The percent is the click's fraction across the track, clamped to 0..100 (a
 * click left of the track yields 0, right of it yields 100), and the same fixed
 * geometry that ::board_overlay_compose draws the track with is used here so the drawn
 * fill and the drag position stay in lock-step.
 *
 * @param[in]  x       Click column in composite pixels (top-left origin).
 * @param[in]  panel_w Panel width the composite was built with (sidebar origin).
 * @param[out] out_pct Receives the mapped state-of-charge percent (0..100).
 * @return True when @p out_pct was written, false if @p out_pct is NULL.
 * @since 0.1.0
 */
bool board_overlay_battery_pct_at(uint16_t x, uint16_t panel_w, uint8_t* out_pct);

/**
 * @brief Map a composite-space click on the console tab bar to a tab index.
 *
 * @details The tabbed console wraps @p tab_count equal-width tabs into as many
 * rows as the console panel width needs (the same grid geometry the compose path
 * draws them with). Given a click in composite pixels, the @p panel_w used to
 * compose the frame, and the live @p tab_count, this reports which tab the click
 * landed on so the caller can switch the active console channel. Clicks outside
 * the tab grid (including the console body below it) return false and leave
 * @p out_idx untouched, so the caller can fall through to the body's autoscroll
 * toggle.
 *
 * @param[in]  x         Click column in composite pixels (top-left origin).
 * @param[in]  y         Click row in composite pixels (top-left origin).
 * @param[in]  panel_w   Panel width the composite was built with (sidebar origin).
 * @param[in]  tab_count Number of tabs drawn (1 .. ::k_overlay_console_tabs_max).
 * @param[out] out_idx   Receives the hit tab index (0 .. @p tab_count - 1).
 * @return True when a tab was hit and @p out_idx written; false otherwise.
 * @since 0.1.0
 */
bool board_overlay_hit_console_tab(uint16_t  x,
                                   uint16_t  y,
                                   uint16_t  panel_w,
                                   uint32_t  tab_count,
                                   uint32_t* out_idx);

#ifdef __cplusplus
}
#endif
