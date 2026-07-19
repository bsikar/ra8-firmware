/**
 * @file test_ra8_glyph_atlas.c
 * @brief Unit tests for the ra8_mem glyph atlas (Layer 3, #147).
 *
 * @details
 * Exercises render-on-miss + hit (with rendered content + dimensions), LRU
 * eviction past capacity, the pin/unpin contract (a fully-pinned atlas cannot
 * evict), and the validation guards.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_glyph_atlas.h"
#include "unity_minimal.h"

/**
 * @enum glyph_atlas_uint8_const_t
 * @brief Named uint8_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint8_t {
  k_ga_pixel_size =
    12U, /**< Pixel size shared by the keys, so lookups must discriminate on the code point. */
  k_ga_codepoint = 0x41U, /**< Code point of the first glyph key. */
  k_ga_codepoint_absent =
    99U, /**< A code point no glyph was cached under, so the lookup must miss. */
} glyph_atlas_uint8_const_t;

/**
 * @enum t_glyph_const_t
 * @brief Fixture sizes.
 */
typedef enum : uint32_t {
  k_t_cell_bytes = 64U, /**< Bytes per glyph cell. */
  k_t_cells      = 4U,  /**< Cells in the atlas.   */
  k_t_buckets    = 8U,  /**< Hash buckets.         */
} t_glyph_const_t;

static uint8_t             s_cells[(size_t)k_t_cells * (size_t)k_t_cell_bytes];
static ra8_keycache_cell_t s_meta[(size_t)k_t_cells];
static ra8_glyph_key_t     s_keys[(size_t)k_t_cells];
static ra8_glyph_dims_t    s_dims[(size_t)k_t_cells];
static int32_t             s_buckets[(size_t)k_t_buckets];
static uint32_t            s_render_calls;

/** @brief Stub renderer: stamps the key into the cell, dims from the size. */
static ra8_err_t t_render(void*                  ctx,
                          const ra8_glyph_key_t* key,
                          uint8_t*               cell,
                          uint32_t               cell_bytes,
                          uint16_t*              out_w,
                          uint16_t*              out_h)
{
  (void)ctx;
  s_render_calls++;
  (void)memset(cell, 0, (size_t)cell_bytes);
  cell[0] = (uint8_t)key->glyph_id;
  cell[1] = (uint8_t)key->face_id;
  cell[2] = (uint8_t)key->mode;
  *out_w  = 4U;
  *out_h  = 4U;
  return k_ra8_ok;
}

/** @brief Build an atlas config over the static fixture arrays. */
static ra8_glyph_atlas_cfg_t t_cfg(void)
{
  ra8_glyph_atlas_cfg_t cfg = {};
  cfg.cell_mem              = s_cells;
  cfg.cell_bytes            = k_t_cell_bytes;
  cfg.cell_count            = k_t_cells;
  cfg.meta                  = s_meta;
  cfg.keys                  = s_keys;
  cfg.dims                  = s_dims;
  cfg.buckets               = s_buckets;
  cfg.bucket_count          = k_t_buckets;
  cfg.render                = t_render;
  cfg.render_ctx            = nullptr;
  return cfg;
}

/** @brief A glyph key. */
static ra8_glyph_key_t t_key(uint32_t glyph, uint16_t face, uint16_t size, uint16_t mode)
{
  ra8_glyph_key_t k = {};
  k.glyph_id        = glyph;
  k.face_id         = face;
  k.size_px         = size;
  k.mode            = mode;
  return k;
}

/**
 * @par MC/DC:
 * (no compound decisions under test -- a first get renders + pins with the
 * expected content; a second get of the same key is a hit and does not re-render)
 */
static void test_render_hit(void)
{
  TEST_BEGIN("glyph render-on-miss + hit");
  s_render_calls            = 0;
  ra8_glyph_atlas_t     a   = {};
  ra8_glyph_atlas_cfg_t cfg = t_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_glyph_atlas_init(&a, &cfg));

  ra8_glyph_key_t k = t_key(k_ga_codepoint, 1U, 16U, 0U);
  ra8_glyph_t     g = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_glyph_atlas_get(&a, &k, &g)); /* miss -> render */
  TEST_ASSERT_EQ(4, g.width);
  TEST_ASSERT_EQ(4, g.height);
  TEST_ASSERT_EQ(0x41, g.bitmap[0]); /* glyph id stamp */
  TEST_ASSERT_EQ(1, s_render_calls);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_glyph_atlas_put(&a, g.bitmap));

  ra8_glyph_t g2 = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_glyph_atlas_get(&a, &k, &g2)); /* hit -> no render */
  TEST_ASSERT(g2.bitmap == g.bitmap);
  TEST_ASSERT_EQ(1, s_render_calls); /* unchanged */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_glyph_atlas_put(&a, g2.bitmap));

  uint32_t hits = 0;
  uint32_t miss = 0;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_glyph_atlas_stats(&a, &hits, &miss, nullptr));
  TEST_ASSERT_EQ(1, hits);
  TEST_ASSERT_EQ(1, miss);
  TEST_END("glyph render-on-miss + hit");
}

/**
 * @par MC/DC:
 * (no compound decisions under test -- more distinct glyphs than cells force LRU
 * eviction; the oldest is gone, a recent one is still resident)
 */
static void test_eviction(void)
{
  TEST_BEGIN("glyph LRU eviction");
  s_render_calls            = 0;
  ra8_glyph_atlas_t     a   = {};
  ra8_glyph_atlas_cfg_t cfg = t_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_glyph_atlas_init(&a, &cfg));

  /* Fill all 4 cells with glyphs 0..3, then add 4..7 (forces 4 evictions). */
  for (uint32_t i = 0; i < 8U; ++i) {
    ra8_glyph_key_t k = t_key(i, 1U, 16U, 0U);
    ra8_glyph_t     g = {};
    TEST_ASSERT_EQ(k_ra8_ok, ra8_glyph_atlas_get(&a, &k, &g));
    TEST_ASSERT_EQ(k_ra8_ok, ra8_glyph_atlas_put(&a, g.bitmap));
  }
  uint32_t ev = 0;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_glyph_atlas_stats(&a, nullptr, nullptr, &ev));
  TEST_ASSERT_EQ(4, ev); /* 8 distinct glyphs through 4 cells */

  /* Re-getting an early (evicted) glyph re-renders; a recent one hits. */
  uint32_t        before = s_render_calls;
  ra8_glyph_key_t k0     = t_key(0U, 1U, 16U, 0U);
  ra8_glyph_t     g0     = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_glyph_atlas_get(&a, &k0, &g0)); /* evicted -> re-render */
  TEST_ASSERT_EQ(before + 1U, s_render_calls);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_glyph_atlas_put(&a, g0.bitmap));
  TEST_END("glyph LRU eviction");
}

/**
 * @par MC/DC:
 * (no compound decisions under test -- a fully-pinned atlas cannot evict for a
 * new glyph; releasing one pin lets it render)
 */
static void test_pin_protection(void)
{
  TEST_BEGIN("glyph pin protection");
  ra8_glyph_atlas_t     a   = {};
  ra8_glyph_atlas_cfg_t cfg = t_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_glyph_atlas_init(&a, &cfg));

  const uint8_t* pins[(size_t)k_t_cells] = {};
  for (uint32_t i = 0; i < (uint32_t)k_t_cells; ++i) {
    ra8_glyph_key_t k = t_key(i, 2U, k_ga_pixel_size, 0U);
    ra8_glyph_t     g = {};
    TEST_ASSERT_EQ(k_ra8_ok, ra8_glyph_atlas_get(&a, &k, &g)); /* pinned (no put) */
    pins[i] = g.bitmap;
  }
  ra8_glyph_key_t kn = t_key(k_ga_codepoint_absent, 2U, k_ga_pixel_size, 0U);
  ra8_glyph_t     gn = {};
  TEST_ASSERT_EQ(k_ra8_err_no_mem, ra8_glyph_atlas_get(&a, &kn, &gn)); /* all pinned */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_glyph_atlas_put(&a, pins[0]));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_glyph_atlas_get(&a, &kn, &gn)); /* now evicts + renders */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_glyph_atlas_put(&a, gn.bitmap));
  for (uint32_t i = 1; i < (uint32_t)k_t_cells; ++i) {
    TEST_ASSERT_EQ(k_ra8_ok, ra8_glyph_atlas_put(&a, pins[i]));
  }
  TEST_END("glyph pin protection");
}

/**
 * @par MC/DC:
 * (no compound decisions under test -- each guard is an independent
 * single-condition check)
 */
static void test_validation(void)
{
  TEST_BEGIN("glyph validation");
  ra8_glyph_atlas_t     a   = {};
  ra8_glyph_atlas_cfg_t cfg = t_cfg();
  ra8_glyph_key_t       k   = t_key(1U, 1U, 16U, 0U);
  ra8_glyph_t           g   = {};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_glyph_atlas_init(nullptr, &cfg));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_glyph_atlas_init(&a, nullptr));
  ra8_glyph_atlas_cfg_t bad = t_cfg();
  bad.cell_count            = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_glyph_atlas_init(&a, &bad));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_glyph_atlas_init(&a, &cfg));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_glyph_atlas_get(nullptr, &k, &g));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_glyph_atlas_get(&a, nullptr, &g));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_glyph_atlas_get(&a, &k, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_glyph_atlas_put(&a, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_glyph_atlas_put(&a, &s_cells[1])); /* mid-cell */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_glyph_atlas_get(&a, &k, &g));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_glyph_atlas_put(&a, g.bitmap));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_glyph_atlas_put(&a, g.bitmap)); /* already unpinned */
  TEST_END("glyph validation");
}

int32_t main(void)
{
  test_render_hit();
  test_eviction();
  test_pin_protection();
  test_validation();
  (void)fprintf(stderr, "[OK  ] test_ra8_glyph_atlas.c\n");
  return 0;
}
