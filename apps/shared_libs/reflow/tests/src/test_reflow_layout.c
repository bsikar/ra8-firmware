/**
 * @file test_reflow_layout.c
 * @brief MC/DC unit tests for apps/shared_libs/reflow/src/reflow_layout.c
 * @details Targets reflow layout validation, line construction, geometry limits, and bounded failure behavior with deterministic content.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "reflow.h"
#include "reflow_internal.h"
#include "unity_minimal.h"

typedef enum : uint16_t {
  k_test_reflow_dim_zero  = 0U,   /**< Test reflow dim zero.  */
  k_test_reflow_dim_ok    = 320U, /**< Test reflow dim ok.    */
  k_test_reflow_font_ok   = 16U,  /**< Test reflow font ok.   */
  k_test_reflow_font_low  = 4U,   /**< Test reflow font low.  */
  k_test_reflow_font_high = 200U, /**< Test reflow font high. */
} test_reflow_t;

typedef enum : uint32_t {
  k_test_reflow_color_black = 0x000000FFU, /**< Test reflow color black. */
  k_test_reflow_color_blue  = 0x0000FFFFU, /**< Test reflow color blue.  */
} test_reflow_color_t;

typedef enum : size_t {
  k_test_reflow_font_len_ok = 64U, /**< Test reflow font length ok. */
} test_reflow_len_t;

static const uint8_t s_dummy_font[64];
static reflow_t      s_engine;

/**
 * @brief Verify init with behavior against the reflow contract.
 * @details Exercises the init with path and preserves each documented result and bound.
 * @param[in] w Caller-supplied w value used by the scenario.
 * @param[in] h Caller-supplied h value used by the scenario.
 * @param[in] font Caller-supplied font value used by the scenario.
 * @param[in] font_px Caller-supplied font px value used by the scenario.
 * @param[in,out] out Caller-supplied out value used by the scenario.
 * @return A status code describing the completed reflow operation.
 * @retval k_ra8_ok The operation completed successfully.
 * @retval nonzero Validation or bounded-resource checks rejected the operation.
 * @pre The referenced fixture inputs are valid for this scenario.
 * @post All assertions for the scenario have passed before this function returns.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 * @pre Bounded working storage remains available for the complete operation.
 * @post No state outside the documented outputs is modified by this helper.
 */
RA8_INTERNAL static ra8_err_t
internal_init_with(uint16_t w, uint16_t h, const uint8_t* font, uint16_t font_px, reflow_t* out)
{
  return reflow_init(w,
                     h,
                     font,
                     (size_t)k_test_reflow_font_len_ok,
                     font_px,
                     (uint32_t)k_test_reflow_color_black,
                     (uint32_t)k_test_reflow_color_blue,
                     out);
}

/**
 * @test internal_test_mcdc_reflow_init_null_pair
 *
 * @par MC/DC:
 * Decision: ``if (font_data == NULL || out_engine == NULL)``
 * (2 conditions, apps/shared_libs/reflow/src/reflow_layout.c around line 764)
 * Per DO-178C 6.4.4.3 N+1 = 3 vectors.
 * @brief Verify mcdc reflow init null pair behavior against the reflow contract.
 * @details Exercises the mcdc reflow init null pair path and preserves each documented result and bound.
 * @pre The referenced fixture inputs are valid for this scenario.
 * @pre Fixed-capacity output buffers are initialized before the operation.
 * @post All assertions for the scenario have passed before this function returns.
 * @post Caller-owned fixture storage remains valid for subsequent vectors.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_mcdc_reflow_init_null_pair(void)
{
  TEST_BEGIN("reflow_init MC/DC: (font_data||out_engine) NULL");
  TEST_ASSERT_EQ(k_ra8_ok,
                 internal_init_with((uint16_t)k_test_reflow_dim_ok,
                                    (uint16_t)k_test_reflow_dim_ok,
                                    s_dummy_font,
                                    (uint16_t)k_test_reflow_font_ok,
                                    &s_engine));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 internal_init_with((uint16_t)k_test_reflow_dim_ok,
                                    (uint16_t)k_test_reflow_dim_ok,
                                    nullptr,
                                    (uint16_t)k_test_reflow_font_ok,
                                    &s_engine));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 internal_init_with((uint16_t)k_test_reflow_dim_ok,
                                    (uint16_t)k_test_reflow_dim_ok,
                                    s_dummy_font,
                                    (uint16_t)k_test_reflow_font_ok,
                                    nullptr));
  TEST_END("reflow_init MC/DC: (font_data||out_engine) NULL");
}

/**
 * @test internal_test_mcdc_reflow_init_viewport_zero
 *
 * @par MC/DC:
 * Decision: ``if (viewport_w == 0U || viewport_h == 0U)``
 * (2 conditions, apps/shared_libs/reflow/src/reflow_layout.c around line 767)
 * Per DO-178C 6.4.4.3 N+1 = 3 vectors.
 * @brief Verify mcdc reflow init viewport zero behavior against the reflow contract.
 * @details Exercises the mcdc reflow init viewport zero path and preserves each documented result and bound.
 * @pre The referenced fixture inputs are valid for this scenario.
 * @pre Fixed-capacity output buffers are initialized before the operation.
 * @post All assertions for the scenario have passed before this function returns.
 * @post Caller-owned fixture storage remains valid for subsequent vectors.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_mcdc_reflow_init_viewport_zero(void)
{
  TEST_BEGIN("reflow_init MC/DC: (viewport_w==0 || viewport_h==0)");
  TEST_ASSERT_EQ(k_ra8_ok,
                 internal_init_with((uint16_t)k_test_reflow_dim_ok,
                                    (uint16_t)k_test_reflow_dim_ok,
                                    s_dummy_font,
                                    (uint16_t)k_test_reflow_font_ok,
                                    &s_engine));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 internal_init_with((uint16_t)k_test_reflow_dim_zero,
                                    (uint16_t)k_test_reflow_dim_ok,
                                    s_dummy_font,
                                    (uint16_t)k_test_reflow_font_ok,
                                    &s_engine));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 internal_init_with((uint16_t)k_test_reflow_dim_ok,
                                    (uint16_t)k_test_reflow_dim_zero,
                                    s_dummy_font,
                                    (uint16_t)k_test_reflow_font_ok,
                                    &s_engine));
  TEST_END("reflow_init MC/DC: (viewport_w==0 || viewport_h==0)");
}

/**
 * @test internal_test_mcdc_reflow_init_font_px_range
 *
 * @par MC/DC:
 * Decision: ``if (font_px < k_reflow_min_font_px ||
 *                 font_px > k_reflow_max_font_px)``
 * (2 conditions, apps/shared_libs/reflow/src/reflow_layout.c around line 770)
 * Per DO-178C 6.4.4.3 N+1 = 3 vectors.
 * @brief Verify mcdc reflow init font px range behavior against the reflow contract.
 * @details Exercises the mcdc reflow init font px range path and preserves each documented result and bound.
 * @pre The referenced fixture inputs are valid for this scenario.
 * @pre Fixed-capacity output buffers are initialized before the operation.
 * @post All assertions for the scenario have passed before this function returns.
 * @post Caller-owned fixture storage remains valid for subsequent vectors.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_mcdc_reflow_init_font_px_range(void)
{
  TEST_BEGIN("reflow_init MC/DC: (font_px<min || font_px>max)");
  TEST_ASSERT_EQ(k_ra8_ok,
                 internal_init_with((uint16_t)k_test_reflow_dim_ok,
                                    (uint16_t)k_test_reflow_dim_ok,
                                    s_dummy_font,
                                    (uint16_t)k_test_reflow_font_ok,
                                    &s_engine));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 internal_init_with((uint16_t)k_test_reflow_dim_ok,
                                    (uint16_t)k_test_reflow_dim_ok,
                                    s_dummy_font,
                                    (uint16_t)k_test_reflow_font_low,
                                    &s_engine));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 internal_init_with((uint16_t)k_test_reflow_dim_ok,
                                    (uint16_t)k_test_reflow_dim_ok,
                                    s_dummy_font,
                                    (uint16_t)k_test_reflow_font_high,
                                    &s_engine));
  TEST_END("reflow_init MC/DC: (font_px<min || font_px>max)");
}

/**
 * @test internal_test_mcdc_reflow_set_font_size_no_cached_buf
 *
 * @par MC/DC:
 * Decision (apps/shared_libs/reflow/src/reflow_layout.c):
 *   ``if (engine->xhtml_buf == NULL || engine->xhtml_len == 0U)``
 *   (2 conditions)
 * - Vector 1 (T,T): fresh init, no layout_chapter call yet; both
 *   xhtml_buf and xhtml_len are zero -> returns invalid_state.
 * - Vector 2 (F,F): after a successful layout_chapter the cached
 *   pair is non-NULL/non-zero -> set_font_size proceeds (covered by
 *   test_set_font_size_reflows in tests/src/test_reflow.c).
 * The mixed (T,F)/(F,T) vectors are unreachable: layout_chapter
 * rejects xhtml_len==0 at line 848 before caching, so the cached
 * pair is always (NULL,0) or (non-NULL, non-zero). Recorded as a
 * deactivated condition per DO-178C 6.4.4.3.
 * @brief Verify mcdc reflow set font size no cached buf behavior against the reflow contract.
 * @details Exercises the mcdc reflow set font size no cached buf path and preserves each documented result and bound.
 * @pre The referenced fixture inputs are valid for this scenario.
 * @pre Fixed-capacity output buffers are initialized before the operation.
 * @post All assertions for the scenario have passed before this function returns.
 * @post Caller-owned fixture storage remains valid for subsequent vectors.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_mcdc_reflow_set_font_size_no_cached_buf(void)
{
  TEST_BEGIN("reflow_set_font_size MC/DC: no cached xhtml buffer");
  TEST_ASSERT_EQ(k_ra8_ok,
                 internal_init_with((uint16_t)k_test_reflow_dim_ok,
                                    (uint16_t)k_test_reflow_dim_ok,
                                    s_dummy_font,
                                    (uint16_t)k_test_reflow_font_ok,
                                    &s_engine));
  /* Vector 1: T,T -- xhtml_buf NULL and xhtml_len 0 right after init. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_state,
                 reflow_set_font_size(&s_engine, (uint16_t)k_test_reflow_font_ok));
  TEST_END("reflow_set_font_size MC/DC: no cached xhtml buffer");
}

/**
 * @test internal_test_mcdc_reflow_internal_is_indent_tag
 *
 * @par MC/DC:
 * Decision at apps/shared_libs/reflow/src/reflow_layout.c (helper)
 * which both line 479 (internal_open_block) and line 513 (internal_close_block)
 * delegate to:
 *   ``(tag == k_reflow_tag_li) || (tag == k_reflow_tag_blockquote)``
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
 * @brief Verify mcdc reflow internal is indent tag behavior against the reflow contract.
 * @details Exercises the mcdc reflow internal is indent tag path and preserves each documented result and bound.
 * @pre The referenced fixture inputs are valid for this scenario.
 * @pre Fixed-capacity output buffers are initialized before the operation.
 * @post All assertions for the scenario have passed before this function returns.
 * @post Caller-owned fixture storage remains valid for subsequent vectors.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_mcdc_reflow_internal_is_indent_tag(void)
{
  TEST_BEGIN("reflow MC/DC: is_indent_tag OR");
  TEST_ASSERT(priv_reflow_internal_is_indent_tag((uint8_t)k_reflow_tag_li));
  TEST_ASSERT(priv_reflow_internal_is_indent_tag((uint8_t)k_reflow_tag_blockquote));
  TEST_ASSERT(!priv_reflow_internal_is_indent_tag((uint8_t)k_reflow_tag_h1));
  TEST_ASSERT(!priv_reflow_internal_is_indent_tag((uint8_t)k_reflow_tag_p));
  TEST_END("reflow MC/DC: is_indent_tag OR");
}

/**
 * @test internal_test_mcdc_reflow_internal_right_overflow_break
 *
 * @par MC/DC:
 * Decision at apps/shared_libs/reflow/src/reflow_layout.c (helper) which
 * the inline call sites at lines 404, 468, 605 delegate to:
 *   ``(cur->x + advance > right_limit) && (line_has_content != 0)``
 *   (2 conditions, AND). Direct-call vectors:
 * - V1: x+adv <= right, content=1 -> false (both false-side)
 * - V2: x+adv >  right, content=1 -> true  (varies left)
 * - V3: x+adv >  right, content=0 -> false (varies right)
 * V1+V2 isolate left; V2+V3 isolate right. N+1 = 3.
 * @brief Verify mcdc reflow internal right overflow break behavior against the reflow contract.
 * @details Exercises the mcdc reflow internal right overflow break path and preserves each documented result and bound.
 * @pre The referenced fixture inputs are valid for this scenario.
 * @pre Fixed-capacity output buffers are initialized before the operation.
 * @post All assertions for the scenario have passed before this function returns.
 * @post Caller-owned fixture storage remains valid for subsequent vectors.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_mcdc_reflow_internal_right_overflow_break(void)
{
  TEST_BEGIN("reflow MC/DC: right_overflow_break AND");
  TEST_ASSERT(!priv_reflow_internal_right_overflow_break(0, 10, 100, 1U));
  TEST_ASSERT(priv_reflow_internal_right_overflow_break(95, 10, 100, 1U));
  TEST_ASSERT(!priv_reflow_internal_right_overflow_break(95, 10, 100, 0U));
  TEST_END("reflow MC/DC: right_overflow_break AND");
}

/**
 * @test internal_test_mcdc_reflow_internal_xhtml_invalid
 *
 * @par MC/DC:
 * Decision at apps/shared_libs/reflow/src/reflow_layout.c (helper) which
 * the inline call site at line 953 delegates to:
 *   ``(xhtml_buf == NULL) || (xhtml_len == 0)`` (2 conditions, OR).
 * - V1: buf!=NULL, len!=0 -> false
 * - V2: buf==NULL, len!=0 -> true
 * - V3: buf!=NULL, len==0 -> true
 * V1+V2 isolate buf; V1+V3 isolate len. N+1 = 3.
 * @brief Verify mcdc reflow internal xhtml invalid behavior against the reflow contract.
 * @details Exercises the mcdc reflow internal xhtml invalid path and preserves each documented result and bound.
 * @pre The referenced fixture inputs are valid for this scenario.
 * @pre Fixed-capacity output buffers are initialized before the operation.
 * @post All assertions for the scenario have passed before this function returns.
 * @post Caller-owned fixture storage remains valid for subsequent vectors.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_mcdc_reflow_internal_xhtml_invalid(void)
{
  TEST_BEGIN("reflow MC/DC: xhtml_invalid OR");
  static const uint8_t buf[1] = {};
  TEST_ASSERT(!priv_reflow_internal_xhtml_invalid(buf, 1U));
  TEST_ASSERT(priv_reflow_internal_xhtml_invalid(nullptr, 1U));
  TEST_ASSERT(priv_reflow_internal_xhtml_invalid(buf, 0U));
  TEST_END("reflow MC/DC: xhtml_invalid OR");
}

/**
 * @test internal_test_mcdc_reflow_internal_final_page_needed
 *
 * @par MC/DC:
 * Decision at apps/shared_libs/reflow/src/reflow_layout.c (helper) which
 * the inline call site at line 750 delegates to:
 *   ``(page_count == 0) && (token_count > 0)`` (2 conditions, AND).
 * - V1: pages>0,  tokens>0  -> false
 * - V2: pages==0, tokens>0  -> true
 * - V3: pages==0, tokens==0 -> false
 * V1+V2 isolate pages; V2+V3 isolate tokens. N+1 = 3.
 * @brief Verify mcdc reflow internal final page needed behavior against the reflow contract.
 * @details Exercises the mcdc reflow internal final page needed path and preserves each documented result and bound.
 * @pre The referenced fixture inputs are valid for this scenario.
 * @pre Fixed-capacity output buffers are initialized before the operation.
 * @post All assertions for the scenario have passed before this function returns.
 * @post Caller-owned fixture storage remains valid for subsequent vectors.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_mcdc_reflow_internal_final_page_needed(void)
{
  TEST_BEGIN("reflow MC/DC: final_page_needed AND");
  TEST_ASSERT(!priv_reflow_internal_final_page_needed(1U, 5U));
  TEST_ASSERT(priv_reflow_internal_final_page_needed(0U, 5U));
  TEST_ASSERT(!priv_reflow_internal_final_page_needed(0U, 0U));
  TEST_END("reflow MC/DC: final_page_needed AND");
}

int main(void)
{
  internal_test_mcdc_reflow_init_null_pair();
  internal_test_mcdc_reflow_init_viewport_zero();
  internal_test_mcdc_reflow_init_font_px_range();
  internal_test_mcdc_reflow_set_font_size_no_cached_buf();
  internal_test_mcdc_reflow_internal_is_indent_tag();
  internal_test_mcdc_reflow_internal_right_overflow_break();
  internal_test_mcdc_reflow_internal_xhtml_invalid();
  internal_test_mcdc_reflow_internal_final_page_needed();
  return 0;
}
