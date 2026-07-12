/**
 * @file test_ra8_doc_window.c
 * @brief Unit tests for the DOC window-comparison API.
 *
 * @details
 * Exercises ra8_doc_set_window() and ra8_doc_window_compare() using the
 * simulated MMIO window provided by ra8_sim_mmap. Covers:
 *  - inside-band: flag set when DODSR0 < value < DODSR1
 *  - outside-band: flag set when value < DODSR0 or DODSR1 < value
 *  - boundary: value == lower or value == upper -> no flag in either mode
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
#include "ra8_sim_mmap.h"
#include "unity_minimal.h"

/**
 * @enum ra8_doc_win_test_val_t
 * @brief 16-bit threshold and sample values used across the window tests.
 *
 * @details
 * The window is [k_doc_wt_lower, k_doc_wt_upper] with a midpoint at
 * k_doc_wt_mid. k_doc_wt_below is strictly below lower; k_doc_wt_above
 * is strictly above upper. The boundary values equal the thresholds
 * exactly and must not trigger DOPCF in either mode.
 */
typedef enum : uint16_t {
  k_doc_wt_lower = 0x1000U, /**< Lower threshold written to DODSR0.             */
  k_doc_wt_upper = 0x2000U, /**< Upper threshold written to DODSR1.             */
  k_doc_wt_mid   = 0x1800U, /**< Midpoint strictly inside [lower, upper].       */
  k_doc_wt_below = 0x0500U, /**< Strictly below lower -- triggers outside-mode. */
  k_doc_wt_above = 0x2500U, /**< Strictly above upper -- triggers outside-mode. */
} ra8_doc_win_test_val_t;

/**
 * @enum ra8_doc_win_test_misc_t
 * @brief Miscellaneous test constants.
 */
typedef enum : uint8_t {
  k_doc_wt_bad_polarity = 2U, /**< One past k_ra8_doc_window_outside: invalid. */
} ra8_doc_win_test_misc_t;

/**
 * @brief Reset sim MMIO and call ra8_doc_init() before each test.
 *
 * @details
 * Zeroes the simulated peripheral RAM and initialises the DOC so all
 * tests start from a clean, well-defined state.
 *
 * @pre Host MMIO substrate is linked into the test binary.
 * @pre ra8_mstp module is safe to call without initialisation.
 * @post DOC registers are at their post-init values (DOCR=0, DODIR=0, ...).
 */
static void prep_window(void)
{
  ra8_sim_mmap_reset();
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
/* Inside-window mode */
/* -------------------------------------------------------------------------- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the happy path where
 * the midpoint value falls strictly inside the band)
 */
static void test_window_inside_mid_sets_flag(void)
{
  TEST_BEGIN("doc window inside: midpoint -> flag true");
  prep_window();

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_doc_set_window((uint16_t)k_doc_wt_lower,
                                    (uint16_t)k_doc_wt_upper,
                                    k_ra8_doc_window_inside));
  bool flag = false;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_doc_window_compare((uint16_t)k_doc_wt_mid, &flag));
  TEST_ASSERT_EQ(true, flag);
  TEST_END("doc window inside: midpoint -> flag true");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the miss path where
 * a value below the band does not set the flag in inside mode)
 */
static void test_window_inside_below_no_flag(void)
{
  TEST_BEGIN("doc window inside: value below lower -> flag false");
  prep_window();

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_doc_set_window((uint16_t)k_doc_wt_lower,
                                    (uint16_t)k_doc_wt_upper,
                                    k_ra8_doc_window_inside));
  bool flag = true;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_doc_window_compare((uint16_t)k_doc_wt_below, &flag));
  TEST_ASSERT_EQ(false, flag);
  TEST_END("doc window inside: value below lower -> flag false");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the miss path where
 * a value above the band does not set the flag in inside mode)
 */
static void test_window_inside_above_no_flag(void)
{
  TEST_BEGIN("doc window inside: value above upper -> flag false");
  prep_window();

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_doc_set_window((uint16_t)k_doc_wt_lower,
                                    (uint16_t)k_doc_wt_upper,
                                    k_ra8_doc_window_inside));
  bool flag = true;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_doc_window_compare((uint16_t)k_doc_wt_above, &flag));
  TEST_ASSERT_EQ(false, flag);
  TEST_END("doc window inside: value above upper -> flag false");
}

/* -------------------------------------------------------------------------- */
/* Outside-window mode */
/* -------------------------------------------------------------------------- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the hit path where
 * a value strictly below lower triggers the outside-window flag)
 */
static void test_window_outside_below_sets_flag(void)
{
  TEST_BEGIN("doc window outside: value below lower -> flag true");
  prep_window();

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_doc_set_window((uint16_t)k_doc_wt_lower,
                                    (uint16_t)k_doc_wt_upper,
                                    k_ra8_doc_window_outside));
  bool flag = false;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_doc_window_compare((uint16_t)k_doc_wt_below, &flag));
  TEST_ASSERT_EQ(true, flag);
  TEST_END("doc window outside: value below lower -> flag true");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the hit path where
 * a value strictly above upper triggers the outside-window flag)
 */
static void test_window_outside_above_sets_flag(void)
{
  TEST_BEGIN("doc window outside: value above upper -> flag true");
  prep_window();

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_doc_set_window((uint16_t)k_doc_wt_lower,
                                    (uint16_t)k_doc_wt_upper,
                                    k_ra8_doc_window_outside));
  bool flag = false;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_doc_window_compare((uint16_t)k_doc_wt_above, &flag));
  TEST_ASSERT_EQ(true, flag);
  TEST_END("doc window outside: value above upper -> flag true");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the miss path where
 * the midpoint value is inside the band and does not trigger outside mode)
 */
static void test_window_outside_mid_no_flag(void)
{
  TEST_BEGIN("doc window outside: midpoint -> flag false");
  prep_window();

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_doc_set_window((uint16_t)k_doc_wt_lower,
                                    (uint16_t)k_doc_wt_upper,
                                    k_ra8_doc_window_outside));
  bool flag = true;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_doc_window_compare((uint16_t)k_doc_wt_mid, &flag));
  TEST_ASSERT_EQ(false, flag);
  TEST_END("doc window outside: midpoint -> flag false");
}

/* -------------------------------------------------------------------------- */
/* Boundary values (== lower or == upper): no flag in either mode */
/* -------------------------------------------------------------------------- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the silicon boundary
 * behavior: DODIR == DODSR0 is NOT inside, so inside-mode flag stays clear)
 */
static void test_window_boundary_lower_inside_no_flag(void)
{
  TEST_BEGIN("doc window inside: value == lower -> flag false (strict <)");
  prep_window();

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_doc_set_window((uint16_t)k_doc_wt_lower,
                                    (uint16_t)k_doc_wt_upper,
                                    k_ra8_doc_window_inside));
  bool flag = true;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_doc_window_compare((uint16_t)k_doc_wt_lower, &flag));
  TEST_ASSERT_EQ(false, flag);
  TEST_END("doc window inside: value == lower -> flag false (strict <)");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the silicon boundary
 * behavior: DODIR == DODSR1 is NOT inside, so inside-mode flag stays clear)
 */
static void test_window_boundary_upper_inside_no_flag(void)
{
  TEST_BEGIN("doc window inside: value == upper -> flag false (strict <)");
  prep_window();

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_doc_set_window((uint16_t)k_doc_wt_lower,
                                    (uint16_t)k_doc_wt_upper,
                                    k_ra8_doc_window_inside));
  bool flag = true;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_doc_window_compare((uint16_t)k_doc_wt_upper, &flag));
  TEST_ASSERT_EQ(false, flag);
  TEST_END("doc window inside: value == upper -> flag false (strict <)");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the silicon boundary
 * behavior: DODIR == DODSR0 is neither < DODSR0 nor > DODSR1, so
 * outside-mode flag stays clear)
 */
static void test_window_boundary_lower_outside_no_flag(void)
{
  TEST_BEGIN("doc window outside: value == lower -> flag false (strict <)");
  prep_window();

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_doc_set_window((uint16_t)k_doc_wt_lower,
                                    (uint16_t)k_doc_wt_upper,
                                    k_ra8_doc_window_outside));
  bool flag = true;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_doc_window_compare((uint16_t)k_doc_wt_lower, &flag));
  TEST_ASSERT_EQ(false, flag);
  TEST_END("doc window outside: value == lower -> flag false (strict <)");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the silicon boundary
 * behavior: DODIR == DODSR1 is neither < DODSR0 nor > DODSR1, so
 * outside-mode flag stays clear)
 */
static void test_window_boundary_upper_outside_no_flag(void)
{
  TEST_BEGIN("doc window outside: value == upper -> flag false (strict <)");
  prep_window();

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_doc_set_window((uint16_t)k_doc_wt_lower,
                                    (uint16_t)k_doc_wt_upper,
                                    k_ra8_doc_window_outside));
  bool flag = true;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_doc_window_compare((uint16_t)k_doc_wt_upper, &flag));
  TEST_ASSERT_EQ(false, flag);
  TEST_END("doc window outside: value == upper -> flag false (strict <)");
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

/* -------------------------------------------------------------------------- */
/* Flag is cleared between calls */
/* -------------------------------------------------------------------------- */

/**
 * @par MC/DC:
 * (no compound decisions in this test -- verifies DOPCF is cleared on
 * exit so a miss after a hit returns false, not a stale true)
 */
static void test_window_flag_cleared_between_calls(void)
{
  TEST_BEGIN("doc window_compare flag cleared between calls");
  prep_window();

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_doc_set_window((uint16_t)k_doc_wt_lower,
                                    (uint16_t)k_doc_wt_upper,
                                    k_ra8_doc_window_inside));

  /* First call: hit. */
  bool flag = false;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_doc_window_compare((uint16_t)k_doc_wt_mid, &flag));
  TEST_ASSERT_EQ(true, flag);

  /* Second call: miss -- flag must be false, not stale from first call. */
  flag = true;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_doc_window_compare((uint16_t)k_doc_wt_below, &flag));
  TEST_ASSERT_EQ(false, flag);
  TEST_END("doc window_compare flag cleared between calls");
}

int32_t main(void)
{
  test_set_window_inside_programs_regs();
  test_set_window_outside_programs_regs();
  test_window_inside_mid_sets_flag();
  test_window_inside_below_no_flag();
  test_window_inside_above_no_flag();
  test_window_outside_below_sets_flag();
  test_window_outside_above_sets_flag();
  test_window_outside_mid_no_flag();
  test_window_boundary_lower_inside_no_flag();
  test_window_boundary_upper_inside_no_flag();
  test_window_boundary_lower_outside_no_flag();
  test_window_boundary_upper_outside_no_flag();
  test_set_window_lower_gt_upper_rejected();
  test_set_window_lower_eq_upper_rejected();
  test_set_window_bad_polarity_rejected();
  test_window_compare_null_flag_rejected();
  test_window_compare_not_compare_mode_rejected();
  test_window_flag_cleared_between_calls();
  (void)fprintf(stderr, "[OK ] test_ra8_doc_window.c\n");
  return 0;
}
