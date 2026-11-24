/**
 * @file ra8_drw_mve.c
 * @brief Helium MVE pixel converters and DRW clipped band-blitter.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(__ARM_FEATURE_MVE) && (__ARM_FEATURE_MVE >= 1)
#include <arm_mve.h>
#endif

#include "ra8_attributes.h"
#include "ra8_cache.h"
#include "ra8_check.h"
#include "ra8_drw.h"
#include "ra8_drw_mve.h"
#include "ra8_drw_regs.h"
#include "ra8_err.h"

/** @brief Logging tag for the DRW MVE seam. */
static const char* const s_tag = "DRW";

/** @brief Default constants for MVE pixel conversion and DRW polling. */
typedef enum : uint32_t {
  k_mve_lanes_16       = 8U,        /**< Eight 16-bit lanes per 128-bit Q reg.  */
  k_mve_r5_shift       = 3U,        /**< Red MSB alignment to 5 bits.           */
  k_mve_g6_shift       = 2U,        /**< Green MSB alignment to 6 bits.         */
  k_mve_b5_shift       = 3U,        /**< Blue MSB alignment to 5 bits.          */
  k_mve_r5_pos         = 11U,       /**< RGB565 red bit position.               */
  k_mve_g6_pos         = 5U,        /**< RGB565 green bit position.             */
  k_mve_default_budget = 1000000UL, /**< Default DRW wait_idle poll iterations. */
} drw_mve_const_t;

/** @brief Pack scalar R8/G8/B8 into little-endian RGB565. */
RA8_INTERNAL static inline uint16_t internal_pack_rgb565(uint8_t r, uint8_t g, uint8_t b)
{
  const uint16_t r5 = (uint16_t)((uint32_t)(r >> k_mve_r5_shift) << k_mve_r5_pos);
  const uint16_t g6 = (uint16_t)((uint32_t)(g >> k_mve_g6_shift) << k_mve_g6_pos);
  const uint16_t b5 = (uint16_t)(b >> k_mve_b5_shift);
  return (uint16_t)(r5 | g6 | b5);
}

#if defined(__ARM_FEATURE_MVE) && (__ARM_FEATURE_MVE >= 1)
/** @brief Convert R, G, B 8-lane u16 vectors to packed RGB565 and store predicated. */
RA8_INTERNAL static inline void internal_mve_pack_store_rgb565(uint16_t*    dst,
                                                               uint16x8_t   rv,
                                                               uint16x8_t   gv,
                                                               uint16x8_t   bv,
                                                               mve_pred16_t pred)
{
  const uint16x8_t r5 =
    vshlq_n_u16(vshrq_n_u16(rv, (int32_t)k_mve_r5_shift), (int32_t)k_mve_r5_pos);
  const uint16x8_t g6 =
    vshlq_n_u16(vshrq_n_u16(gv, (int32_t)k_mve_g6_shift), (int32_t)k_mve_g6_pos);
  const uint16x8_t b5  = vshrq_n_u16(bv, (int32_t)k_mve_b5_shift);
  const uint16x8_t out = vorrq_u16(vorrq_u16(r5, g6), b5);
  vstrhq_p_u16(dst, out, pred);
}

/** @brief Helium MVE strided RGB888/RGBA8888 gather-convert loop. */
RA8_INTERNAL static void internal_mve_rgb_strided_to_rgb565(const uint8_t*  src,
                                                            uint16_t*       dst,
                                                            uint32_t        pixel_count,
                                                            uint32_t        bpp,
                                                            const uint16_t* off_table)
{
  const uint16x8_t off    = vldrhq_u16(off_table);
  const uint32_t   blocks = ((pixel_count - 1U) / (uint32_t)k_mve_lanes_16) + 1U;
  for (uint32_t blk = 0U; blk < blocks; ++blk) {
    const uint32_t     idx  = blk * (uint32_t)k_mve_lanes_16;
    const uint32_t     rem  = pixel_count - idx;
    const mve_pred16_t pred = vctp16q(rem);
    const uint8_t*     p    = &src[(size_t)idx * (size_t)bpp];
    const uint16x8_t   rv   = vldrbq_gather_offset_z_u16(p, off, pred);
    const uint16x8_t   gv   = vldrbq_gather_offset_z_u16(p + 1U, off, pred);
    const uint16x8_t   bv   = vldrbq_gather_offset_z_u16(p + 2U, off, pred);
    internal_mve_pack_store_rgb565(&dst[idx], rv, gv, bv, pred);
  }
}
#endif

/** @brief Implementation of `ra8_drw_mve_rgb888_to_rgb565()` -- MVE gather / scalar loop. */
[[nodiscard]] ra8_err_t
ra8_drw_mve_rgb888_to_rgb565(const uint8_t* src, uint16_t* dst, uint32_t pixel_count)
{
  RA8_CHECK_NULL_PTR(src, s_tag, "src must not be nullptr");
  RA8_CHECK_NULL_PTR(dst, s_tag, "dst must not be nullptr");
  if (pixel_count == 0U) {
    return k_ra8_ok;
  }
#if defined(__ARM_FEATURE_MVE) && (__ARM_FEATURE_MVE >= 1)
  static const uint16_t k_off_rgb888[k_mve_lanes_16] = {
    0U,
    3U,
    6U,
    9U,
    12U,
    15U,
    18U,
    21U,
  };
  internal_mve_rgb_strided_to_rgb565(src,
                                     dst,
                                     pixel_count,
                                     (uint32_t)k_ra8_drw_px_bpp_rgb888,
                                     k_off_rgb888);
#else
  for (uint32_t i = 0U; i < pixel_count; ++i) {
    const size_t base = (size_t)i * (size_t)k_ra8_drw_px_bpp_rgb888;
    dst[i]            = internal_pack_rgb565(src[base], src[base + 1U], src[base + 2U]);
  }
#endif
  return k_ra8_ok;
}

/** @brief Implementation of `ra8_drw_mve_gray8_to_rgb565()` -- MVE widening load / scalar. */
[[nodiscard]] ra8_err_t
ra8_drw_mve_gray8_to_rgb565(const uint8_t* src, uint16_t* dst, uint32_t pixel_count)
{
  RA8_CHECK_NULL_PTR(src, s_tag, "src must not be nullptr");
  RA8_CHECK_NULL_PTR(dst, s_tag, "dst must not be nullptr");
  if (pixel_count == 0U) {
    return k_ra8_ok;
  }
#if defined(__ARM_FEATURE_MVE) && (__ARM_FEATURE_MVE >= 1)
  const uint32_t blocks = ((pixel_count - 1U) / (uint32_t)k_mve_lanes_16) + 1U;
  for (uint32_t blk = 0U; blk < blocks; ++blk) {
    const uint32_t     idx  = blk * (uint32_t)k_mve_lanes_16;
    const uint32_t     rem  = pixel_count - idx;
    const mve_pred16_t pred = vctp16q(rem);
    const uint16x8_t   gv   = vldrbq_z_u16(&src[idx], pred);
    internal_mve_pack_store_rgb565(&dst[idx], gv, gv, gv, pred);
  }
#else
  for (uint32_t i = 0U; i < pixel_count; ++i) {
    const uint8_t y = src[i];
    dst[i]          = internal_pack_rgb565(y, y, y);
  }
#endif
  return k_ra8_ok;
}

/** @brief Implementation of `ra8_drw_mve_rgba8888_to_rgb565()` -- MVE stride-4 gather / scalar. */
[[nodiscard]] ra8_err_t
ra8_drw_mve_rgba8888_to_rgb565(const uint8_t* src, uint16_t* dst, uint32_t pixel_count)
{
  RA8_CHECK_NULL_PTR(src, s_tag, "src must not be nullptr");
  RA8_CHECK_NULL_PTR(dst, s_tag, "dst must not be nullptr");
  if (pixel_count == 0U) {
    return k_ra8_ok;
  }
#if defined(__ARM_FEATURE_MVE) && (__ARM_FEATURE_MVE >= 1)
  static const uint16_t k_off_rgba8888[k_mve_lanes_16] = {
    0U,
    4U,
    8U,
    12U,
    16U,
    20U,
    24U,
    28U,
  };
  internal_mve_rgb_strided_to_rgb565(src,
                                     dst,
                                     pixel_count,
                                     (uint32_t)k_ra8_drw_px_bpp_rgba8888,
                                     k_off_rgba8888);
#else
  for (uint32_t i = 0U; i < pixel_count; ++i) {
    const size_t base = (size_t)i * (size_t)k_ra8_drw_px_bpp_rgba8888;
    dst[i]            = internal_pack_rgb565(src[base], src[base + 1U], src[base + 2U]);
  }
#endif
  return k_ra8_ok;
}

/** @brief Convert one packed row segment by bpp into RGB565. */
RA8_INTERNAL static ra8_err_t
internal_convert_row(const uint8_t* src, uint16_t* dst, uint32_t count, uint8_t bpp)
{
  if (bpp == (uint8_t)k_ra8_drw_px_bpp_gray8) {
    return ra8_drw_mve_gray8_to_rgb565(src, dst, count);
  }
  if (bpp == (uint8_t)k_ra8_drw_px_bpp_rgb565) {
    for (uint32_t i = 0U; i < count; ++i) {
      const size_t idx = (size_t)i * 2U;
      dst[i]           = (uint16_t)((uint16_t)src[idx] | (uint16_t)((uint32_t)src[idx + 1U] << 8U));
    }
    return k_ra8_ok;
  }
  if (bpp == (uint8_t)k_ra8_drw_px_bpp_rgb888) {
    return ra8_drw_mve_rgb888_to_rgb565(src, dst, count);
  }
  if (bpp == (uint8_t)k_ra8_drw_px_bpp_rgba8888) {
    return ra8_drw_mve_rgba8888_to_rgb565(src, dst, count);
  }
  return k_ra8_err_invalid_arg;
}

/** @brief Implementation of `ra8_drw_mve_convert_rect_to_rgb565()` -- row-wise dispatch. */
[[nodiscard]] ra8_err_t ra8_drw_mve_convert_rect_to_rgb565(const ra8_drw_mve_rect_conv_t* conv)
{
  RA8_CHECK_NULL_PTR(conv, s_tag, "conv must not be nullptr");
  RA8_CHECK_NULL_PTR(conv->src, s_tag, "conv->src must not be nullptr");
  RA8_CHECK_NULL_PTR(conv->dst, s_tag, "conv->dst must not be nullptr");
  if ((conv->width_px == 0U) || (conv->height_px == 0U) ||
      ((uint32_t)conv->src_x + (uint32_t)conv->width_px > (uint32_t)conv->src_stride_px) ||
      (conv->width_px > conv->dst_stride_px)) {
    return k_ra8_err_invalid_arg;
  }
  for (uint16_t r = 0U; r < conv->height_px; ++r) {
    const size_t src_off =
      (((size_t)(conv->src_y + r) * (size_t)conv->src_stride_px) + (size_t)conv->src_x) *
      (size_t)conv->src_bpp;
    const size_t    dst_off = (size_t)r * (size_t)conv->dst_stride_px;
    const ra8_err_t rc =
      internal_convert_row(&conv->src[src_off], &conv->dst[dst_off], conv->width_px, conv->src_bpp);
    if (rc != k_ra8_ok) {
      return rc;
    }
  }
  return k_ra8_ok;
}

/** @brief Compute viewport intersection and clipped source offsets. */
RA8_INTERNAL static bool internal_clip_rect(const ra8_drw_mve_band_blit_t* blit,
                                            ra8_drw_rect_t*                out_rect,
                                            uint16_t*                      out_sx,
                                            uint16_t*                      out_sy)
{
  const int32_t x0 = (blit->dst_x > 0) ? blit->dst_x : 0;
  const int32_t y0 = (blit->dst_y > 0) ? blit->dst_y : 0;
  const int32_t rx = blit->dst_x + (int32_t)blit->src_w;
  const int32_t ry = blit->dst_y + (int32_t)blit->src_h;
  const int32_t x1 = (rx < (int32_t)blit->view_w) ? rx : (int32_t)blit->view_w;
  const int32_t y1 = (ry < (int32_t)blit->view_h) ? ry : (int32_t)blit->view_h;
  if ((x0 >= x1) || (y0 >= y1)) {
    return false;
  }
  out_rect->x              = (int16_t)x0;
  out_rect->y              = (int16_t)y0;
  out_rect->width_px       = (uint16_t)(x1 - x0);
  out_rect->height_px      = (uint16_t)(y1 - y0);
  out_rect->color_argb8888 = 0UL;
  *out_sx                  = (uint16_t)(x0 - blit->dst_x);
  *out_sy                  = (uint16_t)(y0 - blit->dst_y);
  return true;
}

/** @brief Clean+invalidate FB rows before DRW write, or invalidate after DRW write. */
RA8_INTERNAL static ra8_err_t internal_sync_fb_region(uint16_t*             fb_ptr,
                                                      uint16_t              fb_pitch_px,
                                                      const ra8_drw_rect_t* rect,
                                                      bool                  clean_before)
{
  if (fb_ptr == nullptr) {
    return k_ra8_ok;
  }
  const uint32_t row_bytes = (uint32_t)rect->width_px * (uint32_t)sizeof(uint16_t);
  if ((rect->x == 0) && (rect->width_px == fb_pitch_px)) {
    const size_t   start = (size_t)rect->y * (size_t)fb_pitch_px;
    const uint32_t total = (uint32_t)rect->height_px * row_bytes;
    return clean_before ? ra8_cache_dcache_clean_invalidate_by_addr(&fb_ptr[start], total)
                        : ra8_cache_dcache_invalidate_by_addr(&fb_ptr[start], total);
  }
  for (uint16_t r = 0U; r < rect->height_px; ++r) {
    const size_t    off = ((size_t)(rect->y + (int16_t)r) * (size_t)fb_pitch_px) + (size_t)rect->x;
    const ra8_err_t rc  = clean_before
                            ? ra8_cache_dcache_clean_invalidate_by_addr(&fb_ptr[off], row_bytes)
                            : ra8_cache_dcache_invalidate_by_addr(&fb_ptr[off], row_bytes);
    if (rc != k_ra8_ok) {
      return rc;
    }
  }
  return k_ra8_ok;
}

/** @brief Program DRW texture, blit sub-rectangle, wait idle, and clear texture bit. */
RA8_INTERNAL static ra8_err_t internal_drw_exec_tex_blit(uint32_t              tex_addr,
                                                         uint16_t              tex_w,
                                                         uint16_t              tex_h,
                                                         uint16_t              src_x,
                                                         uint16_t              src_y,
                                                         const ra8_drw_rect_t* dst_rect,
                                                         uint16_t*             fb_ptr,
                                                         uint16_t              fb_pitch_px,
                                                         uint32_t              budget)
{
  const ra8_drw_texture_t tex = {
    .base_addr        = tex_addr,
    .pitch_px         = tex_w,
    .u_mask           = (uint16_t)(tex_w - 1U),
    .v_mask           = (uint32_t)tex_w * ((uint32_t)tex_h - 1U),
    .format           = k_ra8_drw_readfmt_rgb565,
    .clamp_x          = true,
    .clamp_y          = true,
    .filter_x         = false,
    .filter_y         = false,
    .enable_clut      = false,
    .clut_565         = false,
    .clut_offset      = 0U,
    .enable_color_key = false,
    .color_key_rgb    = 0UL,
    .enable_rle       = false,
    .rle_pixel_width  = 0U,
  };
  ra8_err_t rc = internal_sync_fb_region(fb_ptr, fb_pitch_px, dst_rect, true);
  if (rc != k_ra8_ok) {
    return rc;
  }
  rc = ra8_drw_set_texture(&tex);
  if (rc != k_ra8_ok) {
    return rc;
  }
  rc = ra8_drw_blit_textured_subrect(dst_rect, src_x, src_y);
  if (rc == k_ra8_ok) {
    rc = ra8_drw_wait_idle(budget);
  }
  (void)ra8_drw_clear_texture();
  if (rc == k_ra8_ok) {
    rc = internal_sync_fb_region(fb_ptr, fb_pitch_px, dst_rect, false);
  }
  return rc;
}

/** @brief Stage MVE conversion into caller buffer in strips and blit through DRW. */
RA8_INTERNAL static ra8_err_t internal_blit_staged(const ra8_drw_mve_band_blit_t* blit,
                                                   const ra8_drw_rect_t*          clip,
                                                   uint16_t                       sx,
                                                   uint16_t                       sy,
                                                   uint32_t                       budget)
{
  if ((blit->stage_cap_px < (uint32_t)clip->width_px)) {
    return k_ra8_err_invalid_size;
  }
  const uint32_t max_rows_u32 = blit->stage_cap_px / (uint32_t)clip->width_px;
  uint16_t       max_rows =
    (max_rows_u32 > (uint32_t)clip->height_px) ? clip->height_px : (uint16_t)max_rows_u32;
  if (max_rows > (uint16_t)k_ra8_drw_max_height_px) {
    max_rows = (uint16_t)k_ra8_drw_max_height_px;
  }
  const uint32_t tex_addr =
    (blit->stage_tex_addr != 0UL) ? blit->stage_tex_addr : (uint32_t)(uintptr_t)blit->stage_565;
  for (uint16_t row = 0U; row < clip->height_px; row = (uint16_t)(row + max_rows)) {
    const uint16_t                rem  = (uint16_t)(clip->height_px - row);
    const uint16_t                h    = (rem < max_rows) ? rem : max_rows;
    const ra8_drw_mve_rect_conv_t conv = {
      .src           = blit->src_pixels,
      .dst           = blit->stage_565,
      .src_stride_px = blit->src_w,
      .dst_stride_px = clip->width_px,
      .src_x         = sx,
      .src_y         = (uint16_t)(sy + row),
      .width_px      = clip->width_px,
      .height_px     = h,
      .src_bpp       = blit->src_bpp,
    };
    ra8_err_t rc = ra8_drw_mve_convert_rect_to_rgb565(&conv);
    if (rc != k_ra8_ok) {
      return rc;
    }
    const uint32_t clean_bytes =
      (uint32_t)clip->width_px * (uint32_t)h * (uint32_t)sizeof(uint16_t);
    rc = ra8_cache_dcache_clean_by_addr(blit->stage_565, clean_bytes);
    if (rc != k_ra8_ok) {
      return rc;
    }
    const ra8_drw_rect_t dst = {
      .x              = clip->x,
      .y              = (int16_t)(clip->y + (int16_t)row),
      .width_px       = clip->width_px,
      .height_px      = h,
      .color_argb8888 = 0UL,
    };
    rc = internal_drw_exec_tex_blit(tex_addr,
                                    clip->width_px,
                                    h,
                                    0U,
                                    0U,
                                    &dst,
                                    blit->fb_ptr,
                                    blit->fb_pitch_px,
                                    budget);
    if (rc != k_ra8_ok) {
      return rc;
    }
  }
  return k_ra8_ok;
}

/** @brief Implementation of `ra8_drw_mve_blit_band()` -- clip, convert/cache-sync, DRW blit. */
[[nodiscard]] ra8_err_t ra8_drw_mve_blit_band(const ra8_drw_mve_band_blit_t* blit)
{
  RA8_CHECK_NULL_PTR(blit, s_tag, "blit must not be nullptr");
  RA8_CHECK_NULL_PTR(blit->src_pixels, s_tag, "blit->src_pixels must not be nullptr");
  if ((blit->src_w == 0U) || (blit->src_h == 0U) || (blit->view_w == 0U) || (blit->view_h == 0U) ||
      (blit->src_bpp < (uint8_t)k_ra8_drw_px_bpp_gray8) ||
      (blit->src_bpp > (uint8_t)k_ra8_drw_px_bpp_rgba8888)) {
    return k_ra8_err_invalid_arg;
  }
  ra8_drw_rect_t clip = {};
  uint16_t       sx   = 0U;
  uint16_t       sy   = 0U;
  if (!internal_clip_rect(blit, &clip, &sx, &sy)) {
    return k_ra8_ok;
  }
  if ((blit->fb_ptr != nullptr) &&
      ((uint32_t)clip.x + (uint32_t)clip.width_px > (uint32_t)blit->fb_pitch_px)) {
    return k_ra8_err_invalid_arg;
  }
  const uint32_t budget =
    (blit->poll_budget != 0UL) ? blit->poll_budget : (uint32_t)k_mve_default_budget;
  if ((blit->src_bpp == (uint8_t)k_ra8_drw_px_bpp_rgb565) && (blit->stage_565 == nullptr)) {
    const uint32_t src_bytes =
      (uint32_t)blit->src_w * (uint32_t)blit->src_h * (uint32_t)sizeof(uint16_t);
    const ra8_err_t rc = ra8_cache_dcache_clean_by_addr(blit->src_pixels, src_bytes);
    if (rc != k_ra8_ok) {
      return rc;
    }
    const uint32_t tex_addr =
      (blit->src_tex_addr != 0UL) ? blit->src_tex_addr : (uint32_t)(uintptr_t)blit->src_pixels;
    return internal_drw_exec_tex_blit(tex_addr,
                                      blit->src_w,
                                      blit->src_h,
                                      sx,
                                      sy,
                                      &clip,
                                      blit->fb_ptr,
                                      blit->fb_pitch_px,
                                      budget);
  }
  RA8_CHECK_NULL_PTR(blit->stage_565, s_tag, "blit->stage_565 must not be nullptr");
  return internal_blit_staged(blit, &clip, sx, sy, budget);
}

/** @brief Implementation of `ra8_drw_mve_is_hardware_enabled()` -- compile-time MVE probe. */
[[nodiscard]] bool ra8_drw_mve_is_hardware_enabled(void)
{
#if defined(__ARM_FEATURE_MVE) && (__ARM_FEATURE_MVE >= 1)
  return true;
#else
  return false;
#endif
}
