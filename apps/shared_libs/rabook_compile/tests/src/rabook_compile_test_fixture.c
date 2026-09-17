/**
 * @file rabook_compile_test_fixture.c
 * @brief Shared RABOOK1 compiler fixture implementation.
 *
 * @details
 * Maps caller-owned arenas into the production builder and constructs one small
 * canonical book shared by resident and external-stream test executables.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "rabook_compile_test_fixture.h"

#include <string.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "unity_minimal.h"

RA8_TEST_HELPER
ra8_rabook_buffers_t ra8_test_rabook_buffers(ra8_test_rabook_fixture_t* fixture)
{
  TEST_ASSERT_NOT_NULL(fixture);
  const ra8_rabook_buffers_t buf = {
    .chapters       = fixture->chapters,
    .chapter_cap    = (uint32_t)k_ra8_test_rabook_chapter_cap,
    .nodes          = fixture->nodes,
    .node_cap       = (uint32_t)k_ra8_test_rabook_node_cap,
    .attrs          = fixture->attrs,
    .attr_cap       = (uint32_t)k_ra8_test_rabook_attr_cap,
    .stylesheets    = fixture->styles,
    .stylesheet_cap = (uint32_t)k_ra8_test_rabook_style_cap,
    .images         = fixture->images,
    .image_cap      = (uint32_t)k_ra8_test_rabook_image_cap,
    .string_pool    = fixture->strpool,
    .string_cap     = (uint32_t)k_ra8_test_rabook_string_cap,
    .image_pool     = fixture->imgpool,
    .image_pool_cap = (uint32_t)k_ra8_test_rabook_imgpool_cap,
    .out            = fixture->out,
    .out_cap        = (uint32_t)k_ra8_test_rabook_out_cap,
  };
  return buf;
}

RA8_TEST_HELPER void ra8_test_rabook_init(ra8_test_rabook_fixture_t* fixture, ra8_rabook_ctx_t* ctx)
{
  TEST_ASSERT_NOT_NULL(ctx);
  const ra8_rabook_buffers_t buf = ra8_test_rabook_buffers(fixture);
  TEST_ASSERT_EQ(k_ra8_ok, rabook_compile_init(ctx, &buf));
}

/**
 * @brief Append the two-byte cover through the selected pool API.
 * @details Uses resident bytes directly or reserves their logical external span
 *          and copies the fixture payload into the external backing array.
 * @param[in,out] fixture Caller-owned fixture storage.
 * @param[in,out] ctx Initialized compiler context.
 * @param[in] external_image_pool Select external reservation when true.
 * @return Appended image descriptor index.
 * @retval uint32_t Valid cover-image index.
 * @pre @p fixture and @p ctx are non-NULL.
 * @pre The image descriptor and selected pool have capacity for two bytes.
 * @post One cover descriptor is appended.
 * @post External mode initializes the reserved backing bytes.
 * @note Test-only fixture construction helper.
 * @since 0.1.0
 */
RA8_INTERNAL
static uint32_t internal_add_cover(ra8_test_rabook_fixture_t* fixture,
                                   ra8_rabook_ctx_t*          ctx,
                                   bool                       external_image_pool)
{
  static const uint8_t s_img_bytes[2] = {0xF0U, 0x0FU};
  const uint32_t       img_href       = ra8_rabook_intern(ctx, "cover.png");
  if (!external_image_pool) {
    return ra8_rabook_add_image(ctx,
                                img_href,
                                (uint16_t)k_ra8_test_rabook_img_w,
                                (uint16_t)k_ra8_test_rabook_img_h,
                                (uint8_t)k_book_image_gray4,
                                (uint8_t)k_book_pixfmt_gray4,
                                s_img_bytes,
                                (uint32_t)sizeof(s_img_bytes));
  }
  uint32_t       data_off = 0U;
  const uint32_t idx      = ra8_rabook_add_image_external(ctx,
                                                          img_href,
                                                          (uint16_t)k_ra8_test_rabook_img_w,
                                                          (uint16_t)k_ra8_test_rabook_img_h,
                                                          (uint8_t)k_book_image_gray4,
                                                          (uint8_t)k_book_pixfmt_gray4,
                                                          (uint32_t)sizeof(s_img_bytes),
                                                          &data_off);
  TEST_ASSERT_EQ(0U, data_off);
  (void)memcpy(&fixture->external_pool[data_off], s_img_bytes, sizeof(s_img_bytes));
  return idx;
}

RA8_TEST_HELPER void ra8_test_rabook_populate(ra8_test_rabook_fixture_t*   fixture,
                                              ra8_rabook_ctx_t*            ctx,
                                              ra8_test_rabook_roundtrip_t* roundtrip,
                                              bool                         external_image_pool)
{
  TEST_ASSERT_NOT_NULL(fixture);
  TEST_ASSERT_NOT_NULL(ctx);
  TEST_ASSERT_NOT_NULL(roundtrip);
  TEST_ASSERT_EQ(0U, ra8_rabook_intern(ctx, ""));

  const uint32_t title_off  = ra8_rabook_intern(ctx, "The Title");
  const uint32_t author_off = ra8_rabook_intern(ctx, "An Author");
  const uint32_t lang_off   = ra8_rabook_intern(ctx, "en");
  const uint32_t id_off     = ra8_rabook_intern(ctx, "urn:isbn:123");

  roundtrip->img_idx = internal_add_cover(fixture, ctx, external_image_pool);
  TEST_ASSERT_EQ(0U, roundtrip->img_idx);

  const uint32_t css_off = ra8_rabook_intern(ctx, "p{margin:0}");
  (void)ra8_rabook_add_stylesheet(ctx, css_off, (uint32_t)k_book_nil);

  roundtrip->body_name      = ra8_rabook_intern(ctx, "body");
  const uint32_t p_name     = ra8_rabook_intern(ctx, "p");
  const uint32_t class_name = ra8_rabook_intern(ctx, "class");
  const uint32_t class_val  = ra8_rabook_intern(ctx, "ch");
  const uint32_t hello_off  = ra8_rabook_intern(ctx, "Hello");

  const book_attr_t body_attrs[1] = {
    {.name_off = class_name, .value_off = class_val},
  };
  roundtrip->body_idx = ra8_rabook_add_element(ctx, roundtrip->body_name, body_attrs, 1U);
  roundtrip->p_idx    = ra8_rabook_add_element(ctx, p_name, nullptr, 0U);
  roundtrip->text_idx = ra8_rabook_add_text(ctx, hello_off);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rabook_link_child(ctx, roundtrip->body_idx, roundtrip->p_idx));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rabook_link_child(ctx, roundtrip->p_idx, roundtrip->text_idx));

  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_rabook_set_metadata(ctx, title_off, author_off, lang_off, id_off, roundtrip->img_idx));

  const uint32_t toc1   = ra8_rabook_intern(ctx, "Chapter One");
  const uint32_t href1  = ra8_rabook_intern(ctx, "ch1.xhtml");
  const uint32_t ch_idx = ra8_rabook_add_chapter(ctx, toc1, href1, roundtrip->body_idx);
  TEST_ASSERT_EQ(0U, ch_idx);
}

RA8_TEST_HELPER void ra8_test_rabook_verify(const ra8_test_rabook_roundtrip_t* roundtrip)
{
  TEST_ASSERT_NOT_NULL(roundtrip);
  const void* blob = roundtrip->blob;
  TEST_ASSERT_NOT_NULL(blob);

  const book_header_t* hdr = book_header(blob);
  TEST_ASSERT_EQ(0, strcmp(book_string(blob, hdr->title_off), "The Title"));
  TEST_ASSERT_EQ(0, strcmp(book_string(blob, hdr->author_off), "An Author"));
  TEST_ASSERT_EQ(0, strcmp(book_string(blob, hdr->language_off), "en"));
  TEST_ASSERT_EQ(0, strcmp(book_string(blob, hdr->identifier_off), "urn:isbn:123"));
  TEST_ASSERT_EQ(1U, hdr->chapter_count);
  TEST_ASSERT_EQ(roundtrip->img_idx, hdr->cover_image_index);

  const book_chapter_t* chapters = book_chapters(blob);
  TEST_ASSERT_EQ(0, strcmp(book_string(blob, chapters[0].title_off), "Chapter One"));
  TEST_ASSERT_EQ(0, strcmp(book_string(blob, chapters[0].href_off), "ch1.xhtml"));
  TEST_ASSERT_EQ(roundtrip->body_idx, chapters[0].root_node);

  const book_node_t* nodes = book_nodes(blob);
  const book_node_t* body  = &nodes[roundtrip->body_idx];
  TEST_ASSERT_EQ(k_book_node_element, body->kind);
  TEST_ASSERT_EQ(0, strcmp(book_node_name(blob, body), "body"));
  TEST_ASSERT_EQ(1U, body->attr_count);
  TEST_ASSERT_EQ(roundtrip->p_idx, body->first_child);

  const book_attr_t* attrs = book_attrs(blob);
  const book_attr_t* attr0 = &attrs[body->first_attr];
  TEST_ASSERT_EQ(0, strcmp(book_string(blob, attr0->name_off), "class"));
  TEST_ASSERT_EQ(0, strcmp(book_string(blob, attr0->value_off), "ch"));

  const book_node_t* para = &nodes[roundtrip->p_idx];
  TEST_ASSERT_EQ(roundtrip->text_idx, para->first_child);
  const book_node_t* text = &nodes[roundtrip->text_idx];
  TEST_ASSERT_EQ(k_book_node_text, text->kind);
  TEST_ASSERT_EQ(0, strcmp(book_node_text(blob, text), "Hello"));

  TEST_ASSERT_EQ(1U, hdr->image_count);
  const book_image_t* images = book_images(blob);
  TEST_ASSERT_EQ(k_ra8_test_rabook_img_w, images[0].width);
  TEST_ASSERT_EQ(k_ra8_test_rabook_img_h, images[0].height);
  TEST_ASSERT_EQ(k_book_image_gray4, images[0].format);
  TEST_ASSERT_EQ(2U, images[0].data_size);
  TEST_ASSERT_EQ(2U, images[0].raw_size);
  const uint8_t* image_data = book_image_data(blob, &images[0]);
  TEST_ASSERT_EQ(0xF0, image_data[0]);
  TEST_ASSERT_EQ(0x0F, image_data[1]);
  TEST_ASSERT_EQ(1U, hdr->stylesheet_count);
}
