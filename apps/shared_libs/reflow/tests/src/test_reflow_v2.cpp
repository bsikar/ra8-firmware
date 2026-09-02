/**
 * @file test_reflow_v2.cpp
 * @brief Host-side unit tests for the LiteHTML-backed reflow v2 engine.
 *
 * @par Tag
 * [Ring 4 / Reflow] {World: NS}
 *
 * @details
 * Exercises the public reflow API against the v2 implementation
 * (``apps/shared_libs/reflow/v2/src/reflow_v2.cpp``). Built only when
 * ``-DREFLOW_USE_LITEHTML=ON``.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <cassert>
#include <cstdint>
#include <cstring>
#include <string>

extern "C" {
#include "ra8_attributes.h"
#include "ra8_err.h"
#include "reflow.h"
}

#include "ra8_test_output.h"

namespace {
// NOLINTBEGIN(readability-static-definition-in-anonymous-namespace) -- the
// RA8_INTERNAL contract requires literal static linkage so the annotations
// gate can verify it; the anonymous namespace alone does not satisfy it.

typedef enum : uint8_t {
  k_test_paragraph_count = 40U, /**< Paragraphs in the pagination fixture. */
  k_test_viewport_count  = 5U,  /**< Number of viewport heights measured.  */
} test_dim_t;

typedef enum : uint8_t {
  k_test_body_color = 0x000000U, /**< Body text colour (black, 0x000000). */
  k_test_link_color = 0x0000FFU, /**< Link colour (blue, 0x0000FF).       */
} test_color_t;

const uint8_t s_dummy_font[32] = {
  0x00, 0x01, 0x00, 0x00, 0x00, 0x0A, 0x00, 0x80, 0x00, 0x03, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

reflow_t s_engine;

/**
 * @brief Write one test label to raw result descriptor 1.
 * @details Delegates one already formatted label to the caller-local
 * descriptor convenience edge without installing stream state.
 * @param[in] label NUL-terminated test label.
 * @pre @p label remains readable for the complete call.
 * @pre Raw descriptor 1 is available when result output must be observed.
 * @post The complete label has been attempted without its terminator.
 * @post No descriptor is closed and no global sink is installed.
 * @note Destination failure does not change the assertion verdict.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_write_label(const char* label)
{
  (void)internal_test_output_fd_text(STDOUT_FILENO, label);
}

/**
 * @brief Write one `ok (pages=N)` completion line.
 * @details Composes the fixed label and unsigned page count through one
 * caller-local descriptor handle.
 * @param[in] pages Observed chapter page count.
 * @pre @p pages was returned by a successful layout operation.
 * @pre Raw descriptor 1 is available when result output must be observed.
 * @post One complete page-count line has been attempted.
 * @post No descriptor is closed and no global sink is installed.
 * @note Destination failure does not change the assertion verdict.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_write_pages(uint32_t pages)
{
  ra8_test_output_t    output = {};
  ra8_test_output_fd_t state  = {};
  (void)internal_test_output_fd_init(&output, &state, STDOUT_FILENO);
  (void)internal_test_output_text(&output, "ok (pages=");
  (void)internal_test_output_u64(&output, pages);
  (void)internal_test_output_text(&output, ")\n");
}

/**
 * @brief Write the viewport pagination vector as one result line.
 * @details Composes every ordered viewport result with fixed separators through
 * a single caller-local descriptor handle.
 * @param[in] page_counts Complete viewport page-count vector.
 * @pre @p page_counts spans ::k_test_viewport_count readable values.
 * @pre Raw descriptor 1 is available when result output must be observed.
 * @post One complete comma-separated pagination line has been attempted.
 * @post The caller's page-count vector remains unchanged.
 * @note Destination failure does not change the assertion verdict.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_write_page_counts(const uint32_t* page_counts)
{
  ra8_test_output_t    output = {};
  ra8_test_output_fd_t state  = {};
  (void)internal_test_output_fd_init(&output, &state, STDOUT_FILENO);
  (void)internal_test_output_text(&output, "ok (pages: ");
  for (size_t index = 0U; index < (size_t)k_test_viewport_count; ++index) {
    if (index != 0U) {
      (void)internal_test_output_text(&output, ", ");
    }
    (void)internal_test_output_u64(&output, page_counts[index]);
  }
  (void)internal_test_output_text(&output, ")\n");
}

/**
 * @brief Verify init default behavior against the reflow contract.
 * @details Exercises the init default path and preserves each documented result and bound.
 * @param[in] w Caller-supplied w value used by the scenario.
 * @param[in] h Caller-supplied h value used by the scenario.
 * @return Reflow-engine initialization status.
 * @pre The referenced fixtures and fixed-capacity buffers are valid.
 * @post All assertions for the scenario have passed before this function returns.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 * @pre Bounded working storage remains available for the complete operation.
 * @post No state outside the documented outputs is modified by this helper.
 * @retval k_ra8_ok The default fixture initialized the engine.
 * @retval nonzero The fixture or engine parameters were rejected.
 */
RA8_INTERNAL static ra8_err_t internal_init_default(uint16_t w, uint16_t h)
{
  return reflow_init(w,
                     h,
                     s_dummy_font,
                     sizeof(s_dummy_font),
                     (uint16_t)k_reflow_default_font_px,
                     (uint32_t)k_test_body_color,
                     (uint32_t)k_test_link_color,
                     &s_engine);
}

/**
 * @brief Verify empty body behavior against the reflow contract.
 * @details Exercises the empty body path and preserves each documented result and bound.
 * @pre The referenced fixtures and fixed-capacity buffers are valid.
 * @post All assertions for the scenario have passed before this function returns.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 * @pre Bounded working storage remains available for the complete operation.
 * @post No state outside the documented outputs is modified by this helper.
 */
RA8_INTERNAL static void internal_test_empty_body(void)
{
  internal_write_label("test_empty_body: ");
  std::memset(&s_engine, 0, sizeof(s_engine));

  assert(internal_init_default(480U, 240U) == k_ra8_ok);

  uint32_t          pages = 0U;
  const std::string ws    = " ";
  const ra8_err_t   err   = reflow_layout_chapter(&s_engine,
                                                  reinterpret_cast<const uint8_t*>(ws.data()),
                                                  ws.size(),
                                                  &pages);
  assert(err == k_ra8_ok);
  assert(pages >= 1U);

  uint32_t reported = 0U;
  assert(reflow_get_page_count(&s_engine, &reported) == k_ra8_ok);
  assert(reported == pages);

  assert(reflow_close(&s_engine) == k_ra8_ok);
  internal_write_pages(pages);
}

/**
 * @brief Verify single paragraph behavior against the reflow contract.
 * @details Exercises the single paragraph path and preserves each documented result and bound.
 * @pre The referenced fixtures and fixed-capacity buffers are valid.
 * @post All assertions for the scenario have passed before this function returns.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 * @pre Bounded working storage remains available for the complete operation.
 * @post No state outside the documented outputs is modified by this helper.
 */
RA8_INTERNAL static void internal_test_single_paragraph(void)
{
  internal_write_label("test_single_paragraph: ");
  std::memset(&s_engine, 0, sizeof(s_engine));

  assert(internal_init_default(480U, 240U) == k_ra8_ok);

  const std::string body  = "<p>The quick brown fox jumps over the lazy dog. "
                            "Pack my box with five dozen liquor jugs. "
                            "How vexingly quick daft zebras jump.</p>";
  uint32_t          pages = 0U;
  const ra8_err_t   err   = reflow_layout_chapter(&s_engine,
                                                  reinterpret_cast<const uint8_t*>(body.data()),
                                                  body.size(),
                                                  &pages);
  assert(err == k_ra8_ok);
  assert(pages >= 1U);

  assert(reflow_render_page(&s_engine, 0U, nullptr) == k_ra8_ok);
  assert(reflow_render_page(&s_engine, pages, nullptr) == k_ra8_err_out_of_range);

  assert(reflow_close(&s_engine) == k_ra8_ok);
  internal_write_pages(pages);
}

/**
 * @brief Verify paragraph and image behavior against the reflow contract.
 * @details Exercises the paragraph and image path and preserves each documented result and bound.
 * @pre The referenced fixtures and fixed-capacity buffers are valid.
 * @post All assertions for the scenario have passed before this function returns.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 * @pre Bounded working storage remains available for the complete operation.
 * @post No state outside the documented outputs is modified by this helper.
 */
RA8_INTERNAL static void internal_test_paragraph_and_image(void)
{
  internal_write_label("test_paragraph_and_image: ");
  std::memset(&s_engine, 0, sizeof(s_engine));

  assert(internal_init_default(320U, 240U) == k_ra8_ok);

  const std::string body  = "<p>Inline figure follows.</p>"
                            "<p><img src=\"figure.png\" alt=\"figure\"/></p>"
                            "<p>Caption text continues after the inline figure.</p>";
  uint32_t          pages = 0U;
  const ra8_err_t   err   = reflow_layout_chapter(&s_engine,
                                                  reinterpret_cast<const uint8_t*>(body.data()),
                                                  body.size(),
                                                  &pages);
  assert(err == k_ra8_ok);
  assert(pages >= 1U);

  assert(reflow_close(&s_engine) == k_ra8_ok);
  internal_write_pages(pages);
}

/**
 * @brief Verify paginate across viewports behavior against the reflow contract.
 * @details Exercises the paginate across viewports path and preserves each documented result and bound.
 * @pre The referenced fixtures and fixed-capacity buffers are valid.
 * @post All assertions for the scenario have passed before this function returns.
 * @note Test helpers use caller-owned or fixed-capacity fixture storage.
 * @since 0.1.0
 * @pre Bounded working storage remains available for the complete operation.
 * @post No state outside the documented outputs is modified by this helper.
 */
RA8_INTERNAL static void internal_test_paginate_across_viewports(void)
{
  internal_write_label("test_paginate_across_viewports: ");

  std::string body = "<div>";
  for (int i = 0; i < (int)k_test_paragraph_count; ++i) {
    body.append("<p>Paragraph ");
    body.append(std::to_string(i));
    body.append(" with enough words to wrap onto a couple of lines on a "
                "narrow viewport. Lorem ipsum dolor sit amet.</p>");
  }
  body.append("</div>");

  const uint16_t heights[k_test_viewport_count]     = {64U, 96U, 160U, 240U, 480U};
  uint32_t       page_counts[k_test_viewport_count] = {0U, 0U, 0U, 0U, 0U};

  for (size_t i = 0; i < (size_t)k_test_viewport_count; ++i) {
    std::memset(&s_engine, 0, sizeof(s_engine));
    assert(internal_init_default(320U, heights[i]) == k_ra8_ok);

    uint32_t        pages = 0U;
    const ra8_err_t err   = reflow_layout_chapter(&s_engine,
                                                  reinterpret_cast<const uint8_t*>(body.data()),
                                                  body.size(),
                                                  &pages);
    assert(err == k_ra8_ok);
    assert(pages >= 1U);
    page_counts[i] = pages;

    assert(reflow_close(&s_engine) == k_ra8_ok);
  }

  for (size_t i = 1; i < (size_t)k_test_viewport_count; ++i) {
    assert(page_counts[i] <= page_counts[i - 1]);
  }
  /* The smallest viewport must yield strictly more pages than the
   * largest one when the document is non-trivial. */
  assert(page_counts[0] > page_counts[k_test_viewport_count - 1]);

  internal_write_page_counts(page_counts);
}

// NOLINTEND(readability-static-definition-in-anonymous-namespace)
} // namespace

/**
 * @brief Run every reflow-v2 integration scenario.
 * @details Exercises empty, paragraph, image, and viewport-pagination paths and checks their observable results.
 * @return Process status after completing the integration scenarios.
 * @retval 0 Every reflow-v2 assertion passed.
 * @pre The vendored font fixture is readable by the test process.
 * @pre Fixed-capacity page-count storage is available for every viewport.
 * @post Every integration assertion has passed before returning.
 * @post The diagnostic sink has received the final page-count summary.
 * @note The test binds only caller-owned fixture and result storage.
 * @since 0.1.0
 */
int main(void)
{
  internal_test_empty_body();
  internal_test_single_paragraph();
  internal_test_paragraph_and_image();
  internal_test_paginate_across_viewports();
  internal_write_label("test_reflow_v2: all passed\n");
  return 0;
}
