/**
 * @file test_ra8_manga_stream_policy.c
 * @brief #232 manga-scale streaming gate (part 2): SLRU scan resistance + the
 *        eviction-split tuning knob under a manga page-turn workload.
 *
 * @details
 * Drives the shared harness in `test_ra8_manga_stream.h` to prove the two
 * eviction-policy properties #232 asks for, both over the synthetic
 * volume-of-JOF-atlases streamed through the real tile-cache + page-cache stack:
 *
 *   1. **Scan resistance.** A bookmarked chapter's JOF metadata, promoted into
 *      the page cache's SLRU protected segment, survives a one-shot flood that
 *      skims a band from every OTHER chapter -- a plain LRU page cache would have
 *      thrashed it out.
 *   2. **The split is a real tuning knob.** Re-running an identical page-turn
 *      revisit workload with a manga-sized `protected_pct` versus a too-small one
 *      yields strictly fewer page-cache misses, while residency stays bounded
 *      under both.
 *
 * The bounded-residency + byte-correctness gate lives in the companion
 * `test_ra8_manga_stream.c`.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "test_ra8_manga_stream.h"

/**
 * @enum t_mg_scan_t
 * @brief Cross-chapter scan-resistance scenario sizing.
 *
 * @details A few "bookmarked" chapters' JOF metadata is promoted into the SLRU
 *          protected segment, then a large one-shot flood skims one band from
 *          each of many OTHER chapters (distinct atlases -> no re-referenced
 *          frame -> pure probationary). The warm metadata must survive.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_mg_scan_atlases = 220U, /**< Volume atlases for the scan scenario.         */
  k_mg_scan_warm    = 8U,   /**< Bookmarked chapters whose metadata is warmed. */
  k_mg_scan_flood   = 192U, /**< Other chapters skimmed one band each (flood). */
  k_mg_scan_band    = 5U,   /**< Band index skimmed per flooded chapter.       */
} t_mg_scan_t;

/**
 * @enum t_mg_tune_t
 * @brief Tuning-subtest working set + the two SLRU splits under comparison.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_mg_tune_atlases  = 40U, /**< Volume atlases for the tuning workload.        */
  k_mg_tune_ws       = 6U,  /**< Working-set atlases revisited each round.      */
  k_mg_tune_rounds   = 4U,  /**< Revisit rounds (rounds 2+ reuse metadata).     */
  k_mg_tune_bands    = 3U,  /**< Bands read per atlas per round.                */
  k_mg_tune_pct_good = 60U, /**< Manga-sized protected split (holds metadata).  */
  k_mg_tune_pct_tiny = 2U,  /**< Too-small protected split (thrashes metadata). */
} t_mg_tune_t;

/**
 * @brief Re-parse the warmed chapters and return the page-cache miss delta.
 *
 * @param[in]  vm   Page cache (for the miss counter).
 * @param[in]  st   Volume byte stream.
 * @param[out] info Scratch geometry output for the re-parses.
 *
 * @return Page-cache misses incurred re-opening chapters `0 .. k_mg_scan_warm-1`.
 * @retval 0 Every warmed chapter's metadata was still resident (all hits).
 *
 * @pre `vm`/`st` are the wired stack; `info` is writable.
 * @pre Chapters `0 .. k_mg_scan_warm-1` were parsed at least once before.
 * @post No cache state beyond the SLRU order / counters is mutated.
 * @post The delta counts only the re-parse page-ins.
 *
 * @note Not thread-safe.
 *
 * @par MC/DC:
 * (no compound decisions -- a single-condition loop re-opening each warmed page)
 *
 * @since 0.1.0
 */
static uint32_t
t_mg_reparse_warm_delta(const ra8_vmem_t* vm, ra8_vmem_stream_t* st, ra8_tileatlas_info_t* info)
{
  uint32_t before = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vmem_stats(vm, nullptr, &before, nullptr));
  for (uint32_t p = 0U; p < (uint32_t)k_mg_scan_warm; ++p) {
    t_mg_open_page(st, p, info);
  }
  uint32_t after = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vmem_stats(vm, nullptr, &after, nullptr));
  return after - before;
}

/**
 * @test manga_stream_metadata_scan_resistance
 * @brief A bookmarked chapter's JOF metadata, promoted into the page cache's
 *        SLRU protected segment, survives a one-shot flood that skims a band from
 *        every OTHER chapter (#147 scan resistance, faithful to a manga skim).
 *
 * @details Chapters `0 .. k_mg_scan_warm-1` are opened twice, promoting each
 *          atlas's header + footer frames into the protected segment. Then one
 *          band is skimmed from each of `k_mg_scan_flood` DISTINCT other
 *          chapters: every such read touches frames unique to its atlas exactly
 *          once, so nothing is re-referenced and the flood is pure probationary
 *          (thousands of frame loads through the 64-frame budget). A plain LRU
 *          page cache would have evicted the warmed metadata; the SLRU protected
 *          segment keeps it, so re-opening the bookmarked chapters pages nothing
 *          new in.
 *
 * @par MC/DC:
 * (no compound decisions under test -- single-condition warm/flood loop bounds
 * plus a zero-delta equality on the post-flood re-parse)
 */
static void test_manga_stream_metadata_scan_resistance(void)
{
  TEST_BEGIN("manga stream: bookmarked chapter metadata survives a cross-chapter flood");
  ra8_vsource_t        vs      = {};
  ra8_vsource_obj_t    objs[1] = {};
  ra8_vmem_t           vm      = {};
  ra8_vmem_stream_t    st      = {};
  ra8_tile_cache_t     tc      = {};
  t_mg_decode_ctx_t    dc      = {};
  ra8_tileatlas_info_t info    = {};
  (void)t_mg_setup((uint32_t)k_mg_scan_atlases, 0U, &vs, objs, &vm, &st, &tc, &dc, &info);
  t_mg_hw_t hw = {};

  /* Warm the bookmarked chapters' header + footer into the protected segment
   * (2nd parse of each re-references those frames -> SLRU promote). */
  for (uint32_t pass = 0U; pass < 2U; ++pass) {
    for (uint32_t p = 0U; p < (uint32_t)k_mg_scan_warm; ++p) {
      t_mg_open_page(&st, p, &info);
    }
  }
  /* The warmed metadata is resident now: an immediate re-parse pages in nothing. */
  TEST_ASSERT_EQ(0U, t_mg_reparse_warm_delta(&vm, &st, &info));

  /* One-shot cross-chapter flood: one band from each of many DISTINCT other
   * chapters. Every read touches frames unique to its atlas exactly once, so the
   * flood stays probationary and promotes nothing. */
  for (uint32_t f = 0U; f < (uint32_t)k_mg_scan_flood; ++f) {
    const uint32_t page = (uint32_t)k_mg_scan_warm + f;
    t_mg_touch_band(&tc, page, (uint32_t)k_mg_scan_band, &hw);
  }

  /* Scan resistance: the protected bookmark metadata SURVIVED the flood, so the
   * post-flood re-parse still pages nothing new in -- a plain LRU would have
   * thrashed it out after ~64 frames. */
  TEST_ASSERT_EQ(0U, t_mg_reparse_warm_delta(&vm, &st, &info));

  /* Residency stayed bounded across the whole flood. */
  TEST_ASSERT_EQ(k_mg_frames, t_mg_pc_valid());
  TEST_END("manga stream: bookmarked chapter metadata survives a cross-chapter flood");
}

/**
 * @brief Run a revisit workload and return the page-cache miss count.
 *
 * @details A working set of ::k_mg_tune_ws atlases is opened + skimmed for
 *          ::k_mg_tune_rounds rounds. Rounds 2+ re-reference each atlas's JOF
 *          metadata (header/footer/index frames), so whether those stay resident
 *          -- and thus how many misses the run costs -- depends entirely on the
 *          SLRU protected/probationary split.
 *
 * @param[in] protected_pct Page-cache split under test (0 selects the default).
 *
 * @return Total page-cache misses over the whole workload.
 * @retval >0 Always (round 1 is entirely cold).
 *
 * @pre `protected_pct <= 100`.
 * @pre The static cache arrays are idle.
 * @post Residency never exceeded the fixed frame budget.
 * @post The returned miss count is deterministic for the split.
 *
 * @note Not thread-safe.
 *
 * @par MC/DC:
 * (no compound decisions -- nested single-condition round/atlas/band loops)
 *
 * @since 0.1.0
 */
static uint32_t t_mg_tune_misses(uint8_t protected_pct)
{
  ra8_vsource_t        vs      = {};
  ra8_vsource_obj_t    objs[1] = {};
  ra8_vmem_t           vm      = {};
  ra8_vmem_stream_t    st      = {};
  ra8_tile_cache_t     tc      = {};
  t_mg_decode_ctx_t    dc      = {};
  ra8_tileatlas_info_t info    = {};
  (void)
    t_mg_setup((uint32_t)k_mg_tune_atlases, protected_pct, &vs, objs, &vm, &st, &tc, &dc, &info);
  t_mg_hw_t hw = {};

  for (uint32_t round = 0U; round < (uint32_t)k_mg_tune_rounds; ++round) {
    for (uint32_t a = 0U; a < (uint32_t)k_mg_tune_ws; ++a) {
      t_mg_open_page(&st, a, &info);
      for (uint32_t band = 0U; band < (uint32_t)k_mg_tune_bands; ++band) {
        t_mg_touch_band(&tc, a, band, &hw);
      }
    }
  }
  /* Residency stays bounded regardless of the split. */
  TEST_ASSERT_EQ(k_mg_frames, t_mg_pc_valid());
  uint32_t miss = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vmem_stats(&vm, nullptr, &miss, nullptr));
  return miss;
}

/**
 * @test manga_stream_split_tuning
 * @brief The SLRU protected/probationary split is a real tuning knob (#232): a
 *        manga-sized split beats a too-small one on the page-turn revisit
 *        workload, and residency stays bounded under both.
 *
 * @par MC/DC:
 * (no compound decisions under test -- a single strict-inequality between the
 * two deterministic miss counts, plus the bounded-residency asserts inside
 * ::t_mg_tune_misses)
 */
static void test_manga_stream_split_tuning(void)
{
  TEST_BEGIN("manga stream: page-cache split tuning has teeth");
  const uint32_t miss_good = t_mg_tune_misses((uint8_t)k_mg_tune_pct_good);
  const uint32_t miss_tiny = t_mg_tune_misses((uint8_t)k_mg_tune_pct_tiny);
  /* A protected segment sized for the metadata working set holds it across
   * rounds; a 2% split thrashes it, re-paging metadata every round. */
  TEST_ASSERT(miss_tiny > miss_good);
  TEST_END("manga stream: page-cache split tuning has teeth");
}

/**
 * @brief Test entry point -- runs the manga-scale eviction-policy gates.
 *
 * @return 0 on success; unity_minimal.h exits non-zero on first failure.
 *
 * @pre None.
 * @pre None.
 * @post The gates ran (or the process exited on first failure).
 * @post stderr carries the per-test PASS lines.
 *
 * @note Not thread-safe. No SIGALRM / timers used.
 * @since 0.1.0
 */
int32_t main(void)
{
  test_manga_stream_metadata_scan_resistance();
  test_manga_stream_split_tuning();
  (void)fprintf(stderr, "[OK  ] test_ra8_manga_stream_policy.c\n");
  return 0;
}
