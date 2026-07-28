/**
 * @file trace.c
 * @brief Implementation of the #147 reader access-trace corpus + loader.
 *
 * @details All synthetic workloads are driven by a fixed-seed xorshift PRNG so
 * every policy sees byte-identical input and runs are reproducible (the same
 * determinism ra8_emulator gives the captured traces).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * [Ring 7 / Tooling] {World: NS}
 *
 * @since 0.1.0
 */
#include "trace.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * @enum cb_workload_dim_t
 * @brief Workload sizing constants for the synthetic access-trace generators.
 * @details
 * All synthetic-trace parameters live here so every generator function is
 * free of magic literals. The values are chosen to create a realistic spread
 * of access patterns (sequential, random, hot-set locality, scan-flood) at a
 * scale large enough to distinguish policy hit rates while finishing in under
 * one second on a desktop host.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_cb_obj_book    = 1U,      /**< Object id of the paged EPUB.                 */
  k_cb_obj_comic   = 2U,      /**< Object id of the scrolled CBZ tiles.         */
  k_cb_footprint   = 8192U,   /**< Pages in the huge file (>> any cache).       */
  k_cb_accesses    = 120000U, /**< Accesses per synthetic trace.                */
  k_cb_hot_pages   = 96U,     /**< Re-read working-set size (locality).         */
  k_cb_reread_pct  = 82U,     /**< % of re-read accesses inside the hotset.     */
  k_cb_jump_pct    = 4U,      /**< % of TOC-jump accesses that teleport.        */
  k_cb_tile_span   = 6144U,   /**< Comic tiles (sequential scroll).             */
  k_cb_sr_hot      = 192U,    /**< Re-referenced hot set (fits mid caches).     */
  k_cb_sr_hot_pass = 3U,      /**< Hot-set passes between scan floods.          */
  k_cb_sr_scan     = 1500U,   /**< Unique pages in each one-time scan.          */
  k_cb_pct_full    = 100U,    /**< Divisor for percentage-range decisions.      */
  k_cb_mixed_phase = 2048U,   /**< Accesses per phase in the mixed session.     */
  k_cb_load_init   = 4096U,   /**< Initial key-array capacity in cb_trace_load. */
  k_cb_line_max    = 128U,    /**< Line-buffer size for the trace-file loader.  */
} cb_workload_dim_t;

/**
 * @enum cb_huge_dim_t
 * @brief GB-class "huge book" workload sizing (the #147 headline case).
 * @details A genuinely massive object -- 1,835,008 pages, i.e. ~7 GiB at a 4 KiB
 *          page -- so the footprint dwarfs even the largest swept cache (2048
 *          frames) by ~900x and is independent of the resident budget entirely.
 *          The pattern is a re-referenced hot front-matter set (TOC / progress
 *          furniture) interleaved with one-shot linear floods: the realistic
 *          huge-EPUB reader workload. SLRU's protected segment must hold the hot
 *          set while the flood -- which can never fit -- churns the probationary
 *          segment, the regime where bounded residency and scan resistance matter
 *          most and where LRU/CLOCK thrash hardest.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_cb_huge_footprint = 1835008U, /**< Pages in the ~7 GiB object (>> any cache).  */
  k_cb_huge_hot       = 256U,     /**< Re-referenced hot front-matter working set. */
  k_cb_huge_hot_pass  = 3U,       /**< Hot-set passes between scan floods.         */
  k_cb_huge_scan      = 4000U,    /**< Unique pages in each one-shot linear flood. */
} cb_huge_dim_t;

/**
 * @enum cb_rng_shift_t
 * @brief xorshift64 shift-amount triple for @ref cb_rng.
 * @details The triple (13, 7, 17) is one of the parameter sets in
 *          Marsaglia (2003) for a full-period 64-bit xorshift generator.
 *          Changing any value breaks the period guarantee.
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_rng_shift_a = 13U, /**< MAGIC-OK: xorshift64 left-shift a (Marsaglia 2003 set)  */
  k_rng_shift_b = 7U,  /**< MAGIC-OK: xorshift64 right-shift b (Marsaglia 2003 set) */
  k_rng_shift_c = 17U, /**< MAGIC-OK: xorshift64 left-shift c (Marsaglia 2003 set)  */
} cb_rng_shift_t;

/**
 * @enum cb_rng_seed_t
 * @brief Fixed initial PRNG seeds used by the synthetic trace generators.
 * @details Each generator uses a distinct seed so the traces are
 *          statistically independent. The values are arbitrary non-zero 64-bit
 *          integers chosen for good initial bit distribution (odd values avoid
 *          trivial zero-collapse in xorshift); the MAGIC-OK markers record
 *          that their exact bit patterns are not semantically significant.
 * @since 0.1.0
 */
typedef enum : uint64_t {
  k_rng_seed_random =
    0x9E3779B97F4A7C15ULL, /**< MAGIC-OK: Fibonacci golden-ratio seed (random trace) */
  k_rng_seed_reread =
    0xD1B54A32D192ED03ULL, /**< MAGIC-OK: fixed arbitrary seed (reread-locality trace) */
  k_rng_seed_toc = 0x2545F4914F6CDD1DULL, /**< MAGIC-OK: fixed arbitrary seed (toc-jumps trace) */
  k_rng_seed_mixed_a =
    0x9E3779B97F4A7C15ULL, /**< MAGIC-OK: Fibonacci golden-ratio seed (mixed trace base) */
  k_rng_seed_mixed_b =
    0xABCDEF1234567890ULL, /**< MAGIC-OK: fixed arbitrary seed XOR'd into mixed trace base */
} cb_rng_seed_t;

/** @brief Fixed-seed xorshift64; deterministic across runs and platforms. */
static uint64_t cb_rng(uint64_t* s)
{
  uint64_t x = *s;
  x ^= x << (uint8_t)k_rng_shift_a;
  x ^= x >> (uint8_t)k_rng_shift_b;
  x ^= x << (uint8_t)k_rng_shift_c;
  *s = x;
  return x;
}

/** @brief Uniform integer in [0, span). */
static uint32_t cb_rand_below(uint64_t* s, uint32_t span)
{
  return (span == 0U) ? 0U : (uint32_t)(cb_rng(s) % (uint64_t)span);
}

/** @brief Allocate a trace's key array; returns false on OOM. */
static bool cb_alloc(cb_trace_t* t, const char* name, uint64_t n)
{
  t->name      = name;
  t->n         = n;
  t->footprint = 0U;
  t->keys      = (cb_key_t*)calloc((size_t)n, sizeof(cb_key_t));
  return t->keys != nullptr;
}

/** @brief Sequential page-turn flooding: linear passes through a huge book. */
static cb_trace_t cb_gen_sequential(void)
{
  cb_trace_t t = {};
  if (!cb_alloc(&t, "seq-pageturn", k_cb_accesses)) {
    return t;
  }
  for (uint64_t i = 0U; i < t.n; ++i) {
    const uint32_t page = (uint32_t)(i % (uint64_t)k_cb_footprint);
    t.keys[i]           = (cb_key_t){.object_id = k_cb_obj_book, .page = page};
  }
  t.footprint = k_cb_footprint;
  return t;
}

/** @brief Uniform random access over the whole book (TOC/bookmark chaos). */
static cb_trace_t cb_gen_random(void)
{
  cb_trace_t t = {};
  if (!cb_alloc(&t, "random", k_cb_accesses)) {
    return t;
  }
  uint64_t s = (uint64_t)k_rng_seed_random;
  for (uint64_t i = 0U; i < t.n; ++i) {
    t.keys[i] = (cb_key_t){.object_id = k_cb_obj_book, .page = cb_rand_below(&s, k_cb_footprint)};
  }
  t.footprint = k_cb_footprint;
  return t;
}

/** @brief Back-and-forth re-reading: a hot working set with occasional spread. */
static cb_trace_t cb_gen_reread(void)
{
  cb_trace_t t = {};
  if (!cb_alloc(&t, "reread-locality", k_cb_accesses)) {
    return t;
  }
  uint64_t s    = (uint64_t)k_rng_seed_reread;
  uint32_t base = 0U;
  for (uint64_t i = 0U; i < t.n; ++i) {
    uint32_t page;
    if (cb_rand_below(&s, (uint32_t)k_cb_pct_full) < (uint32_t)k_cb_reread_pct) {
      page = base + cb_rand_below(&s, k_cb_hot_pages);
    } else {
      page = cb_rand_below(&s, k_cb_footprint);
      base = (page < k_cb_hot_pages) ? 0U : (page - k_cb_hot_pages); /* drift the hotset */
    }
    t.keys[i] = (cb_key_t){.object_id = k_cb_obj_book, .page = page % k_cb_footprint};
  }
  t.footprint = k_cb_footprint;
  return t;
}

/** @brief Mostly-linear reading with occasional TOC/bookmark teleports. */
static cb_trace_t cb_gen_toc_jumps(void)
{
  cb_trace_t t = {};
  if (!cb_alloc(&t, "linear+jumps", k_cb_accesses)) {
    return t;
  }
  uint64_t s    = (uint64_t)k_rng_seed_toc;
  uint32_t page = 0U;
  for (uint64_t i = 0U; i < t.n; ++i) {
    if (cb_rand_below(&s, (uint32_t)k_cb_pct_full) < (uint32_t)k_cb_jump_pct) {
      page = cb_rand_below(&s, k_cb_footprint);
    } else {
      page = (page + 1U) % k_cb_footprint;
    }
    t.keys[i] = (cb_key_t){.object_id = k_cb_obj_book, .page = page};
  }
  t.footprint = k_cb_footprint;
  return t;
}

/** @brief CBZ image-tile scroll: long sequential runs over a second object. */
static cb_trace_t cb_gen_scroll(void)
{
  cb_trace_t t = {};
  if (!cb_alloc(&t, "cbz-scroll", k_cb_accesses)) {
    return t;
  }
  for (uint64_t i = 0U; i < t.n; ++i) {
    t.keys[i] =
      (cb_key_t){.object_id = k_cb_obj_comic, .page = (uint32_t)(i % (uint64_t)k_cb_tile_span)};
  }
  t.footprint = k_cb_tile_span;
  return t;
}

/** @brief A realistic mixed session: read -> jump -> reread -> scroll, repeated. */
static cb_trace_t cb_gen_mixed(void)
{
  cb_trace_t t = {};
  if (!cb_alloc(&t, "mixed-session", k_cb_accesses)) {
    return t;
  }
  uint64_t s    = (uint64_t)k_rng_seed_mixed_a ^ (uint64_t)k_rng_seed_mixed_b;
  uint32_t page = 0U;
  uint32_t hot  = 0U;
  for (uint64_t i = 0U; i < t.n; ++i) {
    const uint32_t phase =
      (uint32_t)((i / (uint64_t)k_cb_mixed_phase) % 4U); /* alternate behaviours */
    if (phase == 0U) {
      page = (page + 1U) % k_cb_footprint; /* linear */
    } else if (phase == 1U) {
      page = hot + cb_rand_below(&s, k_cb_hot_pages); /* reread */
    } else if (phase == 2U) {
      page = cb_rand_below(&s, k_cb_footprint); /* random jumps */
      hot  = (page < k_cb_hot_pages) ? 0U : (page - k_cb_hot_pages);
    } else {
      t.keys[i] = (cb_key_t){.object_id = k_cb_obj_comic, .page = page % k_cb_tile_span};
      continue;
    }
    t.keys[i] = (cb_key_t){.object_id = k_cb_obj_book, .page = page % k_cb_footprint};
  }
  t.footprint = k_cb_footprint;
  return t;
}

/**
 * @brief The scan-resistance case: a fixed hot set re-read between one-time
 *        linear scans that flood the cache. LRU/CLOCK evict the hot set under
 *        the scan and thrash; SLRU/SRRIP keep the re-referenced hot set.
 */
static cb_trace_t cb_gen_scan_resist(void)
{
  cb_trace_t t = {};
  if (!cb_alloc(&t, "hotset+scan", k_cb_accesses)) {
    return t;
  }
  uint64_t i        = 0U;
  uint32_t scan_pos = (uint32_t)k_cb_hot_pages; /* scan starts past the hot set */
  while (i < t.n) {
    for (uint32_t pass = 0U; (pass < (uint32_t)k_cb_sr_hot_pass) && (i < t.n); ++pass) {
      for (uint32_t h = 0U; (h < (uint32_t)k_cb_sr_hot) && (i < t.n); ++h, ++i) {
        t.keys[i] = (cb_key_t){.object_id = k_cb_obj_book, .page = h};
      }
    }
    for (uint32_t s = 0U; (s < (uint32_t)k_cb_sr_scan) && (i < t.n); ++s, ++i) {
      const uint32_t page =
        (uint32_t)k_cb_sr_hot + ((scan_pos + s) % (k_cb_footprint - (uint32_t)k_cb_sr_hot));
      t.keys[i] = (cb_key_t){.object_id = k_cb_obj_book, .page = page};
    }
    scan_pos += (uint32_t)k_cb_sr_scan;
  }
  t.footprint = k_cb_footprint;
  return t;
}

/**
 * @brief GB-class huge-book: a hot front-matter set re-read between one-shot
 *        linear floods across a ~7 GiB object. Same scan-resistance shape as
 *        ::cb_gen_scan_resist but with a footprint that dwarfs every swept cache
 *        (>> 2048 frames), so the SLRU-vs-LRU gap persists at all budgets and the
 *        resident set stays bounded independent of the (huge) file size.
 */
static cb_trace_t cb_gen_hugebook(void)
{
  cb_trace_t t = {};
  if (!cb_alloc(&t, "hugebook-7GiB", k_cb_accesses)) {
    return t;
  }
  uint64_t i        = 0U;
  uint32_t scan_pos = (uint32_t)k_cb_huge_hot; /* scan starts past the hot set */
  while (i < t.n) {
    for (uint32_t pass = 0U; (pass < (uint32_t)k_cb_huge_hot_pass) && (i < t.n); ++pass) {
      for (uint32_t h = 0U; (h < (uint32_t)k_cb_huge_hot) && (i < t.n); ++h, ++i) {
        t.keys[i] = (cb_key_t){.object_id = k_cb_obj_book, .page = h};
      }
    }
    for (uint32_t s = 0U; (s < (uint32_t)k_cb_huge_scan) && (i < t.n); ++s, ++i) {
      const uint32_t page = (uint32_t)k_cb_huge_hot +
                            ((scan_pos + s) % (k_cb_huge_footprint - (uint32_t)k_cb_huge_hot));
      t.keys[i]           = (cb_key_t){.object_id = k_cb_obj_book, .page = page};
    }
    scan_pos += (uint32_t)k_cb_huge_scan;
  }
  t.footprint = (uint32_t)k_cb_huge_footprint;
  return t;
}

cb_trace_t* cb_traces_synthetic(uint32_t* out_count)
{
  cb_trace_t (*gens[])(void) = {
    cb_gen_sequential,
    cb_gen_random,
    cb_gen_reread,
    cb_gen_toc_jumps,
    cb_gen_scroll,
    cb_gen_scan_resist,
    cb_gen_hugebook,
    cb_gen_mixed,
  };
  const uint32_t count = (uint32_t)(sizeof(gens) / sizeof(gens[0]));
  cb_trace_t*    out   = (cb_trace_t*)calloc((size_t)count, sizeof(cb_trace_t));
  if (out == nullptr) {
    *out_count = 0U;
    /* cppcheck-suppress memleak ; false positive: cppcheck 2.13 does not
     * model the C23 nullptr keyword, so it cannot see out is NULL here. */
    return nullptr;
  }
  for (uint32_t i = 0U; i < count; ++i) {
    out[i] = gens[i]();
  }
  *out_count = count;
  return out;
}

/**
 * @enum cb_radix_t
 * @brief Numeric radix constants for the trace-line parser.
 * @details Trace files store both fields in decimal.
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_cb_base_dec = 10U, /**< Decimal radix passed to strtoul. */
} cb_radix_t;

/**
 * @brief Parse one `<object> <page>` trace line with full error detection.
 *
 * @details Converts the two unsigned decimal fields with `strtoul`, checking
 *          the end pointer and `errno` for each (the CERT ERR34-C way of
 *          detecting conversion errors that `fscanf("%u")` cannot report).
 *
 * @param[in]  line NUL-terminated text line.
 * @param[out] obj  Receives the object id field.
 * @param[out] pg   Receives the page index field.
 *
 * @return bool true when both fields parsed cleanly, false otherwise.
 * @retval true  @p obj and @p pg hold the two parsed values.
 * @retval false The line is malformed or a value is out of range.
 *
 * @pre @p line, @p obj, and @p pg are non-NULL.
 * @post On false, @p obj / @p pg are unspecified (caller stops the load).
 *
 * @note Not thread-safe (reads and writes `errno`).
 * @since 0.1.0
 */
static bool cb_parse_trace_line(const char* line, uint32_t* obj, uint32_t* pg)
{
  char* end             = nullptr;
  errno                 = 0;
  const unsigned long o = strtoul(line, &end, (int)k_cb_base_dec);
  if ((end == line) || (errno != 0) || (o > UINT32_MAX)) {
    return false;
  }
  const char* second    = end;
  errno                 = 0;
  const unsigned long p = strtoul(second, &end, (int)k_cb_base_dec);
  if ((end == second) || (errno != 0) || (p > UINT32_MAX)) {
    return false;
  }
  *obj = (uint32_t)o;
  *pg  = (uint32_t)p;
  return true;
}

cb_trace_t cb_trace_load(const char* path, const char* name)
{
  cb_trace_t t = {.name = name};
  FILE*      f = fopen(path, "r");
  if (f == nullptr) {
    /* cppcheck-suppress resourceLeak ; false positive: cppcheck 2.13 does not
     * model the C23 nullptr keyword, so it cannot see f is NULL here. */
    return t;
  }
  uint64_t cap = (uint64_t)k_cb_load_init;
  t.keys       = (cb_key_t*)calloc((size_t)cap, sizeof(cb_key_t));
  if (t.keys == nullptr) {
    (void)fclose(f);
    return t;
  }
  char line[k_cb_line_max] = {};
  while (fgets(line, (int)sizeof(line), f) != nullptr) {
    uint32_t obj = 0U;
    uint32_t pg  = 0U;
    if (!cb_parse_trace_line(line, &obj, &pg)) {
      break; /* stop at the first malformed record, as fscanf did */
    }
    if (t.n == cap) {
      cap *= 2U;
      cb_key_t* grown = (cb_key_t*)realloc(t.keys, (size_t)cap * sizeof(cb_key_t));
      if (grown == nullptr) {
        break;
      }
      t.keys = grown;
    }
    t.keys[t.n] = (cb_key_t){.object_id = obj, .page = pg};
    t.n++;
  }
  (void)fclose(f);
  return t;
}

void cb_trace_free(cb_trace_t* t)
{
  if (t != nullptr) {
    free(t->keys);
    t->keys = nullptr;
    t->n    = 0U;
  }
}

void cb_traces_free(cb_trace_t* traces, uint32_t count)
{
  if (traces == nullptr) {
    return;
  }
  for (uint32_t i = 0U; i < count; ++i) {
    cb_trace_free(&traces[i]);
  }
  free(traces);
}
