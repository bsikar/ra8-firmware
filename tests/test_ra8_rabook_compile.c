/**
 * @file test_ra8_rabook_compile.c
 * @brief Resident-arena and error-arm tests for the RABOOK1 builder.
 *
 * @details
 * Drives the zero-heap @ref ra8_rabook_compile builder over its happy path and
 * its resident-arena failure arms:
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
 * External image pools and callback finalization are exercised by the sibling
 * stream test, while XHTML parsing and gray4 transcoding keep their own suites.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_book.h"
#include "ra8_err.h"
#include "ra8_log.h"
#include "ra8_rabook_compile.h"
#include "support/rabook_compile_test_fixture.h"
#include "unity_minimal.h"

/** @brief Caller-owned arenas shared by the resident tests in this executable. */
static ra8_test_rabook_fixture_t s_fixture;

/**
 * @test internal_test_rabook_compile_roundtrip
 * @brief Build a blob, validate it, and read every field back identical.
 *
 * @par MC/DC:
 * (no compound decisions in this test -- it exercises the builder happy path end
 * to end: intern -> add_image -> add_stylesheet -> add_element/text ->
 * link_child -> set_metadata -> add_chapter -> finalize -> ra8_book_validate,
 * then reads every field back through the accessors and confirms interning
 * de-dups. Each builder guard on this success path is a single-condition
 * bounds/relational check; the arena-overflow decisions have their own error-arm
 * tests in this file) @details Executes the rabook compile roundtrip scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL
static void internal_test_rabook_compile_roundtrip(void)
{
  TEST_BEGIN("ra8_rabook_compile: build -> validate -> read back");

  ra8_rabook_ctx_t ctx = {};
  ra8_test_rabook_init(&s_fixture, &ctx);

  ra8_test_rabook_roundtrip_t rt = {};
  ra8_test_rabook_populate(&s_fixture, &ctx, &rt, false);

  const void* blob     = nullptr;
  uint32_t    blob_len = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rabook_finalize(&ctx, &blob, &blob_len));
  TEST_ASSERT_NOT_NULL(blob);
  rt.blob     = blob;
  rt.blob_len = blob_len;

  /* The blob the emitter produced must satisfy the on-device reader. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_book_validate(blob, (size_t)blob_len));
  ra8_test_rabook_verify(&rt);

  /* String interning de-dups: re-interning "body" returns the same offset. */
  TEST_ASSERT_EQ(rt.body_name, ra8_rabook_intern(&ctx, "body"));

  TEST_END("ra8_rabook_compile: build -> validate -> read back");
}

/**
 * @test internal_test_rabook_overflow_nodes
 * @brief A node-arena overflow latches ctx->failed so finalize returns no_mem.
 *
 * @par MC/DC:
 * No compound decision under test: the overflow guard
 * `ctx->node_count >= ctx->buf.node_cap` is a single relational condition.
 * Vectors: first add (0 >= 1 -> false, succeeds at index 0); second add
 * (1 >= 1 -> true, latches `failed` and returns nil). finalize then takes its
 * single `ctx->failed` arm and reports @ref k_ra8_err_no_mem. @details Executes the rabook overflow nodes scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL
static void internal_test_rabook_overflow_nodes(void)
{
  TEST_BEGIN("ra8_rabook_compile: node-arena overflow -> no_mem");
  ra8_rabook_buffers_t buf = ra8_test_rabook_buffers(&s_fixture);
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
 * @test internal_test_rabook_overflow_attrs
 * @brief An attr-arena overflow latches ctx->failed so finalize returns no_mem.
 *
 * @par MC/DC:
 * The guard `(uint32_t)attr_count > (ctx->buf.attr_cap - ctx->attr_count)` is a
 * single relational condition; this vector takes its true arm (2 > 1) on the
 * first add, latching `failed`. finalize then reports @ref k_ra8_err_no_mem. @details Executes the rabook overflow attrs scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL
static void internal_test_rabook_overflow_attrs(void)
{
  TEST_BEGIN("ra8_rabook_compile: attr-arena overflow -> no_mem");
  ra8_rabook_buffers_t buf = ra8_test_rabook_buffers(&s_fixture);
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
 * @test internal_test_rabook_overflow_strings
 * @brief A string-pool overflow latches ctx->failed so finalize returns no_mem.
 *
 * @par MC/DC:
 * The guard `need > (ctx->buf.string_cap - ctx->string_size)` is a single
 * relational condition. With string_cap = 4 the init-reserved "" leaves only 3
 * free bytes, so interning a 10-char string (need = 11) takes the true arm and
 * latches `failed`; finalize then reports @ref k_ra8_err_no_mem. @details Executes the rabook overflow strings scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL
static void internal_test_rabook_overflow_strings(void)
{
  TEST_BEGIN("ra8_rabook_compile: string-pool overflow -> no_mem");
  ra8_rabook_buffers_t buf = ra8_test_rabook_buffers(&s_fixture);
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
 * @test internal_test_rabook_overflow_images
 * @brief An image-descriptor overflow latches ctx->failed -> finalize no_mem.
 *
 * @par MC/DC:
 * The guard `ctx->image_count >= ctx->buf.image_cap` is a single relational
 * condition. Vectors: first add (0 >= 1 -> false, image 0); second add
 * (1 >= 1 -> true, latches `failed`, returns nil). finalize reports no_mem. @details Executes the rabook overflow images scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL
static void internal_test_rabook_overflow_images(void)
{
  TEST_BEGIN("ra8_rabook_compile: image-arena overflow -> no_mem");
  ra8_rabook_buffers_t buf = ra8_test_rabook_buffers(&s_fixture);
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
 * @test internal_test_rabook_crc_empty_body
 * @brief A degenerate empty builder finalizes a header-only blob, exercising
 *        the len==0 arm of the body CRC (which returns 0).
 *
 * @par MC/DC:
 * No compound decision under test. This white-box case bypasses
 * @ref ra8_rabook_compile_init (which would reserve "" in the string pool) so the
 * computed body length is 0 and rabook_crc32()'s `for (i < len)` loop never
 * runs -- the documented empty-range path. The resulting CRC is the seed XORed
 * with itself, i.e. 0; the blob still passes @ref ra8_book_validate. @details Executes the rabook crc empty body scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL
static void internal_test_rabook_crc_empty_body(void)
{
  TEST_BEGIN("ra8_rabook_compile: empty-body CRC (len==0) -> 0");
  ra8_rabook_buffers_t buf = ra8_test_rabook_buffers(&s_fixture);
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
 * @test internal_test_rabook_add_image_zero_data
 * @brief add_image with data_size==0 records a descriptor with no pool bytes.
 *
 * @par MC/DC:
 * The two `if (data_size != 0U)` guards inside add_image are single conditions;
 * this vector takes their false arms (no NULL-data check, no pool memcpy). The
 * blob is finalized and read back to confirm a zero-length image descriptor. @details Executes the rabook add image zero data scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL
static void internal_test_rabook_add_image_zero_data(void)
{
  TEST_BEGIN("ra8_rabook_compile: add_image data_size==0");
  ra8_rabook_buffers_t buf = ra8_test_rabook_buffers(&s_fixture);
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
 * @test internal_test_rabook_offset_zero
 * @brief intern("") yields offset 0 and a real string never lands at offset 0.
 *
 * @par MC/DC:
 * No compound decision under test. Confirms the offset-0 == "" contract: init
 * reserves "" at offset 0; re-interning "" de-dups back to 0; the first real
 * string is appended after the reserved NUL, so its offset is non-zero. @details Executes the rabook offset zero scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since Version 0.1.0 */
RA8_INTERNAL
static void internal_test_rabook_offset_zero(void)
{
  TEST_BEGIN("ra8_rabook_compile: offset-0 == \"\"");
  ra8_rabook_buffers_t buf = ra8_test_rabook_buffers(&s_fixture);
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
 * @note Not thread-safe (host single-thread test driver). @since Version 0.1.0 */
RA8_INTERNAL static void internal_log_sink(void* ctx, uint8_t byte)
{
  (void)ctx;
  (void)byte;
}

int main(void)
{
  ra8_log_set_byte_sink(internal_log_sink, nullptr);
  internal_test_rabook_compile_roundtrip();
  internal_test_rabook_overflow_nodes();
  internal_test_rabook_overflow_attrs();
  internal_test_rabook_overflow_strings();
  internal_test_rabook_overflow_images();
  internal_test_rabook_crc_empty_body();
  internal_test_rabook_add_image_zero_data();
  internal_test_rabook_offset_zero();
  return 0;
}
