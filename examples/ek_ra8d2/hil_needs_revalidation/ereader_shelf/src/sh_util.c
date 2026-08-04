/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file examples/ek_ra8d2/hil_needs_revalidation/ereader_shelf/src/sh_util.c
 * @brief Small shared drawing + formatting helpers for ereader_shelf.
 *
 * @details
 * Text/measure/format primitives every screen reuses: bitmap-font cell math,
 * ellipsis truncation, decimal formatting, rectangle outlines, and the common
 * header bar. All drawing goes through `ra8_gfx` into the bound framebuffer.
 *
 *
 * [Ring 6 / App] {World: NS}
 *
 * @since 0.1.0
 */
#include <string.h>

#include "ra8_gfx.h"
#include "ra8_gfx_font.h"
#include "sh_app.h"

/** @enum sh_util_const_t @brief Local layout constants. */
typedef enum : uint32_t {
  k_sh_ellipsis_min = 2U, /**< Chars kept before the ".." truncation. */
} sh_util_const_t;

int32_t sh_cells(int32_t pixels)
{
  return pixels / (int32_t)k_sh_glyph_w;
}

void sh_fit(char* dst, size_t cap, const char* src, int32_t max_chars)
{
  size_t limit = (max_chars < 1) ? 1U : (size_t)max_chars;
  if (limit >= cap) {
    limit = cap - 1U;
  }
  const size_t len = strlen(src);
  if (len <= limit) {
    memcpy(dst, src, len);
    dst[len] = '\0';
    return;
  }
  const size_t keep = (limit < k_sh_ellipsis_min) ? 0U : (limit - k_sh_ellipsis_min);
  memcpy(dst, src, keep);
  dst[keep]      = '.';
  dst[keep + 1U] = '.';
  dst[keep + 2U] = '\0';
}

size_t sh_fmt_uint(char* dst, size_t pos, uint32_t v)
{
  char     tmp[k_sh_dec_base];
  uint32_t n = 0U;
  if (v == 0U) {
    tmp[n++] = '0';
  }
  while ((v > 0U) && (n < (uint32_t)k_sh_dec_base)) {
    tmp[n++] = (char)('0' + (v % (uint32_t)k_sh_dec_base));
    v /= (uint32_t)k_sh_dec_base;
  }
  for (uint32_t k = 0U; k < n; ++k) {
    dst[pos++] = tmp[n - 1U - k];
  }
  return pos;
}

void sh_border(ra8_ui_rect_t r, uint32_t colour, int32_t width)
{
  for (int32_t i = 0; i < width; ++i) {
    (void)ra8_gfx_rect(r.x + i, r.y + i, r.w - (2 * i), r.h - (2 * i), colour, false);
  }
}

void sh_titlebar(const char* title, const char* right)
{
  (void)ra8_gfx_rect(0, 0, (int32_t)k_sh_fb_w, (int32_t)k_sh_bar_h, (uint32_t)k_sh_col_bar, true);
  (void)ra8_gfx_text_out((int32_t)k_sh_pad,
                         (int32_t)((k_sh_bar_h - k_sh_glyph_h) / 2U),
                         title,
                         &ra8_gfx_font_8x16,
                         (uint32_t)k_sh_col_barfg,
                         (uint32_t)k_sh_col_bar);
  if (right != nullptr) {
    const int32_t rx =
      (int32_t)k_sh_fb_w - (int32_t)k_sh_pad - ((int32_t)strlen(right) * (int32_t)k_sh_glyph_w);
    (void)ra8_gfx_text_out(rx,
                           (int32_t)((k_sh_bar_h - k_sh_glyph_h) / 2U),
                           right,
                           &ra8_gfx_font_8x16,
                           (uint32_t)k_sh_col_barfg,
                           (uint32_t)k_sh_col_bar);
  }
}
