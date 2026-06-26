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
 * @enum cb_hash_param_t
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

  cb_frame_t*    frames = (cb_frame_t*)calloc((size_t)capacity, sizeof(cb_frame_t));
  const uint32_t hsize  = cb_pow2_ceil(capacity * 4U);
  int32_t*       bucket = (int32_t*)malloc((size_t)hsize * sizeof(int32_t));
  int32_t*       next   = (int32_t*)malloc((size_t)capacity * sizeof(int32_t));
  if ((frames == nullptr) || (bucket == nullptr) || (next == nullptr)) {
    free(frames);
    free(bucket);
    free(next);
    return 1;
  }
  for (uint32_t i = 0U; i < hsize; ++i) {
    bucket[i] = -1;
  }
  const uint32_t mask = hsize - 1U;

  cb_cache_t cache = {.frames = frames, .capacity = capacity, .policy_data = nullptr};
  if ((pol->init != nullptr) && (pol->init(&cache) != 0)) {
    free(frames);
    free(bucket);
    free(next);
    return 1;
  }

  uint32_t filled = 0U;
  for (uint64_t i = 0U; i < n; ++i) {
    const cb_key_t key = keys[i];
    const uint32_t b   = cb_hash(key) & mask;

    int32_t found = -1;
    for (int32_t j = bucket[b]; j != -1; j = next[j]) {
      if (cb_key_eq(frames[j].key, key)) {
        found = j;
        break;
      }
    }
    out->accesses++;

    if (found != -1) {
      out->hits++;
      if (pol->on_access != nullptr) {
        pol->on_access(&cache, (uint32_t)found);
      }
      continue;
    }

    uint32_t frame;
    if (filled < capacity) {
      frame = filled++;
    } else {
      uint32_t scanned = 0U;
      frame            = pol->pick_victim(&cache, &scanned);
      out->evictions++;
      out->total_scan += scanned;
      if (scanned > out->worst_scan) {
        out->worst_scan = scanned;
      }
      /* Unlink the victim's old key from its bucket chain. */
      const uint32_t ob   = cb_hash(frames[frame].key) & mask;
      int32_t        prev = -1;
      for (int32_t j = bucket[ob]; j != -1; prev = j, j = next[j]) {
        if (j == (int32_t)frame) {
          if (prev == -1) {
            bucket[ob] = next[j];
          } else {
            next[prev] = next[j];
          }
          break;
        }
      }
    }

    frames[frame].key  = key;
    frames[frame].live = true;
    next[frame]        = bucket[b];
    bucket[b]          = (int32_t)frame;
    if (pol->on_insert != nullptr) {
      pol->on_insert(&cache, frame);
    }
  }

  if (pol->deinit != nullptr) {
    pol->deinit(&cache);
  }
  free(frames);
  free(bucket);
  free(next);
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

int main(int argc, char** argv)
{
  uint32_t    ntr    = 0U;
  cb_trace_t* traces = cb_traces_synthetic(&ntr);
  if ((traces == nullptr) || (ntr == 0U)) {
    (void)fprintf(stderr, "cache_bench: failed to build synthetic traces\n");
    return 1;
  }

  /* Extra captured traces: each argv is `<name>=<path>` (e.g. hw-reader=...). */
  cb_trace_t loaded[8] = {};
  uint32_t   nloaded   = 0U;
  for (int a = 1; (a < argc) && (nloaded < 8U); ++a) {
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
