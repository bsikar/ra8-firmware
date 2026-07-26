/**
 * @file examples/ek_ra8d2/hil_needs_revalidation/ereader_shelf/src/sh_toc.c
 * @brief Table-of-contents screen: scrollable chapter list for the open book.
 *
 * @details
 * Lists the open book's spine chapters using their `ra8_book` TOC labels (falling
 * back to "Chapter N" for unlabelled spine items). The list scrolls a page at a
 * time via SW1/SW2; tapping a visible row opens the reader at that chapter. The
 * window of visible rows is derived from the panel height and ::k_sh_toc_row_h.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * [Ring 6 / App] {World: NS}
 *
 * @since 0.1.0
 */
#include "ra8_gfx.h"
#include "ra8_gfx_font.h"
#include "sh_app.h"

/** @enum sh_toc_const_t @brief TOC layout constants. */
typedef enum : int32_t {
  k_sh_toc_text_pad = 14, /**< Row text inset.              */
  k_sh_toc_num_w    = 64, /**< Chapter-number column width. */
} sh_toc_const_t;

/** @brief Rows of the chapter list visible at once. */
static int32_t sh_toc_visible_rows(void)
{
  return ((int32_t)k_sh_fb_h - (int32_t)k_sh_bar_h) / (int32_t)k_sh_toc_row_h;
}

/** @brief Resolve chapter @p idx's display label into @p buf (cap bytes), truncated. */
static void sh_toc_label(uint32_t idx, char* buf, size_t cap)
{
  char full[k_sh_linebuf];
  sh_book_chapter_label(idx, full, sizeof full);
  sh_fit(buf, cap, full, sh_cells((int32_t)k_sh_fb_w - (2 * (int32_t)k_sh_pad) - k_sh_toc_num_w));
}

void sh_toc_render(void)
{
  (void)ra8_gfx_clear((uint32_t)k_sh_col_card);
  sh_titlebar("< Cover", g_sh.entry[g_sh.selected].title);
  const int32_t rows = sh_toc_visible_rows();
  for (int32_t r = 0; r < rows; ++r) {
    const uint32_t ci = (uint32_t)(g_sh.toc_scroll + r);
    if (ci >= g_sh.chapter_count) {
      break;
    }
    const int32_t  ry  = (int32_t)k_sh_bar_h + (r * (int32_t)k_sh_toc_row_h);
    const bool     cur = (ci == g_sh.chapter);
    const uint32_t bg  = cur ? (uint32_t)k_sh_col_rowhi : (uint32_t)k_sh_col_card;
    (void)ra8_gfx_rect(0, ry, (int32_t)k_sh_fb_w, (int32_t)k_sh_toc_row_h, bg, true);
    (void)ra8_gfx_rect(0,
                       ry + (int32_t)k_sh_toc_row_h - 1,
                       (int32_t)k_sh_fb_w,
                       1,
                       (uint32_t)k_sh_col_edge,
                       true);
    char   num[k_sh_dec_base];
    size_t np        = sh_fmt_uint(num, 0U, ci + 1U);
    num[np]          = '\0';
    const int32_t ty = ry + (((int32_t)k_sh_toc_row_h - (int32_t)k_sh_glyph_h) / 2);
    (void)
      ra8_gfx_text_out(k_sh_toc_text_pad, ty, num, &ra8_gfx_font_8x16, (uint32_t)k_sh_col_sub, bg);
    char label[k_sh_linebuf];
    sh_toc_label(ci, label, sizeof label);
    (void)ra8_gfx_text_out(k_sh_toc_text_pad + k_sh_toc_num_w,
                           ty,
                           label,
                           &ra8_gfx_font_8x16,
                           (uint32_t)k_sh_col_ink,
                           bg);
  }
}

int32_t sh_toc_hit(int32_t x, int32_t y)
{
  (void)x;
  if (y < (int32_t)k_sh_bar_h) {
    return -1;
  }
  const int32_t  row = (y - (int32_t)k_sh_bar_h) / (int32_t)k_sh_toc_row_h;
  const uint32_t ci  = (uint32_t)(g_sh.toc_scroll + row);
  return (ci < g_sh.chapter_count) ? (int32_t)ci : -1;
}

void sh_toc_scroll(int32_t dir)
{
  const int32_t rows = sh_toc_visible_rows();
  int32_t       top  = g_sh.toc_scroll + (dir * rows);
  const int32_t max  = (int32_t)g_sh.chapter_count - rows;
  if (top > max) {
    top = max;
  }
  if (top < 0) {
    top = 0;
  }
  g_sh.toc_scroll = top;
}
