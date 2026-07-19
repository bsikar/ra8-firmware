/**
 * @file test_ra8_cache_store.c
 * @brief Unit tests for ra8_cache_store over LevelX standalone + a RAM NOR sim (#201).
 *
 * @details
 * Drives the persistent key->blob store end to end on the host: put/get random
 * reads (through the `ra8_vsource` streaming shape), duplicate rejection,
 * eviction + sector reuse, pin-blocks-evict, and both recovery paths -- a clean
 * checkpoint reload and an unclean-shutdown log replay that discards a torn
 * (payload-written, header-missing) tail. LevelX runs standalone
 * (`LX_STANDALONE_ENABLE`) backed by ::lx_nor_sim_ram_init.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "lx_api.h"
#include "lx_nor_sim_ram.h"
#include "ra8_cache_store.h"
#include "ra8_cache_store_internal.h"
#include "ra8_err.h"
#include "ra8_vsource.h"
#include "unity_minimal.h"

/**
 * @enum cs_seed_t
 * @brief Payload fill seeds handed to ::t_fill, which stamps `buf[i] = seed + i`.
 *
 * @details
 * The absolute values carry no meaning. What matters is that the entries one
 * test writes are seeded differently, so a stale sector, a swapped index entry
 * or a short read surfaces as a `memcmp` mismatch instead of passing by luck.
 * One seed per (test, entry) pair.
 */
typedef enum : uint8_t {
  k_cs_seed_roundtrip            = 7U,    /**< Entry of the put/get round-trip.        */
  k_cs_seed_vsource              = 33U,   /**< Entry paged back via ra8_vsource.       */
  k_cs_seed_evict_reuse          = 9U,    /**< Entry evicted, then re-put over itself. */
  k_cs_seed_pinned               = 5U,    /**< Entry pinned while an evict is tried.   */
  k_cs_seed_checkpoint_a         = 11U,   /**< Key A across a clean close + reopen.    */
  k_cs_seed_checkpoint_b         = 22U,   /**< Key B beside it: catches a run mix-up.  */
  k_cs_seed_replay_a             = 44U,   /**< Key A committed before the crash.       */
  k_cs_seed_replay_b             = 55U,   /**< Key B committed before the crash.       */
  k_cs_seed_post_replay          = 66U,   /**< Entry put after replay: store usable.   */
  k_cs_seed_torn_tail            = 0x5AU, /**< The header-less "torn write" sector.    */
  k_cs_seed_evicted_before_crash = 12U,   /**< Evicted entry that must stay gone.      */
  k_cs_seed_survives_crash       = 34U,   /**< Entry that must outlive that crash.     */
  k_cs_seed_bad_header_scan      = 21U,   /**< The one real entry among planted junk.  */
  k_cs_seed_corrupt_super_base   = 70U,   /**< `+ variant` separates the two rounds.   */
} cs_seed_t;

/**
 * @enum cs_plant_t
 * @brief Hand-planted on-media fixtures: where a bogus header goes and what it claims.
 *
 * @details
 * Each sector below sits in the free region past the committed entries, so a
 * replay scan reaches it; each stamped field trips exactly one of the scan's
 * validation guards. Sectors are spaced two apart to keep the runs disjoint.
 */
typedef enum : uint16_t {
  k_cs_plant_sector_bad_start  = 20U,    /**< Header whose start_sector disagrees.   */
  k_cs_plant_sector_zero_count = 22U,    /**< Header claiming a zero-sector run.     */
  k_cs_plant_sector_overrun    = 24U,    /**< Header whose run overshoots the media. */
  k_cs_plant_sector_bad_crc    = 26U,    /**< Header sealed with a wrong CRC.        */
  k_cs_plant_start_mismatch    = 999U,   /**< A start_sector no header can sit at.   */
  k_cs_plant_count_past_media  = 60000U, /**< A run length past the end of media.    */
} cs_plant_t;

/**
 * @enum cs_bytes_t
 * @brief Payload sizes, chosen against the fixture's 512-byte logical sector.
 *
 * @details
 * Sizes are picked for how many sectors they occupy, not for the number
 * itself: sub-sector, two-sector and three-sector runs exercise the whole /
 * sliced / partial-tail read paths, and neighbouring entries differ in length
 * so a swapped index record cannot pass a size assertion.
 */
typedef enum : uint16_t {
  k_cs_bytes_index_filler   = 100,   /**< One sector: hits the index cap first.        */
  k_cs_bytes_sub_sector     = 300,   /**< Under one sector; also a slice length.       */
  k_cs_bytes_post_replay    = 400,   /**< Put after a replay: allocation still ok.     */
  k_cs_bytes_survives_crash = 500,   /**< Entry outliving the evict-then-crash run.    */
  k_cs_bytes_two_sector     = 600,   /**< Smallest size that makes a two-sector run.   */
  k_cs_bytes_entry_a        = 700,   /**< Key A: two sectors, so evicting frees a run. */
  k_cs_bytes_replay_b       = 900,   /**< Key B: a different run length from A.        */
  k_cs_bytes_evict_reuse    = 1000,  /**< Re-put at this exact size after an evict.    */
  k_cs_bytes_checkpoint_b   = 1300,  /**< Longer than A across the checkpoint.         */
  k_cs_bytes_three_sector   = 1500,  /**< Three sectors: cross-sector + partial tail.  */
  k_cs_bytes_budget_hog     = 24000, /**< Big enough that a few puts exhaust budget.   */
} cs_bytes_t;

/**
 * @enum cs_offset_t
 * @brief Read-path offsets, and the keys and loop bound of the budget probe.
 */
typedef enum : uint16_t {
  k_cs_tail_frame_payload_bytes = 476U,  /**< Real bytes in the last 512-byte frame
                                              of a 1500-byte entry; the rest is
                                              zero padding.                        */
  k_cs_key_budget_base          = 2000U, /**< First key of the budget-exhaustion
                                              run, clear of the index-cap keys.    */
  k_cs_budget_put_attempts      = 20U,   /**< Cap on that run: a safety bound, not
                                              an expected count -- the budget is
                                              meant to run out first.              */
} cs_offset_t;

/**
 * @enum cs_corrupt_t
 * @brief Values stamped where a valid magic or CRC belongs, to force a recovery path.
 *
 * @details
 * Each is deliberately not the value the reader expects, so the store must
 * fall back from "trust the checkpoint" to "replay the log" (or discard the
 * record) rather than accepting corrupt metadata.
 */
typedef enum : uint32_t {
  k_cs_plant_key_base      = 0xDEAD0000U, /**< Planted-header key base; + sector. */
  k_cs_super_crc_corrupt   = 0xDEADBEEFU, /**< Wrong superblock CRC.              */
  k_cs_super_magic_corrupt = 0x0BADC0DEU, /**< Wrong superblock magic.            */
  k_cs_hdr_crc_corrupt     = 0x1234ABCDU, /**< Wrong entry-header CRC.            */
} cs_corrupt_t;

/**
 * @enum cs_cfg_reject_t
 * @brief Configuration values init() must refuse.
 */
typedef enum : uint8_t {
  k_cs_overprovision_pct_rejected = 99U, /**< Above the accepted overprovision range. */
} cs_cfg_reject_t;

/**
 * @enum t_cs_const_t
 * @brief Fixture geometry for the tests.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_t_index_cap    = 16U,         /**< Index slots.                   */
  k_t_logical      = 200U,        /**< Usable LevelX logical sectors. */
  k_t_sector_bytes = 512U,        /**< LevelX logical-sector size.    */
  k_t_key_a        = 0xA1A1A1A1U, /**< Test key A.                    */
  k_t_key_b        = 0xB2B2B2B2U, /**< Test key B.                    */
  k_t_key_c        = 0xC3C3C3C3U, /**< Test key C.                    */
} t_cs_const_t;

/** @brief Shared staging sector (caller-owned). */
static uint8_t s_staging[k_t_sector_bytes];
/** @brief Shared index array (caller-owned). */
static ra8_cache_store_entry_t s_index[k_t_index_cap];

/**
 * @enum t_pool_const_t
 * @brief Size of the LevelX control-block pool.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_t_flash_pool = 40U, /**< Distinct control blocks across the whole suite. */
} t_pool_const_t;

/**
 * @var s_flash_pool
 * @brief Pool of LevelX control blocks; each open takes a fresh one.
 * @details The crash tests intentionally never close a store, so a control block
 *          must never be re-opened (that corrupts LevelX's global open list).
 *          Handing out a fresh block per open sidesteps that entirely.
 * @since 0.1.0
 */
static LX_NOR_FLASH s_flash_pool[k_t_flash_pool];
/** @brief Next free control block in ::s_flash_pool. */
static uint32_t s_flash_next;

/**
 * @brief Hand out a fresh, zeroed LevelX control block.
 * @return A control block never previously opened.
 * @pre The pool is not exhausted.
 * @pre The returned block out-lives the store built over it.
 * @post The block is zero-initialised.
 * @post The pool cursor advances by one.
 * @since 0.1.0
 */
static LX_NOR_FLASH* t_next_flash(void)
{
  TEST_ASSERT(s_flash_next < (uint32_t)k_t_flash_pool);
  LX_NOR_FLASH* f = &s_flash_pool[s_flash_next];
  s_flash_next++;
  (void)memset(f, 0, sizeof(*f));
  return f;
}

/**
 * @brief Build an init config over the fixture buffers.
 * @param[in] f      LevelX control block to bind.
 * @param[in] format True to format (wipe) before open.
 * @return A populated config.
 * @pre @p f points at caller-owned control-block storage.
 * @pre The fixture buffers are file-static and alive.
 * @post The returned config references the fixture buffers.
 * @post No global state is changed.
 * @since 0.1.0
 */
static ra8_cache_store_cfg_t t_cfg(LX_NOR_FLASH* f, bool format)
{
  return (ra8_cache_store_cfg_t){.nor_flash         = f,
                                 .nor_driver_init   = lx_nor_sim_ram_init,
                                 .name              = "cs",
                                 .index             = s_index,
                                 .staging           = s_staging,
                                 .staging_bytes     = (uint32_t)sizeof(s_staging),
                                 .logical_sectors   = (uint32_t)k_t_logical,
                                 .index_cap         = (uint16_t)k_t_index_cap,
                                 .overprovision_pct = (uint8_t)k_ra8_cache_store_overprov_pct,
                                 .format            = format};
}

/**
 * @brief Fill @p buf with a deterministic byte pattern.
 * @param[out] buf  Destination buffer.
 * @param[in]  len  Byte count.
 * @param[in]  seed Pattern seed.
 * @return Nothing.
 * @pre @p buf covers @p len bytes.
 * @pre @p len is the intended pattern length.
 * @post `buf[i] == (seed + i) & 0xFF` for every i.
 * @post No state outside @p buf changes.
 * @since 0.1.0
 */
static void t_fill(uint8_t* buf, uint32_t len, uint8_t seed)
{
  for (uint32_t i = 0U; i < len; i++) {
    buf[i] = (uint8_t)(seed + (uint8_t)i);
  }
}

/**
 * @test put_get_roundtrip
 * @brief A sealed entry streams back byte-identical, across sector boundaries.
 * @par MC/DC:
 * (no compound decisions under test -- exercises put, get, whole + sliced +
 * out-of-range reads through ra8_cache_store_read)
 */
static void test_put_get_roundtrip(void)
{
  TEST_BEGIN("put/get roundtrip");
  lx_nor_sim_ram_wipe();
  ra8_cache_store_t     st  = {};
  ra8_cache_store_cfg_t cfg = t_cfg(t_next_flash(), true);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cache_store_init(&st, &cfg));

  uint8_t data[k_cs_bytes_three_sector];
  t_fill(data, sizeof(data), k_cs_seed_roundtrip);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cache_store_put(&st, k_t_key_a, data, sizeof(data)));

  ra8_cache_store_reader_t rd = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cache_store_get(&st, k_t_key_a, &rd));
  TEST_ASSERT_EQ(1500, rd.byte_len);

  uint8_t out[k_cs_bytes_three_sector];
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cache_store_read(&rd, 0, out, sizeof(out)));
  TEST_ASSERT(memcmp(out, data, sizeof(data)) == 0);

  uint8_t slice[k_cs_bytes_sub_sector];
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cache_store_read(&rd, 400, slice, sizeof(slice)));
  TEST_ASSERT(memcmp(slice, &data[400], sizeof(slice)) == 0);

  TEST_ASSERT_EQ(k_ra8_err_out_of_range, ra8_cache_store_read(&rd, 1400, out, 200));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cache_store_close(&st));
  TEST_END("put/get roundtrip");
}

/**
 * @test vsource_integration
 * @brief A cached entry pages through ra8_vsource_add_paged / ra8_vsource_loader.
 * @par MC/DC:
 * (no compound decisions under test -- binds ra8_cache_store_read into a vsource
 * and checks a full frame plus a zero-padded tail frame)
 */
static void test_vsource_integration(void)
{
  TEST_BEGIN("vsource integration");
  lx_nor_sim_ram_wipe();
  ra8_cache_store_t     st  = {};
  ra8_cache_store_cfg_t cfg = t_cfg(t_next_flash(), true);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cache_store_init(&st, &cfg));

  uint8_t data[k_cs_bytes_three_sector];
  t_fill(data, sizeof(data), k_cs_seed_vsource);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cache_store_put(&st, k_t_key_a, data, sizeof(data)));
  ra8_cache_store_reader_t rd = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cache_store_get(&st, k_t_key_a, &rd));

  ra8_vsource_t     vs      = {};
  ra8_vsource_obj_t objs[1] = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vsource_init(&vs, objs, 1U));
  uint32_t id = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_vsource_add_paged(&vs, ra8_cache_store_read, &rd, 0U, rd.byte_len, &id));

  uint8_t frame[k_t_sector_bytes];
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vsource_loader(&vs, id, 0U, frame, sizeof(frame)));
  TEST_ASSERT(memcmp(frame, data, sizeof(frame)) == 0);

  /* Bytes 1024..1499 = 476 real bytes then zero padding. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vsource_loader(&vs, id, 1024U, frame, sizeof(frame)));
  TEST_ASSERT(memcmp(frame, &data[1024], 476U) == 0);
  for (uint32_t i = k_cs_tail_frame_payload_bytes; i < sizeof(frame); i++) {
    TEST_ASSERT_EQ(0, frame[i]);
  }
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cache_store_close(&st));
  TEST_END("vsource integration");
}

/**
 * @test put_edge_cases
 * @brief Missing key, duplicate put, and NULL/size guards behave.
 * @par MC/DC:
 * (no compound decisions under test -- validation guards are single-condition)
 */
static void test_put_edge_cases(void)
{
  TEST_BEGIN("put edge cases");
  lx_nor_sim_ram_wipe();
  ra8_cache_store_t     st  = {};
  ra8_cache_store_cfg_t cfg = t_cfg(t_next_flash(), true);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cache_store_init(&st, &cfg));

  ra8_cache_store_reader_t rd = {};
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_cache_store_get(&st, k_t_key_a, &rd));

  uint8_t data[k_cs_bytes_two_sector];
  t_fill(data, sizeof(data), 1U);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cache_store_put(&st, k_t_key_a, data, sizeof(data)));
  TEST_ASSERT_EQ(k_ra8_err_exists, ra8_cache_store_put(&st, k_t_key_a, data, sizeof(data)));

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_cache_store_put(&st, k_t_key_b, NULL, 8U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_cache_store_put(&st, k_t_key_b, data, 0U));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_cache_store_put(NULL, k_t_key_b, data, 8U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cache_store_close(&st));
  TEST_END("put edge cases");
}

/**
 * @test evict_reuse
 * @brief Eviction frees the key and its sectors for a later put.
 * @par MC/DC:
 * (no compound decisions under test -- put, evict, re-get miss, re-put)
 */
static void test_evict_reuse(void)
{
  TEST_BEGIN("evict + reuse");
  lx_nor_sim_ram_wipe();
  ra8_cache_store_t     st  = {};
  ra8_cache_store_cfg_t cfg = t_cfg(t_next_flash(), true);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cache_store_init(&st, &cfg));

  uint8_t data[k_cs_bytes_evict_reuse];
  t_fill(data, sizeof(data), k_cs_seed_evict_reuse);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cache_store_put(&st, k_t_key_a, data, sizeof(data)));
  uint32_t live_after_put = st.live_sectors;
  TEST_ASSERT(live_after_put > 0U);

  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_cache_store_evict(&st, k_t_key_b));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cache_store_evict(&st, k_t_key_a));
  TEST_ASSERT_EQ(0, st.live_sectors);

  ra8_cache_store_reader_t rd = {};
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_cache_store_get(&st, k_t_key_a, &rd));
  /* Sectors reclaimed: a fresh put of the same size fits again. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cache_store_put(&st, k_t_key_a, data, sizeof(data)));
  TEST_ASSERT_EQ(live_after_put, st.live_sectors);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cache_store_close(&st));
  TEST_END("evict + reuse");
}

/**
 * @test pin_blocks_evict
 * @brief A pinned entry refuses eviction until unpinned.
 * @par MC/DC:
 * (no compound decisions under test -- pin, evict->busy, unpin, evict->ok)
 */
static void test_pin_blocks_evict(void)
{
  TEST_BEGIN("pin blocks evict");
  lx_nor_sim_ram_wipe();
  ra8_cache_store_t     st  = {};
  ra8_cache_store_cfg_t cfg = t_cfg(t_next_flash(), true);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cache_store_init(&st, &cfg));

  uint8_t data[k_cs_bytes_sub_sector];
  t_fill(data, sizeof(data), k_cs_seed_pinned);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cache_store_put(&st, k_t_key_a, data, sizeof(data)));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cache_store_pin(&st, k_t_key_a, true));
  TEST_ASSERT_EQ(k_ra8_err_busy, ra8_cache_store_evict(&st, k_t_key_a));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cache_store_pin(&st, k_t_key_a, false));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cache_store_evict(&st, k_t_key_a));
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_cache_store_pin(&st, k_t_key_b, true));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cache_store_close(&st));
  TEST_END("pin blocks evict");
}

/**
 * @test recovery_clean
 * @brief A clean close reloads the index (and pins) from the checkpoint.
 * @par MC/DC:
 * (no compound decisions under test -- close then reopen over the same media)
 */
static void test_recovery_clean(void)
{
  TEST_BEGIN("recovery: clean checkpoint");
  lx_nor_sim_ram_wipe();
  ra8_cache_store_t     st  = {};
  ra8_cache_store_cfg_t cfg = t_cfg(t_next_flash(), true);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cache_store_init(&st, &cfg));

  uint8_t a[k_cs_bytes_entry_a];
  uint8_t b[k_cs_bytes_checkpoint_b];
  t_fill(a, sizeof(a), k_cs_seed_checkpoint_a);
  t_fill(b, sizeof(b), k_cs_seed_checkpoint_b);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cache_store_put(&st, k_t_key_a, a, sizeof(a)));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cache_store_put(&st, k_t_key_b, b, sizeof(b)));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cache_store_pin(&st, k_t_key_a, true));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cache_store_close(&st));

  /* Reboot: fresh control block over the same (persisted) backing. */
  ra8_cache_store_t     st2  = {};
  ra8_cache_store_cfg_t cfg2 = t_cfg(t_next_flash(), false);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cache_store_init(&st2, &cfg2));

  ra8_cache_store_reader_t rd = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cache_store_get(&st2, k_t_key_a, &rd));
  uint8_t out[k_cs_bytes_entry_a];
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cache_store_read(&rd, 0, out, sizeof(out)));
  TEST_ASSERT(memcmp(out, a, sizeof(a)) == 0);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cache_store_get(&st2, k_t_key_b, &rd));

  /* Pin survived the checkpoint. */
  TEST_ASSERT_EQ(k_ra8_err_busy, ra8_cache_store_evict(&st2, k_t_key_a));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cache_store_evict(&st2, k_t_key_b));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cache_store_close(&st2));
  TEST_END("recovery: clean checkpoint");
}

/**
 * @test recovery_crash_replay
 * @brief An unclean shutdown replays the log and discards a torn tail.
 * @par MC/DC:
 * (no compound decisions under test -- commit two entries, plant a
 * header-less payload, reopen without close, verify a scan recovers only the
 * committed entries)
 */
static void test_recovery_crash_replay(void)
{
  TEST_BEGIN("recovery: crash replay");
  lx_nor_sim_ram_wipe();
  ra8_cache_store_t     st  = {};
  ra8_cache_store_cfg_t cfg = t_cfg(t_next_flash(), true);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cache_store_init(&st, &cfg));

  uint8_t a[k_cs_bytes_entry_a];
  uint8_t b[k_cs_bytes_replay_b];
  t_fill(a, sizeof(a), k_cs_seed_replay_a);
  t_fill(b, sizeof(b), k_cs_seed_replay_b);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cache_store_put(&st, k_t_key_a, a, sizeof(a)));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cache_store_put(&st, k_t_key_b, b, sizeof(b)));
  uint32_t live_committed = st.live_sectors;

  /* Simulate a torn put: write a payload sector with NO header in front of it,
   * in the free region past B (header slot left blank). */
  int32_t sb = ra8_cs_index_find(&st, k_t_key_b);
  TEST_ASSERT(sb >= 0);
  uint32_t torn_header = st.index[sb].start_sector + st.index[sb].sector_count;
  uint8_t  garbage[k_t_sector_bytes];
  t_fill(garbage, sizeof(garbage), k_cs_seed_torn_tail);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cs_sector_write(&st, torn_header + 1U, garbage));

  /* Crash: no close. Reopen a fresh control block over the same backing. */
  ra8_cache_store_t     st2  = {};
  ra8_cache_store_cfg_t cfg2 = t_cfg(t_next_flash(), false);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cache_store_init(&st2, &cfg2));

  ra8_cache_store_reader_t rd = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cache_store_get(&st2, k_t_key_a, &rd));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cache_store_get(&st2, k_t_key_b, &rd));
  /* The torn tail is not indexed: only A + B are live. */
  TEST_ASSERT_EQ(live_committed, st2.live_sectors);

  /* The store is fully usable after replay. */
  uint8_t c[k_cs_bytes_post_replay];
  t_fill(c, sizeof(c), k_cs_seed_post_replay);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cache_store_put(&st2, k_t_key_c, c, sizeof(c)));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cache_store_get(&st2, k_t_key_c, &rd));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cache_store_close(&st2));
  TEST_END("recovery: crash replay");
}

/**
 * @test helper_guards
 * @brief The internal helpers reject bad arguments per contract.
 * @par MC/DC:
 * (no compound decisions under test -- each guard is a single-condition check
 * exercised directly: crc NULL/empty, index find/add NULL store + NULL index,
 * sector read/write/release NULL guards)
 */
static void test_helper_guards(void)
{
  TEST_BEGIN("helper guards");
  uint8_t buf[8];
  t_fill(buf, sizeof(buf), 1U); /* the ramp 1,2,..,8 -- any non-empty input will do */
  TEST_ASSERT_EQ(0, ra8_cs_crc32(NULL, 8U));
  TEST_ASSERT_EQ(0, ra8_cs_crc32(buf, 0U));
  TEST_ASSERT(ra8_cs_crc32(buf, 8U) != 0U);

  ra8_cache_store_t empty = {};
  TEST_ASSERT_EQ(-1, ra8_cs_index_find(NULL, 0U));
  TEST_ASSERT_EQ(-1, ra8_cs_index_find(&empty, 0U)); /* index == NULL */
  TEST_ASSERT_EQ(-1, ra8_cs_index_add(NULL, 0U, 0U, 1U, 0U, false));
  TEST_ASSERT_EQ(-1, ra8_cs_index_add(&empty, 0U, 0U, 1U, 0U, false)); /* index == NULL */

  uint8_t sect[k_t_sector_bytes];
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_cs_sector_read(NULL, 0U, sect));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_cs_sector_read(&empty, 0U, NULL));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_cs_sector_write(NULL, 0U, sect));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_cs_sector_release(NULL, 0U));
  TEST_END("helper guards");
}

/**
 * @test init_validation
 * @brief init() rejects every missing dependency / bad geometry input.
 * @par MC/DC:
 * (no compound decisions under test -- one bad field per call, each a
 * single-condition validation guard)
 */
static void test_init_validation(void)
{
  TEST_BEGIN("init validation");
  ra8_cache_store_t     st   = {};
  ra8_cache_store_cfg_t base = t_cfg(t_next_flash(), true);

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_cache_store_init(NULL, &base));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_cache_store_init(&st, NULL));

  ra8_cache_store_cfg_t c = base;
  c.nor_flash             = NULL;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_cache_store_init(&st, &c));
  c                 = base;
  c.nor_driver_init = NULL;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_cache_store_init(&st, &c));
  c       = base;
  c.index = NULL;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_cache_store_init(&st, &c));
  c         = base;
  c.staging = NULL;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_cache_store_init(&st, &c));
  c           = base;
  c.index_cap = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_cache_store_init(&st, &c));
  c               = base;
  c.staging_bytes = 8U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_cache_store_init(&st, &c));
  c                   = base;
  c.overprovision_pct = k_cs_overprovision_pct_rejected;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_cache_store_init(&st, &c));
  c                 = base;
  c.logical_sectors = 3U; /* < log_start + min */
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_cache_store_init(&st, &c));

  /* Uninitialised-store guards. */
  ra8_cache_store_reader_t rd   = {};
  uint8_t                  d[4] = {};
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_cache_store_put(&st, 1U, d, 4U));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_cache_store_get(&st, 1U, &rd));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_cache_store_evict(&st, 1U));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_cache_store_pin(&st, 1U, true));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_cache_store_sync(&st));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_cache_store_read(NULL, 0U, d, 4U));
  TEST_END("init validation");
}

/**
 * @test limits_and_sync
 * @brief Index-full and capacity-budget puts fail; sync checkpoints mid-run.
 * @par MC/DC:
 * (no compound decisions under test -- fill to the index cap then the sector
 * budget, checking each no_mem, with a sync between)
 */
static void test_limits_and_sync(void)
{
  TEST_BEGIN("limits + sync");
  lx_nor_sim_ram_wipe();
  ra8_cache_store_t     st  = {};
  ra8_cache_store_cfg_t cfg = t_cfg(t_next_flash(), true);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cache_store_init(&st, &cfg));

  uint8_t tiny[k_cs_bytes_index_filler];
  t_fill(tiny, sizeof(tiny), 3U);
  for (uint32_t i = 0U; i < (uint32_t)k_t_index_cap; i++) {
    TEST_ASSERT_EQ(k_ra8_ok, ra8_cache_store_put(&st, 1000U + i, tiny, sizeof(tiny)));
  }
  /* Index full -> no_mem. */
  TEST_ASSERT_EQ(k_ra8_err_no_mem, ra8_cache_store_put(&st, 9999U, tiny, sizeof(tiny)));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cache_store_sync(&st));

  /* Free the index, then exhaust the sector budget with big entries. */
  for (uint32_t i = 0U; i < (uint32_t)k_t_index_cap; i++) {
    TEST_ASSERT_EQ(k_ra8_ok, ra8_cache_store_evict(&st, 1000U + i));
  }
  static uint8_t s_big[k_cs_bytes_budget_hog];
  t_fill(s_big, sizeof(s_big), 4U);
  ra8_err_t rc = k_ra8_ok;
  uint32_t  n  = 0U;
  for (uint32_t i = 0U; i < k_cs_budget_put_attempts; i++) {
    rc = ra8_cache_store_put(&st, k_cs_key_budget_base + i, s_big, sizeof(s_big));
    if (rc != k_ra8_ok) {
      break;
    }
    n++;
  }
  TEST_ASSERT_EQ(k_ra8_err_no_mem, rc); /* budget (overprovisioned) exhausted */
  TEST_ASSERT(n > 0U);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cache_store_close(&st));
  TEST_END("limits + sync");
}

/**
 * @test evict_then_crash
 * @brief An entry evicted before a crash stays gone after the replay.
 * @par MC/DC:
 * (no compound decisions under test -- evict A, crash-reopen, assert A absent
 * and B present)
 */
static void test_evict_then_crash(void)
{
  TEST_BEGIN("evict then crash");
  lx_nor_sim_ram_wipe();
  ra8_cache_store_t     st  = {};
  ra8_cache_store_cfg_t cfg = t_cfg(t_next_flash(), true);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cache_store_init(&st, &cfg));

  uint8_t a[k_cs_bytes_entry_a];
  uint8_t b[k_cs_bytes_survives_crash];
  t_fill(a, sizeof(a), k_cs_seed_evicted_before_crash);
  t_fill(b, sizeof(b), k_cs_seed_survives_crash);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cache_store_put(&st, k_t_key_a, a, sizeof(a)));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cache_store_put(&st, k_t_key_b, b, sizeof(b)));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cache_store_evict(&st, k_t_key_a));

  /* Crash (no close): released sectors must not resurrect on replay. */
  ra8_cache_store_t     st2  = {};
  ra8_cache_store_cfg_t cfg2 = t_cfg(t_next_flash(), false);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cache_store_init(&st2, &cfg2));
  ra8_cache_store_reader_t rd = {};
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_cache_store_get(&st2, k_t_key_a, &rd));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cache_store_get(&st2, k_t_key_b, &rd));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cache_store_close(&st2));
  TEST_END("evict then crash");
}

/**
 * @brief Plant a hand-built superblock at sector 0 of an open store.
 * @param[in,out] st       Open store to write.
 * @param[in]     magic    Magic to stamp (valid or corrupt).
 * @param[in]     good_crc True to seal with a correct CRC, false to corrupt it.
 * @return Nothing.
 * @pre @p st is an open store with geometry set.
 * @pre `st->staging` is available (we use a local buffer instead).
 * @post Sector 0 holds the planted record.
 * @post The record is clean-marked but magic/CRC may be invalid per args.
 * @since 0.1.0
 */
static void t_plant_super(ra8_cache_store_t* st, uint32_t magic, bool good_crc)
{
  ra8_cs_super_t sb = {.magic           = magic,
                       .version         = (uint32_t)k_ra8_cs_format_version,
                       .seq             = st->next_seq,
                       .clean           = (uint32_t)k_ra8_cs_clean,
                       .entry_count     = 0U,
                       .live_sectors    = 0U,
                       .next_seq        = st->next_seq,
                       .log_start       = st->log_start,
                       .data_capacity   = st->data_capacity,
                       .logical_sectors = st->logical_sectors,
                       .crc             = 0U};
  sb.crc = good_crc ? ra8_cs_crc32((const uint8_t*)&sb, (uint32_t)(sizeof(sb) - sizeof(sb.crc)))
                    : k_cs_super_crc_corrupt;
  uint8_t buf[k_t_sector_bytes];
  (void)memset(buf, 0, sizeof(buf));
  (void)memcpy(buf, &sb, sizeof(sb));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cs_sector_write(st, 0U, buf));
}

/**
 * @test corrupt_super_replays
 * @brief A bad-magic or bad-CRC superblock falls back to a safe log replay.
 * @par MC/DC:
 * (no compound decisions under test -- plant each corrupt superblock variant,
 * reopen, and confirm the committed entry survives via a scan)
 */
static void test_corrupt_super_replays(void)
{
  TEST_BEGIN("corrupt super replays");
  for (uint32_t variant = 0U; variant < 2U; variant++) {
    lx_nor_sim_ram_wipe();
    ra8_cache_store_t     st  = {};
    ra8_cache_store_cfg_t cfg = t_cfg(t_next_flash(), true);
    TEST_ASSERT_EQ(k_ra8_ok, ra8_cache_store_init(&st, &cfg));
    uint8_t a[k_cs_bytes_two_sector];
    t_fill(a, sizeof(a), (uint8_t)(k_cs_seed_corrupt_super_base + variant));
    TEST_ASSERT_EQ(k_ra8_ok, ra8_cache_store_put(&st, k_t_key_a, a, sizeof(a)));

    if (variant == 0U) {
      t_plant_super(&st, (uint32_t)k_ra8_cs_super_magic, false); /* good magic, bad CRC */
    } else {
      t_plant_super(&st, k_cs_super_magic_corrupt, true); /* bad magic */
    }

    ra8_cache_store_t     st2  = {};
    ra8_cache_store_cfg_t cfg2 = t_cfg(t_next_flash(), false);
    TEST_ASSERT_EQ(k_ra8_ok, ra8_cache_store_init(&st2, &cfg2));
    ra8_cache_store_reader_t rd = {};
    TEST_ASSERT_EQ(k_ra8_ok, ra8_cache_store_get(&st2, k_t_key_a, &rd));
    TEST_ASSERT_EQ(k_ra8_ok, ra8_cache_store_close(&st2));
  }
  TEST_END("corrupt super replays");
}

/**
 * @brief Plant a hand-built entry header at @p at_sector of an open store.
 * @param[in,out] st         Open store to write.
 * @param[in]     at_sector  Logical sector to place the header at.
 * @param[in]     self_field Value to stamp in the header's `start_sector`.
 * @param[in]     count      Value to stamp in the header's `sector_count`.
 * @param[in]     good_crc   True to seal with a correct CRC, false to corrupt it.
 * @return Nothing.
 * @pre @p st is an open store; @p at_sector is in the log region.
 * @pre @p self_field / @p count select which validation branch fires.
 * @post @p at_sector holds the planted header record.
 * @post The record's magic is always valid (other fields vary per args).
 * @since 0.1.0
 */
static void t_plant_hdr(ra8_cache_store_t* st,
                        uint32_t           at_sector,
                        uint32_t           self_field,
                        uint16_t           count,
                        bool               good_crc)
{
  ra8_cs_entry_hdr_t h = {.magic        = (uint32_t)k_ra8_cs_entry_magic,
                          .seq          = 1U,
                          .key          = k_cs_plant_key_base + at_sector,
                          .byte_len     = 16U,
                          .start_sector = self_field,
                          .sector_count = count,
                          .flags        = 0U,
                          .hdr_crc      = 0U};
  h.hdr_crc = good_crc ? ra8_cs_crc32((const uint8_t*)&h, (uint32_t)(sizeof(h) - sizeof(h.hdr_crc)))
                       : k_cs_hdr_crc_corrupt;
  uint8_t buf[k_t_sector_bytes];
  (void)memset(buf, 0, sizeof(buf));
  (void)memcpy(buf, &h, sizeof(h));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cs_sector_write(st, at_sector, buf));
}

/**
 * @test scan_rejects_bad_headers
 * @brief The replay scan rejects every malformed header shape.
 * @par MC/DC:
 * (no compound decisions under test -- plant headers that trip each single
 * validation guard, reopen, and confirm none become entries)
 */
static void test_scan_rejects_bad_headers(void)
{
  TEST_BEGIN("scan rejects bad headers");
  lx_nor_sim_ram_wipe();
  ra8_cache_store_t     st  = {};
  ra8_cache_store_cfg_t cfg = t_cfg(t_next_flash(), true);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cache_store_init(&st, &cfg));
  uint8_t a[k_cs_bytes_two_sector];
  t_fill(a, sizeof(a), k_cs_seed_bad_header_scan);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cache_store_put(&st, k_t_key_a, a, sizeof(a)));

  t_plant_hdr(&st,
              k_cs_plant_sector_bad_start,
              k_cs_plant_start_mismatch,
              2U,
              true); /* start_sector mismatch */
  t_plant_hdr(&st,
              k_cs_plant_sector_zero_count,
              k_cs_plant_sector_zero_count,
              0U,
              true); /* sector_count == 0 */
  t_plant_hdr(&st,
              k_cs_plant_sector_overrun,
              k_cs_plant_sector_overrun,
              k_cs_plant_count_past_media,
              true); /* run runs past the media */
  t_plant_hdr(&st, k_cs_plant_sector_bad_crc, k_cs_plant_sector_bad_crc, 2U, false); /* bad CRC */

  /* Crash: leave st open (unclosed) and reopen a fresh block over the media. */
  ra8_cache_store_t     st2  = {};
  ra8_cache_store_cfg_t cfg2 = t_cfg(t_next_flash(), false);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cache_store_init(&st2, &cfg2));
  ra8_cache_store_reader_t rd = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cache_store_get(&st2, k_t_key_a, &rd));
  /* None of the planted junk became a real entry. */
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_cache_store_get(&st2, 0xDEAD0014U, &rd));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cache_store_close(&st2));
  TEST_END("scan rejects bad headers");
}

/**
 * @test write_fault_propagates
 * @brief A LevelX write failure surfaces as an error from put.
 * @par MC/DC:
 * (no compound decisions under test -- inject a write fault and assert put
 * returns the hardware error)
 */
static void test_write_fault_propagates(void)
{
  TEST_BEGIN("write fault propagates");
  lx_nor_sim_ram_wipe();
  ra8_cache_store_t     st  = {};
  ra8_cache_store_cfg_t cfg = t_cfg(t_next_flash(), true);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cache_store_init(&st, &cfg));

  uint8_t a[k_cs_bytes_sub_sector];
  t_fill(a, sizeof(a), 8U);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_cache_store_put(&st, k_t_key_a, a, sizeof(a)));

  /* Store is now dirty; the next put skips the mark-dirty write and fails on
   * the payload/header write instead. */
  lx_nor_sim_ram_fail_writes(1U);
  TEST_ASSERT_EQ(k_ra8_err_hw_init_failed, ra8_cache_store_put(&st, k_t_key_b, a, sizeof(a)));
  lx_nor_sim_ram_fail_writes(0U);
  TEST_END("write fault propagates");
}

/**
 * @brief Run every ra8_cache_store test.
 * @return 0 on success (a failing assertion exits non-zero before returning).
 * @pre The host provides stdio for the Unity-style macros.
 * @pre The RAM NOR simulator backing is available (static storage).
 * @post Every registered test has run.
 * @post A non-zero exit indicates the first failing assertion.
 * @since 0.1.0
 */
int main(void)
{
  test_put_get_roundtrip();
  test_vsource_integration();
  test_put_edge_cases();
  test_evict_reuse();
  test_pin_blocks_evict();
  test_recovery_clean();
  test_recovery_crash_replay();
  test_helper_guards();
  test_init_validation();
  test_limits_and_sync();
  test_evict_then_crash();
  test_corrupt_super_replays();
  test_scan_rejects_bad_headers();
  test_write_fault_propagates();
  (void)fprintf(stderr, "[DONE] ra8_cache_store\n");
  return 0;
}
