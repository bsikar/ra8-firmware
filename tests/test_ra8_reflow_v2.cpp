/**
 * @file test_ra8_reflow_v2.cpp
 * @brief Host-side unit tests for the LiteHTML-backed ra8_reflow v2 engine.
 *
 * @par Tag
 * [Ring 4 / Reflow] {World: NS}
 *
 * @details
 * Exercises the public ra8_reflow API against the v2 implementation
 * (``libs/ra8_reflow/v2/ra8_reflow_v2.cpp``). Built only when
 * ``-DRA8_REFLOW_USE_LITEHTML=ON``.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

extern "C" {
#include "ra8_err.h"
#include "ra8_reflow.h"
}

namespace {

typedef enum : uint8_t {
  k_test_paragraph_count = 40U, /**< Paragraphs in the pagination fixture. */
  k_test_viewport_count  = 5U,  /**< Number of viewport heights measured.  */
} test_dim_t;

typedef enum : uint8_t {
  k_test_body_color = 0x000000U, /**< Body text colour (black, 0x000000). */
  k_test_link_color = 0x0000FFU, /**< Link colour (blue, 0x0000FF).       */
} test_color_t;

const uint8_t k_dummy_font[32] = {
  0x00, 0x01, 0x00, 0x00, 0x00, 0x0A, 0x00, 0x80, 0x00, 0x03, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

ra8_reflow_t s_engine;

ra8_err_t init_default(uint16_t w, uint16_t h)
{
  return ra8_reflow_init(w,
                         h,
                         k_dummy_font,
                         sizeof(k_dummy_font),
                         (uint16_t)k_ra8_reflow_default_font_px,
                         (uint32_t)k_test_body_color,
                         (uint32_t)k_test_link_color,
                         &s_engine);
}

void test_empty_body(void)
{
  std::printf("test_empty_body: ");
  std::memset(&s_engine, 0, sizeof(s_engine));

  assert(init_default(480U, 240U) == k_ra8_ok);

  uint32_t          pages = 0U;
  const std::string ws    = " ";
  const ra8_err_t   err   = ra8_reflow_layout_chapter(&s_engine,
                                                      reinterpret_cast<const uint8_t*>(ws.data()),
                                                      ws.size(),
                                                      &pages);
  assert(err == k_ra8_ok);
  assert(pages >= 1U);

  uint32_t reported = 0U;
  assert(ra8_reflow_get_page_count(&s_engine, &reported) == k_ra8_ok);
  assert(reported == pages);

  assert(ra8_reflow_close(&s_engine) == k_ra8_ok);
  std::printf("ok (pages=%u)\n", (unsigned)pages);
}

void test_single_paragraph(void)
{
  std::printf("test_single_paragraph: ");
  std::memset(&s_engine, 0, sizeof(s_engine));

  assert(init_default(480U, 240U) == k_ra8_ok);

  const std::string body  = "<p>The quick brown fox jumps over the lazy dog. "
                            "Pack my box with five dozen liquor jugs. "
                            "How vexingly quick daft zebras jump.</p>";
  uint32_t          pages = 0U;
  const ra8_err_t   err   = ra8_reflow_layout_chapter(&s_engine,
                                                      reinterpret_cast<const uint8_t*>(body.data()),
                                                      body.size(),
                                                      &pages);
  assert(err == k_ra8_ok);
  assert(pages >= 1U);

  assert(ra8_reflow_render_page(&s_engine, 0U, nullptr) == k_ra8_ok);
  assert(ra8_reflow_render_page(&s_engine, pages, nullptr) == k_ra8_err_out_of_range);

  assert(ra8_reflow_close(&s_engine) == k_ra8_ok);
  std::printf("ok (pages=%u)\n", (unsigned)pages);
}

void test_paragraph_and_image(void)
{
  std::printf("test_paragraph_and_image: ");
  std::memset(&s_engine, 0, sizeof(s_engine));

  assert(init_default(320U, 240U) == k_ra8_ok);

  const std::string body  = "<p>Inline figure follows.</p>"
                            "<p><img src=\"figure.png\" alt=\"figure\"/></p>"
                            "<p>Caption text continues after the inline figure.</p>";
  uint32_t          pages = 0U;
  const ra8_err_t   err   = ra8_reflow_layout_chapter(&s_engine,
                                                      reinterpret_cast<const uint8_t*>(body.data()),
                                                      body.size(),
                                                      &pages);
  assert(err == k_ra8_ok);
  assert(pages >= 1U);

  assert(ra8_reflow_close(&s_engine) == k_ra8_ok);
  std::printf("ok (pages=%u)\n", (unsigned)pages);
}

void test_paginate_across_viewports(void)
{
  std::printf("test_paginate_across_viewports: ");

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
    assert(init_default(320U, heights[i]) == k_ra8_ok);

    uint32_t        pages = 0U;
    const ra8_err_t err   = ra8_reflow_layout_chapter(&s_engine,
                                                      reinterpret_cast<const uint8_t*>(body.data()),
                                                      body.size(),
                                                      &pages);
    assert(err == k_ra8_ok);
    assert(pages >= 1U);
    page_counts[i] = pages;

    assert(ra8_reflow_close(&s_engine) == k_ra8_ok);
  }

  for (size_t i = 1; i < (size_t)k_test_viewport_count; ++i) {
    assert(page_counts[i] <= page_counts[i - 1]);
  }
  /* The smallest viewport must yield strictly more pages than the
   * largest one when the document is non-trivial. */
  assert(page_counts[0] > page_counts[k_test_viewport_count - 1]);

  std::printf("ok (pages: %u, %u, %u, %u, %u)\n",
              (unsigned)page_counts[0],
              (unsigned)page_counts[1],
              (unsigned)page_counts[2],
              (unsigned)page_counts[3],
              (unsigned)page_counts[4]);
}

} // namespace

int main(void)
{
  test_empty_body();
  test_single_paragraph();
  test_paragraph_and_image();
  test_paginate_across_viewports();
  std::printf("test_ra8_reflow_v2: all passed\n");
  return 0;
}
