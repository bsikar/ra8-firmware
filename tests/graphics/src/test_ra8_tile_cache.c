/**
 * @file test_ra8_tile_cache.c
 * @brief Unit tests for the ra8_mem image-tile cache (Layer 3b, #147).
 *
 * @details
 * Mirrors the glyph-atlas tests for the second ::ra8_keycache facade:
 * decode-on- miss + hit (with decoded content + reported dimensions, including
 * a smaller edge tile), LRU eviction past capacity, the pin/unpin contract (a
 * fully-pinned cache cannot evict), a decoder failure leaving the victim cold,
 * and the validation guards.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_log.h"
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

/**
 * @brief Decode a deterministic tile payload and dimensions.
 * @details Clears the cell, stamps key fields, counts calls, and selects full
 * or edge geometry.
 * @param[in] ctx Unused decoder context.
 * @param[in] key Tile identity supplied by the cache miss path.
 * @param[out] cell Writable cache cell receiving the fixture bytes.
 * @param[in] cell_bytes Capacity of the supplied cell.
 * @param[out] out_w Destination receiving decoded width.
 * @param[out] out_h Destination receiving decoded height.
 * @return A repository error code describing decode success.
 * @retval k_ra8_ok The fixture payload and dimensions were published.
 * @pre Required pointers are non-null and `cell_bytes` is at least four.
 * @pre Key coordinates are representable by the stamped bytes in this fixture.
 * @post The decode counter increments and the cell begins with key identity.
 * @post Dimensions identify either the full tile or configured edge tile.
 * @note Complete cell clearing exposes stale-payload cache errors.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_t_decode(void*                 ctx,
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

/* The pointer parameters below cannot be const: this mock implements a
 * function-pointer interface (the DI seam under test), so its signature is
 * fixed by the typedef it is assigned to -- adding const changes the
 * function type and the assignment stops compiling. */
// NOLINTBEGIN(readability-non-const-parameter) -- interface contract fixes this writable pointer type.
/**
 * @brief Return a deterministic tile-decode failure without output mutation.
 * @details Implements the decoder ABI solely to exercise miss rollback and
 * error propagation.
 * @param[in] ctx Unused decoder context.
 * @param[in] key Unused tile key.
 * @param[out] cell Unmodified candidate cell.
 * @param[in] cell_bytes Unused candidate cell capacity.
 * @param[out] out_w Unmodified width destination.
 * @param[out] out_h Unmodified height destination.
 * @return The fixed timeout error for the failure vector.
 * @retval k_ra8_err_timeout Decoding was deliberately rejected.
 * @pre The callback is installed through a type-compatible decode slot.
 * @pre Callers discard candidate outputs after a non-ok return.
 * @post All supplied storage remains untouched.
 * @post The timeout code is returned verbatim.
 * @note Non-const pointer types are dictated by the injected callback ABI.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_t_decode_fail(void*                 ctx,
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

/**
 * @brief Build a tile-cache configuration over fixture storage.
 * @details Binds cells, metadata, keys, dimensions, buckets, and the healthy
 * decoder.
 * @return A complete configuration referencing file-scope arrays.
 * @retval ra8_tile_cache_cfg_t Configuration sized to the fixture capacities.
 * @pre All arrays match their declared counts and byte sizes.
 * @pre ::internal_t_decode remains callable for the cache lifetime.
 * @post Every required pointer and count is populated consistently.
 * @post Decoder context is explicitly null.
 * @note The returned value owns no storage.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_tile_cache_cfg_t internal_t_cfg(void)
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
  cfg.decode               = internal_t_decode;
  cfg.decode_ctx           = nullptr;
  return cfg;
}

/**
 * @brief Construct one tile-cache key from explicit coordinates.
 * @details Zero-initializes then assigns image, x, y, and zoom identity fields.
 * @param[in] image Image identifier.
 * @param[in] tx Tile-column coordinate.
 * @param[in] ty Tile-row coordinate.
 * @param[in] zoom Zoom-level discriminator.
 * @return A fully initialized tile key.
 * @retval ra8_tile_key_t Key containing the supplied identity fields.
 * @pre Inputs are representable by their destination field types.
 * @pre Callers keep the returned key immutable during lookup.
 * @post All four identity fields equal their arguments.
 * @post No shared fixture state is modified.
 * @note Value construction avoids key-lifetime coupling.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_tile_key_t
internal_t_key(uint32_t image, uint16_t tx, uint16_t ty, uint16_t zoom)
{
  ra8_tile_key_t k = {};
  k.image_id       = image;
  k.tile_x         = tx;
  k.tile_y         = ty;
  k.zoom           = zoom;
  return k;
}

/**
 * @brief Verify decode-on-miss, hit reuse, and edge dimensions.
 * @details Fetches one full tile twice and one edge tile while checking
 * payload, counters, and statistics.
 * @pre A fresh cache can bind all fixture arrays.
 * @pre The decode counter starts at zero.
 * @post The repeated key reuses its cell without another decode.
 * @post The edge key reports reduced dimensions and statistics show one hit,
 * two misses.
 * @note Every successful borrow is balanced by a put.
 * @since 0.1.0
 *
 * @par MC/DC:
 * (no compound decisions under test -- a first get decodes + pins with the
 * expected content + dimensions; a second get of the same key is a hit and does
 * not re-decode; an edge tile reports its smaller dimensions)
 */
RA8_INTERNAL static void internal_test_decode_hit(void)
{
  TEST_BEGIN("tile decode-on-miss + hit");
  s_decode_calls           = 0;
  ra8_tile_cache_t     tc  = {};
  ra8_tile_cache_cfg_t cfg = internal_t_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tile_cache_init(&tc, &cfg));

  ra8_tile_key_t k = internal_t_key(k_t_image_id, 1U, 2U, 0U);
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
  ra8_tile_key_t ke = internal_t_key(k_t_image_id, (uint16_t)k_t_edge_x, 0U, 0U);
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
 * @brief Verify LRU eviction when distinct tiles exceed capacity.
 * @details Streams six released keys through three cells and refetches the
 * oldest key.
 * @pre The cache starts empty with all cells unpinned.
 * @pre The decode counter uniquely identifies misses.
 * @post Three evictions are recorded after the initial sweep.
 * @post Refetching the oldest tile decodes once more.
 * @note This pins replacement ordering in addition to capacity accounting.
 * @since 0.1.0
 *
 * @par MC/DC:
 * (no compound decisions under test -- more distinct tiles than cells force LRU
 * eviction; the oldest is gone, a recent one is still resident)
 */
RA8_INTERNAL static void internal_test_eviction(void)
{
  TEST_BEGIN("tile LRU eviction");
  s_decode_calls           = 0;
  ra8_tile_cache_t     tc  = {};
  ra8_tile_cache_cfg_t cfg = internal_t_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tile_cache_init(&tc, &cfg));

  /* Fill all 3 cells with tiles 0..2, then add 3..5 (forces 3 evictions). */
  for (uint32_t i = 0; i < 6U; ++i) {
    ra8_tile_key_t k = internal_t_key(1U, (uint16_t)i, 0U, 0U);
    ra8_tile_t     t = {};
    TEST_ASSERT_EQ(k_ra8_ok, ra8_tile_cache_get(&tc, &k, &t));
    TEST_ASSERT_EQ(k_ra8_ok, ra8_tile_cache_put(&tc, t.pixels));
  }
  uint32_t ev = 0;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tile_cache_stats(&tc, nullptr, nullptr, &ev));
  TEST_ASSERT_EQ(3, ev); /* 6 distinct tiles through 3 cells */

  /* Re-getting an early (evicted) tile re-decodes; a recent one hits. */
  uint32_t       before = s_decode_calls;
  ra8_tile_key_t k0     = internal_t_key(1U, 0U, 0U, 0U);
  ra8_tile_t     t0     = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tile_cache_get(&tc, &k0, &t0)); /* evicted -> re-decode */
  TEST_ASSERT_EQ(before + 1U, s_decode_calls);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tile_cache_put(&tc, t0.pixels));
  TEST_END("tile LRU eviction");
}

/**
 * @brief Verify pinned tiles cannot be evicted.
 * @details Pins every cell, checks a new key fails, releases one cell, and
 * retries successfully.
 * @pre The pins array has one slot per resident cell.
 * @pre Initial gets remain borrowed until their explicit puts.
 * @post An all-pinned miss reports no memory without replacement.
 * @post One released cell permits the new decode and all pins are balanced.
 * @note The vector protects pixel-view lifetime under pressure.
 * @since 0.1.0
 *
 * @par MC/DC:
 * (no compound decisions under test -- a fully-pinned cache cannot evict for a
 * new tile; releasing one pin lets it decode)
 */
RA8_INTERNAL static void internal_test_pin_protection(void)
{
  TEST_BEGIN("tile pin protection");
  ra8_tile_cache_t     tc  = {};
  ra8_tile_cache_cfg_t cfg = internal_t_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tile_cache_init(&tc, &cfg));

  const uint8_t* pins[(size_t)k_t_cells] = {};
  for (uint32_t i = 0; i < (uint32_t)k_t_cells; ++i) {
    ra8_tile_key_t k = internal_t_key(2U, (uint16_t)i, 1U, 0U);
    ra8_tile_t     t = {};
    TEST_ASSERT_EQ(k_ra8_ok, ra8_tile_cache_get(&tc, &k, &t)); /* pinned (no put) */
    pins[i] = t.pixels;
  }
  ra8_tile_key_t kn = internal_t_key(2U, k_t_tile_id, 1U, 0U);
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
 * @brief Verify decoder failure leaves no resident tile.
 * @details Injects the timeout decoder, requests one key, and observes
 * propagated failure and miss accounting.
 * @pre The failure callback satisfies the production decoder ABI.
 * @pre Fixture storage can initialize a fresh cache.
 * @post The timeout code reaches the caller unchanged.
 * @post Miss count increments once without publishing a tile view.
 * @note Output identity is intentionally not inspected after failure.
 * @since 0.1.0
 *
 * @par MC/DC:
 * (no compound decisions under test -- a decode failure on a miss returns the
 * callback's error verbatim and leaves no resident entry)
 */
RA8_INTERNAL static void internal_test_decode_failure(void)
{
  TEST_BEGIN("tile decode failure leaves cell cold");
  ra8_tile_cache_t     tc  = {};
  ra8_tile_cache_cfg_t cfg = internal_t_cfg();
  cfg.decode               = internal_t_decode_fail;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tile_cache_init(&tc, &cfg));

  ra8_tile_key_t k = internal_t_key(1U, 1U, 1U, 1U);
  ra8_tile_t     t = {};
  TEST_ASSERT_EQ(k_ra8_err_timeout, ra8_tile_cache_get(&tc, &k, &t)); /* decode fails */

  uint32_t miss = 0;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tile_cache_stats(&tc, nullptr, &miss, nullptr));
  TEST_ASSERT_EQ(1, miss);
  TEST_END("tile decode failure leaves cell cold");
}

/**
 * @brief Verify tile-cache construction, get, put, and stats guards.
 * @details Covers nulls, missing decoder, zero cells, mid-cell release, double
 * release, and null statistics.
 * @pre A valid baseline configuration is available after malformed variants.
 * @pre The fixture key and output tile remain writable.
 * @post Invalid calls return their documented error categories.
 * @post One valid borrow/release succeeds before repeated release is rejected.
 * @note Mid-cell rejection binds ownership to exact published cell bases.
 * @since 0.1.0
 *
 * @par MC/DC:
 * (no compound decisions under test -- each guard is an independent
 * single-condition check)
 */
RA8_INTERNAL static void internal_test_validation(void)
{
  TEST_BEGIN("tile validation");
  ra8_tile_cache_t     tc  = {};
  ra8_tile_cache_cfg_t cfg = internal_t_cfg();
  ra8_tile_key_t       k   = internal_t_key(1U, 1U, 1U, 0U);
  ra8_tile_t           t   = {};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_tile_cache_init(nullptr, &cfg));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_tile_cache_init(&tc, nullptr));
  ra8_tile_cache_cfg_t bad = internal_t_cfg();
  bad.decode               = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_tile_cache_init(&tc, &bad));
  bad            = internal_t_cfg();
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

/**
 * @brief Build a pan-prefetch request over the fixture grid.
 * @details Supplies the viewport, direction, budget, image identity, grid
 * extent, and zoom.
 * @param[in] v Inclusive visible tile rectangle.
 * @param[in] dir Pan direction selecting the lead edge.
 * @param[in] max_tiles Maximum tiles the prefetch may warm.
 * @return A complete pan-prefetch request.
 * @retval ra8_tile_prefetch_req_t Request bound to the fixture image and grid.
 * @pre Rectangle coordinates describe a candidate within the fixture grid.
 * @pre `max_tiles` is representable by uint16_t.
 * @post Supplied view, direction, and budget are retained exactly.
 * @post Grid and image fields equal fixture constants.
 * @note Validation remains the responsibility of the production prefetch entry
 * point.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_tile_prefetch_req_t
internal_t_req(ra8_tile_rect_t v, ra8_tile_pan_dir_t dir, uint16_t max_tiles)
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

/**
 * @brief Execute one pan prefetch and return its warmed count.
 * @details Builds a request, requires a successful operation, and exposes the
 * resulting count to test vectors.
 * @param[in,out] tc Initialized tile cache to warm.
 * @param[in] v Inclusive visible tile rectangle.
 * @param[in] dir Direction selecting the lead edge.
 * @param[in] maxt Maximum warm budget.
 * @return Number of tiles warmed by the operation.
 * @retval uint16_t Production-reported warmed tile count.
 * @pre `tc` is initialized with the fixture decoder and storage.
 * @pre The request arguments satisfy production validation.
 * @post The prefetch call has returned `k_ra8_ok`.
 * @post Returned count does not exceed `maxt`.
 * @note Assertion failure terminates the current Unity vector.
 * @since 0.1.0
 */
RA8_INTERNAL static uint16_t
internal_t_pan(ra8_tile_cache_t* tc, ra8_tile_rect_t v, ra8_tile_pan_dir_t dir, uint16_t maxt)
{
  ra8_tile_prefetch_req_t req    = internal_t_req(v, dir, maxt);
  uint16_t                warmed = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tile_cache_prefetch_pan(tc, &req, &warmed));
  return warmed;
}

/**
 * @brief Verify single-tile prefetch warms and unpins its entry.
 * @details Prefetches one key, confirms a following hit, then fills capacity
 * and observes later eviction.
 * @pre The cache starts empty and the decode counter is reset.
 * @pre Filler keys are distinct from the prefetched key.
 * @post Immediate get hits without another decode.
 * @post Capacity pressure can evict the prefetched entry, proving it was
 * unpinned.
 * @note The final refetch counter differentiates eviction from data
 * coincidence.
 * @since 0.1.0
 *
 * @par MC/DC:
 * (no compound decisions under test -- a prefetch decodes on a miss then
 * unpins, so the tile is resident for a following get (hit, no re-decode) yet
 * is evicted when the cache overflows, proving the pin was released)
 */
RA8_INTERNAL static void internal_test_prefetch(void)
{
  TEST_BEGIN("tile prefetch warms resident + unpinned");
  s_decode_calls           = 0;
  ra8_tile_cache_t     tc  = {};
  ra8_tile_cache_cfg_t cfg = internal_t_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tile_cache_init(&tc, &cfg));

  ra8_tile_key_t k = internal_t_key(k_t_image_id, 1U, 2U, 0U);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tile_cache_prefetch(&tc, &k)); /* miss -> decode + unpin */
  TEST_ASSERT_EQ(1, s_decode_calls);

  ra8_tile_t t = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tile_cache_get(&tc, &k, &t)); /* resident -> hit */
  TEST_ASSERT_EQ(1, s_decode_calls);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tile_cache_put(&tc, t.pixels));

  /* Was unpinned: filling the 3-cell cache with distinct tiles evicts it. */
  for (uint16_t i = 0U; i < (uint16_t)k_t_cells; ++i) {
    ra8_tile_key_t kf = internal_t_key(k_t_image_id, (uint16_t)(k_t_distinct_tx + i), 0U, 0U);
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
 * @brief Verify right-pan prefetch warms exactly the lead column.
 * @details Warms two tiles ahead of a two-row viewport and confirms both become
 * resident hits.
 * @pre The cache starts empty with enough cells for both lead tiles.
 * @pre The viewport lies inside the four-by-four fixture grid.
 * @post Warmed and decode counts both equal two.
 * @post Both lead-tile gets hit and a null warmed sink is accepted.
 * @note This pins lead-edge ordering and optional output semantics.
 * @since 0.1.0
 *
 * @par MC/DC:
 * (no compound decisions under test -- a pan-right prefetch warms exactly the
 * lead column ahead of the viewport within its budget; the warmed tiles are
 * then resident so a following get is a hit with no re-decode)
 */
RA8_INTERNAL static void internal_test_prefetch_pan_warms(void)
{
  TEST_BEGIN("tile pan prefetch warms the lead column, resident");
  s_decode_calls           = 0;
  ra8_tile_cache_t     tc  = {};
  ra8_tile_cache_cfg_t cfg = internal_t_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tile_cache_init(&tc, &cfg));

  /* Visible col 0 rows 0..1; pan right -> warm col 1 rows 0..1 (2 tiles). */
  const ra8_tile_rect_t   v      = {.tx0 = 0U, .ty0 = 0U, .tx1 = 0U, .ty1 = 1U};
  ra8_tile_prefetch_req_t req    = internal_t_req(v, k_ra8_tile_pan_right, (uint16_t)k_t_pan_span);
  uint16_t                warmed = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tile_cache_prefetch_pan(&tc, &req, &warmed));
  TEST_ASSERT_EQ(2, warmed);
  TEST_ASSERT_EQ(2, s_decode_calls);

  /* Both lead tiles resident: gets are hits (decode count unchanged). */
  ra8_tile_key_t k10 = internal_t_key(k_t_image_id, 1U, 0U, 0U);
  ra8_tile_key_t k11 = internal_t_key(k_t_image_id, 1U, 1U, 0U);
  ra8_tile_t     t   = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tile_cache_get(&tc, &k10, &t));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tile_cache_put(&tc, t.pixels));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tile_cache_get(&tc, &k11, &t));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tile_cache_put(&tc, t.pixels));
  TEST_ASSERT_EQ(2, s_decode_calls);

  /* out_warmed is optional: a NULL sink is accepted (already-resident re-warm).
   */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tile_cache_prefetch_pan(&tc, &req, nullptr));
  TEST_END("tile pan prefetch warms the lead column, resident");
}

/**
 * @brief Verify every pan direction, grid edge, none direction, and budget cap.
 * @details Applies interior and boundary rectangles to right, left, down, up,
 * none, and one-tile budget cases.
 * @pre The initialized cache can accept repeated best-effort prefetches.
 * @pre All rectangles use the fixture grid coordinate system.
 * @post Interior directions each report two warmed tiles.
 * @post Edge and none cases report zero while a one-tile budget reports one.
 * @note Existing residency may change decode activity but not requested warmed
 * counts.
 * @since 0.1.0
 *
 * @par MC/DC:
 * (no compound decisions under test -- exercises every ::ra8_tile_pan_dir_t
 * arm: an interior viewport warms its 2-tile lead edge in each direction, a
 * viewport against the grid edge warms nothing, `none` warms nothing, and a
 * max_tiles=1 budget caps the warm to one tile)
 */
RA8_INTERNAL static void internal_test_prefetch_pan_dirs(void)
{
  TEST_BEGIN("tile pan prefetch: each direction, edges, none, budget");
  ra8_tile_cache_t     tc  = {};
  ra8_tile_cache_cfg_t cfg = internal_t_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tile_cache_init(&tc, &cfg));
  const ra8_tile_rect_t col = {.tx0 = 1U, .ty0 = 1U, .tx1 = 1U, .ty1 = 2U}; /* pan L/R */
  const ra8_tile_rect_t row = {.tx0 = 1U, .ty0 = 1U, .tx1 = 2U, .ty1 = 1U}; /* pan U/D */
  TEST_ASSERT_EQ(2, internal_t_pan(&tc, col, k_ra8_tile_pan_right, (uint16_t)k_t_pan_span));
  TEST_ASSERT_EQ(2, internal_t_pan(&tc, col, k_ra8_tile_pan_left, (uint16_t)k_t_pan_span));
  TEST_ASSERT_EQ(2, internal_t_pan(&tc, row, k_ra8_tile_pan_down, (uint16_t)k_t_pan_span));
  TEST_ASSERT_EQ(2, internal_t_pan(&tc, row, k_ra8_tile_pan_up, (uint16_t)k_t_pan_span));
  /* A viewport already against each grid edge warms nothing. */
  const ra8_tile_rect_t e_r = {.tx0 = 3U, .ty0 = 0U, .tx1 = 3U, .ty1 = 1U};
  const ra8_tile_rect_t e_l = {.tx0 = 0U, .ty0 = 0U, .tx1 = 0U, .ty1 = 1U};
  const ra8_tile_rect_t e_d = {.tx0 = 0U, .ty0 = 3U, .tx1 = 1U, .ty1 = 3U};
  const ra8_tile_rect_t e_u = {.tx0 = 0U, .ty0 = 0U, .tx1 = 1U, .ty1 = 0U};
  TEST_ASSERT_EQ(0, internal_t_pan(&tc, e_r, k_ra8_tile_pan_right, (uint16_t)k_t_pan_span));
  TEST_ASSERT_EQ(0, internal_t_pan(&tc, e_l, k_ra8_tile_pan_left, (uint16_t)k_t_pan_span));
  TEST_ASSERT_EQ(0, internal_t_pan(&tc, e_d, k_ra8_tile_pan_down, (uint16_t)k_t_pan_span));
  TEST_ASSERT_EQ(0, internal_t_pan(&tc, e_u, k_ra8_tile_pan_up, (uint16_t)k_t_pan_span));
  TEST_ASSERT_EQ(0, internal_t_pan(&tc, col, k_ra8_tile_pan_none, (uint16_t)k_t_pan_span));
  TEST_ASSERT_EQ(1, internal_t_pan(&tc, col, k_ra8_tile_pan_right, 1U)); /* budget caps to one */
  TEST_END("tile pan prefetch: each direction, edges, none, budget");
}

/**
 * @brief Verify pan prefetch remains successful when every cell is pinned.
 * @details Pins all cells, requests a lead edge, and then releases the retained
 * views.
 * @pre The pins array retains every borrowed cell pointer.
 * @pre The lead-edge request requires replacement to warm anything.
 * @post Prefetch reports success with zero warmed tiles.
 * @post All original pins are released successfully.
 * @note Read-ahead failure must not fail the foreground pan operation.
 * @since 0.1.0
 *
 * @par MC/DC:
 * (no compound decisions under test -- a fully-pinned cache cannot evict to
 * warm, so the pan prefetch warms nothing yet still returns k_ra8_ok:
 * read-ahead is best-effort and never fails the pan it serves)
 */
RA8_INTERNAL static void internal_test_prefetch_pan_best_effort(void)
{
  TEST_BEGIN("tile pan prefetch is best-effort on a full cache");
  ra8_tile_cache_t     tc  = {};
  ra8_tile_cache_cfg_t cfg = internal_t_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tile_cache_init(&tc, &cfg));

  const uint8_t* pins[(size_t)k_t_cells] = {};
  for (uint32_t i = 0U; i < (uint32_t)k_t_cells; ++i) {
    ra8_tile_key_t kp = internal_t_key(k_t_image_id, (uint16_t)i, 3U, 0U);
    ra8_tile_t     tp = {};
    TEST_ASSERT_EQ(k_ra8_ok, ra8_tile_cache_get(&tc, &kp, &tp)); /* pinned (no put) */
    pins[i] = tp.pixels;
  }
  const ra8_tile_rect_t v = {.tx0 = 0U, .ty0 = 0U, .tx1 = 0U, .ty1 = 1U};
  const uint16_t warmed   = internal_t_pan(&tc, v, k_ra8_tile_pan_right, (uint16_t)k_t_pan_span);
  TEST_ASSERT_EQ(0, warmed); /* all cells pinned: nothing warmed, still k_ra8_ok */
  for (uint32_t i = 0U; i < (uint32_t)k_t_cells; ++i) {
    TEST_ASSERT_EQ(k_ra8_ok, ra8_tile_cache_put(&tc, pins[i]));
  }
  TEST_END("tile pan prefetch is best-effort on a full cache");
}

/**
 * @brief Verify prefetch capacity reporting and request guards.
 * @details Checks null and uninitialized capacity, null entry arguments,
 * unordered rectangles, and off-grid bounds.
 * @pre A valid configuration can initialize the cache after pre-init probes.
 * @pre The baseline request lies inside the fixture grid.
 * @post Initialized capacity equals the configured cell count.
 * @post Every malformed request returns null-pointer or invalid-argument as
 * appropriate.
 * @note Each rectangle mutation isolates one validation boundary.
 * @since 0.1.0
 *
 * @par MC/DC:
 * (no compound decisions under test -- capacity reports 0 for NULL /
 * uninitialised and the cell count once bound; the prefetch entry points reject
 * NULL arguments and each unordered / off-grid rectangle bound is an
 * independent guard)
 */
RA8_INTERNAL static void internal_test_prefetch_guards(void)
{
  TEST_BEGIN("tile prefetch validation + capacity");
  ra8_tile_cache_t        tc  = {};
  ra8_tile_cache_cfg_t    cfg = internal_t_cfg();
  ra8_tile_key_t          k   = internal_t_key(k_t_image_id, 1U, 1U, 0U);
  const ra8_tile_rect_t   v   = {.tx0 = 0U, .ty0 = 0U, .tx1 = 0U, .ty1 = 1U};
  ra8_tile_prefetch_req_t req = internal_t_req(v, k_ra8_tile_pan_right, (uint16_t)k_t_pan_span);
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
  internal_test_decode_hit();
  internal_test_eviction();
  internal_test_pin_protection();
  internal_test_decode_failure();
  internal_test_validation();
  internal_test_prefetch();
  internal_test_prefetch_pan_warms();
  internal_test_prefetch_pan_dirs();
  internal_test_prefetch_pan_best_effort();
  internal_test_prefetch_guards();
  ra8_log_set_byte_sink(nullptr, nullptr);
  return 0;
}
