/**
 * @file examples/ek_ra8d2/hil_needs_revalidation/ereader_shelf/src/sh_cover.c
 * @brief Cover / title page for the open book.
 *
 * @details
 * Renders the open book's full cover (decoded straight from its 4bpp image pool)
 * on the left, with the title, author, and two action buttons -- "Read this
 * book" and "Table of Contents" -- on the right. Tapping the cover or the read
 * button starts the reader; the contents button opens the TOC.
 *
 *
 * [Ring 6 / App] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */
#include "ra8_gfx.h"
#include "ra8_gfx_font.h"
#include "sh_app.h"

/** @enum sh_cover_layout_t @brief Cover-page geometry (framebuffer pixels). */
typedef enum : int32_t {
  k_sh_cv_box_x   = 32,  /**< Cover box left.        */
  k_sh_cv_box_y   = 80,  /**< Cover box top.         */
  k_sh_cv_box_w   = 340, /**< Cover box width.       */
  k_sh_cv_box_h   = 490, /**< Cover box height.      */
  k_sh_cv_panel_x = 420, /**< Right text panel left. */
  k_sh_cv_title_y = 110, /**< Title baseline.        */
  k_sh_cv_auth_y  = 150, /**< Author baseline.       */
  k_sh_cv_btn_w   = 360, /**< Action button width.   */
  k_sh_cv_btn_h   = 52,  /**< Action button height.  */
  k_sh_cv_read_y  = 250, /**< "Read" button top.     */
  k_sh_cv_toc_y   = 326, /**< "Contents" button top. */
  k_sh_cv_btn_pad = 16,  /**< Button text inset.     */
} sh_cover_layout_t;

/** @brief Draw a filled action button with a left-inset label. */
static void sh_cover_button(int32_t y, const char* label)
{
  (void)
    ra8_gfx_rect(k_sh_cv_panel_x, y, k_sh_cv_btn_w, k_sh_cv_btn_h, (uint32_t)k_sh_col_bar, true);
  (void)ra8_gfx_text_out(k_sh_cv_panel_x + k_sh_cv_btn_pad,
                         y + ((k_sh_cv_btn_h - (int32_t)k_sh_glyph_h) / 2),
                         label,
                         &ra8_gfx_font_8x16,
                         (uint32_t)k_sh_col_barfg,
                         (uint32_t)k_sh_col_bar);
}

void sh_cover_render(void)
{
  (void)ra8_gfx_clear((uint32_t)k_sh_col_bg);
  sh_titlebar("< Library", nullptr);
  sh_book_cover_fullscreen(k_sh_cv_box_x, k_sh_cv_box_y, k_sh_cv_box_w, k_sh_cv_box_h);
  const sh_entry_t* e = &g_sh.entry[g_sh.selected];
  char              buf[k_sh_linebuf];
  const int32_t     panel_w = (int32_t)k_sh_fb_w - k_sh_cv_panel_x - (int32_t)k_sh_pad;
  sh_fit(buf, sizeof buf, e->title, sh_cells(panel_w));
  (void)ra8_gfx_text_out(k_sh_cv_panel_x,
                         k_sh_cv_title_y,
                         buf,
                         &ra8_gfx_font_8x16,
                         (uint32_t)k_sh_col_card,
                         (uint32_t)k_sh_col_bg);
  sh_fit(buf, sizeof buf, e->author, sh_cells(panel_w));
  (void)ra8_gfx_text_out(k_sh_cv_panel_x,
                         k_sh_cv_auth_y,
                         buf,
                         &ra8_gfx_font_8x16,
                         (uint32_t)k_sh_col_sub,
                         (uint32_t)k_sh_col_bg);

  (void)ra8_gfx_text_out(k_sh_cv_panel_x,
                         k_sh_cv_auth_y + (int32_t)k_sh_glyph_h,
                         e->from_sd ? "Source: SD card" : "Source: baked (MRAM)",
                         &ra8_gfx_font_8x16,
                         (uint32_t)k_sh_col_sub,
                         (uint32_t)k_sh_col_bg);

  sh_cover_button(k_sh_cv_read_y, "Read this book");
  sh_cover_button(k_sh_cv_toc_y, "Table of Contents");
}

/** @brief Inclusive-left / exclusive-right point-in-rect test. */
static bool sh_in_rect(int32_t x, int32_t y, int32_t rx, int32_t ry, int32_t rw, int32_t rh)
{
  return (x >= rx) && (x < (rx + rw)) && (y >= ry) && (y < (ry + rh));
}

sh_cover_act_t sh_cover_action(int32_t x, int32_t y)
{
  if (sh_in_rect(x, y, k_sh_cv_panel_x, k_sh_cv_toc_y, k_sh_cv_btn_w, k_sh_cv_btn_h)) {
    return k_sh_cover_toc;
  }
  if (sh_in_rect(x, y, k_sh_cv_panel_x, k_sh_cv_read_y, k_sh_cv_btn_w, k_sh_cv_btn_h) ||
      sh_in_rect(x, y, k_sh_cv_box_x, k_sh_cv_box_y, k_sh_cv_box_w, k_sh_cv_box_h)) {
    return k_sh_cover_read;
  }
  return k_sh_cover_none;
}

bool sh_cover_loupe_map(int32_t px, int32_t py, int32_t* out_cx, int32_t* out_cy)
{
  if (g_sh.open_fmt != k_sh_fmt_rabook) {
    return false; /* EPUB covers are decoded rasters, not the gray4 image pool */
  }
  const uint32_t cover = g_sh.book_src.hdr.cover_image_index;
  if ((g_sh.book_src.vm == nullptr) || (cover == k_ra8_book_nil)) {
    return false;
  }
  return sh_image_loupe_map(&g_sh.book_src,
                            cover,
                            k_sh_cv_box_x,
                            k_sh_cv_box_y,
                            k_sh_cv_box_w,
                            k_sh_cv_box_h,
                            px,
                            py,
                            out_cx,
                            out_cy);
}

void sh_cover_loupe_render(void)
{
  if ((g_sh.open_fmt != k_sh_fmt_rabook) || (g_sh.book_src.vm == nullptr)) {
    return;
  }
  const uint32_t cover = g_sh.book_src.hdr.cover_image_index;
  if (cover == k_ra8_book_nil) {
    return;
  }
  const int32_t dx = ((int32_t)k_sh_fb_w - (int32_t)k_sh_loupe_w) / 2;
  const int32_t dy = ((int32_t)k_sh_fb_h - (int32_t)k_sh_loupe_h) / 2;
  (void)sh_image_loupe(&g_sh.book_src, cover, g_sh.loupe_cx, g_sh.loupe_cy, dx, dy);
}
