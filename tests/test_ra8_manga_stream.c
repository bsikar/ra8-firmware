/**
 * @file test_ra8_manga_stream.c
 * @brief #232 manga-scale streaming gate (part 1): bounded residency + byte
 *        correctness paging a 612 MB and a 2.75 GB book through a fixed budget.
 *
 * @details
 * Drives the shared harness in `test_ra8_manga_stream.h` (the real
 * ra8_tile_cache + ra8_vmem stack over a synthetic volume-of-JOF-atlases) under
 * a manga access pattern -- a cover scroll flood, forward multi-page scroll,
 * cross-volume chapter jumps, a scroll-up back-flip, a far-boundary read, and
 * repeated cover revisits -- and asserts the two load-bearing #232 properties:
 *
 *   1. **Bounded residency.** Both caches (decoded tile cells + raw page frames)
 *      saturate at -- and never exceed -- their fixed budgets, whatever the
 *      volume size, and the 1-in-1-out eviction law holds exactly on both.
 *   2. **Byte-correct page-in.** Every band returned through the tile cache is
 *      compared to an independent re-derivation of the atlas content, and a
 *      boundary band is cross-checked against the raw page-cache bytes directly.
 *
 * The scan-resistance and eviction-split-tuning gates live in the companion
 * `test_ra8_manga_stream_policy.c`.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "test_ra8_manga_stream.h"

#include <stdint.h>

/**
 * @enum t_mg_pattern_t
 * @brief Manga access-pattern sizing for the bounded-residency workload.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_mg_fwd_pages    = 8U,   /**< Forward-scroll pages after the cover.       */
  k_mg_fwd_bands    = 16U,  /**< Bands read per forward-scroll page.         */
  k_mg_samples      = 384U, /**< Cross-volume random-seek band samples.      */
  k_mg_sample_prime = 733U, /**< Prime stride spreading samples volume-wide. */
  k_mg_backflip     = 12U,  /**< Scroll-up (back-flip) bands re-read.        */
  k_mg_cover_reread = 6U,   /**< Cover revisits after the flood.             */
} t_mg_pattern_t;

/**
 * @brief Cross-check a cached band against the raw page-cache bytes directly.
 *
 * @details The strongest oracle: read the band's raw stream straight from the
 *          volume stream (bypassing both the tile cache and the JOF decode),
 *          and require it to equal the independent reference on a pixel sample.
 *          For the raw codec the decoded payload IS the stored stream, so this
 *          proves the tile-cache path and the page-cache path agree.
 *
 * @param[in] st   Volume byte stream.
 * @param[in] page Atlas (page) index.
 * @param[in] band Band (tile row) index.
 *
 * @return Nothing (asserts on failure).
 *
 * @pre `st` is bound to the volume; `band < k_mg_bands`.
 * @pre The atlas lies within the volume.
 * @post The raw stream equalled the reference on every sampled pixel.
 * @post No pin is leaked (the stream copies through the cache).
 *
 * @note Not thread-safe.
 *
 * @par MC/DC:
 * (no compound decisions -- a single-condition pixel-sample loop over one band's
 * raw stream)
 *
 * @since 0.1.0
 */
static void t_mg_crosscheck_raw(ra8_vmem_stream_t* st, uint32_t page, uint32_t band)
{
  static uint8_t s_raw[(size_t)k_mg_payload];
  const uint64_t abs = ((uint64_t)page * (uint64_t)k_mg_atlas_size) +
                       (uint64_t)k_ra8_jof_hdr_bytes + ((uint64_t)band * (uint64_t)k_mg_payload);
  const size_t   got = ra8_vmem_stream_read(st, abs, s_raw, (size_t)k_mg_payload);
  TEST_ASSERT_EQ(k_mg_payload, got);
  for (uint32_t i = 0U; i < (uint32_t)k_mg_payload; i += (uint32_t)k_mg_pix_stride) {
    TEST_ASSERT_EQ(t_mg_payload_byte((uint64_t)page, band, i), s_raw[i]);
  }
}

/**
 * @brief Skim the first few bands of each of the next few pages.
 *
 * @details
 * Models a reader paging forward through consecutive chapters without reading
 * any page to its end -- each page is opened, its top bands are touched, and
 * the reader moves on. This is the access pattern that most stresses page
 * turnover, because no page stays current long enough for its metadata to be
 * worth keeping.
 *
 * @param[in,out] st   Stream to open pages against.
 * @param[in,out] tc   Tile cache the bands are read through.
 * @param[in,out] hw   Residency high-water marks, updated per band.
 * @param[out]    info Receives the geometry of the last page opened.
 *
 * @return Count of bands touched, for the caller's distinct-band tally.
 *
 * @pre The stack was wired by ::t_mg_setup over the same volume.
 * @pre The volume holds at least `k_mg_fwd_pages` pages past page 0.
 * @post Every touched band matched the reference.
 * @post @p hw reflects the peak residency reached during the skim.
 *
 * @note Not thread-safe.
 *
 * @par MC/DC:
 * (no compound decisions -- nested single-condition loop bounds only)
 *
 * @since 0.1.0
 */
static uint32_t t_mg_skim_forward_pages(ra8_vmem_stream_t* st,
                                        ra8_tile_cache_t*  tc,
                                        t_mg_hw_t*         hw,
                                        ra8_jof_info_t*    info)
{
  uint32_t touched = 0U;
  for (uint32_t p = 1U; p <= (uint32_t)k_mg_fwd_pages; ++p) {
    t_mg_open_page(st, p, info);
    for (uint32_t band = 0U; band < (uint32_t)k_mg_fwd_bands; ++band) {
      t_mg_touch_band(tc, p, band, hw);
      ++touched;
    }
  }
  return touched;
}

/**
 * @brief Seek to scattered bands spread across the whole volume.
 *
 * @details
 * Models chapter-jumping: a reader landing anywhere in the book with no
 * locality between one landing and the next. Successive targets are spaced by
 * a prime stride modulo the total band count, which walks the whole volume
 * without repeating and without the regularity a cache could exploit -- so
 * residency here is a fair worst case rather than a lucky sequence.
 *
 * @param[in]     atlas_count Atlases in the modelled volume; must be non-zero.
 * @param[in,out] st          Stream to open pages against.
 * @param[in,out] tc          Tile cache the bands are read through.
 * @param[in,out] hw          Residency high-water marks, updated per band.
 * @param[out]    info        Receives the geometry of the last page opened.
 *
 * @return Count of bands touched, for the caller's distinct-band tally.
 *
 * @pre The stack was wired by ::t_mg_setup over the same volume.
 * @pre `atlas_count * k_mg_bands` does not overflow uint32_t.
 * @post Every touched band matched the reference.
 * @post @p hw reflects the peak residency reached during the seeks.
 *
 * @note Not thread-safe.
 *
 * @par MC/DC:
 * (no compound decisions -- a single-condition loop bound only)
 *
 * @since 0.1.0
 */
static uint32_t t_mg_seek_across_volume(uint32_t           atlas_count,
                                        ra8_vmem_stream_t* st,
                                        ra8_tile_cache_t*  tc,
                                        t_mg_hw_t*         hw,
                                        ra8_jof_info_t*    info)
{
  const uint32_t total_bands = atlas_count * (uint32_t)k_mg_bands;
  for (uint32_t s = 0U; s < (uint32_t)k_mg_samples; ++s) {
    const uint32_t global = (s * (uint32_t)k_mg_sample_prime) % total_bands;
    const uint32_t page   = global / (uint32_t)k_mg_bands;
    const uint32_t band   = global % (uint32_t)k_mg_bands;
    t_mg_open_page(st, page, info);
    t_mg_touch_band(tc, page, band, hw);
  }
  return (uint32_t)k_mg_samples;
}

/**
 * @brief Drive the manga access pattern over @p atlas_count atlases.
 *
 * @details Cover scroll flood, forward multi-page scroll, cross-volume chapter
 *          jumps (random seeks), a scroll-up back-flip, a far-boundary read, and
 *          repeated cover revisits -- verifying every band and tracking the
 *          residency high-water. Returns the count of DISTINCT bands touched so
 *          the caller can assert oversubscription.
 *
 * @param[in]  atlas_count Atlases in the volume.
 * @param[in]  st          Volume byte stream.
 * @param[in]  tc          Tile cache.
 * @param[out] hw          Residency high-water marks.
 *
 * @return Approximate distinct-band count touched by the flood + samples.
 * @retval >0 Always (the cover flood alone touches every band of atlas 0).
 *
 * @pre The stack was wired by ::t_mg_setup over the same volume.
 * @pre `atlas_count >= 2` so cross-volume seeks are meaningful.
 * @post Every touched band matched the reference.
 * @post `hw` reflects the peak residency of the run.
 *
 * @note Not thread-safe.
 *
 * @par MC/DC:
 * (no compound decisions -- nested single-condition scroll/sample loop bounds)
 *
 * @since 0.1.0
 */
static uint32_t t_mg_run_manga_pattern(uint32_t           atlas_count,
                                       ra8_vmem_stream_t* st,
                                       ra8_tile_cache_t*  tc,
                                       t_mg_hw_t*         hw)
{
  ra8_jof_info_t info = {};

  /* 1. Cover scroll: read every band of atlas 0 top-to-bottom (the flood). */
  for (uint32_t band = 0U; band < (uint32_t)k_mg_bands; ++band) {
    t_mg_touch_band(tc, 0U, band, hw);
  }
  uint32_t distinct = (uint32_t)k_mg_bands;

  /* 2. Forward multi-page scroll: open + skim the next few chapters. */
  distinct += t_mg_skim_forward_pages(st, tc, hw, &info);

  /* 3. Cross-volume chapter jumps: random seeks across the WHOLE book. */
  distinct += t_mg_seek_across_volume(atlas_count, st, tc, hw, &info);

  /* 4. Far boundary: the last atlas's last band (deep-in-the-volume seek). */
  const uint32_t last_page = atlas_count - 1U;
  t_mg_open_page(st, last_page, &info);
  t_mg_touch_band(tc, last_page, (uint32_t)k_mg_bands - 1U, hw);
  ++distinct;

  /* 5. Scroll-up back-flip: re-read the top bands of the last forward page. */
  for (uint32_t band = 0U; band < (uint32_t)k_mg_backflip; ++band) {
    t_mg_touch_band(tc, (uint32_t)k_mg_fwd_pages, band, hw);
  }

  /* 6. Cover revisits: re-open + re-skim atlas 0 (its metadata stays hot). */
  for (uint32_t r = 0U; r < (uint32_t)k_mg_cover_reread; ++r) {
    t_mg_open_page(st, 0U, &info);
    t_mg_touch_band(tc, 0U, 0U, hw);
  }
  return distinct;
}

/**
 * @brief Run + assert the bounded-residency + correctness gate at one volume size.
 *
 * @param[in] atlas_count Atlases in the modelled volume.
 * @param[in] target      The cited target size the volume must reach (bytes).
 *
 * @return Nothing (asserts on failure).
 *
 * @pre `atlas_count * atlas_size >= target`.
 * @pre The static cache arrays are idle (previous run finished).
 * @post Residency stayed bounded by the fixed budgets throughout.
 * @post Every band matched the reference; the 1-in-1-out law held on both caches.
 *
 * @note Not thread-safe.
 *
 * @par MC/DC:
 * (no compound decisions under test -- the gate is a set of independent
 * single-condition equalities/inequalities over the cache counters, the
 * residency probes, and the reference byte compares)
 *
 * @since 0.1.0
 */
static void t_mg_gate_size(uint32_t atlas_count, uint64_t target)
{
  ra8_vsource_t     vs      = {};
  ra8_vsource_obj_t objs[1] = {};
  ra8_vmem_t        vm      = {};
  ra8_vmem_stream_t st      = {};
  ra8_tile_cache_t  tc      = {};
  t_mg_decode_ctx_t dc      = {};
  ra8_jof_info_t    info    = {};
  const uint64_t    vol     = t_mg_setup(atlas_count, 0U, &vs, objs, &vm, &st, &tc, &dc, &info);

  /* The modelled volume genuinely spans the cited manga scale. */
  TEST_ASSERT(vol >= target);

  /* A boundary band is byte-identical through the raw page-cache bytes
   * (independent of the tile cache) before the flood. */
  t_mg_crosscheck_raw(&st, atlas_count - 1U, (uint32_t)k_mg_bands - 1U);

  t_mg_hw_t      hw       = {};
  const uint32_t distinct = t_mg_run_manga_pattern(atlas_count, &st, &tc, &hw);

  /* (1) Bounded residency: both caches saturated at -- and never exceeded --
   *     their fixed budgets, whatever the volume size. */
  TEST_ASSERT_EQ(k_mg_frames, hw.pc_frames);
  TEST_ASSERT_EQ(k_mg_cells, hw.tc_cells);
  TEST_ASSERT_EQ(k_mg_frames, t_mg_pc_valid());
  TEST_ASSERT_EQ(k_mg_cells, t_mg_tc_valid());

  /* (2) 1-in-1-out on BOTH caches: every post-fill miss evicts exactly one. */
  uint32_t pc_hits = 0U;
  uint32_t pc_miss = 0U;
  uint32_t pc_evic = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vmem_stats(&vm, &pc_hits, &pc_miss, &pc_evic));
  TEST_ASSERT_EQ(pc_miss - (uint32_t)k_mg_frames, pc_evic);
  uint32_t tc_hits = 0U;
  uint32_t tc_miss = 0U;
  uint32_t tc_evic = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_tile_cache_stats(&tc, &tc_hits, &tc_miss, &tc_evic));
  TEST_ASSERT_EQ(tc_miss - (uint32_t)k_mg_cells, tc_evic);

  /* (3) The working set genuinely dwarfs the decoded budget (>= 100x). */
  TEST_ASSERT(distinct > ((uint32_t)k_mg_cells * (uint32_t)k_mg_min_oversub));

  /* The fixed resident budget is a tiny fraction of the modelled volume. */
  const uint64_t resident = ((uint64_t)k_mg_frames * (uint64_t)k_mg_frame_bytes) +
                            ((uint64_t)k_mg_cells * (uint64_t)k_mg_cell_bytes);
  TEST_ASSERT(vol > (resident * (uint64_t)k_mg_min_oversub));
}

/**
 * @test manga_stream_bounded_and_correct
 * @brief The two-tier stack streams a 612 MB and a 2.75 GB manga volume through
 *        a fixed RAM budget with byte-correct bands and bounded residency (#232).
 *
 * @par MC/DC:
 * (integration gate: the compound decisions inside the JOF reader, page cache,
 * and tile cache carry their vectors in test_ra8_jof*.c, test_ra8_vmem.c,
 * and test_ra8_keycache.c; here the oracles are bounded residency + a reference
 * byte compare, both single-condition.)
 */
static void test_manga_stream_bounded_and_correct(void)
{
  TEST_BEGIN("manga stream: 612 MB + 2.75 GB through a fixed budget");
  /* 25 atlases * ~24 MiB ~= 630 MB >= 612 MB; 110 atlases ~= 2.77 GB >= 2.75 GB. */
  const uint32_t small_atlases = (uint32_t)((k_mg_target_small / (uint64_t)k_mg_atlas_size) + 1U);
  const uint32_t big_atlases   = (uint32_t)((k_mg_target_big / (uint64_t)k_mg_atlas_size) + 1U);
  t_mg_gate_size(small_atlases, (uint64_t)k_mg_target_small);
  t_mg_gate_size(big_atlases, (uint64_t)k_mg_target_big);
  TEST_END("manga stream: 612 MB + 2.75 GB through a fixed budget");
}

/**
 * @brief Test entry point -- runs the bounded-residency + correctness gate.
 *
 * @return 0 on success; unity_minimal.h exits non-zero on first failure.
 *
 * @pre None.
 * @pre None.
 * @post The gate ran (or the process exited on first failure).
 * @post stderr carries the PASS line.
 *
 * @note Not thread-safe. No SIGALRM / timers used.
 * @since 0.1.0
 */
int32_t main(void)
{
  test_manga_stream_bounded_and_correct();
  return 0;
}
