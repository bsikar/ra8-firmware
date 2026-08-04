/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file examples/ek_ra8d2/hil_needs_revalidation/ereader_shelf/src/sh_shelf.c
 * @brief Bookshelf screen: a grid of book cards with cover thumbnails.
 *
 * @details
 * Lays out the unified baked+SD entry table as an `ra8_box` grid, drawing each
 * book's cached cover thumbnail above its title and author. Baked thumbnails
 * are pre-decoded gray8 arrays copied straight out of library.h at boot
 * (::sh_shelf_build_thumbs); SD covers decode lazily on first open through the
 * demand-paged source, so the shelf paints without holding any book in RAM.
 * The selected card is highlighted.
 *
 *
 * [Ring 6 / App] {World: NS}
 *
 * @since 0.1.0
 */
#include <string.h>

#include "ra8_box.h"
#include "ra8_gfx.h"
#include "ra8_gfx_font.h"
#include "sh_app.h"

/** @enum sh_shelf_const_t @brief Shelf-layout constants. */
typedef enum : uint32_t {
  k_sh_box_cap    = 32U,  /**< ra8_box scratch node capacity.                   */
  k_sh_sel_border = 4U,   /**< Selected card border width.                      */
  k_sh_card_bord  = 2U,   /**< Idle card border width.                          */
  k_sh_title_gap  = 6U,   /**< Cover-to-title vertical gap.                     */
  k_sh_two        = 2U,   /**< Centring divisor.                                */
  k_sh_card_h     = 272U, /**< Card height (cover box + 2 text lines + insets). */
} sh_shelf_const_t;

void sh_decode_cover(uint16_t idx, const ra8_book_src_t* src)
{
  g_sh.thumb_w[idx] = 0U;
  g_sh.thumb_h[idx] = 0U;
  if (src == nullptr) {
    return;
  }
  const uint32_t cover = src->hdr.cover_image_index;
  if (cover == k_ra8_book_nil) {
    return;
  }
  int32_t w = 0;
  int32_t h = 0;
  if (sh_image_decode_gray8(src,
                            cover,
                            g_sh.thumb[idx],
                            (int32_t)k_sh_thumb_w,
                            (int32_t)k_sh_thumb_h,
                            &w,
                            &h) == k_ra8_ok) {
    g_sh.thumb_w[idx] = (uint16_t)w;
    g_sh.thumb_h[idx] = (uint16_t)h;
  }
}

void sh_shelf_build_thumbs(void)
{
  /* Baked covers are pre-decoded in library.h, so boot just copies the gray8
   * thumbnail -- no per-book decode (which dominated boot time). SD covers
   * still load lazily on first open. */
  for (uint16_t i = 0U; i < g_sh.book_count; ++i) {
    g_sh.thumb_w[i]           = 0U;
    g_sh.thumb_h[i]           = 0U;
    const sh_entry_t* const e = &g_sh.entry[i];
    if (e->from_sd || (e->thumb == nullptr)) {
      continue; /* SD entries get their cover from sh_book_open on first tap */
    }
    (void)memcpy(g_sh.thumb[i], e->thumb, (size_t)e->thumb_w * (size_t)e->thumb_h);
    g_sh.thumb_w[i] = e->thumb_w;
    g_sh.thumb_h[i] = e->thumb_h;
  }
}

/** @brief Draw one book card: cover thumbnail (or placeholder) + title + author. */
static void sh_draw_card(ra8_ui_rect_t r, uint16_t idx)
{
  const bool     sel = (idx == g_sh.selected);
  const uint32_t bg  = sel ? (uint32_t)k_sh_col_card_hi : (uint32_t)k_sh_col_card;
  (void)ra8_gfx_rect(r.x, r.y, r.w, r.h, bg, true);
  sh_border(r,
            sel ? (uint32_t)k_sh_col_sel : (uint32_t)k_sh_col_edge,
            sel ? (int32_t)k_sh_sel_border : (int32_t)k_sh_card_bord);

  const int32_t tw = (int32_t)g_sh.thumb_w[idx];
  const int32_t th = (int32_t)g_sh.thumb_h[idx];
  const int32_t cx = r.x + ((r.w - tw) / (int32_t)k_sh_two);
  const int32_t cy = r.y + (int32_t)k_sh_card_pad;
  if (tw > 0) {
    sh_image_blit_gray8(g_sh.thumb[idx], tw, th, cx, cy);
    sh_border((ra8_ui_rect_t){cx, cy, tw, th}, (uint32_t)k_sh_col_edge, 1);
  } else {
    (void)ra8_gfx_rect(cx,
                       cy,
                       (int32_t)k_sh_thumb_w,
                       (int32_t)k_sh_thumb_h,
                       (uint32_t)k_sh_col_edge,
                       false);
  }

  char          buf[k_sh_linebuf];
  const int32_t inner = r.w - (2 * (int32_t)k_sh_card_pad);
  const int32_t ty    = cy + (int32_t)k_sh_thumb_h + (int32_t)k_sh_title_gap;
  sh_fit(buf, sizeof buf, g_sh.entry[idx].title, sh_cells(inner));
  (void)ra8_gfx_text_out(r.x + (int32_t)k_sh_card_pad,
                         ty,
                         buf,
                         &ra8_gfx_font_8x16,
                         (uint32_t)k_sh_col_ink,
                         bg);
  sh_fit(buf, sizeof buf, g_sh.entry[idx].author, sh_cells(inner));
  (void)ra8_gfx_text_out(r.x + (int32_t)k_sh_card_pad,
                         ty + (int32_t)k_sh_glyph_h,
                         buf,
                         &ra8_gfx_font_8x16,
                         (uint32_t)k_sh_col_sub,
                         bg);
}

/** @brief Lay out the shelf grid into ::sh_state_t::card_rect; returns count placed. */
static uint16_t sh_layout_cards(void)
{
  ra8_box_t      store[k_sh_box_cap];
  ra8_box_tree_t tree;
  if (ra8_box_tree_init(&tree, store, (uint16_t)k_sh_box_cap) != k_ra8_ok) {
    return 0U;
  }
  const ra8_box_t grid = {.kind      = k_ra8_box_grid,
                          .grid_cols = (uint8_t)k_sh_grid_cols,
                          .flex      = 1U,
                          .pad       = (int16_t)k_sh_pad,
                          .gap       = (int16_t)k_sh_gap};
  const int16_t   gi   = ra8_box_add(&tree, (int16_t)k_ra8_box_none, &grid);
  for (uint16_t i = 0U; i < g_sh.book_count; ++i) {
    const ra8_box_t cell = {.kind = k_ra8_box_leaf, .flex = 1U, .tag = (int16_t)i};
    (void)ra8_box_add(&tree, gi, &cell);
  }
  const int32_t rows =
    (int32_t)((g_sh.book_count + (uint16_t)k_sh_grid_cols - 1U) / (uint16_t)k_sh_grid_cols);
  const int32_t avail = (int32_t)k_sh_fb_h - (int32_t)k_sh_bar_h;
  int32_t       ah    = (rows * (int32_t)k_sh_card_h) + (int32_t)k_sh_pad;
  if (ah > avail) {
    ah = avail;
  }
  const ra8_ui_rect_t area = {.x = 0, .y = (int32_t)k_sh_bar_h, .w = (int32_t)k_sh_fb_w, .h = ah};
  if (ra8_box_layout(&tree, gi, &area) != k_ra8_ok) {
    return 0U;
  }
  for (uint16_t i = 0U; i < g_sh.book_count; ++i) {
    g_sh.card_rect[i] = store[(size_t)gi + 1U + (size_t)i].rect;
  }
  return g_sh.book_count;
}

void sh_shelf_render(void)
{
  (void)ra8_gfx_clear((uint32_t)k_sh_col_bg);
  sh_titlebar("Library", g_sh.sd_ready ? "SD + baked" : "baked");
  const uint16_t n = sh_layout_cards();
  for (uint16_t i = 0U; i < n; ++i) {
    sh_draw_card(g_sh.card_rect[i], i);
  }
}

int32_t sh_shelf_hit(int32_t x, int32_t y)
{
  for (uint16_t i = 0U; i < g_sh.book_count; ++i) {
    const ra8_ui_rect_t r = g_sh.card_rect[i];
    if ((x >= r.x) && (x < (r.x + r.w)) && (y >= r.y) && (y < (r.y + r.h))) {
      return (int32_t)i;
    }
  }
  return -1;
}
