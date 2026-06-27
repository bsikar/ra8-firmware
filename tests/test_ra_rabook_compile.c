/**
 * @file test_ra_rabook_compile.c
 * @brief Round-trip test for the RABOOK1 emitter (#149 compiler back-end).
 *
 * @details
 * Builds a small book through the zero-heap @ref ra_rabook_compile builder
 * (metadata, two-deep DOM with an attribute, a text run, a gray4 image and a
 * stylesheet), finalizes the blob, then proves fidelity two ways: the emitted
 * blob passes @ref ra_book_validate (magic + version + bounds + body CRC-32),
 * and every field read back through the @ref ra_book accessors matches what was
 * built. This is the EMITTER (back-end) test; the XHTML -> DOM front-end and the
 * raster -> gray4 transcode are the next stage-(a) pieces.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ra_book.h"
#include "ra_err.h"
#include "ra_rabook_compile.h"
#include "unity_minimal.h"

/** @brief Fixed arenas for the builder (no dynamic allocation). */
typedef enum : uint32_t {
  k_t_chapter_cap = 8U,    /**< Max chapters in the test book. */
  k_t_node_cap    = 64U,   /**< Max DOM nodes.                 */
  k_t_attr_cap    = 32U,   /**< Max attribute records.         */
  k_t_style_cap   = 4U,    /**< Max stylesheets.               */
  k_t_image_cap   = 8U,    /**< Max image descriptors.         */
  k_t_string_cap  = 1024U, /**< String-pool capacity (bytes).  */
  k_t_imgpool_cap = 256U,  /**< Image-pool capacity (bytes).   */
  k_t_out_cap     = 4096U, /**< Output-blob capacity (bytes).  */
} test_rabook_cap_t;

/** @brief Test image geometry: a 2x2 gray4 cover (4 px -> 2 packed bytes). */
typedef enum : uint16_t {
  k_t_img_w = 2U, /**< Cover width in pixels.  */
  k_t_img_h = 2U, /**< Cover height in pixels. */
} test_rabook_img_t;

static ra_book_chapter_t    s_chapters[k_t_chapter_cap];
static ra_book_node_t       s_nodes[k_t_node_cap];
static ra_book_attr_t       s_attrs[k_t_attr_cap];
static ra_book_stylesheet_t s_styles[k_t_style_cap];
static ra_book_image_t      s_images[k_t_image_cap];
static char                 s_strpool[k_t_string_cap];
static uint8_t              s_imgpool[k_t_imgpool_cap];
static uint8_t              s_out[k_t_out_cap];

/**
 * @test test_rabook_compile_roundtrip
 * @brief Build a blob, validate it, and read every field back identical.
 */
static void test_rabook_compile_roundtrip(void)
{
  TEST_BEGIN("ra_rabook_compile: build -> validate -> read back");

  const ra_rabook_buffers_t buf = {
    .chapters       = s_chapters,
    .chapter_cap    = (uint32_t)k_t_chapter_cap,
    .nodes          = s_nodes,
    .node_cap       = (uint32_t)k_t_node_cap,
    .attrs          = s_attrs,
    .attr_cap       = (uint32_t)k_t_attr_cap,
    .stylesheets    = s_styles,
    .stylesheet_cap = (uint32_t)k_t_style_cap,
    .images         = s_images,
    .image_cap      = (uint32_t)k_t_image_cap,
    .string_pool    = s_strpool,
    .string_cap     = (uint32_t)k_t_string_cap,
    .image_pool     = s_imgpool,
    .image_pool_cap = (uint32_t)k_t_imgpool_cap,
    .out            = s_out,
    .out_cap        = (uint32_t)k_t_out_cap,
  };
  ra_rabook_ctx_t ctx = {};
  TEST_ASSERT_EQ(k_ra_ok, ra_rabook_compile_init(&ctx, &buf));

  /* Intern "" first so empty/default offsets resolve to "" at offset 0. */
  TEST_ASSERT_EQ(0U, ra_rabook_intern(&ctx, ""));

  const uint32_t title_off  = ra_rabook_intern(&ctx, "The Title");
  const uint32_t author_off = ra_rabook_intern(&ctx, "An Author");
  const uint32_t lang_off   = ra_rabook_intern(&ctx, "en");
  const uint32_t id_off     = ra_rabook_intern(&ctx, "urn:isbn:123");

  /* A 2x2 gray4 cover image (high/low nibble pixels). */
  static const uint8_t img_bytes[2] = {0xF0U, 0x0FU};
  const uint32_t       img_href     = ra_rabook_intern(&ctx, "cover.png");
  const uint32_t       img_idx      = ra_rabook_add_image(&ctx,
                                                          img_href,
                                                          (uint16_t)k_t_img_w,
                                                          (uint16_t)k_t_img_h,
                                                          (uint8_t)k_ra_book_image_gray4,
                                                          img_bytes,
                                                          (uint32_t)sizeof(img_bytes));
  TEST_ASSERT_EQ(0U, img_idx);

  const uint32_t css_off = ra_rabook_intern(&ctx, "p{margin:0}");
  (void)ra_rabook_add_stylesheet(&ctx, css_off, (uint32_t)k_ra_book_nil);

  /* DOM: <body class="ch"><p>Hello</p></body> */
  const uint32_t body_name  = ra_rabook_intern(&ctx, "body");
  const uint32_t p_name     = ra_rabook_intern(&ctx, "p");
  const uint32_t class_name = ra_rabook_intern(&ctx, "class");
  const uint32_t class_val  = ra_rabook_intern(&ctx, "ch");
  const uint32_t hello_off  = ra_rabook_intern(&ctx, "Hello");

  const ra_book_attr_t body_attrs[1] = {
    {.name_off = class_name, .value_off = class_val},
  };
  const uint32_t body_idx = ra_rabook_add_element(&ctx, body_name, body_attrs, 1U);
  const uint32_t p_idx    = ra_rabook_add_element(&ctx, p_name, nullptr, 0U);
  const uint32_t text_idx = ra_rabook_add_text(&ctx, hello_off);
  TEST_ASSERT_EQ(k_ra_ok, ra_rabook_link_child(&ctx, body_idx, p_idx));
  TEST_ASSERT_EQ(k_ra_ok, ra_rabook_link_child(&ctx, p_idx, text_idx));

  TEST_ASSERT_EQ(k_ra_ok,
                 ra_rabook_set_metadata(&ctx, title_off, author_off, lang_off, id_off, img_idx));

  const uint32_t toc1   = ra_rabook_intern(&ctx, "Chapter One");
  const uint32_t href1  = ra_rabook_intern(&ctx, "ch1.xhtml");
  const uint32_t ch_idx = ra_rabook_add_chapter(&ctx, toc1, href1, body_idx);
  TEST_ASSERT_EQ(0U, ch_idx);

  const void* blob     = nullptr;
  uint32_t    blob_len = 0U;
  TEST_ASSERT_EQ(k_ra_ok, ra_rabook_finalize(&ctx, &blob, &blob_len));
  TEST_ASSERT_NOT_NULL(blob);

  /* The blob the emitter produced must satisfy the on-device reader. */
  TEST_ASSERT_EQ(k_ra_ok, ra_book_validate(blob, (size_t)blob_len));

  const ra_book_header_t* hdr = ra_book_header(blob);
  TEST_ASSERT_EQ(0, strcmp(ra_book_string(blob, hdr->title_off), "The Title"));
  TEST_ASSERT_EQ(0, strcmp(ra_book_string(blob, hdr->author_off), "An Author"));
  TEST_ASSERT_EQ(0, strcmp(ra_book_string(blob, hdr->language_off), "en"));
  TEST_ASSERT_EQ(0, strcmp(ra_book_string(blob, hdr->identifier_off), "urn:isbn:123"));
  TEST_ASSERT_EQ(1U, hdr->chapter_count);
  TEST_ASSERT_EQ(img_idx, hdr->cover_image_index);

  const ra_book_chapter_t* chaps = ra_book_chapters(blob);
  TEST_ASSERT_EQ(0, strcmp(ra_book_string(blob, chaps[0].title_off), "Chapter One"));
  TEST_ASSERT_EQ(0, strcmp(ra_book_string(blob, chaps[0].href_off), "ch1.xhtml"));
  TEST_ASSERT_EQ(body_idx, chaps[0].root_node);

  const ra_book_node_t* nodes = ra_book_nodes(blob);
  const ra_book_node_t* body  = &nodes[body_idx];
  TEST_ASSERT_EQ((uint8_t)k_ra_book_node_element, body->kind);
  TEST_ASSERT_EQ(0, strcmp(ra_book_node_name(blob, body), "body"));
  TEST_ASSERT_EQ(1U, body->attr_count);
  TEST_ASSERT_EQ(p_idx, body->first_child);

  const ra_book_attr_t* attrs = ra_book_attrs(blob);
  const ra_book_attr_t* attr0 = &attrs[body->first_attr];
  TEST_ASSERT_EQ(0, strcmp(ra_book_string(blob, attr0->name_off), "class"));
  TEST_ASSERT_EQ(0, strcmp(ra_book_string(blob, attr0->value_off), "ch"));

  const ra_book_node_t* para = &nodes[p_idx];
  TEST_ASSERT_EQ(text_idx, para->first_child);
  const ra_book_node_t* text = &nodes[text_idx];
  TEST_ASSERT_EQ((uint8_t)k_ra_book_node_text, text->kind);
  TEST_ASSERT_EQ(0, strcmp(ra_book_node_text(blob, text), "Hello"));

  TEST_ASSERT_EQ(1U, hdr->image_count);
  const ra_book_image_t* imgs = ra_book_images(blob);
  TEST_ASSERT_EQ((uint16_t)k_t_img_w, imgs[0].width);
  TEST_ASSERT_EQ((uint16_t)k_t_img_h, imgs[0].height);
  TEST_ASSERT_EQ((uint8_t)k_ra_book_image_gray4, imgs[0].format);
  TEST_ASSERT_EQ(2U, imgs[0].data_size);
  TEST_ASSERT_EQ(2U, imgs[0].raw_size);
  const uint8_t* idata = ra_book_image_data(blob, &imgs[0]);
  TEST_ASSERT_EQ(0xF0, idata[0]);
  TEST_ASSERT_EQ(0x0F, idata[1]);

  TEST_ASSERT_EQ(1U, hdr->stylesheet_count);

  /* String interning de-dups: re-interning "body" returns the same offset. */
  TEST_ASSERT_EQ(body_name, ra_rabook_intern(&ctx, "body"));

  TEST_END("ra_rabook_compile: build -> validate -> read back");
}

int32_t main(void)
{
  test_rabook_compile_roundtrip();
  (void)fprintf(stderr, "[OK ] test_ra_rabook_compile.c\n");
  return 0;
}
