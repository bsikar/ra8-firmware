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
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
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
static uint8_t               s_stream_out[k_t_out_cap];
static uint8_t               s_expected[k_t_out_cap];
static uint8_t               s_external_pool[k_t_imgpool_cap];

/** @brief Bounded read/write fixture used by the streaming-finalizer tests. */
typedef struct {
  const uint8_t* read_data;   /**< External image-pool bytes.          */
  uint8_t*       write_data;  /**< Stream destination bytes.           */
  uint32_t       read_len;    /**< Readable external-pool byte count.   */
  uint32_t       write_cap;   /**< Writable destination byte count.    */
  uint32_t       write_used;  /**< Bytes appended so far.              */
  uint32_t       read_calls;  /**< Read callback invocation count.     */
  uint32_t       write_calls; /**< Write callback invocation count.    */
  uint32_t       max_read;    /**< Largest one-call read request.      */
  bool           short_read;  /**< Return one byte short once.         */
  bool           short_write; /**< Return one byte short once.         */
} rabook_stream_io_t;

/**
 * @brief Exact external-pool reader with an opt-in short-read fault.
 * @details Copies from the fixture pool, tracks request high-water, and can
 *          return one deliberately short successful transfer.
 * @param[in,out] opaque Fixture I/O state.
 * @param[in] offset External-pool byte offset.
 * @param[out] dst Read destination.
 * @param[in] requested Requested bytes.
 * @param[out] out_read Actual copied bytes.
 * @return Fixture read status.
 * @retval k_ra8_ok Full or deliberately short fixture transfer.
 * @retval k_ra8_err_invalid_size Requested range exceeds the fixture pool.
 * @pre All pointer arguments are non-NULL.
 * @pre @p dst spans @p requested writable bytes.
 * @post The call count and maximum request are updated.
 * @post @p out_read reports exactly the initialized destination prefix.
 * @note Test-only and not thread-safe.
 * @since 0.1.0
 */
static ra8_err_t
stream_read(void* opaque, uint32_t offset, uint8_t* dst, uint32_t requested, uint32_t* out_read)
{
  rabook_stream_io_t* io = (rabook_stream_io_t*)opaque;
  io->read_calls++;
  if (requested > io->max_read) {
    io->max_read = requested;
  }
  if ((offset > io->read_len) || (requested > (io->read_len - offset))) {
    *out_read = 0U;
    return k_ra8_err_invalid_size;
  }
  uint32_t delivered = requested;
  if (io->short_read && (requested != 0U)) {
    delivered--;
    io->short_read = false;
  }
  (void)memcpy(dst, &io->read_data[offset], (size_t)delivered);
  *out_read = delivered;
  return k_ra8_ok;
}

/**
 * @brief Bounded stream writer with opt-in short-write/output-exhaustion faults.
 * @details Appends into the fixture destination and can return either a deliberate
 *          short success or a bounded-capacity error.
 * @param[in,out] opaque Fixture I/O state.
 * @param[in] src Source bytes.
 * @param[in] requested Requested bytes.
 * @param[out] out_written Actual copied bytes.
 * @return Fixture write status.
 * @retval k_ra8_ok Full or deliberately short fixture transfer.
 * @retval k_ra8_err_invalid_size Destination capacity is exhausted.
 * @pre All pointer arguments are non-NULL.
 * @pre @p src spans @p requested readable bytes.
 * @post The write call count is incremented.
 * @post @p out_written reports exactly the appended prefix.
 * @note Test-only and not thread-safe.
 * @since 0.1.0
 */
static ra8_err_t
stream_write(void* opaque, const uint8_t* src, uint32_t requested, uint32_t* out_written)
{
  rabook_stream_io_t* io = (rabook_stream_io_t*)opaque;
  io->write_calls++;
  if (requested > (io->write_cap - io->write_used)) {
    *out_written = 0U;
    return k_ra8_err_invalid_size;
  }
  uint32_t delivered = requested;
  if (io->short_write && (requested != 0U)) {
    delivered--;
    io->short_write = false;
  }
  (void)memcpy(&io->write_data[io->write_used], src, (size_t)delivered);
  io->write_used += delivered;
  *out_written = delivered;
  return k_ra8_ok;
}

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
 * @brief Append the two-byte fixture cover through either pool API.
 * @param[in,out] ctx Initialised compiler context.
 * @param[in] external_image_pool Select the external logical pool reservation.
 * @return Appended image descriptor index.
 * @retval uint32_t Valid cover-image index.
 * @pre @p ctx is non-NULL and has image/string capacity.
 * @pre Shared external-pool storage is writable when external mode is selected.
 * @post The builder contains one 2x2 gray4 image.
 * @post External mode stores the bytes at the returned logical pool offset.
 * @note Test-only; mutates shared fixture storage.
 * @since 0.1.0
 */
static uint32_t rabook_rt_add_cover(ra8_rabook_ctx_t* ctx, bool external_image_pool)
{
  static const uint8_t img_bytes[2] = {0xF0U, 0x0FU};
  const uint32_t       img_href     = ra8_rabook_intern(ctx, "cover.png");
  if (!external_image_pool) {
    return ra8_rabook_add_image(ctx,
                                img_href,
                                (uint16_t)k_t_img_w,
                                (uint16_t)k_t_img_h,
                                (uint8_t)k_ra8_book_image_gray4,
                                (uint8_t)k_ra8_book_pixfmt_gray4,
                                img_bytes,
                                (uint32_t)sizeof(img_bytes));
  }
  uint32_t       data_off = 0U;
  const uint32_t idx      = ra8_rabook_add_image_external(ctx,
                                                          img_href,
                                                          (uint16_t)k_t_img_w,
                                                          (uint16_t)k_t_img_h,
                                                          (uint8_t)k_ra8_book_image_gray4,
                                                          (uint8_t)k_ra8_book_pixfmt_gray4,
                                                          (uint32_t)sizeof(img_bytes),
                                                          &data_off);
  TEST_ASSERT_EQ(0U, data_off);
  (void)memcpy(&s_external_pool[data_off], img_bytes, sizeof(img_bytes));
  return idx;
}

/**
 * @brief Build the fixture book (metadata, cover, stylesheet, DOM, chapter).
 * @param[in,out] ctx Initialised compiler context.
 * @param[out]    rt  Receives the node / image indices and the "body" offset.
 * @param[in] external_image_pool Whether the cover bytes use the external-pool API.
 * @pre rabook_rt_init(@p ctx) already ran.
 * @post @p rt index fields are set; the book is ready to finalise.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void rabook_rt_populate(ra8_rabook_ctx_t* ctx, rabook_rt_t* rt, bool external_image_pool)
{
  /* Intern "" first so empty/default offsets resolve to "" at offset 0. */
  TEST_ASSERT_EQ(0U, ra8_rabook_intern(ctx, ""));

  const uint32_t title_off  = ra8_rabook_intern(ctx, "The Title");
  const uint32_t author_off = ra8_rabook_intern(ctx, "An Author");
  const uint32_t lang_off   = ra8_rabook_intern(ctx, "en");
  const uint32_t id_off     = ra8_rabook_intern(ctx, "urn:isbn:123");

  rt->img_idx = rabook_rt_add_cover(ctx, external_image_pool);
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
  rabook_rt_populate(&ctx, &rt, false);

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
 * @test test_rabook_stream_external_parity
 * @brief A one-byte-scratch external finalizer is byte-identical to memory finalize.
 * @details Builds the same complete fixture through each image-pool API, validates
 *          the streamed result, and compares every emitted byte including CRC.
 * @pre Shared fixture arenas are writable.
 * @pre The production validator is linked into this host test.
 * @post Internal and external stream outputs equal the memory-wrapper blob.
 * @post External reads never exceed the one-byte caller scratch.
 * @note Test-only; mutates shared fixture arenas.
 * @since 0.1.0
 */
static void test_rabook_stream_external_parity(void)
{
  TEST_BEGIN("ra8_rabook_compile: external stream == memory blob");
  ra8_rabook_ctx_t ctx = {};
  rabook_rt_t      rt  = {};
  rabook_rt_init(&ctx);
  rabook_rt_populate(&ctx, &rt, false);
  const void* mem_blob = nullptr;
  uint32_t    mem_len  = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rabook_finalize(&ctx, &mem_blob, &mem_len));
  (void)memcpy(s_expected, mem_blob, (size_t)mem_len);

  rabook_stream_io_t io = {.write_data = s_stream_out, .write_cap = (uint32_t)sizeof(s_stream_out)};
  uint32_t           stream_len = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_rabook_finalize_stream(&ctx,
                                            nullptr,
                                            nullptr,
                                            stream_write,
                                            &io,
                                            nullptr,
                                            0U,
                                            &stream_len));
  TEST_ASSERT_EQ(mem_len, stream_len);
  TEST_ASSERT_EQ(0, memcmp(s_expected, s_stream_out, (size_t)stream_len));

  rabook_rt_init(&ctx);
  rt = (rabook_rt_t){};
  rabook_rt_populate(&ctx, &rt, true);
  io                 = (rabook_stream_io_t){.read_data  = s_external_pool,
                                            .write_data = s_stream_out,
                                            .read_len   = 2U,
                                            .write_cap  = (uint32_t)sizeof(s_stream_out)};
  uint8_t scratch[1] = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_rabook_finalize_stream(&ctx,
                                            stream_read,
                                            &io,
                                            stream_write,
                                            &io,
                                            scratch,
                                            (uint32_t)sizeof(scratch),
                                            &stream_len));
  TEST_ASSERT_EQ(mem_len, stream_len);
  TEST_ASSERT_EQ(0, memcmp(s_expected, s_stream_out, (size_t)stream_len));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_book_validate(s_stream_out, (size_t)stream_len));
  TEST_ASSERT_EQ(1U, io.max_read);
  TEST_ASSERT_EQ(4U, io.read_calls); /* two bytes, once for CRC and once for emit */
  TEST_ASSERT(io.write_calls > 1U);
  TEST_END("ra8_rabook_compile: external stream == memory blob");
}

/**
 * @test test_rabook_stream_short_io
 * @brief Successful-but-short callbacks are rejected and leave out_len untouched.
 * @details Injects one short read during the CRC pass and one short header write,
 *          proving neither is accepted as a complete stream.
 * @pre Shared fixture arenas are writable.
 * @pre The external cover fixture contains two readable bytes.
 * @post Both short transfers return @ref k_ra8_err_invalid_size.
 * @post The sentinel output length remains unchanged.
 * @note Test-only; mutates shared fixture arenas.
 * @since 0.1.0
 */
static void test_rabook_stream_short_io(void)
{
  TEST_BEGIN("ra8_rabook_compile: short stream read/write rejected");
  ra8_rabook_ctx_t ctx = {};
  rabook_rt_t      rt  = {};
  rabook_rt_init(&ctx);
  rabook_rt_populate(&ctx, &rt, true);
  uint8_t  scratch[2] = {};
  uint32_t out_len    = UINT32_C(0xA5A5A5A5);

  rabook_stream_io_t io = {.read_data  = s_external_pool,
                           .write_data = s_stream_out,
                           .read_len   = 2U,
                           .write_cap  = (uint32_t)sizeof(s_stream_out),
                           .short_read = true};
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_rabook_finalize_stream(&ctx,
                                            stream_read,
                                            &io,
                                            stream_write,
                                            &io,
                                            scratch,
                                            (uint32_t)sizeof(scratch),
                                            &out_len));
  TEST_ASSERT_EQ(UINT32_C(0xA5A5A5A5), out_len);
  TEST_ASSERT_EQ(0U, io.write_calls); /* CRC pass fails before header publication. */

  io = (rabook_stream_io_t){.read_data   = s_external_pool,
                            .write_data  = s_stream_out,
                            .read_len    = 2U,
                            .write_cap   = (uint32_t)sizeof(s_stream_out),
                            .short_write = true};
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_rabook_finalize_stream(&ctx,
                                            stream_read,
                                            &io,
                                            stream_write,
                                            &io,
                                            scratch,
                                            (uint32_t)sizeof(scratch),
                                            &out_len));
  TEST_ASSERT_EQ(UINT32_C(0xA5A5A5A5), out_len);
  TEST_ASSERT_EQ(1U, io.read_calls); /* complete CRC pass precedes the short header write */
  TEST_ASSERT_EQ(1U, io.write_calls);
  TEST_END("ra8_rabook_compile: short stream read/write rejected");
}

/**
 * @test test_rabook_stream_write_exhaustion
 * @brief Destination exhaustion propagates without publishing a final length.
 * @details Gives the bounded test writer less than one header of capacity after
 *          a successful external-pool CRC pass.
 * @pre Shared fixture and external-pool arenas are writable.
 * @pre The bounded writer reports invalid size without modifying its cursor.
 * @post Finalization reports invalid size after one CRC read and one write call.
 * @post The caller's output length remains unchanged.
 * @note Test-only; exercises writer error propagation, not a short success.
 * @since 0.1.0
 */
static void test_rabook_stream_write_exhaustion(void)
{
  TEST_BEGIN("ra8_rabook_compile: stream destination exhaustion");
  ra8_rabook_ctx_t ctx = {};
  rabook_rt_t      rt  = {};
  rabook_rt_init(&ctx);
  rabook_rt_populate(&ctx, &rt, true);

  rabook_stream_io_t io         = {.read_data  = s_external_pool,
                                   .write_data = s_stream_out,
                                   .read_len   = 2U,
                                   .write_cap  = 1U};
  uint8_t            scratch[2] = {};
  uint32_t           out_len    = UINT32_C(0xA5A5A5A5);
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_rabook_finalize_stream(&ctx,
                                            stream_read,
                                            &io,
                                            stream_write,
                                            &io,
                                            scratch,
                                            (uint32_t)sizeof(scratch),
                                            &out_len));
  TEST_ASSERT_EQ(1U, io.read_calls);
  TEST_ASSERT_EQ(1U, io.write_calls);
  TEST_ASSERT_EQ(0U, io.write_used);
  TEST_ASSERT_EQ(UINT32_C(0xA5A5A5A5), out_len);
  TEST_END("ra8_rabook_compile: stream destination exhaustion");
}

/**
 * @test test_rabook_external_offsets
 * @brief External spans are contiguous and cannot be mixed with internal bytes.
 * @details Checks sequential offsets and mixed-mode rejection after the logical
 *          external pool becomes non-empty.
 * @pre Shared fixture arenas are writable.
 * @pre The image table admits at least two descriptors.
 * @post Reserved offsets exactly tile the logical image pool.
 * @post A later non-empty internal append latches builder failure.
 * @note Test-only; mutates shared fixture arenas.
 * @since 0.1.0
 */
static void test_rabook_external_offsets(void)
{
  TEST_BEGIN("ra8_rabook_compile: external pool offsets and mode");
  ra8_rabook_buffers_t buf = full_buffers();
  ra8_rabook_ctx_t     ctx = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rabook_compile_init(&ctx, &buf));
  const uint32_t href = ra8_rabook_intern(&ctx, "spooled.bin");
  uint32_t       off  = UINT32_MAX;
  TEST_ASSERT_EQ(0U,
                 ra8_rabook_add_image_external(&ctx,
                                               href,
                                               2U,
                                               2U,
                                               (uint8_t)k_ra8_book_image_gray4,
                                               (uint8_t)k_ra8_book_pixfmt_gray4,
                                               2U,
                                               &off));
  TEST_ASSERT_EQ(0U, off);
  TEST_ASSERT_EQ(1U,
                 ra8_rabook_add_image_external(&ctx,
                                               href,
                                               2U,
                                               3U,
                                               (uint8_t)k_ra8_book_image_gray4,
                                               (uint8_t)k_ra8_book_pixfmt_gray4,
                                               3U,
                                               &off));
  TEST_ASSERT_EQ(2U, off);
  TEST_ASSERT_EQ(5U, ctx.image_pool_size);

  static const uint8_t local[1] = {0U};
  TEST_ASSERT_EQ(k_ra8_book_nil,
                 ra8_rabook_add_image(&ctx,
                                      href,
                                      1U,
                                      1U,
                                      (uint8_t)k_ra8_book_image_gray4,
                                      (uint8_t)k_ra8_book_pixfmt_gray4,
                                      local,
                                      1U));
  TEST_ASSERT(ctx.failed);
  TEST_END("ra8_rabook_compile: external pool offsets and mode");
}

/**
 * @test test_rabook_external_pool_overflow
 * @brief Logical external-pool uint32 addition overflow fails closed.
 * @details Places the logical cursor at UINT32_MAX and verifies that reserving
 *          one additional byte is rejected without descriptor publication.
 * @pre Shared fixture arenas are writable.
 * @pre The image table admits one descriptor.
 * @post The append returns nil and latches builder failure.
 * @post The logical pool size remains UINT32_MAX.
 * @note Test-only; directly establishes the overflow boundary state.
 * @since 0.1.0
 */
static void test_rabook_external_pool_overflow(void)
{
  TEST_BEGIN("ra8_rabook_compile: external pool uint32 overflow");
  ra8_rabook_buffers_t buf = full_buffers();
  ra8_rabook_ctx_t     ctx = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rabook_compile_init(&ctx, &buf));
  const uint32_t href = ra8_rabook_intern(&ctx, "spooled.bin");
  uint32_t       off  = 0U;
  ctx.image_pool_size = UINT32_MAX;
  TEST_ASSERT_EQ(k_ra8_book_nil,
                 ra8_rabook_add_image_external(&ctx,
                                               href,
                                               1U,
                                               1U,
                                               (uint8_t)k_ra8_book_image_gray4,
                                               (uint8_t)k_ra8_book_pixfmt_gray4,
                                               1U,
                                               &off));
  TEST_ASSERT(ctx.failed);
  TEST_ASSERT_EQ(UINT32_MAX, ctx.image_pool_size);
  TEST_END("ra8_rabook_compile: external pool uint32 overflow");
}

/**
 * @test test_rabook_external_layout_overflow
 * @brief A representable external span that breaks final layout is rejected early.
 * @details Reserves UINT32_MAX logical pool bytes and verifies layout arithmetic
 *          fails before the finalizer invokes the spool reader or writer.
 * @pre Shared fixture arenas are writable.
 * @pre The image table admits one descriptor.
 * @post Finalization reports invalid size and performs no callback I/O.
 * @post The caller's output length remains unchanged.
 * @note Test-only; no UINT32_MAX backing allocation is required.
 * @since 0.1.0
 */
static void test_rabook_external_layout_overflow(void)
{
  TEST_BEGIN("ra8_rabook_compile: external pool layout overflow");
  ra8_rabook_buffers_t buf = full_buffers();
  ra8_rabook_ctx_t     ctx = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rabook_compile_init(&ctx, &buf));
  const uint32_t href = ra8_rabook_intern(&ctx, "spooled.bin");
  uint32_t       off  = 0U;
  TEST_ASSERT_EQ(0U,
                 ra8_rabook_add_image_external(&ctx,
                                               href,
                                               1U,
                                               1U,
                                               (uint8_t)k_ra8_book_image_gray4,
                                               (uint8_t)k_ra8_book_pixfmt_gray4,
                                               UINT32_MAX,
                                               &off));
  rabook_stream_io_t io         = {.read_data  = s_external_pool,
                                   .write_data = s_stream_out,
                                   .read_len   = (uint32_t)sizeof(s_external_pool),
                                   .write_cap  = (uint32_t)sizeof(s_stream_out)};
  uint8_t            scratch[1] = {};
  uint32_t           out_len    = UINT32_C(0xA5A5A5A5);
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_rabook_finalize_stream(&ctx,
                                            stream_read,
                                            &io,
                                            stream_write,
                                            &io,
                                            scratch,
                                            (uint32_t)sizeof(scratch),
                                            &out_len));
  TEST_ASSERT_EQ(0U, io.read_calls); /* layout overflow precedes any spool access */
  TEST_ASSERT_EQ(0U, io.write_calls);
  TEST_ASSERT_EQ(UINT32_C(0xA5A5A5A5), out_len);
  TEST_END("ra8_rabook_compile: external pool layout overflow");
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
  test_rabook_stream_external_parity();
  test_rabook_stream_short_io();
  test_rabook_stream_write_exhaustion();
  test_rabook_external_offsets();
  test_rabook_external_pool_overflow();
  test_rabook_external_layout_overflow();
  (void)fprintf(stderr, "[OK ] test_ra8_rabook_compile.c\n");
  return 0;
}
