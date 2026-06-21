/**
 * @file board_overlay.c
 * @brief Composite board-view renderer implementation (see board_overlay.h)
 *
 * @details
 * Renders the panel framebuffer plus a status sidebar into one RGB565 buffer.
 * Text is drawn with an embedded 5x7 column-major ASCII font (printable range
 * 0x20..0x7E); each glyph is five bytes, one per column, with the low seven
 * bits giving the rows top-to-bottom. Keeping the whole composite in a plain
 * pixel buffer is deliberate: the macOS window and the @c --ppm snapshot show
 * the identical bytes, so the overlay is verifiable headlessly.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#include "board_overlay.h"

#include <stdint.h>
#include <stdio.h>

/** @brief Status-sidebar layout offsets (pixels). */
typedef enum : int32_t {
  k_led_label_dy = 6,   /**< LED label vertical offset within its dot. */
  k_led_row_dy   = 22,  /**< "LEDS" heading to the LED row.            */
  k_led_col_dx   = 150, /**< Horizontal spacing between LEDs.          */
  k_led_row_h    = 40,  /**< LED row height (advance to next block).   */
  k_pad_x        = 18,  /**< Left padding inside the sidebar.          */
  k_sidebar_top  = 16,  /**< Sidebar top margin.                       */
  k_title_gap    = 26,  /**< Title line to the app-name line.          */
  k_appname_gap  = 30,  /**< App-name line to the run-stats block.     */
  k_section_gap  = 16,  /**< Gap above a section heading.              */
  k_heading_gap  = 20,  /**< Section heading to its first row.         */
  k_row_step     = 16,  /**< Vertical step between text rows.          */
} overlay_layout_t;

/** @brief Sidebar geometry and palette (RGB565). */
typedef enum : uint32_t {
  k_ovl_sidebar_w   = 520U,    /**< Sidebar width added on the right.       */
  k_ovl_min_h       = 600U,    /**< Minimum composite height.               */
  k_ovl_bg          = 0x10A2U, /**< Dark slate sidebar background.          */
  k_ovl_bg_alt      = 0x18E3U, /**< Slightly lighter band (section panel).  */
  k_ovl_panel_bg    = 0x0000U, /**< Fill behind a missing/short panel.      */
  k_ovl_divider     = 0x4208U, /**< Vertical divider between panel/sidebar. */
  k_ovl_rule        = 0x39C7U, /**< Thin horizontal section rule.           */
  k_ovl_text        = 0xCE59U, /**< Light-grey body text.                   */
  k_ovl_dim         = 0x8410U, /**< Dim label text (field names).           */
  k_ovl_heading     = 0xFFFFU, /**< White heading text.                     */
  k_ovl_accent      = 0x5D1FU, /**< Cyan-blue accent (title / headings).    */
  k_ovl_ok          = 0x3666U, /**< Green "running / ok" indicator.         */
  k_ovl_amber       = 0xFD20U, /**< Amber "paused / attention" indicator.   */
  k_ovl_console_bg  = 0x0841U, /**< Console panel background (near-black).   */
  k_ovl_console_txt = 0x07E6U, /**< Console text (terminal green).          */
  k_ovl_console_new = 0xFFFFU, /**< Newest console line (white highlight).  */
  k_ovl_led_off     = 0x2104U, /**< Unlit LED dot.                          */
  k_ovl_led_ring    = 0x6B4DU, /**< LED dot outline.                        */
  k_ovl_btn_border  = 0x8430U, /**< On-screen button outline.               */
  k_ovl_btn_up      = 0x3186U, /**< Button face, released.                  */
  k_ovl_btn_down    = 0x05E0U, /**< Button face, pressed (green).           */
  k_ovl_btn_label   = 0xFFFFU, /**< Button caption text.                    */
  k_ovl_red         = 0xF800U, /**< Red gauge fill (low / critical SOC).    */
  k_ovl_glyph_w     = 5U,      /**< Font glyph width in pixels.             */
  k_ovl_glyph_h     = 7U,      /**< Font glyph height in pixels.            */
  k_ovl_glyph_first = 0x20U,   /**< First glyph in the font table (space).  */
  k_ovl_glyph_last  = 0x7EU,   /**< Last glyph in the font table (tilde).   */
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
  k_btn_x_dx    = 18,  /**< Button column inset from the sidebar origin.  */
  k_btn_head_y  = 404, /**< "BUTTONS" heading row.                        */
  k_btn_y       = 424, /**< Button face top.                              */
  k_btn_w       = 150, /**< Button face width.                            */
  k_btn_h       = 36,  /**< Button face height.                           */
  k_btn_gap     = 20,  /**< Horizontal gap between SW1 and SW2.           */
  k_btn_label_x = 14,  /**< Caption inset within the button face.         */
  k_btn_label_y = 12,  /**< Caption top inset within the button face.     */
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
} overlay_pwr_layout_t;

/** @brief Console-panel layout (the bottom scrolling UART log). */
typedef enum : int32_t {
  k_con_head_y = 480, /**< "CONSOLE" heading row.                        */
  k_con_y      = 496, /**< Console panel top.                            */
  k_con_pad    = 8,   /**< Inner padding inside the console panel.       */
  k_con_line_h = 10,  /**< Vertical step between console text lines.     */
  k_con_bottom = 14,  /**< Bottom margin below the console panel.        */
} overlay_con_layout_t;

/* 5x7 column-major font, ASCII 0x20..0x7E. Five bytes per glyph; bit b of a
 * column byte lights row b (0 = top). A public-domain 5x7 cell font. */
static const uint8_t k_font5x7[(k_ovl_glyph_last - k_ovl_glyph_first) + 1U][k_ovl_glyph_w] = {
  {0x00, 0x00, 0x00, 0x00, 0x00}, /* (space) */
  {0x00, 0x00, 0x5F, 0x00, 0x00}, /* ! */
  {0x00, 0x07, 0x00, 0x07, 0x00}, /* " */
  {0x14, 0x7F, 0x14, 0x7F, 0x14}, /* # */
  {0x24, 0x2A, 0x7F, 0x2A, 0x12}, /* $ */
  {0x23, 0x13, 0x08, 0x64, 0x62}, /* % */
  {0x36, 0x49, 0x55, 0x22, 0x50}, /* & */
  {0x00, 0x05, 0x03, 0x00, 0x00}, /* ' */
  {0x00, 0x1C, 0x22, 0x41, 0x00}, /* ( */
  {0x00, 0x41, 0x22, 0x1C, 0x00}, /* ) */
  {0x14, 0x08, 0x3E, 0x08, 0x14}, /* * */
  {0x08, 0x08, 0x3E, 0x08, 0x08}, /* + */
  {0x00, 0x50, 0x30, 0x00, 0x00}, /* , */
  {0x08, 0x08, 0x08, 0x08, 0x08}, /* - */
  {0x00, 0x60, 0x60, 0x00, 0x00}, /* . */
  {0x20, 0x10, 0x08, 0x04, 0x02}, /* / */
  {0x3E, 0x51, 0x49, 0x45, 0x3E}, /* 0 */
  {0x00, 0x42, 0x7F, 0x40, 0x00}, /* 1 */
  {0x42, 0x61, 0x51, 0x49, 0x46}, /* 2 */
  {0x21, 0x41, 0x45, 0x4B, 0x31}, /* 3 */
  {0x18, 0x14, 0x12, 0x7F, 0x10}, /* 4 */
  {0x27, 0x45, 0x45, 0x45, 0x39}, /* 5 */
  {0x3C, 0x4A, 0x49, 0x49, 0x30}, /* 6 */
  {0x01, 0x71, 0x09, 0x05, 0x03}, /* 7 */
  {0x36, 0x49, 0x49, 0x49, 0x36}, /* 8 */
  {0x06, 0x49, 0x49, 0x29, 0x1E}, /* 9 */
  {0x00, 0x36, 0x36, 0x00, 0x00}, /* : */
  {0x00, 0x56, 0x36, 0x00, 0x00}, /* ; */
  {0x08, 0x14, 0x22, 0x41, 0x00}, /* < */
  {0x14, 0x14, 0x14, 0x14, 0x14}, /* = */
  {0x00, 0x41, 0x22, 0x14, 0x08}, /* > */
  {0x02, 0x01, 0x51, 0x09, 0x06}, /* ? */
  {0x32, 0x49, 0x79, 0x41, 0x3E}, /* @ */
  {0x7E, 0x11, 0x11, 0x11, 0x7E}, /* A */
  {0x7F, 0x49, 0x49, 0x49, 0x36}, /* B */
  {0x3E, 0x41, 0x41, 0x41, 0x22}, /* C */
  {0x7F, 0x41, 0x41, 0x22, 0x1C}, /* D */
  {0x7F, 0x49, 0x49, 0x49, 0x41}, /* E */
  {0x7F, 0x09, 0x09, 0x09, 0x01}, /* F */
  {0x3E, 0x41, 0x49, 0x49, 0x7A}, /* G */
  {0x7F, 0x08, 0x08, 0x08, 0x7F}, /* H */
  {0x00, 0x41, 0x7F, 0x41, 0x00}, /* I */
  {0x20, 0x40, 0x41, 0x3F, 0x01}, /* J */
  {0x7F, 0x08, 0x14, 0x22, 0x41}, /* K */
  {0x7F, 0x40, 0x40, 0x40, 0x40}, /* L */
  {0x7F, 0x02, 0x0C, 0x02, 0x7F}, /* M */
  {0x7F, 0x04, 0x08, 0x10, 0x7F}, /* N */
  {0x3E, 0x41, 0x41, 0x41, 0x3E}, /* O */
  {0x7F, 0x09, 0x09, 0x09, 0x06}, /* P */
  {0x3E, 0x41, 0x51, 0x21, 0x5E}, /* Q */
  {0x7F, 0x09, 0x19, 0x29, 0x46}, /* R */
  {0x46, 0x49, 0x49, 0x49, 0x31}, /* S */
  {0x01, 0x01, 0x7F, 0x01, 0x01}, /* T */
  {0x3F, 0x40, 0x40, 0x40, 0x3F}, /* U */
  {0x1F, 0x20, 0x40, 0x20, 0x1F}, /* V */
  {0x7F, 0x20, 0x18, 0x20, 0x7F}, /* W */
  {0x63, 0x14, 0x08, 0x14, 0x63}, /* X */
  {0x03, 0x04, 0x78, 0x04, 0x03}, /* Y */
  {0x61, 0x51, 0x49, 0x45, 0x43}, /* Z */
  {0x00, 0x7F, 0x41, 0x41, 0x00}, /* [ */
  {0x02, 0x04, 0x08, 0x10, 0x20}, /* (backslash) */
  {0x00, 0x41, 0x41, 0x7F, 0x00}, /* ] */
  {0x04, 0x02, 0x01, 0x02, 0x04}, /* ^ */
  {0x40, 0x40, 0x40, 0x40, 0x40}, /* _ */
  {0x00, 0x01, 0x02, 0x04, 0x00}, /* ` */
  {0x20, 0x54, 0x54, 0x54, 0x78}, /* a */
  {0x7F, 0x48, 0x44, 0x44, 0x38}, /* b */
  {0x38, 0x44, 0x44, 0x44, 0x20}, /* c */
  {0x38, 0x44, 0x44, 0x48, 0x7F}, /* d */
  {0x38, 0x54, 0x54, 0x54, 0x18}, /* e */
  {0x08, 0x7E, 0x09, 0x01, 0x02}, /* f */
  {0x0C, 0x52, 0x52, 0x52, 0x3E}, /* g */
  {0x7F, 0x08, 0x04, 0x04, 0x78}, /* h */
  {0x00, 0x44, 0x7D, 0x40, 0x00}, /* i */
  {0x20, 0x40, 0x44, 0x3D, 0x00}, /* j */
  {0x7F, 0x10, 0x28, 0x44, 0x00}, /* k */
  {0x00, 0x41, 0x7F, 0x40, 0x00}, /* l */
  {0x7C, 0x04, 0x18, 0x04, 0x78}, /* m */
  {0x7C, 0x08, 0x04, 0x04, 0x78}, /* n */
  {0x38, 0x44, 0x44, 0x44, 0x38}, /* o */
  {0x7C, 0x14, 0x14, 0x14, 0x08}, /* p */
  {0x08, 0x14, 0x14, 0x18, 0x7C}, /* q */
  {0x7C, 0x08, 0x04, 0x04, 0x08}, /* r */
  {0x48, 0x54, 0x54, 0x54, 0x20}, /* s */
  {0x04, 0x3F, 0x44, 0x40, 0x20}, /* t */
  {0x3C, 0x40, 0x40, 0x20, 0x7C}, /* u */
  {0x1C, 0x20, 0x40, 0x20, 0x1C}, /* v */
  {0x3C, 0x40, 0x30, 0x40, 0x3C}, /* w */
  {0x44, 0x28, 0x10, 0x28, 0x44}, /* x */
  {0x0C, 0x50, 0x50, 0x50, 0x3C}, /* y */
  {0x44, 0x64, 0x54, 0x4C, 0x44}, /* z */
  {0x00, 0x08, 0x36, 0x41, 0x00}, /* { */
  {0x00, 0x00, 0x7F, 0x00, 0x00}, /* | */
  {0x00, 0x41, 0x36, 0x08, 0x00}, /* } */
  {0x08, 0x04, 0x08, 0x10, 0x08}, /* ~ */
};

uint16_t board_overlay_sidebar_width(void)
{
  return (uint16_t)k_ovl_sidebar_w;
}

uint16_t board_overlay_total_width(uint16_t panel_w)
{
  return (uint16_t)(panel_w + (uint16_t)k_ovl_sidebar_w);
}

uint16_t board_overlay_total_height(uint16_t panel_h)
{
  return (panel_h > (uint16_t)k_ovl_min_h) ? panel_h : (uint16_t)k_ovl_min_h;
}

/** @brief Plot one pixel if it lies inside the @p w by @p h composite. */
static void px(uint16_t* out, uint16_t w, uint16_t h, int32_t x, int32_t y, uint16_t color)
{
  if ((x >= 0) && (y >= 0) && (x < (int32_t)w) && (y < (int32_t)h)) {
    out[(size_t)y * (size_t)w + (size_t)x] = color;
  }
}

/** @brief Fill the axis-aligned rectangle [x,x+rw) x [y,y+rh) with @p color. */
static void fill_rect(uint16_t* out,
                      uint16_t  w,
                      uint16_t  h,
                      int32_t   x,
                      int32_t   y,
                      int32_t   rw,
                      int32_t   rh,
                      uint16_t  color)
{
  for (int32_t yy = 0; yy < rh; yy++) {
    for (int32_t xx = 0; xx < rw; xx++) {
      px(out, w, h, x + xx, y + yy, color);
    }
  }
}

/** @brief Draw one ASCII glyph at scale @p sc; non-printable maps to space. */
static void draw_glyph(uint16_t* out,
                       uint16_t  w,
                       uint16_t  h,
                       int32_t   x,
                       int32_t   y,
                       char      ch,
                       uint16_t  color,
                       int32_t   sc)
{
  uint8_t c = (uint8_t)ch;
  if ((c < (uint8_t)k_ovl_glyph_first) || (c > (uint8_t)k_ovl_glyph_last)) {
    c = (uint8_t)k_ovl_glyph_first;
  }
  const uint8_t* g = k_font5x7[c - (uint8_t)k_ovl_glyph_first];
  for (uint32_t col = 0U; col < (uint32_t)k_ovl_glyph_w; col++) {
    for (uint32_t row = 0U; row < (uint32_t)k_ovl_glyph_h; row++) {
      if (((g[col] >> row) & 1U) != 0U) {
        fill_rect(out, w, h, x + ((int32_t)col * sc), y + ((int32_t)row * sc), sc, sc, color);
      }
    }
  }
}

/** @brief Draw a NUL-terminated string; returns the x just past the last cell. */
static int32_t draw_text(uint16_t*   out,
                         uint16_t    w,
                         uint16_t    h,
                         int32_t     x,
                         int32_t     y,
                         const char* s,
                         uint16_t    color,
                         int32_t     sc)
{
  const int32_t adv = ((int32_t)k_ovl_glyph_w + 1) * sc;
  int32_t       cx  = x;
  for (uint32_t i = 0U; (s != nullptr) && (s[i] != '\0'); i++) {
    draw_glyph(out, w, h, cx, y, s[i], color, sc);
    cx += adv;
  }
  return cx;
}

/** @brief Draw a section heading (accent colour) with a thin rule beneath it. */
static int32_t
section_head(uint16_t* out, uint16_t w, uint16_t h, int32_t x, int32_t y, const char* title)
{
  draw_text(out, w, h, x, y, title, (uint16_t)k_ovl_accent, 1);
  fill_rect(out,
            w,
            h,
            x,
            y + 10,
            (int32_t)k_ovl_sidebar_w - (2 * (int32_t)k_pad_x),
            1,
            (uint16_t)k_ovl_rule);
  return y + (int32_t)k_heading_gap;
}

/** @brief Draw a "label  value" row: dim label, bright value, returns next y. */
static int32_t kv_row(uint16_t*   out,
                      uint16_t    w,
                      uint16_t    h,
                      int32_t     x,
                      int32_t     y,
                      const char* label,
                      const char* value,
                      uint16_t    val_color)
{
  const int32_t adv      = ((int32_t)k_ovl_glyph_w + 1);
  const int32_t value_dx = 7 * adv; /* fixed label column so values align */
  draw_text(out, w, h, x, y, label, (uint16_t)k_ovl_dim, 1);
  draw_text(out, w, h, x + value_dx, y, value, val_color, 1);
  return y + (int32_t)k_row_step;
}

/** @brief Blit the panel framebuffer into the composite's top-left region. */
static void blit_panel(uint16_t*       out,
                       uint16_t        w,
                       uint16_t        h,
                       const uint16_t* panel,
                       uint16_t        panel_w,
                       uint16_t        panel_h)
{
  for (uint16_t y = 0U; y < h; y++) {
    for (uint16_t x = 0U; x < panel_w; x++) {
      const bool     in_panel = (panel != nullptr) && (y < panel_h);
      const uint16_t c =
        in_panel ? panel[(size_t)y * (size_t)panel_w + (size_t)x] : (uint16_t)k_ovl_panel_bg;
      out[(size_t)y * (size_t)w + (size_t)x] = c;
    }
  }
}

/** @brief Draw one LED indicator dot (filled when on) plus its caption. */
static void
draw_led(uint16_t* out, uint16_t w, uint16_t h, int32_t x, int32_t y, const board_led_status_t* led)
{
  enum : int32_t {
    k_dot = 18, /**< LED dot side in pixels. */
  };
  fill_rect(out, w, h, x - 1, y - 1, k_dot + 2, k_dot + 2, (uint16_t)k_ovl_led_ring);
  const uint16_t fill = led->on ? led->color : (uint16_t)k_ovl_led_off;
  fill_rect(out, w, h, x, y, k_dot, k_dot, fill);
  draw_text(out,
            w,
            h,
            x + k_dot + 8,
            y + k_led_label_dy,
            (led->label != nullptr) ? led->label : "LED",
            (uint16_t)k_ovl_text,
            1);
}

/** @brief Paint the three LED indicators in a row; returns the next free y. */
static int32_t
draw_leds(uint16_t* out, uint16_t w, uint16_t h, int32_t x, int32_t y, const board_status_t* st)
{
  const int32_t row_y = section_head(out, w, h, x, y, "LEDS");
  for (uint32_t i = 0U; i < (uint32_t)k_overlay_led_count; i++) {
    draw_led(out, w, h, x + ((int32_t)i * k_led_col_dx), row_y, &st->leds[i]);
  }
  return row_y + k_led_row_h;
}

/** @brief Paint the run-stats block (PC / chunks / MMIO / state); next free y. */
static int32_t draw_run_stats(uint16_t*             out,
                              uint16_t              w,
                              uint16_t              h,
                              int32_t               x,
                              int32_t               y,
                              const board_status_t* st)
{
  char    buf[64];
  int32_t cy = section_head(out, w, h, x, y, "RUN");
  (void)snprintf(buf, sizeof(buf), "0x%08X", st->pc);
  cy = kv_row(out, w, h, x, cy, "pc", buf, (uint16_t)k_ovl_heading);
  (void)snprintf(buf, sizeof(buf), "%s   chunk %u", st->running ? "RUNNING" : "parked", st->chunks);
  cy =
    kv_row(out, w, h, x, cy, "state", buf, st->running ? (uint16_t)k_ovl_ok : (uint16_t)k_ovl_dim);
  (void)snprintf(buf, sizeof(buf), "%u rd / %u wr", st->mmio_reads, st->mmio_writes);
  cy = kv_row(out, w, h, x, cy, "mmio", buf, (uint16_t)k_ovl_text);
  return cy;
}

/** @brief Paint the I/O block (USB / IRQ / touch / SD); returns next free y. */
static int32_t
draw_io_block(uint16_t* out, uint16_t w, uint16_t h, int32_t x, int32_t y, const board_status_t* st)
{
  char    buf[96];
  int32_t cy = section_head(out, w, h, x, y, "I/O");
  cy         = kv_row(out,
                      w,
                      h,
                      x,
                      cy,
                      "usb",
                      (st->usb_state != nullptr) ? st->usb_state : "-",
                      (uint16_t)k_ovl_text);
  (void)
    snprintf(buf, sizeof(buf), "%u total  IRQ0 x%u  IRQ1 x%u", st->irq_total, st->irq0, st->irq1);
  cy = kv_row(out, w, h, x, cy, "irq", buf, (uint16_t)k_ovl_text);
  if (st->has_touch) {
    (void)snprintf(buf, sizeof(buf), "%u, %u", st->touch_x, st->touch_y);
  } else {
    (void)snprintf(buf, sizeof(buf), "-");
  }
  cy                        = kv_row(out, w, h, x, cy, "touch", buf, (uint16_t)k_ovl_text);
  const bool          sd_gb = (st->sd_bytes >= (1024ULL * 1024ULL * 1024ULL));
  const unsigned long sd_sz = sd_gb ? (unsigned long)(st->sd_bytes / (1024ULL * 1024ULL * 1024ULL))
                                    : (unsigned long)(st->sd_bytes / (1024ULL * 1024ULL));
  const char*         sd_u  = sd_gb ? "GB" : "MB";
  if (!st->sd_attached) {
    (void)snprintf(buf, sizeof(buf), "-");
  } else if (st->sd_fat_bits != 0U) {
    (void)snprintf(buf,
                   sizeof(buf),
                   "%lu %s FAT%u %s",
                   sd_sz,
                   sd_u,
                   (unsigned)st->sd_fat_bits,
                   (st->sd_label != nullptr) ? st->sd_label : "");
  } else {
    (void)snprintf(buf, sizeof(buf), "%lu %s (image)", sd_sz, sd_u);
  }
  cy = kv_row(out, w, h, x, cy, "sd", buf, (uint16_t)k_ovl_text);
  return cy;
}

/** @brief Paint the scrolling console panel (newest line at the bottom). */
static void draw_console(uint16_t* out, uint16_t w, uint16_t h, int32_t x, const board_status_t* st)
{
  char     buf[80];
  uint16_t head_col;
  if (!st->console_autoscroll) {
    /* Paused: amber heading + how far back the held view sits and how to resume. */
    (void)snprintf(buf,
                   sizeof(buf),
                   "CONSOLE  PAUSED -%u/%u  click=follow",
                   st->console_scroll,
                   st->console_total);
    head_col = (uint16_t)k_ovl_amber;
  } else {
    (void)snprintf(buf, sizeof(buf), "CONSOLE  LIVE  (UART %u B, wheel=scroll)", st->uart_tx_total);
    head_col = (uint16_t)k_ovl_ok;
  }
  draw_text(out, w, h, x, (int32_t)k_con_head_y, buf, head_col, 1);
  fill_rect(out,
            w,
            h,
            x,
            (int32_t)k_con_head_y + 10,
            (int32_t)k_ovl_sidebar_w - (2 * (int32_t)k_pad_x),
            1,
            (uint16_t)k_ovl_rule);
  const int32_t panel_x = x;
  const int32_t panel_w = (int32_t)k_ovl_sidebar_w - (2 * (int32_t)k_pad_x);
  const int32_t panel_h = (int32_t)h - (int32_t)k_con_y - (int32_t)k_con_bottom;
  if (panel_h < (int32_t)k_con_line_h) {
    return;
  }
  fill_rect(out, w, h, panel_x, (int32_t)k_con_y, panel_w, panel_h, (uint16_t)k_ovl_console_bg);
  /* Fit as many lines as the panel height allows; show the newest at the bottom
   * so the console scrolls upward like a terminal. console[0] is the newest. */
  const int32_t max_rows = (panel_h - (2 * (int32_t)k_con_pad)) / (int32_t)k_con_line_h;
  int32_t       rows     = (int32_t)st->console_count;
  if (rows > max_rows) {
    rows = max_rows;
  }
  for (int32_t i = 0; i < rows; i++) {
    /* Bottom row (i=0 from the bottom) is console[0], the newest line. */
    const int32_t ly =
      (int32_t)k_con_y + panel_h - (int32_t)k_con_pad - ((i + 1) * (int32_t)k_con_line_h);
    const uint16_t col = (i == 0) ? (uint16_t)k_ovl_console_new : (uint16_t)k_ovl_console_txt;
    draw_text(out, w, h, panel_x + (int32_t)k_con_pad, ly, st->console[i], col, 1);
  }
}

/** @brief Draw one labelled push-button face at @p x (green when @p pressed). */
static void
draw_button(uint16_t* out, uint16_t w, uint16_t h, int32_t x, const char* label, bool pressed)
{
  const uint16_t face = pressed ? (uint16_t)k_ovl_btn_down : (uint16_t)k_ovl_btn_up;
  fill_rect(out,
            w,
            h,
            x - 1,
            (int32_t)k_btn_y - 1,
            (int32_t)k_btn_w + 2,
            (int32_t)k_btn_h + 2,
            (uint16_t)k_ovl_btn_border);
  fill_rect(out, w, h, x, (int32_t)k_btn_y, (int32_t)k_btn_w, (int32_t)k_btn_h, face);
  draw_text(out,
            w,
            h,
            x + (int32_t)k_btn_label_x,
            (int32_t)k_btn_y + (int32_t)k_btn_label_y,
            label,
            (uint16_t)k_ovl_btn_label,
            2);
}

/** @brief Paint the "BUTTONS" heading and the clickable SW1 / SW2 buttons. */
static void draw_buttons(uint16_t* out, uint16_t w, uint16_t h, int32_t x, const board_status_t* st)
{
  (void)section_head(out, w, h, x, (int32_t)k_btn_head_y, "BUTTONS  (click)");
  draw_button(out, w, h, x, "SW1", st->sw1_pressed);
  draw_button(out, w, h, x + (int32_t)k_btn_w + (int32_t)k_btn_gap, "SW2", st->sw2_pressed);
}

/** @brief Clamp a SOC value to 0..100 for display. */
static uint8_t battery_clamp(uint8_t soc)
{
  return (soc > (uint8_t)k_pwr_soc_full) ? (uint8_t)k_pwr_soc_full : soc;
}

/** @brief Pick the battery gauge fill colour: green charging, else red/amber/green by level. */
static uint16_t battery_fill_color(uint8_t soc, bool charging)
{
  if (charging) {
    return (uint16_t)k_ovl_ok; /* charging always reads green. */
  }
  if (soc <= (uint8_t)k_pwr_soc_low) {
    return (uint16_t)k_ovl_red;
  }
  if (soc <= (uint8_t)k_pwr_soc_mid) {
    return (uint16_t)k_ovl_amber;
  }
  return (uint16_t)k_ovl_ok;
}

/**
 * @brief Paint the POWER section: a drag-to-set battery slider and a CHG toggle.
 *
 * @details The slider track is a bordered bar whose left portion is filled to the
 * SOC fraction in the level colour (red <=20%, amber <=50%, green above, or green
 * while charging); the "NN%" reading is drawn over it. To the right, a CHG button
 * shows the charge state -- green "CHG +" while charging, dim "BATT" on battery.
 * The geometry matches ::board_overlay_hit_button and
 * ::board_overlay_battery_pct_at so a click drags the percent or toggles charge.
 *
 * @param[out] out Composite buffer.
 * @param[in]  w   Composite width in pixels.
 * @param[in]  h   Composite height in pixels.
 * @param[in]  x   Section text origin (sidebar left + padding).
 * @param[in]  st  Live status snapshot (battery_soc / battery_charging).
 */
static void draw_power(uint16_t* out, uint16_t w, uint16_t h, int32_t x, const board_status_t* st)
{
  (void)section_head(out, w, h, x, (int32_t)k_pwr_head_y, "POWER  (drag)");
  const uint8_t  soc    = battery_clamp(st->battery_soc);
  const int32_t  fill_w = ((int32_t)k_pwr_track_w * (int32_t)soc) / (int32_t)k_pwr_soc_full;
  const uint16_t fill   = battery_fill_color(soc, st->battery_charging);
  /* Slider track: border, dark bed, then the SOC-proportional level fill. */
  fill_rect(out,
            w,
            h,
            x - 1,
            (int32_t)k_pwr_y - 1,
            (int32_t)k_pwr_track_w + 2,
            (int32_t)k_pwr_h + 2,
            (uint16_t)k_ovl_btn_border);
  fill_rect(out,
            w,
            h,
            x,
            (int32_t)k_pwr_y,
            (int32_t)k_pwr_track_w,
            (int32_t)k_pwr_h,
            (uint16_t)k_ovl_btn_up);
  fill_rect(out, w, h, x, (int32_t)k_pwr_y, fill_w, (int32_t)k_pwr_h, fill);
  char buf[16];
  (void)snprintf(buf, sizeof(buf), "%u%%", (unsigned)soc);
  draw_text(out,
            w,
            h,
            x + (int32_t)k_btn_label_x,
            (int32_t)k_pwr_y + (int32_t)k_pwr_label_y,
            buf,
            (uint16_t)k_ovl_heading,
            1);
  /* CHG toggle: green "CHG +" while charging, dim "BATT" on battery. */
  const int32_t  cx   = x + (int32_t)k_pwr_chg_off;
  const uint16_t face = st->battery_charging ? (uint16_t)k_ovl_btn_down : (uint16_t)k_ovl_btn_up;
  fill_rect(out,
            w,
            h,
            cx - 1,
            (int32_t)k_pwr_y - 1,
            (int32_t)k_pwr_chg_w + 2,
            (int32_t)k_pwr_h + 2,
            (uint16_t)k_ovl_btn_border);
  fill_rect(out, w, h, cx, (int32_t)k_pwr_y, (int32_t)k_pwr_chg_w, (int32_t)k_pwr_h, face);
  draw_text(out,
            w,
            h,
            cx + (int32_t)k_btn_label_x,
            (int32_t)k_pwr_y + (int32_t)k_pwr_label_y,
            st->battery_charging ? "CHG +" : "BATT",
            (uint16_t)k_ovl_btn_label,
            1);
}

/** @brief Paint the sidebar background, divider, title and all status sections. */
static void
draw_sidebar(uint16_t* out, uint16_t w, uint16_t h, uint16_t panel_w, const board_status_t* st)
{
  fill_rect(out,
            w,
            h,
            (int32_t)panel_w,
            0,
            (int32_t)k_ovl_sidebar_w,
            (int32_t)h,
            (uint16_t)k_ovl_bg);
  fill_rect(out, w, h, (int32_t)panel_w, 0, 3, (int32_t)h, (uint16_t)k_ovl_divider);
  if (st == nullptr) {
    return;
  }
  const int32_t x = (int32_t)panel_w + (int32_t)k_pad_x;
  int32_t       y = k_sidebar_top;
  /* Title bar: a filled accent band with the board name at 2x scale. */
  fill_rect(out,
            w,
            h,
            (int32_t)panel_w + 3,
            0,
            (int32_t)k_ovl_sidebar_w - 3,
            40,
            (uint16_t)k_ovl_bg_alt);
  draw_text(out, w, h, x, y, "EK-RA8D2  SIMULATOR", (uint16_t)k_ovl_accent, 2);
  y = 40 + (int32_t)k_section_gap;
  draw_text(out,
            w,
            h,
            x,
            y,
            (st->app_name != nullptr) ? st->app_name : "",
            (uint16_t)k_ovl_heading,
            1);
  y += (int32_t)k_title_gap;
  /* The RUN and LEDS sections sit on the upper sidebar; the layout below them
   * (I/O, BUTTONS, CONSOLE) uses fixed Y anchors so the console always fills the
   * bottom regardless of panel height. */
  y = draw_run_stats(out, w, h, x, y, st);
  y += (int32_t)k_section_gap;
  (void)draw_leds(out, w, h, x, y, st);
  (void)draw_io_block(out, w, h, x, (int32_t)k_io_head_y, st);
  draw_power(out, w, h, x, st);
  draw_buttons(out, w, h, x, st);
  draw_console(out, w, h, x, st);
}

void board_overlay_compose(uint16_t*             out,
                           const uint16_t*       panel,
                           uint16_t              panel_w,
                           uint16_t              panel_h,
                           const board_status_t* st)
{
  if ((out == nullptr) || (panel_w == 0U)) {
    return;
  }
  const uint16_t w = board_overlay_total_width(panel_w);
  const uint16_t h = board_overlay_total_height(panel_h);
  blit_panel(out, w, h, panel, panel_w, panel_h);
  draw_sidebar(out, w, h, panel_w, st);
}

board_overlay_btn_t board_overlay_hit_button(uint16_t x, uint16_t y, uint16_t panel_w)
{
  const int32_t cx       = (int32_t)x;
  const int32_t cy       = (int32_t)y;
  const int32_t side_lo  = (int32_t)panel_w;
  const int32_t side_hi  = (int32_t)panel_w + (int32_t)k_ovl_sidebar_w;
  const bool    in_sidex = (cx >= side_lo) && (cx < side_hi);
  /* SW1 / SW2 push-buttons. */
  if ((cy >= (int32_t)k_btn_y) && (cy < ((int32_t)k_btn_y + (int32_t)k_btn_h))) {
    const int32_t bx1 = (int32_t)panel_w + (int32_t)k_btn_x_dx;
    if ((cx >= bx1) && (cx < (bx1 + (int32_t)k_btn_w))) {
      return k_board_overlay_btn_sw1;
    }
    const int32_t bx2 = bx1 + (int32_t)k_btn_w + (int32_t)k_btn_gap;
    if ((cx >= bx2) && (cx < (bx2 + (int32_t)k_btn_w))) {
      return k_board_overlay_btn_sw2;
    }
  }
  /* POWER row: the battery slider track and the CHG toggle share one row. */
  if ((cy >= (int32_t)k_pwr_y) && (cy < ((int32_t)k_pwr_y + (int32_t)k_pwr_h))) {
    const int32_t sx = (int32_t)panel_w + (int32_t)k_pwr_x_dx;
    if ((cx >= sx) && (cx < (sx + (int32_t)k_pwr_track_w))) {
      return k_board_overlay_btn_battery;
    }
    const int32_t cgx = sx + (int32_t)k_pwr_chg_off;
    if ((cx >= cgx) && (cx < (cgx + (int32_t)k_pwr_chg_w))) {
      return k_board_overlay_btn_batt_chg;
    }
  }
  /* Console region (heading + panel): a click toggles autoscroll / jumps live,
   * like clicking the Arduino Serial Monitor's pane. */
  if (in_sidex && (cy >= (int32_t)k_con_head_y)) {
    return k_board_overlay_btn_console;
  }
  return k_board_overlay_btn_none;
}

bool board_overlay_battery_pct_at(uint16_t x, uint16_t panel_w, uint8_t* out_pct)
{
  if (out_pct == nullptr) {
    return false;
  }
  const int32_t cx = (int32_t)x;
  const int32_t sx = (int32_t)panel_w + (int32_t)k_pwr_x_dx;
  if (cx <= sx) {
    *out_pct = 0U;
  } else if (cx >= (sx + (int32_t)k_pwr_track_w)) {
    *out_pct = (uint8_t)k_pwr_soc_full;
  } else {
    *out_pct = (uint8_t)(((cx - sx) * (int32_t)k_pwr_soc_full) / (int32_t)k_pwr_track_w);
  }
  return true;
}
