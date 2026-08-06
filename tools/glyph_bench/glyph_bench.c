/**
 * @file glyph_bench.c
 * @brief #147/#162/#164 glyph-cache workload sweep: drive the real
 *        ra8_glyph_atlas with a realistic text-render glyph stream and report the
 *        hit rate (and rasterisations saved) versus the cache budget.
 *
 * @details
 * HOST tool (not firmware). The Layer-3 glyph atlas earns its keep when the
 * renderer stops re-rasterising glyphs it just drew; this sweep quantifies that
 * on a realistic page-render workload so #164 can size the cell budget. It models
 * reading a book: each page draws a stream of glyphs from an English
 * letter-frequency distribution (plus spaces, capitals, punctuation, digits) at a
 * body font size with occasional headings, and page-turns re-render the same
 * working set. The stream is replayed through the ACTUAL ra8_glyph_atlas at a
 * range of budgets (cell counts); for each it reports the hit rate and the number
 * of render-on-miss calls (= rasterisations the renderer would actually perform).
 *
 * Cache hit/miss/eviction depends only on the key stream and the budget, not on
 * the bitmap content, so the render callback is a cheap deterministic stub -- the
 * measured hit rate is exact. Each cell in production is sized to the largest
 * glyph bitmap (2 * max font px squared, ~36 KiB), so the RAM cost of a budget is
 * `cells * ~36 KiB`; the table prints that so the knee is easy to read.
 *
 *
 * [Ring 7 / Tooling] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 *
 *

 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_glyph_atlas.h"
#include "ra8_keycache.h"
#include "ra8_log.h"

/** @brief Workload model dimensions (no bare literals). */
typedef enum : uint32_t {
  k_gb_pages         = 200U,  /**< Pages rendered in the session.               */
  k_gb_page_glyphs   = 1800U, /**< Glyph draws per page (a dense page).         */
  k_gb_reread_pages  = 3U,    /**< Re-render each page this many times (turns). */
  k_gb_body_px       = 24U,   /**< Body text font size.                         */
  k_gb_heading_px    = 36U,   /**< Heading font size.                           */
  k_gb_heading_pct   = 3U,    /**< % of glyphs drawn at the heading size.       */
  k_gb_cap_pct       = 8U,    /**< % of letters drawn as capitals.              */
  k_gb_punct_pct     = 6U,    /**< % of glyphs drawn as punctuation.            */
  k_gb_digit_pct     = 2U,    /**< % of glyphs drawn as digits.                 */
  k_gb_pct_base      = 100U,  /**< Percentage base.                             */
  k_gb_cell_bytes    = 64U,   /**< Bench cell size (content-irrelevant).        */
  k_gb_buckets       = 512U,  /**< Hash buckets for the atlas.                  */
  k_gb_prod_cell_kib = 36U,   /**< Production cell cost (192x192 alpha8) KiB.   */
} gb_dim_t;

/** @brief xorshift64 + fill constants. */
typedef enum : uint32_t {
  k_gb_xs_a       = 13U, /**< xorshift shift 1. */
  k_gb_xs_b       = 7U,  /**< xorshift shift 2. */
  k_gb_xs_c       = 17U, /**< xorshift shift 3. */
  k_gb_ascii_az_a = 97U, /**< ASCII 'a'.        */
  k_gb_ascii_az_z = 26U, /**< Letters in a-z.   */
  k_gb_case_delta = 32U, /**< 'a' - 'A'.        */
  k_gb_ascii_0    = 48U, /**< ASCII '0'.        */
  k_gb_digits     = 10U, /**< Digit count.      */
} gb_const_t;

/** @brief Deterministic xorshift64 seed. */
typedef enum : uint64_t {
  k_gb_seed = 0xD1B54A32D192ED03ULL, /**< Fixed seed. */
} gb_seed_t;

/** @brief Swept cache budgets (cell counts). */
static const uint32_t k_gb_budgets[] =
  {16U, 32U, 48U, 64U, 96U, 128U, 192U, 256U}; /* MAGIC-OK: swept-budget data table */

/**
 * @brief English letter relative frequencies, scaled to a per-mille-ish table
 *        indexed by letter (a..z). Sums to ::k_gb_freq_total.
 */
/* MAGIC-OK: published English letter-frequency data table, not control constants. */
static const uint16_t k_gb_letter_freq[26] = {
  817U, 150U, 278U, 425U, 1270U, 223U, 202U,
  609U, 697U, 15U,  77U,  403U,  241U, /* MAGIC-OK: freq data */
  675U, 751U, 193U, 10U,  599U,  633U, 906U,
  276U, 98U,  236U, 15U,  197U,  7U}; /* MAGIC-OK: freq data */

/** @brief Sum of ::k_gb_letter_freq (the selection range). */
typedef enum : uint32_t {
  k_gb_freq_total = 9999U, /**< Sum of the 26 letter weights above. */
} gb_freq_t;

/** @brief Log backend stubs so ra8_check's RA8_CHECK_NULL_PTR links host-side. */
void internal_ra8_log_error(const char* tag, const char* message)
{
  (void)fprintf(stderr, "[ra8_log] %s: %s\n", tag, message);
}

/** @brief Valued log backend stub (present for the linker). */
void internal_ra8_log_error_val(const char* tag, const char* message, uint32_t value)
{
  (void)fprintf(stderr, "[ra8_log] %s: %s =%u\n", tag, message, value);
}

/**
 * @brief Advance a fixed-seed xorshift64 generator and return the new state.
 *
 * @details
 * The standard xorshift64 recurrence (shifts 13/7/17) over @p s, updated in
 * place. Deterministic across runs and platforms, so a given seed always
 * produces the same glyph stream -- which is what makes the benched hit rate
 * exactly reproducible.
 *
 * @param[in,out] s The 64-bit generator state; replaced with the next state.
 *
 * @return The next 64-bit state value.
 * @retval 0 Never, for a non-zero seed: xorshift64 cannot reach 0 from a
 *           non-zero state, and ::k_gb_seed is non-zero.
 *
 * @pre @p s is non-NULL.
 * @pre @p s was seeded non-zero (::k_gb_seed).
 * @post @p s holds the advanced state.
 * @post The return value equals the new @p s.
 *
 * @note Not thread-safe; each caller owns its own state word.
 * @since 0.1.0
 */
static uint64_t gb_rng(uint64_t* s)
{
  uint64_t x = *s;
  x ^= x << (uint32_t)k_gb_xs_a;
  x ^= x >> (uint32_t)k_gb_xs_b;
  x ^= x << (uint32_t)k_gb_xs_c;
  *s = x;
  return x;
}

/**
 * @brief Draw a uniform pseudo-random integer in [0, @p span).
 *
 * @details
 * Advances ::gb_rng once and reduces modulo @p span. The modulo bias is
 * negligible for the small spans this workload uses and does not affect the
 * cache hit/miss decision being measured.
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
static uint32_t gb_below(uint64_t* s, uint32_t span)
{
  return (span == 0U) ? 0U : (uint32_t)(gb_rng(s) % (uint64_t)span);
}

/**
 * @brief Pick a lowercase letter codepoint weighted by English frequency.
 *
 * @details
 * Draws a value in [0, ::k_gb_freq_total) and walks the cumulative
 * ::k_gb_letter_freq table, so 'e'/'t'/'a' appear far more often than 'q'/'z'
 * -- the distribution that makes the glyph cache behave like real text. The
 * final fallthrough returns 'a' only if a rounding gap leaves the draw
 * unmatched.
 *
 * @param[in,out] s Generator state, advanced during selection.
 *
 * @return An ASCII lowercase codepoint in 'a'..'z'.
 * @retval 97 ('a') the frequency walk fell through (draw exceeded the table sum).
 *
 * @pre @p s is non-NULL and seeded.
 * @pre ::k_gb_letter_freq sums to ::k_gb_freq_total.
 * @post @p s is advanced.
 * @post The result is a valid lowercase-letter codepoint.
 *
 * @note Not thread-safe; shares @p s with the caller.
 * @since 0.1.0
 */
static uint32_t gb_pick_letter(uint64_t* s)
{
  uint32_t r = gb_below(s, (uint32_t)k_gb_freq_total);
  for (uint32_t i = 0U; i < (uint32_t)k_gb_ascii_az_z; ++i) {
    if (r < (uint32_t)k_gb_letter_freq[i]) {
      return (uint32_t)k_gb_ascii_az_a + i;
    }
    r -= (uint32_t)k_gb_letter_freq[i];
  }
  return (uint32_t)k_gb_ascii_az_a; /* fallthrough: 'a' */
}

/** @brief A small punctuation/space repertoire (codepoints). */
static const uint32_t k_gb_punct[] = {' ', '.', ',', ';', ':', '\'', '"', '-', '!', '?'};

/**
 * @brief Choose one codepoint for the glyph stream from the weighted mix.
 *
 * @details
 * Rolls once to split the repertoire into punctuation/space
 * (::k_gb_punct_pct), digits (::k_gb_digit_pct) and letters (the remainder);
 * for a letter it draws a frequency-weighted lowercase codepoint
 * (::gb_pick_letter) and upper-cases it with probability ::k_gb_cap_pct. The
 * distribution mirrors body text so the atlas working set matches a real page.
 *
 * @param[in,out] s Generator state, advanced during selection.
 *
 * @return The chosen glyph codepoint (letter, digit, or punctuation/space).
 * @retval 32 (space) when the punctuation roll selected the first repertoire
 *            entry, ::k_gb_punct[0].
 *
 * @pre @p s is non-NULL and seeded.
 * @pre The percentage constants sum to at most ::k_gb_pct_base.
 * @post @p s is advanced by one or more steps.
 * @post The result is a printable ASCII codepoint.
 *
 * @note Not thread-safe; shares @p s with the caller.
 * @since 0.1.0
 */
static uint32_t gb_pick_codepoint(uint64_t* s)
{
  const uint32_t roll = gb_below(s, (uint32_t)k_gb_pct_base);
  if (roll < (uint32_t)k_gb_punct_pct) {
    const uint32_t n = (uint32_t)(sizeof(k_gb_punct) / sizeof(k_gb_punct[0]));
    return k_gb_punct[gb_below(s, n)];
  }
  if (roll < (uint32_t)k_gb_punct_pct + (uint32_t)k_gb_digit_pct) {
    return (uint32_t)k_gb_ascii_0 + gb_below(s, (uint32_t)k_gb_digits);
  }
  uint32_t cp = gb_pick_letter(s);
  if (gb_below(s, (uint32_t)k_gb_pct_base) < (uint32_t)k_gb_cap_pct) {
    cp -= (uint32_t)k_gb_case_delta; /* uppercase variant */
  }
  return cp;
}

/**
 * @brief Atlas render-on-miss callback: stand in for one rasterisation.
 *
 * @details
 * The atlas invokes this on a cache miss to fill the freed cell. Hit / miss /
 * eviction depends only on the key stream and the budget, not on the bitmap
 * content, so this stub just zeroes the cell and reports a 1x1 glyph -- each
 * call therefore counts as exactly one rasterisation the renderer would have
 * performed, keeping the measured hit rate exact while the bench stays cheap.
 *
 * @param[in]  ctx        Unused render context (the bench carries no state).
 * @param[in]  key        Unused glyph key (content is irrelevant to hit rate).
 * @param[out] cell       Cell memory to fill; zeroed here.
 * @param[in]  cell_bytes Size of @p cell in bytes.
 * @param[out] out_w      Receives the glyph width (1).
 * @param[out] out_h      Receives the glyph height (1).
 *
 * @return Render status.
 * @retval k_ra8_ok Always; the stub cannot fail.
 *
 * @pre @p cell is non-NULL with at least @p cell_bytes bytes.
 * @pre @p out_w and @p out_h are non-NULL.
 * @post @p cell is zeroed and @p out_w / @p out_h are set to 1.
 * @post No global state is touched.
 *
 * @note Not thread-safe; matches the single-threaded bench driver.
 * @since 0.1.0
 */
static ra8_err_t gb_render(void*                  ctx,
                           const ra8_glyph_key_t* key,
                           uint8_t*               cell,
                           uint32_t               cell_bytes,
                           uint16_t*              out_w,
                           uint16_t*              out_h)
{
  (void)ctx;
  (void)key;
  (void)memset(cell, 0, (size_t)cell_bytes);
  *out_w = 1U;
  *out_h = 1U;
  return k_ra8_ok;
}

/** @brief One glyph access in the generated stream. */
typedef struct {
  uint32_t cp;      /**< Codepoint. */
  uint16_t size_px; /**< Font size. */
} gb_access_t;

/** @brief Build the deterministic glyph access stream. Returns count via out_n. */
RA8_NASA_RULE_3_OK /* host-only bench: glyph sequence array */
  static gb_access_t*
  gb_build_stream(uint64_t* rng, uint64_t* out_n)
{
  const uint64_t total =
    (uint64_t)k_gb_pages * (uint64_t)k_gb_reread_pages * (uint64_t)k_gb_page_glyphs;
  gb_access_t* seq = (gb_access_t*)malloc((size_t)total * sizeof(gb_access_t));
  if (seq == NULL) {
    *out_n = 0U;
    return nullptr;
  }
  uint64_t w = 0U;
  for (uint32_t p = 0U; p < (uint32_t)k_gb_pages; ++p) {
    /* Generate the page's glyph stream once, then replay it on each re-render
     * so a page-turn re-references exactly the same working set. */
    const uint64_t page_start = w;
    for (uint32_t g = 0U; g < (uint32_t)k_gb_page_glyphs; ++g) {
      const bool     heading = gb_below(rng, (uint32_t)k_gb_pct_base) < (uint32_t)k_gb_heading_pct;
      const uint16_t size_px = heading ? (uint16_t)k_gb_heading_px : (uint16_t)k_gb_body_px;
      seq[w].cp              = gb_pick_codepoint(rng);
      seq[w].size_px         = size_px;
      ++w;
    }
    for (uint32_t r = 1U; r < (uint32_t)k_gb_reread_pages; ++r) {
      (void)memcpy(&seq[w], &seq[page_start], (size_t)k_gb_page_glyphs * sizeof(gb_access_t));
      w += (uint64_t)k_gb_page_glyphs;
    }
  }
  *out_n = w;
  return seq;
}

/**
 * @brief Replay the glyph stream through an atlas of @p budget cells.
 *
 * @details
 * Allocates the atlas backing (cell memory, key/dim/meta arrays and the bucket
 * table), initialises the REAL ra8_glyph_atlas at @p budget cells, then issues
 * one get/put per access in @p seq. Misses drive ::gb_render, so the atlas's own
 * miss counter is the number of rasterisations the renderer would perform. The
 * hit rate is returned and the miss count reported via @p out_rasters; every
 * buffer is freed before returning. A backing-allocation failure yields a 0.0
 * hit rate and 0 rasterisations.
 *
 * @param[in]  seq         The generated glyph access stream.
 * @param[in]  n           Number of accesses in @p seq.
 * @param[in]  budget      Atlas capacity in cells for this run.
 * @param[out] out_rasters Receives the render-on-miss count.
 *
 * @return The cache hit rate as a percentage in [0, 100].
 * @retval 0.0 @p n was 0, or a backing allocation failed.
 *
 * @pre @p seq holds @p n valid accesses (or @p n is 0).
 * @pre @p out_rasters is non-NULL.
 * @post @p out_rasters holds the miss count (0 on allocation failure).
 * @post Every buffer allocated for the run is freed before returning.
 *
 * @note Not thread-safe; the tool is single-threaded.
 * @since 0.1.0
 */
RA8_NASA_RULE_3_OK /* host-only bench: dynamic atlas arrays */
  static double
  gb_run(const gb_access_t* seq, uint64_t n, uint32_t budget, uint32_t* out_rasters)
{
  uint8_t*             cell_mem = (uint8_t*)malloc((size_t)budget * (size_t)k_gb_cell_bytes);
  ra8_glyph_key_t*     keys     = (ra8_glyph_key_t*)calloc((size_t)budget, sizeof(ra8_glyph_key_t));
  ra8_glyph_dims_t*    dims = (ra8_glyph_dims_t*)calloc((size_t)budget, sizeof(ra8_glyph_dims_t));
  ra8_keycache_cell_t* meta =
    (ra8_keycache_cell_t*)calloc((size_t)budget, sizeof(ra8_keycache_cell_t));
  int32_t* buckets = (int32_t*)malloc((size_t)k_gb_buckets * sizeof(int32_t));
  if ((cell_mem == nullptr) || (keys == nullptr) || (dims == nullptr) || (meta == nullptr) ||
      (buckets == nullptr)) {
    free(cell_mem);
    free(keys);
    free(dims);
    free(meta);
    free(buckets);
    *out_rasters = 0U;
    return 0.0;
  }
  ra8_glyph_atlas_cfg_t cfg   = {.cell_mem     = cell_mem,
                                 .cell_bytes   = (uint32_t)k_gb_cell_bytes,
                                 .cell_count   = budget,
                                 .meta         = meta,
                                 .keys         = keys,
                                 .dims         = dims,
                                 .buckets      = buckets,
                                 .bucket_count = (uint32_t)k_gb_buckets,
                                 .render       = gb_render,
                                 .render_ctx   = nullptr};
  ra8_glyph_atlas_t     atlas = {};
  (void)ra8_glyph_atlas_init(&atlas, &cfg);
  for (uint64_t i = 0U; i < n; ++i) {
    ra8_glyph_key_t k = {};
    k.glyph_id        = seq[i].cp;
    k.size_px         = seq[i].size_px;
    ra8_glyph_t g     = {};
    if (ra8_glyph_atlas_get(&atlas, &k, &g) == k_ra8_ok) {
      (void)ra8_glyph_atlas_put(&atlas, g.bitmap);
    }
  }
  uint32_t hits = 0U;
  uint32_t miss = 0U;
  (void)ra8_glyph_atlas_stats(&atlas, &hits, &miss, nullptr);
  *out_rasters         = miss;
  const double hit_pct = (n == 0U) ? 0.0 : (100.0 * (double)hits / (double)n);
  free(cell_mem);
  free(keys);
  free(dims);
  free(meta);
  free(buckets);
  return hit_pct;
}

int main(void)
{
  uint64_t           rng = (uint64_t)k_gb_seed;
  uint64_t           n   = 0U;
  const gb_access_t* seq = gb_build_stream(&rng, &n);
  if ((seq == nullptr) || (n == 0U)) {
    (void)fprintf(stderr, "glyph_bench: failed to build the glyph stream\n");
    return 1;
  }

  (void)printf("# #147/#164 glyph-cache budget sweep\n\n");
  (void)printf("Workload: %u pages x %u re-renders x %u glyphs = %llu glyph gets\n",
               (unsigned)k_gb_pages,
               (unsigned)k_gb_reread_pages,
               (unsigned)k_gb_page_glyphs,
               (unsigned long long)n);
  (void)printf("(English letter frequencies + caps/punct/digits; body %upx, heading %upx)\n\n",
               (unsigned)k_gb_body_px,
               (unsigned)k_gb_heading_px);
  (void)printf("| cells | RAM @36KiB/cell | hit rate %% | rasterisations |\n");
  (void)printf("|------:|----------------:|-----------:|---------------:|\n");
  const uint32_t nb = (uint32_t)(sizeof(k_gb_budgets) / sizeof(k_gb_budgets[0]));
  for (uint32_t b = 0U; b < nb; ++b) {
    const uint32_t budget  = k_gb_budgets[b];
    uint32_t       rasters = 0U;
    const double   hit     = gb_run(seq, n, budget, &rasters);
    (void)printf("| %5u | %12u KiB | %10.2f | %14u |\n",
                 budget,
                 budget * (uint32_t)k_gb_prod_cell_kib,
                 hit,
                 rasters);
  }
  (void)printf("\nWithout a cache the renderer rasterises once per glyph get (%llu).\n",
               (unsigned long long)n);

  return 0;
}
