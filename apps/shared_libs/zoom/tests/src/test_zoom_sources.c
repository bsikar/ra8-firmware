/**
 * @file test_zoom_sources.c
 * @brief Host unit tests for the two zoom source adapters (#478).
 *
 * @details
 * The zoom engine sees pixels only through ::zoom_read_fn, and this file
 * owns the two implementations of that seam plus the ::ra8_tile_rect_t producer
 * they share:
 *
 *   - **tiled** (`zoom_tiles`): a gray8 image paged through a real
 *     ::ra8_tile_cache whose decoder is a coordinate-encoding generator, so an
 *     assembled rectangle can be checked pixel by pixel AND the cache's own
 *     counters can prove that only the requested tiles were ever touched. The
 *     residency test is the acceptance bar of #478: a rectangle spanning a
 *     handful of tiles must never decode more than those tiles, whatever the
 *     image size.
 *   - **book** (`zoom_book`): a `.rabook` image-pool figure, at both
 *     retained depths, checked against ::book_src_image_rect so the adapter
 *     is proven to be a pass-through rather than a second unpacker.
 *   - `ra8_tile_rect_of_pixels`, the pixel-rect -> tile-rect conversion that had
 *     no library producer before this change.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "book.h"
#include "book_paged.h"
#include "ra8_err.h"
#include "ra8_tile_cache.h"
#include "ra8_ui.h"
#include "unity_minimal.h"
#include "zoom.h"
#include "zoom_book.h"
#include "zoom_tiles.h"

/**
 * @enum t_tiles_geom_t
 * @brief Synthetic tiled-image geometry.
 * @details 100x70 over 32x32 tiles: 4x3 tiles with partial edge tiles on both
 *          axes (100 = 3*32 + 4, 70 = 2*32 + 6), so the edge-tile extent maths
 *          is exercised rather than assumed away by an exact tiling.
 */
typedef enum : uint32_t {
  k_t_img_w     = 100U,   /**< Synthetic image width, pixels.             */
  k_t_img_h     = 70U,    /**< Synthetic image height, pixels.            */
  k_t_tile      = 32U,    /**< Tile edge, pixels.                         */
  k_t_cols      = 4U,     /**< Derived tile columns.                      */
  k_t_rows      = 3U,     /**< Derived tile rows.                         */
  k_t_cells     = 6U,     /**< Tile-cache cells (deliberately < 12).      */
  k_t_buckets   = 8U,     /**< Tile-cache hash buckets.                   */
  k_t_image_id  = 7U,     /**< Tile-cache key image id.                   */
  k_t_out_w     = 64U,    /**< Assembled-rectangle buffer width.          */
  k_t_out_h     = 40U,    /**< Assembled-rectangle buffer height.         */
  k_t_byte_mask = 0xFFU,  /**< Sample modulus of the synthetic image.     */
  k_t_rect_x    = 20U,    /**< Straddling rectangle left edge.            */
  k_t_rect_y    = 20U,    /**< Straddling rectangle top edge.             */
  k_t_rect_w    = 40U,    /**< Straddling rectangle width, pixels.        */
  k_t_rect_h    = 20U,    /**< Straddling rectangle height, pixels.       */
  k_t_edge_x    = 90U,    /**< Corner rectangle left edge (edge tile).    */
  k_t_edge_y    = 62U,    /**< Corner rectangle top edge (edge tile).     */
  k_t_edge_w    = 10U,    /**< Corner rectangle width, pixels.            */
  k_t_edge_h    = 8U,     /**< Corner rectangle height, pixels.           */
  k_t_pf_view_w = 32U,    /**< Prefetch-test viewport width.              */
  k_t_pf_view_h = 16U,    /**< Prefetch-test viewport height.             */
  k_t_pf_budget = 4U,     /**< Prefetch-test residency budget, tiles.     */
  k_t_pf_rows   = 2U,     /**< Prefetch-test strip rows.                  */
  k_t_poison_a  = 0xAAU,  /**< Output poison A (10101010).                */
  k_t_poison_b  = 0x55U,  /**< Output poison B (01010101), distinct.      */
  k_t_grid_over = 70000U, /**< An extent whose 1 px tiles overrun uint16. */
} t_tiles_geom_t;

/** @brief Assembled-rectangle destination. */
static uint8_t s_out[(size_t)k_t_out_h * (size_t)k_t_out_w];

/** @brief Tiles decoded since the last ::t_tiles_open, for the residency check. */
static uint32_t s_decodes;
/** @brief When true, the decoder reports a deliberately wrong tile WIDTH. */
static bool s_bad_width;
/** @brief When true, the decoder reports a deliberately wrong tile HEIGHT. */
static bool s_bad_height;

/**
 * @brief The synthetic image sample: a pure function of the coordinate.
 * @param[in] x Source column.
 * @param[in] y Source row.
 * @return A gray8 value unique to (x, y) modulo 256.
 */
static uint8_t t_img_sample(uint32_t x, uint32_t y)
{
  return (uint8_t)(((y * (uint32_t)k_t_img_w) + x) & (uint32_t)k_t_byte_mask);
}

/**
 * @brief ::ra8_tile_decode_fn generating one tile from ::t_img_sample.
 * @param[in]  ctx        Unused.
 * @param[in]  key        The tile being decoded.
 * @param[out] cell       Destination cell.
 * @param[in]  cell_bytes Capacity of @p cell.
 * @param[out] out_w      Receives the decoded width.
 * @param[out] out_h      Receives the decoded height.
 * @return k_ra8_ok, or k_ra8_err_no_mem when the cell is too small.
 */
static ra8_err_t t_tile_decode(void*                 ctx,
                               const ra8_tile_key_t* key,
                               uint8_t*              cell,
                               uint32_t              cell_bytes,
                               uint16_t*             out_w,
                               uint16_t*             out_h)
{
  (void)ctx;
  s_decodes++;
  const uint32_t org_x = (uint32_t)key->tile_x * (uint32_t)k_t_tile;
  const uint32_t org_y = (uint32_t)key->tile_y * (uint32_t)k_t_tile;
  const uint32_t tw    = ((org_x + (uint32_t)k_t_tile) > (uint32_t)k_t_img_w)
                           ? ((uint32_t)k_t_img_w - org_x)
                           : (uint32_t)k_t_tile;
  const uint32_t th    = ((org_y + (uint32_t)k_t_tile) > (uint32_t)k_t_img_h)
                           ? ((uint32_t)k_t_img_h - org_y)
                           : (uint32_t)k_t_tile;
  if (cell_bytes < (tw * th)) {
    return k_ra8_err_no_mem;
  }
  for (uint32_t r = 0U; r < th; ++r) {
    for (uint32_t c = 0U; c < tw; ++c) {
      cell[(r * tw) + c] = t_img_sample(org_x + c, org_y + r);
    }
  }
  *out_w = s_bad_width ? (uint16_t)1U : (uint16_t)tw;
  *out_h = s_bad_height ? (uint16_t)1U : (uint16_t)th;
  return k_ra8_ok;
}

/**
 * @brief Initialise the tile cache and bind a tiled zoom source over it.
 * @param[out] cache Cache to initialise.
 * @param[out] ts    Tiled-source binding to populate.
 */
static void t_tiles_open(ra8_tile_cache_t* cache, zoom_tile_src_t* ts)
{
  static uint8_t s_cell_storage[(size_t)k_t_cells * (size_t)k_t_tile * (size_t)k_t_tile];
  static ra8_keycache_cell_t s_metadata[k_t_cells];
  static ra8_tile_key_t      s_keys[k_t_cells];
  static ra8_tile_dims_t     s_dimensions[k_t_cells];
  static int32_t             s_buckets[k_t_buckets];
  const ra8_tile_cache_cfg_t cfg = {
    .cell_mem     = s_cell_storage,
    .cell_bytes   = (uint32_t)k_t_tile * (uint32_t)k_t_tile,
    .cell_count   = (uint32_t)k_t_cells,
    .meta         = s_metadata,
    .keys         = s_keys,
    .dims         = s_dimensions,
    .buckets      = s_buckets,
    .bucket_count = (uint32_t)k_t_buckets,
    .decode       = t_tile_decode,
    .decode_ctx   = nullptr,
  };
  s_decodes    = 0U;
  s_bad_width  = false;
  s_bad_height = false;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tile_cache_init(cache, &cfg));
  TEST_ASSERT_EQ(k_ra8_ok,
                 zoom_tile_src_init(ts,
                                    cache,
                                    (uint32_t)k_t_image_id,
                                    (uint32_t)k_t_img_w,
                                    (uint32_t)k_t_img_h,
                                    (uint16_t)k_t_tile,
                                    (uint16_t)k_t_tile));
}

/**
 * @test tile_rect_of_pixels
 *
 * @par MC/DC:
 * Decision libs/ra8_mem/src/ra8_tile_cache.c@ra8_tile_rect_of_pixels
 * `if ((pw == 0U) || (ph == 0U))` (2 conditions):
 * - V1: pw=40, ph=20 -> false (control: a real rectangle)
 * - V2: pw=0,  ph=20 -> true  (varies pw only)
 * - V3: pw=40, ph=0  -> true  (varies ph only)
 * Second decision in the same function,
 * `if ((tile_w == 0U) || (tile_h == 0U) || (tile_cols == 0U) || (tile_rows == 0U))`
 * (4 conditions):
 * - V4: 32/32/4/3 -> false (control: a real grid)
 * - V5: 0/32/4/3  -> true  (varies tile_w only)
 * - V6: 32/0/4/3  -> true  (varies tile_h only)
 * - V7: 32/32/0/3 -> true  (varies tile_cols only)
 * - V8: 32/32/4/0 -> true  (varies tile_rows only)
 * Each of V5..V8 pairs with V4 to give one condition independent influence.
 * N+1 = 3 vectors for the first decision, 5 for the second.
 */
static void t_tile_rect_of_pixels(void)
{
  TEST_BEGIN("tile_rect_of_pixels");
  ra8_tile_rect_t r = {};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_tile_rect_of_pixels(0U, 0U, 4U, 4U, 32U, 32U, 4U, 3U, nullptr));
  /* V1 / V4: a rectangle straddling two tile columns and two tile rows. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tile_rect_of_pixels(30U, 30U, 40U, 20U, 32U, 32U, 4U, 3U, &r));
  TEST_ASSERT_EQ(0U, r.tx0);
  TEST_ASSERT_EQ(2U, r.tx1);
  TEST_ASSERT_EQ(0U, r.ty0);
  TEST_ASSERT_EQ(1U, r.ty1);
  /* A single pixel names exactly one tile. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tile_rect_of_pixels(33U, 65U, 1U, 1U, 32U, 32U, 4U, 3U, &r));
  TEST_ASSERT_EQ(1U, r.tx0);
  TEST_ASSERT_EQ(1U, r.tx1);
  TEST_ASSERT_EQ(2U, r.ty0);
  TEST_ASSERT_EQ(2U, r.ty1);
  /* A rectangle that runs past the image clamps to the last tile rather than
   * handing the prefetch an out-of-grid index to re-clamp. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tile_rect_of_pixels(0U, 0U, 4000U, 4000U, 32U, 32U, 4U, 3U, &r));
  TEST_ASSERT_EQ(3U, r.tx1);
  TEST_ASSERT_EQ(2U, r.ty1);
  /* V2 / V3 */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_tile_rect_of_pixels(0U, 0U, 0U, 20U, 32U, 32U, 4U, 3U, &r));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_tile_rect_of_pixels(0U, 0U, 40U, 0U, 32U, 32U, 4U, 3U, &r));
  /* V5 / V6 / V7 / V8 */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_tile_rect_of_pixels(0U, 0U, 40U, 20U, 0U, 32U, 4U, 3U, &r));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_tile_rect_of_pixels(0U, 0U, 40U, 20U, 32U, 0U, 4U, 3U, &r));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_tile_rect_of_pixels(0U, 0U, 40U, 20U, 32U, 32U, 0U, 3U, &r));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_tile_rect_of_pixels(0U, 0U, 40U, 20U, 32U, 32U, 4U, 0U, &r));
  TEST_END("tile_rect_of_pixels");
}

/**
 * @brief The bind-half of ::t_tile_src_init_validates.
 * @details Split out only to stay inside the 60-line function cap. Re-opens the
 *          real geometry first, because the parent test's last vector left the
 *          binding at a 1 px tile grid.
 * @param[in,out] cache The tile cache to re-open.
 * @param[in,out] ts    The tiled-source binding to re-open and then bind.
 */
static void t_tile_src_bind_checks(ra8_tile_cache_t* cache, zoom_tile_src_t* ts)
{
  t_tiles_open(cache, ts);
  zoom_source_t src = {};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, zoom_tile_src_bind(nullptr, &src));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, zoom_tile_src_bind(ts, nullptr));
  zoom_tile_src_t unbound = {};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, zoom_tile_src_bind(&unbound, &src));
  TEST_ASSERT_EQ(k_ra8_ok, zoom_tile_src_bind(ts, &src));
  TEST_ASSERT_EQ(k_t_img_w, src.width);
  TEST_ASSERT(src.ctx == ts);
}

/**
 * @test tile_src_init_validates
 *
 * @par MC/DC:
 * Decision apps/shared_libs/zoom/src/zoom_tiles.c@zoom_tile_src_init
 * `if ((width == 0U) || (height == 0U) || (tile_w == 0U) || (tile_h == 0U))`
 * (4 conditions):
 * - V1: 100/70/32/32 -> false (control: binds)
 * - V2: 0/70/32/32   -> true  (varies width only)
 * - V3: 100/0/32/32  -> true  (varies height only)
 * - V4: 100/70/0/32  -> true  (varies tile_w only)
 * - V5: 100/70/32/0  -> true  (varies tile_h only)
 * Second decision, `if ((cols > UINT16_MAX) || (rows > UINT16_MAX))`:
 * - V6: a 1-px tile over a 100x70 image  -> false (control: 100 x 70 tiles)
 * - V7: a 1-px tile over a 70000-wide image -> true (varies the cols condition)
 * - V8: a 1-px tile over a 70000-tall image -> true (varies the rows condition)
 * Each vector pairs with its control to give one condition independent
 * influence. N+1 = 5 and 3 vectors respectively.
 */
static void t_tile_src_init_validates(void)
{
  TEST_BEGIN("tile_src_init_validates");
  ra8_tile_cache_t cache = {};
  zoom_tile_src_t  ts    = {};
  t_tiles_open(&cache, &ts);
  /* V1 already asserted inside t_tiles_open. */
  TEST_ASSERT_EQ(k_t_cols, ts.tile_cols);
  TEST_ASSERT_EQ(k_t_rows, ts.tile_rows);

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, zoom_tile_src_init(nullptr, &cache, 1U, 8U, 8U, 4U, 4U));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, zoom_tile_src_init(&ts, nullptr, 1U, 8U, 8U, 4U, 4U));
  /* V2 .. V5 */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 zoom_tile_src_init(&ts,
                                    &cache,
                                    1U,
                                    0U,
                                    (uint32_t)k_t_img_h,
                                    (uint16_t)k_t_tile,
                                    (uint16_t)k_t_tile));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 zoom_tile_src_init(&ts,
                                    &cache,
                                    1U,
                                    (uint32_t)k_t_img_w,
                                    0U,
                                    (uint16_t)k_t_tile,
                                    (uint16_t)k_t_tile));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 zoom_tile_src_init(&ts,
                                    &cache,
                                    1U,
                                    (uint32_t)k_t_img_w,
                                    (uint32_t)k_t_img_h,
                                    0U,
                                    (uint16_t)k_t_tile));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 zoom_tile_src_init(&ts,
                                    &cache,
                                    1U,
                                    (uint32_t)k_t_img_w,
                                    (uint32_t)k_t_img_h,
                                    (uint16_t)k_t_tile,
                                    0U));
  /* V6 / V7 / V8 */
  TEST_ASSERT_EQ(
    k_ra8_ok,
    zoom_tile_src_init(&ts, &cache, 1U, (uint32_t)k_t_img_w, (uint32_t)k_t_img_h, 1U, 1U));
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_size,
    zoom_tile_src_init(&ts, &cache, 1U, (uint32_t)k_t_grid_over, (uint32_t)k_t_img_h, 1U, 1U));
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_size,
    zoom_tile_src_init(&ts, &cache, 1U, (uint32_t)k_t_img_w, (uint32_t)k_t_grid_over, 1U, 1U));

  t_tile_src_bind_checks(&cache, &ts);
  TEST_END("tile_src_init_validates");
}

/**
 * @brief The edge-tile and fail-closed halves of the tile-read test.
 * @details Split out only to stay inside the 60-line function cap; the MC/DC
 *          vector label (V9) is continuous with the parent test's block.
 * @param[in,out] cache The initialised tile cache.
 * @param[in,out] ts    The bound tiled source over it.
 */
static void t_tile_read_edges_and_failclosed(ra8_tile_cache_t* cache, zoom_tile_src_t* ts)
{
  /* The bottom-right corner exercises the partial edge tiles on BOTH axes,
   * where exp_w and exp_h are the clamped remainders rather than the tile edge. */
  (void)memset(s_out, (int)k_t_poison_b, sizeof(s_out));
  TEST_ASSERT_EQ(k_ra8_ok,
                 zoom_tile_read(ts,
                                (uint32_t)k_t_edge_x,
                                (uint32_t)k_t_edge_y,
                                (uint32_t)k_t_edge_w,
                                (uint32_t)k_t_edge_h,
                                s_out,
                                (uint32_t)k_t_out_w));
  for (uint32_t r = 0U; r < (uint32_t)k_t_edge_h; ++r) {
    for (uint32_t c = 0U; c < (uint32_t)k_t_edge_w; ++c) {
      TEST_ASSERT_EQ(t_img_sample((uint32_t)k_t_edge_x + c, (uint32_t)k_t_edge_y + r),
                     s_out[(r * (uint32_t)k_t_out_w) + c]);
    }
  }

  /* Residency is bounded by the cache, not by the rectangle: a full-width sweep
   * over a cache smaller than the tile count still completes. */
  uint32_t hits      = 0U;
  uint32_t misses    = 0U;
  uint32_t evictions = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 zoom_tile_read(ts, 0U, 0U, (uint32_t)k_t_img_w, 8U, s_out, (uint32_t)k_t_img_w));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tile_cache_stats(cache, &hits, &misses, &evictions));
  TEST_ASSERT(misses <= ((uint32_t)k_t_cols * (uint32_t)k_t_rows));

  /* V9 / V10: a decoder that reports the wrong extent fails closed rather than
   * misrendering -- the #339 colour-atlas trap. Each extent is mangled on its
   * own, so each condition of the guard is shown to flip the outcome by itself. */
  t_tiles_open(cache, ts);
  s_bad_width = true;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 zoom_tile_read(ts, 0U, 0U, 8U, 8U, s_out, (uint32_t)k_t_out_w));
  s_bad_width = false;
  t_tiles_open(cache, ts);
  s_bad_height = true;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 zoom_tile_read(ts, 0U, 0U, 8U, 8U, s_out, (uint32_t)k_t_out_w));
  s_bad_height = false;
}

/**
 * @test tile_read_assembles_and_bounds_residency
 *
 * @par MC/DC:
 * Decision apps/shared_libs/zoom/src/zoom_tiles.c@internal_read_rect_ok
 * `if ((w == 0U) || (h == 0U) || (out_stride < w))` (3 conditions):
 * - V1: w=40, h=20, stride=64 -> false (control: assembled)
 * - V2: w=0,  h=20, stride=64 -> true  (varies w only)
 * - V3: w=40, h=0,  stride=64 -> true  (varies h only)
 * - V4: w=40, h=20, stride=8  -> true  (varies the stride condition only)
 * Second decision, `if (((x + w) > ts->width) || ((y + h) > ts->height))`:
 * - V5: x=0,  y=0,  w=40, h=20 -> false (control: inside the image)
 * - V6: x=90, y=0,  w=40, h=20 -> true  (varies the width condition)
 * - V7: x=0,  y=60, w=40, h=20 -> true  (varies the height condition)
 * Each vector pairs with its control for independent influence. N+1 = 4 and 3.
 *
 * @par MC/DC:
 * Decision apps/shared_libs/zoom/src/zoom_tiles.c@internal_tile_fetch
 * `if (((uint32_t)tile.width != exp_w) || ((uint32_t)tile.height != exp_h))`
 * (2 conditions):
 * - V8:  a decoder reporting the declared extent -> false (control: copies)
 * - V9:  a decoder reporting a wrong WIDTH only  -> true  (varies condition 1)
 * - V10: a decoder reporting a wrong HEIGHT only -> true  (varies condition 2)
 * V8+V9 prove the width condition's independent influence; V8+V10 the height's.
 * The fixture decoder mangles each extent under its own flag precisely so that
 * neither condition is masked by the other. N+1 = 3 vectors for N=2.
 *
 * @details Beyond MC/DC this is the residency acceptance bar of #478: a
 *          rectangle spanning six tiles must decode six tiles and no more,
 *          whatever the image size, and re-reading it must decode nothing.
 */
static void t_tile_read_assembles_and_bounds_residency(void)
{
  TEST_BEGIN("tile_read_assembles_and_bounds_residency");
  ra8_tile_cache_t cache = {};
  zoom_tile_src_t  ts    = {};
  t_tiles_open(&cache, &ts);

  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 zoom_tile_read(nullptr, 0U, 0U, 4U, 4U, s_out, (uint32_t)k_t_out_w));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 zoom_tile_read(&ts, 0U, 0U, 4U, 4U, nullptr, (uint32_t)k_t_out_w));
  zoom_tile_src_t unbound = {};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 zoom_tile_read(&unbound, 0U, 0U, 4U, 4U, s_out, (uint32_t)k_t_out_w));
  /* V2 / V3 / V4 */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 zoom_tile_read(&ts, 0U, 0U, 0U, 20U, s_out, (uint32_t)k_t_out_w));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 zoom_tile_read(&ts, 0U, 0U, 40U, 0U, s_out, (uint32_t)k_t_out_w));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, zoom_tile_read(&ts, 0U, 0U, 40U, 20U, s_out, 8U));
  /* V6 / V7 */
  TEST_ASSERT_EQ(k_ra8_err_out_of_range,
                 zoom_tile_read(&ts, 90U, 0U, 40U, 20U, s_out, (uint32_t)k_t_out_w));
  TEST_ASSERT_EQ(k_ra8_err_out_of_range,
                 zoom_tile_read(&ts, 0U, 60U, 40U, 20U, s_out, (uint32_t)k_t_out_w));

  /* V1 / V5 / V8: a rectangle straddling 2x2 tiles assembles pixel-exactly. */
  (void)memset(s_out, (int)k_t_poison_a, sizeof(s_out));
  s_decodes = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 zoom_tile_read(&ts,
                                (uint32_t)k_t_rect_x,
                                (uint32_t)k_t_rect_y,
                                (uint32_t)k_t_rect_w,
                                (uint32_t)k_t_rect_h,
                                s_out,
                                (uint32_t)k_t_out_w));
  for (uint32_t r = 0U; r < (uint32_t)k_t_rect_h; ++r) {
    for (uint32_t c = 0U; c < (uint32_t)k_t_rect_w; ++c) {
      TEST_ASSERT_EQ(t_img_sample((uint32_t)k_t_rect_x + c, (uint32_t)k_t_rect_y + r),
                     s_out[(r * (uint32_t)k_t_out_w) + c]);
    }
  }
  /* Exactly the 2x2 tiles under the rectangle were decoded -- never the image. */
  TEST_ASSERT_EQ(4U, s_decodes);
  /* Re-reading the same rectangle decodes nothing: the tiles are resident. */
  s_decodes = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 zoom_tile_read(&ts,
                                (uint32_t)k_t_rect_x,
                                (uint32_t)k_t_rect_y,
                                (uint32_t)k_t_rect_w,
                                (uint32_t)k_t_rect_h,
                                s_out,
                                (uint32_t)k_t_out_w));
  TEST_ASSERT_EQ(0U, s_decodes);

  t_tile_read_edges_and_failclosed(&cache, &ts);
  TEST_END("tile_read_assembles_and_bounds_residency");
}

/**
 * @test tiles_prefetch_warms_lead_edge
 */
static void t_tiles_prefetch_warms_lead_edge(void)
{
  TEST_BEGIN("tiles_prefetch_warms_lead_edge");
  ra8_tile_cache_t cache = {};
  zoom_tile_src_t  ts    = {};
  t_tiles_open(&cache, &ts);
  zoom_source_t src = {};
  TEST_ASSERT_EQ(k_ra8_ok, zoom_tile_src_bind(&ts, &src));

  static uint8_t  s_pf_row[k_t_img_w];
  static uint8_t  s_pf_strip[(size_t)k_t_img_w * (size_t)k_t_pf_rows];
  static uint8_t  s_pf_packed[(size_t)k_t_img_w];
  zoom_view_t     v   = {};
  zoom_view_cfg_t cfg = {
    .src       = src,
    .scratch   = {.row        = s_pf_row,
                  .row_cap    = (uint32_t)sizeof(s_pf_row),
                  .strip      = s_pf_strip,
                  .strip_cap  = (uint32_t)sizeof(s_pf_strip),
                  .packed     = s_pf_packed,
                  .packed_cap = (uint32_t)sizeof(s_pf_packed)},
    .dst       = {.x = 0, .y = 0, .w = (int32_t)k_t_pf_view_w, .h = (int32_t)k_t_pf_view_h},
    .scale     = 1U,
    .scale_max = 4U,
    .policy    = k_zoom_policy_responsive,
    .settle_ms = 0U,
    .focus_x   = 0,
    .focus_y   = 0,
  };
  TEST_ASSERT_EQ(k_ra8_ok, zoom_view_open(&v, &cfg));

  uint16_t warmed = 0U;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 zoom_tiles_prefetch(nullptr, &v, k_zoom_pan_right, 4U, &warmed));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 zoom_tiles_prefetch(&ts, nullptr, k_zoom_pan_right, 4U, &warmed));
  zoom_tile_src_t unbound = {};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 zoom_tiles_prefetch(&unbound, &v, k_zoom_pan_right, 4U, &warmed));

  /* No travel warms nothing; a rightward pan warms the column beyond the view. */
  s_decodes = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, zoom_tiles_prefetch(&ts, &v, k_zoom_pan_none, 4U, &warmed));
  TEST_ASSERT_EQ(0U, warmed);
  TEST_ASSERT_EQ(k_ra8_ok, zoom_tiles_prefetch(&ts, &v, k_zoom_pan_right, 4U, &warmed));
  TEST_ASSERT(warmed > 0U);
  TEST_ASSERT(warmed <= 4U);
  TEST_ASSERT_EQ(warmed, s_decodes);
  TEST_END("tiles_prefetch_warms_lead_edge");
}

/* --- the `.rabook` figure adapter ------------------------------------------ */

/**
 * @enum t_book_geom_t
 * @brief Fixture `.rabook` geometry and packing constants.
 */
typedef enum : uint16_t {
  k_b_g4_w     = 5U,    /**< gray4 image width (odd -> nibble parity).    */
  k_b_g4_h     = 3U,    /**< gray4 image height.                          */
  k_b_g4_bytes = 8U,    /**< Packed bytes for the gray4 image.            */
  k_b_g8_w     = 12U,   /**< gray8 image width.                           */
  k_b_g8_h     = 4U,    /**< gray8 image height.                          */
  k_b_g8_off   = 8U,    /**< gray8 pool offset (== the gray4 byte count). */
  k_b_g8_bytes = 48U,   /**< gray8 bytes (12 * 4, one byte per pixel).    */
  k_b_pool_cap = 128U,  /**< Image-pool capacity.                         */
  k_b_str_cap  = 16U,   /**< String-pool capacity.                        */
  k_b_out_cap  = 64U,   /**< gray8 output buffer capacity.                */
  k_b_nib_sh   = 4U,    /**< Nibble shift / 4->8 replicate amount.        */
  k_b_nib_mask = 0x0FU, /**< Low-nibble value / slot mask.                */
  k_b_nib_hi   = 0xF0U, /**< High-nibble slot mask.                       */
  k_b_g8_mul   = 37U,   /**< Odd multiplier: strides the 0..255 range.    */
  k_b_g8_bias  = 5U,    /**< Bias, so (0,0) is off the gray4 grid.        */
  k_b_svg_idx  = 2U,    /**< Fixture image index of the SVG entry.        */
} t_book_geom_t;

/**
 * @enum t_book_crc_t
 * @brief CRC-32/ISO-HDLC constants (must match book_validate).
 */
typedef enum : uint32_t {
  k_b_crc_init = 0xFFFFFFFFU, /**< CRC-32 initial value and final XOR-out. */
  k_b_crc_poly = 0xEDB88320U, /**< The reflected CRC-32 polynomial.        */
} t_book_crc_t;

/**
 * @struct t_book_t
 * @brief Self-contained `.rabook` blob: a 1-node DOM plus a 3-image pool.
 * @details One gray4 raster, one gray8 raster, and one SVG entry so the
 *          adapter's fail-closed vector branch has something to reject.
 * @invariant The header offsets address this struct's own members.
 * @par Example:
 * @code
 * static t_book_t blob;
 * t_book_setup(&blob);
 * @endcode
 * @see t_book_setup
 * @since 0.1.0
 */
typedef struct {
  book_header_t  hdr;                  /**< Header.      */
  book_chapter_t chapters[1];          /**< Chapters.    */
  book_node_t    nodes[1];             /**< Nodes.       */
  book_image_t   images[3];            /**< Descriptors. */
  char           strings[k_b_str_cap]; /**< String pool. */
  uint8_t        pool[k_b_pool_cap];   /**< Image pool.  */
} t_book_t;

/** @brief The fixture blob. */
static t_book_t s_book;

/** @brief CRC-32/ISO-HDLC over the blob body (matches book_validate). */
static uint32_t t_book_crc32(const uint8_t* data, size_t len)
{
  uint32_t crc = (uint32_t)k_b_crc_init;
  for (size_t i = 0U; i < len; ++i) {
    crc ^= data[i];
    for (uint8_t bit = 0U; bit < 8U; ++bit) {
      const uint32_t mask = ((crc & 1U) != 0U) ? UINT32_MAX : 0U;
      crc                 = (crc >> 1U) ^ ((uint32_t)k_b_crc_poly & mask);
    }
  }
  return crc ^ (uint32_t)k_b_crc_init;
}

/** @brief The gray4 nibble of pixel `(px, py)` in the fixture's gray4 image. */
static uint8_t t_book_nib(uint32_t px, uint32_t py)
{
  return (uint8_t)(((py * (uint32_t)k_b_g4_w) + px) & (uint32_t)k_b_nib_mask);
}

/** @brief The gray8 value of pixel `(px, py)` in the fixture's gray8 image. */
static uint8_t t_book_g8(uint32_t px, uint32_t py)
{
  return (uint8_t)((((py * (uint32_t)k_b_g8_w) + px) * (uint32_t)k_b_g8_mul) +
                   (uint32_t)k_b_g8_bias);
}

/**
 * @brief Pack both fixture rasters into the blob's image pool.
 * @details The gray4 image is nibble-packed at 2 px/byte (odd width, so a row
 *          starts on either nibble); the gray8 image is one byte per pixel.
 */
static void t_book_pack_pool(void)
{
  for (uint32_t py = 0U; py < (uint32_t)k_b_g4_h; ++py) {
    for (uint32_t px = 0U; px < (uint32_t)k_b_g4_w; ++px) {
      const uint32_t flat = (py * (uint32_t)k_b_g4_w) + px;
      const uint8_t  nib  = t_book_nib(px, py);
      uint8_t*       b    = &s_book.pool[flat >> 1U];
      if ((flat & 1U) == 0U) {
        *b = (uint8_t)((*b & (uint8_t)k_b_nib_mask) | (uint8_t)(nib << (uint8_t)k_b_nib_sh));
      } else {
        *b = (uint8_t)((*b & (uint8_t)k_b_nib_hi) | nib);
      }
    }
  }
  for (uint32_t py = 0U; py < (uint32_t)k_b_g8_h; ++py) {
    for (uint32_t px = 0U; px < (uint32_t)k_b_g8_w; ++px) {
      s_book.pool[(uint32_t)k_b_g8_off + (py * (uint32_t)k_b_g8_w) + px] = t_book_g8(px, py);
    }
  }
}

/** @brief Populate the fixture blob (header, tables, packed pool, CRC). */
static void t_book_setup(void)
{
  (void)memset(&s_book, 0, sizeof(s_book));
  (void)memcpy(s_book.hdr.magic, "RABOOK1", 8);
  s_book.hdr.format_version    = k_book_format_version;
  s_book.hdr.total_size        = (uint32_t)sizeof(s_book);
  s_book.hdr.chapter_count     = 1U;
  s_book.hdr.chapter_off       = (uint32_t)offsetof(t_book_t, chapters);
  s_book.hdr.node_count        = 1U;
  s_book.hdr.node_off          = (uint32_t)offsetof(t_book_t, nodes);
  s_book.hdr.attr_off          = (uint32_t)offsetof(t_book_t, strings);
  s_book.hdr.stylesheet_off    = (uint32_t)offsetof(t_book_t, strings);
  s_book.hdr.image_count       = 3U;
  s_book.hdr.image_off         = (uint32_t)offsetof(t_book_t, images);
  s_book.hdr.string_off        = (uint32_t)offsetof(t_book_t, strings);
  s_book.hdr.string_size       = (uint32_t)sizeof(s_book.strings);
  s_book.hdr.image_pool_off    = (uint32_t)offsetof(t_book_t, pool);
  s_book.hdr.image_pool_size   = (uint32_t)sizeof(s_book.pool);
  s_book.hdr.cover_image_index = k_book_nil;

  s_book.chapters[0].root_node = 0U;
  s_book.nodes[0]              = (book_node_t){.kind         = k_book_node_text,
                                               .text_off     = 0U,
                                               .first_child  = k_book_nil,
                                               .next_sibling = k_book_nil};

  s_book.images[0] = (book_image_t){.width        = (uint16_t)k_b_g4_w,
                                    .height       = (uint16_t)k_b_g4_h,
                                    .format       = (uint8_t)k_book_image_gray4,
                                    .pixel_format = (uint8_t)k_book_pixfmt_gray4,
                                    .data_off     = 0U,
                                    .data_size    = (uint32_t)k_b_g4_bytes,
                                    .raw_size     = (uint32_t)k_b_g4_bytes};
  s_book.images[1] = (book_image_t){.width        = (uint16_t)k_b_g8_w,
                                    .height       = (uint16_t)k_b_g8_h,
                                    .format       = (uint8_t)k_book_image_gray4,
                                    .pixel_format = (uint8_t)k_book_pixfmt_gray8,
                                    .data_off     = (uint32_t)k_b_g8_off,
                                    .data_size    = (uint32_t)k_b_g8_bytes,
                                    .raw_size     = (uint32_t)k_b_g8_bytes};
  s_book.images[2] = (book_image_t){.width     = 4U,
                                    .height    = 4U,
                                    .format    = (uint8_t)k_book_image_svg,
                                    .data_off  = (uint32_t)k_b_g8_off + k_b_g8_bytes,
                                    .data_size = 1U,
                                    .raw_size  = 1U};

  t_book_pack_pool();

  const uint8_t* bytes = (const uint8_t*)&s_book;
  s_book.hdr.crc32_val =
    t_book_crc32(&bytes[sizeof(book_header_t)], (size_t)(sizeof(s_book) - sizeof(book_header_t)));
}

/**
 * @brief The gray8 and fail-closed halves of ::t_book_src_binds_both_depths.
 * @details Split out only to stay inside the 60-line function cap; the MC/DC
 *          vector labels (V2 / V3) are continuous with the parent test's block.
 * @param[in] src The bound resident book source over the fixture blob.
 */
static void t_book_gray8_and_failclosed(const book_src_t* src)
{
  static uint8_t  s_via_adapter[k_b_out_cap];
  zoom_book_src_t bs   = {};
  zoom_source_t   zsrc = {};
  /* The gray8 figure -- the retained full-resolution representation the loupe
   * exists for -- comes back verbatim, with no quantisation anywhere. */
  TEST_ASSERT_EQ(k_ra8_ok, zoom_book_src_init(&bs, src, 1U));
  TEST_ASSERT_EQ(k_ra8_ok, zoom_book_src_bind(&bs, &zsrc));
  TEST_ASSERT_EQ(k_b_g8_w, zsrc.width);
  TEST_ASSERT_EQ(k_ra8_ok, zsrc.read(zsrc.ctx, 2U, 1U, 4U, 2U, s_via_adapter, (uint32_t)k_b_g8_w));
  for (uint32_t py = 0U; py < 2U; ++py) {
    for (uint32_t px = 0U; px < 4U; ++px) {
      TEST_ASSERT_EQ(t_book_g8(2U + px, 1U + py), s_via_adapter[(py * (uint32_t)k_b_g8_w) + px]);
    }
  }
  /* A rectangle that leaves the figure is refused by the library beneath. */
  TEST_ASSERT_EQ(
    k_ra8_err_out_of_range,
    zsrc.read(zsrc.ctx, 0U, 0U, (uint32_t)k_b_g8_w + 1U, 1U, s_via_adapter, (uint32_t)k_b_out_cap));

  /* V2 / V3: a hand-built descriptor with a zero extent fails closed. */
  s_book.images[0].width = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, zoom_book_src_init(&bs, src, 0U));
  s_book.images[0].width  = (uint16_t)k_b_g4_w;
  s_book.images[0].height = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, zoom_book_src_init(&bs, src, 0U));
  s_book.images[0].height = (uint16_t)k_b_g4_h;
}

/**
 * @brief The null / vector-entry guards of ::t_book_src_binds_both_depths.
 * @details Split out only to stay inside the function-size bar. Leaves @p bs
 *          bound to the gray4 figure and @p zsrc pointing at it, which is the
 *          state the parent test carries on from (its MC/DC vector V1).
 * @param[in]  src  The bound resident book source over the fixture blob.
 * @param[out] bs   Receives the binding for image 0 (the gray4 raster).
 * @param[out] zsrc Receives the zoom source over that binding.
 */
static void t_book_guards(const book_src_t* src, zoom_book_src_t* bs, zoom_source_t* zsrc)
{
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, zoom_book_src_init(nullptr, src, 0U));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, zoom_book_src_init(bs, nullptr, 0U));
  /* A vector entry is not magnifiable by a raster viewer, and says so. */
  TEST_ASSERT_EQ(k_ra8_err_not_supported, zoom_book_src_init(bs, src, (uint32_t)k_b_svg_idx));

  /* V1: the gray4 figure binds and reads as gray8. */
  TEST_ASSERT_EQ(k_ra8_ok, zoom_book_src_init(bs, src, 0U));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, zoom_book_src_bind(nullptr, zsrc));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, zoom_book_src_bind(bs, nullptr));
  zoom_book_src_t unbound = {};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, zoom_book_src_bind(&unbound, zsrc));
  TEST_ASSERT_EQ(k_ra8_ok, zoom_book_src_bind(bs, zsrc));
  TEST_ASSERT_EQ(k_b_g4_w, zsrc->width);

  static uint8_t s_guard_buf[k_b_out_cap];
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, zoom_book_read(nullptr, 0U, 0U, 1U, 1U, s_guard_buf, 1U));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, zoom_book_read(bs, 0U, 0U, 1U, 1U, nullptr, 1U));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, zoom_book_read(&unbound, 0U, 0U, 1U, 1U, s_guard_buf, 1U));
}

/**
 * @test book_src_binds_both_depths
 *
 * @par MC/DC:
 * Decision apps/shared_libs/zoom/src/zoom_book.c@zoom_book_src_init
 * `if ((img.width == 0U) || (img.height == 0U))` (2 conditions):
 * - V1: 5x3 -> false (control: the figure binds)
 * - V2: 0x3 -> true  (varies width only)
 * - V3: 5x0 -> true  (varies height only)
 * V1+V2 prove width's independent influence; V1+V3 prove height's. N+1 = 3.
 * The zero extents are injected into the descriptor table directly, because a
 * compiled `.rabook` cannot carry one -- the guard is the library's own
 * fail-closed check against a corrupt or hand-built blob.
 *
 * @details Also asserts the adapter is a pass-through: the gray8 it hands the
 *          zoom engine is byte-identical to ::book_src_image_rect's own
 *          output at BOTH retained depths, so no second unpacker exists to
 *          drift from the library's.
 */
static void t_book_src_binds_both_depths(void)
{
  TEST_BEGIN("book_src_binds_both_depths");
  t_book_setup();
  book_src_t src = {};
  TEST_ASSERT_EQ(k_ra8_ok, book_src_resident(&src, &s_book, (uint32_t)sizeof(s_book)));

  zoom_book_src_t bs   = {};
  zoom_source_t   zsrc = {};
  t_book_guards(&src, &bs, &zsrc);

  static uint8_t s_via_adapter[k_b_out_cap];
  static uint8_t s_via_library[k_b_out_cap];
  TEST_ASSERT_EQ(k_ra8_ok,
                 zsrc.read(zsrc.ctx,
                           0U,
                           0U,
                           (uint32_t)k_b_g4_w,
                           (uint32_t)k_b_g4_h,
                           s_via_adapter,
                           (uint32_t)k_b_g4_w));
  TEST_ASSERT_EQ(k_ra8_ok,
                 book_src_image_rect(&src,
                                     &bs.img,
                                     0U,
                                     0U,
                                     (uint32_t)k_b_g4_w,
                                     (uint32_t)k_b_g4_h,
                                     s_via_library,
                                     (uint32_t)k_b_g4_w));
  TEST_ASSERT_EQ(0, memcmp(s_via_adapter, s_via_library, (size_t)k_b_g4_w * (size_t)k_b_g4_h));
  for (uint32_t py = 0U; py < (uint32_t)k_b_g4_h; ++py) {
    for (uint32_t px = 0U; px < (uint32_t)k_b_g4_w; ++px) {
      const uint8_t nib = t_book_nib(px, py);
      TEST_ASSERT_EQ(((nib << (uint8_t)k_b_nib_sh) | nib),
                     s_via_adapter[(py * (uint32_t)k_b_g4_w) + px]);
    }
  }

  t_book_gray8_and_failclosed(&src);
  TEST_END("book_src_binds_both_depths");
}

/**
 * @brief Host test entry point.
 * @return 0 on success; any assertion `exit(1)`s before returning.
 */
int main(void)
{
  t_tile_rect_of_pixels();
  t_tile_src_init_validates();
  t_tile_read_assembles_and_bounds_residency();
  t_tiles_prefetch_warms_lead_edge();
  t_book_src_binds_both_depths();
  return 0;
}
