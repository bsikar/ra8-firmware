/**
 * @file test_ra8_flash_config_cov.c
 * @brief Coverage-completion tests for ra8_flash_config.c (ARC counters).
 *
 * @details
 * ra8_flash_config.c is exercised broadly by test_ra8_flash.c, but the
 * anti-rollback-counter (ARC) read / increment paths were left
 * uncovered on the host. The reason is memory topology: the ARCCS,
 * ARC_SEC and ARC_NSEC counter pages live at ``0x02E17932`` and
 * ``0x02F27E00`` -- OUTSIDE every window the shared ``ra8_sim_mmap``
 * harness backs (its MRAM window is only ``0x02C00000 .. 0x02D00000``).
 * A real load from those addresses on the host segfaults, so the
 * counter-summation helpers were never entered.
 *
 * This file backs those two counter pages itself with a ``MAP_FIXED``
 * mmap in a module constructor (exactly the mechanism ``ra8_sim_mmap``
 * uses for the windows it does own), then drives the *real*
 * ra8_flash_config.c code with seeded register / counter contents and
 * injected error bits. No statements are split; nothing is excluded.
 *
 * A handful of legs stay out of reach on the host and are NOT chased:
 *  - ``internal_arc_to_mcntselr`` default arm -- callers validate
 *    ``counter < k_ra8_flash_arc_count`` so no out-of-range id arrives.
 *  - the ``words_per`` clamp -- ``words_per`` is only ever 16 or 2, so
 *    ``words_per > k_ra8_mram_arc_max_words`` (16) can never fire.
 *  - every ``ra8_flash_enter_pe_mode() != k_ra8_ok`` early-return -- the
 *    simulator reads back the keyed value the driver just wrote, so the
 *    PE-entry poll always succeeds on its first iteration.
 * These are genuinely undrivable on the host; the module still clears
 * the 90% bar without them, so no source line is excluded. The
 * MSUINITR-kick timeout, previously listed here, is now driven in
 * test_ra8_flash.c through the ra8_sim_mmio fault seam.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "ra8_err.h"
#include "ra8_flash.h"
#include "ra8_flash_regs.h"
#include "ra8_sim_mmap.h"
#include "unity_minimal.h"

/**
 * @enum flash_config_cov_uint32_const_t
 * @brief Named uint32_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint32_t {
  k_flash_config_cov_count_ffffffff = 0xFFFFFFFFUL,
} flash_config_cov_uint32_const_t;

/**
 * @enum ra8_arc_page_const_t
 * @brief Page bases / size for the host-backed ARC counter windows.
 *
 * @details
 * The ARC counter registers sit above the harness-owned MRAM window, so
 * this test maps their containing 4 KiB pages itself. ``0x02E17932``
 * (ARCCS) lands in page ``0x02E17000``; ``0x02F27E00`` (ARC_SEC) and
 * ``0x02F27E08`` (ARC_NSEC, 64 words) both land in page ``0x02F27000``.
 */
typedef enum : uintptr_t {
  k_arc_page_arccs = 0x02E17000UL, /**< Page holding ARCCS (0x02E17932).    */
  k_arc_page_data  = 0x02F27000UL, /**< Page holding ARC_SEC/ARC_NSEC data. */
} ra8_arc_page_const_t;

/** @brief Size, in bytes, of one backed ARC counter page. */
enum : size_t {
  k_arc_page_size = 0x1000U, /**< One host page. */
};

/**
 * @enum ra8_arc_seed_const_t
 * @brief Counter seed patterns and their expected set-bit populations.
 */
typedef enum : uint32_t {
  k_seed_sec_w0    = 0x00000007UL, /**< 3 set bits.                       */
  k_seed_sec_w1    = 0x80000000UL, /**< 1 set bit.                        */
  k_seed_sec_count = 4U,           /**< 3 + 1.                            */
  k_seed_nsec0_w0  = 0x00000003UL, /**< 2 set bits.                       */
  k_seed_nsec0_pop = 2U,           /**< popcount.                         */
  k_seed_nsec1_w0  = 0x00000001UL, /**< 1 set bit.                        */
  k_seed_nsec1_pop = 1U,           /**< popcount.                         */
  k_seed_nsec2_w0  = 0x0000000FUL, /**< 4 set bits.                       */
  k_seed_nsec2_pop = 4U,           /**< popcount.                         */
  k_seed_nsec3_w0  = 0x0000007FUL, /**< 7 set bits.                       */
  k_seed_nsec3_pop = 7U,           /**< popcount.                         */
  k_arc_single_gap = 16U,          /**< Words per slot when ARCNS=single. */
} ra8_arc_seed_const_t;

/**
 * @brief Install RAM backing for the two ARC counter pages.
 *
 * @details Runs before main via the constructor attribute, mirroring
 *          ``ra8_sim_mmap``. A failed map would segfault every ARC test,
 *          so a map failure aborts the process loudly.
 *
 * @pre The host permits MAP_FIXED at the requested low addresses.
 * @post Both ARC pages are readable/writable zeroed memory.
 * @note Host-test only.
 */
[[gnu::constructor]] static void arc_pages_install(void)
{
  void* a = mmap((void*)k_arc_page_arccs,
                 (size_t)k_arc_page_size,
                 PROT_READ | PROT_WRITE,
                 MAP_FIXED | MAP_ANONYMOUS | MAP_PRIVATE,
                 -1,
                 0);
  void* b = mmap((void*)k_arc_page_data,
                 (size_t)k_arc_page_size,
                 PROT_READ | PROT_WRITE,
                 MAP_FIXED | MAP_ANONYMOUS | MAP_PRIVATE,
                 -1,
                 0);
  if (a == MAP_FAILED || b == MAP_FAILED) {
    (void)fprintf(stderr, "arc_pages_install: MAP_FIXED failed\n");
    _exit(1);
  }
}

/**
 * @brief Zero both ARC counter pages between tests.
 *
 * @pre The ARC pages are mapped (see @ref arc_pages_install).
 * @post ARCCS and all counter words read back as zero.
 */
static void arc_pages_reset(void)
{
  (void)memset((void*)k_arc_page_arccs, 0, (size_t)k_arc_page_size);
  (void)memset((void*)k_arc_page_data, 0, (size_t)k_arc_page_size);
}

/** @brief Seed the ARCCS attribute halfword (selects single vs multiple). */
static void arc_seed_arccs(uint16_t v)
{
  *(volatile uint16_t*)(uintptr_t)k_ra8_flash_ofs_arccs_addr = v;
}

/** @brief Write one 32-bit word into the ARC_SEC counter image. */
static void arc_seed_sec_word(uint32_t idx, uint32_t v)
{
  ((volatile uint32_t*)(uintptr_t)k_ra8_flash_ofs_arc_sec_addr)[idx] = v;
}

/** @brief Write one 32-bit word into the ARC_NSEC counter image. */
static void arc_seed_nsec_word(uint32_t idx, uint32_t v)
{
  ((volatile uint32_t*)(uintptr_t)k_ra8_flash_ofs_arc_nsec_addr)[idx] = v;
}

/** @brief Build a valid ra8_flash configuration for init. */
static ra8_flash_cfg_t make_cfg(void)
{
  const ra8_flash_cfg_t cfg = {
    .mrcfreq_mhz        = 200U,
    .mrefreq_mhz        = 100U,
    .prefetch_en        = true,
    .ecc_encoder_enable = true,
    .ecc_decoder_enable = true,
  };
  return cfg;
}

/** @brief Reset simulator MMIO + ARC pages and re-init the driver. */
static void fresh_init(void)
{
  ra8_sim_mmap_reset();
  arc_pages_reset();
  const ra8_flash_cfg_t cfg = make_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_flash_init(&cfg));
}

/* ---------------------------------------------------------------------------
 * ARC read: SEC path (internal_arc_read_locked SEC branch + popcount).
 * ------------------------------------------------------------------------ */

/**
 * @test test_arc_read_sec_counts_bits
 *
 * @par MC/DC:
 * No compound (&&/||) decision on this path. ``ra8_flash_arc_read`` takes
 * the non-OEMBL return, ``internal_arc_read_locked`` selects the
 * ``id == k_ra8_flash_arc_sec`` else-if arm (a single condition), and the
 * SEC summation loop walks the two ARC_SEC words. Seeded 3 + 1 set bits
 * across the two words -> expected count 4.
 */
static void test_arc_read_sec_counts_bits(void)
{
  TEST_BEGIN("flash arc_read SEC counts bits");
  fresh_init();

  arc_seed_sec_word(0U, (uint32_t)k_seed_sec_w0);
  arc_seed_sec_word(1U, (uint32_t)k_seed_sec_w1);

  uint32_t count = k_flash_config_cov_count_ffffffff;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_flash_arc_read(k_ra8_flash_arc_sec, &count));
  TEST_ASSERT_EQ(k_seed_sec_count, count);
  TEST_END("flash arc_read SEC counts bits");
}

/* ---------------------------------------------------------------------------
 * ARC read: NSEC path, single mode (internal_arc_nsec_count, 16 words/slot).
 * ------------------------------------------------------------------------ */

/**
 * @test test_arc_read_nsec_all_slots_single
 *
 * @par MC/DC:
 * No compound decision. Exercises all four ``k_ra8_flash_arc_nsec_*``
 * ids so ``internal_arc_to_mcntselr`` visits each NSEC case and
 * ``internal_arc_nsec_count`` walks each slot. With ARCNS=single each
 * slot is 16 words apart; the seeded first word of each slot fixes the
 * expected populations 2 / 1 / 4 / 7.
 */
static void test_arc_read_nsec_all_slots_single(void)
{
  TEST_BEGIN("flash arc_read NSEC all slots (single)");
  fresh_init();

  arc_seed_arccs((uint16_t)k_ra8_arc_arcns_single);
  arc_seed_nsec_word(0U * (uint32_t)k_arc_single_gap, (uint32_t)k_seed_nsec0_w0);
  arc_seed_nsec_word(1U * (uint32_t)k_arc_single_gap, (uint32_t)k_seed_nsec1_w0);
  arc_seed_nsec_word(2U * (uint32_t)k_arc_single_gap, (uint32_t)k_seed_nsec2_w0);
  arc_seed_nsec_word(3U * (uint32_t)k_arc_single_gap, (uint32_t)k_seed_nsec3_w0);

  uint32_t c0 = 0U;
  uint32_t c1 = 0U;
  uint32_t c2 = 0U;
  uint32_t c3 = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_flash_arc_read(k_ra8_flash_arc_nsec_0, &c0));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_flash_arc_read(k_ra8_flash_arc_nsec_1, &c1));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_flash_arc_read(k_ra8_flash_arc_nsec_2, &c2));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_flash_arc_read(k_ra8_flash_arc_nsec_3, &c3));
  TEST_ASSERT_EQ(k_seed_nsec0_pop, c0);
  TEST_ASSERT_EQ(k_seed_nsec1_pop, c1);
  TEST_ASSERT_EQ(k_seed_nsec2_pop, c2);
  TEST_ASSERT_EQ(k_seed_nsec3_pop, c3);
  TEST_END("flash arc_read NSEC all slots (single)");
}

/* ---------------------------------------------------------------------------
 * ARC read: NSEC path, multiple mode (words_per = 2).
 * ------------------------------------------------------------------------ */

/**
 * @test test_arc_read_nsec_multiple_mode
 *
 * @par MC/DC:
 * No compound decision. ARCNS != single drives the ``words_per = 2``
 * ternary arm inside ``internal_arc_nsec_count`` and the
 * ``id == k_ra8_flash_arc_nsec_1`` slot offset. Seeds slot 1 (words 2..3)
 * with 1 set bit -> expected count 1.
 */
static void test_arc_read_nsec_multiple_mode(void)
{
  TEST_BEGIN("flash arc_read NSEC multiple mode");
  fresh_init();

  arc_seed_arccs(0U); /* ARCNS != single -> 2 words per slot. */
  /* Slot 1 begins at word index words_per (== 2). */
  arc_seed_nsec_word(2U, (uint32_t)k_seed_nsec1_w0);

  uint32_t c1 = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_flash_arc_read(k_ra8_flash_arc_nsec_1, &c1));
  TEST_ASSERT_EQ(k_seed_nsec1_pop, c1);
  TEST_END("flash arc_read NSEC multiple mode");
}

/* ---------------------------------------------------------------------------
 * ARC increment: SEC success (internal_arc_max_count SEC leg).
 * ------------------------------------------------------------------------ */

/**
 * @test test_arc_increment_sec_ok
 *
 * @par MC/DC:
 * No compound decision. Seeds ARC_SEC to zero so the read yields count 0;
 * ``internal_arc_max_count`` returns the SEC maximum (64) via its
 * ``id == k_ra8_flash_arc_sec`` single-condition guard, ``0 + 1 <= 64``
 * proceeds to the increment command, and pre-staged MRDY + clear MASTAT
 * let ``internal_arc_cmd`` report success.
 */
static void test_arc_increment_sec_ok(void)
{
  TEST_BEGIN("flash arc_increment SEC ok");
  fresh_init();

  /* ARC_SEC already zeroed by arc_pages_reset -> current count 0. */
  *ra8_mram_reg32((uint16_t)k_ra8_mram_off_mstatr) = (uint32_t)k_ra8_mstatr_mask_mrdy;
  *ra8_mram_reg8((uint16_t)k_ra8_mram_off_mastat)  = 0U;

  TEST_ASSERT_EQ(k_ra8_ok, ra8_flash_arc_increment(k_ra8_flash_arc_sec));
  TEST_END("flash arc_increment SEC ok");
}

/* ---------------------------------------------------------------------------
 * ARC increment: SEC with command lock (internal_arc_cmd error return).
 * ------------------------------------------------------------------------ */

/**
 * @test test_arc_increment_sec_cmdlk
 *
 * @par MC/DC:
 * No compound decision. Same SEC increment flow as above, but MASTAT is
 * pre-staged with CMDLK set so ``internal_arc_cmd`` observes the latched
 * command lock after MRDY and returns ``k_ra8_err_hw_error``, which the
 * increment wrapper propagates.
 */
static void test_arc_increment_sec_cmdlk(void)
{
  TEST_BEGIN("flash arc_increment SEC cmdlk");
  fresh_init();

  *ra8_mram_reg32((uint16_t)k_ra8_mram_off_mstatr) = (uint32_t)k_ra8_mstatr_mask_mrdy;
  *ra8_mram_reg8((uint16_t)k_ra8_mram_off_mastat)  = (uint8_t)k_ra8_mastat_mask_cmdlk;

  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_flash_arc_increment(k_ra8_flash_arc_sec));
  TEST_END("flash arc_increment SEC cmdlk");
}

/* ---------------------------------------------------------------------------
 * ARC increment: NSEC max_count single vs multiple legs.
 * ------------------------------------------------------------------------ */

/**
 * @test test_arc_increment_nsec_single_max
 *
 * @par MC/DC:
 * No compound decision. ``internal_arc_max_count`` falls past the SEC and
 * OEMBL single-condition guards into the NSEC branch, reads ARCCS, and
 * (ARCNS=single) returns ``k_ra8_arc_nsec_single`` (256). Count 0 + 1
 * <= 256 proceeds; staged MRDY + clear MASTAT report success.
 */
static void test_arc_increment_nsec_single_max(void)
{
  TEST_BEGIN("flash arc_increment NSEC single max");
  fresh_init();

  arc_seed_arccs((uint16_t)k_ra8_arc_arcns_single);
  *ra8_mram_reg32((uint16_t)k_ra8_mram_off_mstatr) = (uint32_t)k_ra8_mstatr_mask_mrdy;
  *ra8_mram_reg8((uint16_t)k_ra8_mram_off_mastat)  = 0U;

  TEST_ASSERT_EQ(k_ra8_ok, ra8_flash_arc_increment(k_ra8_flash_arc_nsec_0));
  TEST_END("flash arc_increment NSEC single max");
}

/**
 * @test test_arc_increment_nsec_multiple_max
 *
 * @par MC/DC:
 * No compound decision. ARCNS != single makes ``internal_arc_max_count``
 * return the multiple-counter maximum ``k_ra8_arc_nsec_multiple`` (64) via
 * its final else. Count 0 + 1 <= 64 proceeds; staged MRDY + clear MASTAT
 * report success.
 */
static void test_arc_increment_nsec_multiple_max(void)
{
  TEST_BEGIN("flash arc_increment NSEC multiple max");
  fresh_init();

  arc_seed_arccs(0U); /* ARCNS != single -> multiple-counter max. */
  *ra8_mram_reg32((uint16_t)k_ra8_mram_off_mstatr) = (uint32_t)k_ra8_mstatr_mask_mrdy;
  *ra8_mram_reg8((uint16_t)k_ra8_mram_off_mastat)  = 0U;

  TEST_ASSERT_EQ(k_ra8_ok, ra8_flash_arc_increment(k_ra8_flash_arc_nsec_1));
  TEST_END("flash arc_increment NSEC multiple max");
}

/* ---------------------------------------------------------------------------
 * set_startup_area permanent path: config-set write reports a HW error.
 * ------------------------------------------------------------------------ */

/**
 * @test test_set_startup_area_permanent_hw_error
 *
 * @par MC/DC:
 * The compound decision on this path is ``in_ofs`` /
 * ``!in_ofs && !in_extra`` inside ``ra8_flash_config_set_write``; the
 * startup config-set target ``k_ra8_msaddr_config_set_startup`` sits in
 * the OFS window, so ``in_ofs`` is T,T and the rejection is skipped
 * (F,F) -- already an MC/DC vector covered by test_ra8_flash.c. This test
 * adds no new condition vector; it drives the post-MACI error leg by
 * pre-staging MSTATR with MRDY plus an error bit so config_set_write
 * returns ``k_ra8_err_hw_error`` and ``ra8_flash_set_startup_area``
 * propagates it after exiting P/E mode.
 */
static void test_set_startup_area_permanent_hw_error(void)
{
  TEST_BEGIN("flash set_startup_area permanent hw error");
  ra8_sim_mmap_reset();
  arc_pages_reset();

  /* MRDY high so the MACI wait passes, plus a composite error bit so the
   * MSTATR error check trips. */
  *ra8_mram_reg32((uint16_t)k_ra8_mram_off_mstatr) =
    (uint32_t)((uint32_t)k_ra8_mstatr_mask_mrdy | (uint32_t)k_ra8_mstatr_mask_any_err);

  TEST_ASSERT_EQ(k_ra8_err_hw_error,
                 ra8_flash_set_startup_area(k_ra8_flash_startup_default, false));
  TEST_END("flash set_startup_area permanent hw error");
}

/* ---------------------------------------------------------------------------
 * extra_mram_write: region overruns the extra-MRAM end.
 * ------------------------------------------------------------------------ */

/**
 * @test test_extra_mram_write_end_past_extra
 *
 * @par MC/DC:
 * No compound decision reached uniquely here. The start address is at or
 * above ``k_ra8_flash_extra_start`` (so the earlier guard passes), but
 * ``end_excl = mram_addr + len`` overruns
 * ``k_ra8_flash_extra_start + k_ra8_flash_extra_size``, driving the
 * end-of-region rejection to ``k_ra8_err_invalid_arg``. Complements the
 * pre-existing below-start and past-start rejections in test_ra8_flash.c.
 */
static void test_extra_mram_write_end_past_extra(void)
{
  TEST_BEGIN("flash extra_mram_write end past extra");
  ra8_sim_mmap_reset();

  const uint8_t  buf[k_ra8_mram_write_size_bytes] = {};
  const uint32_t near_end =
    (uint32_t)k_ra8_flash_extra_start + (uint32_t)k_ra8_flash_extra_size - 16U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_flash_extra_mram_write(near_end, buf, (uint32_t)k_ra8_mram_write_size_bytes));
  TEST_END("flash extra_mram_write end past extra");
}

/**
 * @brief Run every ra8_flash_config coverage-completion test.
 *
 * @return 0 always (a failing assertion aborts the process).
 * @post Every registered test has executed once.
 */
int32_t main(void)
{
  test_arc_read_sec_counts_bits();
  test_arc_read_nsec_all_slots_single();
  test_arc_read_nsec_multiple_mode();
  test_arc_increment_sec_ok();
  test_arc_increment_sec_cmdlk();
  test_arc_increment_nsec_single_max();
  test_arc_increment_nsec_multiple_max();
  test_set_startup_area_permanent_hw_error();
  test_extra_mram_write_end_past_extra();

  (void)fprintf(stderr, "[OK  ] test_ra8_flash_config_cov.c\n");
  return 0;
}
