/**
 * @file test_ra8_reflow_link.c
 * @brief Host unit tests + MC/DC for the #110 link/anchor query API.
 *
 * @details
 * Covers libs/ra8_reflow/src/ra8_reflow_link.c:
 *  - ra8_reflow_href_split(): classification of fragment / chapter /
 *    chapter+fragment / external / empty hrefs, with MC/DC vectors for the
 *    same-chapter-vs-cross-chapter and fragment-present decisions.
 *  - ra8_reflow_hit_test_link() and ra8_reflow_find_anchor() against a
 *    hand-populated engine (no font / layout needed for the query logic).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_reflow.h"
#include "unity_minimal.h"

/**
 * @enum reflow_link_uint8_const_t
 * @brief Named uint8_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint8_t {
  k_reflow_link_h_24             = 24,
  k_reflow_link_text_pool_used_9 = 9U,
  k_reflow_link_w_80             = 80,
  k_reflow_link_x_20             = 20,
  k_reflow_link_y_100            = 100,
  k_reflow_link_y_50             = 50,
} reflow_link_uint8_const_t;

/** @brief Shared engine handle (large -- keep off the stack). */
static ra8_reflow_t s_eng;

/* Helper to run href_split and return just the kind. */
static ra8_reflow_href_kind_t split_kind(const char* href)
{
  ra8_reflow_href_kind_t kind = k_ra8_reflow_href_empty;
  uint32_t               pl   = 0U;
  uint32_t               fo   = 0U;
  uint32_t               fl   = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_reflow_href_split(href, (uint32_t)strlen(href), &kind, &pl, &fo, &fl));
  return kind;
}

/**
 * @test test_href_split_classify_mcdc
 *
 * @par MC/DC:
 * Decisions in ra8_reflow_href_split()/priv_href_classify():
 *  (A) `if (hash == 0)` -- same-chapter (path empty) vs cross-chapter.
 *  (B) `has_frag ? ... : ...` -- fragment present vs absent.
 *  (C) priv_has_scheme(): `href[i]=='/'` / `href[i]==':'` -- external scheme.
 *
 * Vectors (each flips one decision, holding the others):
 *  - "#foot"        -> A=T, B=T            -> fragment       (same-chapter).
 *  - "ch2.xhtml"    -> A=F, B=F, C=F       -> chapter        (cross-chapter).
 *  - "ch2.xhtml#f"  -> A=F, B=T, C=F       -> chapter+frag.
 *  - "http://x/y"   -> C=T (':' before '/')-> external.
 *  - "sub/ch.xhtml" -> C=F ('/' before ':')-> chapter (path '/' is not a scheme).
 *  - ""             -> len==0              -> empty.
 *  - "#"            -> A=T, B=T (empty frag)-> fragment.
 *  A T-vs-F pair on hash (#foot vs ch2.xhtml) and on has_frag (ch2.xhtml vs
 *  ch2.xhtml#f) shows each independently flips the outcome.
 */
static void test_href_split_classify_mcdc(void)
{
  TEST_BEGIN("ra8_reflow_href_split classify MC/DC");
  TEST_ASSERT_EQ(k_ra8_reflow_href_fragment, split_kind("#foot"));
  TEST_ASSERT_EQ(k_ra8_reflow_href_chapter, split_kind("ch2.xhtml"));
  TEST_ASSERT_EQ(k_ra8_reflow_href_chapter_fragment, split_kind("ch2.xhtml#f"));
  TEST_ASSERT_EQ(k_ra8_reflow_href_external, split_kind("http://x/y"));
  TEST_ASSERT_EQ(k_ra8_reflow_href_external, split_kind("mailto:a@b"));
  TEST_ASSERT_EQ(k_ra8_reflow_href_chapter, split_kind("sub/ch.xhtml"));
  TEST_ASSERT_EQ(k_ra8_reflow_href_empty, split_kind(""));
  TEST_ASSERT_EQ(k_ra8_reflow_href_fragment, split_kind("#"));
  TEST_END("ra8_reflow_href_split classify MC/DC");
}

/**
 * @test test_href_split_spans
 * @brief The path/fragment spans are returned correctly + null guard.
 */
static void test_href_split_spans(void)
{
  TEST_BEGIN("ra8_reflow_href_split spans + null guard");
  ra8_reflow_href_kind_t kind = k_ra8_reflow_href_empty;
  uint32_t               pl   = 0U;
  uint32_t               fo   = 0U;
  uint32_t               fl   = 0U;
  const char*            h    = "chap.xhtml#sec3";
  TEST_ASSERT_EQ(k_ra8_ok, ra8_reflow_href_split(h, (uint32_t)strlen(h), &kind, &pl, &fo, &fl));
  TEST_ASSERT_EQ(k_ra8_reflow_href_chapter_fragment, kind);
  TEST_ASSERT_EQ(10, pl); /* "chap.xhtml"   */
  TEST_ASSERT_EQ(11, fo); /* index past '#' */
  TEST_ASSERT_EQ(4, fl);  /* "sec3"         */
  TEST_ASSERT_EQ(0, memcmp(&h[fo], "sec3", 4));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_reflow_href_split(NULL, 1U, &kind, &pl, &fo, &fl));
  TEST_END("ra8_reflow_href_split spans + null guard");
}

/**
 * @test test_hit_test_link
 * @brief Hit-test resolves a point inside a link rect to its href slice.
 */
static void test_hit_test_link(void)
{
  TEST_BEGIN("ra8_reflow_hit_test_link");
  memset(&s_eng, 0, sizeof s_eng);
  s_eng.in_use = 1U;
  /* text pool holds the href "ch2.xhtml" at offset 0 */
  memcpy(s_eng.text_pool, "ch2.xhtml", 9);
  s_eng.text_pool_used           = k_reflow_link_text_pool_used_9;
  s_eng.link_targets[0].href_off = 0U;
  s_eng.link_targets[0].href_len = k_reflow_link_text_pool_used_9;
  s_eng.link_target_count        = 1U;
  s_eng.link_rects[0].x          = k_reflow_link_x_20;
  s_eng.link_rects[0].y          = k_reflow_link_y_100;
  s_eng.link_rects[0].w          = k_reflow_link_w_80;
  s_eng.link_rects[0].h          = k_reflow_link_h_24;
  s_eng.link_rects[0].target     = 0U;
  s_eng.link_rects[0].page_index = 1U;
  s_eng.link_rect_count          = 1U;

  uint32_t off = 0U;
  uint32_t len = 0U;
  /* Inside the rect on the right page. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_reflow_hit_test_link(&s_eng, 1U, 40, 110, &off, &len));
  TEST_ASSERT_EQ(0, off);
  TEST_ASSERT_EQ(9, len);
  TEST_ASSERT_EQ(0, memcmp(&s_eng.text_pool[off], "ch2.xhtml", 9));
  /* Outside the rect -> not found. */
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_reflow_hit_test_link(&s_eng, 1U, 5, 110, &off, &len));
  /* Right point, wrong page -> not found. */
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_reflow_hit_test_link(&s_eng, 0U, 40, 110, &off, &len));
  /* Null guard. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_reflow_hit_test_link(&s_eng, 1U, 40, 110, NULL, &len));
  TEST_END("ra8_reflow_hit_test_link");
}

/**
 * @test test_hit_test_rect_bounds_mcdc
 *
 * @par MC/DC:
 * Decision (libs/ra8_reflow/src/ra8_reflow_link.c@ra8_reflow_hit_test_link):
 * `if ((x >= rect->x) && (x < (rect->x + rect->w)) &&
 *      (y >= rect->y) && (y < (rect->y + rect->h)))` (4 conditions, all AND).
 * Driven directly through the public API: a hit returns k_ra8_ok, a miss
 * returns k_ra8_err_not_found -- production-source MC/DC.
 *
 * The single rect is x=20,y=100,w=80,h=24 -> the open box [20,100)..(99,123].
 * Vectors (Chilenski masking-MC/DC, N+1 = 5 for N=4); each flips exactly one
 * condition while the other three are held at their true (control) value:
 *  - V1: x=40, y=110 -> C1 T, C2 T, C3 T, C4 T -> decision T (found).
 *  - V2: x=19, y=110 -> C1 F (x < rect->x)               -> decision F.
 *  - V3: x=100,y=110 -> C2 F (x >= rect->x + rect->w)     -> decision F.
 *  - V4: x=40, y=99  -> C3 F (y < rect->y)                -> decision F.
 *  - V5: x=40, y=124 -> C4 F (y >= rect->y + rect->h)     -> decision F.
 *
 * Independence: V1 vs V2 flip C1, V1 vs V3 flip C2, V1 vs V4 flip C3, V1 vs V5
 * flip C4 -- each pair leaves the other three conditions true and the outcome
 * flips, proving each condition independently affects the result.
 */
static void test_hit_test_rect_bounds_mcdc(void)
{
  TEST_BEGIN("ra8_reflow_hit_test_link rect-bounds MC/DC");
  memset(&s_eng, 0, sizeof s_eng);
  s_eng.in_use = 1U;
  memcpy(s_eng.text_pool, "ch2.xhtml", 9);
  s_eng.text_pool_used           = k_reflow_link_text_pool_used_9;
  s_eng.link_targets[0].href_off = 0U;
  s_eng.link_targets[0].href_len = k_reflow_link_text_pool_used_9;
  s_eng.link_target_count        = 1U;
  s_eng.link_rects[0].x          = k_reflow_link_x_20;
  s_eng.link_rects[0].y          = k_reflow_link_y_100;
  s_eng.link_rects[0].w          = k_reflow_link_w_80;
  s_eng.link_rects[0].h          = k_reflow_link_h_24;
  s_eng.link_rects[0].target     = 0U;
  s_eng.link_rects[0].page_index = 1U;
  s_eng.link_rect_count          = 1U;

  uint32_t off = 0U;
  uint32_t len = 0U;
  /* V1: control -- all four conditions true -> hit. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_reflow_hit_test_link(&s_eng, 1U, 40, 110, &off, &len));
  TEST_ASSERT_EQ(0, off);
  TEST_ASSERT_EQ(9, len);
  /* V2: x just left of the rect -> C1 false. */
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_reflow_hit_test_link(&s_eng, 1U, 19, 110, &off, &len));
  /* V3: x at the right edge (exclusive) -> C2 false. */
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_reflow_hit_test_link(&s_eng, 1U, 100, 110, &off, &len));
  /* V4: y just above the rect -> C3 false. */
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_reflow_hit_test_link(&s_eng, 1U, 40, 99, &off, &len));
  /* V5: y at the bottom edge (exclusive) -> C4 false. */
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_reflow_hit_test_link(&s_eng, 1U, 40, 124, &off, &len));
  TEST_END("ra8_reflow_hit_test_link rect-bounds MC/DC");
}

/**
 * @test test_find_anchor
 * @brief Same-chapter anchor lookup returns the anchor's page.
 */
static void test_find_anchor(void)
{
  TEST_BEGIN("ra8_reflow_find_anchor");
  memset(&s_eng, 0, sizeof s_eng);
  s_eng.in_use = 1U;
  memcpy(s_eng.text_pool, "foot", 4);
  s_eng.text_pool_used        = 4U;
  s_eng.anchors[0].id_off     = 0U;
  s_eng.anchors[0].id_len     = 4U;
  s_eng.anchors[0].page_index = 3U;
  s_eng.anchors[0].y          = k_reflow_link_y_50;
  s_eng.anchor_count          = 1U;

  uint32_t page = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_reflow_find_anchor(&s_eng, "foot", 4U, &page));
  TEST_ASSERT_EQ(3, page);
  /* Different length should not match (prefix guard). */
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_reflow_find_anchor(&s_eng, "foo", 3U, &page));
  /* Unknown id. */
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_reflow_find_anchor(&s_eng, "barz", 4U, &page));
  /* Zero length -> invalid arg. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_reflow_find_anchor(&s_eng, "foot", 0U, &page));
  TEST_END("ra8_reflow_find_anchor");
}

/**
 * @brief Test entry point.
 * @return 0 on success; unity macros exit(1) on the first failure.
 */
int32_t main(void)
{
  test_href_split_classify_mcdc();
  test_href_split_spans();
  test_hit_test_link();
  test_hit_test_rect_bounds_mcdc();
  test_find_anchor();
  (void)fprintf(stderr, "[OK ] test_ra8_reflow_link.c\n");
  return 0;
}
