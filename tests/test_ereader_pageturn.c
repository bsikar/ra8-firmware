/**
 * @file test_ereader_pageturn.c
 * @brief Host unit tests for the e-reader page-turn + input decisions (#78).
 *
 * @details
 * Exercises the pure decision functions in the reader's `er_pageturn.h`
 * (`er_tap_to_dir`, `er_buttons_to_dir`, `er_pageturn_step`) -- the edge-tap
 * region split, the button mapping, and the next/prev page walk with chapter
 * crossing and book-end clamping -- with MC/DC on the page-walk boundary
 * decisions. Pure logic; no MMIO / GLCDC / touch.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "unity_minimal.h"

/* The decisions live in the reader app (not a library); include the pure,
 * header-only logic directly. Relative path: tests/ -> examples/.../ereader_ui. */
#include "../examples/ek_ra8d2/hw_validated/hil/ereader_ui/er_pageturn.h"

/**
 * @enum ereader_pageturn_fixture_t
 * @brief Poison values written into out-parameters before a call, so one that fails without assigning is detectable.
 */
typedef enum : uint8_t {
  k_pageturn_poison_out =
    9U, /**< Poison in the chapter/page out-parameters; a call setting neither is detectable. */
} ereader_pageturn_fixture_t;

/**
 * @test er_tap_to_dir splits the screen into thirds (edge taps).
 *
 * @par MC/DC:
 * (no compound decisions in this test -- exercises er_tap_to_dir's third-split
 * mapping: left third = prev, middle = none, right third = next, and zero width
 * = none; every branch in er_tap_to_dir is single-condition, with no && or || on
 * the path this case touches.)
 */
static void test_tap_thirds(void)
{
  TEST_BEGIN("tap thirds: left=prev, middle=none, right=next");
  const int32_t w = 900; /* thirds at 300 / 600 */
  TEST_ASSERT_EQ(k_er_dir_prev, er_tap_to_dir(0, w));
  TEST_ASSERT_EQ(k_er_dir_prev, er_tap_to_dir(299, w));
  TEST_ASSERT_EQ(k_er_dir_none, er_tap_to_dir(300, w));
  TEST_ASSERT_EQ(k_er_dir_none, er_tap_to_dir(599, w));
  TEST_ASSERT_EQ(k_er_dir_next, er_tap_to_dir(600, w));
  TEST_ASSERT_EQ(k_er_dir_next, er_tap_to_dir(899, w));
  TEST_ASSERT_EQ(k_er_dir_none, er_tap_to_dir(100, 0)); /* zero width -> none */
  TEST_END("tap thirds: left=prev, middle=none, right=next");
}

/**
 * @test er_buttons_to_dir maps SW1=prev, SW2=next, both=prev, none=none.
 *
 * @par MC/DC:
 * (no compound decisions in this test -- exercises er_buttons_to_dir's SW1 = prev,
 * SW2 = next, both = prev (SW1 wins) and none = none mapping; er_buttons_to_dir is
 * a chain of single-condition ifs, with no && or || on the path this case touches.)
 */
static void test_buttons(void)
{
  TEST_BEGIN("buttons: sw1=prev, sw2=next, both=prev, none=none");
  TEST_ASSERT_EQ(k_er_dir_prev, er_buttons_to_dir(true, false));
  TEST_ASSERT_EQ(k_er_dir_next, er_buttons_to_dir(false, true));
  TEST_ASSERT_EQ(k_er_dir_prev, er_buttons_to_dir(true, true)); /* prev wins */
  TEST_ASSERT_EQ(k_er_dir_none, er_buttons_to_dir(false, false));
  TEST_END("buttons: sw1=prev, sw2=next, both=prev, none=none");
}

/**
 * @test er_pageturn_step within-chapter next/prev and the no-op direction.
 *
 * @par MC/DC:
 * (no compound decisions in this test -- exercises er_pageturn_step's
 * within-chapter next/prev advance and the k_er_dir_none no-op; every decision in
 * er_pageturn_step is single-condition, with no && or || on the path this case
 * touches. The step boundary branches get their branch vectors in
 * test_mcdc_boundaries.)
 */
static void test_step_within(void)
{
  TEST_BEGIN("step: within-chapter next/prev + none");
  uint32_t c = k_pageturn_poison_out;
  uint32_t p = k_pageturn_poison_out;
  bool     x = true;
  /* next within a 3-chapter spine, chapter 1 page 1 of 5 -> page 2 */
  TEST_ASSERT(er_pageturn_step(1U, 1U, 5U, 3U, k_er_dir_next, &c, &p, &x));
  TEST_ASSERT_EQ(1U, c);
  TEST_ASSERT_EQ(2U, p);
  TEST_ASSERT_EQ(false, x);
  /* prev within -> page 0 */
  TEST_ASSERT(er_pageturn_step(1U, 1U, 5U, 3U, k_er_dir_prev, &c, &p, &x));
  TEST_ASSERT_EQ(0U, p);
  TEST_ASSERT_EQ(false, x);
  /* none -> no change */
  TEST_ASSERT_EQ(false, er_pageturn_step(1U, 1U, 5U, 3U, k_er_dir_none, &c, &p, &x));
  TEST_END("step: within-chapter next/prev + none");
}

/**
 * @test er_pageturn_step crosses chapters and clamps at the book ends.
 *
 * @par MC/DC:
 * (no compound decisions in this test -- exercises er_pageturn_step's chapter
 * crossing (NEXT past the last page, PREV before page 0) and both book-end clamps;
 * every decision in er_pageturn_step is single-condition, with no && or || on the
 * path this case touches. The boundary branches get their vectors in
 * test_mcdc_boundaries.)
 */
static void test_step_cross_and_clamp(void)
{
  TEST_BEGIN("step: chapter crossing + book-end clamp");
  uint32_t c = 0U;
  uint32_t p = 0U;
  bool     x = false;
  /* NEXT at the last page of chapter 0 (3 pages) -> chapter 1 page 0, crossed */
  TEST_ASSERT(er_pageturn_step(0U, 2U, 3U, 3U, k_er_dir_next, &c, &p, &x));
  TEST_ASSERT_EQ(1U, c);
  TEST_ASSERT_EQ(0U, p);
  TEST_ASSERT_EQ(true, x);
  /* PREV at page 0 of chapter 1 -> chapter 0, last-page sentinel, crossed */
  TEST_ASSERT(er_pageturn_step(1U, 0U, 4U, 3U, k_er_dir_prev, &c, &p, &x));
  TEST_ASSERT_EQ(0U, c);
  TEST_ASSERT_EQ(k_er_page_last, p);
  TEST_ASSERT_EQ(true, x);
  /* NEXT at the last page of the last chapter -> book end, no change */
  TEST_ASSERT_EQ(false, er_pageturn_step(2U, 2U, 3U, 3U, k_er_dir_next, &c, &p, &x));
  /* PREV at page 0 of chapter 0 -> book start, no change */
  TEST_ASSERT_EQ(false, er_pageturn_step(0U, 0U, 3U, 3U, k_er_dir_prev, &c, &p, &x));
  TEST_END("step: chapter crossing + book-end clamp");
}

/**
 * @test MC/DC for the page-walk boundary decisions.
 *
 * @par MC/DC:
 * NEXT path, decision A `(page + 1) < pages` then B `(chapter + 1) < ch_count`:
 * - A=T (B unevaluated): within-chapter advance       -> page++ (V1)
 * - A=F, B=T:           chapter crossing               -> next chapter (V2)
 * - A=F, B=F:           book end                       -> no change (V3)
 * V1 vs V2 vary A (independent effect: advance vs not-advance). V2 vs V3 vary B
 * (independent effect: cross vs clamp). PREV path is symmetric with decision
 * C `page > 0` then D `chapter > 0`:
 * - C=T: within-chapter retreat (V4); C=F,D=T: cross back (V5); C=F,D=F: clamp (V6).
 * V4 vs V5 vary C; V5 vs V6 vary D. 6 vectors prove each condition's independent
 * influence on the next/prev outcome.
 */
static void test_mcdc_boundaries(void)
{
  TEST_BEGIN("step MC/DC: next/prev boundary conditions");
  uint32_t c = 0U;
  uint32_t p = 0U;
  bool     x = false;
  /* NEXT: A=T -> advance (V1) */
  TEST_ASSERT(er_pageturn_step(0U, 0U, 2U, 2U, k_er_dir_next, &c, &p, &x));
  TEST_ASSERT_EQ(1U, p);
  /* NEXT: A=F,B=T -> cross (V2) */
  TEST_ASSERT(er_pageturn_step(0U, 1U, 2U, 2U, k_er_dir_next, &c, &p, &x));
  TEST_ASSERT((c == 1U) && (p == 0U) && x);
  /* NEXT: A=F,B=F -> clamp (V3) */
  TEST_ASSERT_EQ(false, er_pageturn_step(1U, 1U, 2U, 2U, k_er_dir_next, &c, &p, &x));
  /* PREV: C=T -> retreat (V4) */
  TEST_ASSERT(er_pageturn_step(1U, 1U, 2U, 2U, k_er_dir_prev, &c, &p, &x));
  TEST_ASSERT_EQ(0U, p);
  /* PREV: C=F,D=T -> cross back (V5) */
  TEST_ASSERT(er_pageturn_step(1U, 0U, 2U, 2U, k_er_dir_prev, &c, &p, &x));
  TEST_ASSERT((c == 0U) && (p == (uint32_t)k_er_page_last) && x);
  /* PREV: C=F,D=F -> clamp (V6) */
  TEST_ASSERT_EQ(false, er_pageturn_step(0U, 0U, 2U, 2U, k_er_dir_prev, &c, &p, &x));
  TEST_END("step MC/DC: next/prev boundary conditions");
}

int main(void)
{
  test_tap_thirds();
  test_buttons();
  test_step_within();
  test_step_cross_and_clamp();
  test_mcdc_boundaries();
  return 0;
}
