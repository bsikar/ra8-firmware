/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file board_overlay_draw.c
 * @brief Overlay draw primitives + embedded 5x7 font
 *
 * @details
 * The pixel/rect/text/blit primitives and the public-domain 5x7 column-major
 * ASCII font every overlay panel draws with -- moved verbatim out of
 * board_overlay.c.
 *
 *
 * @since 0.1.0
 */

#include <string.h>

#include "board_overlay_internal.h"

/* 5x7 column-major font, ASCII 0x20..0x7E. Five bytes per glyph; bit b of a
 * column byte lights row b (0 = top). A public-domain 5x7 cell font. */
static const uint8_t k_font5x7[(k_ovl_glyph_last - k_ovl_glyph_first) + 1U][k_ovl_glyph_w] = {
  {0x00, 0x00, 0x00, 0x00, 0x00}, /* (space) */
  {0x00, 0x00, 0x5F, 0x00, 0x00}, /* !       */
  {0x00, 0x07, 0x00, 0x07, 0x00}, /* "       */
  {0x14, 0x7F, 0x14, 0x7F, 0x14}, /* #       */
  {0x24, 0x2A, 0x7F, 0x2A, 0x12}, /* $       */
  {0x23, 0x13, 0x08, 0x64, 0x62}, /* %       */
  {0x36, 0x49, 0x55, 0x22, 0x50}, /* &       */
  {0x00, 0x05, 0x03, 0x00, 0x00}, /* '       */
  {0x00, 0x1C, 0x22, 0x41, 0x00}, /* (       */
  {0x00, 0x41, 0x22, 0x1C, 0x00}, /* )       */
  {0x14, 0x08, 0x3E, 0x08, 0x14}, /* * */
  {0x08, 0x08, 0x3E, 0x08, 0x08}, /* +           */
  {0x00, 0x50, 0x30, 0x00, 0x00}, /* ,           */
  {0x08, 0x08, 0x08, 0x08, 0x08}, /* -           */
  {0x00, 0x60, 0x60, 0x00, 0x00}, /* .           */
  {0x20, 0x10, 0x08, 0x04, 0x02}, /* /           */
  {0x3E, 0x51, 0x49, 0x45, 0x3E}, /* 0           */
  {0x00, 0x42, 0x7F, 0x40, 0x00}, /* 1           */
  {0x42, 0x61, 0x51, 0x49, 0x46}, /* 2           */
  {0x21, 0x41, 0x45, 0x4B, 0x31}, /* 3           */
  {0x18, 0x14, 0x12, 0x7F, 0x10}, /* 4           */
  {0x27, 0x45, 0x45, 0x45, 0x39}, /* 5           */
  {0x3C, 0x4A, 0x49, 0x49, 0x30}, /* 6           */
  {0x01, 0x71, 0x09, 0x05, 0x03}, /* 7           */
  {0x36, 0x49, 0x49, 0x49, 0x36}, /* 8           */
  {0x06, 0x49, 0x49, 0x29, 0x1E}, /* 9           */
  {0x00, 0x36, 0x36, 0x00, 0x00}, /* :           */
  {0x00, 0x56, 0x36, 0x00, 0x00}, /* ;           */
  {0x08, 0x14, 0x22, 0x41, 0x00}, /* <           */
  {0x14, 0x14, 0x14, 0x14, 0x14}, /* =           */
  {0x00, 0x41, 0x22, 0x14, 0x08}, /* >           */
  {0x02, 0x01, 0x51, 0x09, 0x06}, /* ?           */
  {0x32, 0x49, 0x79, 0x41, 0x3E}, /* @           */
  {0x7E, 0x11, 0x11, 0x11, 0x7E}, /* A           */
  {0x7F, 0x49, 0x49, 0x49, 0x36}, /* B           */
  {0x3E, 0x41, 0x41, 0x41, 0x22}, /* C           */
  {0x7F, 0x41, 0x41, 0x22, 0x1C}, /* D           */
  {0x7F, 0x49, 0x49, 0x49, 0x41}, /* E           */
  {0x7F, 0x09, 0x09, 0x09, 0x01}, /* F           */
  {0x3E, 0x41, 0x49, 0x49, 0x7A}, /* G           */
  {0x7F, 0x08, 0x08, 0x08, 0x7F}, /* H           */
  {0x00, 0x41, 0x7F, 0x41, 0x00}, /* I           */
  {0x20, 0x40, 0x41, 0x3F, 0x01}, /* J           */
  {0x7F, 0x08, 0x14, 0x22, 0x41}, /* K           */
  {0x7F, 0x40, 0x40, 0x40, 0x40}, /* L           */
  {0x7F, 0x02, 0x0C, 0x02, 0x7F}, /* M           */
  {0x7F, 0x04, 0x08, 0x10, 0x7F}, /* N           */
  {0x3E, 0x41, 0x41, 0x41, 0x3E}, /* O           */
  {0x7F, 0x09, 0x09, 0x09, 0x06}, /* P           */
  {0x3E, 0x41, 0x51, 0x21, 0x5E}, /* Q           */
  {0x7F, 0x09, 0x19, 0x29, 0x46}, /* R           */
  {0x46, 0x49, 0x49, 0x49, 0x31}, /* S           */
  {0x01, 0x01, 0x7F, 0x01, 0x01}, /* T           */
  {0x3F, 0x40, 0x40, 0x40, 0x3F}, /* U           */
  {0x1F, 0x20, 0x40, 0x20, 0x1F}, /* V           */
  {0x7F, 0x20, 0x18, 0x20, 0x7F}, /* W           */
  {0x63, 0x14, 0x08, 0x14, 0x63}, /* X           */
  {0x03, 0x04, 0x78, 0x04, 0x03}, /* Y           */
  {0x61, 0x51, 0x49, 0x45, 0x43}, /* Z           */
  {0x00, 0x7F, 0x41, 0x41, 0x00}, /* [           */
  {0x02, 0x04, 0x08, 0x10, 0x20}, /* (backslash) */
  {0x00, 0x41, 0x41, 0x7F, 0x00}, /* ]           */
  {0x04, 0x02, 0x01, 0x02, 0x04}, /* ^           */
  {0x40, 0x40, 0x40, 0x40, 0x40}, /* _           */
  {0x00, 0x01, 0x02, 0x04, 0x00}, /* `           */
  {0x20, 0x54, 0x54, 0x54, 0x78}, /* a           */
  {0x7F, 0x48, 0x44, 0x44, 0x38}, /* b           */
  {0x38, 0x44, 0x44, 0x44, 0x20}, /* c           */
  {0x38, 0x44, 0x44, 0x48, 0x7F}, /* d           */
  {0x38, 0x54, 0x54, 0x54, 0x18}, /* e           */
  {0x08, 0x7E, 0x09, 0x01, 0x02}, /* f           */
  {0x0C, 0x52, 0x52, 0x52, 0x3E}, /* g           */
  {0x7F, 0x08, 0x04, 0x04, 0x78}, /* h           */
  {0x00, 0x44, 0x7D, 0x40, 0x00}, /* i           */
  {0x20, 0x40, 0x44, 0x3D, 0x00}, /* j           */
  {0x7F, 0x10, 0x28, 0x44, 0x00}, /* k           */
  {0x00, 0x41, 0x7F, 0x40, 0x00}, /* l           */
  {0x7C, 0x04, 0x18, 0x04, 0x78}, /* m           */
  {0x7C, 0x08, 0x04, 0x04, 0x78}, /* n           */
  {0x38, 0x44, 0x44, 0x44, 0x38}, /* o           */
  {0x7C, 0x14, 0x14, 0x14, 0x08}, /* p           */
  {0x08, 0x14, 0x14, 0x18, 0x7C}, /* q           */
  {0x7C, 0x08, 0x04, 0x04, 0x08}, /* r           */
  {0x48, 0x54, 0x54, 0x54, 0x20}, /* s           */
  {0x04, 0x3F, 0x44, 0x40, 0x20}, /* t           */
  {0x3C, 0x40, 0x40, 0x20, 0x7C}, /* u           */
  {0x1C, 0x20, 0x40, 0x20, 0x1C}, /* v           */
  {0x3C, 0x40, 0x30, 0x40, 0x3C}, /* w           */
  {0x44, 0x28, 0x10, 0x28, 0x44}, /* x           */
  {0x0C, 0x50, 0x50, 0x50, 0x3C}, /* y           */
  {0x44, 0x64, 0x54, 0x4C, 0x44}, /* z           */
  {0x00, 0x08, 0x36, 0x41, 0x00}, /* {           */
  {0x00, 0x00, 0x7F, 0x00, 0x00}, /* |           */
  {0x00, 0x41, 0x36, 0x08, 0x00}, /* }           */
  {0x08, 0x04, 0x08, 0x10, 0x08}, /* ~           */
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
void px(uint16_t* out, uint16_t w, uint16_t h, int32_t x, int32_t y, uint16_t color)
{
  if ((x >= 0) && (y >= 0) && (x < (int32_t)w) && (y < (int32_t)h)) {
    out[((size_t)y * (size_t)w) + (size_t)x] = color;
  }
}

/** @brief Fill the axis-aligned rectangle [x,x+rw) x [y,y+rh) with @p color. */
void fill_rect(uint16_t* out,
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
int32_t draw_text(uint16_t*   out,
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
int32_t section_head(uint16_t* out, uint16_t w, uint16_t h, int32_t x, int32_t y, const char* title)
{
  draw_text(out, w, h, x, y, title, (uint16_t)k_ovl_accent, 1);
  fill_rect(out,
            w,
            h,
            x,
            y + (int32_t)k_rule_dy,
            (int32_t)k_ovl_sidebar_w - (2 * (int32_t)k_pad_x),
            1,
            (uint16_t)k_ovl_rule);
  return y + (int32_t)k_heading_gap;
}

/** @brief Draw a "label  value" row: dim label, bright value, returns next y. */
int32_t kv_row(uint16_t*   out,
               uint16_t    w,
               uint16_t    h,
               int32_t     x,
               int32_t     y,
               const char* label,
               const char* value,
               uint16_t    val_color)
{
  const int32_t adv      = ((int32_t)k_ovl_glyph_w + 1);
  const int32_t value_dx = (int32_t)k_kv_label_cols * adv; /* fixed label column so values align */
  draw_text(out, w, h, x, y, label, (uint16_t)k_ovl_dim, 1);
  draw_text(out, w, h, x + value_dx, y, value, val_color, 1);
  return y + (int32_t)k_row_step;
}

/** @brief Blit the panel framebuffer into the composite's top-left region. */
void blit_panel(uint16_t*       out,
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
        in_panel ? panel[((size_t)y * (size_t)panel_w) + (size_t)x] : (uint16_t)k_ovl_panel_bg;
      out[((size_t)y * (size_t)w) + (size_t)x] = c;
    }
  }
}
