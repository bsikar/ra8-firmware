/**
 * @file ls_drw_target.c
 * @brief DRW + Helium MVE longstrip compositor callbacks for ereader_longstrip.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ls_drw_target.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "longstrip.h"
#include "ra8_attributes.h"
#include "ra8_cache.h"
#include "ra8_check.h"
#include "ra8_drw.h"
#include "ra8_drw_mve.h"
#include "ra8_err.h"

/** @brief Logging tag for the longstrip DRW target adapter. */
static const char* const s_tag = "LS_DRW";

/** @brief DRW wait budget and opaque alpha constants. */
typedef enum : uint32_t {
  k_ls_drw_poll_budget = 1000000UL,
  k_ls_alpha_opaque    = 0xFF000000UL,
} ls_drw_cfg_const_t;

static uint16_t*          s_fb565     = nullptr;
static uint16_t*          s_stage_565 = nullptr;
static uint32_t           s_stage_cap = 0UL;
static uint16_t           s_pitch_px  = 0U;
static longstrip_target_t s_target    = {};

/** @brief Synchronize CPU D-cache lines for a rectangle in @c s_fb565. */
RA8_INTERNAL static void
internal_sync_fb_rect(int32_t x, int32_t y, uint16_t w, uint16_t h, bool clean_before)
{
  const uint32_t row_bytes = (uint32_t)w * (uint32_t)sizeof(uint16_t);
  if ((x == 0) && (w == s_pitch_px)) {
    const size_t   start = (size_t)y * (size_t)s_pitch_px;
    const uint32_t total = (uint32_t)h * row_bytes;
    if (clean_before) {
      (void)ra8_cache_dcache_clean_invalidate_by_addr(&s_fb565[start], total);
    } else {
      (void)ra8_cache_dcache_invalidate_by_addr(&s_fb565[start], total);
    }
    return;
  }
  for (uint16_t r = 0U; r < h; ++r) {
    const size_t off = ((size_t)(y + (int32_t)r) * (size_t)s_pitch_px) + (size_t)x;
    if (clean_before) {
      (void)ra8_cache_dcache_clean_invalidate_by_addr(&s_fb565[off], row_bytes);
    } else {
      (void)ra8_cache_dcache_invalidate_by_addr(&s_fb565[off], row_bytes);
    }
  }
}

/** @brief Hardware band-blit callback routing through MVE conversion and DRW texture blit. */
RA8_INTERNAL static ra8_err_t internal_hw_blit_band(void*          ctx,
                                                    const uint8_t* pixels,
                                                    uint16_t       src_w,
                                                    uint16_t       src_h,
                                                    uint8_t        bpp,
                                                    int32_t        dst_x,
                                                    int32_t        dst_y,
                                                    uint16_t       view_w,
                                                    uint16_t       view_h)
{
  (void)ctx;
  const ra8_drw_mve_band_blit_t job = {
    .src_pixels     = pixels,
    .src_w          = src_w,
    .src_h          = src_h,
    .src_bpp        = bpp,
    .dst_x          = dst_x,
    .dst_y          = dst_y,
    .view_w         = view_w,
    .view_h         = view_h,
    .stage_565      = s_stage_565,
    .stage_cap_px   = s_stage_cap,
    .stage_tex_addr = 0UL,
    .src_tex_addr   = 0UL,
    .fb_ptr         = s_fb565,
    .fb_pitch_px    = s_pitch_px,
    .poll_budget    = (uint32_t)k_ls_drw_poll_budget,
  };
  return ra8_drw_mve_blit_band(&job);
}

/** @brief Hardware solid rectangle fill callback using DRW with FB cache sync. */
RA8_INTERNAL static ra8_err_t
internal_hw_fill_rect(void* ctx, int32_t x, int32_t y, uint16_t w, uint16_t h, uint32_t argb8888)
{
  (void)ctx;
  const ra8_drw_rect_t rect = {
    .x              = (int16_t)x,
    .y              = (int16_t)y,
    .width_px       = w,
    .height_px      = h,
    .color_argb8888 = (uint32_t)k_ls_alpha_opaque | argb8888,
  };
  internal_sync_fb_rect(x, y, w, h, true);
  ra8_err_t rc = ra8_drw_fill_rect(&rect);
  if (rc == k_ra8_ok) {
    rc = ra8_drw_wait_idle((uint32_t)k_ls_drw_poll_budget);
  }
  internal_sync_fb_rect(x, y, w, h, false);
  return rc;
}

/** @brief Implementation of `ls_drw_target_init()` -- DRW init and target binding. */
[[nodiscard]] ra8_err_t ls_drw_target_init(uint16_t* fb565,
                                           uint16_t  view_w,
                                           uint16_t  view_h,
                                           uint16_t* stage_565,
                                           uint32_t  stage_cap)
{
  RA8_CHECK_NULL_PTR(fb565, s_tag, "fb565 must not be nullptr");
  RA8_CHECK_NULL_PTR(stage_565, s_tag, "stage_565 must not be nullptr");
  if ((view_w == 0U) || (view_h == 0U) || (stage_cap < (uint32_t)view_w)) {
    return k_ra8_err_invalid_arg;
  }
  s_fb565     = fb565;
  s_stage_565 = stage_565;
  s_stage_cap = stage_cap;
  s_pitch_px  = view_w;
  s_target    = (longstrip_target_t){
    .fb565       = fb565,
    .fb_pitch_px = view_w,
    .view_w      = view_w,
    .view_h      = view_h,
    .x_off       = 0,
    .prefer_hw   = true,
    .hw = {.blit_band = internal_hw_blit_band, .fill_rect = internal_hw_fill_rect, .ctx = nullptr},
  };
  const ra8_drw_config_t cfg = {
    .framebuffer_addr       = (uintptr_t)fb565,
    .pitch_px               = view_w,
    .format                 = k_ra8_drw_writefmt_rgb565,
    .enable_caches          = false,
    .enable_buffered_writes = false,
  };
  return ra8_drw_init(&cfg);
}

/** @brief Implementation of `ls_drw_target()` -- returns initialized target pointer. */
[[nodiscard]] longstrip_target_t* ls_drw_target(void)
{
  return &s_target;
}
