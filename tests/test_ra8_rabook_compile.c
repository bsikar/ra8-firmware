/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file test_ra8_rabook_compile.c
 * @brief Emitter + error-arm tests for the RABOOK1 builder (#149 back-end).
 *
 * @details
 * Drives the zero-heap @ref ra8_rabook_compile builder over its happy path and
 * its failure arms:
 *
 *  - Round-trip: build a small book (metadata, two-deep DOM with an attribute,
 *    a text run, a gray4 image and a stylesheet), finalize, accept it through
 *    @ref ra8_book_validate (magic + version + bounds + body CRC-32), and read
 *    every field back through the @ref ra8_book accessors. This asserts
 *    field-by-field equality of what was built vs. what reads back -- NOT raw
 *    byte-identity with the desktop tool (that parity is the desktop-tool's own
 *    golden test, not asserted here).
 *  - Arena overflow: each builder arena (nodes, attrs, string pool, image
 *    descriptors) is sized to overflow, latching the sticky `ctx->failed` flag
 *    so @ref ra8_rabook_finalize reports @ref k_ra8_err_no_mem.
 *  - CRC-32 empty range: a degenerate empty builder finalizes a 100-byte
 *    header-only blob, exercising the `len == 0` arm of the body CRC (result 0).
 *  - @ref ra8_rabook_add_image with `data_size == 0` (descriptor, no pool bytes).
 *  - Offset-0 contract: `intern("")` returns 0 and a real string never lands
 *    at offset 0.
 *
 * This is the EMITTER (back-end) test; the XHTML -> DOM front-end and the
 * raster -> gray4 transcode are exercised by their own test files.
 *
 * @since 0.1.0
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ra8_book.h"
#include "ra8_err.h"
#include "ra8_log.h"
#include "ra8_rabook_compile.h"
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

static ra8_book_chapter_t    s_chapters[k_t_chapter_cap];
static ra8_book_node_t       s_nodes[k_t_node_cap];
static ra8_book_attr_t       s_attrs[k_t_attr_cap];
static ra8_book_stylesheet_t s_styles[k_t_style_cap];
static ra8_book_image_t      s_images[k_t_image_cap];
static char                  s_strpool[k_t_string_cap];
static uint8_t               s_imgpool[k_t_imgpool_cap];
static uint8_t               s_out[k_t_out_cap];

/**
 * @brief Build a full-capacity buffers struct over the file-scope arenas.
 * @details The error-arm tests start from this and shrink exactly one `*_cap`
 *          field to force a single arena to overflow. Capacities (not the
 *          backing array sizes) drive the builder's bounds, so a deliberately
 *          small cap over a large array exercises the overflow path safely.
 * @return A fully-populated @ref ra8_rabook_buffers_t over the static arenas.
 * @pre The file-scope arenas are defined (always true at TU scope).
 * @pre The returned struct is consumed before the arenas are reused.
 * @post Every member pointer is non-NULL and every cap is the full array size.
 * @post No global state is mutated (pure constructor).
 * @note Not thread-safe (returns a view over shared file-scope arenas).
 */
static ra8_rabook_buffers_t full_buffers(void)
{
  const ra8_rabook_buffers_t buf = {
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
  return buf;
}

/**
 * @struct rabook_rt_t
 * @brief Round-trip handles carried from the build phase into verification.
 * @invariant Node/image indices are those returned by the compiler builders.
 * @see test_rabook_compile_roundtrip
 */
typedef struct {
  const void* blob;      /**< Finalised book blob.                    */
  uint32_t    blob_len;  /**< Blob length in bytes.                   */
  uint32_t    body_idx;  /**< Node index of the <body> element.       */
  uint32_t    p_idx;     /**< Node index of the <p> element.          */
  uint32_t    text_idx;  /**< Node index of the "Hello" text node.    */
  uint32_t    img_idx;   /**< Image index of the cover image.         */
  uint32_t    body_name; /**< Interned offset of the "body" tag name. */
} rabook_rt_t;

/**
 * @brief Initialise the compiler context over the shared scratch buffers.
 * @param[out] ctx Compiler context to initialise.
 * @pre The shared scratch buffers are available.
 * @post `ra8_rabook_compile_init` succeeded on @p ctx.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void rabook_rt_init(ra8_rabook_ctx_t* ctx)
{
  const ra8_rabook_buffers_t buf = {
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
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rabook_compile_init(ctx, &buf));
}

/**
 * @brief Build the fixture book (metadata, cover, stylesheet, DOM, chapter).
 * @param[in,out] ctx Initialised compiler context.
 * @param[out]    rt  Receives the node / image indices and the "body" offset.
 * @pre rabook_rt_init(@p ctx) already ran.
 * @post @p rt index fields are set; the book is ready to finalise.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void rabook_rt_populate(ra8_rabook_ctx_t* ctx, rabook_rt_t* rt)
{
  /* Intern "" first so empty/default offsets resolve to "" at offset 0. */
  TEST_ASSERT_EQ(0U, ra8_rabook_intern(ctx, ""));

  const uint32_t title_off  = ra8_rabook_intern(ctx, "The Title");
  const uint32_t author_off = ra8_rabook_intern(ctx, "An Author");
  const uint32_t lang_off   = ra8_rabook_intern(ctx, "en");
  const uint32_t id_off     = ra8_rabook_intern(ctx, "urn:isbn:123");

  /* A 2x2 gray4 cover image (high/low nibble pixels). */
  static const uint8_t img_bytes[2] = {0xF0U, 0x0FU};
  const uint32_t       img_href     = ra8_rabook_intern(ctx, "cover.png");
  rt->img_idx                       = ra8_rabook_add_image(ctx,
                                                           img_href,
                                                           (uint16_t)k_t_img_w,
                                                           (uint16_t)k_t_img_h,
                                                           (uint8_t)k_ra8_book_image_gray4,
                                                           (uint8_t)k_ra8_book_pixfmt_gray4,
                                                           img_bytes,
                                                           (uint32_t)sizeof(img_bytes));
  TEST_ASSERT_EQ(0U, rt->img_idx);

  const uint32_t css_off = ra8_rabook_intern(ctx, "p{margin:0}");
  (void)ra8_rabook_add_stylesheet(ctx, css_off, (uint32_t)k_ra8_book_nil);

  /* DOM: <body class="ch"><p>Hello</p></body> */
  rt->body_name             = ra8_rabook_intern(ctx, "body");
  const uint32_t p_name     = ra8_rabook_intern(ctx, "p");
  const uint32_t class_name = ra8_rabook_intern(ctx, "class");
  const uint32_t class_val  = ra8_rabook_intern(ctx, "ch");
  const uint32_t hello_off  = ra8_rabook_intern(ctx, "Hello");

  const ra8_book_attr_t body_attrs[1] = {
    {.name_off = class_name, .value_off = class_val},
  };
  rt->body_idx = ra8_rabook_add_element(ctx, rt->body_name, body_attrs, 1U);
  rt->p_idx    = ra8_rabook_add_element(ctx, p_name, nullptr, 0U);
  rt->text_idx = ra8_rabook_add_text(ctx, hello_off);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rabook_link_child(ctx, rt->body_idx, rt->p_idx));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rabook_link_child(ctx, rt->p_idx, rt->text_idx));

  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_rabook_set_metadata(ctx, title_off, author_off, lang_off, id_off, rt->img_idx));

  const uint32_t toc1   = ra8_rabook_intern(ctx, "Chapter One");
  const uint32_t href1  = ra8_rabook_intern(ctx, "ch1.xhtml");
  const uint32_t ch_idx = ra8_rabook_add_chapter(ctx, toc1, href1, rt->body_idx);
  TEST_ASSERT_EQ(0U, ch_idx);
}

/**
 * @brief Assert the finalised blob reads back through the on-device reader.
 * @param[in] rt Round-trip handles from the build phase.
 * @pre @p rt->blob passed ra8_book_validate().
 * @post Header, chapter, DOM, attribute, image and stylesheet all matched.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void rabook_rt_verify(const rabook_rt_t* rt)
{
  const void* blob = rt->blob;

  const ra8_book_header_t* hdr = ra8_book_header(blob);
  TEST_ASSERT_EQ(0, strcmp(ra8_book_string(blob, hdr->title_off), "The Title"));
  TEST_ASSERT_EQ(0, strcmp(ra8_book_string(blob, hdr->author_off), "An Author"));
  TEST_ASSERT_EQ(0, strcmp(ra8_book_string(blob, hdr->language_off), "en"));
  TEST_ASSERT_EQ(0, strcmp(ra8_book_string(blob, hdr->identifier_off), "urn:isbn:123"));
  TEST_ASSERT_EQ(1U, hdr->chapter_count);
  TEST_ASSERT_EQ(rt->img_idx, hdr->cover_image_index);

  const ra8_book_chapter_t* chaps = ra8_book_chapters(blob);
  TEST_ASSERT_EQ(0, strcmp(ra8_book_string(blob, chaps[0].title_off), "Chapter One"));
  TEST_ASSERT_EQ(0, strcmp(ra8_book_string(blob, chaps[0].href_off), "ch1.xhtml"));
  TEST_ASSERT_EQ(rt->body_idx, chaps[0].root_node);

  const ra8_book_node_t* nodes = ra8_book_nodes(blob);
  const ra8_book_node_t* body  = &nodes[rt->body_idx];
  TEST_ASSERT_EQ(k_ra8_book_node_element, body->kind);
  TEST_ASSERT_EQ(0, strcmp(ra8_book_node_name(blob, body), "body"));
  TEST_ASSERT_EQ(1U, body->attr_count);
  TEST_ASSERT_EQ(rt->p_idx, body->first_child);

  const ra8_book_attr_t* attrs = ra8_book_attrs(blob);
  const ra8_book_attr_t* attr0 = &attrs[body->first_attr];
  TEST_ASSERT_EQ(0, strcmp(ra8_book_string(blob, attr0->name_off), "class"));
  TEST_ASSERT_EQ(0, strcmp(ra8_book_string(blob, attr0->value_off), "ch"));

  const ra8_book_node_t* para = &nodes[rt->p_idx];
  TEST_ASSERT_EQ(rt->text_idx, para->first_child);
  const ra8_book_node_t* text = &nodes[rt->text_idx];
  TEST_ASSERT_EQ(k_ra8_book_node_text, text->kind);
  TEST_ASSERT_EQ(0, strcmp(ra8_book_node_text(blob, text), "Hello"));

  TEST_ASSERT_EQ(1U, hdr->image_count);
  const ra8_book_image_t* imgs = ra8_book_images(blob);
  TEST_ASSERT_EQ(k_t_img_w, imgs[0].width);
  TEST_ASSERT_EQ(k_t_img_h, imgs[0].height);
  TEST_ASSERT_EQ(k_ra8_book_image_gray4, imgs[0].format);
  TEST_ASSERT_EQ(2U, imgs[0].data_size);
  TEST_ASSERT_EQ(2U, imgs[0].raw_size);
  const uint8_t* idata = ra8_book_image_data(blob, &imgs[0]);
  TEST_ASSERT_EQ(0xF0, idata[0]);
  TEST_ASSERT_EQ(0x0F, idata[1]);

  TEST_ASSERT_EQ(1U, hdr->stylesheet_count);
}

/**
 * @test test_rabook_compile_roundtrip
 * @brief Build a blob, validate it, and read every field back identical.
 *
 * @par MC/DC:
 * (no compound decisions in this test -- it exercises the builder happy path end
 * to end: intern -> add_image -> add_stylesheet -> add_element/text ->
 * link_child -> set_metadata -> add_chapter -> finalize -> ra8_book_validate,
 * then reads every field back through the accessors and confirms interning
 * de-dups. Each builder guard on this success path is a single-condition
 * bounds/relational check; the arena-overflow decisions have their own error-arm
 * tests in this file)
 */
static void test_rabook_compile_roundtrip(void)
{
  TEST_BEGIN("ra8_rabook_compile: build -> validate -> read back");

  ra8_rabook_ctx_t ctx = {};
  rabook_rt_init(&ctx);

  rabook_rt_t rt = {};
  rabook_rt_populate(&ctx, &rt);

  const void* blob     = nullptr;
  uint32_t    blob_len = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rabook_finalize(&ctx, &blob, &blob_len));
  TEST_ASSERT_NOT_NULL(blob);
  rt.blob     = blob;
  rt.blob_len = blob_len;

  /* The blob the emitter produced must satisfy the on-device reader. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_book_validate(blob, (size_t)blob_len));
  rabook_rt_verify(&rt);

  /* String interning de-dups: re-interning "body" returns the same offset. */
  TEST_ASSERT_EQ(rt.body_name, ra8_rabook_intern(&ctx, "body"));

  TEST_END("ra8_rabook_compile: build -> validate -> read back");
}

/**
 * @test test_rabook_overflow_nodes
 * @brief A node-arena overflow latches ctx->failed so finalize returns no_mem.
 *
 * @par MC/DC:
 * No compound decision under test: the overflow guard
 * `ctx->node_count >= ctx->buf.node_cap` is a single relational condition.
 * Vectors: first add (0 >= 1 -> false, succeeds at index 0); second add
 * (1 >= 1 -> true, latches `failed` and returns nil). finalize then takes its
 * single `ctx->failed` arm and reports @ref k_ra8_err_no_mem.
 */
static void test_rabook_overflow_nodes(void)
{
  TEST_BEGIN("ra8_rabook_compile: node-arena overflow -> no_mem");
  ra8_rabook_buffers_t buf = full_buffers();
  buf.node_cap             = 1U;
  ra8_rabook_ctx_t ctx     = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rabook_compile_init(&ctx, &buf));

  const uint32_t a_off = ra8_rabook_intern(&ctx, "a");
  const uint32_t b_off = ra8_rabook_intern(&ctx, "b");
  TEST_ASSERT_EQ(0U, ra8_rabook_add_element(&ctx, a_off, nullptr, 0U));
  TEST_ASSERT_EQ(k_ra8_book_nil, ra8_rabook_add_element(&ctx, b_off, nullptr, 0U));

  const void* blob = nullptr;
  uint32_t    len  = 0U;
  TEST_ASSERT_EQ(k_ra8_err_no_mem, ra8_rabook_finalize(&ctx, &blob, &len));
  TEST_END("ra8_rabook_compile: node-arena overflow -> no_mem");
}

/**
 * @test test_rabook_overflow_attrs
 * @brief An attr-arena overflow latches ctx->failed so finalize returns no_mem.
 *
 * @par MC/DC:
 * The guard `(uint32_t)attr_count > (ctx->buf.attr_cap - ctx->attr_count)` is a
 * single relational condition; this vector takes its true arm (2 > 1) on the
 * first add, latching `failed`. finalize then reports @ref k_ra8_err_no_mem.
 */
static void test_rabook_overflow_attrs(void)
{
  TEST_BEGIN("ra8_rabook_compile: attr-arena overflow -> no_mem");
  ra8_rabook_buffers_t buf = full_buffers();
  buf.attr_cap             = 1U;
  ra8_rabook_ctx_t ctx     = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rabook_compile_init(&ctx, &buf));

  const uint32_t        name_off = ra8_rabook_intern(&ctx, "class");
  const uint32_t        val_off  = ra8_rabook_intern(&ctx, "x");
  const ra8_book_attr_t two[2]   = {
    {.name_off = name_off, .value_off = val_off},
    {.name_off = name_off, .value_off = val_off},
  };
  /* 2 attrs requested but attr_cap is 1: overflow latched, element rejected. */
  TEST_ASSERT_EQ(k_ra8_book_nil, ra8_rabook_add_element(&ctx, name_off, two, 2U));

  const void* blob = nullptr;
  uint32_t    len  = 0U;
  TEST_ASSERT_EQ(k_ra8_err_no_mem, ra8_rabook_finalize(&ctx, &blob, &len));
  TEST_END("ra8_rabook_compile: attr-arena overflow -> no_mem");
}

/**
 * @test test_rabook_overflow_strings
 * @brief A string-pool overflow latches ctx->failed so finalize returns no_mem.
 *
 * @par MC/DC:
 * The guard `need > (ctx->buf.string_cap - ctx->string_size)` is a single
 * relational condition. With string_cap = 4 the init-reserved "" leaves only 3
 * free bytes, so interning a 10-char string (need = 11) takes the true arm and
 * latches `failed`; finalize then reports @ref k_ra8_err_no_mem.
 */
static void test_rabook_overflow_strings(void)
{
  TEST_BEGIN("ra8_rabook_compile: string-pool overflow -> no_mem");
  ra8_rabook_buffers_t buf = full_buffers();
  buf.string_cap           = 4U; /* "" reserves byte 0; 3 free bytes remain. */
  ra8_rabook_ctx_t ctx     = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rabook_compile_init(&ctx, &buf));

  TEST_ASSERT_EQ(0U, ra8_rabook_intern(&ctx, ""));
  TEST_ASSERT_EQ(k_ra8_book_nil, ra8_rabook_intern(&ctx, "overflowme"));

  const void* blob = nullptr;
  uint32_t    len  = 0U;
  TEST_ASSERT_EQ(k_ra8_err_no_mem, ra8_rabook_finalize(&ctx, &blob, &len));
  TEST_END("ra8_rabook_compile: string-pool overflow -> no_mem");
}

/**
 * @test test_rabook_overflow_images
 * @brief An image-descriptor overflow latches ctx->failed -> finalize no_mem.
 *
 * @par MC/DC:
 * The guard `ctx->image_count >= ctx->buf.image_cap` is a single relational
 * condition. Vectors: first add (0 >= 1 -> false, image 0); second add
 * (1 >= 1 -> true, latches `failed`, returns nil). finalize reports no_mem.
 */
static void test_rabook_overflow_images(void)
{
  TEST_BEGIN("ra8_rabook_compile: image-arena overflow -> no_mem");
  ra8_rabook_buffers_t buf = full_buffers();
  buf.image_cap            = 1U;
  ra8_rabook_ctx_t ctx     = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rabook_compile_init(&ctx, &buf));

  const uint32_t       href    = ra8_rabook_intern(&ctx, "img.bin");
  static const uint8_t data[1] = {0xABU};
  TEST_ASSERT_EQ(0U,
                 ra8_rabook_add_image(&ctx,
                                      href,
                                      1U,
                                      1U,
                                      (uint8_t)k_ra8_book_image_gray4,
                                      (uint8_t)k_ra8_book_pixfmt_gray4,
                                      data,
                                      1U));
  TEST_ASSERT_EQ(k_ra8_book_nil,
                 ra8_rabook_add_image(&ctx,
                                      href,
                                      1U,
                                      1U,
                                      (uint8_t)k_ra8_book_image_gray4,
                                      (uint8_t)k_ra8_book_pixfmt_gray4,
                                      data,
                                      1U));

  const void* blob = nullptr;
  uint32_t    len  = 0U;
  TEST_ASSERT_EQ(k_ra8_err_no_mem, ra8_rabook_finalize(&ctx, &blob, &len));
  TEST_END("ra8_rabook_compile: image-arena overflow -> no_mem");
}

/**
 * @test test_rabook_crc_empty_body
 * @brief A degenerate empty builder finalizes a header-only blob, exercising
 *        the len==0 arm of the body CRC (which returns 0).
 *
 * @par MC/DC:
 * No compound decision under test. This white-box case bypasses
 * @ref ra8_rabook_compile_init (which would reserve "" in the string pool) so the
 * computed body length is 0 and rabook_crc32()'s `for (i < len)` loop never
 * runs -- the documented empty-range path. The resulting CRC is the seed XORed
 * with itself, i.e. 0; the blob still passes @ref ra8_book_validate.
 */
static void test_rabook_crc_empty_body(void)
{
  TEST_BEGIN("ra8_rabook_compile: empty-body CRC (len==0) -> 0");
  ra8_rabook_buffers_t buf = full_buffers();
  /* Hand-build a zeroed ctx WITHOUT init so the string pool stays empty and the
   * body length is 0 -- the only way to reach the rabook_crc32 len==0 arm. */
  ra8_rabook_ctx_t ctx  = {};
  ctx.buf               = buf;
  ctx.cover_image_index = (uint32_t)k_ra8_book_nil;

  const void* blob = nullptr;
  uint32_t    len  = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rabook_finalize(&ctx, &blob, &len));
  TEST_ASSERT_NOT_NULL(blob);
  TEST_ASSERT_EQ(k_ra8_book_sizeof_header, len);

  const ra8_book_header_t* hdr = ra8_book_header(blob);
  TEST_ASSERT_EQ(0U, hdr->crc32);
  TEST_ASSERT_EQ(0U, hdr->string_size);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_book_validate(blob, (size_t)len));
  TEST_END("ra8_rabook_compile: empty-body CRC (len==0) -> 0");
}

/**
 * @test test_rabook_add_image_zero_data
 * @brief add_image with data_size==0 records a descriptor with no pool bytes.
 *
 * @par MC/DC:
 * The two `if (data_size != 0U)` guards inside add_image are single conditions;
 * this vector takes their false arms (no NULL-data check, no pool memcpy). The
 * blob is finalized and read back to confirm a zero-length image descriptor.
 */
static void test_rabook_add_image_zero_data(void)
{
  TEST_BEGIN("ra8_rabook_compile: add_image data_size==0");
  ra8_rabook_buffers_t buf = full_buffers();
  ra8_rabook_ctx_t     ctx = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rabook_compile_init(&ctx, &buf));

  const uint32_t href = ra8_rabook_intern(&ctx, "empty.bin");
  const uint32_t idx  = ra8_rabook_add_image(&ctx,
                                             href,
                                             4U,
                                             4U,
                                             (uint8_t)k_ra8_book_image_gray4,
                                             (uint8_t)k_ra8_book_pixfmt_gray4,
                                             nullptr,
                                             0U);
  TEST_ASSERT_EQ(0U, idx);

  const void* blob = nullptr;
  uint32_t    len  = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rabook_finalize(&ctx, &blob, &len));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_book_validate(blob, (size_t)len));

  const ra8_book_header_t* hdr = ra8_book_header(blob);
  TEST_ASSERT_EQ(1U, hdr->image_count);
  TEST_ASSERT_EQ(0U, hdr->image_pool_size);
  const ra8_book_image_t* imgs = ra8_book_images(blob);
  TEST_ASSERT_EQ(0U, imgs[0].data_size);
  TEST_ASSERT_EQ(0U, imgs[0].raw_size);
  TEST_ASSERT_EQ(4U, imgs[0].width);
  TEST_END("ra8_rabook_compile: add_image data_size==0");
}

/**
 * @test test_rabook_offset_zero
 * @brief intern("") yields offset 0 and a real string never lands at offset 0.
 *
 * @par MC/DC:
 * No compound decision under test. Confirms the offset-0 == "" contract: init
 * reserves "" at offset 0; re-interning "" de-dups back to 0; the first real
 * string is appended after the reserved NUL, so its offset is non-zero.
 */
static void test_rabook_offset_zero(void)
{
  TEST_BEGIN("ra8_rabook_compile: offset-0 == \"\"");
  ra8_rabook_buffers_t buf = full_buffers();
  ra8_rabook_ctx_t     ctx = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rabook_compile_init(&ctx, &buf));

  TEST_ASSERT_EQ(0U, ra8_rabook_intern(&ctx, ""));
  const uint32_t real = ra8_rabook_intern(&ctx, "Title");
  TEST_ASSERT(real != 0U);
  TEST_ASSERT_EQ(0, strcmp(&ctx.buf.string_pool[0], ""));
  TEST_ASSERT_EQ(0, strcmp(&ctx.buf.string_pool[real], "Title"));
  TEST_END("ra8_rabook_compile: offset-0 == \"\"");
}

/**
 * @brief No-op log byte sink so the logger never pokes ITM MMIO on the host.
 * @details The error-arm tests deliberately drive @ref ra8_rabook_finalize and
 *          the builder into their failure paths, which call `ra8_log_error`.
 *          Registering a sink makes `internal_itm_ready()` short-circuit so
 *          those log calls route here instead of the unmapped Cortex-M85 ITM
 *          registers (which would SIGSEGV on the host).
 * @param[in] ctx  Unused sink context.
 * @param[in] byte Unused log byte.
 * @pre Installed in main() before any test runs.
 * @pre Never called from interrupt context (host build).
 * @post No global state is mutated.
 * @post The byte is discarded.
 * @note Not thread-safe (host single-thread test driver).
 */
static void s_log_sink(void* ctx, uint8_t byte)
{
  (void)ctx;
  (void)byte;
}

int32_t main(void)
{
  ra8_log_set_byte_sink(s_log_sink, nullptr);
  test_rabook_compile_roundtrip();
  test_rabook_overflow_nodes();
  test_rabook_overflow_attrs();
  test_rabook_overflow_strings();
  test_rabook_overflow_images();
  test_rabook_crc_empty_body();
  test_rabook_add_image_zero_data();
  test_rabook_offset_zero();
  (void)fprintf(stderr, "[OK ] test_ra8_rabook_compile.c\n");
  return 0;
}
