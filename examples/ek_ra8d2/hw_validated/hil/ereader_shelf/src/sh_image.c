/**
 * @file examples/ek_ra8d2/hw_validated/hil/ereader_shelf/src/sh_image.c
 * @brief 4bpp-grayscale cover / image decode + scaled blit for ereader_shelf.
 *
 * @details
 * `ra_book` stores raster images as panel-native 4-bit grayscale (two pixels
 * per byte) in the flat blob's image pool, addressed by a flat nibble index
 * `y*w + x` (so an odd image width staggers the high/low nibble across rows).
 * The open book is demand-paged (#204/#205), so this module never holds a
 * resident pointer into the pool: it copies one *source row* of packed nibbles
 * at a time out of the page cache via ra_book_src_read, then unpacks,
 * nearest-neighbour aspect-fit scales, and emits gray8 rows. Covers feed both
 * the shelf thumbnail cache (gray8 buffer) and the full-screen cover page
 * (row-wise ra_gfx_blit_gray8). The row-at-a-time reads produce bytes
 * identical to the old whole-blob walk -- only the fetch granularity changed.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * [Ring 6 / App] {World: NS}
 *
 * @since 0.1.0
 */
#include "ra_check.h"
#include "ra_gfx.h"
#include "sh_app.h"

/** @enum sh_img_const_t @brief Nibble packing + decode-bound constants. */
typedef enum : uint32_t {
  k_sh_nib_lo    = 0x0FU, /**< Low-nibble mask.                            */
  k_sh_nib_sh    = 4U,    /**< Nibble shift (and 4->8 bit replicate).      */
  k_sh_two       = 2U,    /**< Centring divisor / pixels per pool byte.    */
  k_sh_img_max_w = 2048U, /**< Max accepted source width (row-buffer cap). */
} sh_img_const_t;

/**
 * @var s_sh_src_row
 * @brief One packed gray4 source row copied out of the paged image pool.
 * @details Sized for a ::k_sh_img_max_w-wide source row: `w` pixels span at
 *          most `w/2 + 1` pool bytes (odd widths stagger the nibble parity, so
 *          a row can straddle one extra byte at each end). File-scope (not
 *          stack) to keep frames small; the single-threaded UI never re-enters.
 * @since 0.1.0
 */
static uint8_t s_sh_src_row[(k_sh_img_max_w / k_sh_two) + 1U];

/**
 * @var s_sh_loupe
 * @brief One magnifier-loupe source window, repacked as a compact gray4 image.
 * @details Sized for the largest lens window (`k_sh_loupe_w / k_sh_loupe_zoom` by
 *          `k_sh_loupe_h / k_sh_loupe_zoom` pixels, two per byte). Filled one
 *          bounded source-row span per frame from the demand-paged image pool,
 *          then handed to ra_gfx_blit_gray4_zoom(). File-scope to keep frames
 *          small; the single-threaded UI never re-enters the loupe render.
 * @warning A paged book's loupe locality differs from sequential reading: panning
 *          samples scattered sub-rect rows that each span several inflated chunks,
 *          so the per-frame read is bounded to just this window (the #207
 *          prefetch policy is a separate concern).
 * @since 0.1.0
 */
static uint8_t
  s_sh_loupe[((k_sh_loupe_w / k_sh_loupe_zoom) * (k_sh_loupe_h / k_sh_loupe_zoom)) / k_sh_two];

static_assert((k_sh_loupe_w % k_sh_loupe_zoom) == 0U, "loupe width must be a multiple of zoom");
static_assert((k_sh_loupe_h % k_sh_loupe_zoom) == 0U, "loupe height must be a multiple of zoom");

/** @brief Expand the gray4 pixel at flat index @p flat0 + @p sx from the
 *         fetched row (@p row holds pool bytes from byte `flat0 >> 1`). */
static uint8_t sh_gray4_row_at(const uint8_t* row, uint32_t flat0, uint32_t sx)
{
  const uint32_t flat = flat0 + sx;
  const uint8_t  byte = row[(flat >> 1U) - (flat0 >> 1U)];
  const uint8_t  nib =
    ((flat & 1U) != 0U) ? (uint8_t)(byte & k_sh_nib_lo) : (uint8_t)(byte >> k_sh_nib_sh);
  return (uint8_t)((nib << k_sh_nib_sh) | nib);
}

/** @brief Copy source row @p sy's packed bytes into ::s_sh_src_row; sets the
 *         row's flat-index origin. Bounds are enforced by ra_book_src_read. */
static bool sh_gray4_fetch_row(const ra_book_src_t*   src,
                               const ra_book_image_t* img,
                               uint32_t               sy,
                               uint32_t*              out_flat0)
{
  const uint32_t flat0 = sy * (uint32_t)img->width;
  const uint32_t first = flat0 >> 1U;
  const uint32_t last  = (flat0 + (uint32_t)img->width - 1U) >> 1U;
  const uint32_t n     = (last - first) + 1U;
  if (n > (uint32_t)sizeof s_sh_src_row) {
    return false; /* unreachable: width capped at k_sh_img_max_w */
  }
  const uint64_t off = (uint64_t)src->hdr.image_pool_off + (uint64_t)img->data_off + first;
  if ((off + n) > (uint64_t)src->size) {
    return false; /* descriptor points past the blob */
  }
  if (ra_book_src_read(src, (uint32_t)off, s_sh_src_row, n) != k_ra_ok) {
    return false;
  }
  *out_flat0 = flat0;
  return true;
}

/** @brief Aspect-fit @p src_w x @p src_h into @p box; sets @p fit_w / @p fit_h (>=1). */
static void sh_fit_box(int32_t  src_w,
                       int32_t  src_h,
                       int32_t  box_w,
                       int32_t  box_h,
                       int32_t* fit_w,
                       int32_t* fit_h)
{
  int32_t fw = box_w;
  int32_t fh = (src_w > 0) ? ((box_w * src_h) / src_w) : box_h;
  if (fh > box_h) {
    fh = box_h;
    fw = (src_h > 0) ? ((box_h * src_w) / src_h) : box_w;
  }
  *fit_w = (fw < 1) ? 1 : fw;
  *fit_h = (fh < 1) ? 1 : fh;
}

/** @brief Read image descriptor @p img_idx out of the paged source; false if
 *         absent, not gray4, or wider than the row buffer supports. */
static bool sh_gray4_image(const ra_book_src_t* src, uint32_t img_idx, ra_book_image_t* out_img)
{
  if (img_idx >= src->hdr.image_count) {
    return false;
  }
  const uint64_t off = (uint64_t)src->hdr.image_off + ((uint64_t)img_idx * sizeof(ra_book_image_t));
  if (ra_book_src_read(src, (uint32_t)off, out_img, (uint32_t)sizeof *out_img) != k_ra_ok) {
    return false;
  }
  if (out_img->format != (uint8_t)k_ra_book_image_gray4) {
    return false;
  }
  if ((out_img->width == 0U) || (out_img->height == 0U)) {
    return false;
  }
  return (uint32_t)out_img->width <= (uint32_t)k_sh_img_max_w;
}

/** @brief Clamp @p v into [@p lo, @p hi] (assumes @p hi >= @p lo). */
static int32_t sh_clampi(int32_t v, int32_t lo, int32_t hi)
{
  const int32_t x = (v < lo) ? lo : v;
  return (x > hi) ? hi : x;
}

/** @brief Copy only the packed byte span covering source-row @p sy pixels
 *         [@p x0, @p x0 + @p w) into ::s_sh_src_row; sets the span's flat origin.
 *         Bounded per-frame read for the loupe (one window, not a whole row). */
static bool sh_gray4_fetch_span(const ra_book_src_t*   src,
                                const ra_book_image_t* img,
                                uint32_t               sy,
                                uint32_t               x0,
                                uint32_t               w,
                                uint32_t*              out_flat0)
{
  const uint32_t flat0 = (sy * (uint32_t)img->width) + x0;
  const uint32_t first = flat0 >> 1U;
  const uint32_t last  = (flat0 + w - 1U) >> 1U;
  const uint32_t n     = (last - first) + 1U;
  if (n > (uint32_t)sizeof s_sh_src_row) {
    return false; /* window span wider than the row buffer (unreachable at cap) */
  }
  const uint64_t off = (uint64_t)src->hdr.image_pool_off + (uint64_t)img->data_off + first;
  if ((off + n) > (uint64_t)src->size) {
    return false; /* descriptor points past the blob */
  }
  if (ra_book_src_read(src, (uint32_t)off, s_sh_src_row, n) != k_ra_ok) {
    return false;
  }
  *out_flat0 = flat0;
  return true;
}

/** @brief Pack gray8 pixel @p g8 into ::s_sh_loupe at flat index @p j * @p w + @p i. */
static void sh_loupe_pack(int32_t j, int32_t i, int32_t w, uint8_t g8)
{
  const size_t  flat = ((size_t)j * (size_t)w) + (size_t)i;
  const uint8_t nib  = (uint8_t)(g8 >> k_sh_nib_sh);
  const size_t  bi   = flat >> 1U;
  if ((flat & 1U) == 0U) {
    s_sh_loupe[bi] = (uint8_t)(nib << k_sh_nib_sh); /* even flat: high nibble (clears low) */
  } else {
    s_sh_loupe[bi] = (uint8_t)(s_sh_loupe[bi] | nib); /* odd flat: OR in the low nibble */
  }
}

/** @brief Stage the @p w x @p h source window at (@p sx, @p sy) into ::s_sh_loupe. */
static bool sh_loupe_stage(const ra_book_src_t*   src,
                           const ra_book_image_t* img,
                           int32_t                sx,
                           int32_t                sy,
                           int32_t                w,
                           int32_t                h)
{
  for (int32_t j = 0; j < h; ++j) {
    uint32_t flat0 = 0U;
    if (!sh_gray4_fetch_span(src, img, (uint32_t)(sy + j), (uint32_t)sx, (uint32_t)w, &flat0)) {
      return false;
    }
    for (int32_t i = 0; i < w; ++i) {
      sh_loupe_pack(j, i, w, sh_gray4_row_at(s_sh_src_row, flat0, (uint32_t)i));
    }
  }
  return true;
}

/** @brief Frame the lens window with a ::k_sh_loupe_border-thick ink border. */
static void sh_loupe_chrome(int32_t dst_x, int32_t dst_y)
{
  const ra_ui_rect_t r = {.x = dst_x,
                          .y = dst_y,
                          .w = (int32_t)k_sh_loupe_w,
                          .h = (int32_t)k_sh_loupe_h};
  sh_border(r, (uint32_t)k_sh_col_ink, (int32_t)k_sh_loupe_border);
}

ra_err_t sh_image_decode_gray8(const ra_book_src_t* src,
                               uint32_t             img_idx,
                               uint8_t*             out,
                               int32_t              box_w,
                               int32_t              box_h,
                               int32_t*             out_w,
                               int32_t*             out_h)
{
  RA_CHECK_NULL_PTR(src, "sh_image", "decode: null src");
  RA_CHECK_NULL_PTR(out, "sh_image", "decode: null out");
  ra_book_image_t img = {};
  if (!sh_gray4_image(src, img_idx, &img) || (box_w <= 0) || (box_h <= 0)) {
    return k_ra_err_invalid_arg;
  }
  int32_t fw = 0;
  int32_t fh = 0;
  sh_fit_box((int32_t)img.width, (int32_t)img.height, box_w, box_h, &fw, &fh);
  uint32_t last_sy = UINT32_MAX;
  uint32_t flat0   = 0U;
  for (int32_t dy = 0; dy < fh; ++dy) {
    const uint32_t sy = (uint32_t)((dy * (int32_t)img.height) / fh);
    if ((sy != last_sy) && !sh_gray4_fetch_row(src, &img, sy, &flat0)) {
      return k_ra_err_invalid_arg;
    }
    last_sy = sy;
    for (int32_t dx = 0; dx < fw; ++dx) {
      const uint32_t sx   = (uint32_t)((dx * (int32_t)img.width) / fw);
      out[(dy * fw) + dx] = sh_gray4_row_at(s_sh_src_row, flat0, sx);
    }
  }
  *out_w = fw;
  *out_h = fh;
  return k_ra_ok;
}

void sh_image_blit_gray8(const uint8_t* src, int32_t w, int32_t h, int32_t dst_x, int32_t dst_y)
{
  if ((src == nullptr) || (w <= 0) || (h <= 0)) {
    return;
  }
  /* One clipped row-loop in ra_gfx instead of a per-pixel ra_gfx_pixel chain. */
  (void)ra_gfx_blit_gray8(src, w, h, dst_x, dst_y);
}

/**
 * @var s_sh_cover_row
 * @brief One decoded gray8 row of the scaled cover, reused across rows.
 * @details Sized to the panel width (the largest possible fitted cover width),
 *          so ::sh_image_blit_cover can hand a whole row to ::ra_gfx_blit_gray8
 *          instead of plotting pixel by pixel. File-scope (not stack) to keep the
 *          frame small; the single-threaded UI never re-enters the blit.
 * @since 0.1.0
 */
static uint8_t s_sh_cover_row[k_sh_fb_w];

ra_err_t sh_image_blit_cover(const ra_book_src_t* src,
                             uint32_t             img_idx,
                             int32_t              dst_x,
                             int32_t              dst_y,
                             int32_t              box_w,
                             int32_t              box_h,
                             int32_t*             out_w,
                             int32_t*             out_h)
{
  RA_CHECK_NULL_PTR(src, "sh_image", "cover: null src");
  ra_book_image_t img = {};
  if (!sh_gray4_image(src, img_idx, &img) || (box_w <= 0) || (box_h <= 0)) {
    return k_ra_err_invalid_arg;
  }
  int32_t fw = 0;
  int32_t fh = 0;
  sh_fit_box((int32_t)img.width, (int32_t)img.height, box_w, box_h, &fw, &fh);
  const int32_t ox = dst_x + ((box_w - fw) / (int32_t)k_sh_two);
  const int32_t oy = dst_y + ((box_h - fh) / (int32_t)k_sh_two);
  /* Fetch one packed source row from the page cache, decode + nearest-neighbour
   * scale it into a gray8 row, then blit that row in a single clipped pass --
   * the same pixels the resident per-pixel loop wrote, far fewer calls. */
  const int32_t rw      = (fw < (int32_t)k_sh_fb_w) ? fw : (int32_t)k_sh_fb_w;
  uint32_t      last_sy = UINT32_MAX;
  uint32_t      flat0   = 0U;
  for (int32_t dy = 0; dy < fh; ++dy) {
    const uint32_t sy = (uint32_t)((dy * (int32_t)img.height) / fh);
    if ((sy != last_sy) && !sh_gray4_fetch_row(src, &img, sy, &flat0)) {
      return k_ra_err_invalid_arg;
    }
    last_sy = sy;
    for (int32_t dx = 0; dx < rw; ++dx) {
      const uint32_t sx          = (uint32_t)((dx * (int32_t)img.width) / fw);
      s_sh_cover_row[(size_t)dx] = sh_gray4_row_at(s_sh_src_row, flat0, sx);
    }
    (void)ra_gfx_blit_gray8(s_sh_cover_row, rw, 1, ox, oy + dy);
  }
  if (out_w != nullptr) {
    *out_w = fw;
  }
  if (out_h != nullptr) {
    *out_h = fh;
  }
  return k_ra_ok;
}

ra_err_t sh_image_loupe(const ra_book_src_t* src,
                        uint32_t             img_idx,
                        int32_t              src_cx,
                        int32_t              src_cy,
                        int32_t              dst_x,
                        int32_t              dst_y)
{
  RA_CHECK_NULL_PTR(src, "sh_image", "loupe: null src");
  ra_book_image_t img = {};
  if (!sh_gray4_image(src, img_idx, &img)) {
    return k_ra_err_invalid_arg;
  }
  /* Sample a FULL-RESOLUTION window (never scale-to-fit) and magnify it 1:1. The
   * window shrinks to the image if a small source cannot fill the lens. */
  const int32_t sub_w = (int32_t)k_sh_loupe_w / (int32_t)k_sh_loupe_zoom;
  const int32_t sub_h = (int32_t)k_sh_loupe_h / (int32_t)k_sh_loupe_zoom;
  const int32_t w     = ((int32_t)img.width < sub_w) ? (int32_t)img.width : sub_w;
  const int32_t h     = ((int32_t)img.height < sub_h) ? (int32_t)img.height : sub_h;
  const int32_t sx    = sh_clampi(src_cx - (w / (int32_t)k_sh_two), 0, (int32_t)img.width - w);
  const int32_t sy    = sh_clampi(src_cy - (h / (int32_t)k_sh_two), 0, (int32_t)img.height - h);
  if (!sh_loupe_stage(src, &img, sx, sy, w, h)) {
    return k_ra_err_invalid_arg;
  }
  /* Confine the magnified blit to the lens window so nothing spills, then frame. */
  (void)ra_gfx_set_clip(dst_x, dst_y, (int32_t)k_sh_loupe_w, (int32_t)k_sh_loupe_h);
  (void)
    ra_gfx_blit_gray4_zoom(s_sh_loupe, w, h, 0, 0, w, h, (int32_t)k_sh_loupe_zoom, dst_x, dst_y);
  (void)ra_gfx_reset_clip();
  sh_loupe_chrome(dst_x, dst_y);
  return k_ra_ok;
}

bool sh_image_loupe_map(const ra_book_src_t* src,
                        uint32_t             img_idx,
                        int32_t              box_x,
                        int32_t              box_y,
                        int32_t              box_w,
                        int32_t              box_h,
                        int32_t              px,
                        int32_t              py,
                        int32_t*             out_cx,
                        int32_t*             out_cy)
{
  if ((src == nullptr) || (out_cx == nullptr) || (out_cy == nullptr)) {
    return false;
  }
  ra_book_image_t img = {};
  if (!sh_gray4_image(src, img_idx, &img) || (box_w <= 0) || (box_h <= 0)) {
    return false;
  }
  int32_t fw = 0;
  int32_t fh = 0;
  sh_fit_box((int32_t)img.width, (int32_t)img.height, box_w, box_h, &fw, &fh);
  const int32_t ox     = box_x + ((box_w - fw) / (int32_t)k_sh_two);
  const int32_t oy     = box_y + ((box_h - fh) / (int32_t)k_sh_two);
  const bool    inside = (px >= ox) && (px < (ox + fw)) && (py >= oy) && (py < (oy + fh));
  if (!inside) {
    return false;
  }
  *out_cx = ((px - ox) * (int32_t)img.width) / fw;
  *out_cy = ((py - oy) * (int32_t)img.height) / fh;
  return true;
}
