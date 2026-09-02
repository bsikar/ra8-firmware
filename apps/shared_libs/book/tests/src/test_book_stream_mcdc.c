/**
 * @file test_book_stream_mcdc.c
 * @brief Per-operand MC/DC vectors for the strict RABOOK1 stream validators.
 *
 * @details
 * Isolates one condition at a time in every compound decision the strict flat
 * and chunked validators own: the exact-read and string-envelope guards, the
 * metadata, stylesheet and node-kind rules, the header, chapter and
 * forward-link operands, the raster, SVG and pool-tiling operands, and both
 * public argument guards. Vectors that no validated public layout can request
 * call the documented private validator seams directly; the shared fixture in
 * book_stream_fixture.c keeps each of them deterministic and allocation-free.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "book_chunked.h"
#include "book_stream.h"
#include "book_stream_fixture_internal.h"
#include "book_stream_internal.h"
#include "ra8_attributes.h"
#include "unity_minimal.h"

/**
 * @test internal_test_mcdc_stream_read_and_envelope
 * @brief Isolate exact-read and string-envelope compound guards.
 * @par MC/DC:
 * `apps/shared_libs/book/src/book_stream_wire.c@priv_book_stream_read` receives the
 * valid end/zero control, then offset-past-end and length-past-remaining
 * vectors that independently determine its two-condition OR.
 * `apps/shared_libs/book/src/book_stream_wire.c@priv_book_stream_validate_string_envelope`
 * receives canonical, first-byte, last-byte, and same-corruption/read-fault
 * vectors. They independently determine read success and both sentinel tests.
 * @details Calls the documented private seam because validated public layouts
 * cannot request an out-of-range internal span.
 * @pre The fixture source is mutable and restored before each vector.
 * @pre The validation context borrows live fixture storage.
 * @post Every compound condition has an independently determining vector.
 * @post The final fixture is canonical.
 * @note Test-only; no public book ABI changes.
 * @since Version 0.1.0
 */
RA8_INTERNAL static void internal_test_mcdc_stream_read_and_envelope(void)
{
  TEST_BEGIN("book stream read and envelope MC/DC");
  priv_book_fixture_setup();
  stream_mem_t      mem  = {};
  stream_validate_t ctx  = priv_book_fixture_context(&mem);
  uint8_t           byte = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, priv_book_stream_read(&ctx, ctx.source_size, &byte, 0U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 priv_book_stream_read(&ctx, ctx.source_size + 1U, &byte, 0U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, priv_book_stream_read(&ctx, ctx.source_size, &byte, 1U));
  TEST_ASSERT_EQ(k_ra8_ok, priv_book_stream_validate_string_envelope(&ctx));
  g_book.strings[0] = 1;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, priv_book_stream_validate_string_envelope(&ctx));
  mem.calls     = 0U;
  mem.fail_call = 1U;
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, priv_book_stream_validate_string_envelope(&ctx));
  priv_book_fixture_setup();
  ctx                                         = priv_book_fixture_context(&mem);
  g_book.strings[sizeof(g_book.strings) - 1U] = 1;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, priv_book_stream_validate_string_envelope(&ctx));
  priv_book_fixture_setup();
  TEST_END("book stream read and envelope MC/DC");
}

/**
 * @test internal_test_mcdc_stream_metadata_and_styles
 * @brief Isolate optional cover and stylesheet-scope guards.
 * @par MC/DC:
 * `apps/shared_libs/book/src/book_stream.c@priv_book_stream_validate_metadata`
 * receives nil, valid, and image-count cover values for its two-condition AND.
 * `apps/shared_libs/book/src/book_stream.c@priv_book_stream_validate_styles`
 * receives nil, valid, out-of-range, and invalid-source/out-of-range vectors
 * for read success, non-nil scope, and the chapter bound.
 * @details Rebuilds the canonical flat fixture between independent mutations.
 * @pre Fixture records use canonical host/wire little-endian layout.
 * @pre The direct context borrows the current fixture.
 * @post All metadata and stylesheet conditions independently determine.
 * @post The final fixture is canonical.
 * @note Test-only direct private-seam coverage.
 * @since Version 0.1.0
 */
RA8_INTERNAL static void internal_test_mcdc_stream_metadata_and_styles(void)
{
  TEST_BEGIN("book stream metadata and styles MC/DC");
  priv_book_fixture_setup();
  stream_mem_t      mem = {};
  stream_validate_t ctx = priv_book_fixture_context(&mem);
  TEST_ASSERT_EQ(k_ra8_ok, priv_book_stream_validate_metadata(&ctx));
  ctx.hdr.cover_image_index = (uint32_t)k_book_nil;
  TEST_ASSERT_EQ(k_ra8_ok, priv_book_stream_validate_metadata(&ctx));
  ctx.hdr.cover_image_index = ctx.hdr.image_count;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, priv_book_stream_validate_metadata(&ctx));
  ctx.hdr = g_book.hdr;
  TEST_ASSERT_EQ(k_ra8_ok, priv_book_stream_validate_styles(&ctx));
  g_book.styles[0].scope_chapter = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, priv_book_stream_validate_styles(&ctx));
  g_book.styles[0].scope_chapter = ctx.hdr.chapter_count;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, priv_book_stream_validate_styles(&ctx));
  g_book.styles[0].source_off = ctx.hdr.string_size;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, priv_book_stream_validate_styles(&ctx));
  priv_book_fixture_setup();
  TEST_END("book stream metadata and styles MC/DC");
}

/**
 * @test internal_test_mcdc_stream_node_kinds
 * @brief Isolate element attribute-span and text-field guards.
 * @par MC/DC:
 * `apps/shared_libs/book/src/book_stream.c@priv_book_stream_validate_element`
 * receives canonical, wrong-first, and excessive-count vectors for its OR.
 * `apps/shared_libs/book/src/book_stream.c@priv_book_stream_validate_text`
 * receives canonical plus one mutation for each of its four OR operands.
 * @details Copies canonical records before each one-field mutation.
 * @pre The string pool and referenced names are canonical.
 * @pre Host layout matches the asserted RABOOK1 wire record layout.
 * @post Every element and text operand independently determines rejection.
 * @post The source fixture remains usable by later tests.
 * @note Test-only direct private-seam coverage.
 * @since Version 0.1.0
 */
RA8_INTERNAL static void internal_test_mcdc_stream_node_kinds(void)
{
  TEST_BEGIN("book stream node-kind MC/DC");
  priv_book_fixture_setup();
  stream_mem_t      mem    = {};
  stream_validate_t ctx    = priv_book_fixture_context(&mem);
  uint32_t          cursor = 0U;
  TEST_ASSERT_EQ(
    k_ra8_ok,
    priv_book_stream_validate_element(&ctx, (const uint8_t*)&g_book.nodes[0], &cursor));
  book_node_t element = g_book.nodes[0];
  element.first_attr  = 1U;
  cursor              = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 priv_book_stream_validate_element(&ctx, (const uint8_t*)&element, &cursor));
  element            = g_book.nodes[0];
  element.attr_count = 2U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 priv_book_stream_validate_element(&ctx, (const uint8_t*)&element, &cursor));
  const book_node_t canonical = g_book.nodes[1];
  TEST_ASSERT_EQ(k_ra8_ok, priv_book_stream_validate_text(&ctx, (const uint8_t*)&canonical));
  book_node_t text = canonical;
  text.attr_count  = 1U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 priv_book_stream_validate_text(&ctx, (const uint8_t*)&text));
  text          = canonical;
  text.name_off = 1U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 priv_book_stream_validate_text(&ctx, (const uint8_t*)&text));
  text            = canonical;
  text.first_attr = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 priv_book_stream_validate_text(&ctx, (const uint8_t*)&text));
  text             = canonical;
  text.first_child = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 priv_book_stream_validate_text(&ctx, (const uint8_t*)&text));
  TEST_END("book stream node-kind MC/DC");
}

/**
 * @test internal_test_mcdc_stream_header_string_and_dom_operands
 * @brief Isolate the header, string-status, chapter-root, and link operands.
 * @par MC/DC:
 * An unsupported format version varies the first operand of the header
 * feature guard against the canonical control. A shortened image pool leaves
 * every segment canonical but the walked cursor short of total_size, varying
 * the cursor operand of the exact-length gate. A read fault on the first
 * string byte varies the status operand of both the boundary and the
 * non-empty string checks. An out-of-pool chapter title varies the status
 * operand of the chapter root-bound gate. A text root, and an element root
 * carrying a sibling, independently vary the two operands of the root-shape
 * gate. A child index equal to the node count varies the upper-bound operand
 * of the forward-link rule against the already-covered lower bound.
 * Decisions:
 * - apps/shared_libs/book/src/book_stream_wire.c@internal_validate_header_fields
 * - apps/shared_libs/book/src/book_stream_wire.c@internal_validate_header_layout
 * - apps/shared_libs/book/src/book_stream_wire.c@priv_book_stream_string_ref
 * - apps/shared_libs/book/src/book_stream_wire.c@priv_book_stream_nonempty_string_ref
 * - apps/shared_libs/book/src/book_stream.c@internal_validate_chapters
 * - apps/shared_libs/book/src/book_stream.c@internal_forward_link
 */
RA8_INTERNAL static void internal_test_mcdc_stream_header_string_and_dom_operands(void)
{
  TEST_BEGIN("book stream header, string and DOM operand MC/DC");
  priv_book_fixture_setup();
  g_book.hdr.format_version = (uint32_t)k_book_format_version + 1U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, priv_book_fixture_validate());

  priv_book_fixture_setup();
  g_book.hdr.image_pool_size = (uint32_t)sizeof(g_book.pool) - 1U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, priv_book_fixture_validate());

  priv_book_fixture_setup();
  stream_mem_t      mem    = {};
  stream_validate_t ctx    = priv_book_fixture_context(&mem);
  uint32_t          cursor = 0U;
  mem.calls                = 0U;
  mem.fail_call            = 1U;
  TEST_ASSERT_EQ(
    k_ra8_err_hw_timeout,
    priv_book_stream_validate_element(&ctx, (const uint8_t*)&g_book.nodes[0], &cursor));

  priv_book_fixture_setup();
  g_book.chapters[0].title_off = g_book.hdr.string_size;
  priv_book_fixture_refresh_crc();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, priv_book_fixture_validate());

  priv_book_fixture_setup();
  g_book.chapters[0].root_node = 1U;
  priv_book_fixture_refresh_crc();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, priv_book_fixture_validate());

  priv_book_fixture_setup();
  g_book.nodes[0].next_sibling = 1U;
  priv_book_fixture_refresh_crc();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, priv_book_fixture_validate());

  priv_book_fixture_setup();
  g_book.nodes[0].first_child = g_book.hdr.node_count;
  priv_book_fixture_refresh_crc();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, priv_book_fixture_validate());

  priv_book_fixture_setup();
  TEST_END("book stream header, string and DOM operand MC/DC");
}

/**
 * @test internal_test_mcdc_stream_raster_operands
 * @brief Isolate every raster-shape, raster-size, and reserved-field operand.
 * @par MC/DC:
 * Zero width, zero height, an unknown depth, and the eight-bit depth
 * independently vary the four operands of the raster-shape gate: the eight-bit
 * vector is the one that leaves the not-gray4 operand true while the not-gray8
 * operand is false, and it validates end to end. A raw length that disagrees
 * with the computed extent varies the third operand of the raster-size gate. A
 * non-zero reserved field and an empty image id independently vary the two
 * operands of the reserved gate.
 * Decisions:
 * - apps/shared_libs/book/src/book_stream.c@internal_validate_raster
 * - apps/shared_libs/book/src/book_stream.c@internal_validate_images
 */
RA8_INTERNAL static void internal_test_mcdc_stream_raster_operands(void)
{
  TEST_BEGIN("book stream raster operand MC/DC");
  priv_book_fixture_setup();
  g_book.images[0].width = 0U;
  priv_book_fixture_refresh_crc();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, priv_book_fixture_validate());

  priv_book_fixture_setup();
  g_book.images[0].height = 0U;
  priv_book_fixture_refresh_crc();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, priv_book_fixture_validate());

  priv_book_fixture_setup();
  g_book.images[0].pixel_format = UINT8_MAX;
  priv_book_fixture_refresh_crc();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, priv_book_fixture_validate());

  priv_book_fixture_setup();
  g_book.images[0].width        = 2U;
  g_book.images[0].height       = 1U;
  g_book.images[0].pixel_format = (uint8_t)k_book_pixfmt_gray8;
  priv_book_fixture_refresh_crc();
  TEST_ASSERT_EQ(k_ra8_ok, priv_book_fixture_validate());

  priv_book_fixture_setup();
  g_book.images[0].raw_size = g_book.images[0].data_size + 1U;
  priv_book_fixture_refresh_crc();
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, priv_book_fixture_validate());

  priv_book_fixture_setup();
  g_book.images[0].reserved2 = 1U;
  priv_book_fixture_refresh_crc();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, priv_book_fixture_validate());

  priv_book_fixture_setup();
  g_book.images[0].id_off = 0U;
  priv_book_fixture_refresh_crc();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, priv_book_fixture_validate());

  priv_book_fixture_setup();
  TEST_END("book stream raster operand MC/DC");
}

/**
 * @test internal_test_mcdc_stream_svg_and_tiling_operands
 * @brief Isolate every SVG sentinel, SVG size, and pool-tiling operand.
 * @par MC/DC:
 * A non-zero SVG width, height, and depth independently vary the three
 * operands of the SVG sentinel gate; an SVG whose raw length differs from its
 * stored length varies the second operand of its size gate; and an oversized
 * SVG payload sitting at the canonical pool offset varies the remaining-bytes
 * operand of the tiling gate while its offset operand stays false.
 * Decisions:
 * - apps/shared_libs/book/src/book_stream.c@internal_validate_svg
 * - apps/shared_libs/book/src/book_stream.c@internal_validate_images
 */
RA8_INTERNAL static void internal_test_mcdc_stream_svg_and_tiling_operands(void)
{
  TEST_BEGIN("book stream SVG and tiling operand MC/DC");
  priv_book_fixture_setup();
  g_book.images[1].width = 1U;
  priv_book_fixture_refresh_crc();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, priv_book_fixture_validate());

  priv_book_fixture_setup();
  g_book.images[1].height = 1U;
  priv_book_fixture_refresh_crc();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, priv_book_fixture_validate());

  priv_book_fixture_setup();
  g_book.images[1].pixel_format = (uint8_t)k_book_pixfmt_gray8;
  priv_book_fixture_refresh_crc();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, priv_book_fixture_validate());

  priv_book_fixture_setup();
  g_book.images[1].raw_size = g_book.images[1].data_size + 1U;
  priv_book_fixture_refresh_crc();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, priv_book_fixture_validate());

  priv_book_fixture_setup();
  g_book.images[1].data_size = g_book.hdr.image_pool_size;
  g_book.images[1].raw_size  = g_book.hdr.image_pool_size;
  priv_book_fixture_refresh_crc();
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, priv_book_fixture_validate());

  priv_book_fixture_setup();
  TEST_END("book stream SVG and tiling operand MC/DC");
}

/**
 * @test internal_test_mcdc_stream_flat_guards
 * @brief Vary each public flat-validator argument guard operand alone.
 * @par MC/DC:
 * A null scratch varies the second operand of the pointer guard against the
 * already-covered null callback, and a source shorter than the fixed header
 * varies the first operand of the size guard against the already-covered zero
 * scratch capacity. Both rejected vectors leave the pre-seeded output header
 * cleared.
 * Decisions: apps/shared_libs/book/src/book_stream.c@book_validate_stream_strict
 */
RA8_INTERNAL static void internal_test_mcdc_stream_flat_guards(void)
{
  TEST_BEGIN("book stream flat guard MC/DC");
  priv_book_fixture_setup();
  stream_mem_t  mem = {.data = (uint8_t*)&g_book, .len = priv_book_fixture_flat_len()};
  book_header_t hdr = {.total_size = UINT32_MAX};
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    book_validate_stream_strict(priv_book_fixture_read, &mem, mem.len, nullptr, 1U, &hdr));
  TEST_ASSERT_EQ(0U, hdr.total_size);
  hdr.total_size = UINT32_MAX;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 book_validate_stream_strict(priv_book_fixture_read,
                                             &mem,
                                             (uint64_t)k_book_sizeof_header - 1U,
                                             g_validate_work,
                                             sizeof(g_validate_work),
                                             &hdr));
  TEST_ASSERT_EQ(0U, hdr.total_size);
  TEST_END("book stream flat guard MC/DC");
}

/**
 * @test internal_test_mcdc_chunked_argument_guards
 * @brief Vary each chunked-validator argument guard operand alone.
 * @par MC/DC:
 * A null transfer buffer and a null scratch independently vary the two
 * operands of the chunked pointer guard, and a zero transfer capacity and a
 * zero scratch capacity independently vary the two operands of its size guard,
 * each against the fully valid control the round-trip test supplies. Every
 * rejected vector leaves the pre-seeded output header cleared.
 * Decisions:
 * apps/shared_libs/book/src/book_chunked_validate.c@book_chunked_validate_strict
 */
RA8_INTERNAL static void internal_test_mcdc_chunked_argument_guards(void)
{
  TEST_BEGIN("book stream chunked guard MC/DC");
  stream_guard_fixture_t fixture = {};
  priv_book_fixture_open_packed(&fixture);
  book_header_t hdr = {.total_size = UINT32_MAX};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 book_chunked_validate_strict(&fixture.reader,
                                              nullptr,
                                              sizeof(g_reader_chunk),
                                              g_validate_work,
                                              sizeof(g_validate_work),
                                              &hdr));
  TEST_ASSERT_EQ(0U, hdr.total_size);
  hdr.total_size = UINT32_MAX;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 book_chunked_validate_strict(&fixture.reader,
                                              g_reader_chunk,
                                              sizeof(g_reader_chunk),
                                              nullptr,
                                              sizeof(g_validate_work),
                                              &hdr));
  TEST_ASSERT_EQ(0U, hdr.total_size);
  hdr.total_size = UINT32_MAX;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 book_chunked_validate_strict(&fixture.reader,
                                              g_reader_chunk,
                                              0U,
                                              g_validate_work,
                                              sizeof(g_validate_work),
                                              &hdr));
  TEST_ASSERT_EQ(0U, hdr.total_size);
  hdr.total_size = UINT32_MAX;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 book_chunked_validate_strict(&fixture.reader,
                                              g_reader_chunk,
                                              sizeof(g_reader_chunk),
                                              g_validate_work,
                                              0U,
                                              &hdr));
  TEST_ASSERT_EQ(0U, hdr.total_size);
  priv_book_fixture_setup();
  TEST_END("book stream chunked guard MC/DC");
}

int main(void)
{
  internal_test_mcdc_stream_read_and_envelope();
  internal_test_mcdc_stream_metadata_and_styles();
  internal_test_mcdc_stream_node_kinds();
  internal_test_mcdc_stream_header_string_and_dom_operands();
  internal_test_mcdc_stream_raster_operands();
  internal_test_mcdc_stream_svg_and_tiling_operands();
  internal_test_mcdc_stream_flat_guards();
  internal_test_mcdc_chunked_argument_guards();
  return 0;
}
