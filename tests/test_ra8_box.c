/**
 * @file test_ra8_box.c
 * @brief Unit tests for the ra8_box layout engine (stack / grid / sizing).
 *
 * @details
 * Pure host tests -- ra8_box is allocation-free geometry. Covers the
 * builder (add / overflow / bad parent), stack and grid layout maths
 * (padding, gap, fixed vs flex), plus MC/DC vector sets for the three
 * functions that carry compound decisions: ``internal_iter_live`` (the
 * shared child-walk guard), ``ra8_box_add`` and ``ra8_box_layout``.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>

#include "ra8_box.h"
#include "ra8_err.h"
#include "ra8_ui.h"
#include "unity_minimal.h"

/**
 * @enum box_uint8_const_t
 * @brief Named uint8_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint8_t {
  k_box_leaf_10 = 10,
  k_box_leaf_20 = 20,
  k_box_leaf_30 = 30,
} box_uint8_const_t;

/** @brief Node-storage capacity for the test trees. */
enum : uint16_t { k_box_cap = 32U /**< Box cap. */ };

/** @brief A leaf node template. */
static ra8_box_t leaf(int16_t fixed, uint16_t flex)
{
  ra8_box_t n = {};
  n.kind      = (uint8_t)k_ra8_box_leaf;
  n.fixed     = fixed;
  n.flex      = flex;
  n.grid_cols = 1U;
  n.tag       = (int16_t)k_ra8_box_none;
  return n;
}

/** @brief A container node template. */
static ra8_box_t container(ra8_box_kind_t kind, int16_t pad, int16_t gap, uint8_t cols)
{
  ra8_box_t n = {};
  n.kind      = (uint8_t)kind;
  n.pad       = pad;
  n.gap       = gap;
  n.grid_cols = (cols >= 1U) ? cols : 1U;
  n.flex      = 1U;
  n.tag       = (int16_t)k_ra8_box_none;
  return n;
}

/* ===========================================================================
 * Builder
 * ===========================================================================
 */

/**
 * @brief Functional: init validation + add links + tag round-trip.
 *
 * @par MC/DC:
 * (functional test; ra8_box_add and ra8_box_layout compound decisions are
 * covered by their dedicated MC/DC tests)
 */
static void test_box_build(void)
{
  TEST_BEGIN("ra8_box build + links");
  ra8_box_t      store[k_box_cap];
  ra8_box_tree_t t;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_box_tree_init(nullptr, store, k_box_cap));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_box_tree_init(&t, store, 0U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_box_tree_init(&t, store, k_box_cap));

  ra8_box_t     root = container(k_ra8_box_stack_v, 0, 0, 1U);
  const int16_t ri   = ra8_box_add(&t, (int16_t)k_ra8_box_none, &root);
  TEST_ASSERT_EQ(0, ri);
  ra8_box_t     a  = leaf(k_box_leaf_10, 0U);
  ra8_box_t     b  = leaf(k_box_leaf_20, 0U);
  const int16_t ai = ra8_box_add(&t, ri, &a);
  const int16_t bi = ra8_box_add(&t, ri, &b);
  TEST_ASSERT_EQ(1, ai);
  TEST_ASSERT_EQ(2, bi);
  /* Sibling chain: root.first_child == a, a.next == b. */
  TEST_ASSERT_EQ(ai, store[ri].first_child);
  TEST_ASSERT_EQ(bi, store[ai].next);
  TEST_END("ra8_box build + links");
}

/* ===========================================================================
 * Stack layout maths
 * ===========================================================================
 */

/**
 * @brief Functional: vertical stack with padding, gap, fixed + flex.
 *
 * @par MC/DC:
 * (functional layout assertions; the child-walk guard is covered by
 * test_mcdc_iter_live)
 */
static void test_box_stack_v(void)
{
  TEST_BEGIN("ra8_box stack_v fixed+flex");
  ra8_box_t      store[k_box_cap];
  ra8_box_tree_t t;
  (void)ra8_box_tree_init(&t, store, k_box_cap);
  ra8_box_t           rootn = container(k_ra8_box_stack_v, k_box_leaf_10, k_box_leaf_10, 1U);
  const int16_t       r     = ra8_box_add(&t, (int16_t)k_ra8_box_none, &rootn);
  ra8_box_t           an    = leaf(k_box_leaf_30, 0U);
  ra8_box_t           bn    = leaf(0, 1U);
  ra8_box_t           cn    = leaf(k_box_leaf_20, 0U);
  const int16_t       a     = ra8_box_add(&t, r, &an);
  const int16_t       b     = ra8_box_add(&t, r, &bn);
  const int16_t       c     = ra8_box_add(&t, r, &cn);
  const ra8_ui_rect_t frame = {0, 0, 100, 200};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_box_layout(&t, r, &frame));
  /* content = (10,10,80,180); gaps=20; flex_space=180-50-20=110. */
  TEST_ASSERT_EQ(10, store[a].rect.y);
  TEST_ASSERT_EQ(30, store[a].rect.h);
  TEST_ASSERT_EQ(80, store[a].rect.w);
  TEST_ASSERT_EQ(50, store[b].rect.y);
  TEST_ASSERT_EQ(110, store[b].rect.h);
  TEST_ASSERT_EQ(170, store[c].rect.y);
  TEST_ASSERT_EQ(20, store[c].rect.h);
  TEST_END("ra8_box stack_v fixed+flex");
}

/**
 * @brief Functional: horizontal stack splits two equal flex children.
 *
 * @par MC/DC:
 * (functional layout assertions; guard covered by test_mcdc_iter_live)
 */
static void test_box_stack_h(void)
{
  TEST_BEGIN("ra8_box stack_h flex split");
  ra8_box_t      store[k_box_cap];
  ra8_box_tree_t t;
  (void)ra8_box_tree_init(&t, store, k_box_cap);
  ra8_box_t           rootn = container(k_ra8_box_stack_h, 0, k_box_leaf_20, 1U);
  const int16_t       r     = ra8_box_add(&t, (int16_t)k_ra8_box_none, &rootn);
  ra8_box_t           an    = leaf(0, 1U);
  ra8_box_t           bn    = leaf(0, 1U);
  const int16_t       a     = ra8_box_add(&t, r, &an);
  const int16_t       b     = ra8_box_add(&t, r, &bn);
  const ra8_ui_rect_t frame = {0, 0, 220, 50};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_box_layout(&t, r, &frame));
  /* flex_space = 220 - 0 - 20 = 200; each = 100. */
  TEST_ASSERT_EQ(0, store[a].rect.x);
  TEST_ASSERT_EQ(100, store[a].rect.w);
  TEST_ASSERT_EQ(120, store[b].rect.x);
  TEST_ASSERT_EQ(100, store[b].rect.w);
  TEST_END("ra8_box stack_h flex split");
}

/**
 * @brief Functional: 2-column grid places 3 children row-major.
 *
 * @par MC/DC:
 * (functional layout assertions; guard covered by test_mcdc_iter_live)
 */
static void test_box_grid(void)
{
  TEST_BEGIN("ra8_box grid row-major");
  ra8_box_t      store[k_box_cap];
  ra8_box_tree_t t;
  (void)ra8_box_tree_init(&t, store, k_box_cap);
  ra8_box_t           rootn = container(k_ra8_box_grid, 0, k_box_leaf_10, 2U);
  const int16_t       r     = ra8_box_add(&t, (int16_t)k_ra8_box_none, &rootn);
  ra8_box_t           c0    = leaf(0, 0U);
  ra8_box_t           c1    = leaf(0, 0U);
  ra8_box_t           c2    = leaf(0, 0U);
  const int16_t       i0    = ra8_box_add(&t, r, &c0);
  const int16_t       i1    = ra8_box_add(&t, r, &c1);
  const int16_t       i2    = ra8_box_add(&t, r, &c2);
  const ra8_ui_rect_t frame = {0, 0, 100, 100};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_box_layout(&t, r, &frame));
  /* cell_w=(100-10)/2=45; rows=2; cell_h=(100-10)/2=45. */
  TEST_ASSERT_EQ(0, store[i0].rect.x);
  TEST_ASSERT_EQ(0, store[i0].rect.y);
  TEST_ASSERT_EQ(45, store[i0].rect.w);
  TEST_ASSERT_EQ(55, store[i1].rect.x);
  TEST_ASSERT_EQ(0, store[i1].rect.y);
  TEST_ASSERT_EQ(0, store[i2].rect.x);
  TEST_ASSERT_EQ(55, store[i2].rect.y);
  TEST_END("ra8_box grid row-major");
}

/* ===========================================================================
 * MC/DC
 * ===========================================================================
 */

/**
 * @test test_mcdc_iter_live
 *
 * @par MC/DC:
 * Decision: `(link != k_ra8_box_none) && (guard < count)`
 * (2 conditions, libs/ra8_box/src/ra8_box.c@internal_iter_live)
 * Standard: DO-178C Table A-7 obj 5. Exercised indirectly through the
 * child walks in ra8_box_layout:
 * - V1: mid-walk on a 2-child stack -> link!=none=T, guard<count=T
 *       -> continue (C1=T,C2=T baseline).
 * - V2: end of chain -> link==none=F -> stop (C1 flips vs V1).
 * The (C1=T, C2=F) row -- guard hitting count while the link is still
 * live -- is only reachable on a cyclic link, which ra8_box_add never
 * builds; it is a defensive bound, documented per docs/MCDC.md as
 * unreachable through the public API (DO-178C 6.4.4.3 deactivated row).
 */
static void test_mcdc_iter_live(void)
{
  TEST_BEGIN("ra8_box internal_iter_live MC/DC");
  ra8_box_t      store[k_box_cap];
  ra8_box_tree_t t;
  (void)ra8_box_tree_init(&t, store, k_box_cap);
  ra8_box_t           rootn = container(k_ra8_box_stack_v, 0, 0, 1U);
  const int16_t       r     = ra8_box_add(&t, (int16_t)k_ra8_box_none, &rootn);
  ra8_box_t           an    = leaf(k_box_leaf_10, 0U);
  ra8_box_t           bn    = leaf(k_box_leaf_10, 0U);
  const int16_t       a     = ra8_box_add(&t, r, &an); /* walked: C1=T (V1)          */
  const int16_t       b     = ra8_box_add(&t, r, &bn); /* then chain ends: C1=F (V2) */
  const ra8_ui_rect_t frame = {0, 0, 40, 40};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_box_layout(&t, r, &frame));
  /* Both children were visited (proves the T then F traversal). */
  TEST_ASSERT_EQ(0, store[a].rect.y);
  TEST_ASSERT_EQ(10, store[b].rect.y);
  TEST_END("ra8_box internal_iter_live MC/DC");
}

/**
 * @test test_mcdc_box_add
 *
 * @par MC/DC:
 * Decisions in libs/ra8_box/src/ra8_box.c@ra8_box_add:
 * D1 null guard `(tree == nullptr) || (node == nullptr)`:
 * - tree=NULL              -> C1=T            -> none (C1 flips).
 * - tree ok, node=NULL     -> C1=F, C2=T      -> none (C2 flips).
 * - tree ok, node ok       -> C1=F, C2=F      -> add (baseline).
 * D2 parent guard `(parent != none) && ((parent < 0) || (parent >= count))`:
 * - parent=none            -> A=F             -> add as root.
 * - parent in range        -> A=T, B=F, C=F   -> add as child.
 * - parent < -1            -> A=T, B=T        -> none (B flips).
 * - parent >= count        -> A=T, B=F, C=T   -> none (C flips).
 */
static void test_mcdc_box_add(void)
{
  TEST_BEGIN("ra8_box ra8_box_add MC/DC");
  ra8_box_t      store[k_box_cap];
  ra8_box_tree_t t;
  (void)ra8_box_tree_init(&t, store, k_box_cap);
  ra8_box_t n = leaf(0, 1U);

  TEST_ASSERT_EQ(k_ra8_box_none, ra8_box_add(nullptr, (int16_t)k_ra8_box_none, &n));
  TEST_ASSERT_EQ(k_ra8_box_none, ra8_box_add(&t, (int16_t)k_ra8_box_none, nullptr));

  const int16_t root = ra8_box_add(&t, (int16_t)k_ra8_box_none, &n); /* A=F -> root */
  TEST_ASSERT_EQ(0, root);
  const int16_t child = ra8_box_add(&t, root, &n); /* A=T,B=F,C=F -> child */
  TEST_ASSERT_EQ(1, child);
  TEST_ASSERT_EQ(k_ra8_box_none, ra8_box_add(&t, (int16_t)-2, &n)); /* B flips */
  TEST_ASSERT_EQ(k_ra8_box_none, ra8_box_add(&t, (int16_t)50, &n)); /* C flips */
  TEST_END("ra8_box ra8_box_add MC/DC");
}

/**
 * @test test_mcdc_box_layout
 *
 * @par MC/DC:
 * Decision in libs/ra8_box/src/ra8_box.c@ra8_box_layout:
 * `(count == 0) || (root < 0) || (root >= count)`
 * - empty tree (count=0) -> C1=T              -> invalid_arg (C1 flips).
 * - root=-1 on a 1-node  -> C1=F, C2=T        -> invalid_arg (C2 flips).
 * - root=5 on a 1-node   -> C1=F, C2=F, C3=T  -> invalid_arg (C3 flips).
 * - root=0 on a 1-node   -> C1=F, C2=F, C3=F  -> ok (baseline).
 */
static void test_mcdc_box_layout(void)
{
  TEST_BEGIN("ra8_box ra8_box_layout MC/DC");
  ra8_box_t           store[k_box_cap];
  ra8_box_tree_t      t;
  const ra8_ui_rect_t frame = {0, 0, 10, 10};
  (void)ra8_box_tree_init(&t, store, k_box_cap);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_box_layout(&t, 0, &frame)); /* count=0 */
  ra8_box_t     n = leaf(0, 1U);
  const int16_t r = ra8_box_add(&t, (int16_t)k_ra8_box_none, &n);
  TEST_ASSERT_EQ(0, r);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_box_layout(&t, (int16_t)-1, &frame)); /* root<0      */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_box_layout(&t, (int16_t)5, &frame));  /* root>=count */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_box_layout(&t, 0, nullptr));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_box_layout(&t, 0, &frame)); /* valid */
  TEST_END("ra8_box ra8_box_layout MC/DC");
}

int main(void)
{
  test_box_build();
  test_box_stack_v();
  test_box_stack_h();
  test_box_grid();
  test_mcdc_iter_live();
  test_mcdc_box_add();
  test_mcdc_box_layout();
  return 0;
}
