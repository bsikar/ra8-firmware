/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file board_overlay_internal.h
 * @brief Module-private layout constants + draw primitives for the overlay
 *
 * @details
 * The board-view overlay is one logical module split across two translation
 * units: the panel composer / hit-testing core (board_overlay.c) and the
 * pixel-level draw primitives + embedded 5x7 font
 * (board_overlay_draw.c). The shared layout/colour/font constants and the
 * primitive declarations live here; nothing in this header is part of the
 * emulator-facing API in inc/board_overlay.h.
 *
 *
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

#include "board_overlay.h"
#include "ra8_attributes.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum : int32_t {
  k_led_label_dy  = 6,   /**< LED label vertical offset within its dot.    */
  k_led_row_dy    = 22,  /**< "LEDS" heading to the LED row.               */
  k_led_col_dx    = 150, /**< Horizontal spacing between LEDs.             */
  k_led_row_h     = 40,  /**< LED row height (advance to next block).      */
  k_pad_x         = 18,  /**< Left padding inside the sidebar.             */
  k_sidebar_top   = 16,  /**< Sidebar top margin.                          */
  k_title_gap     = 26,  /**< Title line to the app-name line.             */
  k_appname_gap   = 30,  /**< App-name line to the run-stats block.        */
  k_section_gap   = 16,  /**< Gap above a section heading.                 */
  k_heading_gap   = 20,  /**< Section heading to its first row.            */
  k_row_step      = 16,  /**< Vertical step between text rows.             */
  k_rule_dy       = 10,  /**< Heading baseline to its underline rule.      */
  k_kv_label_cols = 7,   /**< Label column width (glyphs) so values align. */
  k_title_bar_h   = 40,  /**< Title-bar band height (board name at 2x).    */
} overlay_layout_t;

/** @brief Sidebar geometry and palette (RGB565). */
typedef enum : uint32_t {
  k_ovl_sidebar_w     = 520U,    /**< Sidebar width added on the right.         */
  k_ovl_min_h         = 600U,    /**< Minimum composite height.                 */
  k_ovl_bg            = 0x10A2U, /**< Dark slate sidebar background.            */
  k_ovl_bg_alt        = 0x18E3U, /**< Slightly lighter band (section panel).    */
  k_ovl_panel_bg      = 0x0000U, /**< Fill behind a missing/short panel.        */
  k_ovl_divider       = 0x4208U, /**< Vertical divider between panel/sidebar.   */
  k_ovl_rule          = 0x39C7U, /**< Thin horizontal section rule.             */
  k_ovl_text          = 0xCE59U, /**< Light-grey body text.                     */
  k_ovl_dim           = 0x8410U, /**< Dim label text (field names).             */
  k_ovl_heading       = 0xFFFFU, /**< White heading text.                       */
  k_ovl_accent        = 0x5D1FU, /**< Cyan-blue accent (title / headings).      */
  k_ovl_ok            = 0x3666U, /**< Green "running / ok" indicator.           */
  k_ovl_amber         = 0xFD20U, /**< Amber "paused / attention" indicator.     */
  k_ovl_console_bg    = 0x0841U, /**< Console panel background (near-black).    */
  k_ovl_console_txt   = 0x07E6U, /**< Console text (terminal green).            */
  k_ovl_console_new   = 0xFFFFU, /**< Newest console line (white highlight).    */
  k_ovl_tab_on_bg     = 0x2965U, /**< Active console tab background (blue).     */
  k_ovl_tab_off_bg    = 0x18E3U, /**< Inactive console tab background (dim).    */
  k_ovl_tab_on_txt    = 0xFFFFU, /**< Active console tab caption (white).       */
  k_ovl_tab_off_txt   = 0x8410U, /**< Inactive tab caption, has traffic (grey). */
  k_ovl_tab_empty_txt = 0x4208U, /**< Inactive tab caption, no traffic (dim).   */
  k_ovl_led_off       = 0x2104U, /**< Unlit LED dot.                            */
  k_ovl_led_ring      = 0x6B4DU, /**< LED dot outline.                          */
  k_ovl_btn_border    = 0x8430U, /**< On-screen button outline.                 */
  k_ovl_btn_up        = 0x3186U, /**< Button face, released.                    */
  k_ovl_btn_down      = 0x05E0U, /**< Button face, pressed (green).             */
  k_ovl_btn_label     = 0xFFFFU, /**< Button caption text.                      */
  k_ovl_red           = 0xF800U, /**< Red gauge fill (low / critical SOC).      */
  k_ovl_glyph_w       = 5U,      /**< Font glyph width in pixels.               */
  k_ovl_glyph_h       = 7U,      /**< Font glyph height in pixels.              */
  k_ovl_glyph_first   = 0x20U,   /**< First glyph in the font table (space).    */
  k_ovl_glyph_last    = 0x7EU,   /**< Last glyph in the font table (tilde).     */
} board_overlay_cfg_t;

/**
 * @brief Fixed layout of the on-screen SW1 / SW2 buttons (sidebar-relative px).
 *
 * @details Both ::draw_buttons and ::board_overlay_hit_button derive the button
 * rectangles from these constants, so the drawn face and the click hit-box stay
 * in lock-step. The block sits below the status-text block (which ends near y
 * 166 for any panel) and above the minimum composite height, so it is on-screen
 * for every panel size.
 */
typedef enum : int32_t {
  k_btn_x_dx    = 18,  /**< Button column inset from the sidebar origin. */
  k_btn_head_y  = 404, /**< "BUTTONS" heading row.                       */
  k_btn_y       = 424, /**< Button face top.                             */
  k_btn_w       = 150, /**< Button face width.                           */
  k_btn_h       = 36,  /**< Button face height.                          */
  k_btn_gap     = 20,  /**< Horizontal gap between SW1 and SW2.          */
  k_btn_label_x = 14,  /**< Caption inset within the button face.        */
  k_btn_label_y = 12,  /**< Caption top inset within the button face.    */
} overlay_btn_layout_t;

/** @brief Fixed Y anchor for the I/O section (below the LED row). */
typedef enum : int32_t {
  k_io_head_y = 250, /**< "I/O" section heading row (clear of the LEDs). */
} overlay_io_layout_t;

/**
 * @brief Fixed layout of the POWER section: battery slider + CHG toggle.
 *
 * @details ::draw_power, ::board_overlay_hit_button and
 * ::board_overlay_battery_pct_at all derive their rectangles from these, so the
 * drawn slider, its click hit-box, and the drag-to-percent map stay in lock-step.
 * The block sits below the I/O text block (which ends near y 334) and above the
 * BUTTONS block, so it is on-screen for every panel size.
 */
typedef enum : int32_t {
  k_pwr_x_dx     = 18,  /**< POWER column inset from the sidebar origin.   */
  k_pwr_head_y   = 344, /**< "POWER" heading row.                          */
  k_pwr_y        = 364, /**< Battery slider track top.                     */
  k_pwr_h        = 26,  /**< Slider track height.                          */
  k_pwr_track_w  = 320, /**< Slider track width (the full drag range).     */
  k_pwr_chg_off  = 338, /**< CHG toggle x offset from the track origin.    */
  k_pwr_chg_w    = 126, /**< CHG toggle face width.                        */
  k_pwr_label_y  = 9,   /**< Caption top inset within a POWER face.        */
  k_pwr_soc_low  = 20,  /**< SOC at or below this draws the gauge red.     */
  k_pwr_soc_mid  = 50,  /**< SOC at or below this draws the gauge amber.   */
  k_pwr_soc_full = 100, /**< SOC clamp / percent denominator.              */
  k_core_lp_off  = 338, /**< Low-power toggle x offset (right of SW1/SW2). */
  k_core_lp_w    = 126, /**< Low-power toggle face width.                  */
} overlay_pwr_layout_t;

/** @brief Console-panel layout (the bottom scrolling log with a tab bar). */
typedef enum : int32_t {
  k_con_head_y    = 480, /**< "CONSOLE" heading row.                     */
  k_con_y         = 496, /**< Console panel top.                         */
  k_con_pad       = 8,   /**< Inner padding inside the console panel.    */
  k_con_line_h    = 10,  /**< Vertical step between console text lines.  */
  k_con_bottom    = 14,  /**< Bottom margin below the console panel.     */
  k_con_tab_h     = 13,  /**< Tab-bar row height (one row of the grid).  */
  k_con_tab_txt   = 3,   /**< Caption inset within a tab cell.           */
  k_con_tab_gap   = 1,   /**< Gap between adjacent tab cells.            */
  k_con_tab_min_w = 58,  /**< Minimum tab-cell pitch; sets tabs-per-row. */
  k_con_tab_cap_y = 3,   /**< Caption y-offset within a tab row.         */
} overlay_con_layout_t;

/** @brief Binary size-unit factor for the SD-card capacity readout. */
typedef enum : uint64_t {
  k_sd_unit_div = 1024ULL, /**< Bytes per KiB step (KiB->MiB->GiB ladder). */
} overlay_sd_unit_t;

/** @brief Plot one clipped pixel into the composite (draw primitive). */
RA8_PRIV void px(uint16_t* out, uint16_t w, uint16_t h, int32_t x, int32_t y, uint16_t color);

/** @brief Fill a clipped rectangle in the composite (draw primitive). */
RA8_PRIV void fill_rect(uint16_t* out,
                        uint16_t  w,
                        uint16_t  h,
                        int32_t   x,
                        int32_t   y,
                        int32_t   rw,
                        int32_t   rh,
                        uint16_t  color);

/** @brief Draw a NUL-terminated string; returns the x after the last glyph. */
RA8_PRIV int32_t draw_text(uint16_t*   out,
                           uint16_t    w,
                           uint16_t    h,
                           int32_t     x,
                           int32_t     y,
                           const char* s,
                           uint16_t    color,
                           int32_t     scale);

/** @brief Draw a section heading (accent colour) with a thin rule beneath it. */
RA8_PRIV int32_t
section_head(uint16_t* out, uint16_t w, uint16_t h, int32_t x, int32_t y, const char* title);

/** @brief Draw a "label  value" row: dim label, coloured value; returns next y. */
RA8_PRIV int32_t kv_row(uint16_t*   out,
                        uint16_t    w,
                        uint16_t    h,
                        int32_t     x,
                        int32_t     y,
                        const char* label,
                        const char* value,
                        uint16_t    val_color);

/** @brief Blit the rendered panel into the composite's panel region. */
RA8_PRIV void blit_panel(uint16_t*       out,
                         uint16_t        w,
                         uint16_t        h,
                         const uint16_t* panel,
                         uint16_t        panel_w,
                         uint16_t        panel_h);

#ifdef __cplusplus
}
#endif
