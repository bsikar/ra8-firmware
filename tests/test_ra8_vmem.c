/**
 * @file test_ra8_vmem.c
 * @brief Unit tests for the ra8_mem unified page cache (Layer 2, #147).
 *
 * @details
 * Exercises miss-load + hit, loaded-page content, capacity-bounded eviction,
 * the pin/unpin contract (a fully-pinned cache cannot evict), validation
 * guards, and
 * -- the headline property -- SLRU **scan resistance**: a re-referenced hot set
 * promoted into the protected segment survives a linear page-turn flood that a
 * plain LRU/CLOCK cache would have thrashed away.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_log.h"
#include "ra8_vmem.h"
#include "unity_minimal.h"

/**
 * @enum t_vmem_page_t
 * @brief Page and object ids addressed by the cache arms.
 *
 * @details
 * Ids are opaque to the cache -- it only maps (object, page) to a slot -- so
 * these are chosen distinct and non-adjacent, which makes an off-by-one in the
 * slot hash land on an id no arm uses.
 */
typedef enum : uint16_t {
  k_t_page_lo    = 100U, /**< First page of the eviction sweep. */
  k_t_page_hi    = 130U, /**< One past its last page: 30 pages, more than the
                            cache holds, so eviction must run.               */
  k_t_page_reuse = 5U,   /**< Page fetched twice: miss then hit.     */
  k_t_obj_probe  = 7U,   /**< Object id for the single-fetch arm.    */
  k_t_obj_free   = 9U,   /**< Object and page for the free-path arm. */
} t_vmem_page_t;

/**
 * @enum t_vmem_pct_t
 * @brief Protected-set percentages the config validator sees.
 */
typedef enum : uint8_t {
  k_t_pct_over   = 101U, /**< Past 100%: must be rejected. */
  k_t_pct_narrow = 25U,  /**< A small protected set.       */
  k_t_pct_wide   = 50U,  /**< Half the cache protected.    */
} t_vmem_pct_t;

/**
 * @enum t_vmem_const_t
 * @brief Fixture sizes.
 */
typedef enum : uint32_t {
  k_t_frame_bytes = 64U, /**< Bytes per frame.     */
  k_t_frames      = 8U,  /**< Frames in the cache. */
  k_t_buckets     = 16U, /**< Hash buckets.        */
} t_vmem_const_t;

static uint8_t          s_frames[(size_t)k_t_frames * (size_t)k_t_frame_bytes];
static ra8_vmem_frame_t s_meta[(size_t)k_t_frames];
static ra8_vmem_key_t   s_keys[(size_t)k_t_frames];
static int32_t          s_buckets[(size_t)k_t_buckets];

/**
 * @brief Load a deterministic object-and-page stamp into one frame.
 * @details Clears the complete frame and writes object identity plus the page
 * derived from byte offset.
 * @param[in] ctx Unused loader context.
 * @param[in] object_id Object identifier requested by vmem.
 * @param[in] offset Byte offset of the requested frame.
 * @param[out] frame Writable frame receiving deterministic content.
 * @param[in] frame_bytes Capacity of the supplied frame.
 * @return A repository error code describing load success.
 * @retval k_ra8_ok The frame was cleared and stamped.
 * @pre `frame` is non-null and `frame_bytes` is at least two.
 * @pre Fixture offsets are aligned to `k_t_frame_bytes`.
 * @post The first bytes identify the object and page number.
 * @post Remaining frame bytes are zero.
 * @note The page stamp exposes wrong-offset cache hits.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_t_loader(void*    ctx,
                                                uint32_t object_id,
                                                uint64_t offset,
                                                uint8_t* frame,
                                                uint32_t frame_bytes)
{
  (void)ctx;
  const uint8_t page = (uint8_t)(offset / (uint64_t)k_t_frame_bytes);
  (void)memset(frame, 0, (size_t)frame_bytes);
  frame[0] = (uint8_t)object_id;
  frame[1] = page;
  return k_ra8_ok;
}

/**
 * @brief Build a vmem configuration over all fixture arrays.
 * @details Binds frame bytes, metadata, keys, buckets, and the deterministic
 * loader.
 * @return A complete configuration referencing file-scope storage.
 * @retval ra8_vmem_cfg_t Configuration sized to the fixture capacities.
 * @pre Every backing array matches its enum-declared count.
 * @pre ::internal_t_loader remains callable for the cache lifetime.
 * @post Required pointers, counts, and sizes are populated consistently.
 * @post Loader context is explicitly null.
 * @note The returned value owns no storage.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_vmem_cfg_t internal_t_cfg(void)
{
  ra8_vmem_cfg_t cfg = {};
  cfg.frame_mem      = s_frames;
  cfg.frame_bytes    = k_t_frame_bytes;
  cfg.frame_count    = k_t_frames;
  cfg.meta           = s_meta;
  cfg.keys           = s_keys;
  cfg.buckets        = s_buckets;
  cfg.bucket_count   = k_t_buckets;
  cfg.loader         = internal_t_loader;
  cfg.loader_ctx     = nullptr;
  return cfg;
}

/**
 * @brief Borrow one object page and require the cache get to succeed.
 * @details Converts a page index to its byte offset, invokes vmem, and returns the pinned frame.
 * @param[in,out] vm Initialized cache providing the page.
 * @param[in] obj Object identifier in the injected loader namespace.
 * @param[in] page Page index multiplied by the fixture frame size.
 * @return Pointer to the borrowed resident frame.
 * @retval void* Non-null frame pointer published by `ra8_vmem_get`.
 * @pre `vm` is initialized with ::internal_t_cfg.
 * @pre The page-to-byte multiplication is representable as uint64_t.
 * @post The returned frame has one outstanding pin owned by the caller.
 * @post The helper has asserted that the get result was `k_ra8_ok`.
 * @note Callers must balance every returned frame with `ra8_vmem_put`.
 * @since 0.1.0
 */
RA8_INTERNAL static void* internal_t_get(ra8_vmem_t* vm, uint32_t obj, uint32_t page)
{
  void*           p = nullptr;
  const ra8_err_t e = ra8_vmem_get(vm, obj, (uint64_t)page * (uint64_t)k_t_frame_bytes, &p);
  TEST_ASSERT_EQ(k_ra8_ok, e);
  return p;
}

/**
 * @brief Verify one miss loads content and a repeated get hits.
 * @details Fetches the same object page twice while checking frame identity,
 * pointer reuse, and statistics.
 * @pre A fresh cache can bind all fixture storage.
 * @pre The selected page number fits the one-byte loader stamp.
 * @post Both borrows are released and the second returns the same frame
 * pointer.
 * @post Statistics report exactly one miss and one hit.
 * @note Payload stamps independently validate key-to-frame routing.
 * @since 0.1.0
 *
 * @par MC/DC:
 * (no compound decisions under test -- a first get loads + pins, the loaded
 * page carries the expected stamp, and a second get of the same key is a hit)
 */
RA8_INTERNAL static void internal_test_miss_hit_content(void)
{
  TEST_BEGIN("vmem miss/hit + content");
  ra8_vmem_t     vm  = {};
  ra8_vmem_cfg_t cfg = internal_t_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vmem_init(&vm, &cfg));

  uint8_t* p = (uint8_t*)internal_t_get(&vm, 3U, k_t_page_reuse); /* miss -> load      */
  TEST_ASSERT_EQ(3, p[0]);                                        /* object id stamp   */
  TEST_ASSERT_EQ(5, p[1]);                                        /* page number stamp */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vmem_put(&vm, p));

  uint8_t* q = (uint8_t*)internal_t_get(&vm, 3U, k_t_page_reuse); /* hit        */
  TEST_ASSERT(q == p);                                            /* same frame */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vmem_put(&vm, q));

  uint32_t hits = 0;
  uint32_t miss = 0;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vmem_stats(&vm, &hits, &miss, nullptr));
  TEST_ASSERT_EQ(1, hits);
  TEST_ASSERT_EQ(1, miss);
  TEST_END("vmem miss/hit + content");
}

/**
 * @brief Verify SLRU protects a rereferenced hot set from a scan flood.
 * @details Promotes two hot pages, streams thirty cold pages, and rereads the
 * hot set while observing miss count.
 * @pre The cache uses its default protected/probationary split.
 * @pre Every get in the warmup and flood is paired with a put.
 * @post Both hot pages retain their expected content after the flood.
 * @post Hot rereads add no misses.
 * @note The vector distinguishes SLRU from a scan-sensitive single LRU.
 * @since 0.1.0
 *
 * @par MC/DC:
 * (no compound decisions under test -- the SLRU protected segment retains a
 * re-referenced hot set across a probationary scan flood)
 */
RA8_INTERNAL static void internal_test_scan_resistance(void)
{
  TEST_BEGIN("vmem SLRU scan resistance");
  ra8_vmem_t     vm  = {};
  ra8_vmem_cfg_t cfg = internal_t_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vmem_init(&vm, &cfg));

  /* Promote a 2-page hot set into the protected segment (access each twice). */
  for (uint32_t pass = 0; pass < 2U; ++pass) {
    TEST_ASSERT_EQ(k_ra8_ok, ra8_vmem_put(&vm, internal_t_get(&vm, 1U, 0U)));
    TEST_ASSERT_EQ(k_ra8_ok, ra8_vmem_put(&vm, internal_t_get(&vm, 1U, 1U)));
  }
  /* Linear page-turn flood: 30 distinct probationary pages through 8 frames. */
  for (uint32_t pg = k_t_page_lo; pg < k_t_page_hi; ++pg) {
    TEST_ASSERT_EQ(k_ra8_ok, ra8_vmem_put(&vm, internal_t_get(&vm, 1U, pg)));
  }
  /* The hot set must have SURVIVED the flood (protected segment). */
  uint32_t miss_before = 0;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vmem_stats(&vm, nullptr, &miss_before, nullptr));
  uint8_t* h0 = (uint8_t*)internal_t_get(&vm, 1U, 0U);
  uint8_t* h1 = (uint8_t*)internal_t_get(&vm, 1U, 1U);
  TEST_ASSERT_EQ(0, h0[1]);
  TEST_ASSERT_EQ(1, h1[1]);
  uint32_t miss_after = 0;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vmem_stats(&vm, nullptr, &miss_after, nullptr));
  TEST_ASSERT_EQ(miss_before, miss_after); /* both were hits -> no new misses */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vmem_put(&vm, h0));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vmem_put(&vm, h1));
  TEST_END("vmem SLRU scan resistance");
}

/**
 * @brief Verify pinned vmem frames cannot be replaced.
 * @details Borrows every frame, checks a new page fails, releases one frame,
 * and retries successfully.
 * @pre The pins array has one slot per cache frame.
 * @pre Initial gets remain borrowed until their explicit puts.
 * @post An all-pinned miss reports no memory without publishing output.
 * @post Releasing one pin permits the new page and all borrows are balanced.
 * @note This protects caller view lifetime across page pressure.
 * @since 0.1.0
 *
 * @par MC/DC:
 * (no compound decisions under test -- a fully-pinned cache cannot evict for a
 * new miss; releasing one pin makes the miss succeed)
 */
RA8_INTERNAL static void internal_test_pin_protection(void)
{
  TEST_BEGIN("vmem pin protection");
  ra8_vmem_t     vm  = {};
  ra8_vmem_cfg_t cfg = internal_t_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vmem_init(&vm, &cfg));

  /* Pin every frame (get and do NOT put). */
  void* pins[(size_t)k_t_frames] = {};
  for (uint32_t i = 0; i < (uint32_t)k_t_frames; ++i) {
    pins[i] = internal_t_get(&vm, 2U, i);
  }
  /* A new distinct page cannot evict -> no_mem. */
  void* extra = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_no_mem,
                 ra8_vmem_get(&vm, 2U, (uint64_t)k_t_frames * (uint64_t)k_t_frame_bytes, &extra));
  /* Release one pin; now the miss succeeds. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vmem_put(&vm, pins[0]));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_vmem_get(&vm, 2U, (uint64_t)k_t_frames * (uint64_t)k_t_frame_bytes, &extra));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vmem_put(&vm, extra));
  for (uint32_t i = 1; i < (uint32_t)k_t_frames; ++i) {
    TEST_ASSERT_EQ(k_ra8_ok, ra8_vmem_put(&vm, pins[i]));
  }
  TEST_END("vmem pin protection");
}

/**
 * @brief Verify vmem initialization, get, and put validation guards.
 * @details Covers null objects, zero frames, null outputs, mid-frame pointers,
 * and repeated put.
 * @pre A valid baseline configuration is available after malformed variants.
 * @pre The fixture output pointer remains writable.
 * @post Invalid calls return their documented repository errors.
 * @post One valid borrow/release succeeds before double release is rejected.
 * @note Mid-frame rejection binds ownership to exact published frame bases.
 * @since 0.1.0
 *
 * @par MC/DC:
 * (no compound decisions under test -- each guard is an independent
 * single-condition check)
 */
RA8_INTERNAL static void internal_test_validation(void)
{
  TEST_BEGIN("vmem validation");
  ra8_vmem_t     vm  = {};
  ra8_vmem_cfg_t cfg = internal_t_cfg();
  void*          p   = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_vmem_init(nullptr, &cfg));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_vmem_init(&vm, nullptr));
  ra8_vmem_cfg_t bad = internal_t_cfg();
  bad.frame_count    = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_vmem_init(&vm, &bad));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_vmem_init(&vm, &cfg));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_vmem_get(nullptr, 0U, 0U, &p));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_vmem_get(&vm, 0U, 0U, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_vmem_put(&vm, nullptr));
  /* put of a non-frame pointer and of an unpinned frame */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_vmem_put(&vm, &s_frames[1])); /* mid-frame */
  void* fr = internal_t_get(&vm, k_t_obj_free, k_t_obj_free);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vmem_put(&vm, fr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_vmem_put(&vm, fr)); /* already unpinned */
  TEST_END("vmem validation");
}

/**
 * @brief Verify prefetch loads, leaves resident, and releases a page.
 * @details Prefetches one page, observes miss accounting, gets it as a hit, and
 * prefetches it again.
 * @pre A fresh cache can initialize with the deterministic loader.
 * @pre The requested offset is frame aligned.
 * @post The first prefetch creates one miss and the following get and prefetch
 * create hits.
 * @post The page remains unpinned after every prefetch and explicit put.
 * @note A null-cache call separately proves the public guard.
 * @since 0.1.0
 *
 * @par MC/DC:
 * (no compound decisions under test -- prefetch is a straight-line get+put; the
 * warmed page is proven resident by a subsequent get counting as a hit, and the
 * null-vm guard is a single-condition check)
 */
RA8_INTERNAL static void internal_test_prefetch_warms(void)
{
  TEST_BEGIN("vmem prefetch warms cache");
  ra8_vmem_t     vm  = {};
  ra8_vmem_cfg_t cfg = internal_t_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vmem_init(&vm, &cfg));

  /* Warm object 7 page 2: a bounded get+put -- loads it (a miss) then unpins.
   */
  const uint64_t off = (uint64_t)2U * (uint64_t)k_t_frame_bytes;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vmem_prefetch(&vm, 7U, off));
  uint32_t hits = 0U;
  uint32_t miss = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vmem_stats(&vm, &hits, &miss, nullptr));
  TEST_ASSERT_EQ(0, hits); /* the prefetch itself is the loading miss */
  TEST_ASSERT_EQ(1, miss);

  /* The next real get of the warmed key is now a hit (already resident). */
  void* p = internal_t_get(&vm, k_t_obj_probe, 2U);
  TEST_ASSERT_EQ(7, ((const uint8_t*)p)[0]); /* object id stamp   */
  TEST_ASSERT_EQ(2, ((const uint8_t*)p)[1]); /* page number stamp */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vmem_put(&vm, p));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vmem_stats(&vm, &hits, &miss, nullptr));
  TEST_ASSERT_EQ(1, hits); /* the post-prefetch get hit          */
  TEST_ASSERT_EQ(1, miss); /* still just the prefetch's one load */

  /* Re-prefetching a resident page is a hit and leaves it unpinned. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vmem_prefetch(&vm, 7U, off));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vmem_stats(&vm, &hits, &miss, nullptr));
  TEST_ASSERT_EQ(2, hits); /* second prefetch found it resident */

  /* Null guard. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_vmem_prefetch(nullptr, 0U, 0U));
  TEST_END("vmem prefetch warms cache");
}

/**
 * @enum t_split_const_t
 * @brief Hot-set + flood dimensions for the split-knob survival probe.
 */
typedef enum : uint32_t {
  k_t_split_obj     = 5U,   /**< Object id used by the split probe.             */
  k_t_split_hot     = 4U,   /**< Re-referenced hot pages promoted to protected. */
  k_t_split_passes  = 2U,   /**< Warmup passes (2nd promotes the hot set).      */
  k_t_split_flood0  = 200U, /**< First page of the one-shot scan flood.         */
  k_t_split_flood_n = 40U,  /**< Distinct pages in the scan flood.              */
} t_split_const_t;

/**
 * @brief Warm a hot set, flood with a one-shot scan, then re-read the hot set.
 *
 * @details Shared body for the split-knob test: promotes ::k_t_split_hot pages
 *          into the protected segment (each read twice), floods the cache with
 *          ::k_t_split_flood_n never-revisited pages, then re-reads the hot set
 *          and reports how many of those re-reads MISSED. A miss means the hot
 *          page was demoted (protected segment too small to hold it) and then
 *          evicted by the flood -- exactly the outcome the split knob controls.
 *
 * @param[in,out] vm Initialised cache (its split determines the survival
 * count).
 *
 * @return Number of hot pages that missed on re-read (0 == whole hot set
 * survived).
 * @retval 0 The protected segment held the entire hot set through the flood.
 *
 * @pre `vm` was populated by ::ra8_vmem_init.
 * @pre The cache has at least ::k_t_split_hot frames.
 * @post Every frame touched here is unpinned on return (get+put balanced).
 * @post No cache state beyond the SLRU order / counters is mutated.
 *
 * @note Not thread-safe.
 *
 * @par MC/DC:
 * (no compound decisions -- single-condition loop bounds only)
 *
 * @since 0.1.0
 */
RA8_INTERNAL static uint32_t internal_t_split_hot_set_survival(ra8_vmem_t* vm)
{
  for (uint32_t pass = 0U; pass < (uint32_t)k_t_split_passes; ++pass) {
    for (uint32_t h = 0U; h < (uint32_t)k_t_split_hot; ++h) {
      TEST_ASSERT_EQ(k_ra8_ok, ra8_vmem_put(vm, internal_t_get(vm, (uint32_t)k_t_split_obj, h)));
    }
  }
  for (uint32_t i = 0U; i < (uint32_t)k_t_split_flood_n; ++i) {
    const uint32_t pg = (uint32_t)k_t_split_flood0 + i;
    TEST_ASSERT_EQ(k_ra8_ok, ra8_vmem_put(vm, internal_t_get(vm, (uint32_t)k_t_split_obj, pg)));
  }
  uint32_t miss_before = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vmem_stats(vm, nullptr, &miss_before, nullptr));
  for (uint32_t h = 0U; h < (uint32_t)k_t_split_hot; ++h) {
    TEST_ASSERT_EQ(k_ra8_ok, ra8_vmem_put(vm, internal_t_get(vm, (uint32_t)k_t_split_obj, h)));
  }
  uint32_t miss_after = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vmem_stats(vm, nullptr, &miss_after, nullptr));
  return miss_after - miss_before;
}

/**
 * @test protected_split_knob
 * @brief The `cfg.protected_pct` SLRU split knob (#232): rejects an over-range
 *        split, sizes `protected_cap`, and changes how much of the hot set
 *        survives a page-turn flood.
 * @details Compares invalid, fifty-percent, and twenty-five-percent
 * configurations using the same hot-set flood probe.
 * @pre Fixture storage can be reinitialized for each split configuration.
 * @pre The hot-set size equals the wide split's protected capacity.
 * @post The over-range percentage is rejected and valid capacities equal four
 * and two.
 * @post The narrow split incurs more hot reread misses than the wide split.
 * @note All other vectors exercise the zero-percent default selector.
 * @since 0.1.0
 *
 * @par MC/DC:
 * Decision under test: `internal_validate_cfg_policy`'s `protected_pct > 100`
 * guard (1 condition) plus `internal_protected_cap`'s `protected_pct == 0`
 * default selector (1 condition, single-condition decisions -- no compound
 * `&&`/`||`).
 * - `protected_pct == 101` -> init rejects with k_ra8_err_invalid_arg (guard
 * true).
 * - `protected_pct == 50`  -> accepted; selector false (uses 50); cap == 4.
 * - `protected_pct == 25`  -> accepted; selector false (uses 25); cap == 2.
 * The `protected_pct == 0` default branch (selector true) is exercised by every
 * other test in this file via ::internal_t_cfg. Wide vs narrow splits give a
 * strictly different hot-set survival count, proving the knob influences
 * eviction.
 */
RA8_INTERNAL static void internal_test_protected_split_knob(void)
{
  TEST_BEGIN("vmem protected/probationary split knob");
  ra8_vmem_t vm = {};

  /* Reject an out-of-range split (guard: protected_pct > 100). */
  ra8_vmem_cfg_t bad = internal_t_cfg();
  bad.protected_pct  = k_t_pct_over;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_vmem_init(&vm, &bad));

  /* A 50% split over 8 frames sizes a 4-frame protected segment: it holds the
   * whole 4-page hot set, so every hot page survives the flood. */
  ra8_vmem_cfg_t wide = internal_t_cfg();
  wide.protected_pct  = k_t_pct_wide;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vmem_init(&vm, &wide));
  TEST_ASSERT_EQ(4U, vm.protected_cap);
  const uint32_t wide_reread = internal_t_split_hot_set_survival(&vm);
  TEST_ASSERT_EQ(0U, wide_reread);

  /* A 25% split sizes only a 2-frame protected segment: two hot pages are
   * demoted to probation during warmup and then thrashed out by the flood. */
  ra8_vmem_cfg_t narrow = internal_t_cfg();
  narrow.protected_pct  = k_t_pct_narrow;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vmem_init(&vm, &narrow));
  TEST_ASSERT_EQ(2U, vm.protected_cap);
  const uint32_t narrow_reread = internal_t_split_hot_set_survival(&vm);
  TEST_ASSERT(narrow_reread > wide_reread); /* the knob has teeth */

  TEST_END("vmem protected/probationary split knob");
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

int32_t main(void)
{
  ra8_log_set_byte_sink(internal_host_log_sink, nullptr);
  internal_test_miss_hit_content();
  internal_test_scan_resistance();
  internal_test_pin_protection();
  internal_test_validation();
  internal_test_prefetch_warms();
  internal_test_protected_split_knob();
  ra8_log_set_byte_sink(nullptr, nullptr);
  return 0;
}
