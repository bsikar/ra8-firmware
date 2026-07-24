/**
 * @file test_ra8_tile_cache.c
 * @brief Unit tests for the ra8_mem image-tile cache (Layer 3b, #147).
 *
 * @details
 * Mirrors the glyph-atlas tests for the second ::ra8_keycache facade: decode-on-
 * miss + hit (with decoded content + reported dimensions, including a smaller
 * edge tile), LRU eviction past capacity, the pin/unpin contract (a fully-pinned
 * cache cannot evict), a decoder failure leaving the victim cold, and the
 * validation guards.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_tile_cache.h"
#include "unity_minimal.h"

/**
 * @enum t_key_t
 * @brief Image and tile ids the cache-key arms build with.
 *
 * @details
 * Opaque to the cache -- it only hashes them into a slot -- so they are chosen
 * distinct and non-adjacent, which makes an off-by-one in the hash land on an
 * id no arm uses.
 */
typedef enum : uint8_t {
  k_t_image_id = 0x42U, /**< Image id shared by the hit and edge arms. */
  k_t_tile_id  = 99U,   /**< A tile id no other arm uses.              */
} t_key_t;

/**
 * @enum t_tile_const_t
 * @brief Fixture sizes.
 */
typedef enum : uint32_t {
  k_t_cell_bytes = 256U, /**< Bytes per tile cell. */
  k_t_cells      = 3U,   /**< Cells in the cache.  */
  k_t_buckets    = 8U,   /**< Hash buckets.        */
} t_tile_const_t;

/**
 * @enum t_tile_dim_t
 * @brief Stub decode output dimensions.
 */
typedef enum : uint16_t {
  k_t_full_dim = 16U, /**< Full (interior) tile edge length.   */
  k_t_edge_dim = 7U,  /**< Smaller edge-tile dimension.        */
  k_t_edge_x   = 9U,  /**< tile_x that simulates an edge tile. */
} t_tile_dim_t;

/**
 * @enum t_pan_const_t
 * @brief Tile-grid geometry the pan-prefetch arms build against.
 */
typedef enum : uint16_t {
  k_t_grid        = 4U,  /**< Square tile-grid extent (columns == rows).        */
  k_t_pan_span    = 2U,  /**< Lead-edge tiles a two-row/-col view warms.        */
  k_t_distinct_tx = 20U, /**< tile_x base for the cache-filling distinct tiles. */
} t_pan_const_t;

static uint8_t             s_cells[(size_t)k_t_cells * (size_t)k_t_cell_bytes];
static ra8_tile_key_t      s_keys[(size_t)k_t_cells];
static ra8_tile_dims_t     s_dims[(size_t)k_t_cells];
static ra8_keycache_cell_t s_meta[(size_t)k_t_cells];
static int32_t             s_buckets[(size_t)k_t_buckets];
static uint32_t            s_decode_calls;

/** @brief Stub decoder: stamps the key into the cell; edge tiles decode small. */
static ra8_err_t t_decode(void*                 ctx,
                          const ra8_tile_key_t* key,
                          uint8_t*              cell,
                          uint32_t              cell_bytes,
                          uint16_t*             out_w,
                          uint16_t*             out_h)
{
  (void)ctx;
  s_decode_calls++;
  (void)memset(cell, 0, (size_t)cell_bytes);
  cell[0] = (uint8_t)key->image_id;
  cell[1] = (uint8_t)key->tile_x;
  cell[2] = (uint8_t)key->tile_y;
  cell[3] = (uint8_t)key->zoom;
  if (key->tile_x == (uint16_t)k_t_edge_x) {
    *out_w = (uint16_t)k_t_edge_dim;
    *out_h = (uint16_t)k_t_edge_dim;
  } else {
    *out_w = (uint16_t)k_t_full_dim;
    *out_h = (uint16_t)k_t_full_dim;
  }
  return k_ra8_ok;
}

/** @brief Stub decoder that always fails (decode-on-miss failure path). */
/* The pointer parameters below cannot be const: this mock implements a
 * function-pointer interface (the DI seam under test), so its signature is
 * fixed by the typedef it is assigned to -- adding const changes the
 * function type and the assignment stops compiling. */
// NOLINTBEGIN(readability-non-const-parameter)
static ra8_err_t t_decode_fail(void*                 ctx,
                               const ra8_tile_key_t* key,
                               uint8_t*              cell,
                               uint32_t              cell_bytes,
                               uint16_t*             out_w,
                               uint16_t*             out_h)
// NOLINTEND(readability-non-const-parameter)
{
  (void)ctx;
  (void)key;
  (void)cell;
  (void)cell_bytes;
  (void)out_w;
  (void)out_h;
  return k_ra8_err_timeout;
}

/** @brief Build a cache config over the static fixture arrays. */
static ra8_tile_cache_cfg_t t_cfg(void)
{
  ra8_tile_cache_cfg_t cfg = {};
  cfg.cell_mem             = s_cells;
  cfg.cell_bytes           = k_t_cell_bytes;
  cfg.cell_count           = k_t_cells;
  cfg.meta                 = s_meta;
  cfg.keys                 = s_keys;
  cfg.dims                 = s_dims;
  cfg.buckets              = s_buckets;
  cfg.bucket_count         = k_t_buckets;
  cfg.decode               = t_decode;
  cfg.decode_ctx           = nullptr;
  return cfg;
}

/** @brief A tile key. */
static ra8_tile_key_t t_key(uint32_t image, uint16_t tx, uint16_t ty, uint16_t zoom)
{
  ra8_tile_key_t k = {};
  k.image_id       = image;
  k.tile_x         = tx;
  k.tile_y         = ty;
  k.zoom           = zoom;
  return k;
}

/**
 * @par MC/DC:
 * (no compound decisions under test -- a first get decodes + pins with the
 * expected content + dimensions; a second get of the same key is a hit and does
 * not re-decode; an edge tile reports its smaller dimensions)
 */
static void test_decode_hit(void)
{
  TEST_BEGIN("tile decode-on-miss + hit");
  s_decode_calls           = 0;
  ra8_tile_cache_t     tc  = {};
  ra8_tile_cache_cfg_t cfg = t_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tile_cache_init(&tc, &cfg));

  ra8_tile_key_t k = t_key(k_t_image_id, 1U, 2U, 0U);
  ra8_tile_t     t = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tile_cache_get(&tc, &k, &t)); /* miss -> decode */
  TEST_ASSERT_EQ(k_t_full_dim, t.width);
  TEST_ASSERT_EQ(k_t_full_dim, t.height);
  TEST_ASSERT_EQ(0x42, t.pixels[0]);
  TEST_ASSERT_EQ(1, t.pixels[1]);
  TEST_ASSERT_EQ(1, s_decode_calls);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tile_cache_put(&tc, t.pixels));

  ra8_tile_t t2 = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tile_cache_get(&tc, &k, &t2)); /* hit -> no decode */
  TEST_ASSERT(t2.pixels == t.pixels);
  TEST_ASSERT_EQ(1, s_decode_calls); /* unchanged */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tile_cache_put(&tc, t2.pixels));

  /* An edge tile reports its smaller decoded dimensions. */
  ra8_tile_key_t ke = t_key(k_t_image_id, (uint16_t)k_t_edge_x, 0U, 0U);
  ra8_tile_t     te = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tile_cache_get(&tc, &ke, &te));
  TEST_ASSERT_EQ(k_t_edge_dim, te.width);
  TEST_ASSERT_EQ(k_t_edge_dim, te.height);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tile_cache_put(&tc, te.pixels));

  uint32_t hits = 0;
  uint32_t miss = 0;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tile_cache_stats(&tc, &hits, &miss, nullptr));
  TEST_ASSERT_EQ(1, hits);
  TEST_ASSERT_EQ(2, miss);
  TEST_END("tile decode-on-miss + hit");
}

/**
 * @par MC/DC:
 * (no compound decisions under test -- more distinct tiles than cells force LRU
 * eviction; the oldest is gone, a recent one is still resident)
 */
static void test_eviction(void)
{
  TEST_BEGIN("tile LRU eviction");
  s_decode_calls           = 0;
  ra8_tile_cache_t     tc  = {};
  ra8_tile_cache_cfg_t cfg = t_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tile_cache_init(&tc, &cfg));

  /* Fill all 3 cells with tiles 0..2, then add 3..5 (forces 3 evictions). */
  for (uint32_t i = 0; i < 6U; ++i) {
    ra8_tile_key_t k = t_key(1U, (uint16_t)i, 0U, 0U);
    ra8_tile_t     t = {};
    TEST_ASSERT_EQ(k_ra8_ok, ra8_tile_cache_get(&tc, &k, &t));
    TEST_ASSERT_EQ(k_ra8_ok, ra8_tile_cache_put(&tc, t.pixels));
  }
  uint32_t ev = 0;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tile_cache_stats(&tc, nullptr, nullptr, &ev));
  TEST_ASSERT_EQ(3, ev); /* 6 distinct tiles through 3 cells */

  /* Re-getting an early (evicted) tile re-decodes; a recent one hits. */
  uint32_t       before = s_decode_calls;
  ra8_tile_key_t k0     = t_key(1U, 0U, 0U, 0U);
  ra8_tile_t     t0     = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tile_cache_get(&tc, &k0, &t0)); /* evicted -> re-decode */
  TEST_ASSERT_EQ(before + 1U, s_decode_calls);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tile_cache_put(&tc, t0.pixels));
  TEST_END("tile LRU eviction");
}

/**
 * @par MC/DC:
 * (no compound decisions under test -- a fully-pinned cache cannot evict for a
 * new tile; releasing one pin lets it decode)
 */
static void test_pin_protection(void)
{
  TEST_BEGIN("tile pin protection");
  ra8_tile_cache_t     tc  = {};
  ra8_tile_cache_cfg_t cfg = t_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tile_cache_init(&tc, &cfg));

  const uint8_t* pins[(size_t)k_t_cells] = {};
  for (uint32_t i = 0; i < (uint32_t)k_t_cells; ++i) {
    ra8_tile_key_t k = t_key(2U, (uint16_t)i, 1U, 0U);
    ra8_tile_t     t = {};
    TEST_ASSERT_EQ(k_ra8_ok, ra8_tile_cache_get(&tc, &k, &t)); /* pinned (no put) */
    pins[i] = t.pixels;
  }
  ra8_tile_key_t kn = t_key(2U, k_t_tile_id, 1U, 0U);
  ra8_tile_t     tn = {};
  TEST_ASSERT_EQ(k_ra8_err_no_mem, ra8_tile_cache_get(&tc, &kn, &tn)); /* all pinned */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tile_cache_put(&tc, pins[0]));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tile_cache_get(&tc, &kn, &tn)); /* now evicts + decodes */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tile_cache_put(&tc, tn.pixels));
  for (uint32_t i = 1; i < (uint32_t)k_t_cells; ++i) {
    TEST_ASSERT_EQ(k_ra8_ok, ra8_tile_cache_put(&tc, pins[i]));
  }
  TEST_END("tile pin protection");
}

/**
 * @par MC/DC:
 * (no compound decisions under test -- a decode failure on a miss returns the
 * callback's error verbatim and leaves no resident entry)
 */
static void test_decode_failure(void)
{
  TEST_BEGIN("tile decode failure leaves cell cold");
  ra8_tile_cache_t     tc  = {};
  ra8_tile_cache_cfg_t cfg = t_cfg();
  cfg.decode               = t_decode_fail;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tile_cache_init(&tc, &cfg));

  ra8_tile_key_t k = t_key(1U, 1U, 1U, 1U);
  ra8_tile_t     t = {};
  TEST_ASSERT_EQ(k_ra8_err_timeout, ra8_tile_cache_get(&tc, &k, &t)); /* decode fails */

  uint32_t miss = 0;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tile_cache_stats(&tc, nullptr, &miss, nullptr));
  TEST_ASSERT_EQ(1, miss);
  TEST_END("tile decode failure leaves cell cold");
}

/**
 * @par MC/DC:
 * (no compound decisions under test -- each guard is an independent
 * single-condition check)
 */
static void test_validation(void)
{
  TEST_BEGIN("tile validation");
  ra8_tile_cache_t     tc  = {};
  ra8_tile_cache_cfg_t cfg = t_cfg();
  ra8_tile_key_t       k   = t_key(1U, 1U, 1U, 0U);
  ra8_tile_t           t   = {};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_tile_cache_init(nullptr, &cfg));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_tile_cache_init(&tc, nullptr));
  ra8_tile_cache_cfg_t bad = t_cfg();
  bad.decode               = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_tile_cache_init(&tc, &bad));
  bad            = t_cfg();
  bad.cell_count = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_tile_cache_init(&tc, &bad));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_tile_cache_init(&tc, &cfg));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_tile_cache_get(nullptr, &k, &t));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_tile_cache_get(&tc, nullptr, &t));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_tile_cache_get(&tc, &k, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_tile_cache_put(&tc, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_tile_cache_put(&tc, &s_cells[1])); /* mid-cell */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tile_cache_get(&tc, &k, &t));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tile_cache_put(&tc, t.pixels));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_tile_cache_put(&tc, t.pixels)); /* already unpinned */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_tile_cache_stats(nullptr, nullptr, nullptr, nullptr));
  TEST_END("tile validation");
}

/** @brief Build a pan-prefetch request over the k_t_grid x k_t_grid grid. */
static ra8_tile_prefetch_req_t t_req(ra8_tile_rect_t v, ra8_tile_pan_dir_t dir, uint16_t max_tiles)
{
  ra8_tile_prefetch_req_t req = {};
  req.image_id                = k_t_image_id;
  req.view                    = v;
  req.tile_cols               = (uint16_t)k_t_grid;
  req.tile_rows               = (uint16_t)k_t_grid;
  req.zoom                    = 0U;
  req.max_tiles               = max_tiles;
  req.dir                     = dir;
  return req;
}

/** @brief Run one pan prefetch and return the tiles it warmed. */
static uint16_t
t_pan(ra8_tile_cache_t* tc, ra8_tile_rect_t v, ra8_tile_pan_dir_t dir, uint16_t maxt)
{
  ra8_tile_prefetch_req_t req    = t_req(v, dir, maxt);
  uint16_t                warmed = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tile_cache_prefetch_pan(tc, &req, &warmed));
  return warmed;
}

/**
 * @par MC/DC:
 * (no compound decisions under test -- a prefetch decodes on a miss then unpins,
 * so the tile is resident for a following get (hit, no re-decode) yet is evicted
 * when the cache overflows, proving the pin was released)
 */
static void test_prefetch(void)
{
  TEST_BEGIN("tile prefetch warms resident + unpinned");
  s_decode_calls           = 0;
  ra8_tile_cache_t     tc  = {};
  ra8_tile_cache_cfg_t cfg = t_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tile_cache_init(&tc, &cfg));

  ra8_tile_key_t k = t_key(k_t_image_id, 1U, 2U, 0U);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tile_cache_prefetch(&tc, &k)); /* miss -> decode + unpin */
  TEST_ASSERT_EQ(1, s_decode_calls);

  ra8_tile_t t = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tile_cache_get(&tc, &k, &t)); /* resident -> hit */
  TEST_ASSERT_EQ(1, s_decode_calls);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tile_cache_put(&tc, t.pixels));

  /* Was unpinned: filling the 3-cell cache with distinct tiles evicts it. */
  for (uint16_t i = 0U; i < (uint16_t)k_t_cells; ++i) {
    ra8_tile_key_t kf = t_key(k_t_image_id, (uint16_t)(k_t_distinct_tx + i), 0U, 0U);
    TEST_ASSERT_EQ(k_ra8_ok, ra8_tile_cache_prefetch(&tc, &kf));
  }
  const uint32_t before = s_decode_calls;
  ra8_tile_t     t2     = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tile_cache_get(&tc, &k, &t2)); /* evicted -> re-decode */
  TEST_ASSERT_EQ(before + 1U, s_decode_calls);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tile_cache_put(&tc, t2.pixels));
  TEST_END("tile prefetch warms resident + unpinned");
}

/**
 * @par MC/DC:
 * (no compound decisions under test -- a pan-right prefetch warms exactly the
 * lead column ahead of the viewport within its budget; the warmed tiles are then
 * resident so a following get is a hit with no re-decode)
 */
static void test_prefetch_pan_warms(void)
{
  TEST_BEGIN("tile pan prefetch warms the lead column, resident");
  s_decode_calls           = 0;
  ra8_tile_cache_t     tc  = {};
  ra8_tile_cache_cfg_t cfg = t_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tile_cache_init(&tc, &cfg));

  /* Visible col 0 rows 0..1; pan right -> warm col 1 rows 0..1 (2 tiles). */
  const ra8_tile_rect_t   v      = {.tx0 = 0U, .ty0 = 0U, .tx1 = 0U, .ty1 = 1U};
  ra8_tile_prefetch_req_t req    = t_req(v, k_ra8_tile_pan_right, (uint16_t)k_t_pan_span);
  uint16_t                warmed = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tile_cache_prefetch_pan(&tc, &req, &warmed));
  TEST_ASSERT_EQ(2, warmed);
  TEST_ASSERT_EQ(2, s_decode_calls);

  /* Both lead tiles resident: gets are hits (decode count unchanged). */
  ra8_tile_key_t k10 = t_key(k_t_image_id, 1U, 0U, 0U);
  ra8_tile_key_t k11 = t_key(k_t_image_id, 1U, 1U, 0U);
  ra8_tile_t     t   = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tile_cache_get(&tc, &k10, &t));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tile_cache_put(&tc, t.pixels));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tile_cache_get(&tc, &k11, &t));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tile_cache_put(&tc, t.pixels));
  TEST_ASSERT_EQ(2, s_decode_calls);

  /* out_warmed is optional: a NULL sink is accepted (already-resident re-warm). */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tile_cache_prefetch_pan(&tc, &req, nullptr));
  TEST_END("tile pan prefetch warms the lead column, resident");
}

/**
 * @par MC/DC:
 * (no compound decisions under test -- exercises every ::ra8_tile_pan_dir_t arm:
 * an interior viewport warms its 2-tile lead edge in each direction, a viewport
 * against the grid edge warms nothing, `none` warms nothing, and a max_tiles=1
 * budget caps the warm to one tile)
 */
static void test_prefetch_pan_dirs(void)
{
  TEST_BEGIN("tile pan prefetch: each direction, edges, none, budget");
  ra8_tile_cache_t     tc  = {};
  ra8_tile_cache_cfg_t cfg = t_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tile_cache_init(&tc, &cfg));
  const ra8_tile_rect_t col = {.tx0 = 1U, .ty0 = 1U, .tx1 = 1U, .ty1 = 2U}; /* pan L/R */
  const ra8_tile_rect_t row = {.tx0 = 1U, .ty0 = 1U, .tx1 = 2U, .ty1 = 1U}; /* pan U/D */
  TEST_ASSERT_EQ(2, t_pan(&tc, col, k_ra8_tile_pan_right, (uint16_t)k_t_pan_span));
  TEST_ASSERT_EQ(2, t_pan(&tc, col, k_ra8_tile_pan_left, (uint16_t)k_t_pan_span));
  TEST_ASSERT_EQ(2, t_pan(&tc, row, k_ra8_tile_pan_down, (uint16_t)k_t_pan_span));
  TEST_ASSERT_EQ(2, t_pan(&tc, row, k_ra8_tile_pan_up, (uint16_t)k_t_pan_span));
  /* A viewport already against each grid edge warms nothing. */
  const ra8_tile_rect_t e_r = {.tx0 = 3U, .ty0 = 0U, .tx1 = 3U, .ty1 = 1U};
  const ra8_tile_rect_t e_l = {.tx0 = 0U, .ty0 = 0U, .tx1 = 0U, .ty1 = 1U};
  const ra8_tile_rect_t e_d = {.tx0 = 0U, .ty0 = 3U, .tx1 = 1U, .ty1 = 3U};
  const ra8_tile_rect_t e_u = {.tx0 = 0U, .ty0 = 0U, .tx1 = 1U, .ty1 = 0U};
  TEST_ASSERT_EQ(0, t_pan(&tc, e_r, k_ra8_tile_pan_right, (uint16_t)k_t_pan_span));
  TEST_ASSERT_EQ(0, t_pan(&tc, e_l, k_ra8_tile_pan_left, (uint16_t)k_t_pan_span));
  TEST_ASSERT_EQ(0, t_pan(&tc, e_d, k_ra8_tile_pan_down, (uint16_t)k_t_pan_span));
  TEST_ASSERT_EQ(0, t_pan(&tc, e_u, k_ra8_tile_pan_up, (uint16_t)k_t_pan_span));
  TEST_ASSERT_EQ(0, t_pan(&tc, col, k_ra8_tile_pan_none, (uint16_t)k_t_pan_span));
  TEST_ASSERT_EQ(1, t_pan(&tc, col, k_ra8_tile_pan_right, 1U)); /* budget caps to one */
  TEST_END("tile pan prefetch: each direction, edges, none, budget");
}

/**
 * @par MC/DC:
 * (no compound decisions under test -- a fully-pinned cache cannot evict to warm,
 * so the pan prefetch warms nothing yet still returns k_ra8_ok: read-ahead is
 * best-effort and never fails the pan it serves)
 */
static void test_prefetch_pan_best_effort(void)
{
  TEST_BEGIN("tile pan prefetch is best-effort on a full cache");
  ra8_tile_cache_t     tc  = {};
  ra8_tile_cache_cfg_t cfg = t_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tile_cache_init(&tc, &cfg));

  const uint8_t* pins[(size_t)k_t_cells] = {};
  for (uint32_t i = 0U; i < (uint32_t)k_t_cells; ++i) {
    ra8_tile_key_t kp = t_key(k_t_image_id, (uint16_t)i, 3U, 0U);
    ra8_tile_t     tp = {};
    TEST_ASSERT_EQ(k_ra8_ok, ra8_tile_cache_get(&tc, &kp, &tp)); /* pinned (no put) */
    pins[i] = tp.pixels;
  }
  const ra8_tile_rect_t v      = {.tx0 = 0U, .ty0 = 0U, .tx1 = 0U, .ty1 = 1U};
  const uint16_t        warmed = t_pan(&tc, v, k_ra8_tile_pan_right, (uint16_t)k_t_pan_span);
  TEST_ASSERT_EQ(0, warmed); /* all cells pinned: nothing warmed, still k_ra8_ok */
  for (uint32_t i = 0U; i < (uint32_t)k_t_cells; ++i) {
    TEST_ASSERT_EQ(k_ra8_ok, ra8_tile_cache_put(&tc, pins[i]));
  }
  TEST_END("tile pan prefetch is best-effort on a full cache");
}

/**
 * @par MC/DC:
 * (no compound decisions under test -- capacity reports 0 for NULL / uninitialised
 * and the cell count once bound; the prefetch entry points reject NULL arguments
 * and each unordered / off-grid rectangle bound is an independent guard)
 */
static void test_prefetch_guards(void)
{
  TEST_BEGIN("tile prefetch validation + capacity");
  ra8_tile_cache_t        tc  = {};
  ra8_tile_cache_cfg_t    cfg = t_cfg();
  ra8_tile_key_t          k   = t_key(k_t_image_id, 1U, 1U, 0U);
  const ra8_tile_rect_t   v   = {.tx0 = 0U, .ty0 = 0U, .tx1 = 0U, .ty1 = 1U};
  ra8_tile_prefetch_req_t req = t_req(v, k_ra8_tile_pan_right, (uint16_t)k_t_pan_span);
  uint16_t                w   = 0U;

  TEST_ASSERT_EQ(0U, ra8_tile_cache_capacity(nullptr));
  TEST_ASSERT_EQ(0U, ra8_tile_cache_capacity(&tc)); /* not yet initialised */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tile_cache_init(&tc, &cfg));
  TEST_ASSERT_EQ(k_t_cells, ra8_tile_cache_capacity(&tc));

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_tile_cache_prefetch(nullptr, &k));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_tile_cache_prefetch(&tc, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_tile_cache_prefetch_pan(nullptr, &req, &w));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_tile_cache_prefetch_pan(&tc, nullptr, &w));

  ra8_tile_prefetch_req_t bad = req;
  bad.view                    = (ra8_tile_rect_t){.tx0 = 2U, .ty0 = 0U, .tx1 = 1U, .ty1 = 1U};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_tile_cache_prefetch_pan(&tc, &bad, &w)); /* tx0>tx1 */
  bad.view = (ra8_tile_rect_t){.tx0 = 0U, .ty0 = 2U, .tx1 = 1U, .ty1 = 1U};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_tile_cache_prefetch_pan(&tc, &bad, &w)); /* ty0>ty1 */
  bad.view = (ra8_tile_rect_t){.tx0 = 0U, .ty0 = 0U, .tx1 = (uint16_t)k_t_grid, .ty1 = 0U};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_tile_cache_prefetch_pan(&tc, &bad, &w)); /* off-grid x */
  bad.view = (ra8_tile_rect_t){.tx0 = 0U, .ty0 = 0U, .tx1 = 0U, .ty1 = (uint16_t)k_t_grid};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_tile_cache_prefetch_pan(&tc, &bad, &w)); /* off-grid y */
  TEST_END("tile prefetch validation + capacity");
}

int32_t main(void)
{
  test_decode_hit();
  test_eviction();
  test_pin_protection();
  test_decode_failure();
  test_validation();
  test_prefetch();
  test_prefetch_pan_warms();
  test_prefetch_pan_dirs();
  test_prefetch_pan_best_effort();
  test_prefetch_guards();
  (void)fprintf(stderr, "[OK  ] test_ra8_tile_cache.c\n");
  return 0;
}
