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
  k_led_label_dy = 5,   /**< LED label vertical offset within its dot. */
  k_led_row_dy   = 14,  /**< "LEDS" heading to the LED row.            */
  k_led_col_dx   = 110, /**< Horizontal spacing between LEDs.          */
  k_led_row_h    = 30,  /**< LED row height (advance to next block).   */
  k_sidebar_top  = 14,  /**< Sidebar top margin.                       */
  k_heading_gap  = 12,  /**< Heading to app-name line.                 */
  k_appname_gap  = 22,  /**< App-name line to the LED block.           */
  k_leds_gap     = 10,  /**< LED block to the status-text block.       */
} overlay_layout_t;

/** @brief Sidebar geometry and palette (RGB565). */
typedef enum : uint32_t {
  k_ovl_sidebar_w   = 360U,    /**< Sidebar width added on the right.       */
  k_ovl_min_h       = 320U,    /**< Minimum composite height.               */
  k_ovl_bg          = 0x18E3U, /**< Dark slate sidebar background.          */
  k_ovl_panel_bg    = 0x0000U, /**< Fill behind a missing/short panel.      */
  k_ovl_divider     = 0x4208U, /**< Vertical divider between panel/sidebar. */
  k_ovl_text        = 0xCE59U, /**< Light-grey body text.                   */
  k_ovl_heading     = 0xFFFFU, /**< White heading text.                     */
  k_ovl_led_off     = 0x2104U, /**< Unlit LED dot.                          */
  k_ovl_led_ring    = 0x6B4DU, /**< LED dot outline.                        */
  k_ovl_btn_border  = 0x8430U, /**< On-screen button outline.               */
  k_ovl_btn_up      = 0x3186U, /**< Button face, released.                  */
  k_ovl_btn_down    = 0x05E0U, /**< Button face, pressed (green).           */
  k_ovl_btn_label   = 0xFFFFU, /**< Button caption text.                    */
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
  k_btn_x_dx    = 16,  /**< Button column inset from the sidebar origin.  */
  k_btn_head_y  = 190, /**< "BUTTONS" heading row.                        */
  k_btn_y       = 206, /**< Button face top.                              */
  k_btn_w       = 150, /**< Button face width.                            */
  k_btn_h       = 36,  /**< Button face height.                           */
  k_btn_gap     = 16,  /**< Horizontal gap between SW1 and SW2.           */
  k_btn_label_x = 12,  /**< Caption inset within the button face.         */
  k_btn_label_y = 12,  /**< Caption top inset within the button face.     */
} overlay_btn_layout_t;

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
  draw_text(out, w, h, x, y, "LEDS", (uint16_t)k_ovl_heading, 1);
  const int32_t row_y = y + k_led_row_dy;
  for (uint32_t i = 0U; i < (uint32_t)k_overlay_led_count; i++) {
    draw_led(out, w, h, x + ((int32_t)i * k_led_col_dx), row_y, &st->leds[i]);
  }
  return row_y + k_led_row_h;
}

/** @brief Paint the USB / UART / IRQ / touch text block; returns next free y. */
static int32_t draw_status_lines(uint16_t*             out,
                                 uint16_t              w,
                                 uint16_t              h,
                                 int32_t               x,
                                 int32_t               y,
                                 const board_status_t* st)
{
  char          buf[96];
  const int32_t step = 16;
  int32_t       cy   = y;
  (void)snprintf(buf, sizeof(buf), "USB:  %s", (st->usb_state != nullptr) ? st->usb_state : "-");
  draw_text(out, w, h, x, cy, buf, (uint16_t)k_ovl_text, 1);
  cy += step;
  (void)snprintf(buf,
                 sizeof(buf),
                 "UART: %s",
                 ((st->uart_line != nullptr) && (st->uart_line[0] != '\0')) ? st->uart_line : "-");
  draw_text(out, w, h, x, cy, buf, (uint16_t)k_ovl_text, 1);
  cy += step;
  (void)snprintf(buf,
                 sizeof(buf),
                 "IRQ:  %u total  IRQ0 x%u  IRQ1 x%u",
                 st->irq_total,
                 st->irq0,
                 st->irq1);
  draw_text(out, w, h, x, cy, buf, (uint16_t)k_ovl_text, 1);
  cy += step;
  if (st->has_touch) {
    (void)snprintf(buf, sizeof(buf), "touch %u,%u", st->touch_x, st->touch_y);
  } else {
    (void)snprintf(buf, sizeof(buf), "touch -");
  }
  draw_text(out, w, h, x, cy, buf, (uint16_t)k_ovl_text, 1);
  cy += step;
  const bool          sd_gb = (st->sd_bytes >= (1024ULL * 1024ULL * 1024ULL));
  const unsigned long sd_sz = sd_gb ? (unsigned long)(st->sd_bytes / (1024ULL * 1024ULL * 1024ULL))
                                    : (unsigned long)(st->sd_bytes / (1024ULL * 1024ULL));
  const char*         sd_u  = sd_gb ? "GB" : "MB";
  if (!st->sd_attached) {
    (void)snprintf(buf, sizeof(buf), "SD:   -");
  } else if (st->sd_fat_bits != 0U) {
    (void)snprintf(buf,
                   sizeof(buf),
                   "SD:   %lu %s FAT%u %s",
                   sd_sz,
                   sd_u,
                   (unsigned)st->sd_fat_bits,
                   (st->sd_label != nullptr) ? st->sd_label : "");
  } else {
    (void)snprintf(buf, sizeof(buf), "SD:   %lu %s (image)", sd_sz, sd_u);
  }
  draw_text(out, w, h, x, cy, buf, (uint16_t)k_ovl_text, 1);
  return cy + step;
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
  draw_text(out, w, h, x, (int32_t)k_btn_head_y, "BUTTONS (click)", (uint16_t)k_ovl_heading, 1);
  draw_button(out, w, h, x, "SW1", st->sw1_pressed);
  draw_button(out, w, h, x + (int32_t)k_btn_w + (int32_t)k_btn_gap, "SW2", st->sw2_pressed);
}

/** @brief Paint the sidebar background, divider, heading and all status rows. */
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
  fill_rect(out, w, h, (int32_t)panel_w, 0, 2, (int32_t)h, (uint16_t)k_ovl_divider);
  if (st == nullptr) {
    return;
  }
  const int32_t x = (int32_t)panel_w + 16;
  int32_t       y = k_sidebar_top;
  draw_text(out, w, h, x, y, "EK-RA8D2 BOARD VIEW", (uint16_t)k_ovl_heading, 1);
  y += k_heading_gap;
  draw_text(out,
            w,
            h,
            x,
            y,
            (st->app_name != nullptr) ? st->app_name : "",
            (uint16_t)k_ovl_text,
            1);
  y += k_appname_gap;
  y = draw_leds(out, w, h, x, y, st);
  y += k_leds_gap;
  (void)draw_status_lines(out, w, h, x, y, st);
  draw_buttons(out, w, h, x, st);
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
  const int32_t cx = (int32_t)x;
  const int32_t cy = (int32_t)y;
  if ((cy < (int32_t)k_btn_y) || (cy >= ((int32_t)k_btn_y + (int32_t)k_btn_h))) {
    return k_board_overlay_btn_none;
  }
  const int32_t bx1 = (int32_t)panel_w + (int32_t)k_btn_x_dx;
  if ((cx >= bx1) && (cx < (bx1 + (int32_t)k_btn_w))) {
    return k_board_overlay_btn_sw1;
  }
  const int32_t bx2 = bx1 + (int32_t)k_btn_w + (int32_t)k_btn_gap;
  if ((cx >= bx2) && (cx < (bx2 + (int32_t)k_btn_w))) {
    return k_board_overlay_btn_sw2;
  }
  return k_board_overlay_btn_none;
}
