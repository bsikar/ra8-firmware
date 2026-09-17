/**
 * @file test_ra8_vmem_hugebook.c
 * @brief #147 huge-book invariant gate: a GB-class object served correctly
 *        through a tiny fixed page-cache budget (Layer 1 + Layer 2).
 *
 * @details
 * The whole premise of the #147 memory hierarchy is: "open *massive* EPUB/CBZ
 * files (GB-class) whose size far exceeds physical RAM ... the resident set
 * must be bounded by a fixed RAM budget we pick, independent of file size." The
 * other tests cover the page cache at toy scale; this file turns that design
 * claim into a CI-enforced invariant by driving the REAL Layer-1/Layer-2 stack
 * (::ra8_vsource + ::ra8_vmem SLRU) over a multi-gigabyte object through a
 * 32-frame (128 KiB) cache and asserting, under heavy eviction churn, all three
 * properties the design rests on:
 *
 *  1. **Bounded residency (no unbounded growth).** The object is ~7.6 GiB
 *     (~62,500x the resident budget) and the workload touches thousands of
 *     distinct pages, yet the count of valid frames never exceeds the fixed
 *     budget and saturates at exactly it. The 1-in-1-out law
 *     `evictions == misses - frame_count` holds exactly, so the resident set is
 *     bounded by construction regardless of file size.
 *  2. **Correctness == a non-cached reference.** Every page returned through
 * the cache is compared byte-for-byte against an independent re-implementation
 * of the object's content (and, for boundary pages, against the production
 *     ::ra8_vsource_loader read directly, bypassing the cache), including the
 *     zero-padded tail past the object's end. Wrong-page returns under churn,
 *     64-bit offset truncation, or stale frames would all be caught here.
 *  3. **Scan resistance at scale.** A hot front-matter set promoted into the
 * SLRU protected segment survives a one-shot linear flood of thousands of
 * unique pages, so each distinct page misses exactly once (`misses ==
 * distinct`) -- the hot set is never thrashed back out, which a plain LRU/CLOCK
 * cache would not guarantee.
 *
 * No genuinely huge buffer is allocated: the object's bytes are generated on
 * the fly by a pure content function, so a 7.6 GiB file is modelled in a few
 * hundred KiB of host RAM.
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
#include "ra8_vsource.h"
#include "unity_minimal.h"

/** @brief Compare two object representations through compatible pointer types. */
static bool internal_objects_equal(const void* lhs, const void* rhs, size_t len)
{
  const uint8_t* const lhs_bytes = lhs;
  const uint8_t* const rhs_bytes = rhs;
  bool                 equal     = true;
  for (size_t index = 0U; index < len; ++index) {
    if (lhs_bytes[index] != rhs_bytes[index]) {
      equal = false;
      break;
    }
  }
  return equal;
}

/**
 * @enum t_huge_dim_t
 * @brief Cache geometry + huge-object/workload dimensions (no magic numbers).
 * @details The resident budget is deliberately tiny relative to the object: a
 *          32-frame x 4096-byte cache (128 KiB) fronts a ~7.6 GiB object, so
 * the file is ~62,500x the budget and the workload's distinct working set is
 *          ~188x the budget -- the "working set >> RAM" regime #147 targets.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_h_frame_bytes = 4096U,    /**< Page-cache frame size (bytes).                    */
  k_h_frames      = 32U,      /**< Fixed resident frame budget (32 * 4 KiB).         */
  k_h_buckets     = 128U,     /**< Hash buckets for the cache.                       */
  k_h_hot         = 8U,       /**< Hot front-matter/TOC pages (re-read working set). */
  k_h_hot_passes  = 3U,       /**< Warmup passes that promote the hot set.           */
  k_h_scan        = 6000U,    /**< Distinct one-shot scan pages (the flood).         */
  k_h_stride      = 331U,     /**< Prime page stride: spread the scan across 7 GiB.  */
  k_h_obj_pages   = 2000000U, /**< Full pages in the object (* 4 KiB ~= 7.6 GiB).    */
  k_h_tail_bytes  = 100U,     /**< Partial-tail bytes in the object's final page.    */
  k_h_far_page    = 1500000U, /**< A page past 4 GiB (exercises 64-bit offsets).     */
  k_h_sample_each = 512U,     /**< Re-check residency every N scan accesses.         */
  k_h_min_oversub = 100U,     /**< Min distinct-pages-to-budget ratio asserted.      */
} t_huge_dim_t;

/** @brief Cache storage shared by setup and the independent residency probe. */
static struct {
  /** Page bytes. */
  uint8_t frames[(size_t)k_h_frames * (size_t)k_h_frame_bytes];
  /** Per-frame residency metadata. */
  ra8_vmem_frame_t meta[(size_t)k_h_frames];
  /** Cache lookup keys. */
  ra8_vmem_key_t keys[(size_t)k_h_frames];
  /** Hash-chain heads. */
  int32_t buckets[(size_t)k_h_buckets];
} s_cache;

/**
 * @enum t_huge_mix_t
 * @brief Content-generator mixing constants for ::internal_t_huge_byte (no
 * magic numbers).
 * @details A page-distinguishing avalanche so a wrong-page return is caught:
 * the high-offset fold makes bytes past the 4 GiB boundary differ from their
 *          low-offset aliases, and the Knuth multiplier spreads the result.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_h_fold_shift = 29U,         /**< Fold high offset bits down before the mix. */
  k_h_knuth_mul  = 2654435761U, /**< Knuth multiplicative hash constant.        */
  k_h_sel_shift  = 24U,         /**< Byte selector shift after the multiply.    */
} t_huge_mix_t;

/**
 * @brief Compute the exact byte length of the modelled huge object.
 * @details Multiplies full page count by frame size and appends the
 * partial-tail byte count in uint64_t.
 * @return Total logical object size in bytes.
 * @retval uint64_t Size of all full pages plus the configured tail.
 * @pre Page, frame, and tail constants fit their uint64_t calculation.
 * @pre The resulting object size exceeds the four-gigabyte boundary used by the
 * test.
 * @post The result is not rounded to a frame boundary.
 * @post No fixture storage or counters are modified.
 * @note Keeping the calculation central avoids inconsistent tail geometry.
 * @since 0.1.0
 */
RA8_INTERNAL static uint64_t internal_t_obj_size(void)
{
  return ((uint64_t)k_h_obj_pages * (uint64_t)k_h_frame_bytes) + (uint64_t)k_h_tail_bytes;
}

/**
 * @brief Generate the deterministic content byte at an absolute object offset.
 * @details Folds high address bits, applies the Knuth multiplier, and selects
 * one output byte.
 * @param[in] abs Absolute byte offset in the logical object.
 * @return Deterministic pseudo-content byte for that offset.
 * @retval uint8_t Mixed byte used by both source and independent reference
 * paths.
 * @pre `abs` is representable as uint64_t.
 * @pre Shift constants are smaller than the source width.
 * @post Equal offsets always produce equal bytes.
 * @post No mutable state is read or written.
 * @note High-bit folding makes truncation above four gigabytes observable.
 * @since 0.1.0
 */
RA8_INTERNAL static uint8_t internal_t_huge_byte(uint64_t abs)
{
  const uint64_t folded = abs ^ (abs >> (uint8_t)k_h_fold_shift);
  return (uint8_t)((folded * (uint64_t)k_h_knuth_mul) >> (uint8_t)k_h_sel_shift);
}

/**
 * @brief ra8_vsource read callback: serve generated bytes (no backing buffer).
 *
 * @details The storage seam for a paged object. The cache requests only
 * in-range bytes (the loader clamps to the object end), so every requested byte
 *          maps to a defined content byte; the tail past the object end is
 * zeroed by ::ra8_vsource_loader, not here.
 *
 * @param[in]  ctx    Unused backing context.
 * @param[in]  offset Absolute byte offset within the object.
 * @param[out] buf    Destination buffer (`len` writable bytes).
 * @param[in]  len    Bytes to generate.
 *
 * @return ra8_err_t Always k_ra8_ok.
 * @retval k_ra8_ok Buffer filled with the generated content.
 *
 * @pre `buf` is writable for `len` bytes.
 * @pre `offset + len` lies within the object size.
 * @post Each `buf[j]` equals the content byte at `offset + j`.
 * @post No state outside `buf` is modified.
 * @note Not thread-safe (irrelevant: the test is single-threaded).
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_t_read(void* ctx, uint64_t offset, uint8_t* buf, uint32_t len)
{
  (void)ctx;
  for (uint32_t j = 0U; j < len; ++j) {
    buf[j] = internal_t_huge_byte(offset + (uint64_t)j);
  }
  return k_ra8_ok;
}

/**
 * @brief Build the uncached reference image of the page at @p aligned_off.
 *
 * @details Independent re-implementation of what a correct cache must return:
 *          generated content for in-range bytes, zero for any tail past the
 *          object's end -- mirroring ::ra8_vsource_loader without using the
 * cache.
 *
 * @param[in]  aligned_off Frame-aligned byte offset of the page.
 * @param[out] out         Destination image (`k_h_frame_bytes` writable bytes).
 *
 * @return Nothing.
 *
 * @pre `out` is writable for `k_h_frame_bytes` bytes.
 * @pre `aligned_off` is a multiple of `k_h_frame_bytes`.
 * @post In-range bytes hold the generated content; tail bytes are zero.
 * @post No state outside `out` is modified.
 * @note Pure with respect to global state.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_t_ref_fill(uint64_t aligned_off, uint8_t* out)
{
  const uint64_t size = internal_t_obj_size();
  for (uint32_t k = 0U; k < (uint32_t)k_h_frame_bytes; ++k) {
    const uint64_t abs = aligned_off + (uint64_t)k;
    out[k]             = (abs < size) ? internal_t_huge_byte(abs) : 0U;
  }
}

/**
 * @brief Count currently valid frame metadata entries.
 * @details Scans the complete fixed metadata array and increments once per
 * valid flag.
 * @return Number of live resident cache frames.
 * @retval uint32_t Count in the inclusive range zero through `k_h_frames`.
 * @pre The metadata array contains initialized cache state or zero
 * initialization.
 * @pre Its declared extent equals `k_h_frames`.
 * @post The result never exceeds the configured frame budget.
 * @post Metadata remains unmodified.
 * @note This is an independent bounded-residency probe, not a cache statistic.
 * @since 0.1.0
 */
RA8_INTERNAL static uint32_t internal_t_count_valid(void)
{
  uint32_t live = 0U;
  for (uint32_t i = 0U; i < (uint32_t)k_h_frames; ++i) {
    if (s_cache.meta[i].valid != 0U) {
      ++live;
    }
  }
  return live;
}

/**
 * @brief Page in frame @p page through the cache and assert it matches the ref.
 *
 * @details Gets (and pins) the page, compares the whole frame to the
 * independent reference image, then releases the pin. Any wrong-page return,
 * stale frame, or offset-truncation bug fails the byte compare here.
 *
 * @param[in] vm   Initialised cache.
 * @param[in] obj  Object id to page in.
 * @param[in] page Page index within the object.
 *
 * @return Nothing (asserts on failure).
 *
 * @pre `vm` was populated by ::ra8_vmem_init.
 * @pre `page` indexes a valid page of the object.
 * @post The frame's pin count is unchanged on return (get then put).
 * @post The returned page equalled the uncached reference byte-for-byte.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_t_touch(ra8_vmem_t* vm, uint32_t obj, uint32_t page)
{
  const uint64_t  off = (uint64_t)page * (uint64_t)k_h_frame_bytes;
  void*           p   = nullptr;
  const ra8_err_t e   = ra8_vmem_get(vm, obj, off, &p);
  TEST_ASSERT_EQ(k_ra8_ok, e);
  uint8_t ref[(size_t)k_h_frame_bytes];
  internal_t_ref_fill(off, ref);
  TEST_ASSERT(internal_objects_equal(p, ref, (size_t)k_h_frame_bytes));
  TEST_ASSERT(internal_t_count_valid() <= (uint32_t)k_h_frames);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vmem_put(vm, p));
}

/**
 * @brief Bind one generated huge object to a caller-owned vsource and vmem
 * cache.
 * @details Initializes a one-object registry, registers the generated reader,
 * and binds fixed cache storage.
 * @param[out] vs Source registry to initialize.
 * @param[out] objs One-entry object table owned by the caller.
 * @param[out] vm Virtual-memory cache to initialize.
 * @return Registered object identifier used by later probes.
 * @retval uint32_t Identifier assigned by the one-entry source registry.
 * @pre All pointers are non-null and their storage outlives the test.
 * @pre File-scope cache arrays match the configured sizes.
 * @post Source and cache are initialized over the same logical object.
 * @post Returned id names the registered generated object.
 * @note Assertions terminate the vector if any setup stage fails.
 * @since 0.1.0
 */
RA8_INTERNAL static uint32_t
internal_t_setup(ra8_vsource_t* vs, ra8_vsource_obj_t* objs, ra8_vmem_t* vm)
{
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vsource_init(vs, objs, 1U));
  uint32_t obj = 0U;
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_vsource_add_paged(vs, internal_t_read, nullptr, 0U, internal_t_obj_size(), &obj));
  ra8_vmem_cfg_t cfg = {.frame_mem    = s_cache.frames,
                        .frame_bytes  = (uint32_t)k_h_frame_bytes,
                        .frame_count  = (uint32_t)k_h_frames,
                        .meta         = s_cache.meta,
                        .keys         = s_cache.keys,
                        .buckets      = s_cache.buckets,
                        .bucket_count = (uint32_t)k_h_buckets,
                        .loader       = ra8_vsource_loader,
                        .loader_ctx   = vs};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vmem_init(vm, &cfg));
  return obj;
}

/**
 * @brief Compare a cached page against the production loader read directly.
 *
 * @details The strongest correctness oracle for boundary pages: read the page
 *          through the un-cached ::ra8_vsource_loader into a scratch buffer,
 * then through the cache, and require both to equal the independent reference.
 *
 * @param[in] vm   Initialised cache.
 * @param[in] vs   The source registry (used as the loader context).
 * @param[in] obj  Object id.
 * @param[in] page Boundary page to cross-check.
 *
 * @return Nothing (asserts on failure).
 *
 * @pre `vm`/`vs` are initialised over the same object.
 * @pre `page` indexes a valid (possibly partial-tail) page.
 * @post The cached page equalled the uncached loader output and the reference.
 * @post No pin is leaked.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static void
internal_t_crosscheck(ra8_vmem_t* vm, ra8_vsource_t* vs, uint32_t obj, uint32_t page)
{
  const uint64_t off = (uint64_t)page * (uint64_t)k_h_frame_bytes;
  uint8_t        uncached[(size_t)k_h_frame_bytes];
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vsource_loader(vs, obj, off, uncached, (uint32_t)k_h_frame_bytes));
  uint8_t ref[(size_t)k_h_frame_bytes];
  internal_t_ref_fill(off, ref);
  TEST_ASSERT_EQ(0, memcmp(uncached, ref, (size_t)k_h_frame_bytes));
  void*           p = nullptr;
  const ra8_err_t e = ra8_vmem_get(vm, obj, off, &p);
  TEST_ASSERT_EQ(k_ra8_ok, e);
  TEST_ASSERT(internal_objects_equal(p, uncached, (size_t)k_h_frame_bytes));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vmem_put(vm, p));
}

/**
 * @brief Warm the hot front-matter set into the SLRU protected segment.
 * @details Touches every hot page for the configured number of promotion
 * passes.
 * @param[in,out] vm Initialized virtual-memory cache.
 * @param[in] obj Registered huge-object identifier.
 * @pre `vm` is bound to the generated source containing `obj`.
 * @pre The hot-set extent is smaller than the logical object page count.
 * @post Every hot page has been rereferenced enough for promotion.
 * @post Every temporary page borrow is released.
 * @note The helper mutates only cache residency, order, and statistics.
 * @since 0.1.0
 *
 * @par MC/DC:
 * (no compound decisions -- nested single-condition loop bounds only)
 */
RA8_INTERNAL static void internal_t_warm_hot(ra8_vmem_t* vm, uint32_t obj)
{
  for (uint32_t pass = 0U; pass < (uint32_t)k_h_hot_passes; ++pass) {
    for (uint32_t h = 0U; h < (uint32_t)k_h_hot; ++h) {
      internal_t_touch(vm, obj, h);
    }
  }
}

/**
 * @brief Flood the cache with a one-shot linear scan across the huge object.
 * @details Touches `k_h_scan` distinct strided pages and periodically asserts
 * the resident bound.
 * @param[in,out] vm Initialized virtual-memory cache.
 * @param[in] obj Registered huge-object identifier.
 * @pre The strided page sequence remains within the generated object.
 * @pre `vm` uses the fixed file-scope frame budget.
 * @post Every scan page has been fetched and released exactly once.
 * @post Sampled valid-frame counts never exceed the configured budget.
 * @note The scan deliberately avoids revisiting pages so SLRU protection is
 * stressed.
 * @since 0.1.0
 *
 * @par MC/DC:
 * (no compound decisions -- single-condition loop bound + a lone modulo check
 * guarding a periodic residency-bound assertion)
 */
RA8_INTERNAL static void internal_t_scan_flood(ra8_vmem_t* vm, uint32_t obj)
{
  for (uint32_t i = 0U; i < (uint32_t)k_h_scan; ++i) {
    const uint32_t page = (uint32_t)k_h_hot + (i * (uint32_t)k_h_stride);
    internal_t_touch(vm, obj, page);
    if ((i % (uint32_t)k_h_sample_each) == 0U) {
      TEST_ASSERT(internal_t_count_valid() <= (uint32_t)k_h_frames);
    }
  }
}

/**
 * @test huge_book_residency_and_correctness
 * @brief Verify bounded residency and exact bytes for a multi-gigabyte logical
 * object.
 * @details Cross-checks far and tail pages, promotes a hot set, floods cold
 * pages, and validates cache laws and counters.
 * @pre Caller-owned registry and cache fixture storage is available.
 * @pre The generated logical object crosses UINT32_MAX and includes a partial
 * tail.
 * @post Every touched page matches both independent and production-source
 * references.
 * @post Residency equals but never exceeds budget and eviction counters obey
 * one-in-one-out.
 * @note No multi-gigabyte buffer exists; bytes are generated by absolute
 * offset.
 * @since 0.1.0
 *
 * @par MC/DC:
 * (no compound decisions under test -- the gate is a set of independent
 * single-condition equalities/inequalities over the cache counters and the
 * residency probe; correctness is a byte compare against an uncached reference)
 */
RA8_INTERNAL static void internal_test_huge_book_residency_and_correctness(void)
{
  TEST_BEGIN("vmem huge-book bounded residency + correctness");
  ra8_vsource_t     vs      = {};
  ra8_vsource_obj_t objs[1] = {};
  ra8_vmem_t        vm      = {};
  const uint32_t    obj     = internal_t_setup(&vs, objs, &vm);

  /* Boundary pages first (cold): 64-bit offset past 4 GiB + the partial tail.
   */
  TEST_ASSERT(((uint64_t)k_h_far_page * (uint64_t)k_h_frame_bytes) > (uint64_t)UINT32_MAX);
  internal_t_crosscheck(&vm, &vs, obj, (uint32_t)k_h_far_page);  /* > 4 GiB offset        */
  internal_t_crosscheck(&vm, &vs, obj, (uint32_t)k_h_obj_pages); /* zero-padded tail page */

  /* Promote the hot set, then flood with a unique-page scan many x the budget.
   */
  internal_t_warm_hot(&vm, obj);
  internal_t_scan_flood(&vm, obj);

  /* The hot set must have SURVIVED the flood: re-reads are hits (no new miss).
   */
  uint32_t miss_before = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vmem_stats(&vm, nullptr, &miss_before, nullptr));
  for (uint32_t h = 0U; h < (uint32_t)k_h_hot; ++h) {
    internal_t_touch(&vm, obj, h);
  }
  uint32_t hits = 0U;
  uint32_t miss = 0U;
  uint32_t evic = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vmem_stats(&vm, &hits, &miss, &evic));
  TEST_ASSERT_EQ(miss_before, miss); /* hot re-reads added zero misses */

  /* Bounded residency: saturates at -- and never exceeds -- the fixed budget.
   */
  TEST_ASSERT_EQ(k_h_frames, internal_t_count_valid());
  /* 1-in-1-out law: every post-fill miss evicts exactly one (no growth). */
  TEST_ASSERT_EQ(miss - (uint32_t)k_h_frames, evic);
  /* Scan resistance: each distinct page (boundary + hot + scan) missed once. */
  const uint32_t distinct = 2U + (uint32_t)k_h_hot + (uint32_t)k_h_scan;
  TEST_ASSERT_EQ(distinct, miss);
  /* The working set genuinely dwarfs the budget (>= 100x). */
  TEST_ASSERT(distinct > ((uint32_t)k_h_frames * (uint32_t)k_h_min_oversub));

  TEST_END("vmem huge-book bounded residency + correctness");
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
  internal_test_huge_book_residency_and_correctness();
  ra8_log_set_byte_sink(nullptr, nullptr);
  return 0;
}
