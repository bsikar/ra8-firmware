/**
 * @file cache_bench.c
 * @brief #147 eviction-policy benchmark harness: replay + sweep + report.
 *
 * @details
 * Replays each access trace through each registered ::cache_policy_t at a swept
 * set of cache capacities and prints a markdown report (hit-rate matrix +
 * WCET/metadata summary + a recommendation) suitable for pasting into the #147
 * decision record. The resident-set lookup is an exact key->frame chained hash
 * (one node per frame, no tombstones) so hit accounting is precise; only
 * ordering is delegated to the policy under test.
 *
 * `--sweep-block` selects the orthogonal #208 mode instead: sweep the block /
 * frame / chunk SIZE in bytes through the real ::ra8_vmem stack (see
 * sweep_block.h) rather than the capacity in frames.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * [Ring 7 / Tooling] {World: NS}
 *
 * @since 0.1.0
 */
#include "cache_bench.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sweep_block.h"
#include "trace.h"

/** @brief Round @p v up to a power of two (>= 1). */
static uint32_t cb_pow2_ceil(uint32_t v)
{
  uint32_t p = 1U;
  while (p < v) {
    p <<= 1U;
  }
  return p;
}

/**
 * @enum cb_hash_mul_t
 * @brief Murmur3 finalizer constants used in ::cb_hash.
 * @details These are the canonical Murmur3 64-bit finalization mix constants
 *          (Austin Appleby, 2011). They are algorithm-specified bit patterns
 *          chosen for avalanche quality and must not be renamed to hide their
 *          origin; the MAGIC-OK markers document the rationale.
 * @see https://github.com/aappleby/smhasher
 * @since 0.1.0
 */
typedef enum : uint64_t {
  k_hash_mix_mul = 0xFF51AFD7ED558CCDULL, /**< MAGIC-OK: Murmur3 64-bit finalization multiplier */
} cb_hash_mul_t;

/**
 * @enum cb_hash_shift_t
 * @brief Murmur3 finalizer bit-shift amounts used in ::cb_hash.
 * @details The value 33 is mandated by the Murmur3 64-bit finalization
 *          algorithm. It must equal exactly 33 for the required avalanche
 *          properties; the MAGIC-OK marker documents this constraint.
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_hash_shift = 33U, /**< MAGIC-OK: Murmur3 64-bit finalizer shift amount */
} cb_hash_shift_t;

/** @brief Mix an (object,page) key into a hash bucket. */
static uint32_t cb_hash(cb_key_t k)
{
  uint64_t h = ((uint64_t)k.object_id << 32U) | (uint64_t)k.page;
  h ^= h >> (uint8_t)k_hash_shift;
  h *= (uint64_t)k_hash_mix_mul;
  h ^= h >> (uint8_t)k_hash_shift;
  return (uint32_t)h;
}

/** @brief true iff two keys are equal. */
static bool cb_key_eq(cb_key_t a, cb_key_t b)
{
  return (a.object_id == b.object_id) && (a.page == b.page);
}

/**
 * @struct cb_index_t
 * @brief Exact key->frame lookup over the resident set (chained hash).
 * @details One chain node per frame, no tombstones, so hit accounting is
 *          precise; only eviction ordering is delegated to the policy.
 * @invariant `mask + 1` is the (power-of-two) bucket count.
 * @since 0.1.0
 */
typedef struct {
  int32_t* bucket; /**< Head frame index per bucket, or -1. */
  int32_t* next;   /**< Per-frame chain link, or -1.        */
  uint32_t mask;   /**< Bucket-count-minus-one bit mask.    */
} cb_index_t;

/** @brief Find the resident frame holding @p key, or -1. */
static int32_t cb_index_find(const cb_index_t* idx, const cb_frame_t* frames, cb_key_t key)
{
  for (int32_t j = idx->bucket[cb_hash(key) & idx->mask]; j != -1; j = idx->next[j]) {
    if (cb_key_eq(frames[j].key, key)) {
      return j;
    }
  }
  return -1;
}

/** @brief Unlink @p frame's current key from its bucket chain (pre-evict). */
static void cb_index_remove(cb_index_t* idx, const cb_frame_t* frames, uint32_t frame)
{
  const uint32_t ob   = cb_hash(frames[frame].key) & idx->mask;
  int32_t        prev = -1;
  for (int32_t j = idx->bucket[ob]; j != -1; prev = j, j = idx->next[j]) {
    if (j == (int32_t)frame) {
      if (prev == -1) {
        idx->bucket[ob] = idx->next[j];
      } else {
        idx->next[prev] = idx->next[j];
      }
      break;
    }
  }
}

/** @brief Link @p frame into the bucket chain for @p key (post-insert). */
static void cb_index_push(cb_index_t* idx, uint32_t frame, cb_key_t key)
{
  const uint32_t b = cb_hash(key) & idx->mask;
  idx->next[frame] = idx->bucket[b];
  idx->bucket[b]   = (int32_t)frame;
}

/**
 * @brief Pick the frame that receives @p key: a free frame while the cache
 *        fills, else the policy's victim (accounted + unlinked).
 *
 * @details Charges eviction stats (count, total/worst scan) to @p out and
 *          unlinks an evicted frame's old key from the index, so the caller
 *          only relinks the new key.
 *
 * @param[in]     pol    Policy under test (its `pick_victim` may run).
 * @param[in,out] cache  The frame cache (frames + policy state).
 * @param[in,out] idx    Resident-set index (victim unlinked in place).
 * @param[in,out] filled Frames used so far; grows while cold.
 * @param[in,out] out    Metrics row receiving eviction accounting.
 *
 * @return uint32_t Frame index to (re)populate (always < capacity).
 *
 * @pre The cache is missing the current key (lookup already failed).
 * @pre `pol->pick_victim` is bound (every registered policy binds it).
 * @post On the eviction path, the victim's old key is no longer findable.
 * @post `*filled <= cache->capacity`.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static uint32_t cb_replay_take_frame(const cache_policy_t* pol,
                                     cb_cache_t*           cache,
                                     cb_index_t*           idx,
                                     uint32_t*             filled,
                                     cb_result_t*          out)
{
  if (*filled < cache->capacity) {
    const uint32_t frame = *filled;
    (*filled)++;
    return frame;
  }
  uint32_t       scanned = 0U;
  const uint32_t frame   = pol->pick_victim(cache, &scanned);
  out->evictions++;
  out->total_scan += scanned;
  if (scanned > out->worst_scan) {
    out->worst_scan = scanned;
  }
  cb_index_remove(idx, cache->frames, frame);
  return frame;
}

/**
 * @brief Allocate the frame array + resident-set index for one replay run.
 *
 * @details Sizes the bucket table at four buckets per frame (power-of-two)
 *          and marks every bucket empty. On a partial allocation failure the
 *          acquired buffers stay bound to @p idx / @p frames; the caller
 *          releases them through ::cb_replay_close on every exit path.
 *
 * @param[out] idx      Receives the initialized (empty) index.
 * @param[out] frames   Receives the zeroed frame array.
 * @param[in]  capacity Frame count (> 0).
 *
 * @return bool true when everything allocated, false on OOM.
 * @retval true  @p idx and @p frames are ready for the replay loop.
 * @retval false At least one buffer is NULL; free via ::cb_replay_close.
 *
 * @pre @p idx and @p frames are non-NULL.
 * @post On true, every bucket head is -1 (empty resident set).
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static bool cb_replay_open(cb_index_t* idx, cb_frame_t** frames, uint32_t capacity)
{
  const uint32_t hsize = cb_pow2_ceil(capacity * 4U);
  *frames              = (cb_frame_t*)calloc((size_t)capacity, sizeof(cb_frame_t));
  *idx                 = (cb_index_t){.bucket = (int32_t*)malloc((size_t)hsize * sizeof(int32_t)),
                                      .next   = (int32_t*)malloc((size_t)capacity * sizeof(int32_t)),
                                      .mask   = hsize - 1U};
  if ((*frames == nullptr) || (idx->bucket == nullptr) || (idx->next == nullptr)) {
    return false;
  }
  for (uint32_t i = 0U; i < hsize; ++i) {
    idx->bucket[i] = -1;
  }
  return true;
}

/** @brief Free everything ::cb_replay_open acquired (idempotent). */
static void cb_replay_close(cb_index_t* idx, cb_frame_t* frames)
{
  free(frames);
  free(idx->bucket);
  free(idx->next);
  *idx = (cb_index_t){};
}

int cb_replay(const cache_policy_t* pol,
              const cb_key_t*       keys,
              uint64_t              n,
              uint32_t              capacity,
              cb_result_t*          out)
{
  *out = (cb_result_t){};
  if ((pol == nullptr) || (keys == nullptr) || (capacity == 0U)) {
    return 1;
  }

  cb_frame_t* frames = nullptr;
  cb_index_t  idx    = {};
  const bool  ready  = cb_replay_open(&idx, &frames, capacity);
  cb_cache_t  cache  = {.frames = frames, .capacity = capacity, .policy_data = nullptr};
  if (!ready || ((pol->init != nullptr) && (pol->init(&cache) != 0))) {
    cb_replay_close(&idx, frames);
    return 1;
  }

  uint32_t filled = 0U;
  for (uint64_t i = 0U; i < n; ++i) {
    const cb_key_t key   = keys[i];
    const int32_t  found = cb_index_find(&idx, frames, key);
    out->accesses++;
    if (found != -1) {
      out->hits++;
      if (pol->on_access != nullptr) {
        pol->on_access(&cache, (uint32_t)found);
      }
      continue;
    }
    const uint32_t frame = cb_replay_take_frame(pol, &cache, &idx, &filled, out);
    frames[frame].key    = key;
    frames[frame].live   = true;
    cb_index_push(&idx, frame, key);
    if (pol->on_insert != nullptr) {
      pol->on_insert(&cache, frame);
    }
  }

  if (pol->deinit != nullptr) {
    pol->deinit(&cache);
  }
  cb_replay_close(&idx, frames);
  return 0;
}

/**
 * @enum cb_bench_size_t
 * @brief Swept cache capacities (in frames) used on the RAM-budget axis.
 * @details These are the seven capacity points that the benchmark sweeps over.
 *          Each is a power of two chosen to cover the expected SRAM/SDRAM
 *          budget range for the RA8D2 page cache (#147 decision record).
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_cb_size_64   = 64U,   /**< Smallest evaluated capacity (frames).  */
  k_cb_size_128  = 128U,  /**< 128-frame sweep point.                 */
  k_cb_size_256  = 256U,  /**< Mid-budget representative sweep point. */
  k_cb_size_512  = 512U,  /**< 512-frame sweep point.                 */
  k_cb_size_1024 = 1024U, /**< 1 K-frame sweep point.                 */
  k_cb_size_2048 = 2048U, /**< Largest evaluated capacity (frames).   */
} cb_bench_size_t;

/** @brief Representative mid-budget capacity used in the summary table. */
typedef enum : uint32_t {
  k_cb_mid_cap = 256U, /**< Mid-point capacity (frames) for the summary view. */
} cb_mid_cap_t;

/** @brief Full scale used when computing a hit-rate percentage (integer form). */
typedef enum : uint32_t {
  k_cb_pct_scale = 100U, /**< Divisor to convert a ratio to a percentage. */
} cb_pct_scale_t;

/** @brief Floating-point 100.0 scale factor for hit-rate percentage output. */
static const double k_cb_pct_scale_f = 100.0; /**< double 100.0 for pct maths. */

/** @brief Swept cache capacities (frames) -- the RAM-budget axis. */
static const uint32_t k_cb_sizes[] = {
  32U,
  (uint32_t)k_cb_size_64,
  (uint32_t)k_cb_size_128,
  (uint32_t)k_cb_size_256,
  (uint32_t)k_cb_size_512,
  (uint32_t)k_cb_size_1024,
  (uint32_t)k_cb_size_2048,
};

/** @brief Print the per-trace hit-rate matrix (policies x cache sizes). */
static void cb_report_trace(const cb_trace_t* tr)
{
  const uint32_t nsz = (uint32_t)(sizeof(k_cb_sizes) / sizeof(k_cb_sizes[0]));
  (void)printf("\n### %s  (%llu accesses, footprint %u pages)\n\n",
               tr->name,
               (unsigned long long)tr->n,
               tr->footprint);
  (void)printf("| policy |");
  for (uint32_t s = 0U; s < nsz; ++s) {
    (void)printf(" %u |", k_cb_sizes[s]);
  }
  (void)printf("\n|--------|");
  for (uint32_t s = 0U; s < nsz; ++s) {
    (void)printf("------|");
  }
  (void)printf("\n");
  for (uint32_t p = 0U; p < g_cb_policy_count; ++p) {
    (void)printf("| %-14s |", g_cb_policies[p]->name);
    for (uint32_t s = 0U; s < nsz; ++s) {
      cb_result_t r = {};
      (void)cb_replay(g_cb_policies[p], tr->keys, tr->n, k_cb_sizes[s], &r);
      const double hit =
        (r.accesses == 0U) ? 0.0 : (k_cb_pct_scale_f * (double)r.hits / (double)r.accesses);
      (void)printf(" %5.1f |", hit);
    }
    (void)printf("\n");
  }
}

/** @brief Print the cross-workload summary (WCET + metadata + mean hit rate). */
static void cb_report_summary(cb_trace_t* traces, uint32_t ntr)
{
  const uint32_t mid_cap = (uint32_t)k_cb_mid_cap;
  (void)printf("\n## Summary at %u frames (mean over all workloads)\n\n", mid_cap);
  (void)printf("| policy | mean hit %% | worst scan/evict | meta bytes/frame |\n");
  (void)printf("|--------|-----------:|-----------------:|-----------------:|\n");
  for (uint32_t p = 0U; p < g_cb_policy_count; ++p) {
    double   sum_hit = 0.0;
    uint32_t worst   = 0U;
    for (uint32_t t = 0U; t < ntr; ++t) {
      cb_result_t r = {};
      (void)cb_replay(g_cb_policies[p], traces[t].keys, traces[t].n, mid_cap, &r);
      sum_hit +=
        (r.accesses == 0U) ? 0.0 : (k_cb_pct_scale_f * (double)r.hits / (double)r.accesses);
      if (r.worst_scan > worst) {
        worst = r.worst_scan;
      }
    }
    (void)printf("| %-14s | %10.2f | %16u | %16zu |\n",
                 g_cb_policies[p]->name,
                 sum_hit / (double)ntr,
                 worst,
                 g_cb_policies[p]->meta_bytes);
  }
}

/** @brief Capacity of the extra captured-trace table filled from argv. */
typedef enum : uint8_t {
  k_cb_max_loaded = 8U, /**< Most `<name>=<path>` traces accepted per run. */
} cb_loaded_cap_t;

/**
 * @brief Load the extra captured traces named on the command line.
 *
 * @details Each argv of the form `<name>=<path>` (e.g. `hw-reader=t.trace`)
 *          is split in place and loaded via ::cb_trace_load; arguments
 *          without `=` are ignored (they are mode flags). Traces that fail
 *          to load (n == 0) are dropped silently, exactly as before.
 *
 * @param[in]  argc   Argument count from main.
 * @param[in]  argv   Argument vector from main (mutated at the `=` split).
 * @param[out] loaded Receives up to ::k_cb_max_loaded loaded traces.
 *
 * @return uint32_t Number of traces actually loaded.
 *
 * @pre @p loaded has capacity ::k_cb_max_loaded.
 * @pre @p argv is writable (the `=` is overwritten with a NUL).
 * @post Entries `loaded[0..return)` all have `n > 0`.
 *
 * @note Not thread-safe (mutates argv in place).
 * @since 0.1.0
 */
static uint32_t cb_load_argv_traces(int argc, char** argv, cb_trace_t* loaded)
{
  uint32_t nloaded = 0U;
  for (int a = 1; (a < argc) && (nloaded < (uint32_t)k_cb_max_loaded); ++a) {
    char* eq = strchr(argv[a], '=');
    if (eq == nullptr) {
      continue;
    }
    *eq             = '\0';
    loaded[nloaded] = cb_trace_load(eq + 1, argv[a]);
    if (loaded[nloaded].n > 0U) {
      nloaded++;
    }
  }
  return nloaded;
}

int main(int argc, char** argv)
{
  if ((argc > 1) && (strcmp(argv[1], "--sweep-block") == 0)) {
    return cb_sweep_block();
  }

  uint32_t    ntr    = 0U;
  cb_trace_t* traces = cb_traces_synthetic(&ntr);
  if ((traces == nullptr) || (ntr == 0U)) {
    (void)fprintf(stderr, "cache_bench: failed to build synthetic traces\n");
    return 1;
  }

  /* Extra captured traces: each argv is `<name>=<path>` (e.g. hw-reader=...). */
  cb_trace_t     loaded[k_cb_max_loaded] = {};
  const uint32_t nloaded                 = cb_load_argv_traces(argc, argv, loaded);

  (void)printf("# #147 eviction-policy benchmark\n");
  (void)printf("\nHit rate (%%) by cache size (frames). Higher is better.\n");
  for (uint32_t t = 0U; t < ntr; ++t) {
    cb_report_trace(&traces[t]);
  }
  for (uint32_t t = 0U; t < nloaded; ++t) {
    cb_report_trace(&loaded[t]);
  }
  cb_report_summary(traces, ntr);

  cb_traces_free(traces, ntr);
  for (uint32_t t = 0U; t < nloaded; ++t) {
    cb_trace_free(&loaded[t]);
  }
  return 0;
}
