/**
 * @file test_ra8_doc_window.c
 * @brief Unit tests for the DOC window-comparison API.
 *
 * @details
 * Exercises ra8_doc_set_window() and ra8_doc_window_compare() using the
 * fake MMIO window provided by ra8_fake_mmap. The RAM-backed host
 * register file has no DOC comparator engine, so the flag tests stage
 * DOSR.DOPCF before the call and assert the driver's real MMIO trace
 * (DODIR trigger write, DOSR decode, DOSCR clear command); the silicon
 * inside/outside/boundary semantics are hardware behaviour proven on
 * the chip. Covers:
 *  - flag decode: staged DOPCF set -> true, staged clear -> false
 *  - trigger + clear trace: DODIR holds the operand, DOSCR holds DOPCFCL
 *  - argument validation: lower >= upper, bad polarity, null out pointer
 *  - register layout: DOCR / DODSR0 / DODSR1 programmed correctly
 *
 * No compound boolean decisions appear in the production code under test,
 * so the MC/DC commentary on each function notes that no decision vectors
 * are required.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_doc.h"
#include "ra8_doc_regs.h"
#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "unity_minimal.h"

/**
 * @enum ra8_doc_win_test_val_t
 * @brief 16-bit threshold and sample values used across the window tests.
 *
 * @details
 * The window is [k_doc_wt_lower, k_doc_wt_upper] with a midpoint at
 * k_doc_wt_mid used as the compared operand in the flag-decode tests.
 */
typedef enum : uint16_t {
  k_doc_wt_lower = 0x1000U, /**< Lower threshold written to DODSR0.       */
  k_doc_wt_upper = 0x2000U, /**< Upper threshold written to DODSR1.       */
  k_doc_wt_mid   = 0x1800U, /**< Midpoint strictly inside [lower, upper]. */
} ra8_doc_win_test_val_t;

/**
 * @enum ra8_doc_win_test_misc_t
 * @brief Miscellaneous test constants.
 */
typedef enum : uint8_t {
  k_doc_wt_bad_polarity = 2U, /**< One past k_ra8_doc_window_outside: invalid. */
} ra8_doc_win_test_misc_t;

/**
 * @brief Reset fake MMIO and call ra8_doc_init() before each test.
 *
 * @details
 * Zeroes the fake peripheral RAM and initialises the DOC so all
 * tests start from a clean, well-defined state.
 *
 * @pre Host MMIO substrate is linked into the test binary.
 * @pre ra8_mstp module is safe to call without initialisation.
 * @post DOC registers are at their post-init values (DOCR=0, DODIR=0, ...).
 */
static void prep_window(void)
{
  ra8_fake_mmap_reset();
  (void)ra8_doc_init();
}

/* -------------------------------------------------------------------------- */
/* Register layout */
/* -------------------------------------------------------------------------- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the register-write
 * happy path; no &&/|| in the production code touched here)
 */
static void test_set_window_inside_programs_regs(void)
{
  TEST_BEGIN("doc set_window inside -> DOCR/DODSR0/DODSR1");
  prep_window();

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_doc_set_window((uint16_t)k_doc_wt_lower,
                                    (uint16_t)k_doc_wt_upper,
                                    k_ra8_doc_window_inside));

  volatile r_doc_regs_t* reg = ra8_doc();
  /* OMS=00, DOBW=0, DCSEL=100b (4<<4=0x40). */
  TEST_ASSERT_EQ(0x40U, reg->DOCR);
  volatile uint16_t* dodsr0 = (volatile uint16_t*)&reg->DODSR0;
  volatile uint16_t* dodsr1 = (volatile uint16_t*)&reg->DODSR1;
  TEST_ASSERT_EQ(k_doc_wt_lower, *dodsr0);
  TEST_ASSERT_EQ(k_doc_wt_upper, *dodsr1);
  /* Stale DOPCF must be cleared on exit. */
  TEST_ASSERT_EQ(0U, (reg->DOSR & (uint8_t)k_ra8_doc_mask_dopcf));
  TEST_END("doc set_window inside -> DOCR/DODSR0/DODSR1");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the outside-polarity
 * branch; no &&/|| in the production code touched here)
 */
static void test_set_window_outside_programs_regs(void)
{
  TEST_BEGIN("doc set_window outside -> DOCR DCSEL=101b");
  prep_window();

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_doc_set_window((uint16_t)k_doc_wt_lower,
                                    (uint16_t)k_doc_wt_upper,
                                    k_ra8_doc_window_outside));

  volatile r_doc_regs_t* reg = ra8_doc();
  /* OMS=00, DOBW=0, DCSEL=101b (5<<4=0x50). */
  TEST_ASSERT_EQ(0x50U, reg->DOCR);
  TEST_END("doc set_window outside -> DOCR DCSEL=101b");
}

/* -------------------------------------------------------------------------- */
/* Flag decode against a staged DOSR (the host RAM has no comparator) */
/* -------------------------------------------------------------------------- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- drives the true leg of the
 * single-condition DOPCF decode `(DOSR & k_ra8_doc_mask_dopcf) != 0`
 * by staging DOPCF set before the call)
 */
static void test_window_compare_staged_flag_reads_true(void)
{
  TEST_BEGIN("doc window_compare: staged DOPCF set -> flag true + trace");
  prep_window();

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_doc_set_window((uint16_t)k_doc_wt_lower,
                                    (uint16_t)k_doc_wt_upper,
                                    k_ra8_doc_window_inside));

  /* Stage the flag the silicon comparator would have latched. */
  volatile r_doc_regs_t* reg = ra8_doc();
  reg->DOSR                  = (uint8_t)k_ra8_doc_mask_dopcf;

  bool flag = false;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_doc_window_compare((uint16_t)k_doc_wt_mid, &flag));
  TEST_ASSERT_EQ(true, flag);

  /* Trigger write landed in DODIR. */
  volatile uint16_t* dodir = (volatile uint16_t*)&reg->DODIR;
  TEST_ASSERT_EQ(k_doc_wt_mid, *dodir);
  /* The clear command (DOPCFCL) was written to DOSCR on exit. */
  TEST_ASSERT_EQ(k_ra8_doc_mask_dopcfcl, reg->DOSCR);
  TEST_END("doc window_compare: staged DOPCF set -> flag true + trace");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- drives the false leg of the
 * single-condition DOPCF decode by leaving DOSR clear)
 */
static void test_window_compare_staged_flag_reads_false(void)
{
  TEST_BEGIN("doc window_compare: DOSR clear -> flag false");
  prep_window();

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_doc_set_window((uint16_t)k_doc_wt_lower,
                                    (uint16_t)k_doc_wt_upper,
                                    k_ra8_doc_window_outside));

  /* DOSR left at 0: the decode must report no hit. */
  bool flag = true;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_doc_window_compare((uint16_t)k_doc_wt_mid, &flag));
  TEST_ASSERT_EQ(false, flag);
  TEST_END("doc window_compare: DOSR clear -> flag false");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- proves each call re-reads DOSR
 * rather than caching a previous result: staged set then re-staged clear)
 */
static void test_window_compare_rereads_dosr_each_call(void)
{
  TEST_BEGIN("doc window_compare: each call re-reads DOSR");
  prep_window();

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_doc_set_window((uint16_t)k_doc_wt_lower,
                                    (uint16_t)k_doc_wt_upper,
                                    k_ra8_doc_window_inside));

  volatile r_doc_regs_t* reg = ra8_doc();

  /* First call: staged hit. */
  reg->DOSR = (uint8_t)k_ra8_doc_mask_dopcf;
  bool flag = false;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_doc_window_compare((uint16_t)k_doc_wt_mid, &flag));
  TEST_ASSERT_EQ(true, flag);

  /* Second call: re-stage a miss -- the result must track the register.
   * (On silicon the DOSCR.DOPCFCL writes perform this clear; the RAM-
   * backed register file has no W1C linkage, so the test stages it.) */
  reg->DOSR = 0U;
  flag      = true;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_doc_window_compare((uint16_t)k_doc_wt_lower, &flag));
  TEST_ASSERT_EQ(false, flag);
  TEST_END("doc window_compare: each call re-reads DOSR");
}

/* -------------------------------------------------------------------------- */
/* Argument validation */
/* -------------------------------------------------------------------------- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the lower > upper
 * rejection path in ra8_doc_set_window())
 */
static void test_set_window_lower_gt_upper_rejected(void)
{
  TEST_BEGIN("doc set_window lower > upper -> k_ra8_err_invalid_arg");
  prep_window();

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_doc_set_window((uint16_t)k_doc_wt_upper,
                                    (uint16_t)k_doc_wt_lower,
                                    k_ra8_doc_window_inside));
  TEST_END("doc set_window lower > upper -> k_ra8_err_invalid_arg");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the lower == upper
 * rejection path; the hardware constraint is DODSR1 > DODSR0 strictly)
 */
static void test_set_window_lower_eq_upper_rejected(void)
{
  TEST_BEGIN("doc set_window lower == upper -> k_ra8_err_invalid_arg");
  prep_window();

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_doc_set_window((uint16_t)k_doc_wt_lower,
                                    (uint16_t)k_doc_wt_lower,
                                    k_ra8_doc_window_inside));
  TEST_END("doc set_window lower == upper -> k_ra8_err_invalid_arg");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the polarity
 * out-of-range rejection path in ra8_doc_set_window())
 */
static void test_set_window_bad_polarity_rejected(void)
{
  TEST_BEGIN("doc set_window bad polarity -> k_ra8_err_invalid_arg");
  prep_window();

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_doc_set_window((uint16_t)k_doc_wt_lower,
                                    (uint16_t)k_doc_wt_upper,
                                    (ra8_doc_window_polarity_t)k_doc_wt_bad_polarity));
  TEST_END("doc set_window bad polarity -> k_ra8_err_invalid_arg");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the null-pointer
 * rejection path in ra8_doc_window_compare())
 */
static void test_window_compare_null_flag_rejected(void)
{
  TEST_BEGIN("doc window_compare null out_flag -> k_ra8_err_null_ptr");
  prep_window();

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_doc_set_window((uint16_t)k_doc_wt_lower,
                                    (uint16_t)k_doc_wt_upper,
                                    k_ra8_doc_window_inside));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_doc_window_compare((uint16_t)k_doc_wt_mid, nullptr));
  TEST_END("doc window_compare null out_flag -> k_ra8_err_null_ptr");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the OMS-check rejection
 * when the DOC is not in compare mode; simulates calling add16/sub16
 * after set_window, which overwrites DOCR.OMS)
 */
static void test_window_compare_not_compare_mode_rejected(void)
{
  TEST_BEGIN("doc window_compare OMS!=0 -> k_ra8_err_invalid_state");
  prep_window();

  /* Force OMS to 01 (add mode) to simulate a stale add16/sub16 call. */
  volatile r_doc_regs_t* reg = ra8_doc();
  reg->DOCR                  = (uint8_t)k_ra8_doc_mode_add;

  bool flag = false;
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_doc_window_compare((uint16_t)k_doc_wt_mid, &flag));
  TEST_END("doc window_compare OMS!=0 -> k_ra8_err_invalid_state");
}

int32_t main(void)
{
  test_set_window_inside_programs_regs();
  test_set_window_outside_programs_regs();
  test_window_compare_staged_flag_reads_true();
  test_window_compare_staged_flag_reads_false();
  test_window_compare_rereads_dosr_each_call();
  test_set_window_lower_gt_upper_rejected();
  test_set_window_lower_eq_upper_rejected();
  test_set_window_bad_polarity_rejected();
  test_window_compare_null_flag_rejected();
  test_window_compare_not_compare_mode_rejected();
  return 0;
}
