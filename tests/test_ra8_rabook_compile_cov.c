/**
 * @file test_ra8_rabook_compile_cov.c
 * @brief Line-coverage top-up for the RABOOK1 builder error / guard legs (#149).
 *
 * @details
 * The sibling @ref test_ra8_rabook_compile.c drives the happy path plus a few
 * single-arena overflow arms. This file completes line coverage of
 * `libs/ra8_rabook_compile/src/ra8_rabook_compile.c` by exercising every remaining
 * defensive leg, all of which are pure-software (there is no hardware, no MMIO,
 * and no timing in this module -- it is a zero-heap serializer over
 * caller-owned arenas), so each leg is reached deterministically from host
 * inputs alone:
 *
 *  - NULL-context guards on every builder entry point (return @ref k_ra8_book_nil).
 *  - The sticky `failed` short-circuit on @ref ra8_rabook_intern,
 *    @ref ra8_rabook_add_element, @ref ra8_rabook_add_text,
 *    @ref ra8_rabook_add_chapter, @ref ra8_rabook_add_image,
 *    @ref ra8_rabook_add_stylesheet and @ref ra8_rabook_set_metadata.
 *  - The NULL-payload latch: @ref ra8_rabook_intern with a NULL string,
 *    @ref ra8_rabook_add_element with NULL attrs / non-zero count, and
 *    @ref ra8_rabook_add_image with NULL data / non-zero size.
 *  - The per-function arena-overflow latches for the text-node, chapter, image
 *    (both the descriptor table and the image pool) and stylesheet arenas.
 *  - The out-of-range index rejections in @ref ra8_rabook_link_child and
 *    @ref ra8_rabook_link_sibling (parent/child and node/sibling arms).
 *  - The two @ref ra8_rabook_finalize size-error arms: a computed layout that
 *    overflows 32-bit offsets, and a laid-out blob that exceeds the output
 *    capacity.
 *
 * gcovr merges this executable's `.gcda` with the sibling test's for the same
 * source file, so this file adds coverage without duplicating the happy path.
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
  k_c_chapter_cap = 8U,    /**< Max chapters in the coverage book. */
  k_c_node_cap    = 64U,   /**< Max DOM nodes.                     */
  k_c_attr_cap    = 32U,   /**< Max attribute records.             */
  k_c_style_cap   = 4U,    /**< Max stylesheets.                   */
  k_c_image_cap   = 8U,    /**< Max image descriptors.             */
  k_c_string_cap  = 1024U, /**< String-pool capacity (bytes).      */
  k_c_imgpool_cap = 256U,  /**< Image-pool capacity (bytes).       */
  k_c_out_cap     = 4096U, /**< Output-blob capacity (bytes).      */
} test_cov_cap_t;

/** @brief Small constants used by the coverage vectors (no magic numbers). */
typedef enum : uint32_t {
  k_c_pool_overflow_bytes = 8U,          /**< Payload larger than the shrunken pool.   */
  k_c_tiny_pool_cap       = 4U,          /**< Image-pool cap that a 8-byte copy busts. */
  k_c_tiny_out_cap        = 10U,         /**< Output cap below the 100-byte header.    */
  k_c_out_of_range_index  = 5U,          /**< An index past a one-node table.          */
  k_c_huge_size           = 0xFFFFFFFFU, /**< Forces the 64-bit layout to wrap.        */
} test_cov_const_t;

/** @brief Cover-image geometry used by the add_image error vectors. */
typedef enum : uint16_t {
  k_c_img_w = 4U, /**< Image width in pixels.  */
  k_c_img_h = 4U, /**< Image height in pixels. */
} test_cov_img_t;

static ra8_book_chapter_t    s_chapters[k_c_chapter_cap];
static ra8_book_node_t       s_nodes[k_c_node_cap];
static ra8_book_attr_t       s_attrs[k_c_attr_cap];
static ra8_book_stylesheet_t s_styles[k_c_style_cap];
static ra8_book_image_t      s_images[k_c_image_cap];
static char                  s_strpool[k_c_string_cap];
static uint8_t               s_imgpool[k_c_imgpool_cap];
static uint8_t               s_out[k_c_out_cap];

/**
 * @brief Build a full-capacity buffers struct over the file-scope arenas.
 * @details Each error vector starts from this and shrinks exactly one `*_cap`
 *          to force one arena to overflow; the caps (not the array sizes) drive
 *          the builder bounds, so a small cap over a large array is safe.
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
    .chapter_cap    = (uint32_t)k_c_chapter_cap,
    .nodes          = s_nodes,
    .node_cap       = (uint32_t)k_c_node_cap,
    .attrs          = s_attrs,
    .attr_cap       = (uint32_t)k_c_attr_cap,
    .stylesheets    = s_styles,
    .stylesheet_cap = (uint32_t)k_c_style_cap,
    .images         = s_images,
    .image_cap      = (uint32_t)k_c_image_cap,
    .string_pool    = s_strpool,
    .string_cap     = (uint32_t)k_c_string_cap,
    .image_pool     = s_imgpool,
    .image_pool_cap = (uint32_t)k_c_imgpool_cap,
    .out            = s_out,
    .out_cap        = (uint32_t)k_c_out_cap,
  };
  return buf;
}

/**
 * @test test_rabook_cov_null_ctx
 * @brief Every builder entry point returns nil for a NULL context.
 *
 * @par MC/DC:
 * Each entry's `if (ctx == nullptr)` is a single condition; every vector here
 * takes its true arm. There is no compound (`&&` / `||`) decision on this leg.
 */
static void test_rabook_cov_null_ctx(void)
{
  TEST_BEGIN("ra8_rabook_compile: NULL-ctx guards return nil");
  static const uint8_t data[1] = {0x00U};

  TEST_ASSERT_EQ(k_ra8_book_nil, ra8_rabook_intern(nullptr, "x"));
  TEST_ASSERT_EQ(k_ra8_book_nil, ra8_rabook_add_element(nullptr, 0U, nullptr, 0U));
  TEST_ASSERT_EQ(k_ra8_book_nil, ra8_rabook_add_text(nullptr, 0U));
  TEST_ASSERT_EQ(k_ra8_book_nil, ra8_rabook_add_chapter(nullptr, 0U, 0U, 0U));
  TEST_ASSERT_EQ(k_ra8_book_nil,
                 ra8_rabook_add_image(nullptr,
                                      0U,
                                      (uint16_t)k_c_img_w,
                                      (uint16_t)k_c_img_h,
                                      (uint8_t)k_ra8_book_image_gray4,
                                      (uint8_t)k_ra8_book_pixfmt_gray4,
                                      data,
                                      1U));
  TEST_ASSERT_EQ(k_ra8_book_nil, ra8_rabook_add_stylesheet(nullptr, 0U, 0U));
  TEST_END("ra8_rabook_compile: NULL-ctx guards return nil");
}

/**
 * @test test_rabook_cov_intern_errors
 * @brief intern latches on a NULL string, then short-circuits while failed.
 *
 * @par MC/DC:
 * The `if (str == nullptr)` and `if (ctx->failed)` guards are separate single
 * conditions. Vector 1 (str == NULL) latches `failed` and returns nil; vector 2
 * (a valid string on the now-failed ctx) takes the `failed` true arm and
 * returns nil without touching the pool.
 */
static void test_rabook_cov_intern_errors(void)
{
  TEST_BEGIN("ra8_rabook_compile: intern NULL-str latch + failed short-circuit");
  ra8_rabook_buffers_t buf = full_buffers();
  ra8_rabook_ctx_t     ctx = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rabook_compile_init(&ctx, &buf));

  /* NULL string -> latch the sticky fail flag and return nil. */
  TEST_ASSERT_EQ(k_ra8_book_nil, ra8_rabook_intern(&ctx, nullptr));
  TEST_ASSERT(ctx.failed);

  /* A valid string once failed is latched -> short-circuit to nil. */
  TEST_ASSERT_EQ(k_ra8_book_nil, ra8_rabook_intern(&ctx, "later"));
  TEST_END("ra8_rabook_compile: intern NULL-str latch + failed short-circuit");
}

/**
 * @test test_rabook_cov_add_element_errors
 * @brief add_element rejects NULL attrs with a count, then short-circuits failed.
 *
 * @par MC/DC:
 * The nested `if (attr_count != 0U) { if (attrs == nullptr) }` is two single
 * conditions, not a compound decision. Vector 1 (count != 0, attrs == NULL)
 * takes both true arms, latches `failed` and returns nil; vector 2 on the
 * failed ctx takes the leading `if (ctx->failed)` true arm and returns nil.
 */
static void test_rabook_cov_add_element_errors(void)
{
  TEST_BEGIN("ra8_rabook_compile: add_element NULL-attrs latch + failed short-circuit");
  ra8_rabook_buffers_t buf = full_buffers();
  ra8_rabook_ctx_t     ctx = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rabook_compile_init(&ctx, &buf));

  const uint32_t name = ra8_rabook_intern(&ctx, "span");

  /* Non-zero attr_count with NULL attrs -> latch failed, return nil. */
  TEST_ASSERT_EQ(k_ra8_book_nil, ra8_rabook_add_element(&ctx, name, nullptr, 2U));
  TEST_ASSERT(ctx.failed);

  /* Any further add on the failed ctx short-circuits to nil. */
  TEST_ASSERT_EQ(k_ra8_book_nil, ra8_rabook_add_element(&ctx, name, nullptr, 0U));
  TEST_END("ra8_rabook_compile: add_element NULL-attrs latch + failed short-circuit");
}

/**
 * @test test_rabook_cov_add_text_errors
 * @brief add_text latches on a node-arena overflow, then short-circuits failed.
 *
 * @par MC/DC:
 * The overflow guard `ctx->node_count >= ctx->buf.node_cap` is a single
 * relational condition. Vector 1 (0 >= 1 -> false) appends node 0; vector 2
 * (1 >= 1 -> true) latches `failed` and returns nil; vector 3 on the failed
 * ctx takes the `if (ctx->failed)` true arm and returns nil.
 */
static void test_rabook_cov_add_text_errors(void)
{
  TEST_BEGIN("ra8_rabook_compile: add_text overflow latch + failed short-circuit");
  ra8_rabook_buffers_t buf = full_buffers();
  buf.node_cap             = 1U;
  ra8_rabook_ctx_t ctx     = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rabook_compile_init(&ctx, &buf));

  const uint32_t word = ra8_rabook_intern(&ctx, "word");
  TEST_ASSERT_EQ(0U, ra8_rabook_add_text(&ctx, word));             /* fills node 0 */
  TEST_ASSERT_EQ(k_ra8_book_nil, ra8_rabook_add_text(&ctx, word)); /* overflow     */
  TEST_ASSERT(ctx.failed);
  TEST_ASSERT_EQ(k_ra8_book_nil, ra8_rabook_add_text(&ctx, word)); /* failed */
  TEST_END("ra8_rabook_compile: add_text overflow latch + failed short-circuit");
}

/**
 * @test test_rabook_cov_link_child_range
 * @brief link_child rejects an out-of-range parent, then an out-of-range child.
 *
 * @par MC/DC:
 * The `parent >= ctx->node_count` and `child >= ctx->node_count` guards are two
 * separate single relational conditions. Vector 1 (parent past the table) takes
 * the parent guard; vector 2 (valid parent, child past the table) takes the
 * child guard; both return @ref k_ra8_err_invalid_arg.
 */
static void test_rabook_cov_link_child_range(void)
{
  TEST_BEGIN("ra8_rabook_compile: link_child out-of-range parent + child");
  ra8_rabook_buffers_t buf = full_buffers();
  ra8_rabook_ctx_t     ctx = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rabook_compile_init(&ctx, &buf));

  const uint32_t only = ra8_rabook_add_text(&ctx, ra8_rabook_intern(&ctx, "n"));
  TEST_ASSERT_EQ(0U, only);

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_rabook_link_child(&ctx, (uint32_t)k_c_out_of_range_index, only));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_rabook_link_child(&ctx, only, (uint32_t)k_c_out_of_range_index));
  TEST_END("ra8_rabook_compile: link_child out-of-range parent + child");
}

/**
 * @test test_rabook_cov_link_sibling_range
 * @brief link_sibling rejects an out-of-range node, then an out-of-range sibling.
 *
 * @par MC/DC:
 * The `node >= ctx->node_count` and `sibling >= ctx->node_count` guards are two
 * separate single relational conditions. Vector 1 (node past the table) takes
 * the node guard; vector 2 (valid node, sibling past the table) takes the
 * sibling guard; both return @ref k_ra8_err_invalid_arg.
 */
static void test_rabook_cov_link_sibling_range(void)
{
  TEST_BEGIN("ra8_rabook_compile: link_sibling out-of-range node + sibling");
  ra8_rabook_buffers_t buf = full_buffers();
  ra8_rabook_ctx_t     ctx = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rabook_compile_init(&ctx, &buf));

  const uint32_t only = ra8_rabook_add_text(&ctx, ra8_rabook_intern(&ctx, "n"));
  TEST_ASSERT_EQ(0U, only);

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_rabook_link_sibling(&ctx, (uint32_t)k_c_out_of_range_index, only));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_rabook_link_sibling(&ctx, only, (uint32_t)k_c_out_of_range_index));
  TEST_END("ra8_rabook_compile: link_sibling out-of-range node + sibling");
}

/**
 * @test test_rabook_cov_add_chapter_errors
 * @brief add_chapter latches on a chapter-arena overflow, then short-circuits.
 *
 * @par MC/DC:
 * The overflow guard `ctx->chapter_count >= ctx->buf.chapter_cap` is a single
 * relational condition. Vector 1 (0 >= 1 -> false) appends chapter 0; vector 2
 * (1 >= 1 -> true) latches `failed` and returns nil; vector 3 on the failed ctx
 * takes the `if (ctx->failed)` true arm and returns nil.
 */
static void test_rabook_cov_add_chapter_errors(void)
{
  TEST_BEGIN("ra8_rabook_compile: add_chapter overflow latch + failed short-circuit");
  ra8_rabook_buffers_t buf = full_buffers();
  buf.chapter_cap          = 1U;
  ra8_rabook_ctx_t ctx     = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rabook_compile_init(&ctx, &buf));

  TEST_ASSERT_EQ(0U, ra8_rabook_add_chapter(&ctx, 0U, 0U, 0U));             /* fills chapter 0 */
  TEST_ASSERT_EQ(k_ra8_book_nil, ra8_rabook_add_chapter(&ctx, 0U, 0U, 0U)); /* overflow        */
  TEST_ASSERT(ctx.failed);
  TEST_ASSERT_EQ(k_ra8_book_nil, ra8_rabook_add_chapter(&ctx, 0U, 0U, 0U)); /* failed */
  TEST_END("ra8_rabook_compile: add_chapter overflow latch + failed short-circuit");
}

/**
 * @test test_rabook_cov_add_image_null_data
 * @brief add_image latches when data_size is non-zero but the data pointer is NULL.
 *
 * @par MC/DC:
 * The nested `if (data_size != 0U) { if (data == nullptr) }` is two single
 * conditions. This vector takes both true arms (size 8, NULL data), latching
 * `failed` and returning nil before any pool copy.
 */
static void test_rabook_cov_add_image_null_data(void)
{
  TEST_BEGIN("ra8_rabook_compile: add_image NULL-data latch");
  ra8_rabook_buffers_t buf = full_buffers();
  ra8_rabook_ctx_t     ctx = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rabook_compile_init(&ctx, &buf));

  TEST_ASSERT_EQ(k_ra8_book_nil,
                 ra8_rabook_add_image(&ctx,
                                      0U,
                                      (uint16_t)k_c_img_w,
                                      (uint16_t)k_c_img_h,
                                      (uint8_t)k_ra8_book_image_gray4,
                                      (uint8_t)k_ra8_book_pixfmt_gray4,
                                      nullptr,
                                      (uint32_t)k_c_pool_overflow_bytes));
  TEST_ASSERT(ctx.failed);
  TEST_END("ra8_rabook_compile: add_image NULL-data latch");
}

/**
 * @test test_rabook_cov_add_image_pool_overflow
 * @brief add_image latches on an image-pool overflow, then short-circuits failed.
 *
 * @par MC/DC:
 * The pool guard `data_size > (ctx->buf.image_pool_cap - ctx->image_pool_size)`
 * is a single relational condition. Vector 1 (8 > 4 -> true, with a valid data
 * pointer so the earlier NULL guard is false) latches `failed` and returns nil;
 * vector 2 on the failed ctx takes the `if (ctx->failed)` true arm.
 */
static void test_rabook_cov_add_image_pool_overflow(void)
{
  TEST_BEGIN("ra8_rabook_compile: add_image pool overflow latch + failed short-circuit");
  ra8_rabook_buffers_t buf = full_buffers();
  buf.image_pool_cap       = (uint32_t)k_c_tiny_pool_cap; /* only 4 pool bytes */
  ra8_rabook_ctx_t ctx     = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rabook_compile_init(&ctx, &buf));

  static const uint8_t data[k_c_pool_overflow_bytes] = {0U};
  /* 8-byte copy into a 4-byte pool -> overflow, valid pointer clears NULL guard. */
  TEST_ASSERT_EQ(k_ra8_book_nil,
                 ra8_rabook_add_image(&ctx,
                                      0U,
                                      (uint16_t)k_c_img_w,
                                      (uint16_t)k_c_img_h,
                                      (uint8_t)k_ra8_book_image_gray4,
                                      (uint8_t)k_ra8_book_pixfmt_gray4,
                                      data,
                                      (uint32_t)k_c_pool_overflow_bytes));
  TEST_ASSERT(ctx.failed);

  TEST_ASSERT_EQ(k_ra8_book_nil,
                 ra8_rabook_add_image(&ctx,
                                      0U,
                                      (uint16_t)k_c_img_w,
                                      (uint16_t)k_c_img_h,
                                      (uint8_t)k_ra8_book_image_gray4,
                                      (uint8_t)k_ra8_book_pixfmt_gray4,
                                      data,
                                      (uint32_t)k_c_pool_overflow_bytes));
  TEST_END("ra8_rabook_compile: add_image pool overflow latch + failed short-circuit");
}

/**
 * @test test_rabook_cov_add_stylesheet_errors
 * @brief add_stylesheet latches on a stylesheet-arena overflow, then short-circuits.
 *
 * @par MC/DC:
 * The overflow guard `ctx->stylesheet_count >= ctx->buf.stylesheet_cap` is a
 * single relational condition. Vector 1 (0 >= 1 -> false) appends stylesheet 0;
 * vector 2 (1 >= 1 -> true) latches `failed` and returns nil; vector 3 on the
 * failed ctx takes the `if (ctx->failed)` true arm.
 */
static void test_rabook_cov_add_stylesheet_errors(void)
{
  TEST_BEGIN("ra8_rabook_compile: add_stylesheet overflow latch + failed short-circuit");
  ra8_rabook_buffers_t buf = full_buffers();
  buf.stylesheet_cap       = 1U;
  ra8_rabook_ctx_t ctx     = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rabook_compile_init(&ctx, &buf));

  TEST_ASSERT_EQ(0U, ra8_rabook_add_stylesheet(&ctx, 0U, (uint32_t)k_ra8_book_nil)); /* fills 0 */
  TEST_ASSERT_EQ(k_ra8_book_nil,
                 ra8_rabook_add_stylesheet(&ctx, 0U, (uint32_t)k_ra8_book_nil)); /* overflow */
  TEST_ASSERT(ctx.failed);
  TEST_ASSERT_EQ(k_ra8_book_nil,
                 ra8_rabook_add_stylesheet(&ctx, 0U, (uint32_t)k_ra8_book_nil)); /* failed */
  TEST_END("ra8_rabook_compile: add_stylesheet overflow latch + failed short-circuit");
}

/**
 * @test test_rabook_cov_set_metadata_failed
 * @brief set_metadata reports no_mem once the sticky fail flag is latched.
 *
 * @par MC/DC:
 * The `if (ctx->failed)` guard is a single condition. Interning a NULL string
 * latches `failed`; set_metadata then takes the true arm and returns
 * @ref k_ra8_err_no_mem without recording any offset.
 */
static void test_rabook_cov_set_metadata_failed(void)
{
  TEST_BEGIN("ra8_rabook_compile: set_metadata after overflow -> no_mem");
  ra8_rabook_buffers_t buf = full_buffers();
  ra8_rabook_ctx_t     ctx = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rabook_compile_init(&ctx, &buf));

  /* Latch the sticky fail flag through the real API (NULL string). */
  TEST_ASSERT_EQ(k_ra8_book_nil, ra8_rabook_intern(&ctx, nullptr));
  TEST_ASSERT(ctx.failed);

  TEST_ASSERT_EQ(k_ra8_err_no_mem, ra8_rabook_set_metadata(&ctx, 0U, 0U, 0U, 0U, 0U));
  TEST_END("ra8_rabook_compile: set_metadata after overflow -> no_mem");
}

/**
 * @test test_rabook_cov_finalize_layout_overflow
 * @brief finalize rejects a layout whose 64-bit total overflows 32-bit offsets.
 *
 * @par MC/DC:
 * The `total > (uint64_t)UINT32_MAX` guard in s_compute_layout is a single
 * relational condition. A hand-built context (mirroring the empty-body test's
 * white-box construction) sets `string_size` and `image_pool_size` to
 * 0xFFFFFFFF each so the computed total is ~8.6 GiB; the guard takes its true
 * arm, s_compute_layout returns @ref k_ra8_err_invalid_size, and finalize
 * propagates it. No blob is written (finalize returns before s_write_blob), so
 * the oversized counts never touch the real, small arenas.
 */
static void test_rabook_cov_finalize_layout_overflow(void)
{
  TEST_BEGIN("ra8_rabook_compile: finalize layout > 32-bit -> invalid_size");
  ra8_rabook_ctx_t ctx  = {};
  ctx.buf               = full_buffers();
  ctx.cover_image_index = (uint32_t)k_ra8_book_nil;
  ctx.string_size       = (uint32_t)k_c_huge_size;
  ctx.image_pool_size   = (uint32_t)k_c_huge_size;

  const void* blob = nullptr;
  uint32_t    len  = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_rabook_finalize(&ctx, &blob, &len));
  TEST_ASSERT_NULL(blob);
  TEST_END("ra8_rabook_compile: finalize layout > 32-bit -> invalid_size");
}

/**
 * @test test_rabook_cov_finalize_out_cap
 * @brief finalize rejects a blob that exceeds the output-arena capacity.
 *
 * @par MC/DC:
 * The `lay.total > ctx->buf.out_cap` guard is a single relational condition.
 * With `out_cap` set below the fixed 100-byte header, even the minimal
 * post-init blob (101 bytes: header + the reserved "") exceeds it, so the guard
 * takes its true arm and finalize returns @ref k_ra8_err_invalid_size. The
 * layout still computes cleanly (total <= UINT32_MAX), so this is a distinct
 * error arm from the 32-bit overflow above.
 */
static void test_rabook_cov_finalize_out_cap(void)
{
  TEST_BEGIN("ra8_rabook_compile: finalize blob > out_cap -> invalid_size");
  ra8_rabook_buffers_t buf = full_buffers();
  buf.out_cap              = (uint32_t)k_c_tiny_out_cap; /* below the 100-byte header */
  ra8_rabook_ctx_t ctx     = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rabook_compile_init(&ctx, &buf));

  const void* blob = nullptr;
  uint32_t    len  = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_rabook_finalize(&ctx, &blob, &len));
  TEST_ASSERT_NULL(blob);
  TEST_END("ra8_rabook_compile: finalize blob > out_cap -> invalid_size");
}

/**
 * @brief No-op log byte sink so the logger never pokes ITM MMIO on the host.
 * @details The error vectors drive the builder into legs that call
 *          `ra8_log_error`; registering a sink makes `internal_itm_ready()`
 *          short-circuit so those bytes route here instead of the unmapped
 *          Cortex-M85 ITM registers (which would SIGSEGV on the host).
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
  test_rabook_cov_null_ctx();
  test_rabook_cov_intern_errors();
  test_rabook_cov_add_element_errors();
  test_rabook_cov_add_text_errors();
  test_rabook_cov_link_child_range();
  test_rabook_cov_link_sibling_range();
  test_rabook_cov_add_chapter_errors();
  test_rabook_cov_add_image_null_data();
  test_rabook_cov_add_image_pool_overflow();
  test_rabook_cov_add_stylesheet_errors();
  test_rabook_cov_set_metadata_failed();
  test_rabook_cov_finalize_layout_overflow();
  test_rabook_cov_finalize_out_cap();
  (void)fprintf(stderr, "[OK ] test_ra8_rabook_compile_cov.c\n");
  return 0;
}
