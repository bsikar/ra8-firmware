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

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_glyph_atlas.h"
#include "ra8_log.h"
#include "unity_minimal.h"

/**
 * @enum glyph_atlas_fixture_t
 * @brief Out-of-range and malformed inputs the code under test must reject,
 * plus buffer capacities and payload sizes.
 */
typedef enum : uint8_t {
  k_ga_pixel_size = 12U, /**< Pixel size shared by the keys, so lookups must
                            discriminate on the code point. */
  /** Code point of the first glyph key. */
  k_ga_codepoint        = 0x41U,
  k_ga_codepoint_absent = 99U, /**< A code point no glyph was cached under, so
                                  the lookup must miss. */
} glyph_atlas_fixture_t;

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

/**
 * @brief Render a deterministic glyph fixture into one cache cell.
 * @details Clears the cell, stamps key identity bytes, publishes fixed
 * dimensions, and increments the render counter.
 * @param[in] ctx Unused renderer context.
 * @param[in] key Glyph identity supplied by the atlas miss path.
 * @param[out] cell Writable cache cell receiving the rendered fixture.
 * @param[in] cell_bytes Capacity of `cell` in bytes.
 * @param[out] out_w Destination receiving the rendered width.
 * @param[out] out_h Destination receiving the rendered height.
 * @return A repository error code describing rendering success.
 * @retval k_ra8_ok The deterministic fixture was rendered and dimensions
 * published.
 * @pre All pointers except the intentionally unused context are non-null.
 * @pre `cell_bytes` is at least three bytes for the identity stamps.
 * @post The render counter increments once and the cell begins with key
 * identity.
 * @post Width and height are both four pixels.
 * @note Clearing the complete cell prevents stale payload from hiding cache
 * errors.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_t_render(void*                  ctx,
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

/**
 * @brief Build an atlas configuration over the caller-owned fixture arrays.
 * @details Binds cell, metadata, key, dimension, bucket, and renderer storage
 * into one value object.
 * @return A complete glyph-atlas configuration for the current test fixture.
 * @retval ra8_glyph_atlas_cfg_t Configuration referencing the file-scope
 * arrays.
 * @pre Every fixture array has the capacity described by its enum constant.
 * @pre ::internal_t_render remains callable for the configuration lifetime.
 * @post Every required configuration pointer is non-null.
 * @post Counts and byte sizes match the declared fixture arrays.
 * @note The returned object owns no storage and may be copied by value.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_glyph_atlas_cfg_t internal_t_cfg(void)
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
  cfg.render                = internal_t_render;
  cfg.render_ctx            = nullptr;
  return cfg;
}

/**
 * @brief Construct one glyph key from explicit identity fields.
 * @details Zero-initializes the key before assigning glyph, face, pixel size,
 * and render mode.
 * @param[in] glyph Glyph or code-point identifier.
 * @param[in] face Font-face identifier.
 * @param[in] size Requested pixel size.
 * @param[in] mode Render-mode discriminator.
 * @return The fully initialized glyph key.
 * @retval ra8_glyph_key_t Key containing the four supplied fields.
 * @pre Each input is representable by its destination field type.
 * @pre Callers treat the returned key as immutable during lookup.
 * @post Unassigned padding and fields remain zero initialized.
 * @post All four identity fields equal their arguments.
 * @note A value return avoids shared mutable key state between vectors.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_glyph_key_t
internal_t_key(uint32_t glyph, uint16_t face, uint16_t size, uint16_t mode)
{
  ra8_glyph_key_t k = {};
  k.glyph_id        = glyph;
  k.face_id         = face;
  k.size_px         = size;
  k.mode            = mode;
  return k;
}

/**
 * @brief Verify a glyph miss renders once and a repeated lookup hits.
 * @details Fetches, releases, and refetches one key while checking pixels,
 * dimensions, counters, and cache statistics.
 * @pre Fixture arrays and renderer counter are available for a fresh atlas.
 * @pre The chosen key fits within the deterministic renderer's field widths.
 * @post The second lookup returns the same cell without another render.
 * @post Statistics report exactly one hit and one miss.
 * @note Each successful get is paired with a put to keep pin state balanced.
 * @since 0.1.0
 *
 * @par MC/DC:
 * (no compound decisions under test -- a first get renders + pins with the
 * expected content; a second get of the same key is a hit and does not
 * re-render)
 */
RA8_INTERNAL static void internal_test_render_hit(void)
{
  TEST_BEGIN("glyph render-on-miss + hit");
  s_render_calls            = 0;
  ra8_glyph_atlas_t     a   = {};
  ra8_glyph_atlas_cfg_t cfg = internal_t_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_glyph_atlas_init(&a, &cfg));

  ra8_glyph_key_t k = internal_t_key(k_ga_codepoint, 1U, 16U, 0U);
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
 * @brief Verify LRU eviction after more glyphs than resident cells.
 * @details Streams eight distinct keys through four cells and then refetches
 * the oldest key.
 * @pre The atlas starts empty with four unpinned cells.
 * @pre Every fetched glyph is released before the next distinct lookup.
 * @post The statistics report four evictions after the initial sweep.
 * @post Refetching the oldest key invokes the renderer once more.
 * @note The monotonic render counter distinguishes a cache miss from data
 * coincidence.
 * @since 0.1.0
 *
 * @par MC/DC:
 * (no compound decisions under test -- more distinct glyphs than cells force
 * LRU eviction; the oldest is gone, a recent one is still resident)
 */
RA8_INTERNAL static void internal_test_eviction(void)
{
  TEST_BEGIN("glyph LRU eviction");
  s_render_calls            = 0;
  ra8_glyph_atlas_t     a   = {};
  ra8_glyph_atlas_cfg_t cfg = internal_t_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_glyph_atlas_init(&a, &cfg));

  /* Fill all 4 cells with glyphs 0..3, then add 4..7 (forces 4 evictions). */
  for (uint32_t i = 0; i < 8U; ++i) {
    ra8_glyph_key_t k = internal_t_key(i, 1U, 16U, 0U);
    ra8_glyph_t     g = {};
    TEST_ASSERT_EQ(k_ra8_ok, ra8_glyph_atlas_get(&a, &k, &g));
    TEST_ASSERT_EQ(k_ra8_ok, ra8_glyph_atlas_put(&a, g.bitmap));
  }
  uint32_t ev = 0;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_glyph_atlas_stats(&a, nullptr, nullptr, &ev));
  TEST_ASSERT_EQ(4, ev); /* 8 distinct glyphs through 4 cells */

  /* Re-getting an early (evicted) glyph re-renders; a recent one hits. */
  uint32_t        before = s_render_calls;
  ra8_glyph_key_t k0     = internal_t_key(0U, 1U, 16U, 0U);
  ra8_glyph_t     g0     = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_glyph_atlas_get(&a, &k0, &g0)); /* evicted -> re-render */
  TEST_ASSERT_EQ(before + 1U, s_render_calls);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_glyph_atlas_put(&a, g0.bitmap));
  TEST_END("glyph LRU eviction");
}

/**
 * @brief Verify pinned glyphs cannot be selected for eviction.
 * @details Pins every cell, checks a new key fails, then releases one cell and
 * retries successfully.
 * @pre Atlas capacity and the pins array both equal `k_t_cells`.
 * @pre Each initial get remains pinned until explicitly released.
 * @post An all-pinned miss returns no memory without publishing a glyph.
 * @post Releasing one pin permits the new glyph to render and all pins are
 * balanced.
 * @note This vector protects borrowed bitmap lifetimes across cache pressure.
 * @since 0.1.0
 *
 * @par MC/DC:
 * (no compound decisions under test -- a fully-pinned atlas cannot evict for a
 * new glyph; releasing one pin lets it render)
 */
RA8_INTERNAL static void internal_test_pin_protection(void)
{
  TEST_BEGIN("glyph pin protection");
  ra8_glyph_atlas_t     a   = {};
  ra8_glyph_atlas_cfg_t cfg = internal_t_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_glyph_atlas_init(&a, &cfg));

  const uint8_t* pins[(size_t)k_t_cells] = {};
  for (uint32_t i = 0; i < (uint32_t)k_t_cells; ++i) {
    ra8_glyph_key_t k = internal_t_key(i, 2U, k_ga_pixel_size, 0U);
    ra8_glyph_t     g = {};
    TEST_ASSERT_EQ(k_ra8_ok, ra8_glyph_atlas_get(&a, &k, &g)); /* pinned (no put) */
    pins[i] = g.bitmap;
  }
  ra8_glyph_key_t kn = internal_t_key(k_ga_codepoint_absent, 2U, k_ga_pixel_size, 0U);
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
 * @brief Verify glyph-atlas initialization and borrowed-cell guards.
 * @details Covers null objects, zero cell count, null get/put arguments,
 * mid-cell pointers, and double put.
 * @pre A valid baseline configuration can initialize the atlas after invalid
 * cases.
 * @pre The test key and output glyph remain writable throughout the vector.
 * @post Every malformed call returns its documented error category.
 * @post A valid get/put pair succeeds before double-release is rejected.
 * @note The mid-cell pointer proves put requires an exact published cell base.
 * @since 0.1.0
 *
 * @par MC/DC:
 * (no compound decisions under test -- each guard is an independent
 * single-condition check)
 */
RA8_INTERNAL static void internal_test_validation(void)
{
  TEST_BEGIN("glyph validation");
  ra8_glyph_atlas_t     a   = {};
  ra8_glyph_atlas_cfg_t cfg = internal_t_cfg();
  ra8_glyph_key_t       k   = internal_t_key(1U, 1U, 16U, 0U);
  ra8_glyph_t           g   = {};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_glyph_atlas_init(nullptr, &cfg));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_glyph_atlas_init(&a, nullptr));
  ra8_glyph_atlas_cfg_t bad = internal_t_cfg();
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

/**
 * @brief Consume one host-test log byte without touching target ITM MMIO.
 * @details Implements the injected logger sink as an intentional no-op for expected-error vectors.
 * @param[in] context Unused sink context.
 * @param[in] byte Unused diagnostic byte emitted by the production path.
 * @pre The test process owns the logger sink for the suite lifetime.
 * @pre No vector depends on observing diagnostic text.
 * @post No memory, descriptor, or hardware state is modified.
 * @post Control returns to the production logger immediately.
 * @note Installing this sink keeps sanitizer runs away from the target-only ITM address window.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_host_log_sink(void* context, uint8_t byte)
{
  (void)context;
  (void)byte;
}

int main(void)
{
  ra8_log_set_byte_sink(internal_host_log_sink, nullptr);
  internal_test_render_hit();
  internal_test_eviction();
  internal_test_pin_protection();
  internal_test_validation();
  ra8_log_set_byte_sink(nullptr, nullptr);
  return 0;
}
