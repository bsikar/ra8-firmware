/**
 * @file src/mg_reader.c
 * @brief Tap-driven pan/zoom viewer over a tiled manga page (ereader_manga).
 *
 * @par Tag
 * [Ring 6 / APP] {World: NS}
 *
 * @details
 * Implementation of the presentation model declared in ``inc/mg_reader.h``.
 * The reader owns no hardware: it maps page pixels to panel pixels, pages the
 * covered tiles through ``ra8_tile_cache`` one at a time, packs gray8 to
 * RGB565, and draws the chrome through ``ra8_gfx`` -- so the same render runs
 * on the firmware, under board_sim, and in the host unit test.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "mg_reader.h"

#include <stddef.h>
#include <stdint.h>

#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_gfx.h"
#include "ra8_gfx_font.h"
#include "ra8_jof.h"
#include "ra8_tile_cache.h"

/** @brief Log tag for the reader's null-pointer guards. */
static const char k_mg_tag[] = "mg_reader";

/**
 * @enum mg_color_t
 * @brief Chrome palette (0x00RRGGBB; ra8_gfx packs to RGB565).
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_mg_col_letterbox    = 0x000000U, /**< Framebuffer clear (page margins). */
  k_mg_col_status_bg    = 0x141420U, /**< Status-bar background.            */
  k_mg_col_status_fg    = 0xFFFFFFU, /**< Status-bar text.                  */
  k_mg_col_minimap_bg   = 0x202830U, /**< Minimap page area.                */
  k_mg_col_minimap_fg   = 0x808890U, /**< Minimap frame.                    */
  k_mg_col_minimap_view = 0xFFE000U, /**< Minimap current-viewport rect.    */
} mg_color_t;

/**
 * @enum mg_metric_t
 * @brief Text / formatting metrics.
 * @since 0.1.0
 */
typedef enum : uint16_t {
  k_mg_status_cap    = 48U, /**< Status-string scratch capacity.      */
  k_mg_status_text_x = 16U, /**< Status text left inset, panel px.    */
  k_mg_glyph_h       = 16U, /**< ra8_gfx_font_8x16 glyph height.      */
  k_mg_dec_max       = 12U, /**< Decimal digit scratch length.        */
  k_mg_str_max       = 32U, /**< Bounded status-fragment copy length. */
} mg_metric_t;

/**
 * @enum mg_bits_t
 * @brief gray8 -> RGB565 field shifts and the decimal radix.
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_mg_dec_ten = 10U, /**< Decimal radix.              */
  k_mg_r_drop  = 3U,  /**< gray8 -> 5-bit field shift. */
  k_mg_g_drop  = 2U,  /**< gray8 -> 6-bit field shift. */
  k_mg_r_sh    = 11U, /**< RGB565 red field shift.     */
  k_mg_g_sh    = 5U,  /**< RGB565 green field shift.   */
} mg_bits_t;

/**
 * @enum mg_fnv_t
 * @brief FNV-1a-32 constants.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_mg_fnv_offset = 2166136261U, /**< FNV-1a-32 offset basis. */
  k_mg_fnv_prime  = 16777619U,   /**< FNV-1a-32 prime.        */
} mg_fnv_t;

/* ===========================================================================
 * Geometry helpers (pure; page-pixel <-> panel-pixel mapping)
 * =========================================================================== */

/** @brief Decimation factor for the active zoom (1 at 1:1, fit_factor at fit). */
static int32_t mg_scale(const mg_reader_t* r)
{
  return (r->zoom == (uint8_t)k_mg_zoom_fit) ? (int32_t)r->fit_factor : 1;
}

/** @brief Content-area height below the status bar, panel px. */
static int32_t mg_content_h(const mg_reader_t* r)
{
  return r->fb_h - (int32_t)k_mg_statusbar_h;
}

/** @brief Source page rectangle currently shown, clamped to the page. */
static void mg_region(const mg_reader_t* r, int32_t* x0, int32_t* y0, int32_t* x1, int32_t* y1)
{
  const int32_t s = mg_scale(r);
  *x0             = r->view_x;
  *y0             = r->view_y;
  int32_t xe      = r->view_x + (r->fb_w * s);
  int32_t ye      = r->view_y + (mg_content_h(r) * s);
  if (xe > (int32_t)r->page_w) {
    xe = (int32_t)r->page_w;
  }
  if (ye > (int32_t)r->page_h) {
    ye = (int32_t)r->page_h;
  }
  *x1 = xe;
  *y1 = ye;
}

/** @brief Panel-pixel offset that centres the shown region in the content area. */
static void mg_offsets(const mg_reader_t* r, int32_t* ox, int32_t* oy)
{
  int32_t x0 = 0;
  int32_t y0 = 0;
  int32_t x1 = 0;
  int32_t y1 = 0;
  mg_region(r, &x0, &y0, &x1, &y1);
  const int32_t s      = mg_scale(r);
  const int32_t disp_w = ((x1 - x0) + s - 1) / s;
  const int32_t disp_h = ((y1 - y0) + s - 1) / s;
  int32_t       px     = (r->fb_w - disp_w) / 2;
  int32_t       py     = (mg_content_h(r) - disp_h) / 2;
  *ox                  = (px > 0) ? px : 0;
  *oy                  = (py > 0) ? py : 0;
}

/** @brief Map a page pixel to a panel pixel; false if off-grid or off-screen. */
static bool mg_map(const mg_reader_t* r,
                   int32_t            s,
                   int32_t            ox,
                   int32_t            oy,
                   int32_t            sx,
                   int32_t            sy,
                   int32_t*           dx,
                   int32_t*           dy)
{
  const int32_t rx = sx - r->view_x;
  const int32_t ry = sy - r->view_y;
  if (((rx % s) != 0) || ((ry % s) != 0)) {
    return false;
  }
  const int32_t px = ox + (rx / s);
  const int32_t py = (int32_t)k_mg_statusbar_h + oy + (ry / s);
  if ((px < 0) || (px >= r->fb_w)) {
    return false;
  }
  if ((py < (int32_t)k_mg_statusbar_h) || (py >= r->fb_h)) {
    return false;
  }
  *dx = px;
  *dy = py;
  return true;
}

/** @brief Pack a gray8 sample into an RGB565 pixel. */
static uint16_t mg_g565(uint8_t g)
{
  const uint16_t r5 = (uint16_t)(g >> k_mg_r_drop);
  const uint16_t g6 = (uint16_t)(g >> k_mg_g_drop);
  const uint16_t b5 = (uint16_t)(g >> k_mg_r_drop);
  return (uint16_t)(((uint16_t)(r5 << k_mg_r_sh)) | ((uint16_t)(g6 << k_mg_g_sh)) | b5);
}

/** @brief Viewport-clamp the reader for the active zoom (fit pins to 0,0). */
static void mg_clamp(mg_reader_t* r)
{
  if (r->zoom == (uint8_t)k_mg_zoom_fit) {
    r->view_x = 0;
    r->view_y = 0;
    return;
  }
  int32_t maxx = (int32_t)r->page_w - r->fb_w;
  int32_t maxy = (int32_t)r->page_h - mg_content_h(r);
  if (maxx < 0) {
    maxx = 0;
  }
  if (maxy < 0) {
    maxy = 0;
  }
  if (r->view_x < 0) {
    r->view_x = 0;
  } else if (r->view_x > maxx) {
    r->view_x = maxx;
  }
  if (r->view_y < 0) {
    r->view_y = 0;
  } else if (r->view_y > maxy) {
    r->view_y = maxy;
  }
}

/* ===========================================================================
 * Rendering
 * =========================================================================== */

/** @brief Blit one pinned gray8 tile's viewport overlap into the framebuffer. */
static void mg_blit_tile(mg_reader_t*      r,
                         const ra8_tile_t* t,
                         uint16_t          tx,
                         uint16_t          ty,
                         int32_t           s,
                         int32_t           ox,
                         int32_t           oy,
                         const int32_t     reg[4])
{
  const int32_t px0 = (int32_t)tx * (int32_t)r->tile_w;
  const int32_t py0 = (int32_t)ty * (int32_t)r->tile_h;
  for (int32_t rr = 0; rr < (int32_t)t->height; ++rr) {
    const int32_t sy = py0 + rr;
    if ((sy < reg[1]) || (sy >= reg[3])) {
      continue;
    }
    for (int32_t cc = 0; cc < (int32_t)t->width; ++cc) {
      const int32_t sx = px0 + cc;
      if ((sx < reg[0]) || (sx >= reg[2])) {
        continue;
      }
      int32_t dx = 0;
      int32_t dy = 0;
      if (!mg_map(r, s, ox, oy, sx, sy, &dx, &dy)) {
        continue;
      }
      const uint8_t g            = t->pixels[(rr * (int32_t)t->width) + cc];
      r->fb[(dy * r->fb_w) + dx] = mg_g565(g);
    }
  }
}

/** @brief Page every tile overlapping the viewport through the cache and blit. */
static ra8_err_t mg_render_page(mg_reader_t* r)
{
  int32_t reg[4] = {};
  mg_region(r, &reg[0], &reg[1], &reg[2], &reg[3]);
  int32_t s  = mg_scale(r);
  int32_t ox = 0;
  int32_t oy = 0;
  mg_offsets(r, &ox, &oy);
  const uint16_t txlo = (uint16_t)(reg[0] / (int32_t)r->tile_w);
  const uint16_t txhi = (uint16_t)((reg[2] - 1) / (int32_t)r->tile_w);
  const uint16_t tylo = (uint16_t)(reg[1] / (int32_t)r->tile_h);
  const uint16_t tyhi = (uint16_t)((reg[3] - 1) / (int32_t)r->tile_h);
  for (uint16_t ty = tylo; ty <= tyhi; ++ty) {
    for (uint16_t tx = txlo; tx <= txhi; ++tx) {
      ra8_tile_key_t  key  = {.image_id = r->image_id, .tile_x = tx, .tile_y = ty, .zoom = 0U};
      ra8_tile_t      tile = {};
      const ra8_err_t e    = ra8_tile_cache_get(r->cache, &key, &tile);
      if (e != k_ra8_ok) {
        return e;
      }
      mg_blit_tile(r, &tile, tx, ty, s, ox, oy, reg);
      const ra8_err_t p = ra8_tile_cache_put(r->cache, tile.pixels);
      if (p != k_ra8_ok) {
        return p;
      }
    }
  }
  return k_ra8_ok;
}

/** @brief Draw the top status bar (background band + the status string). */
static ra8_err_t mg_render_status(mg_reader_t* r)
{
  (void)ra8_gfx_rect(0, 0, r->fb_w, (int32_t)k_mg_statusbar_h, (uint32_t)k_mg_col_status_bg, true);
  char line[k_mg_status_cap] = {};
  (void)mg_reader_status(r, line, (uint32_t)k_mg_status_cap);
  const int32_t ty = ((int32_t)k_mg_statusbar_h - (int32_t)k_mg_glyph_h) / 2;
  return ra8_gfx_text_out((int32_t)k_mg_status_text_x,
                          ty,
                          line,
                          &ra8_gfx_font_8x16,
                          (uint32_t)k_mg_col_status_fg,
                          (uint32_t)k_mg_col_status_bg);
}

/** @brief Draw the bottom-right minimap: the page bounds + the viewport rect. */
static void mg_render_minimap(mg_reader_t* r)
{
  const int32_t mw = (int32_t)k_mg_minimap_w;
  const int32_t mh = (int32_t)k_mg_minimap_h;
  const int32_t mx = r->fb_w - mw - (int32_t)k_mg_minimap_pad;
  const int32_t my = r->fb_h - mh - (int32_t)k_mg_minimap_pad;
  (void)ra8_gfx_rect(mx, my, mw, mh, (uint32_t)k_mg_col_minimap_bg, true);
  (void)ra8_gfx_rect(mx, my, mw, mh, (uint32_t)k_mg_col_minimap_fg, false);
  const int32_t inx    = mx + (int32_t)k_mg_minimap_in;
  const int32_t iny    = my + (int32_t)k_mg_minimap_in;
  const int32_t inw    = mw - (2 * (int32_t)k_mg_minimap_in);
  const int32_t inh    = mh - (2 * (int32_t)k_mg_minimap_in);
  int32_t       reg[4] = {};
  mg_region(r, &reg[0], &reg[1], &reg[2], &reg[3]);
  const int32_t vx = inx + ((reg[0] * inw) / (int32_t)r->page_w);
  const int32_t vy = iny + ((reg[1] * inh) / (int32_t)r->page_h);
  int32_t       vw = ((reg[2] - reg[0]) * inw) / (int32_t)r->page_w;
  int32_t       vh = ((reg[3] - reg[1]) * inh) / (int32_t)r->page_h;
  (void)ra8_gfx_rect(vx,
                     vy,
                     (vw > 1) ? vw : 1,
                     (vh > 1) ? vh : 1,
                     (uint32_t)k_mg_col_minimap_view,
                     false);
}

ra8_err_t mg_reader_render(mg_reader_t* r)
{
  RA8_CHECK_NULL_PTR(r, k_mg_tag, "reader");
  RA8_CHECK_NULL_PTR(r->fb, k_mg_tag, "fb");
  (void)ra8_gfx_clear((uint32_t)k_mg_col_letterbox);
  const ra8_err_t e = mg_render_page(r);
  if (e != k_ra8_ok) {
    return e;
  }
  const ra8_err_t s = mg_render_status(r);
  if (s != k_ra8_ok) {
    return s;
  }
  mg_render_minimap(r);
  return k_ra8_ok;
}

/* ===========================================================================
 * Navigation
 * =========================================================================== */

/** @brief Slide the viewport by (dx,dy) page px; false if pinned / at fit. */
static bool mg_pan(mg_reader_t* r, int32_t dx, int32_t dy)
{
  if (r->zoom == (uint8_t)k_mg_zoom_fit) {
    return false; /* the whole page is shown -- nothing to pan */
  }
  const int32_t ox = r->view_x;
  const int32_t oy = r->view_y;
  r->view_x += dx;
  r->view_y += dy;
  mg_clamp(r);
  return (r->view_x != ox) || (r->view_y != oy);
}

mg_zone_t mg_zone_hit(const mg_reader_t* r, int32_t x, int32_t y)
{
  if (r == nullptr) {
    return k_mg_zone_none;
  }
  if (x < 0) {
    return k_mg_zone_none;
  }
  if (x >= r->fb_w) {
    return k_mg_zone_none;
  }
  if (y < (int32_t)k_mg_statusbar_h) {
    return k_mg_zone_none; /* status bar */
  }
  if (y >= r->fb_h) {
    return k_mg_zone_none;
  }
  const int32_t band = (int32_t)k_mg_edge_band;
  if (y < ((int32_t)k_mg_statusbar_h + band)) {
    return k_mg_zone_pan_up;
  }
  if (y >= (r->fb_h - band)) {
    return k_mg_zone_pan_down;
  }
  if (x < band) {
    return k_mg_zone_pan_left;
  }
  if (x >= (r->fb_w - band)) {
    return k_mg_zone_pan_right;
  }
  return k_mg_zone_zoom;
}

bool mg_reader_toggle_zoom(mg_reader_t* r)
{
  if (r == nullptr) {
    return false;
  }
  r->zoom = (r->zoom == (uint8_t)k_mg_zoom_full) ? (uint8_t)k_mg_zoom_fit : (uint8_t)k_mg_zoom_full;
  mg_clamp(r);
  return true;
}

bool mg_reader_tap(mg_reader_t* r, int32_t x, int32_t y)
{
  if (r == nullptr) {
    return false;
  }
  switch (mg_zone_hit(r, x, y)) {
    case k_mg_zone_zoom:
      return mg_reader_toggle_zoom(r);
    case k_mg_zone_pan_up:
      return mg_pan(r, 0, -(int32_t)k_mg_pan_step);
    case k_mg_zone_pan_down:
      return mg_pan(r, 0, (int32_t)k_mg_pan_step);
    case k_mg_zone_pan_left:
      return mg_pan(r, -(int32_t)k_mg_pan_step, 0);
    case k_mg_zone_pan_right:
      return mg_pan(r, (int32_t)k_mg_pan_step, 0);
    case k_mg_zone_none:
    default:
      return false;
  }
}

/* ===========================================================================
 * Status text + hashing + init + tile decode
 * =========================================================================== */

/** @brief Append an unsigned decimal (from a signed val, clamped >= 0). */
static uint32_t mg_append_uint(char* buf, uint32_t cap, uint32_t pos, int32_t val)
{
  char     tmp[k_mg_dec_max] = {};
  uint32_t n                 = 0U;
  uint32_t v                 = (val < 0) ? 0U : (uint32_t)val;
  if (v == 0U) {
    tmp[n] = '0';
    n++;
  }
  while ((v > 0U) && (n < (uint32_t)k_mg_dec_max)) {
    tmp[n] = (char)('0' + (v % (uint32_t)k_mg_dec_ten));
    n++;
    v /= (uint32_t)k_mg_dec_ten;
  }
  for (uint32_t i = 0U; (i < n) && (pos < (cap - 1U)); ++i) {
    buf[pos] = tmp[n - 1U - i];
    pos++;
  }
  buf[pos] = '\0';
  return pos;
}

/** @brief Append a NUL-terminated fragment, bounded by @p cap. */
static uint32_t mg_append_str(char* buf, uint32_t cap, uint32_t pos, const char* s)
{
  for (uint32_t i = 0U; (i < (uint32_t)k_mg_str_max) && (s[i] != '\0') && (pos < (cap - 1U)); ++i) {
    buf[pos] = s[i];
    pos++;
  }
  buf[pos] = '\0';
  return pos;
}

ra8_err_t mg_reader_status(const mg_reader_t* r, char* buf, uint32_t cap)
{
  RA8_CHECK_NULL_PTR(r, k_mg_tag, "reader");
  RA8_CHECK_NULL_PTR(buf, k_mg_tag, "buf");
  if (cap == 0U) {
    return k_ra8_err_invalid_size;
  }
  uint32_t pos = 0U;
  pos          = mg_append_str(buf, cap, pos, "MANGA  ");
  pos = mg_append_str(buf, cap, pos, (r->zoom == (uint8_t)k_mg_zoom_full) ? "1:1  x=" : "FIT  x=");
  pos = mg_append_uint(buf, cap, pos, r->view_x);
  pos = mg_append_str(buf, cap, pos, " y=");
  (void)mg_append_uint(buf, cap, pos, r->view_y);
  return k_ra8_ok;
}

uint32_t mg_fnv1a(const void* buf, uint32_t len)
{
  const uint8_t* p = (const uint8_t*)buf;
  if (p == nullptr) {
    return (uint32_t)k_mg_fnv_offset;
  }
  uint32_t hsh = (uint32_t)k_mg_fnv_offset;
  for (uint32_t i = 0U; i < len; ++i) {
    hsh = (hsh ^ (uint32_t)p[i]) * (uint32_t)k_mg_fnv_prime;
  }
  return hsh;
}

/** @brief Smallest integer decimation that fits the page in the content area. */
static uint8_t mg_fit_factor(const mg_reader_t* r)
{
  const int32_t cw = r->fb_w;
  const int32_t ch = mg_content_h(r);
  const int32_t fx = ((int32_t)r->page_w + cw - 1) / cw;
  const int32_t fy = ((int32_t)r->page_h + ch - 1) / ch;
  int32_t       f  = (fx > fy) ? fx : fy;
  if (f < 1) {
    f = 1;
  }
  return (uint8_t)f;
}

ra8_err_t mg_reader_init(mg_reader_t* r, const mg_reader_cfg_t* cfg)
{
  RA8_CHECK_NULL_PTR(r, k_mg_tag, "reader");
  RA8_CHECK_NULL_PTR(cfg, k_mg_tag, "cfg");
  RA8_CHECK_NULL_PTR(cfg->fb, k_mg_tag, "fb");
  RA8_CHECK_NULL_PTR(cfg->cache, k_mg_tag, "cache");
  RA8_CHECK_NULL_PTR(cfg->info, k_mg_tag, "info");
  if ((cfg->fb_w <= 0) || (cfg->fb_h <= (int32_t)k_mg_statusbar_h)) {
    return k_ra8_err_invalid_size;
  }
  if ((cfg->info->width == 0U) || (cfg->info->height == 0U) || (cfg->info->tile_w == 0U) ||
      (cfg->info->tile_h == 0U)) {
    return k_ra8_err_invalid_size;
  }
  *r            = (mg_reader_t){.fb        = cfg->fb,
                                .fb_w      = cfg->fb_w,
                                .fb_h      = cfg->fb_h,
                                .cache     = cfg->cache,
                                .image_id  = cfg->image_id,
                                .page_w    = cfg->info->width,
                                .page_h    = cfg->info->height,
                                .tile_w    = cfg->info->tile_w,
                                .tile_h    = cfg->info->tile_h,
                                .tile_cols = cfg->info->tile_cols,
                                .tile_rows = cfg->info->tile_rows,
                                .view_x    = 0,
                                .view_y    = 0,
                                .zoom      = (uint8_t)k_mg_zoom_full};
  r->fit_factor = mg_fit_factor(r);
  mg_clamp(r);
  return k_ra8_ok;
}

ra8_err_t mg_tile_decode(void*                 ctx,
                         const ra8_tile_key_t* key,
                         uint8_t*              cell,
                         uint32_t              cell_bytes,
                         uint16_t*             out_w,
                         uint16_t*             out_h)
{
  RA8_CHECK_NULL_PTR(ctx, k_mg_tag, "src");
  RA8_CHECK_NULL_PTR(key, k_mg_tag, "key");
  const mg_tile_src_t* src = (const mg_tile_src_t*)ctx;
  return ra8_jof_read_tile(src->pread,
                           src->pread_ctx,
                           src->info,
                           key->tile_x,
                           key->tile_y,
                           src->scratch,
                           src->scratch_cap,
                           cell,
                           cell_bytes,
                           out_w,
                           out_h);
}
