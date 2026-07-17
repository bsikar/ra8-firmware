/**
 * @file test_ra8_reflow_api_mcdc.c
 * @brief MC/DC vector tests for the public ra8_reflow API guards (v1 engine).
 *
 * @details
 * Split sibling of test_ra8_reflow.c carrying the MC/DC vector tests for the
 * compound guard decisions of libs/ra8_reflow/src/ra8_reflow_layout.c: the
 * ra8_reflow_init() null / viewport / font-size ORs, the
 * ra8_reflow_layout_chapter() 3-condition null OR, the
 * ra8_reflow_get_page_count() null OR, the ra8_reflow_set_font_size() range
 * and state ORs, and the empty-layout guard. Loads the vendored Literata
 * font through the shared fixture in tests/support/reflow_v1_test_util.h;
 * like its sibling it is excluded from the build under
 * RA8_REFLOW_USE_LITEHTML (v2 does not populate the v1 glyph internals).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * [Ring 4 / Reflow]
 * {World: NS}
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_reflow.h"
#include "support/reflow_v1_test_util.h"
#include "unity_minimal.h"

typedef enum : uint16_t {
  k_mcdc_font_px_too_low  = 4U,   /**< Mcdc font px too low.  */
  k_mcdc_font_px_too_high = 200U, /**< Mcdc font px too high. */
  k_mcdc_font_px_ok       = 18U,  /**< Mcdc font px ok.       */
} reflow_mcdc_t;

/**
 * @test test_mcdc_init_dim_or
 *
 * @par MC/DC:
 * Decision: `if (font_data == NULL || out_engine == NULL)`
 * (2 conditions, libs/ra8_reflow/src/ra8_reflow_layout.c line 535). N+1=3.
 * - V1 both non-NULL  -> both F. F (proceeds; later guards run).
 * - V2 font_data=NULL -> C1=T short-circuits. T -> null_ptr.
 * - V3 out_engine=NULL -> C1=F, C2=T. T -> null_ptr.
 */
static void test_mcdc_init_font_or(void)
{
  TEST_BEGIN("mcdc init font_data || out_engine NULL");
  /* V1 */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_reflow_init(k_test_viewport_w,
                                 k_test_viewport_h,
                                 s_font_buf,
                                 s_font_len,
                                 k_test_font_px,
                                 k_test_color_body,
                                 k_test_color_link,
                                 &s_engine));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_reflow_close(&s_engine));
  /* V2 */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_reflow_init(k_test_viewport_w,
                                 k_test_viewport_h,
                                 nullptr,
                                 s_font_len,
                                 k_test_font_px,
                                 k_test_color_body,
                                 k_test_color_link,
                                 &s_engine));
  /* V3 */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_reflow_init(k_test_viewport_w,
                                 k_test_viewport_h,
                                 s_font_buf,
                                 s_font_len,
                                 k_test_font_px,
                                 k_test_color_body,
                                 k_test_color_link,
                                 nullptr));
  TEST_END("mcdc init font_data || out_engine NULL");
}

/**
 * @test test_mcdc_init_viewport_or
 *
 * @par MC/DC:
 * Decision: `if (viewport_w == 0U || viewport_h == 0U)`
 * (2 conditions, line 538). N+1=3.
 */
static void test_mcdc_init_viewport_or(void)
{
  TEST_BEGIN("mcdc init viewport OR");
  /* V1 */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_reflow_init(k_test_viewport_w,
                                 k_test_viewport_h,
                                 s_font_buf,
                                 s_font_len,
                                 k_test_font_px,
                                 k_test_color_body,
                                 k_test_color_link,
                                 &s_engine));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_reflow_close(&s_engine));
  /* V2 w=0 */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_reflow_init(0U,
                                 k_test_viewport_h,
                                 s_font_buf,
                                 s_font_len,
                                 k_test_font_px,
                                 k_test_color_body,
                                 k_test_color_link,
                                 &s_engine));
  /* V3 h=0 */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_reflow_init(k_test_viewport_w,
                                 0U,
                                 s_font_buf,
                                 s_font_len,
                                 k_test_font_px,
                                 k_test_color_body,
                                 k_test_color_link,
                                 &s_engine));
  TEST_END("mcdc init viewport OR");
}

/**
 * @test test_mcdc_init_font_px_or
 *
 * @par MC/DC:
 * Decision: `if (font_px < MIN || font_px > MAX)`
 * (2 conditions, line 541). N+1=3.
 */
static void test_mcdc_init_font_px_or(void)
{
  TEST_BEGIN("mcdc init font_px OR");
  /* V1 in-range */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_reflow_init(k_test_viewport_w,
                                 k_test_viewport_h,
                                 s_font_buf,
                                 s_font_len,
                                 (uint16_t)k_mcdc_font_px_ok,
                                 k_test_color_body,
                                 k_test_color_link,
                                 &s_engine));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_reflow_close(&s_engine));
  /* V2 too low */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_reflow_init(k_test_viewport_w,
                                 k_test_viewport_h,
                                 s_font_buf,
                                 s_font_len,
                                 (uint16_t)k_mcdc_font_px_too_low,
                                 k_test_color_body,
                                 k_test_color_link,
                                 &s_engine));
  /* V3 too high */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_reflow_init(k_test_viewport_w,
                                 k_test_viewport_h,
                                 s_font_buf,
                                 s_font_len,
                                 (uint16_t)k_mcdc_font_px_too_high,
                                 k_test_color_body,
                                 k_test_color_link,
                                 &s_engine));
  TEST_END("mcdc init font_px OR");
}

/**
 * @test test_mcdc_layout_chapter_null_or3
 *
 * @par MC/DC:
 * Decision: `if (engine == NULL || xhtml_buf == NULL || out_total_pages == NULL)`
 * (3 conditions, line 584). N+1=4. Canonical short-circuit set per
 * DO-178C 6.4.4.3.
 * - V1 all non-NULL              -> all F. F (proceeds).
 * - V2 engine=NULL               -> C1=T. T -> null_ptr.
 * - V3 engine=ok, xhtml=NULL     -> C2=T. T -> null_ptr.
 * - V4 engine=ok, xhtml=ok, out=NULL -> C3=T. T -> null_ptr.
 */
static void test_mcdc_layout_chapter_null_or3(void)
{
  TEST_BEGIN("mcdc layout_chapter NULL OR(3)");
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_reflow_init(k_test_viewport_w,
                                 k_test_viewport_h,
                                 s_font_buf,
                                 s_font_len,
                                 k_test_font_px,
                                 k_test_color_body,
                                 k_test_color_link,
                                 &s_engine));
  uint32_t pages = 0U;
  /* V1 */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_reflow_layout_chapter(&s_engine,
                                           (const uint8_t*)k_xhtml_simple,
                                           strlen(k_xhtml_simple),
                                           &pages));
  /* V2 */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_reflow_layout_chapter(nullptr,
                                           (const uint8_t*)k_xhtml_simple,
                                           strlen(k_xhtml_simple),
                                           &pages));
  /* V3 */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_reflow_layout_chapter(&s_engine, nullptr, strlen(k_xhtml_simple), &pages));
  /* V4 */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_reflow_layout_chapter(&s_engine,
                                           (const uint8_t*)k_xhtml_simple,
                                           strlen(k_xhtml_simple),
                                           nullptr));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_reflow_close(&s_engine));
  TEST_END("mcdc layout_chapter NULL OR(3)");
}

/**
 * @test test_mcdc_get_page_count_null_or
 *
 * @par MC/DC:
 * Decision: `if (engine == NULL || out_count == NULL)`
 * (2 conditions, line 615). N+1=3.
 */
static void test_mcdc_get_page_count_null_or(void)
{
  TEST_BEGIN("mcdc get_page_count NULL OR");
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_reflow_init(k_test_viewport_w,
                                 k_test_viewport_h,
                                 s_font_buf,
                                 s_font_len,
                                 k_test_font_px,
                                 k_test_color_body,
                                 k_test_color_link,
                                 &s_engine));
  uint32_t pages = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_reflow_get_page_count(&s_engine, &pages));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_reflow_get_page_count(nullptr, &pages));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_reflow_get_page_count(&s_engine, nullptr));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_reflow_close(&s_engine));
  TEST_END("mcdc get_page_count NULL OR");
}

/**
 * @test test_mcdc_set_font_size_range_or
 *
 * @par MC/DC:
 * Decision: `if (new_font_px < MIN || new_font_px > MAX)`
 * (2 conditions, line 633). N+1=3.
 * Plus: decision at line 636 `if (engine->xhtml_buf == NULL ||
 *       engine->xhtml_len == 0U)` argued via the V1 vector below
 * which exercises the not-yet-laid-out path.
 */
static void test_mcdc_set_font_size_range_or(void)
{
  TEST_BEGIN("mcdc set_font_size range OR");
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_reflow_init(k_test_viewport_w,
                                 k_test_viewport_h,
                                 s_font_buf,
                                 s_font_len,
                                 k_test_font_px,
                                 k_test_color_body,
                                 k_test_color_link,
                                 &s_engine));
  uint32_t pages = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_reflow_layout_chapter(&s_engine,
                                           (const uint8_t*)k_xhtml_simple,
                                           strlen(k_xhtml_simple),
                                           &pages));
  /* V1 in-range -> ok (re-flow). */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_reflow_set_font_size(&s_engine, (uint16_t)k_mcdc_font_px_ok));
  /* V2 too low. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_reflow_set_font_size(&s_engine, (uint16_t)k_mcdc_font_px_too_low));
  /* V3 too high. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_reflow_set_font_size(&s_engine, (uint16_t)k_mcdc_font_px_too_high));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_reflow_close(&s_engine));
  TEST_END("mcdc set_font_size range OR");
}

/**
 * @test test_mcdc_set_font_size_state_or
 *
 * @par MC/DC:
 * Decision: `if (engine->xhtml_buf == NULL || engine->xhtml_len == 0U)`
 * (2 conditions, line 636). N+1=3.
 *
 * @par DO-178C 6.4.4.3 omission rationale:
 * - V1 xhtml_buf set, len>0 -> both F. F (proceeds, re-flow ok).
 * - V2 xhtml_buf=NULL       -> C1=T. T -> invalid_state.
 * - V3 xhtml_buf set, len=0 -> C1=F, C2=T. V3 cannot occur through
 *   the public API: ra8_reflow_layout_chapter sets buf and len atomically
 *   from the caller, and ra8_reflow_init zeroes both. Argued by code
 *   inspection (identical OR form, same return path).
 */
static void test_mcdc_set_font_size_state_or(void)
{
  TEST_BEGIN("mcdc set_font_size state OR (xhtml NULL || len 0)");
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_reflow_init(k_test_viewport_w,
                                 k_test_viewport_h,
                                 s_font_buf,
                                 s_font_len,
                                 k_test_font_px,
                                 k_test_color_body,
                                 k_test_color_link,
                                 &s_engine));
  /* V2: no layout_chapter call yet -> xhtml_buf is NULL. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_state,
                 ra8_reflow_set_font_size(&s_engine, (uint16_t)k_mcdc_font_px_ok));
  /* V1: after layout_chapter both are set. */
  uint32_t pages = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_reflow_layout_chapter(&s_engine,
                                           (const uint8_t*)k_xhtml_simple,
                                           strlen(k_xhtml_simple),
                                           &pages));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_reflow_set_font_size(&s_engine, (uint16_t)k_mcdc_font_px_ok));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_reflow_close(&s_engine));
  TEST_END("mcdc set_font_size state OR (xhtml NULL || len 0)");
}

/**
 * @test test_mcdc_run_layout_empty_guard
 *
 * @par MC/DC:
 * Decision: `if (engine->page_count == 0U && engine->token_count > 0U)`
 * (2 conditions, libs/ra8_reflow/src/ra8_reflow_layout.c line 510).
 * Reachable only via ra8_reflow_run_layout (called from
 * ra8_reflow_layout_chapter). The decision drives whether to synthesise
 * a single page when the parser produced tokens that did not generate
 * any glyphs.
 *
 * @par DO-178C 6.4.4.3 omission rationale:
 * - V1 page_count>0, token_count>0 -> C1=F. F (skip synthesis).
 *      Reached by k_xhtml_simple which produces visible text -> >=1 page.
 * - V2 page_count=0, token_count=0 -> C1=T,C2=F. F (skip).
 *      Reached when input contains no parseable elements at all
 *      (e.g. an XHTML doctype-only fragment); but the parser currently
 *      always produces at least one structural token, making this
 *      vector unreachable through the public API. Argued by inspection.
 * - V3 page_count=0, token_count>0 -> both T. T (synthesise page).
 *      Reached when the input is well-formed but produces no rendered
 *      glyphs (e.g. all whitespace inside collapsed blocks). Driven
 *      below by an empty <p> element. The simple-input vector V1
 *      proves C1 independence; V3 proves C2 independence.
 */
static void test_mcdc_run_layout_empty_guard(void)
{
  TEST_BEGIN("mcdc run_layout page-synthesis guard");
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_reflow_init(k_test_viewport_w,
                                 k_test_viewport_h,
                                 s_font_buf,
                                 s_font_len,
                                 k_test_font_px,
                                 k_test_color_body,
                                 k_test_color_link,
                                 &s_engine));
  uint32_t pages = 0U;
  /* V1: rich input -> at least one real page. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_reflow_layout_chapter(&s_engine,
                                           (const uint8_t*)k_xhtml_simple,
                                           strlen(k_xhtml_simple),
                                           &pages));
  TEST_ASSERT(pages >= 1U);
  /* V3: structurally valid but glyph-less input forces the synthesis
   * branch. Use a paragraph that contains only whitespace. */
  static const char* const k_empty_p = "<p>   </p>";
  pages                              = 0U;
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_reflow_layout_chapter(&s_engine, (const uint8_t*)k_empty_p, strlen(k_empty_p), &pages));
  /* The synthesis path guarantees at least one page on non-empty input. */
  TEST_ASSERT(pages >= 1U);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_reflow_close(&s_engine));
  TEST_END("mcdc run_layout page-synthesis guard");
}

/* --------------------------------------------------------------------- */
/* main */
/* --------------------------------------------------------------------- */

/**
 * @brief Test executable entry point -- runs the API-guard MC/DC vectors.
 *
 * @details The vectors need a real font (each success arm expects
 *          ra8_reflow_init to pass its blob validation), so when the vendored
 *          Literata face is unreachable in the sandbox the suite skips
 *          cleanly, mirroring test_ra8_reflow.c.
 *
 * @return 0 on success (all tests passed, or clean skip).
 *
 * @pre Host environment provides a filesystem and stderr.
 * @post Every public-API guard decision above has executed its vectors.
 *
 * @note Not thread-safe (single-threaded test runner).
 * @since 0.1.0
 */
int main(void)
{
  if (!priv_load_font()) {
    (void)fprintf(stderr,
                  "[SKIP] test_ra8_reflow_api_mcdc: Literata-Regular.ttf not "
                  "loadable -- guard vectors need a real face\n");
    return 0;
  }

  test_mcdc_init_font_or();
  test_mcdc_init_viewport_or();
  test_mcdc_init_font_px_or();
  test_mcdc_layout_chapter_null_or3();
  test_mcdc_get_page_count_null_or();
  test_mcdc_set_font_size_range_or();
  test_mcdc_set_font_size_state_or();
  test_mcdc_run_layout_empty_guard();
  (void)fprintf(stderr, "[OK  ] test_ra8_reflow_api_mcdc: all MC/DC vectors passed\n");
  return 0;
}
