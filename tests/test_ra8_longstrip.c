/**
 * @file test_ra8_longstrip.c
 * @brief Host unit tests for the continuous vertical-scroll longstrip engine (#289).
 *
 * @details
 * Drives ra8_longstrip over a hand-built raw JOF band-tile atlas (a synthetic
 * tall strip) paged through a real ::ra8_tile_cache. The strip encodes each
 * pixel's absolute canvas coordinate (R=y low, G=y high, B=x low), so the
 * recording blit can assert -- per pixel -- that every band composites at its
 * exact seamless canvas position. Coverage:
 *
 *   - open validation (MC/DC on the geometry + viewport compound decisions),
 *   - O(1) y->band geometry + visible-band range,
 *   - a full top-to-bottom scroll traversal proving ZERO skipped bands, zero
 *     seams and complete canvas coverage (the #289 0-skip contract),
 *   - fling physics: bounded convergence + end clamp (MC/DC on the fling-stop
 *     and end-pin compound decisions),
 *   - directional prefetch (MC/DC on the prefetch-skip compound decision),
 *   - bounded resident memory: the tile cache never holds more than its cell
 *     budget regardless of strip height or scroll distance.
 *
 * The atlas is built with the raw codec (codec 0), so `ra8_jof_read_tile`
 * needs no deflate scratch and the decode path is exercised end to end without
 * pulling the compressor -- the same reader the deflate path uses.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_longstrip.h"
#include "ra8_tile_cache.h"
#include "ra8_jof.h"
#include "unity_minimal.h"

/**
 * @enum longstrip_uint8_const_t
 * @brief Named uint8_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint8_t {
  k_longstrip_first_ff        = 0xFFU,
  k_longstrip_t_wt_put_u16_10 = 10,
  k_longstrip_val_12          = 12,
  k_longstrip_val_13          = 13,
} longstrip_uint8_const_t;

/**
 * @enum longstrip_uint16_const_t
 * @brief Named uint16_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint16_t {
  k_longstrip_band_ffff = 0xFFFFU,
} longstrip_uint16_const_t;

/**
 * @enum longstrip_uint32_const_t
 * @brief Named uint32_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint32_t {
  k_longstrip_ra8_longstrip_scroll_by_100000 = 100000,
} longstrip_uint32_const_t;

/**
 * @enum t_wt_geom_t
 * @brief Synthetic strip + viewport geometry.
 *
 * @details A 192-wide, 800-tall strip cut into 64-px bands: 13 bands, the last
 *          clamped to 32 px (800 = 12*64 + 32), so the edge-band math is
 *          exercised. The 256-tall viewport straddles 4-5 bands.
 */
typedef enum : uint32_t {
  k_t_wt_width       = 192U,            /**< Strip width, pixels.            */
  k_t_wt_band_h      = 64U,             /**< Band (tile) height, pixels.     */
  k_t_wt_height      = 800U,            /**< Strip height, pixels.           */
  k_t_wt_bands       = 13U,             /**< ceil(800 / 64) bands.           */
  k_t_wt_last_band_h = 32U,             /**< 800 - 12*64.                    */
  k_t_wt_bpp         = 3U,              /**< RGB888.                         */
  k_t_wt_view_w      = 192U,            /**< Viewport width, pixels.         */
  k_t_wt_view_h      = 256U,            /**< Viewport height, pixels.        */
  k_t_wt_max_scroll  = 544U,            /**< 800 - 256.                      */
  k_t_wt_band_bytes  = 192U * 64U * 3U, /**< Worst-case band payload, bytes. */
} t_wt_geom_t;

/**
 * @enum t_wt_cache_t
 * @brief Tile-cache sizing (small on purpose: fewer cells than bands so the
 *        LRU must evict during a full scroll -- the bounded-memory proof).
 */
typedef enum : uint32_t {
  k_t_wt_cells   = 4U, /**< Cache cells (< 13 bands -> eviction). */
  k_t_wt_buckets = 8U, /**< Hash buckets.                         */
} t_wt_cache_t;

/**
 * @enum t_wt_layout_t
 * @brief JOF on-disk field sizes used by the raw-atlas builder.
 */
typedef enum : uint32_t {
  k_t_hdr_bytes = 32U,          /**< Header length.           */
  k_t_ftr_bytes = 16U,          /**< Footer length.           */
  k_t_idx_entry = 8U,           /**< Index entry length.      */
  k_t_atlas_cap = 512U * 1024U, /**< Backing buffer capacity. */
  k_t_byte_mask = 0xFFU,        /**< Low-byte mask.           */
  k_t_shift_8   = 8U,           /**< One-byte shift.          */
  k_t_shift_16  = 16U,          /**< Two-byte shift.          */
  k_t_shift_24  = 24U,          /**< Three-byte shift.        */
} t_wt_layout_t;

/* -------------------------------------------------------------------------
 * Static fixtures (host build: plain .bss, no MMIO).
 * ------------------------------------------------------------------------- */

/** @brief Backing store for the hand-built JOF atlas. */
static uint8_t s_atlas[k_t_atlas_cap];

/** @brief Cache cell storage (k_t_wt_cells x k_t_wt_band_bytes). */
static uint8_t s_cells[(size_t)k_t_wt_cells * (size_t)k_t_wt_band_bytes];

static ra8_tile_key_t      s_keys[(size_t)k_t_wt_cells];
static ra8_tile_dims_t     s_dims[(size_t)k_t_wt_cells];
static ra8_keycache_cell_t s_meta[(size_t)k_t_wt_cells];
static int32_t             s_buckets[(size_t)k_t_wt_buckets];

/** @brief Recording blit target: the reconstructed viewport framebuffer. */
static uint8_t s_view_fb[(size_t)k_t_wt_view_h * (size_t)k_t_wt_view_w * 3U];

/** @brief Per-canvas-row "was rendered at least once" flags (0-skip proof). */
static bool s_canvas_covered[k_t_wt_height];

/* -------------------------------------------------------------------------
 * Synthetic strip: pixel generator + raw JOF builder.
 * ------------------------------------------------------------------------- */

/** @brief Deterministic RGB888 pixel: encodes the canvas coordinate. */
static void t_wt_pixel(uint32_t canvas_x, uint32_t canvas_y, uint8_t out[3])
{
  out[0] = (uint8_t)(canvas_y & (uint32_t)k_t_byte_mask);
  out[1] = (uint8_t)((canvas_y >> (uint32_t)k_t_shift_8) & (uint32_t)k_t_byte_mask);
  out[2] = (uint8_t)(canvas_x & (uint32_t)k_t_byte_mask);
}

/** @brief Write a little-endian u16 at @p p. */
static void t_wt_put_u16(uint8_t* p, uint16_t v)
{
  p[0] = (uint8_t)(v & (uint16_t)k_t_byte_mask);
  p[1] = (uint8_t)((v >> (uint16_t)k_t_shift_8) & (uint16_t)k_t_byte_mask);
}

/** @brief Write a little-endian u32 at @p p. */
static void t_wt_put_u32(uint8_t* p, uint32_t v)
{
  p[0] = (uint8_t)(v & (uint32_t)k_t_byte_mask);
  p[1] = (uint8_t)((v >> (uint32_t)k_t_shift_8) & (uint32_t)k_t_byte_mask);
  p[2] = (uint8_t)((v >> (uint32_t)k_t_shift_16) & (uint32_t)k_t_byte_mask);
  p[3] = (uint8_t)((v >> (uint32_t)k_t_shift_24) & (uint32_t)k_t_byte_mask);
}

/**
 * @brief Build a raw (codec 0) JOF band-tile atlas for the synthetic strip.
 *
 * @details Emits header + N raw band payloads + index + footer per the JOF
 *          layout, with `tile_w == width` (one full-width band column). The
 *          result is validated by `ra8_jof_parse()` in the tests, so a
 *          builder bug is caught by the reader's fail-closed checks.
 *
 * @return Total atlas byte length.
 */
static uint32_t t_wt_build_strip(void)
{
  const uint32_t width  = (uint32_t)k_t_wt_width;
  const uint32_t band_h = (uint32_t)k_t_wt_band_h;
  const uint32_t height = (uint32_t)k_t_wt_height;
  const uint32_t bands  = (height + band_h - 1U) / band_h;
  uint8_t*       hdr    = s_atlas;
  (void)memset(hdr, 0, (size_t)k_t_hdr_bytes);
  (void)memcpy(hdr, "JOF1", 4U);
  t_wt_put_u16(&hdr[4], (uint16_t)width);
  t_wt_put_u16(&hdr[6], (uint16_t)height);
  t_wt_put_u16(&hdr[8], (uint16_t)width); /* tile_w == width */
  t_wt_put_u16(&hdr[k_longstrip_t_wt_put_u16_10], (uint16_t)band_h);
  hdr[k_longstrip_val_12] = (uint8_t)k_t_wt_bpp;
  hdr[k_longstrip_val_13] = (uint8_t)k_ra8_jof_codec_raw;
  t_wt_put_u32(&hdr[16], bands);

  uint32_t off = (uint32_t)k_t_hdr_bytes;
  uint32_t tile_off[k_t_wt_bands];
  uint32_t tile_len[k_t_wt_bands];
  for (uint32_t n = 0U; n < bands; n++) {
    const uint32_t y0 = n * band_h;
    const uint32_t th = ((height - y0) < band_h) ? (height - y0) : band_h;
    tile_off[n]       = off;
    tile_len[n]       = width * th * (uint32_t)k_t_wt_bpp;
    for (uint32_t r = 0U; r < th; r++) {
      for (uint32_t x = 0U; x < width; x++) {
        t_wt_pixel(x, y0 + r, &s_atlas[off]);
        off += (uint32_t)k_t_wt_bpp;
      }
    }
  }

  const uint32_t index_off = off;
  for (uint32_t n = 0U; n < bands; n++) {
    t_wt_put_u32(&s_atlas[off], tile_off[n]);
    t_wt_put_u32(&s_atlas[off + 4U], tile_len[n]);
    off += (uint32_t)k_t_idx_entry;
  }
  const uint32_t total = off + (uint32_t)k_t_ftr_bytes;
  t_wt_put_u32(&s_atlas[off], index_off);
  t_wt_put_u32(&s_atlas[off + 4U], bands);
  t_wt_put_u32(&s_atlas[off + 8U], total);
  (void)memcpy(&s_atlas[off + 12U], "JOFE", 4U);
  return total;
}

/* -------------------------------------------------------------------------
 * Harness wiring: memstore pread, tile cache, decode ctx, recording blit.
 * ------------------------------------------------------------------------- */

static ra8_jof_memstore_t   s_store;
static ra8_longstrip_decode_ctx_t s_dctx;
static ra8_tile_cache_t           s_cache;

/** @brief Recording blit context: the current scroll and coverage accounting. */
typedef struct {
  int32_t  scroll_y;   /**< Canvas y of the viewport top for this frame. */
  uint32_t px_checked; /**< Pixels verified this frame.                  */
} t_wt_blit_ctx_t;

static t_wt_blit_ctx_t s_blit;

/**
 * @brief Recording blit: clip into the viewport FB and verify each pixel
 *        equals the generator at its true canvas coordinate (seamless proof).
 */
static ra8_err_t t_wt_blit(void*          ctx,
                           const uint8_t* px,
                           uint16_t       sw,
                           uint16_t       sh,
                           uint8_t        bpp,
                           int32_t        dst_x,
                           int32_t        dst_y)
{
  t_wt_blit_ctx_t* b = (t_wt_blit_ctx_t*)ctx;
  TEST_ASSERT_EQ(k_t_wt_bpp, bpp);
  TEST_ASSERT_EQ(k_t_wt_width, sw);
  for (int32_t row = 0; row < (int32_t)sh; row++) {
    const int32_t fy = dst_y + row;
    if ((fy < 0) || (fy >= (int32_t)k_t_wt_view_h)) {
      continue;
    }
    const uint32_t canvas_y = (uint32_t)(b->scroll_y + fy);
    if (canvas_y < (uint32_t)k_t_wt_height) {
      s_canvas_covered[canvas_y] = true;
    }
    for (int32_t col = 0; col < (int32_t)sw; col++) {
      const int32_t fx = dst_x + col;
      if ((fx < 0) || (fx >= (int32_t)k_t_wt_view_w)) {
        continue;
      }
      const uint8_t* sp = &px[(((size_t)row * (size_t)sw) + (size_t)col) * (size_t)bpp];
      uint8_t        expect[3];
      t_wt_pixel((uint32_t)col, canvas_y, expect);
      TEST_ASSERT_EQ(expect[0], sp[0]);
      TEST_ASSERT_EQ(expect[1], sp[1]);
      TEST_ASSERT_EQ(expect[2], sp[2]);
      uint8_t* dp = &s_view_fb[(((size_t)fy * (size_t)k_t_wt_view_w) + (size_t)fx) * 3U];
      dp[0]       = sp[0];
      dp[1]       = sp[1];
      dp[2]       = sp[2];
      b->px_checked++;
    }
  }
  return k_ra8_ok;
}

/** @brief Build the strip, wire the memstore/cache/decoder and open the strip. */
static void t_wt_open(ra8_longstrip_t* wt)
{
  const uint32_t total = t_wt_build_strip();
  s_store = (ra8_jof_memstore_t){.buf = s_atlas, .cap = k_t_atlas_cap, .len = total};

  s_dctx.pread       = ra8_jof_memstore_pread;
  s_dctx.pread_ctx   = &s_store;
  s_dctx.scratch     = nullptr;
  s_dctx.scratch_cap = 0U;
  /* Parse once so the decode ctx carries validated geometry. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_jof_parse(ra8_jof_memstore_pread, &s_store, total, &s_dctx.info));

  const ra8_tile_cache_cfg_t ccfg = {.cell_mem     = s_cells,
                                     .cell_bytes   = (uint32_t)k_t_wt_band_bytes,
                                     .cell_count   = (uint32_t)k_t_wt_cells,
                                     .meta         = s_meta,
                                     .keys         = s_keys,
                                     .dims         = s_dims,
                                     .buckets      = s_buckets,
                                     .bucket_count = (uint32_t)k_t_wt_buckets,
                                     .decode       = ra8_longstrip_tile_decode,
                                     .decode_ctx   = &s_dctx};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tile_cache_init(&s_cache, &ccfg));

  const ra8_longstrip_cfg_t wcfg = {.pread      = ra8_jof_memstore_pread,
                                    .pread_ctx  = &s_store,
                                    .atlas_size = total,
                                    .cache      = &s_cache,
                                    .image_id   = 1U,
                                    .viewport_w = (uint16_t)k_t_wt_view_w,
                                    .viewport_h = (uint16_t)k_t_wt_view_h,
                                    .blit       = t_wt_blit,
                                    .blit_ctx   = &s_blit};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_longstrip_open(wt, &wcfg));
}

/* =========================================================================
 * Tests
 * ========================================================================= */

/**
 * @test open_validates_inputs
 *
 * @par MC/DC:
 * Decision A `if (viewport_w == 0 || viewport_h == 0)` (2 conditions):
 * - V1: w=192, h=256 -> false (both false: valid open)
 * - V2: w=0,   h=256 -> true  (varies w only)
 * - V3: w=192, h=0   -> true  (varies h only)
 * V1+V2 prove w's independent influence; V1+V3 prove h's. N+1 = 3 vectors.
 *
 * The full-width band-column check `if (info.tile_w != info.width)` is a single
 * condition (not compound): `ra8_jof_parse` derives tile_cols as
 * ceil(width / tile_w), so tile_w == width already implies tile_cols == 1 and a
 * separate `tile_cols != 1` test could never independently flip the outcome. Its
 * false branch is the valid strip (V1 here); its true branch is
 * ::t_open_rejects_non_band.
 */
static void t_open_validates(void)
{
  TEST_BEGIN("open_validates_inputs");
  ra8_longstrip_t wt = {};
  t_wt_open(&wt); /* Decision A V1 + Decision B control: valid open */
  TEST_ASSERT_EQ(k_t_wt_bands, wt.band_count);
  TEST_ASSERT_EQ(k_t_wt_band_h, wt.band_h);
  TEST_ASSERT_EQ(k_t_wt_height, wt.canvas_h);
  TEST_ASSERT_EQ(k_t_wt_max_scroll, wt.max_scroll);

  const uint32_t      total = s_store.len;
  ra8_longstrip_cfg_t cfg   = {.pread      = ra8_jof_memstore_pread,
                               .pread_ctx  = &s_store,
                               .atlas_size = total,
                               .cache      = &s_cache,
                               .image_id   = 1U,
                               .viewport_w = (uint16_t)k_t_wt_view_w,
                               .viewport_h = (uint16_t)k_t_wt_view_h,
                               .blit       = t_wt_blit,
                               .blit_ctx   = &s_blit};

  /* Decision A V2: viewport_w == 0 -> invalid_arg. */
  cfg.viewport_w = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_longstrip_open(&wt, &cfg));
  cfg.viewport_w = (uint16_t)k_t_wt_view_w;
  /* Decision A V3: viewport_h == 0 -> invalid_arg. */
  cfg.viewport_h = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_longstrip_open(&wt, &cfg));
  cfg.viewport_h = (uint16_t)k_t_wt_view_h;

  /* Null-seam guards. */
  ra8_longstrip_cfg_t nn = cfg;
  nn.pread               = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_longstrip_open(&wt, &nn));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_longstrip_open(nullptr, &cfg));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_longstrip_open(&wt, nullptr));
  TEST_END("open_validates_inputs");
}

/**
 * @test open_rejects_non_band_atlas
 *
 * @par MC/DC:
 * Single-condition guard `if (info.tile_w != info.width)` (no compound): the
 * two branches are the valid strip (false; covered by open_validates_inputs V1)
 * and this narrow-tile atlas (tile_w = width/2 => tile_w != width true). A
 * separate `tile_cols != 1` test was removed as provably redundant --
 * `ra8_jof_parse` derives tile_cols as ceil(width / tile_w), so
 * tile_w == width already forces tile_cols == 1 and tile_cols could never
 * independently flip the outcome (its true arm is a strict subset of this one).
 */
static void t_open_rejects_non_band(void)
{
  TEST_BEGIN("open_rejects_non_band_atlas");
  /* Re-tile the SAME pixel dimensions with tile_w = width/2 -> 2 columns, so
   * tile_w != width AND tile_cols != 1. ra8_longstrip_open must reject it. */
  const uint16_t width  = 64U;
  const uint16_t height = 128U;
  const uint16_t tw     = 32U; /* half width -> 2 columns */
  const uint16_t th     = 64U;
  (void)memset(s_atlas, 0, (size_t)k_t_hdr_bytes);
  (void)memcpy(s_atlas, "JOF1", 4U);
  t_wt_put_u16(&s_atlas[4], width);
  t_wt_put_u16(&s_atlas[6], height);
  t_wt_put_u16(&s_atlas[8], tw);
  t_wt_put_u16(&s_atlas[k_longstrip_t_wt_put_u16_10], th);
  s_atlas[k_longstrip_val_12] = 1U; /* gray8 (bpp 1) keeps the atlas tiny */
  s_atlas[k_longstrip_val_13] = (uint8_t)k_ra8_jof_codec_raw;
  const uint32_t cols         = (uint32_t)((width + tw - 1U) / tw);
  const uint32_t rows         = (uint32_t)((height + th - 1U) / th);
  const uint32_t tiles        = cols * rows;
  t_wt_put_u32(&s_atlas[16], tiles);
  uint32_t off = (uint32_t)k_t_hdr_bytes;
  uint32_t offs[8];
  uint32_t lens[8];
  for (uint32_t n = 0U; n < tiles; n++) {
    offs[n] = off;
    lens[n] = (uint32_t)tw * (uint32_t)th; /* bpp 1 */
    (void)memset(&s_atlas[off], (int)n, (size_t)lens[n]);
    off += lens[n];
  }
  const uint32_t index_off = off;
  for (uint32_t n = 0U; n < tiles; n++) {
    t_wt_put_u32(&s_atlas[off], offs[n]);
    t_wt_put_u32(&s_atlas[off + 4U], lens[n]);
    off += (uint32_t)k_t_idx_entry;
  }
  const uint32_t total = off + (uint32_t)k_t_ftr_bytes;
  t_wt_put_u32(&s_atlas[off], index_off);
  t_wt_put_u32(&s_atlas[off + 4U], tiles);
  t_wt_put_u32(&s_atlas[off + 8U], total);
  (void)memcpy(&s_atlas[off + 12U], "JOFE", 4U);

  s_store = (ra8_jof_memstore_t){.buf = s_atlas, .cap = k_t_atlas_cap, .len = total};
  const ra8_longstrip_cfg_t cfg = {.pread      = ra8_jof_memstore_pread,
                                   .pread_ctx  = &s_store,
                                   .atlas_size = total,
                                   .cache      = &s_cache,
                                   .image_id   = 2U,
                                   .viewport_w = 64U,
                                   .viewport_h = 64U,
                                   .blit       = t_wt_blit,
                                   .blit_ctx   = &s_blit};
  ra8_longstrip_t           wt  = {};
  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_longstrip_open(&wt, &cfg));
  TEST_END("open_rejects_non_band_atlas");
}

/**
 * @test geometry_band_math
 *
 * @details O(1) y->band, visible-band range and clamp behaviour at the ends and
 *          the middle of the strip.
 */
static void t_geometry(void)
{
  TEST_BEGIN("geometry_band_math");
  ra8_longstrip_t wt = {};
  t_wt_open(&wt);

  uint16_t band = k_longstrip_band_ffff;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_longstrip_band_at_y(&wt, 0U, &band));
  TEST_ASSERT_EQ(0, band);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_longstrip_band_at_y(&wt, 63U, &band));
  TEST_ASSERT_EQ(0, band);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_longstrip_band_at_y(&wt, 64U, &band));
  TEST_ASSERT_EQ(1, band);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_longstrip_band_at_y(&wt, 799U, &band));
  TEST_ASSERT_EQ(12, band); /* last (edge) band */
  TEST_ASSERT_EQ(k_ra8_err_out_of_range, ra8_longstrip_band_at_y(&wt, 800U, &band));

  /* clamp: below/above/inside. */
  TEST_ASSERT_EQ(0, ra8_longstrip_clamp_scroll(&wt, -100));
  TEST_ASSERT_EQ(k_t_wt_max_scroll, ra8_longstrip_clamp_scroll(&wt, 100000));
  TEST_ASSERT_EQ(300, ra8_longstrip_clamp_scroll(&wt, 300));

  /* visible range at the top: rows [0,256) -> bands 0..3. */
  uint16_t first = k_longstrip_first_ff;
  uint16_t last  = k_longstrip_first_ff;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_longstrip_visible_bands(&wt, 0, &first, &last));
  TEST_ASSERT_EQ(0, first);
  TEST_ASSERT_EQ(3, last);
  /* at the bottom: rows [544,800) -> bands 8..12. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_longstrip_visible_bands(&wt, (int32_t)k_t_wt_max_scroll, &first, &last));
  TEST_ASSERT_EQ(8, first);
  TEST_ASSERT_EQ(12, last);
  TEST_END("geometry_band_math");
}

/**
 * @test scroll_zero_skip_full_traversal
 *
 * @details The #289 contract. Scrolls the whole strip top -> bottom in 40-px
 *          steps; at every step `ra8_longstrip_render` must report `skipped == 0`
 *          and `covered_rows == min(view_h, canvas_h - scroll_y)`, and the
 *          recording blit verifies every composited pixel equals the strip
 *          generator at its true canvas coordinate (no seam, no gap). After the
 *          traversal every one of the 800 canvas rows must have been rendered at
 *          least once -- zero skipped bands across the whole strip.
 */
static void t_zero_skip(void)
{
  TEST_BEGIN("scroll_zero_skip_full_traversal");
  ra8_longstrip_t wt = {};
  t_wt_open(&wt);
  (void)memset(s_canvas_covered, 0, sizeof(s_canvas_covered));

  const int32_t step = 40;
  bool          done = false;
  for (int32_t target = 0; !done; target += step) {
    /* Clamp each step and guarantee the final frame lands exactly on
     * max_scroll so the last (edge) band's rows are scrolled into view. */
    int32_t t = (target >= (int32_t)k_t_wt_max_scroll) ? (int32_t)k_t_wt_max_scroll : target;
    if (t == (int32_t)k_t_wt_max_scroll) {
      done = true;
    }
    /* seek to `t` via a direct scroll (fast-seek path). */
    (void)ra8_longstrip_scroll_by(&wt, t - wt.scroll_y);
    TEST_ASSERT_EQ(t, wt.scroll_y);
    s_blit.scroll_y   = wt.scroll_y;
    s_blit.px_checked = 0U;

    ra8_longstrip_render_stats_t st = {};
    TEST_ASSERT_EQ(k_ra8_ok, ra8_longstrip_render(&wt, &st));
    TEST_ASSERT_EQ(0, st.skipped); /* ZERO skipped bands */
    const uint32_t expect_rows =
      ((uint32_t)k_t_wt_height - (uint32_t)wt.scroll_y < (uint32_t)k_t_wt_view_h)
        ? ((uint32_t)k_t_wt_height - (uint32_t)wt.scroll_y)
        : (uint32_t)k_t_wt_view_h;
    TEST_ASSERT_EQ(expect_rows, st.covered_rows);
    TEST_ASSERT(st.bands_drawn >= 4U);
    TEST_ASSERT(s_blit.px_checked > 0U);
  }

  /* Every canvas row rendered at least once: definitive 0-skip. */
  for (uint32_t y = 0U; y < (uint32_t)k_t_wt_height; y++) {
    if (!s_canvas_covered[y]) {
      TEST_FAIL_FMT("canvas row %u never rendered (a band was skipped)", y);
    }
  }
  TEST_END("scroll_zero_skip_full_traversal");
}

/**
 * @test scroll_by_end_pin
 *
 * @par MC/DC:
 * Decision `if (scroll_y == 0 || scroll_y == max_scroll)` -> zero velocity
 * (2 conditions):
 * - V1: scroll_y=300 (mid), max=544 -> false (both false: velocity kept)
 * - V2: scroll_y=0,   max=544 -> true  (varies condition 1)
 * - V3: scroll_y=544, max=544 -> true  (varies condition 2)
 * V1+V2 prove condition 1's influence; V1+V3 prove condition 2's. N+1 = 3.
 */
static void t_scroll_by_end_pin(void)
{
  TEST_BEGIN("scroll_by_end_pin");
  ra8_longstrip_t wt = {};
  t_wt_open(&wt);

  /* V1: land mid-strip with a live velocity -> velocity retained. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_longstrip_fling(&wt, 7));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_longstrip_scroll_by(&wt, 300));
  TEST_ASSERT_EQ(300, wt.scroll_y);
  TEST_ASSERT_EQ(7, wt.velocity);

  /* V3: drive to the bottom -> pinned at max_scroll -> velocity zeroed. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_longstrip_fling(&wt, 9));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_longstrip_scroll_by(&wt, 100000));
  TEST_ASSERT_EQ(k_t_wt_max_scroll, wt.scroll_y);
  TEST_ASSERT_EQ(0, wt.velocity);

  /* V2: drive to the top -> pinned at 0 -> velocity zeroed. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_longstrip_fling(&wt, -9));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_longstrip_scroll_by(&wt, -100000));
  TEST_ASSERT_EQ(0, wt.scroll_y);
  TEST_ASSERT_EQ(0, wt.velocity);
  TEST_END("scroll_by_end_pin");
}

/**
 * @test fling_converges_and_clamps
 *
 * @par MC/DC:
 * `wt_fling_should_stop` tests the velocity sign once --
 * `return (velocity < 0) ? at_top : at_bottom;` -- so there is no compound
 * decision to cover (the caller guarantees velocity != 0, which makes
 * `velocity > 0` merely the negation of `velocity < 0`; folding both sign tests
 * into one `&&`/`||` expression would leave the second sign condition
 * structurally unable to flip the outcome independently). Both single-condition
 * branches are exercised: the
 * upward fling drives `velocity < 0 -> at_top`, the downward fling
 * `velocity > 0 -> at_bottom` (each returning both false, mid-fling, and true,
 * pinned). Also proves bounded convergence: the fling stops in finite ticks.
 */
static void t_fling(void)
{
  TEST_BEGIN("fling_converges_and_clamps");
  ra8_longstrip_t wt = {};
  t_wt_open(&wt);

  /* Downward fling from the top (velocity > 0 -> at_bottom arm): starts at the
   * top (moving away from it), runs through the middle, converges pinned at the
   * bottom -- the at_bottom branch returns false mid-fling and true when pinned. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_longstrip_fling(&wt, 200));
  int32_t ticks          = 0;
  bool    moved_from_top = false;
  while (ra8_longstrip_tick(&wt)) {
    if (wt.scroll_y > 0) {
      moved_from_top = true;
    }
    ticks++;
    TEST_ASSERT(ticks < 1000); /* bounded convergence */
  }
  TEST_ASSERT(moved_from_top);
  TEST_ASSERT_EQ(0, wt.velocity);
  TEST_ASSERT(wt.scroll_y > 0); /* it advanced downward */

  /* Upward fling from the bottom (velocity < 0 -> at_top arm): pins at the top. */
  (void)ra8_longstrip_scroll_by(&wt, k_longstrip_ra8_longstrip_scroll_by_100000);
  TEST_ASSERT_EQ(k_t_wt_max_scroll, wt.scroll_y);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_longstrip_fling(&wt, -200));
  ticks = 0;
  while (ra8_longstrip_tick(&wt)) {
    ticks++;
    TEST_ASSERT(ticks < 1000);
  }
  TEST_ASSERT_EQ(0, wt.velocity);
  TEST_ASSERT(wt.scroll_y < (int32_t)k_t_wt_max_scroll);

  /* A zero-velocity tick is immediately at rest. */
  TEST_ASSERT(!ra8_longstrip_tick(&wt));
  TEST_END("fling_converges_and_clamps");
}

/**
 * @test prefetch_bounded
 *
 * @par MC/DC:
 * Decision `if (depth == 0 || band_count <= visible_span)` -> skip (2 cond):
 * - V1: depth=2, band_count=13 > span -> false (prefetch runs)
 * - V2: depth=0, band_count=13        -> true  (varies condition 1)
 * - V3: depth=2 but a strip that fits the viewport (band_count <= span) -> true
 *       (varies condition 2)
 * V1+V2 prove depth's influence; V1+V3 prove the fits-in-viewport condition's.
 * Also proves the warm is bounded: prefetch never leaves a pin held, so a
 * later render still succeeds with the small cache.
 */
static void t_prefetch(void)
{
  TEST_BEGIN("prefetch_bounded");
  ra8_longstrip_t wt = {};
  t_wt_open(&wt);

  uint32_t hits0 = 0;
  uint32_t miss0 = 0;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tile_cache_stats(&s_cache, &hits0, &miss0, nullptr));

  /* V2: depth 0 -> no prefetch, no new misses. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_longstrip_prefetch(&wt, 0U));
  uint32_t miss1 = 0;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tile_cache_stats(&s_cache, nullptr, &miss1, nullptr));
  TEST_ASSERT_EQ(miss0, miss1);

  /* V1: downward prefetch (at rest -> downward) warms bands below the view. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_longstrip_fling(&wt, 5)); /* velocity>=0 -> downward */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_longstrip_prefetch(&wt, 2U));
  uint32_t miss2 = 0;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tile_cache_stats(&s_cache, nullptr, &miss2, nullptr));
  TEST_ASSERT(miss2 > miss1); /* prefetch decoded ahead */

  /* prefetch left no pin held: a full render still succeeds with 4 cells. */
  s_blit.scroll_y                 = wt.scroll_y;
  s_blit.px_checked               = 0U;
  ra8_longstrip_render_stats_t st = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_longstrip_render(&wt, &st));
  TEST_ASSERT_EQ(0, st.skipped);

  /* V3: a short strip that fully fits the viewport -> band_count <= span. */
  ra8_longstrip_t small = wt;
  small.band_count      = 1U; /* pretend one band that fits: skip branch taken */
  small.scroll_y        = 0;
  small.max_scroll      = 0;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_longstrip_prefetch(&small, 2U));
  TEST_END("prefetch_bounded");
}

/**
 * @test bounded_resident_memory
 *
 * @details The cache holds at most `cell_count` bands regardless of strip
 *          height or scroll distance: a full traversal of 13 bands through a
 *          4-cell cache must evict (>= 9 evictions) yet every visible band still
 *          renders, and re-visiting the top re-decodes an evicted band (proof it
 *          was reclaimed, not leaked).
 */
static void t_bounded_memory(void)
{
  TEST_BEGIN("bounded_resident_memory");
  ra8_longstrip_t wt = {};
  t_wt_open(&wt);

  for (int32_t target = 0; target <= (int32_t)k_t_wt_max_scroll; target += 32) {
    (void)ra8_longstrip_scroll_by(&wt, target - wt.scroll_y);
    s_blit.scroll_y                 = wt.scroll_y;
    ra8_longstrip_render_stats_t st = {};
    TEST_ASSERT_EQ(k_ra8_ok, ra8_longstrip_render(&wt, &st));
    TEST_ASSERT_EQ(0, st.skipped);
  }
  uint32_t ev = 0;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tile_cache_stats(&s_cache, nullptr, nullptr, &ev));
  TEST_ASSERT(ev >= (uint32_t)(k_t_wt_bands - k_t_wt_cells)); /* forced eviction */

  /* Return to the top: band 0 was long evicted -> a fresh miss decodes it. */
  uint32_t miss_before = 0;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tile_cache_stats(&s_cache, nullptr, &miss_before, nullptr));
  (void)ra8_longstrip_scroll_by(&wt, -k_longstrip_ra8_longstrip_scroll_by_100000);
  s_blit.scroll_y                 = wt.scroll_y;
  ra8_longstrip_render_stats_t st = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_longstrip_render(&wt, &st));
  uint32_t miss_after = 0;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tile_cache_stats(&s_cache, nullptr, &miss_after, nullptr));
  TEST_ASSERT(miss_after > miss_before); /* band 0 was reclaimed and re-decoded */
  TEST_END("bounded_resident_memory");
}

/**
 * @test t_set_viewport_validates
 *
 * @par MC/DC:
 * Decision: `if ((viewport_w == 0U) || (viewport_h == 0U))` (2 conditions)
 * - Vector 1: w=192, h=256 -> false (control: both conditions false)
 * - Vector 2: w=0,   h=256 -> true  (varies viewport_w only)
 * - Vector 3: w=192, h=0   -> true  (varies viewport_h only)
 * Vectors 1+2 prove viewport_w independently affects the outcome; 1+3 prove
 * the same for viewport_h. N+1 = 3 vectors for N=2 conditions: minimal MC/DC.
 */
static void t_set_viewport_validates(void)
{
  TEST_BEGIN("set_viewport_validates");
  ra8_longstrip_t wt = {};
  t_wt_open(&wt);
  /* Vector 1: both non-zero -> accepted, clamp re-derived from the new height. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_longstrip_set_viewport(&wt, k_t_wt_view_w, (uint16_t)k_t_wt_view_h));
  TEST_ASSERT_EQ(k_t_wt_max_scroll, wt.max_scroll);
  /* Vector 2: zero width only. Vector 3: zero height only. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_longstrip_set_viewport(&wt, 0U, k_t_wt_view_h));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_longstrip_set_viewport(&wt, k_t_wt_view_w, 0U));
  /* A rejected call leaves the previous viewport and clamp untouched. */
  TEST_ASSERT_EQ(k_t_wt_max_scroll, wt.max_scroll);
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_longstrip_set_viewport(nullptr, 1U, 1U));
  TEST_END("set_viewport_validates");
}

/**
 * @test t_paginated_last_page_is_not_clamped
 *
 * @details
 * Regression for the duplicated-content render defect: a paginated caller that
 * seeks to `page * page_h` while the engine still holds the FULL page height as
 * its viewport has its final seek clamped to `canvas_h - page_h`, so the last
 * page silently re-composites a window the previous page already showed.
 *
 * The strip is 800 tall and the page height is 256, so `ceil(800/256) == 4`
 * pages and the last page starts at 768 with only 32 rows of content -- a
 * deliberately non-multiple geometry (800 = 3*256 + 32). Setting the viewport to
 * each page's true content height before the seek makes every page land on its
 * own canvas rows, which the recording blit verifies per pixel.
 */
static void t_paginated_last_page_is_not_clamped(void)
{
  TEST_BEGIN("paginated_last_page_is_not_clamped");
  ra8_longstrip_t wt = {};
  t_wt_open(&wt);
  const uint32_t page_h = (uint32_t)k_t_wt_view_h;
  const uint32_t pages  = ((uint32_t)k_t_wt_height + page_h - 1U) / page_h;
  TEST_ASSERT_EQ(4U, pages); /* 800 / 256 -> 4 pages, the last one short */
  for (uint32_t p = 0U; p < pages; p++) {
    const uint32_t top     = p * page_h;
    const uint32_t remain  = (uint32_t)k_t_wt_height - top;
    const uint32_t content = (remain < page_h) ? remain : page_h;
    TEST_ASSERT_EQ(k_ra8_ok, ra8_longstrip_set_viewport(&wt, k_t_wt_view_w, (uint16_t)content));
    TEST_ASSERT_EQ(k_ra8_ok, ra8_longstrip_scroll_by(&wt, (int32_t)top - wt.scroll_y));
    /* The seek must land exactly on the requested page top -- never short. */
    TEST_ASSERT_EQ(top, wt.scroll_y);
    s_blit.scroll_y                 = wt.scroll_y;
    ra8_longstrip_render_stats_t st = {};
    TEST_ASSERT_EQ(k_ra8_ok, ra8_longstrip_render(&wt, &st));
    /* The recording blit asserts every pixel against its true canvas
     * coordinate, so a duplicated window fails inside t_wt_blit first. */
    TEST_ASSERT_EQ(content, st.covered_rows);
  }
  /* The final page is the short one: 800 - 3*256 == 32 rows. */
  TEST_ASSERT_EQ((k_t_wt_height - k_t_wt_last_band_h), wt.scroll_y);
  TEST_END("paginated_last_page_is_not_clamped");
}

/**
 * @brief Host test entry point.
 *
 * @return 0 on success; any assertion `exit(1)`s before returning.
 */
int32_t main(void)
{
  t_open_validates();
  t_open_rejects_non_band();
  t_geometry();
  t_zero_skip();
  t_scroll_by_end_pin();
  t_fling();
  t_prefetch();
  t_bounded_memory();
  t_set_viewport_validates();
  t_paginated_last_page_is_not_clamped();
  (void)fprintf(stderr, "[PASS] test_ra8_longstrip: all cases passed\n");
  return 0;
}
