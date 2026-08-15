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

#include "ra8_attributes.h"
#include "ra8_box.h"
#include "ra8_err.h"
#include "ra8_ui.h"
#include "unity_minimal.h"

/**
 * @enum box_leaf_size_t
 * @brief Main-axis sizes of the three leaf boxes; all different, so a layout that summed the wrong children shows in the total.
 */
typedef enum : uint8_t {
  k_box_leaf_small  = 10, /**< Main-axis size of the small leaf box. */
  k_box_leaf_medium = 20, /**< Of the medium one.                    */
  k_box_leaf_large =
    30, /**< Of the large one; the three differ, so summing the wrong children shows in total. */
} box_leaf_size_t;

/** @brief Node-storage capacity for the test trees. */
enum : uint16_t { k_box_cap = 32U /**< Box cap. */ };

/**
 * @brief Build a leaf-node template.
 * @details Initializes all required fields while leaving geometry for layout.
 * @param[in] fixed Requested fixed main-axis extent.
 * @param[in] flex Relative flexible-size weight.
 * @return Fully initialized leaf descriptor.
 * @retval ra8_box_t A leaf with no tag or child links.
 * @pre `fixed` and `flex` are deliberate test fixture values.
 * @pre The box enumerator values fit their stored field widths.
 * @post The result has kind `k_ra8_box_leaf` and one grid column.
 * @post No tree or caller storage is modified.
 * @note Test-only value constructor.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_box_t internal_leaf(int16_t fixed, uint16_t flex)
{
  ra8_box_t n = {};
  n.kind      = (uint8_t)k_ra8_box_leaf;
  n.fixed     = fixed;
  n.flex      = flex;
  n.grid_cols = 1U;
  n.tag       = (int16_t)k_ra8_box_none;
  return n;
}

/**
 * @brief Build a container-node template.
 * @details Initializes layout policy, spacing, and a normalized column count.
 * @param[in] kind Stack or grid container kind.
 * @param[in] pad Uniform inner padding.
 * @param[in] gap Inter-child spacing.
 * @param[in] cols Requested grid-column count.
 * @return Fully initialized container descriptor.
 * @retval ra8_box_t A flexible container with no tag or child links.
 * @pre `kind` denotes a container layout supported by the test.
 * @pre Spacing values are representable by the published fields.
 * @post A zero column request is normalized to one.
 * @post No tree or caller storage is modified.
 * @note Test-only value constructor shared by all layout vectors.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_box_t
internal_container(ra8_box_kind_t kind, int16_t pad, int16_t gap, uint8_t cols)
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
 * @details Initializes a bounded tree, rejects invalid init inputs, and checks
 * the root-to-child sibling chain built by successive additions.
 * @pre Fixed test storage holds `k_box_cap` nodes.
 * @pre Leaf and container constructors produce valid descriptors.
 * @post Valid additions occupy indices zero through two in order.
 * @post Root and sibling links match insertion order.
 * @note Compound add guards are isolated by a dedicated MC/DC vector.
 * @since 0.1.0
 *
 * @par MC/DC:
 * (functional test; ra8_box_add and ra8_box_layout compound decisions are
 * covered by their dedicated MC/DC tests)
 */
RA8_INTERNAL static void internal_test_box_build(void)
{
  TEST_BEGIN("ra8_box build + links");
  ra8_box_t      store[k_box_cap];
  ra8_box_tree_t t;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_box_tree_init(nullptr, store, k_box_cap));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_box_tree_init(&t, store, 0U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_box_tree_init(&t, store, k_box_cap));

  ra8_box_t     root = internal_container(k_ra8_box_stack_v, 0, 0, 1U);
  const int16_t ri   = ra8_box_add(&t, (int16_t)k_ra8_box_none, &root);
  TEST_ASSERT_EQ(0, ri);
  ra8_box_t     a  = internal_leaf(k_box_leaf_small, 0U);
  ra8_box_t     b  = internal_leaf(k_box_leaf_medium, 0U);
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
 * @details Lays out fixed, flexible, and fixed children in a padded frame.
 * @pre Tree initialization and all four additions succeed.
 * @pre The frame leaves positive content after padding and gaps.
 * @post The flexible child receives the exact remaining height.
 * @post Every child has the expected horizontal extent and vertical offset.
 * @note Pins the vertical main-axis arithmetic.
 * @since 0.1.0
 *
 * @par MC/DC:
 * (functional layout assertions; the child-walk guard is covered by
 * internal_test_mcdc_iter_live)
 */
RA8_INTERNAL static void internal_test_box_stack_v(void)
{
  TEST_BEGIN("ra8_box stack_v fixed+flex");
  ra8_box_t      store[k_box_cap];
  ra8_box_tree_t t;
  (void)ra8_box_tree_init(&t, store, k_box_cap);
  ra8_box_t rootn = internal_container(k_ra8_box_stack_v, k_box_leaf_small, k_box_leaf_small, 1U);
  const int16_t       r     = ra8_box_add(&t, (int16_t)k_ra8_box_none, &rootn);
  ra8_box_t           an    = internal_leaf(k_box_leaf_large, 0U);
  ra8_box_t           bn    = internal_leaf(0, 1U);
  ra8_box_t           cn    = internal_leaf(k_box_leaf_medium, 0U);
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
 * @details Lays out equal-weight children around one fixed horizontal gap.
 * @pre Tree initialization and all three additions succeed.
 * @pre The frame width exceeds the configured gap.
 * @post Both children receive equal widths.
 * @post The second child begins after the first width plus the gap.
 * @note Pins the horizontal main-axis dispatch.
 * @since 0.1.0
 *
 * @par MC/DC:
 * (functional layout assertions; guard covered by internal_test_mcdc_iter_live)
 */
RA8_INTERNAL static void internal_test_box_stack_h(void)
{
  TEST_BEGIN("ra8_box stack_h flex split");
  ra8_box_t      store[k_box_cap];
  ra8_box_tree_t t;
  (void)ra8_box_tree_init(&t, store, k_box_cap);
  ra8_box_t           rootn = internal_container(k_ra8_box_stack_h, 0, k_box_leaf_medium, 1U);
  const int16_t       r     = ra8_box_add(&t, (int16_t)k_ra8_box_none, &rootn);
  ra8_box_t           an    = internal_leaf(0, 1U);
  ra8_box_t           bn    = internal_leaf(0, 1U);
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
 * @details Builds a two-row grid and checks cell geometry for three children.
 * @pre Tree initialization and all four additions succeed.
 * @pre Grid columns are nonzero after constructor normalization.
 * @post Children zero and one occupy the first row in distinct columns.
 * @post Child two starts the second row with the expected gap offset.
 * @note Pins incomplete-final-row handling.
 * @since 0.1.0
 *
 * @par MC/DC:
 * (functional layout assertions; guard covered by internal_test_mcdc_iter_live)
 */
RA8_INTERNAL static void internal_test_box_grid(void)
{
  TEST_BEGIN("ra8_box grid row-major");
  ra8_box_t      store[k_box_cap];
  ra8_box_tree_t t;
  (void)ra8_box_tree_init(&t, store, k_box_cap);
  ra8_box_t           rootn = internal_container(k_ra8_box_grid, 0, k_box_leaf_small, 2U);
  const int16_t       r     = ra8_box_add(&t, (int16_t)k_ra8_box_none, &rootn);
  ra8_box_t           c0    = internal_leaf(0, 0U);
  ra8_box_t           c1    = internal_leaf(0, 0U);
  ra8_box_t           c2    = internal_leaf(0, 0U);
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
 * @test internal_test_mcdc_iter_live
 * @brief Exercise both reachable outcomes of the guarded child iterator.
 * @details Lays out two linked children so traversal continues once and then
 * stops at the end-of-chain sentinel.
 * @pre The public builder creates an acyclic two-child chain.
 * @pre The frame is large enough to give both children visible geometry.
 * @post Both children receive positions, proving the live traversal arm.
 * @post Traversal stops safely when the sibling link becomes none.
 * @note The defensive cycle-bound row is unreachable through the public API.
 * @since 0.1.0
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
RA8_INTERNAL static void internal_test_mcdc_iter_live(void)
{
  TEST_BEGIN("ra8_box internal_iter_live MC/DC");
  ra8_box_t      store[k_box_cap];
  ra8_box_tree_t t;
  (void)ra8_box_tree_init(&t, store, k_box_cap);
  ra8_box_t           rootn = internal_container(k_ra8_box_stack_v, 0, 0, 1U);
  const int16_t       r     = ra8_box_add(&t, (int16_t)k_ra8_box_none, &rootn);
  ra8_box_t           an    = internal_leaf(k_box_leaf_small, 0U);
  ra8_box_t           bn    = internal_leaf(k_box_leaf_small, 0U);
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
 * @test internal_test_mcdc_box_add
 * @brief Prove every condition in the add guards controls the result.
 * @details Supplies null pointers plus absent, valid, negative, and high parents.
 * @pre Bounded node storage is initialized before non-null arms.
 * @pre The fixture node is a valid leaf descriptor.
 * @post Valid root and child additions return consecutive indices.
 * @post Every invalid pointer or parent returns `k_ra8_box_none`.
 * @note Implements the documented N+1 guard vectors.
 * @since 0.1.0
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
RA8_INTERNAL static void internal_test_mcdc_box_add(void)
{
  TEST_BEGIN("ra8_box ra8_box_add MC/DC");
  ra8_box_t      store[k_box_cap];
  ra8_box_tree_t t;
  (void)ra8_box_tree_init(&t, store, k_box_cap);
  ra8_box_t n = internal_leaf(0, 1U);

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
 * @test internal_test_mcdc_box_layout
 * @brief Prove every condition in the root-layout guard controls rejection.
 * @details Compares empty, negative-root, high-root, null-frame, and valid arms.
 * @pre Tree storage is initialized and can hold the one valid node.
 * @pre The frame has nonnegative dimensions.
 * @post All invalid root/frame combinations return their documented errors.
 * @post The valid root and frame return `k_ra8_ok`.
 * @note Covers the three-condition root-range decision.
 * @since 0.1.0
 *
 * @par MC/DC:
 * Decision in libs/ra8_box/src/ra8_box.c@ra8_box_layout:
 * `(count == 0) || (root < 0) || (root >= count)`
 * - empty tree (count=0) -> C1=T              -> invalid_arg (C1 flips).
 * - root=-1 on a 1-node  -> C1=F, C2=T        -> invalid_arg (C2 flips).
 * - root=5 on a 1-node   -> C1=F, C2=F, C3=T  -> invalid_arg (C3 flips).
 * - root=0 on a 1-node   -> C1=F, C2=F, C3=F  -> ok (baseline).
 */
RA8_INTERNAL static void internal_test_mcdc_box_layout(void)
{
  TEST_BEGIN("ra8_box ra8_box_layout MC/DC");
  ra8_box_t           store[k_box_cap];
  ra8_box_tree_t      t;
  const ra8_ui_rect_t frame = {0, 0, 10, 10};
  (void)ra8_box_tree_init(&t, store, k_box_cap);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_box_layout(&t, 0, &frame)); /* count=0 */
  ra8_box_t     n = internal_leaf(0, 1U);
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
  internal_test_box_build();
  internal_test_box_stack_v();
  internal_test_box_stack_h();
  internal_test_box_grid();
  internal_test_mcdc_iter_live();
  internal_test_mcdc_box_add();
  internal_test_mcdc_box_layout();
  return 0;
}
