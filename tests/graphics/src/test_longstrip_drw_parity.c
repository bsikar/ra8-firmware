/**
 * @file test_longstrip_drw_parity.c
 * @brief Host integration parity tests between longstrip SW compositor and DRW+MVE.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "jof.h"
#include "longstrip.h"
#include "ra8_attributes.h"
#include "ra8_board_memmap.h"
#include "ra8_drw.h"
#include "ra8_drw_mve.h"
#include "ra8_drw_regs.h"
#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_mstp.h"
#include "ra8_tile_cache.h"
#include "unity_minimal.h"

/** @brief Test constants for synthetic JOF1 strip and DRW target. */
typedef enum : uint32_t {
  k_p_fb_addr  = k_ra8_board_sdram_base,                /**< SDRAM FB base for fake MMIO.      */
  k_p_tex_addr = (k_ra8_board_sdram_base + 0x100000UL), /**< SDRAM stage texture base.         */
  k_p_width    = 32U,                                   /**< Strip width in pixels.            */
  k_p_height   = 96U,                                   /**< Strip height in pixels (3 bands). */
  k_p_band_h   = 32U,                                   /**< Rows per band.                    */
  k_p_bands    = 3U,                                    /**< Total bands.                      */
  k_p_view_w   = 32U,                                   /**< Viewport width.                   */
  k_p_view_h   = 48U,                                   /**< Viewport height.                  */
  k_p_bpp      = 3U,                                    /**< RGB888 strip source.              */
  k_p_hdr_size = 32U,                                   /**< JOF1 header size.                 */
  k_p_ftr_size = 16U,                                   /**< JOF1 footer size.                 */
  k_p_idx_size = 8U,                                    /**< JOF1 index entry size.            */
  k_p_atlas    = 32768U,                                /**< Static atlas buffer size.         */
  k_p_cells    = 2U,                                    /**< Small LRU cache (< 3 bands).      */
  k_p_buckets  = 4U,                                    /**< Hash buckets.                     */
  k_p_budget   = 64U,                                   /**< DRW poll budget on host.          */
} longstrip_parity_const_t;

/** @brief Shared DRW hardware-adapter context for recording & verifying blits. */
typedef struct {
  uint16_t*       sw_fb;
  uint16_t*       stage_565;
  uint32_t        stage_cap_px;
  uint32_t        hw_blits;
  uint32_t        hw_fills;
  uint16_t        pitch;
  uint16_t        view_w;
  uint16_t        view_h;
  const uint16_t* expect_sw;
  int32_t         scroll_y;
} parity_ctx_t;

static uint8_t             s_atlas[k_p_atlas];
static uint8_t             s_cells[(size_t)k_p_cells * k_p_width * k_p_band_h * k_p_bpp];
static ra8_tile_key_t      s_keys[k_p_cells];
static ra8_tile_dims_t     s_dims[k_p_cells];
static ra8_keycache_cell_t s_meta[k_p_cells];
static int32_t             s_buckets[k_p_buckets];
static uint16_t            s_sw_fb[(size_t)k_p_view_w * k_p_view_h];
static uint16_t            s_stage[(size_t)k_p_view_w * k_p_view_h];
static uint16_t            s_hw_mirror[(size_t)k_p_view_w * k_p_view_h];

/** @brief Write little-endian u16. */
RA8_INTERNAL static void internal_put_u16(uint8_t* p, uint16_t v)
{
  p[0] = (uint8_t)(v & 0xFFU);
  p[1] = (uint8_t)((v >> 8U) & 0xFFU);
}

/** @brief Write little-endian u32. */
RA8_INTERNAL static void internal_put_u32(uint8_t* p, uint32_t v)
{
  p[0] = (uint8_t)(v & 0xFFU);
  p[1] = (uint8_t)((v >> 8U) & 0xFFU);
  p[2] = (uint8_t)((v >> 16U) & 0xFFU);
  p[3] = (uint8_t)((v >> 24U) & 0xFFU);
}

/** @brief Build a minimal raw RGB888 JOF1 strip atlas in static RAM. */
RA8_INTERNAL static uint32_t internal_build_rgb888_atlas(void)
{
  const uint8_t jof1[4] = {'J', 'O', 'F', '1'};
  const uint8_t jofe[4] = {'J', 'O', 'F', 'E'};
  (void)memset(s_atlas, 0, k_p_hdr_size);
  (void)memcpy(s_atlas, jof1, 4U);
  internal_put_u16(&s_atlas[4], (uint16_t)k_p_width);
  internal_put_u16(&s_atlas[6], (uint16_t)k_p_height);
  internal_put_u16(&s_atlas[8], (uint16_t)k_p_width);
  internal_put_u16(&s_atlas[10], (uint16_t)k_p_band_h);
  s_atlas[12] = (uint8_t)k_p_bpp;
  s_atlas[13] = (uint8_t)k_jof_codec_raw;
  internal_put_u32(&s_atlas[16], k_p_bands);

  uint32_t off = k_p_hdr_size;
  uint32_t offs[k_p_bands];
  uint32_t lens[k_p_bands];
  for (uint32_t b = 0U; b < k_p_bands; ++b) {
    offs[b] = off;
    lens[b] = k_p_width * k_p_band_h * k_p_bpp;
    for (uint32_t r = 0U; r < k_p_band_h; ++r) {
      for (uint32_t c = 0U; c < k_p_width; ++c) {
        const uint32_t cy = (b * k_p_band_h) + r;
        s_atlas[off++]    = (uint8_t)((cy * 3U + c) & 0xFFU);
        s_atlas[off++]    = (uint8_t)((cy * 5U) & 0xFFU);
        s_atlas[off++]    = (uint8_t)((c * 7U + b * 11U) & 0xFFU);
      }
    }
  }
  const uint32_t idx_off = off;
  for (uint32_t b = 0U; b < k_p_bands; ++b) {
    internal_put_u32(&s_atlas[off], offs[b]);
    internal_put_u32(&s_atlas[off + 4U], lens[b]);
    off += k_p_idx_size;
  }
  const uint32_t total = off + k_p_ftr_size;
  internal_put_u32(&s_atlas[off], idx_off);
  internal_put_u32(&s_atlas[off + 4U], k_p_bands);
  internal_put_u32(&s_atlas[off + 8U], total);
  (void)memcpy(&s_atlas[off + 12U], jofe, 4U);
  return total;
}

/** @brief HW seam callback binding ra8_drw_mve_blit_band + host pixel check. */
RA8_INTERNAL static ra8_err_t internal_hw_blit_adapter(void*          raw_ctx,
                                                       const uint8_t* pixels,
                                                       uint16_t       src_w,
                                                       uint16_t       src_h,
                                                       uint8_t        bpp,
                                                       int32_t        dst_x,
                                                       int32_t        dst_y,
                                                       uint16_t       view_w,
                                                       uint16_t       view_h)
{
  parity_ctx_t*                 ctx = (parity_ctx_t*)raw_ctx;
  const ra8_drw_mve_band_blit_t job = {
    .src_pixels     = pixels,
    .src_w          = src_w,
    .src_h          = src_h,
    .src_bpp        = bpp,
    .dst_x          = dst_x,
    .dst_y          = dst_y,
    .view_w         = view_w,
    .view_h         = view_h,
    .stage_565      = ctx->stage_565,
    .stage_cap_px   = ctx->stage_cap_px,
    .stage_tex_addr = (uint32_t)k_p_tex_addr,
    .fb_ptr         = nullptr,
    .fb_pitch_px    = ctx->pitch,
    .poll_budget    = (uint32_t)k_p_budget,
  };
  const ra8_err_t rc = ra8_drw_mve_blit_band(&job);
  if (rc != k_ra8_ok) {
    return rc;
  }
  longstrip_clip_rect_t clip = {};
  if ((longstrip_clip_band(dst_x, dst_y, src_w, src_h, view_w, view_h, &clip) == k_ra8_ok) &&
      clip.visible) {
    ctx->hw_blits++;
    TEST_ASSERT_EQ(clip.width, *ra8_drw_reg32(k_ra8_drw_off_lvyaddi));
    TEST_ASSERT_EQ(0UL, *ra8_drw_reg32(k_ra8_drw_off_lustart));
    TEST_ASSERT_EQ(0UL, *ra8_drw_reg32(k_ra8_drw_off_lvstarti));
    for (uint16_t r = 0U; r < clip.height; ++r) {
      const size_t soff = (size_t)r * (size_t)clip.width;
      const size_t doff =
        ((size_t)(clip.dst_y + (int32_t)r) * (size_t)ctx->pitch) + (size_t)clip.dst_x;
      (void)memcpy(&s_hw_mirror[doff],
                   &ctx->stage_565[soff],
                   (size_t)clip.width * sizeof(uint16_t));
    }
  }
  return k_ra8_ok;
}

/** @brief HW fill adapter delegating directly to ra8_drw_fill_rect. */
RA8_INTERNAL static ra8_err_t
internal_hw_fill_adapter(void* raw_ctx, int32_t x, int32_t y, uint16_t w, uint16_t h, uint32_t argb)
{
  parity_ctx_t*        ctx  = (parity_ctx_t*)raw_ctx;
  const ra8_drw_rect_t rect = {
    .x              = (int16_t)x,
    .y              = (int16_t)y,
    .width_px       = w,
    .height_px      = h,
    .color_argb8888 = argb,
  };
  const ra8_err_t rc = ra8_drw_fill_rect(&rect);
  if (rc == k_ra8_ok) {
    ctx->hw_fills++;
  }
  return rc;
}

/** @brief Build and open synthetic strip fixture into @p wt and @p cache. */
RA8_INTERNAL static void internal_open_parity_strip(longstrip_t*            wt,
                                                    ra8_tile_cache_t*       cache,
                                                    longstrip_target_t*     tgt,
                                                    jof_memstore_t*         store,
                                                    longstrip_decode_ctx_t* dctx)
{
  const uint32_t total = internal_build_rgb888_atlas();
  *store               = (jof_memstore_t){.buf = s_atlas, .cap = k_p_atlas, .len = total};
  *dctx                = (longstrip_decode_ctx_t){.pread = jof_memstore_pread, .pread_ctx = store};
  TEST_ASSERT_EQ(k_ra8_ok, jof_parse(jof_memstore_pread, store, total, &dctx->info));
  const ra8_tile_cache_cfg_t ccfg = {.cell_mem     = s_cells,
                                     .cell_bytes   = (uint32_t)(k_p_width * k_p_band_h * k_p_bpp),
                                     .cell_count   = (uint32_t)k_p_cells,
                                     .meta         = s_meta,
                                     .keys         = s_keys,
                                     .dims         = s_dims,
                                     .buckets      = s_buckets,
                                     .bucket_count = (uint32_t)k_p_buckets,
                                     .decode       = longstrip_tile_decode,
                                     .decode_ctx   = dctx};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tile_cache_init(cache, &ccfg));
  const longstrip_cfg_t cfg = {.pread      = jof_memstore_pread,
                               .pread_ctx  = store,
                               .atlas_size = total,
                               .cache      = cache,
                               .image_id   = 7U,
                               .viewport_w = (uint16_t)k_p_view_w,
                               .viewport_h = (uint16_t)k_p_view_h,
                               .blit       = longstrip_target_blit,
                               .blit_ctx   = tgt};
  TEST_ASSERT_EQ(k_ra8_ok, longstrip_open(wt, &cfg));
}

/** @brief Verify SW longstrip compositor matches DRW+MVE pixel-for-pixel. */
RA8_INTERNAL static void internal_test_sw_vs_drw_parity(void)
{
  TEST_BEGIN("longstrip_drw_parity: SW vs DRW+MVE bit-exact across scroll");
  ra8_fake_mmap_reset();
  (void)ra8_mstp_init();
  const ra8_drw_config_t dcfg = {.framebuffer_addr = (uintptr_t)k_p_fb_addr,
                                 .pitch_px         = (uint16_t)k_p_view_w,
                                 .format           = k_ra8_drw_writefmt_rgb565};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_drw_init(&dcfg));

  parity_ctx_t           pctx  = {.sw_fb        = s_sw_fb,
                                  .stage_565    = s_stage,
                                  .stage_cap_px = k_p_view_w * k_p_view_h,
                                  .pitch        = (uint16_t)k_p_view_w,
                                  .view_w       = (uint16_t)k_p_view_w,
                                  .view_h       = (uint16_t)k_p_view_h};
  longstrip_target_t     tgt   = {.fb565       = s_sw_fb,
                                  .fb_pitch_px = (uint16_t)k_p_view_w,
                                  .view_w      = (uint16_t)k_p_view_w,
                                  .view_h      = (uint16_t)k_p_view_h,
                                  .hw          = {.blit_band = internal_hw_blit_adapter,
                                                  .fill_rect = internal_hw_fill_adapter,
                                                  .ctx       = &pctx}};
  longstrip_t            wt    = {};
  ra8_tile_cache_t       cache = {};
  jof_memstore_t         store = {};
  longstrip_decode_ctx_t dctx  = {};
  internal_open_parity_strip(&wt, &cache, &tgt, &store, &dctx);
  TEST_ASSERT_EQ(k_ra8_ok, longstrip_scroll_by(&wt, 20)); /* clips top and bottom bands */

  (void)memset(s_sw_fb, 0, sizeof(s_sw_fb));
  (void)memset(s_hw_mirror, 0, sizeof(s_hw_mirror));
  tgt.prefer_hw               = false;
  longstrip_render_stats_t s1 = {};
  TEST_ASSERT_EQ(k_ra8_ok, longstrip_render(&wt, &s1));
  TEST_ASSERT_EQ(3U, s1.bands_drawn);

  tgt.prefer_hw               = true;
  longstrip_render_stats_t s2 = {};
  TEST_ASSERT_EQ(k_ra8_ok, longstrip_render(&wt, &s2));
  TEST_ASSERT_EQ(3U, pctx.hw_blits);
  TEST_ASSERT_EQ(0, memcmp(s_sw_fb, s_hw_mirror, sizeof(s_sw_fb)));

  TEST_ASSERT_EQ(k_ra8_ok, longstrip_target_fill(&tgt, -4, 4, 16U, 8U, 0xFFAA5500UL));
  TEST_ASSERT_EQ(1U, pctx.hw_fills);
  TEST_ASSERT_EQ(0xFFAA5500UL, *ra8_drw_reg32(k_ra8_drw_off_color1));
  TEST_END("longstrip_drw_parity: SW vs DRW+MVE bit-exact across scroll");
}

int main(void)
{
  internal_test_sw_vs_drw_parity();
  return 0;
}
