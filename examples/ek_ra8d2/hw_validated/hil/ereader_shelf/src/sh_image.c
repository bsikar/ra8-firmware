/**
 * @file examples/ek_ra8d2/hw_validated/hil/ereader_shelf/src/sh_image.c
 * @brief 4bpp-grayscale cover / image decode + scaled blit for ereader_shelf.
 *
 * @details
 * `ra_book` stores raster images as panel-native 4-bit grayscale (two pixels
 * per byte) in the inflated blob, addressed by a flat nibble index `y*w + x`
 * (so an odd image width staggers the high/low nibble across rows). The bundled
 * `ra_gfx` blitter has no grayscale or scaling path, so this module unpacks the
 * nibbles, nearest-neighbour aspect-fit scales them, and writes RGB565 grays via
 * ra_gfx_pixel(). Covers feed both the shelf thumbnail cache (gray8 buffer) and
 * the full-screen cover page (direct framebuffer blit).
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

/** @enum sh_img_const_t @brief Nibble + RGB565-gray packing constants. */
typedef enum : uint32_t {
  k_sh_nib_lo   = 0x0FU, /**< Low-nibble mask.                       */
  k_sh_nib_sh   = 4U,    /**< Nibble shift (and 4->8 bit replicate). */
  k_sh_rgb_sh_r = 16U,   /**< Red byte shift in 0x00RRGGBB.          */
  k_sh_rgb_sh_g = 8U,    /**< Green byte shift in 0x00RRGGBB.        */
  k_sh_two      = 2U,    /**< Centring divisor.                      */
} sh_img_const_t;

/** @brief Expand the 4bpp gray pixel at flat index @p fx,@p fy to 0..255. */
static uint8_t sh_gray4_at(const uint8_t* data, uint32_t width, uint32_t x, uint32_t y)
{
  const uint32_t flat = (y * width) + x;
  const uint8_t  byte = data[flat >> 1U];
  const uint8_t  nib =
    ((flat & 1U) != 0U) ? (uint8_t)(byte & k_sh_nib_lo) : (uint8_t)(byte >> k_sh_nib_sh);
  return (uint8_t)((nib << k_sh_nib_sh) | nib);
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

/** @brief Resolve a gray4 image descriptor; returns NULL if absent/wrong format. */
static const ra_book_image_t* sh_gray4_image(const void* base, uint32_t img_idx)
{
  const ra_book_header_t* hdr = ra_book_header(base);
  if (img_idx >= hdr->image_count) {
    return nullptr;
  }
  const ra_book_image_t* img = &ra_book_images(base)[img_idx];
  return (img->format == (uint8_t)k_ra_book_image_gray4) ? img : nullptr;
}

ra_err_t sh_image_decode_gray8(const void* base,
                               uint32_t    img_idx,
                               uint8_t*    out,
                               int32_t     box_w,
                               int32_t     box_h,
                               int32_t*    out_w,
                               int32_t*    out_h)
{
  RA_CHECK_NULL_PTR(base, "sh_image", "decode: null base");
  RA_CHECK_NULL_PTR(out, "sh_image", "decode: null out");
  const ra_book_image_t* img = sh_gray4_image(base, img_idx);
  if ((img == nullptr) || (box_w <= 0) || (box_h <= 0)) {
    return k_ra_err_invalid_arg;
  }
  const uint8_t* data = ra_book_image_data(base, img);
  int32_t        fw   = 0;
  int32_t        fh   = 0;
  sh_fit_box((int32_t)img->width, (int32_t)img->height, box_w, box_h, &fw, &fh);
  for (int32_t dy = 0; dy < fh; ++dy) {
    const uint32_t sy = (uint32_t)((dy * (int32_t)img->height) / fh);
    for (int32_t dx = 0; dx < fw; ++dx) {
      const uint32_t sx   = (uint32_t)((dx * (int32_t)img->width) / fw);
      out[(dy * fw) + dx] = sh_gray4_at(data, img->width, sx, sy);
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

ra_err_t sh_image_blit_cover(const void* base,
                             uint32_t    img_idx,
                             int32_t     dst_x,
                             int32_t     dst_y,
                             int32_t     box_w,
                             int32_t     box_h,
                             int32_t*    out_w,
                             int32_t*    out_h)
{
  RA_CHECK_NULL_PTR(base, "sh_image", "cover: null base");
  const ra_book_image_t* img = sh_gray4_image(base, img_idx);
  if ((img == nullptr) || (box_w <= 0) || (box_h <= 0)) {
    return k_ra_err_invalid_arg;
  }
  const uint8_t* data = ra_book_image_data(base, img);
  int32_t        fw   = 0;
  int32_t        fh   = 0;
  sh_fit_box((int32_t)img->width, (int32_t)img->height, box_w, box_h, &fw, &fh);
  const int32_t ox = dst_x + ((box_w - fw) / (int32_t)k_sh_two);
  const int32_t oy = dst_y + ((box_h - fh) / (int32_t)k_sh_two);
  /* Decode + nearest-neighbour scale one output row, then blit it in a single
   * clipped pass -- the same pixels the per-pixel loop wrote, far fewer calls. */
  const int32_t rw = (fw < (int32_t)k_sh_fb_w) ? fw : (int32_t)k_sh_fb_w;
  for (int32_t dy = 0; dy < fh; ++dy) {
    const uint32_t sy = (uint32_t)((dy * (int32_t)img->height) / fh);
    for (int32_t dx = 0; dx < rw; ++dx) {
      const uint32_t sx          = (uint32_t)((dx * (int32_t)img->width) / fw);
      s_sh_cover_row[(size_t)dx] = sh_gray4_at(data, img->width, sx, sy);
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
