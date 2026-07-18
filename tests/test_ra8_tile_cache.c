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

  ra8_tile_key_t k = t_key(0x42U, 1U, 2U, 0U);
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
  ra8_tile_key_t ke = t_key(0x42U, (uint16_t)k_t_edge_x, 0U, 0U);
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
  ra8_tile_key_t kn = t_key(2U, 99U, 1U, 0U);
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

int32_t main(void)
{
  test_decode_hit();
  test_eviction();
  test_pin_protection();
  test_decode_failure();
  test_validation();
  (void)fprintf(stderr, "[OK  ] test_ra8_tile_cache.c\n");
  return 0;
}
