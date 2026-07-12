/**
 * @file test_ra8_vmem.c
 * @brief Unit tests for the ra8_mem unified page cache (Layer 2, #147).
 *
 * @details
 * Exercises miss-load + hit, loaded-page content, capacity-bounded eviction, the
 * pin/unpin contract (a fully-pinned cache cannot evict), validation guards, and
 * -- the headline property -- SLRU **scan resistance**: a re-referenced hot set
 * promoted into the protected segment survives a linear page-turn flood that a
 * plain LRU/CLOCK cache would have thrashed away.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_vmem.h"
#include "unity_minimal.h"

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
static int32_t          s_buckets[(size_t)k_t_buckets];

/** @brief Deterministic loader: stamps object id + page number into the frame. */
static ra8_err_t
t_loader(void* ctx, uint32_t object_id, uint64_t offset, uint8_t* frame, uint32_t frame_bytes)
{
  (void)ctx;
  const uint8_t page = (uint8_t)(offset / (uint64_t)k_t_frame_bytes);
  (void)memset(frame, 0, (size_t)frame_bytes);
  frame[0] = (uint8_t)object_id;
  frame[1] = page;
  return k_ra8_ok;
}

/** @brief Build a cache config over the static fixture arrays. */
static ra8_vmem_cfg_t t_cfg(void)
{
  ra8_vmem_cfg_t cfg = {};
  cfg.frame_mem      = s_frames;
  cfg.frame_bytes    = k_t_frame_bytes;
  cfg.frame_count    = k_t_frames;
  cfg.meta           = s_meta;
  cfg.buckets        = s_buckets;
  cfg.bucket_count   = k_t_buckets;
  cfg.loader         = t_loader;
  cfg.loader_ctx     = nullptr;
  return cfg;
}

/** @brief Get page @p page of object @p obj, returning the (pinned) frame. */
static void* t_get(ra8_vmem_t* vm, uint32_t obj, uint32_t page)
{
  void*           p = nullptr;
  const ra8_err_t e = ra8_vmem_get(vm, obj, (uint64_t)page * (uint64_t)k_t_frame_bytes, &p);
  TEST_ASSERT_EQ(k_ra8_ok, e);
  return p;
}

/**
 * @par MC/DC:
 * (no compound decisions under test -- a first get loads + pins, the loaded page
 * carries the expected stamp, and a second get of the same key is a hit)
 */
static void test_miss_hit_content(void)
{
  TEST_BEGIN("vmem miss/hit + content");
  ra8_vmem_t     vm  = {};
  ra8_vmem_cfg_t cfg = t_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vmem_init(&vm, &cfg));

  uint8_t* p = (uint8_t*)t_get(&vm, 3U, 5U); /* miss -> load      */
  TEST_ASSERT_EQ(3, p[0]);                   /* object id stamp   */
  TEST_ASSERT_EQ(5, p[1]);                   /* page number stamp */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vmem_put(&vm, p));

  uint8_t* q = (uint8_t*)t_get(&vm, 3U, 5U); /* hit        */
  TEST_ASSERT(q == p);                       /* same frame */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vmem_put(&vm, q));

  uint32_t hits = 0;
  uint32_t miss = 0;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vmem_stats(&vm, &hits, &miss, nullptr));
  TEST_ASSERT_EQ(1, hits);
  TEST_ASSERT_EQ(1, miss);
  TEST_END("vmem miss/hit + content");
}

/**
 * @par MC/DC:
 * (no compound decisions under test -- the SLRU protected segment retains a
 * re-referenced hot set across a probationary scan flood)
 */
static void test_scan_resistance(void)
{
  TEST_BEGIN("vmem SLRU scan resistance");
  ra8_vmem_t     vm  = {};
  ra8_vmem_cfg_t cfg = t_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vmem_init(&vm, &cfg));

  /* Promote a 2-page hot set into the protected segment (access each twice). */
  for (uint32_t pass = 0; pass < 2U; ++pass) {
    TEST_ASSERT_EQ(k_ra8_ok, ra8_vmem_put(&vm, t_get(&vm, 1U, 0U)));
    TEST_ASSERT_EQ(k_ra8_ok, ra8_vmem_put(&vm, t_get(&vm, 1U, 1U)));
  }
  /* Linear page-turn flood: 30 distinct probationary pages through 8 frames. */
  for (uint32_t pg = 100U; pg < 130U; ++pg) {
    TEST_ASSERT_EQ(k_ra8_ok, ra8_vmem_put(&vm, t_get(&vm, 1U, pg)));
  }
  /* The hot set must have SURVIVED the flood (protected segment). */
  uint32_t miss_before = 0;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vmem_stats(&vm, nullptr, &miss_before, nullptr));
  uint8_t* h0 = (uint8_t*)t_get(&vm, 1U, 0U);
  uint8_t* h1 = (uint8_t*)t_get(&vm, 1U, 1U);
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
 * @par MC/DC:
 * (no compound decisions under test -- a fully-pinned cache cannot evict for a
 * new miss; releasing one pin makes the miss succeed)
 */
static void test_pin_protection(void)
{
  TEST_BEGIN("vmem pin protection");
  ra8_vmem_t     vm  = {};
  ra8_vmem_cfg_t cfg = t_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vmem_init(&vm, &cfg));

  /* Pin every frame (get and do NOT put). */
  void* pins[(size_t)k_t_frames] = {};
  for (uint32_t i = 0; i < (uint32_t)k_t_frames; ++i) {
    pins[i] = t_get(&vm, 2U, i);
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
 * @par MC/DC:
 * (no compound decisions under test -- each guard is an independent
 * single-condition check)
 */
static void test_validation(void)
{
  TEST_BEGIN("vmem validation");
  ra8_vmem_t     vm  = {};
  ra8_vmem_cfg_t cfg = t_cfg();
  void*          p   = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_vmem_init(nullptr, &cfg));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_vmem_init(&vm, nullptr));
  ra8_vmem_cfg_t bad = t_cfg();
  bad.frame_count    = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_vmem_init(&vm, &bad));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_vmem_init(&vm, &cfg));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_vmem_get(nullptr, 0U, 0U, &p));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_vmem_get(&vm, 0U, 0U, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_vmem_put(&vm, nullptr));
  /* put of a non-frame pointer and of an unpinned frame */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_vmem_put(&vm, &s_frames[1])); /* mid-frame */
  void* fr = t_get(&vm, 9U, 9U);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vmem_put(&vm, fr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_vmem_put(&vm, fr)); /* already unpinned */
  TEST_END("vmem validation");
}

/**
 * @par MC/DC:
 * (no compound decisions under test -- prefetch is a straight-line get+put; the
 * warmed page is proven resident by a subsequent get counting as a hit, and the
 * null-vm guard is a single-condition check)
 */
static void test_prefetch_warms(void)
{
  TEST_BEGIN("vmem prefetch warms cache");
  ra8_vmem_t     vm  = {};
  ra8_vmem_cfg_t cfg = t_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vmem_init(&vm, &cfg));

  /* Warm object 7 page 2: a bounded get+put -- loads it (a miss) then unpins. */
  const uint64_t off = (uint64_t)2U * (uint64_t)k_t_frame_bytes;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vmem_prefetch(&vm, 7U, off));
  uint32_t hits = 0U;
  uint32_t miss = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vmem_stats(&vm, &hits, &miss, nullptr));
  TEST_ASSERT_EQ(0, hits); /* the prefetch itself is the loading miss */
  TEST_ASSERT_EQ(1, miss);

  /* The next real get of the warmed key is now a hit (already resident). */
  void* p = t_get(&vm, 7U, 2U);
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

int32_t main(void)
{
  test_miss_hit_content();
  test_scan_resistance();
  test_pin_protection();
  test_validation();
  test_prefetch_warms();
  (void)fprintf(stderr, "[OK  ] test_ra8_vmem.c\n");
  return 0;
}
