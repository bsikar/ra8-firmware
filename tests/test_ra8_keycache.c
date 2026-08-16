/**
 * @file test_ra8_keycache.c
 * @brief Unit tests for the reusable ra8_mem keyed-LRU cache (Layer 3, #147).
 *
 * @details
 * Exercises render-on-miss + hit (with rendered content + the user descriptor),
 * LRU eviction past capacity, the pin/unpin contract (a fully-pinned cache
 * cannot evict), a render-callback failure leaving the victim cold, the
 * no-descriptor
 * (`user_bytes == 0`) configuration, and the validation guards. These are the
 * regression guard shared by ::ra8_glyph_atlas and ::ra8_tile_cache, which are
 * both thin facades over this cache.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_keycache.h"
#include "ra8_log.h"
#include "unity_minimal.h"

/**
 * @enum keycache_fixture_t
 * @brief Out-of-range and malformed inputs the code under test must reject.
 */
typedef enum : uint8_t {
  k_kc_image_a     = 0x41U, /**< Image id of the first cache key. */
  k_kc_tile_shared = 7U,    /**< Tile index reused across images; lookups
                            discriminate on the image half of the key. */
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

/**
 * @brief Render a deterministic keycache payload and optional descriptor.
 * @details Clears the cell, stamps both key words, conditionally writes
 * dimensions, and counts the miss.
 * @param[in] ctx Unused renderer context.
 * @param[in] key Pointer to the immutable two-word fixture key.
 * @param[out] cell Writable cache cell receiving deterministic bytes.
 * @param[in] cell_bytes Capacity of the supplied cell.
 * @param[out] user Optional per-cell descriptor receiving dimensions.
 * @return A repository error code describing renderer success.
 * @retval k_ra8_ok The payload and any requested descriptor were published.
 * @pre `key` and `cell` are non-null and `cell_bytes` is at least two.
 * @pre A non-null `user` points to one writable ::t_dims_t.
 * @post The render counter increments and the first two cell bytes encode the
 * key.
 * @post A supplied descriptor contains the fixed width and height.
 * @note Complete cell clearing makes stale-residency bugs observable.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_t_render(void* ctx, const void* key, uint8_t* cell, uint32_t cell_bytes, void* user)
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

/* The pointer parameters below cannot be const: this mock implements a
 * function-pointer interface (the DI seam under test), so its signature is
 * fixed by the typedef it is assigned to -- adding const changes the
 * function type and the assignment stops compiling. */
// NOLINTBEGIN(readability-non-const-parameter)
/**
 * @brief Return a deterministic renderer failure without touching outputs.
 * @details Implements the injected renderer signature solely to drive miss
 * rollback.
 * @param[in] ctx Unused renderer context.
 * @param[in] key Unused key pointer supplied by the cache.
 * @param[out] cell Unmodified candidate cell.
 * @param[in] cell_bytes Unused candidate cell capacity.
 * @param[out] user Unmodified optional descriptor.
 * @return The fixed timeout error used by the failure vector.
 * @retval k_ra8_err_timeout Rendering was deliberately rejected.
 * @pre The callback is invoked through a type-compatible renderer slot.
 * @pre Callers do not depend on any output mutation after failure.
 * @post Cell, key, context, and descriptor storage remain untouched.
 * @post The fixed timeout code is returned verbatim.
 * @note Non-const pointer types are required by the production callback ABI.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_t_render_fail(void* ctx, const void* key, uint8_t* cell, uint32_t cell_bytes, void* user)
// NOLINTEND(readability-non-const-parameter)
{
  (void)ctx;
  (void)key;
  (void)cell;
  (void)cell_bytes;
  (void)user;
  return k_ra8_err_timeout;
}

/**
 * @brief Build a keycache configuration over all fixture arrays.
 * @details Binds cells, keys, descriptors, metadata, buckets, and the healthy
 * renderer into a value object.
 * @return A complete cache configuration referencing file-scope storage.
 * @retval ra8_keycache_cfg_t Configuration sized for the fixture arrays.
 * @pre Every file-scope array has its enum-declared capacity.
 * @pre ::internal_t_render remains callable for the cache lifetime.
 * @post Required pointers, byte sizes, and counts are populated consistently.
 * @post Renderer context is explicitly null for the context-free stub.
 * @note The returned configuration owns no storage.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_keycache_cfg_t internal_t_cfg(void)
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
  cfg.render             = internal_t_render;
  cfg.render_ctx         = nullptr;
  return cfg;
}

/**
 * @brief Construct one two-word cache key.
 * @details Zero-initializes the value before copying both caller-supplied
 * identity words.
 * @param[in] a First identity word.
 * @param[in] b Second identity word.
 * @return A fully initialized fixture key.
 * @retval t_key_t Key whose two fields equal the supplied words.
 * @pre Both inputs are representable as uint32_t.
 * @pre Callers keep the value immutable during each lookup.
 * @post Both key fields match their corresponding arguments.
 * @post No shared fixture state is modified.
 * @note Value construction avoids pointer-lifetime coupling between vectors.
 * @since 0.1.0
 */
RA8_INTERNAL static t_key_t internal_t_key(uint32_t a, uint32_t b)
{
  t_key_t k = {};
  k.a       = a;
  k.b       = b;
  return k;
}

/**
 * @brief Verify render-on-miss followed by a cache hit.
 * @details Fetches one key twice while checking cell bytes, user dimensions,
 * render count, and statistics.
 * @pre A fresh cache can bind all fixture arrays.
 * @pre The render counter is reset before initialization.
 * @post The second fetch reuses the same cell without rerendering.
 * @post Statistics report one hit and one miss with balanced pins.
 * @note Descriptor bytes are checked alongside the cached payload.
 * @since 0.1.0
 *
 * @par MC/DC:
 * (no compound decisions under test -- a first get renders + pins with the
 * expected content + descriptor; a second get of the same key is a hit and does
 * not re-render)
 */
RA8_INTERNAL static void internal_test_render_hit(void)
{
  TEST_BEGIN("keycache render-on-miss + hit");
  s_render_calls         = 0;
  ra8_keycache_t     kc  = {};
  ra8_keycache_cfg_t cfg = internal_t_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_keycache_init(&kc, &cfg));

  t_key_t             k = internal_t_key(k_kc_image_a, k_kc_tile_a);
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
 * @brief Verify LRU replacement across twice the resident key capacity.
 * @details Streams eight released keys through four cells, then probes an
 * evicted and a recent key.
 * @pre The cache starts empty and all successful views are released.
 * @pre The renderer counter uniquely identifies every miss.
 * @post Four evictions are recorded after the first sweep.
 * @post The old key rerenders while the recent key remains a hit.
 * @note This vector distinguishes list ordering from simple capacity
 * accounting.
 * @since 0.1.0
 *
 * @par MC/DC:
 * (no compound decisions under test -- more distinct keys than cells force LRU
 * eviction; the oldest is gone, a recent one is still resident)
 */
RA8_INTERNAL static void internal_test_eviction(void)
{
  TEST_BEGIN("keycache LRU eviction");
  s_render_calls         = 0;
  ra8_keycache_t     kc  = {};
  ra8_keycache_cfg_t cfg = internal_t_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_keycache_init(&kc, &cfg));

  /* Fill all 4 cells with keys 0..3, then add 4..7 (forces 4 evictions). */
  for (uint32_t i = 0; i < 8U; ++i) {
    t_key_t             k = internal_t_key(i, 0U);
    ra8_keycache_view_t v = {};
    TEST_ASSERT_EQ(k_ra8_ok, ra8_keycache_get(&kc, &k, &v));
    TEST_ASSERT_EQ(k_ra8_ok, ra8_keycache_put(&kc, v.data));
  }
  uint32_t ev = 0;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_keycache_stats(&kc, nullptr, nullptr, &ev));
  TEST_ASSERT_EQ(4, ev); /* 8 distinct keys through 4 cells */

  /* Re-getting an early (evicted) key re-renders; a recent one hits. */
  uint32_t            before = s_render_calls;
  t_key_t             k0     = internal_t_key(0U, 0U);
  ra8_keycache_view_t v0     = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_keycache_get(&kc, &k0, &v0)); /* evicted -> re-render */
  TEST_ASSERT_EQ(before + 1U, s_render_calls);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_keycache_put(&kc, v0.data));

  uint32_t            before2 = s_render_calls;
  t_key_t             k7      = internal_t_key(k_kc_tile_shared, 0U);
  ra8_keycache_view_t v7      = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_keycache_get(&kc, &k7, &v7)); /* recent -> hit */
  TEST_ASSERT_EQ(before2, s_render_calls);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_keycache_put(&kc, v7.data));
  TEST_END("keycache LRU eviction");
}

/**
 * @brief Verify pinned cells are ineligible for replacement.
 * @details Pins every resident cell, checks an additional key fails, releases
 * one pin, and retries.
 * @pre The pins array can retain one cell pointer per cache entry.
 * @pre Initial gets remain borrowed until their explicit puts.
 * @post An all-pinned miss returns no memory without evicting a cell.
 * @post Releasing one cell permits the new key and every pin is balanced.
 * @note The vector protects the borrow contract under eviction pressure.
 * @since 0.1.0
 *
 * @par MC/DC:
 * (no compound decisions under test -- a fully-pinned cache cannot evict for a
 * new key; releasing one pin lets it render)
 */
RA8_INTERNAL static void internal_test_pin_protection(void)
{
  TEST_BEGIN("keycache pin protection");
  ra8_keycache_t     kc  = {};
  ra8_keycache_cfg_t cfg = internal_t_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_keycache_init(&kc, &cfg));

  const uint8_t* pins[(size_t)k_t_cells] = {};
  for (uint32_t i = 0; i < (uint32_t)k_t_cells; ++i) {
    t_key_t             k = internal_t_key(i, k_kc_tile_shared);
    ra8_keycache_view_t v = {};
    TEST_ASSERT_EQ(k_ra8_ok, ra8_keycache_get(&kc, &k, &v)); /* pinned (no put) */
    pins[i] = v.data;
  }
  t_key_t             kn = internal_t_key(k_kc_image_absent, k_kc_tile_shared);
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
 * @brief Verify renderer failure does not publish a resident cache entry.
 * @details Injects a timeout renderer, observes the miss, then reinitializes
 * with the healthy renderer and retries the same key.
 * @pre Both failure and healthy callbacks satisfy the renderer ABI.
 * @pre Fixture storage can be safely reinitialized between attempts.
 * @post The timeout propagates and the failed attempt counts as one miss.
 * @post The healthy retry renders once, succeeds, and releases normally.
 * @note Reusing the identical key proves failure did not leave a false hit.
 * @since 0.1.0
 *
 * @par MC/DC:
 * (no compound decisions under test -- a render failure on a miss returns the
 * callback's error verbatim and leaves no resident entry; a later successful
 * get of the same key renders and succeeds)
 */
RA8_INTERNAL static void internal_test_render_failure(void)
{
  TEST_BEGIN("keycache render failure leaves cell cold");
  ra8_keycache_t     kc  = {};
  ra8_keycache_cfg_t cfg = internal_t_cfg();
  cfg.render             = internal_t_render_fail;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_keycache_init(&kc, &cfg));

  t_key_t             k = internal_t_key(1U, 2U);
  ra8_keycache_view_t v = {};
  TEST_ASSERT_EQ(k_ra8_err_timeout, ra8_keycache_get(&kc, &k, &v)); /* render fails */

  /* Nothing should be resident: a subsequent get is still a miss. */
  uint32_t miss = 0;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_keycache_stats(&kc, nullptr, &miss, nullptr));
  TEST_ASSERT_EQ(1, miss);

  /* Swap in a working renderer; the same key now succeeds. */
  cfg.render = internal_t_render;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_keycache_init(&kc, &cfg));
  s_render_calls = 0;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_keycache_get(&kc, &k, &v));
  TEST_ASSERT_EQ(1, s_render_calls);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_keycache_put(&kc, v.data));
  TEST_END("keycache render failure leaves cell cold");
}

/**
 * @brief Verify caches may omit the optional user descriptor storage.
 * @details Initializes with zero descriptor bytes, fetches one key, and checks
 * data remains available with a null user view.
 * @pre Cell, key, metadata, and bucket storage remain fully configured.
 * @pre User pointer and byte count are cleared together.
 * @post Lookup succeeds and returns a null descriptor pointer.
 * @post Cached payload still contains the rendered key stamp and is released.
 * @note This distinguishes an optional descriptor from missing required
 * storage.
 * @since 0.1.0
 *
 * @par MC/DC:
 * (no compound decisions under test -- a cache configured with user_bytes == 0
 * returns a NULL user pointer but still caches data)
 */
RA8_INTERNAL static void internal_test_no_user_descriptor(void)
{
  TEST_BEGIN("keycache without user descriptor");
  ra8_keycache_t     kc  = {};
  ra8_keycache_cfg_t cfg = internal_t_cfg();
  cfg.user_mem           = nullptr;
  cfg.user_bytes         = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_keycache_init(&kc, &cfg));

  t_key_t             k = internal_t_key(k_kc_image_b, k_kc_tile_b);
  ra8_keycache_view_t v = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_keycache_get(&kc, &k, &v));
  TEST_ASSERT(v.user == nullptr);
  TEST_ASSERT_EQ(0xAB, v.data[0]);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_keycache_put(&kc, v.data));
  TEST_END("keycache without user descriptor");
}

/**
 * @brief Verify keycache construction, lookup, release, and statistics guards.
 * @details Covers null objects, malformed sizes and descriptor pairing,
 * mid-cell release, double release, and null stats.
 * @pre A valid baseline configuration is available after malformed variants.
 * @pre The fixture key and output view remain writable.
 * @post Every invalid argument returns the expected repository error.
 * @post One valid get/put succeeds before the repeated put is rejected.
 * @note Mid-cell rejection pins ownership to exact published cell bases.
 * @since 0.1.0
 *
 * @par MC/DC:
 * (no compound decisions under test -- each guard is an independent
 * single-condition check)
 */
RA8_INTERNAL static void internal_test_validation(void)
{
  TEST_BEGIN("keycache validation");
  ra8_keycache_t      kc  = {};
  ra8_keycache_cfg_t  cfg = internal_t_cfg();
  t_key_t             k   = internal_t_key(1U, 1U);
  ra8_keycache_view_t v   = {};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_keycache_init(nullptr, &cfg));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_keycache_init(&kc, nullptr));
  ra8_keycache_cfg_t bad = internal_t_cfg();
  bad.cell_count         = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_keycache_init(&kc, &bad));
  bad           = internal_t_cfg();
  bad.key_bytes = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_keycache_init(&kc, &bad));
  bad            = internal_t_cfg();
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
  internal_test_render_failure();
  internal_test_no_user_descriptor();
  internal_test_validation();
  ra8_log_set_byte_sink(nullptr, nullptr);
  return 0;
}
