/**
 * @file test_app_ereader_manga.c
 * @brief Host twin of examples/ek_ra8d2/hw_pending/ereader_manga: the #231
 *        import-transcode + tile-render pipeline over the same baked fixture.
 *
 * @details
 * Runs the app's exact pipeline against the app's exact fixture header:
 * streamed EPUB open -> `ra8_epub_tile_binder_import("page1.png")` (PNG ->
 * deflate RTA1 atlas in a memstore) -> a 160x120 centre-crop render through
 * a 2-cell tile cache -> FNV-1a-32 over the RGB565 framebuffer. Every stage
 * is a deterministic integer pipeline, so the asserted numbers ARE the
 * `hil.conf` banner: this test is the golden the board_sim / silicon run is
 * compared against. It also re-derives the fixture's pixel pattern
 * independently and byte-checks a paged tile against it, so the committed
 * fixture and the generator formula cannot drift apart silently.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../examples/ek_ra8d2/hw_pending/ereader_manga/manga_hil_fixture.h"
#include "ra8_epub.h"
#include "ra8_epub_img_tiles.h"
#include "ra8_err.h"
#include "ra8_tile_cache.h"
#include "ra8_tileatlas.h"
#include "ra8_tileatlas_produce.h"
#include "unity_minimal.h"

/** @brief The app's geometry + budget constants (kept in lockstep with main.c). */
enum : uint32_t {
  k_t_fb_w       = 160U,          /**< Framebuffer width, pixels.        */
  k_t_fb_h       = 120U,          /**< Framebuffer height, pixels.       */
  k_t_img_edge   = 512U,          /**< Fixture page edge, pixels.        */
  k_t_tile_edge  = 128U,          /**< Import tile edge, pixels.         */
  k_t_cap_edge   = 1024U,         /**< Import budget cap, pixels.        */
  k_t_cell_bytes = 16384U,        /**< Cache cell = one gray8 tile.      */
  k_t_cells      = 2U,            /**< Deliberately tiny (evictions).    */
  k_t_buckets    = 8U,            /**< Cache hash buckets.               */
  k_t_scratch    = 18688U,        /**< stored_bound(16384) staging.      */
  k_t_work       = 1024U * 1024U, /**< Producer work arena.              */
  k_t_store      = 512U * 1024U,  /**< Atlas memstore.                   */
  k_t_image_id   = 1U,            /**< Binder image id for the page.     */
  k_t_view_x0    = 176U,          /**< Crop left edge (centre 160x120).  */
  k_t_view_y0    = 196U,          /**< Crop top edge (centre 160x120).   */
  k_t_fnv_offset = 2166136261U,   /**< FNV-1a-32 offset basis.           */
  k_t_fnv_prime  = 16777619U,     /**< FNV-1a-32 prime.                  */
  k_t_col_bg     = 0x202028U,     /**< App clear colour (unused pixels). */
  k_t_tiles_want = 16U,           /**< Expected 4x4 tile grid.           */
};

/**
 * @brief Golden banner numbers (must match the app's UART banner and the
 *        `hil.conf` HIL_EXPECT line).
 */
enum : uint32_t {
  k_t_golden_atlas_bytes = 67331U,      /**< Expected atlas total_size. */
  k_t_golden_crc         = 0x4095FBB5U, /**< Expected framebuffer FNV.  */
};

/** @brief RGB565 framebuffer (the app's render target twin). */
static uint16_t s_fb[(size_t)k_t_fb_h * (size_t)k_t_fb_w];
/** @brief The open streamed book. */
static ra8_epub_book_t s_book;
/** @brief The tile binder. */
static ra8_epub_tile_binder_t s_binder;
/** @brief Tile-cache cell storage. */
static uint8_t s_cell_mem[(size_t)k_t_cells * (size_t)k_t_cell_bytes];
/** @brief Tile-cache keys. */
static ra8_tile_key_t s_keys[k_t_cells];
/** @brief Tile-cache dims. */
static ra8_tile_dims_t s_dims[k_t_cells];
/** @brief Tile-cache metadata. */
static ra8_keycache_cell_t s_meta[k_t_cells];
/** @brief Tile-cache buckets. */
static int32_t s_buckets[k_t_buckets];
/** @brief Binder stored-tile staging. */
static uint8_t s_scratch[k_t_scratch];
/** @brief Producer work arena. */
static uint8_t s_work[k_t_work];
/** @brief Atlas memstore backing. */
static uint8_t s_store_buf[k_t_store];
/** @brief Atlas memstore. */
static ra8_tileatlas_memstore_t s_store;

/** @brief The fixture's generator pattern (independent re-derivation). */
static uint8_t fixture_pix(uint32_t x, uint32_t y)
{
  return (uint8_t)((x ^ y) + ((x + y) >> 2U));
}

/** @brief Streamed-media read over the baked fixture. */
static size_t media_read(void* ctx, uint64_t offset, void* buf, size_t len)
{
  (void)ctx;
  if (offset >= (uint64_t)k_manga_epub_len) {
    return 0U;
  }
  const uint64_t avail = (uint64_t)k_manga_epub_len - offset;
  const size_t   take  = (len > (size_t)avail) ? (size_t)avail : len;
  memcpy(buf, &k_manga_epub[(size_t)offset], take);
  return take;
}

/** @brief The app's gray8 -> RGB565 pack. */
static uint16_t pack565(uint8_t g)
{
  return (uint16_t)(((uint16_t)(g >> 3U) << 11U) | ((uint16_t)(g >> 2U) << 5U) |
                    (uint16_t)(g >> 3U));
}

/** @brief FNV-1a-32 over the framebuffer bytes (the app's hash twin). */
static uint32_t fb_hash(void)
{
  const uint8_t* p   = (const uint8_t*)s_fb;
  uint32_t       hsh = (uint32_t)k_t_fnv_offset;
  for (size_t i = 0U; i < sizeof(s_fb); i++) {
    hsh = (hsh ^ (uint32_t)p[i]) * (uint32_t)k_t_fnv_prime;
  }
  return hsh;
}

/** @brief Blit one pinned tile's viewport overlap (the app's blit twin). */
static void blit_tile(const ra8_tile_t* tile, uint16_t tx, uint16_t ty)
{
  const uint32_t px0 = (uint32_t)tx * (uint32_t)k_t_tile_edge;
  const uint32_t py0 = (uint32_t)ty * (uint32_t)k_t_tile_edge;
  for (uint32_t r = 0U; r < (uint32_t)tile->height; r++) {
    const uint32_t iy = py0 + r;
    if ((iy < (uint32_t)k_t_view_y0) || (iy >= ((uint32_t)k_t_view_y0 + (uint32_t)k_t_fb_h))) {
      continue;
    }
    for (uint32_t c = 0U; c < (uint32_t)tile->width; c++) {
      const uint32_t ix = px0 + c;
      if ((ix < (uint32_t)k_t_view_x0) || (ix >= ((uint32_t)k_t_view_x0 + (uint32_t)k_t_fb_w))) {
        continue;
      }
      s_fb[((iy - (uint32_t)k_t_view_y0) * (uint32_t)k_t_fb_w) + (ix - (uint32_t)k_t_view_x0)] =
        pack565(tile->pixels[(r * (uint32_t)tile->width) + c]);
    }
  }
}

/**
 * @test test_manga_app_pipeline
 * @brief The full app pipeline over the committed fixture reproduces the
 *        golden banner numbers (atlas byte count + framebuffer CRC) --
 *        the exact values `hil.conf` asserts on board_sim / silicon.
 *
 * @par MC/DC:
 * (integration gate: the compound decisions inside the producer, atlas
 * reader and binder carry their vectors in test_ra8_tileatlas*.c and
 * test_ra8_epub_img_tiles.c; here the oracle is the end-to-end golden.)
 */
static void test_manga_app_pipeline(void)
{
  TEST_BEGIN("ereader_manga app twin: import + tile render == golden banner");
  const ra8_epub_stream_media_t media = {.read = media_read,
                                         .ctx  = NULL,
                                         .size = (uint64_t)k_manga_epub_len};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_open_streamed(&media, "manga.epub", &s_book));

  const ra8_tile_cache_cfg_t storage = {.cell_mem     = s_cell_mem,
                                        .cell_bytes   = (uint32_t)k_t_cell_bytes,
                                        .cell_count   = (uint32_t)k_t_cells,
                                        .meta         = s_meta,
                                        .keys         = s_keys,
                                        .dims         = s_dims,
                                        .buckets      = s_buckets,
                                        .bucket_count = (uint32_t)k_t_buckets};
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_epub_tile_binder_init(&s_binder, &storage, s_scratch, (uint32_t)sizeof(s_scratch)));

  s_store = (ra8_tileatlas_memstore_t){.buf = s_store_buf, .cap = sizeof(s_store_buf), .len = 0U};
  const ra8_epub_atlas_import_cfg_t cfg = {
    .tile_w     = (uint16_t)k_t_tile_edge,
    .tile_h     = (uint16_t)k_t_tile_edge,
    .codec      = (uint8_t)k_ra8_tileatlas_codec_deflate,
    .max_width  = (uint16_t)k_t_cap_edge,
    .max_height = (uint16_t)k_t_cap_edge,
    .work       = s_work,
    .work_cap   = sizeof(s_work),
    .store      = {.sink      = ra8_tileatlas_memstore_sink,
                   .sink_ctx  = &s_store,
                   .pread     = ra8_tileatlas_memstore_pread,
                   .pread_ctx = &s_store},
  };
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_epub_tile_binder_import(&s_binder, &s_book, "page1.png", k_t_image_id, &cfg));

  ra8_tileatlas_info_t info = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_tile_binder_info(&s_binder, k_t_image_id, &info));
  TEST_ASSERT_EQ(k_t_img_edge, info.width);
  TEST_ASSERT_EQ(k_t_img_edge, info.height);
  TEST_ASSERT_EQ(k_t_tiles_want, info.tile_count);

  /* Fixture <-> generator-pattern parity on one paged tile. */
  ra8_tile_t probe = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_tile_binder_get(&s_binder, k_t_image_id, 1U, 1U, &probe));
  for (uint32_t r = 0U; r < probe.height; r += 17U) {
    for (uint32_t c = 0U; c < probe.width; c += 13U) {
      TEST_ASSERT_EQ(fixture_pix((uint32_t)k_t_tile_edge + c, (uint32_t)k_t_tile_edge + r),
                     probe.pixels[(r * probe.width) + c]);
    }
  }
  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_tile_binder_put(&s_binder, probe.pixels));

  /* The app's clear + centre-crop render through the 2-cell cache. */
  for (size_t i = 0U; i < ((size_t)k_t_fb_h * (size_t)k_t_fb_w); i++) {
    /* The app clears via ra8_gfx_clear(0x202028): RGB565 pack of the bg. */
    s_fb[i] = (uint16_t)((((uint32_t)k_t_col_bg >> 19U) << 11U) |
                         ((((uint32_t)k_t_col_bg >> 10U) & 0x3FU) << 5U) |
                         (((uint32_t)k_t_col_bg >> 3U) & 0x1FU));
  }
  const uint16_t tx0 = (uint16_t)(k_t_view_x0 / k_t_tile_edge);
  const uint16_t ty0 = (uint16_t)(k_t_view_y0 / k_t_tile_edge);
  const uint16_t tx1 = (uint16_t)((k_t_view_x0 + k_t_fb_w - 1U) / k_t_tile_edge);
  const uint16_t ty1 = (uint16_t)((k_t_view_y0 + k_t_fb_h - 1U) / k_t_tile_edge);
  for (uint16_t ty = ty0; ty <= ty1; ty++) {
    for (uint16_t tx = tx0; tx <= tx1; tx++) {
      ra8_tile_t tile = {};
      TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_tile_binder_get(&s_binder, k_t_image_id, tx, ty, &tile));
      blit_tile(&tile, tx, ty);
      TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_tile_binder_put(&s_binder, tile.pixels));
    }
  }

  /* The golden banner numbers. */
  const uint32_t crc = fb_hash();
  (void)fprintf(stderr,
                "[INFO] ereader-manga golden: import %ux%u tiles=%u atlas=%u crc=%08X\n",
                (unsigned)info.width,
                (unsigned)info.height,
                (unsigned)info.tile_count,
                (unsigned)info.total_size,
                (unsigned)crc);
  TEST_ASSERT_EQ(k_t_golden_atlas_bytes, info.total_size);
  TEST_ASSERT_EQ(k_t_golden_crc, crc);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_epub_close(&s_book));
  TEST_END("ereader_manga app twin: import + tile render == golden banner");
}

/**
 * @brief Test entry point -- runs the app-twin gate.
 * @return 0 on success; unity_minimal.h exits non-zero on first failure.
 * @pre None.
 * @pre None.
 * @post The pipeline ran (or the process exited on first failure).
 * @post stderr carries the derived golden banner numbers.
 * @note Not thread-safe. No SIGALRM / timers used.
 * @since 0.1.0
 */
int32_t main(void)
{
  test_manga_app_pipeline();
  (void)fprintf(stderr, "[OK  ] test_app_ereader_manga.c\n");
  return 0;
}
