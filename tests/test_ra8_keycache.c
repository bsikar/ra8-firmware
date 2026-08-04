/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file test_ra8_keycache.c
 * @brief Unit tests for the reusable ra8_mem keyed-LRU cache (Layer 3, #147).
 *
 * @details
 * Exercises render-on-miss + hit (with rendered content + the user descriptor),
 * LRU eviction past capacity, the pin/unpin contract (a fully-pinned cache cannot
 * evict), a render-callback failure leaving the victim cold, the no-descriptor
 * (`user_bytes == 0`) configuration, and the validation guards. These are the
 * regression guard shared by ::ra8_glyph_atlas and ::ra8_tile_cache, which are both
 * thin facades over this cache.
 */

#include <stdint.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_keycache.h"
#include "unity_minimal.h"

/**
 * @enum keycache_fixture_t
 * @brief Out-of-range and malformed inputs the code under test must reject.
 */
typedef enum : uint8_t {
  k_kc_image_a = 0x41U, /**< Image id of the first cache key. */
  k_kc_tile_shared =
    7U, /**< Tile index reused across images; lookups discriminate on the image half of the key. */
  /** Its tile index. */
  k_kc_tile_a = 0x99U,
  /** An image id no entry was inserted under, so the lookup must miss. */
  k_kc_image_absent = 99U,
  /** Image id of a second key, sharing no byte with the first. */
  k_kc_image_b = 0xABU,
  /** Its tile index. */
  k_kc_tile_b = 0xCDU,
} keycache_fixture_t;

/**
 * @enum t_kc_const_t
 * @brief Fixture sizes.
 */
typedef enum : uint32_t {
  k_t_cell_bytes = 32U, /**< Bytes per cell.     */
  k_t_cells      = 4U,  /**< Cells in the cache. */
  k_t_buckets    = 8U,  /**< Hash buckets.       */
} t_kc_const_t;

/**
 * @enum t_kc_dim_t
 * @brief Stub render output dimensions.
 */
typedef enum : uint16_t {
  k_t_dim_w = 5U, /**< Stub rendered width.  */
  k_t_dim_h = 6U, /**< Stub rendered height. */
} t_kc_dim_t;

/**
 * @struct t_key_t
 * @brief A padding-free fixture key.
 */
typedef struct {
  uint32_t a; /**< First key field.  */
  uint32_t b; /**< Second key field. */
} t_key_t;

/**
 * @struct t_dims_t
 * @brief A fixture per-cell user descriptor.
 */
typedef struct {
  uint16_t w; /**< Rendered width.  */
  uint16_t h; /**< Rendered height. */
} t_dims_t;

static uint8_t             s_cells[(size_t)k_t_cells * (size_t)k_t_cell_bytes];
static uint8_t             s_keys[(size_t)k_t_cells * sizeof(t_key_t)];
static t_dims_t            s_users[(size_t)k_t_cells];
static ra8_keycache_cell_t s_meta[(size_t)k_t_cells];
static int32_t             s_buckets[(size_t)k_t_buckets];
static uint32_t            s_render_calls;

/** @brief Stub renderer: stamps the key into the cell, writes fixed dims. */
static ra8_err_t
t_render(void* ctx, const void* key, uint8_t* cell, uint32_t cell_bytes, void* user)
{
  (void)ctx;
  s_render_calls++;
  const t_key_t* k = (const t_key_t*)key;
  (void)memset(cell, 0, (size_t)cell_bytes);
  cell[0] = (uint8_t)k->a;
  cell[1] = (uint8_t)k->b;
  if (user != nullptr) {
    t_dims_t* d = (t_dims_t*)user;
    d->w        = (uint16_t)k_t_dim_w;
    d->h        = (uint16_t)k_t_dim_h;
  }
  return k_ra8_ok;
}

/** @brief Stub renderer that always fails (render-on-miss failure path). */
/* The pointer parameters below cannot be const: this mock implements a
 * function-pointer interface (the DI seam under test), so its signature is
 * fixed by the typedef it is assigned to -- adding const changes the
 * function type and the assignment stops compiling. */
// NOLINTBEGIN(readability-non-const-parameter)
static ra8_err_t
t_render_fail(void* ctx, const void* key, uint8_t* cell, uint32_t cell_bytes, void* user)
// NOLINTEND(readability-non-const-parameter)
{
  (void)ctx;
  (void)key;
  (void)cell;
  (void)cell_bytes;
  (void)user;
  return k_ra8_err_timeout;
}

/** @brief Build a cache config over the static fixture arrays. */
static ra8_keycache_cfg_t t_cfg(void)
{
  ra8_keycache_cfg_t cfg = {};
  cfg.cell_mem           = s_cells;
  cfg.cell_bytes         = k_t_cell_bytes;
  cfg.cell_count         = k_t_cells;
  cfg.key_mem            = s_keys;
  cfg.key_bytes          = (uint32_t)sizeof(t_key_t);
  cfg.user_mem           = (uint8_t*)s_users;
  cfg.user_bytes         = (uint32_t)sizeof(t_dims_t);
  cfg.meta               = s_meta;
  cfg.buckets            = s_buckets;
  cfg.bucket_count       = k_t_buckets;
  cfg.render             = t_render;
  cfg.render_ctx         = nullptr;
  return cfg;
}

/** @brief A fixture key. */
static t_key_t t_key(uint32_t a, uint32_t b)
{
  t_key_t k = {};
  k.a       = a;
  k.b       = b;
  return k;
}

/**
 * @par MC/DC:
 * (no compound decisions under test -- a first get renders + pins with the
 * expected content + descriptor; a second get of the same key is a hit and does
 * not re-render)
 */
static void test_render_hit(void)
{
  TEST_BEGIN("keycache render-on-miss + hit");
  s_render_calls         = 0;
  ra8_keycache_t     kc  = {};
  ra8_keycache_cfg_t cfg = t_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_keycache_init(&kc, &cfg));

  t_key_t             k = t_key(k_kc_image_a, k_kc_tile_a);
  ra8_keycache_view_t v = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_keycache_get(&kc, &k, &v)); /* miss -> render */
  TEST_ASSERT_EQ(0x41, v.data[0]);                         /* key.a stamp    */
  TEST_ASSERT_EQ(0x99, v.data[1]);                         /* key.b stamp    */
  TEST_ASSERT(v.user != nullptr);
  const t_dims_t* d = (const t_dims_t*)v.user;
  TEST_ASSERT_EQ(k_t_dim_w, d->w);
  TEST_ASSERT_EQ(k_t_dim_h, d->h);
  TEST_ASSERT_EQ(1, s_render_calls);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_keycache_put(&kc, v.data));

  ra8_keycache_view_t v2 = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_keycache_get(&kc, &k, &v2)); /* hit -> no render */
  TEST_ASSERT(v2.data == v.data);
  TEST_ASSERT_EQ(1, s_render_calls); /* unchanged */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_keycache_put(&kc, v2.data));

  uint32_t hits = 0;
  uint32_t miss = 0;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_keycache_stats(&kc, &hits, &miss, nullptr));
  TEST_ASSERT_EQ(1, hits);
  TEST_ASSERT_EQ(1, miss);
  TEST_END("keycache render-on-miss + hit");
}

/**
 * @par MC/DC:
 * (no compound decisions under test -- more distinct keys than cells force LRU
 * eviction; the oldest is gone, a recent one is still resident)
 */
static void test_eviction(void)
{
  TEST_BEGIN("keycache LRU eviction");
  s_render_calls         = 0;
  ra8_keycache_t     kc  = {};
  ra8_keycache_cfg_t cfg = t_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_keycache_init(&kc, &cfg));

  /* Fill all 4 cells with keys 0..3, then add 4..7 (forces 4 evictions). */
  for (uint32_t i = 0; i < 8U; ++i) {
    t_key_t             k = t_key(i, 0U);
    ra8_keycache_view_t v = {};
    TEST_ASSERT_EQ(k_ra8_ok, ra8_keycache_get(&kc, &k, &v));
    TEST_ASSERT_EQ(k_ra8_ok, ra8_keycache_put(&kc, v.data));
  }
  uint32_t ev = 0;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_keycache_stats(&kc, nullptr, nullptr, &ev));
  TEST_ASSERT_EQ(4, ev); /* 8 distinct keys through 4 cells */

  /* Re-getting an early (evicted) key re-renders; a recent one hits. */
  uint32_t            before = s_render_calls;
  t_key_t             k0     = t_key(0U, 0U);
  ra8_keycache_view_t v0     = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_keycache_get(&kc, &k0, &v0)); /* evicted -> re-render */
  TEST_ASSERT_EQ(before + 1U, s_render_calls);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_keycache_put(&kc, v0.data));

  uint32_t            before2 = s_render_calls;
  t_key_t             k7      = t_key(k_kc_tile_shared, 0U);
  ra8_keycache_view_t v7      = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_keycache_get(&kc, &k7, &v7)); /* recent -> hit */
  TEST_ASSERT_EQ(before2, s_render_calls);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_keycache_put(&kc, v7.data));
  TEST_END("keycache LRU eviction");
}

/**
 * @par MC/DC:
 * (no compound decisions under test -- a fully-pinned cache cannot evict for a
 * new key; releasing one pin lets it render)
 */
static void test_pin_protection(void)
{
  TEST_BEGIN("keycache pin protection");
  ra8_keycache_t     kc  = {};
  ra8_keycache_cfg_t cfg = t_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_keycache_init(&kc, &cfg));

  const uint8_t* pins[(size_t)k_t_cells] = {};
  for (uint32_t i = 0; i < (uint32_t)k_t_cells; ++i) {
    t_key_t             k = t_key(i, k_kc_tile_shared);
    ra8_keycache_view_t v = {};
    TEST_ASSERT_EQ(k_ra8_ok, ra8_keycache_get(&kc, &k, &v)); /* pinned (no put) */
    pins[i] = v.data;
  }
  t_key_t             kn = t_key(k_kc_image_absent, k_kc_tile_shared);
  ra8_keycache_view_t vn = {};
  TEST_ASSERT_EQ(k_ra8_err_no_mem, ra8_keycache_get(&kc, &kn, &vn)); /* all pinned */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_keycache_put(&kc, pins[0]));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_keycache_get(&kc, &kn, &vn)); /* now evicts + renders */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_keycache_put(&kc, vn.data));
  for (uint32_t i = 1; i < (uint32_t)k_t_cells; ++i) {
    TEST_ASSERT_EQ(k_ra8_ok, ra8_keycache_put(&kc, pins[i]));
  }
  TEST_END("keycache pin protection");
}

/**
 * @par MC/DC:
 * (no compound decisions under test -- a render failure on a miss returns the
 * callback's error verbatim and leaves no resident entry; a later successful get
 * of the same key renders and succeeds)
 */
static void test_render_failure(void)
{
  TEST_BEGIN("keycache render failure leaves cell cold");
  ra8_keycache_t     kc  = {};
  ra8_keycache_cfg_t cfg = t_cfg();
  cfg.render             = t_render_fail;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_keycache_init(&kc, &cfg));

  t_key_t             k = t_key(1U, 2U);
  ra8_keycache_view_t v = {};
  TEST_ASSERT_EQ(k_ra8_err_timeout, ra8_keycache_get(&kc, &k, &v)); /* render fails */

  /* Nothing should be resident: a subsequent get is still a miss. */
  uint32_t miss = 0;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_keycache_stats(&kc, nullptr, &miss, nullptr));
  TEST_ASSERT_EQ(1, miss);

  /* Swap in a working renderer; the same key now succeeds. */
  cfg.render = t_render;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_keycache_init(&kc, &cfg));
  s_render_calls = 0;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_keycache_get(&kc, &k, &v));
  TEST_ASSERT_EQ(1, s_render_calls);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_keycache_put(&kc, v.data));
  TEST_END("keycache render failure leaves cell cold");
}

/**
 * @par MC/DC:
 * (no compound decisions under test -- a cache configured with user_bytes == 0
 * returns a NULL user pointer but still caches data)
 */
static void test_no_user_descriptor(void)
{
  TEST_BEGIN("keycache without user descriptor");
  ra8_keycache_t     kc  = {};
  ra8_keycache_cfg_t cfg = t_cfg();
  cfg.user_mem           = nullptr;
  cfg.user_bytes         = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_keycache_init(&kc, &cfg));

  t_key_t             k = t_key(k_kc_image_b, k_kc_tile_b);
  ra8_keycache_view_t v = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_keycache_get(&kc, &k, &v));
  TEST_ASSERT(v.user == nullptr);
  TEST_ASSERT_EQ(0xAB, v.data[0]);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_keycache_put(&kc, v.data));
  TEST_END("keycache without user descriptor");
}

/**
 * @par MC/DC:
 * (no compound decisions under test -- each guard is an independent
 * single-condition check)
 */
static void test_validation(void)
{
  TEST_BEGIN("keycache validation");
  ra8_keycache_t      kc  = {};
  ra8_keycache_cfg_t  cfg = t_cfg();
  t_key_t             k   = t_key(1U, 1U);
  ra8_keycache_view_t v   = {};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_keycache_init(nullptr, &cfg));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_keycache_init(&kc, nullptr));
  ra8_keycache_cfg_t bad = t_cfg();
  bad.cell_count         = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_keycache_init(&kc, &bad));
  bad           = t_cfg();
  bad.key_bytes = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_keycache_init(&kc, &bad));
  bad            = t_cfg();
  bad.user_bytes = (uint32_t)sizeof(t_dims_t);
  bad.user_mem   = nullptr; /* descriptor requested but no storage */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_keycache_init(&kc, &bad));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_keycache_init(&kc, &cfg));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_keycache_get(nullptr, &k, &v));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_keycache_get(&kc, nullptr, &v));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_keycache_get(&kc, &k, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_keycache_put(&kc, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_keycache_put(&kc, &s_cells[1])); /* mid-cell */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_keycache_get(&kc, &k, &v));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_keycache_put(&kc, v.data));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_keycache_put(&kc, v.data)); /* already unpinned */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_keycache_stats(nullptr, nullptr, nullptr, nullptr));
  TEST_END("keycache validation");
}

int32_t main(void)
{
  test_render_hit();
  test_eviction();
  test_pin_protection();
  test_render_failure();
  test_no_user_descriptor();
  test_validation();
  (void)fprintf(stderr, "[OK  ] test_ra8_keycache.c\n");
  return 0;
}
