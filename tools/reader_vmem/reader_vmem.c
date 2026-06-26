/**
 * @file reader_vmem.c
 * @brief #147/#162 reader workload driver: page a modelled book through the real
 *        ra_vmem page cache + ra_vsource registry, emit the captured vm_get trace
 *        for tools/cache_bench, and report the firmware cache's own hit rate.
 *
 * @details
 * This is a HOST tool (not firmware): it confirms the #147 SLRU decision on a
 * realistic reader workload using the ACTUAL Layer-1/Layer-2 implementation
 * rather than a re-modelled policy. It registers a modelled book (a header/TOC
 * hot region followed by variable-size chapters) as an ::ra_vsource paged object
 * over an in-memory backing, drives ::ra_vmem with a reader navigation session
 * (linear page-turns with a hot page-furniture region + back-glances, TOC-driven
 * jumps, and a scan-resistance phase of hot-set re-reads interleaved with
 * one-shot linear floods), and for every `ra_vmem_get` writes one `<object>
 * <page>` line to the trace file. Feed that file to `cache_bench reader=<file>`
 * to replay it across every candidate policy and confirm SLRU still wins; the
 * firmware cache's own hit/miss counters (printed to stderr) cross-check that
 * `ra_vmem`'s real SLRU tracks the benched policy.
 *
 * The navigation pattern -- not the cold load -- is what makes the cache
 * interesting: a pure sequential load is all-miss for every policy, whereas a
 * reader re-references the TOC and a working set while flooding past one-shot
 * pages, which is exactly where 2Q/SLRU beats LRU/CLOCK.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * [Ring 7 / Tooling] {World: NS}
 *
 * @since 0.1.0
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ra_err.h"
#include "ra_log.h"
#include "ra_vmem.h"
#include "ra_vsource.h"

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

/** @brief ra_vmem sizing for the driver's own cache run. */
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
  ra_vmem_t*   vm;                      /**< The cache being driven.        */
  uint32_t     object_id;               /**< Registered book object id.     */
  FILE*        trace;                   /**< Trace output (`<obj> <page>`). */
  uint64_t     accesses;                /**< Accesses emitted so far.       */
} rv_driver_t;

/** @brief Log backend stub so ra_check's RA_CHECK_NULL_PTR links host-side. */
void internal_ra_log_error(const char* tag, const char* message)
{
  (void)fprintf(stderr, "[ra_log] %s: %s\n", tag, message);
}

/** @brief Valued log backend stub (unused here, present for the linker). */
void internal_ra_log_error_val(const char* tag, const char* message, uint32_t value)
{
  (void)fprintf(stderr, "[ra_log] %s: %s =%u\n", tag, message, value);
}

/** @brief Fixed-seed xorshift64; deterministic across runs and platforms. */
static uint64_t rv_rng(uint64_t* s)
{
  uint64_t x = *s;
  x ^= x << (uint32_t)k_rv_xs_shift_a;
  x ^= x >> (uint32_t)k_rv_xs_shift_b;
  x ^= x << (uint32_t)k_rv_xs_shift_c;
  *s = x;
  return x;
}

/** @brief Uniform integer in [0, span). */
static uint32_t rv_below(uint64_t* s, uint32_t span)
{
  return (span == 0U) ? 0U : (uint32_t)(rv_rng(s) % (uint64_t)span);
}

/** @brief The in-memory book backing (filled deterministically). */
static uint8_t* s_book_backing;
/** @brief Size of ::s_book_backing in bytes. */
static uint64_t s_book_bytes;

/** @brief ra_vsource read callback: serve bytes from the in-memory backing. */
static ra_err_t rv_read(void* ctx, uint64_t offset, uint8_t* buf, uint32_t len)
{
  (void)ctx;
  if (offset + (uint64_t)len > s_book_bytes) {
    return k_ra_err_out_of_range;
  }
  (void)memcpy(buf, &s_book_backing[offset], (size_t)len);
  return k_ra_ok;
}

/** @brief Lay out chapters (varied sizes) after the hot header region. */
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

/** @brief Issue one access: drive ra_vmem and emit the trace line. */
static void rv_touch(rv_driver_t* d, uint32_t frame)
{
  const uint64_t offset = (uint64_t)frame * (uint64_t)k_rv_frame_bytes;
  void*          page   = nullptr;
  const ra_err_t err    = ra_vmem_get(d->vm, d->object_id, offset, &page);
  if (err == k_ra_ok) {
    (void)ra_vmem_put(d->vm, page);
  } else {
    (void)fprintf(stderr, "vm_get(frame %u) failed: %d\n", frame, (int)err);
  }
  (void)fprintf(d->trace, "%u %u\n", (unsigned)d->object_id, (unsigned)frame);
  d->accesses++;
}

/** @brief Touch the hot header/TOC region (page furniture, progress, chapter map). */
static void rv_touch_header(rv_driver_t* d)
{
  for (uint32_t f = 0U; f < (uint32_t)k_rv_header_frames; ++f) {
    rv_touch(d, f);
  }
}

/** @brief Phase A: linear read-through with hot header + occasional back-glance. */
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

/** @brief Phase B: TOC-driven jumps -- re-read the chapter map, teleport, read. */
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

/** @brief Phase C: scan resistance -- hot-set re-reads interleaved with floods. */
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

int main(int argc, char** argv)
{
  const char*    trace_path = (argc > 1) ? argv[1] : "reader_vmem.trace";
  const uint32_t budget = (argc > 2) ? (uint32_t)strtoul(argv[2], nullptr, (int)k_rv_decimal_base)
                                     : (uint32_t)k_rv_def_budget;

  rv_driver_t d = {};
  d.rng         = (uint64_t)k_rv_rng_seed;
  rv_layout_book(&d);

  s_book_bytes   = (uint64_t)d.total_frames * (uint64_t)k_rv_frame_bytes;
  s_book_backing = (uint8_t*)malloc((size_t)s_book_bytes);
  if (s_book_backing == nullptr) {
    (void)fprintf(stderr,
                  "reader_vmem: OOM allocating %llu-byte book\n",
                  (unsigned long long)s_book_bytes);
    return 1;
  }
  for (uint64_t i = 0U; i < s_book_bytes; ++i) {
    s_book_backing[i] = (uint8_t)(i * (uint64_t)k_rv_fill_mul >> (uint32_t)k_rv_fill_shift);
  }

  ra_vsource_obj_t objs[k_rv_max_objs] = {};
  ra_vsource_t     vs                  = {};
  if (ra_vsource_init(&vs, objs, (uint32_t)k_rv_max_objs) != k_ra_ok) {
    free(s_book_backing);
    return 1;
  }
  uint32_t id = 0U;
  if (ra_vsource_add_paged(&vs, rv_read, nullptr, 0U, s_book_bytes, &id) != k_ra_ok) {
    free(s_book_backing);
    return 1;
  }
  d.object_id = id;

  uint8_t*         frame_mem = (uint8_t*)malloc((size_t)budget * (size_t)k_rv_frame_bytes);
  ra_vmem_frame_t* meta      = (ra_vmem_frame_t*)calloc((size_t)budget, sizeof(ra_vmem_frame_t));
  int32_t*         buckets   = (int32_t*)malloc((size_t)k_rv_buckets * sizeof(int32_t));
  if ((frame_mem == nullptr) || (meta == nullptr) || (buckets == nullptr)) {
    free(frame_mem);
    free(meta);
    free(buckets);
    free(s_book_backing);
    return 1;
  }
  ra_vmem_cfg_t cfg = {.frame_mem    = frame_mem,
                       .frame_bytes  = (uint32_t)k_rv_frame_bytes,
                       .frame_count  = budget,
                       .meta         = meta,
                       .buckets      = buckets,
                       .bucket_count = (uint32_t)k_rv_buckets,
                       .loader       = ra_vsource_loader,
                       .loader_ctx   = &vs};
  ra_vmem_t     vm  = {};
  if (ra_vmem_init(&vm, &cfg) != k_ra_ok) {
    free(frame_mem);
    free(meta);
    free(buckets);
    free(s_book_backing);
    return 1;
  }
  d.vm    = &vm;
  d.trace = fopen(trace_path, "w");
  if (d.trace == nullptr) {
    (void)fprintf(stderr, "reader_vmem: cannot open trace file %s\n", trace_path);
    free(frame_mem);
    free(meta);
    free(buckets);
    free(s_book_backing);
    return 1;
  }

  rv_phase_linear(&d);
  rv_phase_toc_jumps(&d);
  rv_phase_scan_resist(&d);
  (void)fclose(d.trace);

  uint32_t hits = 0U;
  uint32_t miss = 0U;
  uint32_t ev   = 0U;
  (void)ra_vmem_stats(&vm, &hits, &miss, &ev);
  const double hit_pct =
    (d.accesses == 0U) ? 0.0 : ((double)k_rv_pct_base * (double)hits / (double)d.accesses);
  (void)fprintf(stderr,
                "reader_vmem: book=%u frames (%llu bytes), budget=%u frames\n"
                "  accesses=%llu  trace=%s\n"
                "  ra_vmem SLRU: hits=%u misses=%u evictions=%u  hit_rate=%.2f%%\n",
                d.total_frames,
                (unsigned long long)s_book_bytes,
                budget,
                (unsigned long long)d.accesses,
                trace_path,
                hits,
                miss,
                ev,
                hit_pct);

  free(frame_mem);
  free(meta);
  free(buckets);
  free(s_book_backing);
  return 0;
}
