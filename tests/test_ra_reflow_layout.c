/**
 * @file test_ra_reflow_layout.c
 * @brief MC/DC unit tests for libs/ra_reflow/src/ra_reflow_layout.c
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>

#include "ra_err.h"
#include "ra_reflow.h"
#include "ra_reflow_internal.h"
#include "unity_minimal.h"

typedef enum : uint16_t {
  k_test_reflow_dim_zero  = 0U,
  k_test_reflow_dim_ok    = 320U,
  k_test_reflow_font_ok   = 16U,
  k_test_reflow_font_low  = 4U,
  k_test_reflow_font_high = 200U,
} test_reflow_t;

typedef enum : uint32_t {
  k_test_reflow_color_black = 0x000000FFU,
  k_test_reflow_color_blue  = 0x0000FFFFU,
} test_reflow_color_t;

typedef enum : size_t {
  k_test_reflow_font_len_ok = 64U,
} test_reflow_len_t;

static const uint8_t s_dummy_font[64];
static ra_reflow_t   s_engine;

static ra_err_t
init_with(uint16_t w, uint16_t h, const uint8_t* font, uint16_t font_px, ra_reflow_t* out)
{
  return ra_reflow_init(w,
                        h,
                        font,
                        (size_t)k_test_reflow_font_len_ok,
                        font_px,
                        (uint32_t)k_test_reflow_color_black,
                        (uint32_t)k_test_reflow_color_blue,
                        out);
}

/**
 * @test test_mcdc_reflow_init_null_pair
 *
 * @par MC/DC:
 * Decision: ``if (font_data == NULL || out_engine == NULL)``
 * (2 conditions, libs/ra_reflow/src/ra_reflow_layout.c around line 764)
 * Per DO-178C 6.4.4.3 N+1 = 3 vectors.
 */
static void test_mcdc_reflow_init_null_pair(void)
{
  TEST_BEGIN("reflow_init MC/DC: (font_data||out_engine) NULL");
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)init_with((uint16_t)k_test_reflow_dim_ok,
                                    (uint16_t)k_test_reflow_dim_ok,
                                    s_dummy_font,
                                    (uint16_t)k_test_reflow_font_ok,
                                    &s_engine));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)init_with((uint16_t)k_test_reflow_dim_ok,
                                    (uint16_t)k_test_reflow_dim_ok,
                                    NULL,
                                    (uint16_t)k_test_reflow_font_ok,
                                    &s_engine));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)init_with((uint16_t)k_test_reflow_dim_ok,
                                    (uint16_t)k_test_reflow_dim_ok,
                                    s_dummy_font,
                                    (uint16_t)k_test_reflow_font_ok,
                                    NULL));
  TEST_END("reflow_init MC/DC: (font_data||out_engine) NULL");
}

/**
 * @test test_mcdc_reflow_init_viewport_zero
 *
 * @par MC/DC:
 * Decision: ``if (viewport_w == 0U || viewport_h == 0U)``
 * (2 conditions, libs/ra_reflow/src/ra_reflow_layout.c around line 767)
 * Per DO-178C 6.4.4.3 N+1 = 3 vectors.
 */
static void test_mcdc_reflow_init_viewport_zero(void)
{
  TEST_BEGIN("reflow_init MC/DC: (viewport_w==0 || viewport_h==0)");
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)init_with((uint16_t)k_test_reflow_dim_ok,
                                    (uint16_t)k_test_reflow_dim_ok,
                                    s_dummy_font,
                                    (uint16_t)k_test_reflow_font_ok,
                                    &s_engine));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)init_with((uint16_t)k_test_reflow_dim_zero,
                                    (uint16_t)k_test_reflow_dim_ok,
                                    s_dummy_font,
                                    (uint16_t)k_test_reflow_font_ok,
                                    &s_engine));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)init_with((uint16_t)k_test_reflow_dim_ok,
                                    (uint16_t)k_test_reflow_dim_zero,
                                    s_dummy_font,
                                    (uint16_t)k_test_reflow_font_ok,
                                    &s_engine));
  TEST_END("reflow_init MC/DC: (viewport_w==0 || viewport_h==0)");
}

/**
 * @test test_mcdc_reflow_init_font_px_range
 *
 * @par MC/DC:
 * Decision: ``if (font_px < k_ra_reflow_min_font_px ||
 *                 font_px > k_ra_reflow_max_font_px)``
 * (2 conditions, libs/ra_reflow/src/ra_reflow_layout.c around line 770)
 * Per DO-178C 6.4.4.3 N+1 = 3 vectors.
 */
static void test_mcdc_reflow_init_font_px_range(void)
{
  TEST_BEGIN("reflow_init MC/DC: (font_px<min || font_px>max)");
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)init_with((uint16_t)k_test_reflow_dim_ok,
                                    (uint16_t)k_test_reflow_dim_ok,
                                    s_dummy_font,
                                    (uint16_t)k_test_reflow_font_ok,
                                    &s_engine));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)init_with((uint16_t)k_test_reflow_dim_ok,
                                    (uint16_t)k_test_reflow_dim_ok,
                                    s_dummy_font,
                                    (uint16_t)k_test_reflow_font_low,
                                    &s_engine));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)init_with((uint16_t)k_test_reflow_dim_ok,
                                    (uint16_t)k_test_reflow_dim_ok,
                                    s_dummy_font,
                                    (uint16_t)k_test_reflow_font_high,
                                    &s_engine));
  TEST_END("reflow_init MC/DC: (font_px<min || font_px>max)");
}

/**
 * @test test_mcdc_reflow_set_font_size_no_cached_buf
 *
 * @par MC/DC:
 * Decision (libs/ra_reflow/src/ra_reflow_layout.c:922):
 *   ``if (engine->xhtml_buf == NULL || engine->xhtml_len == 0U)``
 *   (2 conditions)
 * - Vector 1 (T,T): fresh init, no layout_chapter call yet; both
 *   xhtml_buf and xhtml_len are zero -> returns invalid_state.
 * - Vector 2 (F,F): after a successful layout_chapter the cached
 *   pair is non-NULL/non-zero -> set_font_size proceeds (covered by
 *   test_set_font_size_reflows in tests/test_ra_reflow.c).
 * The mixed (T,F)/(F,T) vectors are unreachable: layout_chapter
 * rejects xhtml_len==0 at line 848 before caching, so the cached
 * pair is always (NULL,0) or (non-NULL, non-zero). Recorded as a
 * deactivated condition per DO-178C 6.4.4.3.
 */
static void test_mcdc_reflow_set_font_size_no_cached_buf(void)
{
  TEST_BEGIN("reflow_set_font_size MC/DC: no cached xhtml buffer");
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)init_with((uint16_t)k_test_reflow_dim_ok,
                                    (uint16_t)k_test_reflow_dim_ok,
                                    s_dummy_font,
                                    (uint16_t)k_test_reflow_font_ok,
                                    &s_engine));
  /* Vector 1: T,T -- xhtml_buf NULL and xhtml_len 0 right after init. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_state,
                 (int32_t)ra_reflow_set_font_size(&s_engine, (uint16_t)k_test_reflow_font_ok));
  TEST_END("reflow_set_font_size MC/DC: no cached xhtml buffer");
}

/**
 * @test test_mcdc_reflow_internal_is_indent_tag
 *
 * @par MC/DC:
 * Decision at libs/ra_reflow/src/ra_reflow_layout.c:42 (helper)
 * which both line 479 (priv_open_block) and line 513 (priv_close_block)
 * delegate to:
 *   ``(tag == k_ra_reflow_tag_li) || (tag == k_ra_reflow_tag_blockquote)``
 *   (2 conditions, OR). A single MC/DC vector set on the underlying
 *   helper covers both production call sites.
 *
 * - V1: tag = li         -> C1=T  (short-circuits) -> true.
 * - V2: tag = blockquote -> C1=F C2=T              -> true.
 * - V3: tag = h1         -> C1=F C2=F              -> false.
 * V1+V3 isolate C1; V2+V3 isolate C2. N+1 = 3 vectors: minimal MC/DC.
 *
 * @par DO-178C 6.4.4.3 rationale:
 * 2-condition OR; N+1 = 3 vectors satisfy MC/DC fully.
 */
static void test_mcdc_reflow_internal_is_indent_tag(void)
{
  TEST_BEGIN("reflow MC/DC: is_indent_tag OR");
  TEST_ASSERT(ra_reflow_internal_is_indent_tag((uint8_t)k_ra_reflow_tag_li));
  TEST_ASSERT(ra_reflow_internal_is_indent_tag((uint8_t)k_ra_reflow_tag_blockquote));
  TEST_ASSERT(!ra_reflow_internal_is_indent_tag((uint8_t)k_ra_reflow_tag_h1));
  TEST_ASSERT(!ra_reflow_internal_is_indent_tag((uint8_t)k_ra_reflow_tag_p));
  TEST_END("reflow MC/DC: is_indent_tag OR");
}

int32_t main(void)
{
  test_mcdc_reflow_init_null_pair();
  test_mcdc_reflow_init_viewport_zero();
  test_mcdc_reflow_init_font_px_range();
  test_mcdc_reflow_set_font_size_no_cached_buf();
  test_mcdc_reflow_internal_is_indent_tag();
  (void)fprintf(stderr, "[OK ] test_ra_reflow_layout.c\n");
  return 0;
}
