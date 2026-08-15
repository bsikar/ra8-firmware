/**
 * @file test_ra8_ui.c
 * @brief Unit tests for the ra8_ui interaction core (hit-test, nav, paging).
 *
 * @details
 * Pure host tests -- ra8_ui is allocation-free plain-data logic, so no
 * register fake is needed. Covers the functional contract of every
 * entry point plus MC/DC vector sets for the three compound decisions:
 * ``ra8_ui_rect_contains`` (4-condition AND), ``ra8_ui_pager_next`` and
 * ``ra8_ui_pager_goto`` (2-condition ANDs).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_ui.h"
#include "unity_minimal.h"

/**
 * @enum ui_test_const_t
 * @brief Test-only screen ids and geometry.
 */
typedef enum : uint16_t {
  k_ui_scr_library = 1U,   /**< Ui scr library. */
  k_ui_scr_reading = 2U,   /**< Ui scr reading. */
  k_ui_scr_toc     = 3U,   /**< Ui scr TOC.     */
  k_ui_act_open    = 100U, /**< Ui act open.    */
  k_ui_act_back    = 101U, /**< Ui act back.    */
  k_ui_rect_x      = 10U,  /**< Ui rect x.      */
  k_ui_rect_y      = 20U,  /**< Ui rect y.      */
  k_ui_rect_w      = 30U,  /**< Ui rect w.      */
  k_ui_rect_h      = 40U,  /**< Ui rect h.      */
} ui_test_const_t;

/* ===========================================================================
 * Hit-testing
 * ===========================================================================
 */

/**
 * @brief Functional: containment edges (inclusive TL, exclusive BR).
 * @details Checks both included origins, both excluded far edges, and null input.
 * @pre The fixed rectangle has positive width and height.
 * @pre The tested coordinates remain representable by `int32_t`.
 * @post Top-left containment and bottom-right exclusion are proven.
 * @post The rectangle fixture remains unchanged.
 * @note Compound-condition independence is supplied by the next vector set.
 * @since 0.1.0
 *
 * @par MC/DC:
 * (functional edge test; the compound decision itself is covered by
 * test_mcdc_rect_contains)
 */
RA8_INTERNAL static void internal_test_rect_contains_edges(void)
{
  TEST_BEGIN("ra8_ui rect_contains edges");
  const ra8_ui_rect_t r = {(int32_t)k_ui_rect_x,
                           (int32_t)k_ui_rect_y,
                           (int32_t)k_ui_rect_w,
                           (int32_t)k_ui_rect_h};
  TEST_ASSERT(ra8_ui_rect_contains(&r, (int32_t)k_ui_rect_x, (int32_t)k_ui_rect_y));
  TEST_ASSERT(
    !ra8_ui_rect_contains(&r, (int32_t)(k_ui_rect_x + k_ui_rect_w), (int32_t)k_ui_rect_y));
  TEST_ASSERT(
    !ra8_ui_rect_contains(&r, (int32_t)k_ui_rect_x, (int32_t)(k_ui_rect_y + k_ui_rect_h)));
  TEST_ASSERT(!ra8_ui_rect_contains(nullptr, 0, 0));
  TEST_END("ra8_ui rect_contains edges");
}

/**
 * @brief Prove every rectangle-containment predicate is independently decisive.
 * @details Supplies the canonical all-true control plus one false arm per edge.
 * @test test_mcdc_rect_contains
 * @pre The fixed rectangle describes `[10,40) x [20,60)`.
 * @pre All five fixture points remain within `int32_t` range.
 * @post The control is contained and each edge-violating point is rejected.
 * @post The rectangle fixture remains unchanged.
 * @note This is the minimal N+1 set for the four-term short-circuit AND.
 * @since 0.1.0
 *
 * @par MC/DC:
 * Decision: `(px >= x) && (px < x+w) && (py >= y) && (py < y+h)`
 * (4 conditions, libs/ra8_ui/src/ra8_ui.c@ra8_ui_rect_contains)
 * Standard: DO-178C Table A-7 obj 5. Rect = {x10,y20,w30,h40} -> X in
 * [10,40), Y in [20,60). N+1 = 5 vectors for N=4 short-circuit AND:
 * - V1: (25,40) C1=T C2=T C3=T C4=T -> Decision T (baseline)
 * - V2: ( 5,40) C1=F                -> Decision F (C1 flips vs V1)
 * - V3: (45,40) C1=T C2=F           -> Decision F (C2 flips vs V1)
 * - V4: (25,10) C1=T C2=T C3=F      -> Decision F (C3 flips vs V1)
 * - V5: (25,70) C1=T C2=T C3=T C4=F -> Decision F (C4 flips vs V1)
 */
RA8_INTERNAL static void internal_test_mcdc_rect_contains(void)
{
  TEST_BEGIN("ra8_ui rect_contains MC/DC: 4-condition AND");
  const ra8_ui_rect_t r = {(int32_t)k_ui_rect_x,
                           (int32_t)k_ui_rect_y,
                           (int32_t)k_ui_rect_w,
                           (int32_t)k_ui_rect_h};
  TEST_ASSERT(ra8_ui_rect_contains(&r, 25, 40));  /* V1: all T */
  TEST_ASSERT(!ra8_ui_rect_contains(&r, 5, 40));  /* V2: C1 F  */
  TEST_ASSERT(!ra8_ui_rect_contains(&r, 45, 40)); /* V3: C2 F  */
  TEST_ASSERT(!ra8_ui_rect_contains(&r, 25, 10)); /* V4: C3 F  */
  TEST_ASSERT(!ra8_ui_rect_contains(&r, 25, 70)); /* V5: C4 F  */
  TEST_END("ra8_ui rect_contains MC/DC: 4-condition AND");
}

/**
 * @brief Functional: hit_test first-match, miss, and NULL guards.
 * @details Exercises the second target, no target, empty input, and each pointer guard.
 * @pre The two fixed target rectangles are nonoverlapping.
 * @pre Action and hit outputs are writable for valid vectors.
 * @post Matching returns the expected action and misses clear the hit flag.
 * @post Every null-guard vector returns the documented error.
 * @note A zero count deliberately permits a null target pointer.
 * @since 0.1.0
 *
 * @par MC/DC:
 * (functional test; rect_contains decision covered by
 * test_mcdc_rect_contains)
 */
RA8_INTERNAL static void internal_test_hit_test_basic(void)
{
  TEST_BEGIN("ra8_ui hit_test basic");
  const ra8_ui_target_t targets[2] = {
    {{0, 0, 50, 50}, (uint16_t)k_ui_act_open, 0U},
    {{50, 0, 50, 50}, (uint16_t)k_ui_act_back, 0U},
  };
  uint16_t action = 0U;
  bool     hit    = false;

  TEST_ASSERT_EQ(k_ra8_ok, ra8_ui_hit_test(targets, 2U, 60, 25, &action, &hit));
  TEST_ASSERT(hit);
  TEST_ASSERT_EQ(k_ui_act_back, action);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_ui_hit_test(targets, 2U, 200, 200, &action, &hit));
  TEST_ASSERT(!hit);

  /* count == 0 with NULL targets is allowed and is always a miss. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ui_hit_test(nullptr, 0U, 0, 0, &action, &hit));
  TEST_ASSERT(!hit);

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_ui_hit_test(targets, 2U, 0, 0, nullptr, &hit));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_ui_hit_test(targets, 2U, 0, 0, &action, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_ui_hit_test(nullptr, 2U, 0, 0, &action, &hit));
  TEST_END("ra8_ui hit_test basic");
}

/* ===========================================================================
 * Navigation
 * ===========================================================================
 */

/**
 * @brief Functional: push/pop/replace/top + overflow + underflow.
 * @details Drives one navigation stack through its complete bounded lifecycle.
 * @pre The published navigation depth is greater than one.
 * @pre The local top outputs are writable.
 * @post LIFO, replace, root retention, overflow, and null guards are proven.
 * @post Every stack object remains within its fixed capacity.
 * @note The root screen cannot be popped by contract.
 * @since 0.1.0
 *
 * @par MC/DC:
 * (functional test; the guards are single-condition comparisons, not
 * compound decisions)
 */
RA8_INTERNAL static void internal_test_nav_stack(void)
{
  TEST_BEGIN("ra8_ui nav stack");
  ra8_ui_nav_t nav;
  uint16_t     top = 0U;

  TEST_ASSERT_EQ(k_ra8_ok, ra8_ui_nav_init(&nav, (uint16_t)k_ui_scr_library));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ui_nav_top(&nav, &top));
  TEST_ASSERT_EQ(k_ui_scr_library, top);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_ui_nav_push(&nav, (uint16_t)k_ui_scr_reading));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ui_nav_push(&nav, (uint16_t)k_ui_scr_toc));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ui_nav_top(&nav, &top));
  TEST_ASSERT_EQ(k_ui_scr_toc, top);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_ui_nav_pop(&nav, &top));
  TEST_ASSERT_EQ(k_ui_scr_reading, top);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_ui_nav_replace(&nav, (uint16_t)k_ui_scr_toc));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ui_nav_top(&nav, &top));
  TEST_ASSERT_EQ(k_ui_scr_toc, top);

  /* Pop back to root; root can never be popped. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ui_nav_pop(&nav, &top));
  TEST_ASSERT_EQ(k_ui_scr_library, top);
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_ui_nav_pop(&nav, &top));

  /* Fill to capacity, then overflow. */
  ra8_ui_nav_t full;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ui_nav_init(&full, 0U));
  for (uint16_t i = 1U; i < (uint16_t)k_ra8_ui_nav_max_depth; ++i) {
    TEST_ASSERT_EQ(k_ra8_ok, ra8_ui_nav_push(&full, i));
  }
  TEST_ASSERT_EQ(k_ra8_err_no_mem, ra8_ui_nav_push(&full, 99U));

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_ui_nav_init(nullptr, 0U));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_ui_nav_top(&nav, nullptr));
  TEST_END("ra8_ui nav stack");
}

/* ===========================================================================
 * Paging
 * ===========================================================================
 */

/**
 * @brief Functional: init / prev clamp / goto clamp / NULL guards.
 * @details Exercises empty initialization, edge clamping, movement, and null outputs.
 * @pre The pager fixture and change flag are writable.
 * @pre The valid fixture uses a nonzero total page count.
 * @post Previous and goto operations publish the expected page and change state.
 * @post Rejected calls leave all accesses within the pager object.
 * @note Next/goto compound guards are closed by dedicated MC/DC vectors.
 * @since 0.1.0
 *
 * @par MC/DC:
 * (functional test; pager_next and pager_goto decisions covered by their
 * dedicated MC/DC tests; pager_prev is a single-condition guard)
 */
RA8_INTERNAL static void internal_test_pager_basic(void)
{
  TEST_BEGIN("ra8_ui pager basic");
  ra8_ui_pager_t p;
  bool           changed = false;

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_ui_pager_init(&p, 0U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ui_pager_init(&p, 5U));
  TEST_ASSERT_EQ(0U, p.current);

  /* prev at page 0 -> clamp, no change. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ui_pager_prev(&p, &changed));
  TEST_ASSERT(!changed);

  /* goto past end clamps to total-1. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ui_pager_goto(&p, 99U, &changed));
  TEST_ASSERT(changed);
  TEST_ASSERT_EQ(4U, p.current);

  /* prev now moves. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ui_pager_prev(&p, &changed));
  TEST_ASSERT(changed);
  TEST_ASSERT_EQ(3U, p.current);

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_ui_pager_init(nullptr, 5U));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_ui_pager_next(&p, nullptr));
  TEST_END("ra8_ui pager basic");
}

/**
 * @brief Prove both pager-next guard predicates are independently decisive.
 * @details Supplies an advancing control, zero-total arm, and last-page arm.
 * @test test_mcdc_pager_next
 * @pre The three pager fixtures are initialized to their documented states.
 * @pre The shared change output is writable.
 * @post Only the all-true control advances exactly one page.
 * @post Both false-arm fixtures remain at page zero.
 * @note This is the minimal N+1 set for the two-term short-circuit AND.
 * @since 0.1.0
 *
 * @par MC/DC:
 * Decision: `(total > 0) && (current < total - 1)`
 * (2 conditions, libs/ra8_ui/src/ra8_ui.c@ra8_ui_pager_next)
 * Standard: DO-178C Table A-7 obj 5. N+1 = 3 vectors:
 * - V1: total=3,current=0 -> C1=T C2=T -> Decision T (advances)
 * - V2: total=0,current=0 -> C1=F      -> Decision F (C1 flips vs V1)
 * - V3: total=1,current=0 -> C1=T C2=F -> Decision F (C2 flips vs V1)
 */
RA8_INTERNAL static void internal_test_mcdc_pager_next(void)
{
  TEST_BEGIN("ra8_ui pager_next MC/DC: total>0 && current<total-1");
  bool changed = false;

  ra8_ui_pager_t v1 = {0U, 3U}; /* current=0, total=3 */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ui_pager_next(&v1, &changed));
  TEST_ASSERT(changed);
  TEST_ASSERT_EQ(1U, v1.current);

  ra8_ui_pager_t v2 = {0U, 0U}; /* total=0 -> C1 F */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ui_pager_next(&v2, &changed));
  TEST_ASSERT(!changed);

  ra8_ui_pager_t v3 = {0U, 1U}; /* total=1, current=0 -> C2 F */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ui_pager_next(&v3, &changed));
  TEST_ASSERT(!changed);
  TEST_END("ra8_ui pager_next MC/DC: total>0 && current<total-1");
}

/**
 * @brief Prove both pager-goto clamp predicates are independently decisive.
 * @details Supplies over-range, zero-total, and in-range target vectors.
 * @test test_mcdc_pager_goto
 * @pre The three pager fixtures and change output are writable.
 * @pre Target values remain representable by the pager index type.
 * @post The over-range target clamps only when total is nonzero.
 * @post Both no-clamp vectors retain their requested targets.
 * @note This is the minimal N+1 set for the two-term short-circuit AND.
 * @since 0.1.0
 *
 * @par MC/DC:
 * Decision: `(total > 0) && (target > total - 1)`  (the clamp guard)
 * (2 conditions, libs/ra8_ui/src/ra8_ui.c@ra8_ui_pager_goto)
 * Standard: DO-178C Table A-7 obj 5. N+1 = 3 vectors:
 * - V1: total=3,target=5 -> C1=T C2=T -> Decision T (clamp to 2)
 * - V2: total=0,target=5 -> C1=F      -> Decision F (C1 flips; no clamp)
 * - V3: total=3,target=1 -> C1=T C2=F -> Decision F (C2 flips; no clamp)
 */
RA8_INTERNAL static void internal_test_mcdc_pager_goto(void)
{
  TEST_BEGIN("ra8_ui pager_goto MC/DC: total>0 && target>total-1");
  bool changed = false;

  ra8_ui_pager_t v1 = {0U, 3U};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ui_pager_goto(&v1, 5U, &changed));
  TEST_ASSERT(changed);
  TEST_ASSERT_EQ(2U, v1.current); /* clamped to total-1 */

  ra8_ui_pager_t v2 = {0U, 0U}; /* total=0 -> C1 F: no clamp, target kept */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ui_pager_goto(&v2, 5U, &changed));
  TEST_ASSERT(changed);
  TEST_ASSERT_EQ(5U, v2.current);

  ra8_ui_pager_t v3 = {0U, 3U}; /* target=1 in range -> C2 F: no clamp */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ui_pager_goto(&v3, 1U, &changed));
  TEST_ASSERT(changed);
  TEST_ASSERT_EQ(1U, v3.current);
  TEST_END("ra8_ui pager_goto MC/DC: total>0 && target>total-1");
}

int main(void)
{
  internal_test_rect_contains_edges();
  internal_test_mcdc_rect_contains();
  internal_test_hit_test_basic();
  internal_test_nav_stack();
  internal_test_pager_basic();
  internal_test_mcdc_pager_next();
  internal_test_mcdc_pager_goto();
  return 0;
}
