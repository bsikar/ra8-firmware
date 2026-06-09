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

/**
 * @brief One board LED's live state for the status sidebar.
 *
 * @details A lit indicator is filled with @c color when @c on is true and drawn
 * dark otherwise; @c label is the short caption printed beside the dot.
 */
typedef struct {
  bool        on;    /**< True when the GPIO pin is driven high.        */
  uint16_t    color; /**< RGB565 lit colour (blue / green / red).       */
  const char* label; /**< Short ASCII caption (e.g. "LED1").            */
} board_led_status_t;

/**
 * @brief Live peripheral snapshot the status sidebar renders each frame.
 *
 * @details main.c fills this from the read-only board_periph / board_usb
 * getters once per present. String fields are borrowed pointers valid only for
 * the duration of the ::board_overlay_compose call; the overlay copies nothing.
 */
typedef struct {
  board_led_status_t leds[k_overlay_led_count]; /**< Per-LED indicator state.   */
  const char*        usb_state;                 /**< USB device state string.   */
  const char*        uart_line;                 /**< Last captured UART line.   */
  uint32_t           irq_total;                 /**< Total NVIC IRQs taken.     */
  uint32_t           irq0;                      /**< IRQ0 taken count.          */
  uint32_t           irq1;                      /**< IRQ1 taken count.          */
  bool               has_touch;                 /**< A touch has been drained.  */
  uint16_t           touch_x;                   /**< Last touch X (if any).     */
  uint16_t           touch_y;                   /**< Last touch Y (if any).     */
  const char*        app_name;                  /**< App / window title.        */
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

#ifdef __cplusplus
}
#endif
