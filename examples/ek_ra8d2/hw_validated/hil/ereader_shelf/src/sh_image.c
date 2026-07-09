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
