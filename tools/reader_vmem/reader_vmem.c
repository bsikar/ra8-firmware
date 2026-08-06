/**
 * @file reader_vmem.c
 * @brief #147/#162 reader workload driver: page a modelled book through the real
 *        ra8_vmem page cache + ra8_vsource registry, emit the captured vm_get trace
 *        for tools/cache_bench, and report the firmware cache's own hit rate.
 *
 * @details
 * This is a HOST tool (not firmware): it confirms the #147 SLRU decision on a
 * realistic reader workload using the ACTUAL Layer-1/Layer-2 implementation
 * rather than a re-modelled policy. It registers a modelled book (a header/TOC
 * hot region followed by variable-size chapters) as an ::ra8_vsource paged object
 * over an in-memory backing, drives ::ra8_vmem with a reader navigation session
 * (linear page-turns with a hot page-furniture region + back-glances, TOC-driven
 * jumps, and a scan-resistance phase of hot-set re-reads interleaved with
 * one-shot linear floods), and for every `ra8_vmem_get` writes one `<object>
 * <page>` line to the trace file. Feed that file to `cache_bench reader=<file>`
 * to replay it across every candidate policy and confirm SLRU still wins; the
 * firmware cache's own hit/miss counters (printed to stderr) cross-check that
 * `ra8_vmem`'s real SLRU tracks the benched policy.
 *
 * The navigation pattern -- not the cold load -- is what makes the cache
 * interesting: a pure sequential load is all-miss for every policy, whereas a
 * reader re-references the TOC and a working set while flooding past one-shot
 * pages, which is exactly where 2Q/SLRU beats LRU/CLOCK.
 *
 *
 * [Ring 7 / Tooling] {World: NS}
 *
 * @since 0.1.0
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_log.h"
#include "ra8_vmem.h"
#include "ra8_vsource.h"

/** @brief Book / workload model dimensions (no magic numbers in the driver). */
typedef enum : uint32_t {
  k_rv_frame_bytes    = 4096U, /**< Page-cache frame size (bytes).                 */
  k_rv_object_book    = 1U,    /**< Trace object id of the book (matches bench).   */
  k_rv_header_frames  = 4U,    /**< Hot header/TOC region (consulted every turn).  */
  k_rv_chapters       = 24U,   /**< Chapters in the modelled book.                 */
  k_rv_chap_min_fr    = 8U,    /**< Smallest chapter, in frames.                   */
  k_rv_chap_span_fr   = 160U,  /**< Chapter-size spread above the minimum.         */
  k_rv_backglance_pct = 12U,   /**< % of content pages that re-read the prior one. */
  k_rv_toc_rounds     = 600U,  /**< TOC-jump rounds in the navigation phase.       */
  k_rv_toc_read_fr    = 6U,    /**< Frames read after each TOC jump.               */
  k_rv_sr_rounds      = 40U,   /**< Scan-resistance outer rounds.                  */
  k_rv_sr_hot_pass    = 3U,    /**< Hot-set re-read passes between floods.         */
  k_rv_sr_hot_fr      = 200U,  /**< Hot working-set size, in frames.               */
  k_rv_sr_scan_fr     = 1200U, /**< One-shot scan length per round, in frames.     */
  k_rv_pct_base       = 100U,  /**< Percentage base.                               */
} rv_dim_t;

/** @brief ra8_vmem sizing for the driver's own cache run. */
typedef enum : uint32_t {
  k_rv_def_budget = 256U,  /**< Default frame budget (matches cache_bench mid). */
  k_rv_buckets    = 1024U, /**< Hash buckets for the cache run.                 */
  k_rv_max_objs   = 2U,    /**< Source-registry slots.                          */
} rv_cache_dim_t;

/** @brief Deterministic fill / RNG / parse constants (no bare literals). */
typedef enum : uint32_t {
  k_rv_xs_shift_a   = 13U,         /**< xorshift64 shift 1.                   */
  k_rv_xs_shift_b   = 7U,          /**< xorshift64 shift 2.                   */
  k_rv_xs_shift_c   = 17U,         /**< xorshift64 shift 3.                   */
  k_rv_fill_mul     = 2654435761U, /**< Knuth multiplier for the book fill.   */
  k_rv_fill_shift   = 24U,         /**< Book-fill byte-selector shift.        */
  k_rv_decimal_base = 10U,         /**< strtoul base for the budget argument. */
} rv_const_t;

/** @brief 64-bit RNG seed (an enum : uint64_t holds the wide literal). */
typedef enum : uint64_t {
  k_rv_rng_seed = 0x9E3779B97F4A7C15ULL, /**< Fixed xorshift64 seed. */
} rv_seed_t;

/** @brief Per-chapter frame extent in the modelled book. */
typedef struct {
  uint32_t first; /**< First frame of the chapter. */
  uint32_t count; /**< Frame count of the chapter. */
} rv_chapter_t;

/** @brief Driver state shared across the navigation phases. */
typedef struct {
  rv_chapter_t chapters[k_rv_chapters]; /**< Chapter extents.               */
  uint32_t     total_frames;            /**< Frames in the whole book.      */
  uint64_t     rng;                     /**< Deterministic xorshift state.  */
  ra8_vmem_t*  vm;                      /**< The cache being driven.        */
  uint32_t     object_id;               /**< Registered book object id.     */
  FILE*        trace;                   /**< Trace output (`<obj> <page>`). */
  uint64_t     accesses;                /**< Accesses emitted so far.       */
} rv_driver_t;

/** @brief Log backend stub so ra8_check's RA8_CHECK_NULL_PTR links host-side. */
void internal_ra8_log_error(const char* tag, const char* message)
{
  (void)fprintf(stderr, "[ra8_log] %s: %s\n", tag, message);
}

/** @brief Valued log backend stub (unused here, present for the linker). */
void internal_ra8_log_error_val(const char* tag, const char* message, uint32_t value)
{
  (void)fprintf(stderr, "[ra8_log] %s: %s =%u\n", tag, message, value);
}

/**
 * @brief Advance a fixed-seed xorshift64 generator and return the new state.
 *
 * @details
 * The xorshift64 recurrence (shifts 13/7/17) over @p s, updated in place.
 * Deterministic across runs and platforms so the modelled reader session -- and
 * therefore the captured trace and the benched hit rate -- reproduce exactly.
 *
 * @param[in,out] s The 64-bit generator state; replaced with the next state.
 *
 * @return The next 64-bit state value.
 * @retval 0 Never, for a non-zero seed: xorshift64 cannot reach 0 from a
 *           non-zero state, and ::k_rv_rng_seed is non-zero.
 *
 * @pre @p s is non-NULL.
 * @pre @p s was seeded non-zero (::k_rv_rng_seed).
 * @post @p s holds the advanced state.
 * @post The return value equals the new @p s.
 *
 * @note Not thread-safe; each caller owns its own state word.
 * @since 0.1.0
 */
static uint64_t rv_rng(uint64_t* s)
{
  uint64_t x = *s;
  x ^= x << (uint32_t)k_rv_xs_shift_a;
  x ^= x >> (uint32_t)k_rv_xs_shift_b;
  x ^= x << (uint32_t)k_rv_xs_shift_c;
  *s = x;
  return x;
}

/**
 * @brief Draw a uniform pseudo-random integer in [0, @p span).
 *
 * @details
 * Advances ::rv_rng once and reduces modulo @p span. Used to size chapters and
 * to roll the back-glance and TOC-jump choices; the small modulo bias does not
 * affect which cache policy wins.
 *
 * @param[in,out] s    Generator state, advanced by one step when @p span > 0.
 * @param[in]     span Exclusive upper bound of the range.
 *
 * @return A value in [0, @p span).
 * @retval 0 @p span was 0 (an empty range is reported as 0).
 *
 * @pre @p s is non-NULL and seeded.
 * @pre @p span fits the intended selection range.
 * @post @p s is advanced by exactly one step when @p span is non-zero.
 * @post The result is strictly less than @p span (or 0 when @p span is 0).
 *
 * @note Not thread-safe; shares @p s with the caller.
 * @since 0.1.0
 */
static uint32_t rv_below(uint64_t* s, uint32_t span)
{
  return (span == 0U) ? 0U : (uint32_t)(rv_rng(s) % (uint64_t)span);
}

/** @brief The in-memory book backing (filled deterministically). */
static uint8_t* s_book_backing;
/** @brief Size of ::s_book_backing in bytes. */
static uint64_t s_book_bytes;

/**
 * @brief ra8_vsource read callback: serve bytes from the in-memory book.
 *
 * @details
 * Copies @p len bytes starting at @p offset out of the deterministically filled
 * ::s_book_backing buffer. ra8_vmem calls this through ra8_vsource on a page
 * miss to load a frame, so the driver exercises the real Layer-1/Layer-2 load
 * path rather than a re-modelled one. The context is unused; one global book
 * backs the single registered object.
 *
 * @param[in]  ctx    Unused source context (the global backing is used).
 * @param[in]  offset Byte offset into the book to read from.
 * @param[out] buf    Destination for @p len bytes.
 * @param[in]  len    Number of bytes to read.
 *
 * @return Source read status.
 * @retval k_ra8_ok               The bytes were served.
 * @retval k_ra8_err_out_of_range @p offset + @p len runs past the book.
 *
 * @pre ::s_book_backing is allocated to ::s_book_bytes.
 * @pre @p buf has room for @p len bytes.
 * @post On success @p buf holds the requested bytes.
 * @post The backing buffer is left unmodified.
 *
 * @note Not thread-safe; the tool is single-threaded.
 * @since 0.1.0
 */
static ra8_err_t rv_read(void* ctx, uint64_t offset, uint8_t* buf, uint32_t len)
{
  (void)ctx;
  if (offset + (uint64_t)len > s_book_bytes) {
    return k_ra8_err_out_of_range;
  }
  (void)memcpy(buf, &s_book_backing[offset], (size_t)len);
  return k_ra8_ok;
}

/**
 * @brief Lay out chapters of varied size after the hot header region.
 *
 * @details
 * Starting past the ::k_rv_header_frames hot region, assigns each of
 * ::k_rv_chapters chapters a random extent in
 * [::k_rv_chap_min_fr, ::k_rv_chap_min_fr + ::k_rv_chap_span_fr) frames and
 * records the running total in @p d->total_frames. The varied sizes give the
 * TOC-jump phase realistic targets.
 *
 * @param[in,out] d Driver state; the chapter table and total_frames are filled.
 *
 * @pre @p d is non-NULL with its RNG seeded.
 * @pre @p d->chapters has room for ::k_rv_chapters entries.
 * @post @p d->chapters holds contiguous, non-overlapping extents.
 * @post @p d->total_frames is the frame count of the whole book.
 *
 * @note Not thread-safe; the tool is single-threaded.
 * @since 0.1.0
 */
static void rv_layout_book(rv_driver_t* d)
{
  uint32_t cur = (uint32_t)k_rv_header_frames;
  for (uint32_t c = 0U; c < (uint32_t)k_rv_chapters; ++c) {
    const uint32_t count =
      (uint32_t)k_rv_chap_min_fr + rv_below(&d->rng, (uint32_t)k_rv_chap_span_fr);
    d->chapters[c].first = cur;
    d->chapters[c].count = count;
    cur += count;
  }
  d->total_frames = cur;
}

/**
 * @brief Issue one page access: drive ra8_vmem and emit the trace line.
 *
 * @details
 * Converts @p frame to a byte offset, calls ra8_vmem_get / ra8_vmem_put on the
 * registered book object (a get failure is reported but does not abort), then
 * appends one "<object> <frame>" line to the trace so tools/cache_bench can
 * replay the exact reference string. Every access increments @p d->accesses.
 *
 * @param[in,out] d     Driver state (cache handle, trace file, access counter).
 * @param[in]     frame Frame index within the book to touch.
 *
 * @pre @p d->vm is an initialised cache and @p d->trace is open for writing.
 * @pre @p frame is within the book's frame range.
 * @post One reference is issued to ra8_vmem and one line appended to the trace.
 * @post @p d->accesses is incremented by one.
 *
 * @note Not thread-safe; the tool is single-threaded.
 * @since 0.1.0
 */
static void rv_touch(rv_driver_t* d, uint32_t frame)
{
  const uint64_t  offset = (uint64_t)frame * (uint64_t)k_rv_frame_bytes;
  void*           page   = nullptr;
  const ra8_err_t err    = ra8_vmem_get(d->vm, d->object_id, offset, &page);
  if (err == k_ra8_ok) {
    (void)ra8_vmem_put(d->vm, page);
  } else {
    (void)fprintf(stderr, "vm_get(frame %u) failed: %d\n", frame, (int)err);
  }
  (void)fprintf(d->trace, "%u %u\n", (unsigned)d->object_id, (unsigned)frame);
  d->accesses++;
}

/**
 * @brief Touch the hot header/TOC region the reader consults every turn.
 *
 * @details
 * Re-references the first ::k_rv_header_frames frames -- the page furniture,
 * progress indicator and chapter map -- which a reader reads on every page turn
 * and every TOC open. Keeping this set hot is what separates scan-resistant
 * policies (2Q/SLRU) from LRU/CLOCK on this workload.
 *
 * @param[in,out] d Driver state, passed through to ::rv_touch.
 *
 * @pre @p d->vm is initialised and @p d->trace is open.
 * @pre ::k_rv_header_frames frames exist at the start of the book.
 * @post The header frames are referenced once each.
 * @post @p d->accesses grows by ::k_rv_header_frames.
 *
 * @note Not thread-safe; the tool is single-threaded.
 * @since 0.1.0
 */
static void rv_touch_header(rv_driver_t* d)
{
  for (uint32_t f = 0U; f < (uint32_t)k_rv_header_frames; ++f) {
    rv_touch(d, f);
  }
}

/**
 * @brief Phase A: linear read-through with hot header and back-glances.
 *
 * @details
 * Reads every chapter frame in order; before each page it consults the hot
 * header (::rv_touch_header) and with probability ::k_rv_backglance_pct
 * re-reads the previous page. This is the cold read of the book interleaved
 * with the recurring hot set a real reader generates.
 *
 * @param[in,out] d Driver state (chapter table, cache, trace).
 *
 * @pre @p d has been laid out (::rv_layout_book) and its cache initialised.
 * @pre @p d->trace is open for writing.
 * @post Every content frame has been referenced at least once.
 * @post The trace holds this phase's reference string.
 *
 * @note Not thread-safe; the tool is single-threaded.
 * @since 0.1.0
 */
static void rv_phase_linear(rv_driver_t* d)
{
  for (uint32_t c = 0U; c < (uint32_t)k_rv_chapters; ++c) {
    const rv_chapter_t ch = d->chapters[c];
    for (uint32_t i = 0U; i < ch.count; ++i) {
      const uint32_t frame = ch.first + i;
      rv_touch_header(d); /* the reader consults the header every page turn */
      rv_touch(d, frame);
      if ((rv_below(&d->rng, (uint32_t)k_rv_pct_base) < (uint32_t)k_rv_backglance_pct) &&
          (i > 0U)) {
        rv_touch(d, frame - 1U); /* glance back at the previous page */
      }
    }
  }
}

/**
 * @brief Phase B: TOC-driven jumps -- reopen the map, teleport, read.
 *
 * @details
 * Runs ::k_rv_toc_rounds rounds; each reopens the hot header (the table of
 * contents), picks a random chapter, and reads its first ::k_rv_toc_read_fr
 * frames. This is the non-sequential navigation that keeps the TOC and a
 * scattered working set hot -- the access pattern where recency-only policies
 * thrash.
 *
 * @param[in,out] d Driver state (chapter table, cache, trace).
 *
 * @pre @p d has been laid out and its cache initialised.
 * @pre @p d->trace is open for writing.
 * @post ::k_rv_toc_rounds jump sequences have been issued and traced.
 * @post @p d->accesses reflects the header re-reads and post-jump reads.
 *
 * @note Not thread-safe; the tool is single-threaded.
 * @since 0.1.0
 */
static void rv_phase_toc_jumps(rv_driver_t* d)
{
  for (uint32_t r = 0U; r < (uint32_t)k_rv_toc_rounds; ++r) {
    rv_touch_header(d); /* open the TOC */
    const uint32_t     c  = rv_below(&d->rng, (uint32_t)k_rv_chapters);
    const rv_chapter_t ch = d->chapters[c];
    for (uint32_t i = 0U; (i < (uint32_t)k_rv_toc_read_fr) && (i < ch.count); ++i) {
      rv_touch(d, ch.first + i);
    }
  }
}

/**
 * @brief Phase C: scan resistance -- hot re-reads interleaved with floods.
 *
 * @details
 * For ::k_rv_sr_rounds rounds, re-reads a ::k_rv_sr_hot_fr-frame hot working
 * set ::k_rv_sr_hot_pass times, then floods ::k_rv_sr_scan_fr one-shot frames
 * that advance across the book. A scan-resistant cache must keep the hot set
 * resident despite the flood; this phase is the direct test of that property.
 *
 * @param[in,out] d Driver state (cache, trace, total_frames).
 *
 * @pre @p d has been laid out and its cache initialised.
 * @pre @p d->total_frames exceeds ::k_rv_header_frames.
 * @post The hot set and the one-shot floods have been referenced and traced.
 * @post @p d->accesses reflects both the re-reads and the floods.
 *
 * @note Not thread-safe; the tool is single-threaded.
 * @since 0.1.0
 */
static void rv_phase_scan_resist(rv_driver_t* d)
{
  uint32_t scan_at = (uint32_t)k_rv_header_frames + (uint32_t)k_rv_sr_hot_fr;
  for (uint32_t r = 0U; r < (uint32_t)k_rv_sr_rounds; ++r) {
    for (uint32_t p = 0U; p < (uint32_t)k_rv_sr_hot_pass; ++p) {
      for (uint32_t h = 0U; h < (uint32_t)k_rv_sr_hot_fr; ++h) {
        rv_touch(d, (uint32_t)k_rv_header_frames + h); /* hot working set */
      }
    }
    for (uint32_t s = 0U; s < (uint32_t)k_rv_sr_scan_fr; ++s) {
      const uint32_t span  = d->total_frames - (uint32_t)k_rv_header_frames;
      const uint32_t frame = (uint32_t)k_rv_header_frames + ((scan_at + s) % span);
      rv_touch(d, frame); /* one-shot linear flood that must not evict the hot set */
    }
    scan_at += (uint32_t)k_rv_sr_scan_fr;
  }
}

/** @brief Owned page-cache backing buffers freed together on every exit path. */
typedef struct {
  uint8_t*          frame_mem; /**< Page-frame pool.         */
  ra8_vmem_frame_t* meta;      /**< Per-frame metadata.      */
  ra8_vmem_key_t*   keys;      /**< Per-frame key storage.   */
  int32_t*          buckets;   /**< Hash-bucket index heads. */
} rv_res_t;

/**
 * @brief Release the vmem backing buffers (each free tolerates NULL).
 *
 * @details
 * Frees the four owned allocations of an ::rv_res_t -- the frame pool, the
 * per-frame metadata and key arrays, and the hash-bucket table -- so a single
 * call cleans up on every error path. Each free() tolerates a NULL member, so a
 * partially-allocated ::rv_res_t is safe to release.
 *
 * @param[in,out] res Backing buffers to free; the members are left dangling.
 *
 * @pre @p res is non-NULL.
 * @pre Each member is either NULL or a live malloc/calloc pointer.
 * @post Every owned buffer is freed.
 * @post The caller must not read @p res's pointers after this returns.
 *
 * @note Not thread-safe; the tool is single-threaded.
 * @since 0.1.0
 */
static void rv_res_free(rv_res_t* res)
{
  free(res->frame_mem);
  free(res->meta);
  free(res->keys);
  free(res->buckets);
}

/**
 * @brief Allocate and deterministically pattern-fill the modelled book bytes.
 *
 * @details
 * Sizes the backing at @p d->total_frames * ::k_rv_frame_bytes, allocates it,
 * and fills every byte from a Knuth-multiplier sequence so page loads move real
 * (reproducible) data. The one global ::s_book_backing backs the single
 * registered vsource object.
 *
 * @param[in] d Driver state whose total_frames sizes the allocation.
 *
 * @return Whether the book backing was allocated.
 * @retval true  ::s_book_backing is allocated and filled.
 * @retval false Allocation failed (reported on stderr); nothing is filled.
 *
 * @pre @p d->total_frames has been set by ::rv_layout_book.
 * @pre ::s_book_backing is not already allocated.
 * @post On success ::s_book_backing and ::s_book_bytes describe the book.
 * @post On failure ::s_book_backing is left NULL.
 *
 * @note Not thread-safe; the tool is single-threaded.
 * @since 0.1.0
 */
RA8_NASA_RULE_3_OK /* host-only bench: mock book buffer */
  static bool
  rv_alloc_book(rv_driver_t* d)
{
  s_book_bytes   = (uint64_t)d->total_frames * (uint64_t)k_rv_frame_bytes;
  s_book_backing = (uint8_t*)malloc((size_t)s_book_bytes);
  if (s_book_backing == nullptr) {
    (void)fprintf(stderr,
                  "reader_vmem: OOM allocating %llu-byte book\n",
                  (unsigned long long)s_book_bytes);
    return false;
  }
  for (uint64_t i = 0U; i < s_book_bytes; ++i) {
    s_book_backing[i] = (uint8_t)(i * (uint64_t)k_rv_fill_mul >> (uint32_t)k_rv_fill_shift);
  }
  return true;
}

/**
 * @brief Allocate the frame pool and metadata and initialise the SLRU vmem.
 *
 * @details
 * Allocates the four backing buffers into @p res (frame pool, per-frame
 * metadata, key array, bucket table), wires them into an ra8_vmem_cfg_t with
 * the ra8_vsource loader over @p vs, and initialises @p vm. On any allocation
 * failure the caller frees @p res via ::rv_res_free; this function does not free
 * on failure, so ownership stays in one place.
 *
 * @param[out]    vm     Cache to initialise.
 * @param[in,out] res    Receives the four owned backing allocations.
 * @param[in]     budget Frame budget (cache capacity) for the run.
 * @param[in]     vs     Registered vsource the loader reads through.
 *
 * @return Whether the cache was set up.
 * @retval true  @p vm is initialised over the allocated buffers.
 * @retval false An allocation failed or ra8_vmem_init rejected the config.
 *
 * @pre @p vm, @p res and @p vs are non-NULL.
 * @pre @p vs has the book object registered.
 * @post On success @p vm is ready and @p res owns the backing buffers.
 * @post On failure whichever buffers were allocated remain owned by @p res.
 *
 * @note Not thread-safe; the tool is single-threaded.
 * @since 0.1.0
 */
RA8_NASA_RULE_3_OK /* host-only bench: dynamic cache arrays */
  static bool
  rv_vmem_setup(ra8_vmem_t* vm, rv_res_t* res, uint32_t budget, ra8_vsource_t* vs)
{
  res->frame_mem = (uint8_t*)malloc((size_t)budget * (size_t)k_rv_frame_bytes);
  res->meta      = (ra8_vmem_frame_t*)calloc((size_t)budget, sizeof(ra8_vmem_frame_t));
  res->keys      = (ra8_vmem_key_t*)calloc((size_t)budget, sizeof(ra8_vmem_key_t));
  res->buckets   = (int32_t*)malloc((size_t)k_rv_buckets * sizeof(int32_t));
  if ((res->frame_mem == nullptr) || (res->meta == nullptr) || (res->keys == nullptr) ||
      (res->buckets == nullptr)) {
    return false;
  }
  ra8_vmem_cfg_t cfg = {.frame_mem    = res->frame_mem,
                        .frame_bytes  = (uint32_t)k_rv_frame_bytes,
                        .frame_count  = budget,
                        .meta         = res->meta,
                        .keys         = res->keys,
                        .buckets      = res->buckets,
                        .bucket_count = (uint32_t)k_rv_buckets,
                        .loader       = ra8_vsource_loader,
                        .loader_ctx   = vs};
  return ra8_vmem_init(vm, &cfg) == k_ra8_ok;
}

/**
 * @brief Run the three navigation phases, close the trace, print SLRU stats.
 *
 * @details
 * Drives phases A/B/C in order (::rv_phase_linear, ::rv_phase_toc_jumps,
 * ::rv_phase_scan_resist), closes the trace file, reads the real ra8_vmem
 * hit/miss/eviction counters, and prints a one-block summary to stderr. Those
 * counters cross-check that ra8_vmem's own SLRU tracks the policy cache_bench
 * later replays from the trace.
 *
 * @param[in,out] d          Driver state (cache, trace, counters).
 * @param[in]     vm         The cache whose stats are reported.
 * @param[in]     budget     Frame budget, echoed in the report.
 * @param[in]     trace_path Trace path, echoed in the report.
 *
 * @pre @p d, @p vm and @p trace_path are non-NULL and the trace is open.
 * @pre The book has been laid out and allocated.
 * @post All three phases have run and @p d->trace is closed.
 * @post The SLRU hit/miss/eviction summary has been printed to stderr.
 *
 * @note Not thread-safe; the tool is single-threaded.
 * @since 0.1.0
 */
static void
rv_run_and_report(rv_driver_t* d, ra8_vmem_t* vm, uint32_t budget, const char* trace_path)
{
  rv_phase_linear(d);
  rv_phase_toc_jumps(d);
  rv_phase_scan_resist(d);
  (void)fclose(d->trace);

  uint32_t hits = 0U;
  uint32_t miss = 0U;
  uint32_t ev   = 0U;
  (void)ra8_vmem_stats(vm, &hits, &miss, &ev);
  const double hit_pct =
    (d->accesses == 0U) ? 0.0 : ((double)k_rv_pct_base * (double)hits / (double)d->accesses);
  (void)fprintf(stderr,
                "reader_vmem: book=%u frames (%llu bytes), budget=%u frames\n"
                "  accesses=%llu  trace=%s\n"
                "  ra8_vmem SLRU: hits=%u misses=%u evictions=%u  hit_rate=%.2f%%\n",
                d->total_frames,
                (unsigned long long)s_book_bytes,
                budget,
                (unsigned long long)d->accesses,
                trace_path,
                hits,
                miss,
                ev,
                hit_pct);
}

int main(int argc, char** argv)
{
  const char*    trace_path = (argc > 1) ? argv[1] : "reader_vmem.trace";
  const uint32_t budget = (argc > 2) ? (uint32_t)strtoul(argv[2], nullptr, (int)k_rv_decimal_base)
                                     : (uint32_t)k_rv_def_budget;

  rv_driver_t d = {};
  d.rng         = (uint64_t)k_rv_rng_seed;
  rv_layout_book(&d);
  if (!rv_alloc_book(&d)) {
    return 1;
  }

  ra8_vsource_obj_t objs[k_rv_max_objs] = {};
  ra8_vsource_t     vs                  = {};
  uint32_t          id                  = 0U;
  if ((ra8_vsource_init(&vs, objs, (uint32_t)k_rv_max_objs) != k_ra8_ok) ||
      (ra8_vsource_add_paged(&vs, rv_read, nullptr, 0U, s_book_bytes, &id) != k_ra8_ok)) {
    free(s_book_backing);
    return 1;
  }
  d.object_id = id;

  ra8_vmem_t vm  = {};
  rv_res_t   res = {};
  if (!rv_vmem_setup(&vm, &res, budget, &vs)) {
    rv_res_free(&res);
    free(s_book_backing);
    return 1;
  }
  d.vm    = &vm;
  d.trace = fopen(trace_path, "w");
  if (d.trace == nullptr) {
    (void)fprintf(stderr, "reader_vmem: cannot open trace file %s\n", trace_path);
    rv_res_free(&res);
    free(s_book_backing);
    // cppcheck-suppress memleak
    // closed inside rv_run_and_report()
    return 1;
  }

  rv_run_and_report(&d, &vm, budget, trace_path);

  rv_res_free(&res);
  free(s_book_backing);
  return 0;
}
