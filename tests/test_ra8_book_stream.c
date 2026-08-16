/**
 * @file test_ra8_book_stream.c
 * @brief Strict streamed RABOOK1 and production RBKC reader validation tests.
 *
 * @details
 * Exercises canonical flat and compressed round trips plus malformed headers,
 * layouts, DOM ownership, image extents, CRCs, callback failures, aliased
 * workspaces, and partial reader state, then isolates one condition at a time
 * in every compound decision the strict validators own. The fixed
 * memory-backed fixture in book_stream_fixture.c keeps every validation and
 * decompression boundary deterministic and allocation-free.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "book_stream_fixture_internal.h"
#include "ra8_attributes.h"
#include "ra8_book_chunked.h"
#include "ra8_book_stream.h"
#include "ra8_book_stream_internal.h"
#include "unity_minimal.h"

/**
 * @test internal_test_mcdc_stream_read_and_envelope
 * @brief Isolate exact-read and string-envelope compound guards.
 * @par MC/DC:
 * `libs/ra8_book/src/ra8_book_stream.c@priv_book_stream_read` receives the
 * valid end/zero control, then offset-past-end and length-past-remaining
 * vectors that independently determine its two-condition OR.
 * `libs/ra8_book/src/ra8_book_stream.c@priv_book_stream_validate_string_envelope`
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
 * `libs/ra8_book/src/ra8_book_stream.c@priv_book_stream_validate_metadata`
 * receives nil, valid, and image-count cover values for its two-condition AND.
 * `libs/ra8_book/src/ra8_book_stream.c@priv_book_stream_validate_styles`
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
  ctx.hdr.cover_image_index = (uint32_t)k_ra8_book_nil;
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
 * `libs/ra8_book/src/ra8_book_stream.c@priv_book_stream_validate_element`
 * receives canonical, wrong-first, and excessive-count vectors for its OR.
 * `libs/ra8_book/src/ra8_book_stream.c@priv_book_stream_validate_text`
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
  ra8_book_node_t element = g_book.nodes[0];
  element.first_attr      = 1U;
  cursor                  = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 priv_book_stream_validate_element(&ctx, (const uint8_t*)&element, &cursor));
  element            = g_book.nodes[0];
  element.attr_count = 2U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 priv_book_stream_validate_element(&ctx, (const uint8_t*)&element, &cursor));
  const ra8_book_node_t canonical = g_book.nodes[1];
  TEST_ASSERT_EQ(k_ra8_ok, priv_book_stream_validate_text(&ctx, (const uint8_t*)&canonical));
  ra8_book_node_t text = canonical;
  text.attr_count      = 1U;
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
 * @test A canonical flat fixture is decoded, validated, and published.
 * @par MC/DC:
 * The public guard `(read == nullptr) || (scratch == nullptr)` executes
 * V1 read=valid, scratch=valid -> false and
 * V2 read=null, scratch=valid -> true; V1/V2 vary only `read`.
 * The size guard
 * `(source_size < k_ra8_book_sizeof_header) || (scratch_cap == 0U)` executes
 * V3 canonical source size, scratch_cap=17 -> false and
 * V4 the same source size, scratch_cap=0 -> true; V3/V4 vary only `scratch_cap`.
 * The happy vector also proves the decoded header is published and the
 * renderer consumes the validated DOM; each rejected vector leaves the
 * pre-seeded output header cleared.
 * Decisions: libs/ra8_book/src/ra8_book_stream.c@ra8_book_validate_stream_strict
 * Decisions: libs/ra8_book/src/ra8_book_xhtml.c@ra8_book_chapter_to_xhtml
 */
RA8_INTERNAL static void internal_test_flat_happy_and_args(void)
{
  TEST_BEGIN("strict stream flat happy and arguments");
  priv_book_fixture_setup();
  TEST_ASSERT_EQ(k_ra8_ok, priv_book_fixture_validate());
  char   xhtml[64] = {};
  size_t xhtml_len = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_book_chapter_to_xhtml(&g_book, 0U, xhtml, sizeof(xhtml) - 1U, &xhtml_len));
  TEST_ASSERT_EQ(strlen("<body class=\"page\">hello</body>"), xhtml_len);
  TEST_ASSERT_EQ(0, strcmp(xhtml, "<body class=\"page\">hello</body>"));
  stream_mem_t      mem = {.data = (uint8_t*)&g_book, .len = priv_book_fixture_flat_len()};
  ra8_book_header_t hdr = {.total_size = UINT32_MAX};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_book_validate_stream_strict(NULL,
                                                 &mem,
                                                 mem.len,
                                                 g_validate_work,
                                                 sizeof(g_validate_work),
                                                 &hdr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_book_validate_stream_strict(priv_book_fixture_read,
                                                 &mem,
                                                 mem.len,
                                                 g_validate_work,
                                                 0U,
                                                 &hdr));
  TEST_ASSERT_EQ(0U, hdr.total_size);
  TEST_END("strict stream flat happy and arguments");
}

/**
 * @test Header, layout, string, index, and CRC corruption is rejected.
 * @par MC/DC:
 * For `(format_version != expected) || (unknown_flags != 0U)`, the canonical
 * header is V1 F,F -> false and the flags-only corruption is V2 F,T -> true,
 * independently varying `unknown_flags`. For
 * `(err == k_ra8_ok) && (preceding != 0U)`, the canonical title reference is
 * V3 T,F -> false and title_off+1 is V4 T,T -> true, independently varying
 * the string-boundary byte. For
 * `(err == k_ra8_ok) && (root >= node_count)`, the canonical chapter is
 * V5 T,F -> false and root=node_count is V6 T,T -> true, independently
 * varying the root bound. Separate vectors reject bad magic, a noncanonical
 * chapter offset, and a body CRC mismatch at their single decisions.
 * Decisions: libs/ra8_book/src/ra8_book_stream.c@internal_validate_header_layout
 * Decisions: libs/ra8_book/src/ra8_book_stream.c@internal_string_ref
 * Decisions: libs/ra8_book/src/ra8_book_stream.c@internal_validate_chapters
 * Decisions: libs/ra8_book/src/ra8_book_stream.c@internal_validate_crc
 */
RA8_INTERNAL static void internal_test_header_table_string_crc_corruption(void)
{
  TEST_BEGIN("strict stream header table string and CRC corruption");
  priv_book_fixture_setup();
  g_book.hdr.magic[0] = 'X';
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, priv_book_fixture_validate());
  priv_book_fixture_setup();
  g_book.hdr.flags = 0x80000000U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, priv_book_fixture_validate());
  priv_book_fixture_setup();
  g_book.hdr.chapter_off++;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, priv_book_fixture_validate());
  priv_book_fixture_setup();
  g_book.hdr.title_off++;
  priv_book_fixture_refresh_crc();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, priv_book_fixture_validate());
  priv_book_fixture_setup();
  g_book.chapters[0].root_node = g_book.hdr.node_count;
  priv_book_fixture_refresh_crc();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, priv_book_fixture_validate());
  priv_book_fixture_setup();
  g_book.hdr.crc32 ^= UINT32_MAX;
  TEST_ASSERT_EQ(k_ra8_err_range_check_failed, priv_book_fixture_validate());
  TEST_END("strict stream header table string and CRC corruption");
}

/**
 * @test Cycles, duplicate ownership, orphan nodes, and empty names fail.
 * @par MC/DC:
 * The forward-link decision `(link <= current) || (link >= count)` executes
 * V1 child=1,current=0,count=2 -> F,F/false and
 * V2 sibling=0,current=1,count=2 -> T,-/true; V1/V2 independently vary the
 * non-forward lower-bound condition. The nonempty-name decision
 * `(err == k_ra8_ok) && (first == 0U)` executes V3 T,F/false for each canonical
 * element/attribute name and V4 T,T/true when its offset is zero, independently
 * varying the first byte. The other vectors independently reach the duplicate
 * owner and orphan-node decisions after rebuilding a canonical fixture, so
 * no earlier corruption masks them.
 * Decisions: libs/ra8_book/src/ra8_book_stream.c@internal_forward_link
 * Decisions: libs/ra8_book/src/ra8_book_stream.c@internal_nonempty_string_ref
 * Decisions: libs/ra8_book/src/ra8_book_stream.c@internal_mark_forward_link
 * Decisions: libs/ra8_book/src/ra8_book_stream.c@internal_validate_nodes
 */
RA8_INTERNAL static void internal_test_dom_ownership_and_renderer_safety(void)
{
  TEST_BEGIN("strict stream DOM ownership and renderer safety");
  priv_book_fixture_setup();
  g_book.nodes[1].next_sibling = 0U;
  priv_book_fixture_refresh_crc();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, priv_book_fixture_validate());
  priv_book_fixture_setup();
  g_book.nodes[1].kind         = (uint8_t)k_ra8_book_node_element;
  g_book.nodes[1].name_off     = g_book.nodes[0].name_off;
  g_book.nodes[1].text_off     = 0U;
  g_book.chapters[0].root_node = 1U;
  priv_book_fixture_refresh_crc();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, priv_book_fixture_validate());
  priv_book_fixture_setup();
  g_book.nodes[0].first_child = (uint32_t)k_ra8_book_nil;
  priv_book_fixture_refresh_crc();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, priv_book_fixture_validate());
  priv_book_fixture_setup();
  g_book.nodes[0].name_off = 0U;
  priv_book_fixture_refresh_crc();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, priv_book_fixture_validate());
  priv_book_fixture_setup();
  g_book.attrs[0].name_off = 0U;
  priv_book_fixture_refresh_crc();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, priv_book_fixture_validate());
  TEST_END("strict stream DOM ownership and renderer safety");
}

/**
 * @test Image formats, depths, dimensions, extents, and pool tiling fail
 * closed.
 * @par MC/DC:
 * The raster-size decision
 * `(expect > UINT32_MAX) || (data_size != expect) || (raw_size != expect)`
 * executes V1 F,F,F/false and V2 F,T,-/true when data_size is incremented;
 * this independently varies `data_size`. The placement decision
 * `(err == k_ra8_ok) &&`
 * `((data_off != pool_cursor) || (data_size > remaining))` executes canonical
 * V3 T,(F,F)/false and SVG-offset V4 T,(T,-)/true, independently varying
 * `data_off`. The SVG decision
 * `(data_size != 0U) && (data_size == raw_size)` executes V5 T,T/true and
 * zero-size V6 F,-/false, independently varying nonzero size. The unknown
 * format vector reaches the format selector before any size decision.
 * Decisions: libs/ra8_book/src/ra8_book_stream.c@internal_validate_raster
 * Decisions: libs/ra8_book/src/ra8_book_stream.c@internal_validate_svg
 * Decisions: libs/ra8_book/src/ra8_book_stream.c@internal_validate_images
 */
RA8_INTERNAL static void internal_test_image_corruption(void)
{
  TEST_BEGIN("strict stream image corruption");
  priv_book_fixture_setup();
  g_book.images[0].format = UINT8_MAX;
  priv_book_fixture_refresh_crc();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, priv_book_fixture_validate());
  priv_book_fixture_setup();
  g_book.images[0].data_size++;
  priv_book_fixture_refresh_crc();
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, priv_book_fixture_validate());
  priv_book_fixture_setup();
  g_book.images[1].data_off++;
  priv_book_fixture_refresh_crc();
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, priv_book_fixture_validate());
  priv_book_fixture_setup();
  g_book.images[1].data_size = 0U;
  g_book.images[1].raw_size  = 0U;
  g_book.hdr.image_pool_size = 2U;
  g_book.hdr.total_size      = g_book.hdr.image_pool_off + 2U;
  priv_book_fixture_refresh_crc();
  stream_mem_t      mem = {.data = (uint8_t*)&g_book, .len = g_book.hdr.total_size};
  ra8_book_header_t hdr = {};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_book_validate_stream_strict(priv_book_fixture_read,
                                                 &mem,
                                                 mem.len,
                                                 g_validate_work,
                                                 sizeof(g_validate_work),
                                                 &hdr));
  TEST_END("strict stream image corruption");
}

/**
 * @test Production writer -> reader validation checks every compressed chunk.
 * @par MC/DC:
 * The one-chunk-cache decision
 * `ctx->loaded && (ctx->loaded_idx == idx)` executes V1 F,-/false on the first
 * read of chunk zero, V2 T,T/true on a repeated read from that chunk, and
 * V3 T,F/false when strict validation advances to another chunk. V1/V2 prove
 * `loaded` independently decides; V2/V3 prove the index match independently
 * decides. The unmodified packed source is the successful control; flipping
 * one byte in a middle compressed chunk independently changes the inflate/read
 * path to failure and leaves the output header cleared.
 * Decisions: libs/ra8_book/src/ra8_book_chunked_validate.c@internal_load_chunk
 * Decisions: libs/ra8_book/src/ra8_book_chunked_validate.c@internal_chunk_flat_read
 * Decisions: libs/ra8_book/src/ra8_book_chunked_validate.c@ra8_book_chunked_validate_strict
 */
RA8_INTERNAL static void internal_test_production_rbkc_round_trip_and_chunk_corruption(void)
{
  TEST_BEGIN("strict stream production RBKC round trip and chunk corruption");
  stream_guard_fixture_t fixture = {};
  priv_book_fixture_open_packed(&fixture);
  ra8_book_header_t hdr = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_book_chunked_validate_strict(&fixture.reader,
                                                  g_reader_chunk,
                                                  sizeof(g_reader_chunk),
                                                  g_validate_work,
                                                  sizeof(g_validate_work),
                                                  &hdr));
  TEST_ASSERT_EQ(priv_book_fixture_flat_len(), hdr.total_size);
  const uint32_t middle     = fixture.reader.chunk_count / 2U;
  const uint64_t corrupt_at = fixture.reader.payload_off + fixture.reader.table[middle] + 2U;
  fixture.file.data[corrupt_at] ^= 0x5AU;
  TEST_ASSERT(ra8_book_chunked_validate_strict(&fixture.reader,
                                               g_reader_chunk,
                                               sizeof(g_reader_chunk),
                                               g_validate_work,
                                               sizeof(g_validate_work),
                                               &hdr) != k_ra8_ok);
  TEST_ASSERT_EQ(0U, hdr.total_size);
  TEST_END("strict stream production RBKC round trip and chunk corruption");
}

/** @brief Prove every known reader/workspace/output alias fails before I/O. */
RA8_INTERNAL
static void internal_check_chunked_alias_guards(stream_guard_fixture_t* fixture)
{
  ra8_book_header_t hdr = {.total_size = UINT32_MAX};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_book_chunked_validate_strict(&fixture->reader,
                                                  g_reader_chunk,
                                                  sizeof(g_reader_chunk),
                                                  g_reader_chunk,
                                                  sizeof(g_reader_chunk),
                                                  &hdr));
  TEST_ASSERT_EQ(0U, hdr.total_size);
  hdr.total_size = UINT32_MAX;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_book_chunked_validate_strict(&fixture->reader,
                                                  g_reader_staging,
                                                  sizeof(g_reader_staging),
                                                  g_validate_work,
                                                  sizeof(g_validate_work),
                                                  &hdr));
  TEST_ASSERT_EQ(0U, hdr.total_size);
  hdr.total_size = UINT32_MAX;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_book_chunked_validate_strict(&fixture->reader,
                                                  (uint8_t*)fixture->table,
                                                  sizeof(g_reader_chunk),
                                                  g_validate_work,
                                                  sizeof(g_validate_work),
                                                  &hdr));
  TEST_ASSERT_EQ(0U, hdr.total_size);

  const ra8_book_chunked_t saved = fixture->reader;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_book_chunked_validate_strict(&fixture->reader,
                                                  g_reader_chunk,
                                                  sizeof(g_reader_chunk),
                                                  g_validate_work,
                                                  sizeof(g_validate_work),
                                                  (ra8_book_header_t*)(void*)&fixture->reader));
  TEST_ASSERT_EQ(0, memcmp(&saved, &fixture->reader, sizeof(saved)));

  union {
    /** @brief Reader view used as the source object. */
    ra8_book_chunked_t reader;
    /** @brief Aliased transfer destination under test. */
    uint8_t chunk[k_stream_chunk];
  } reader_chunk_alias = {.reader = fixture->reader};
  hdr.total_size       = UINT32_MAX;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_book_chunked_validate_strict(&reader_chunk_alias.reader,
                                                  reader_chunk_alias.chunk,
                                                  sizeof(reader_chunk_alias.chunk),
                                                  g_validate_work,
                                                  sizeof(g_validate_work),
                                                  &hdr));
  TEST_ASSERT_EQ(0U, hdr.total_size);
}

/** @brief Prove fabricated/partial reader geometry fails before table access. */
RA8_INTERNAL
static void internal_check_chunked_reader_guards(const stream_guard_fixture_t* fixture)
{
  ra8_book_header_t  hdr    = {.total_size = UINT32_MAX};
  ra8_book_chunked_t broken = fixture->reader;
  broken.chunk_count++;
  TEST_ASSERT_EQ(k_ra8_err_invalid_state,
                 ra8_book_chunked_validate_strict(&broken,
                                                  g_reader_chunk,
                                                  sizeof(g_reader_chunk),
                                                  g_validate_work,
                                                  sizeof(g_validate_work),
                                                  &hdr));
  TEST_ASSERT_EQ(0U, hdr.total_size);
  broken         = fixture->reader;
  broken.staging = nullptr;
  hdr.total_size = UINT32_MAX;
  TEST_ASSERT_EQ(k_ra8_err_invalid_state,
                 ra8_book_chunked_validate_strict(&broken,
                                                  g_reader_chunk,
                                                  sizeof(g_reader_chunk),
                                                  g_validate_work,
                                                  sizeof(g_validate_work),
                                                  &hdr));
  TEST_ASSERT_EQ(0U, hdr.total_size);

  uint64_t short_table[1]  = {};
  broken                   = fixture->reader;
  broken.table             = short_table;
  broken.table_cap_entries = (uint32_t)(sizeof(short_table) / sizeof(short_table[0]));
  hdr.total_size           = UINT32_MAX;
  TEST_ASSERT_EQ(k_ra8_err_invalid_state,
                 ra8_book_chunked_validate_strict(&broken,
                                                  g_reader_chunk,
                                                  sizeof(g_reader_chunk),
                                                  g_validate_work,
                                                  sizeof(g_validate_work),
                                                  &hdr));
  TEST_ASSERT_EQ(0U, hdr.total_size);
}

/** @test Aliased workspaces and partially initialized readers fail before I/O. */
RA8_INTERNAL
static void internal_test_chunked_workspace_and_reader_guards(void)
{
  TEST_BEGIN("strict chunked workspace and reader guards");
  stream_guard_fixture_t fixture = {};
  priv_book_fixture_open_packed(&fixture);
  internal_check_chunked_alias_guards(&fixture);
  internal_check_chunked_reader_guards(&fixture);
  TEST_END("strict chunked workspace and reader guards");
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
 * - libs/ra8_book/src/ra8_book_stream.c@internal_validate_header_layout
 * - libs/ra8_book/src/ra8_book_stream.c@internal_string_ref
 * - libs/ra8_book/src/ra8_book_stream.c@internal_nonempty_string_ref
 * - libs/ra8_book/src/ra8_book_stream.c@internal_validate_chapters
 * - libs/ra8_book/src/ra8_book_stream.c@internal_forward_link
 */
RA8_INTERNAL static void internal_test_mcdc_stream_header_string_and_dom_operands(void)
{
  TEST_BEGIN("book stream header, string and DOM operand MC/DC");
  priv_book_fixture_setup();
  g_book.hdr.format_version = (uint32_t)k_ra8_book_format_version + 1U;
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
 * - libs/ra8_book/src/ra8_book_stream.c@internal_validate_raster
 * - libs/ra8_book/src/ra8_book_stream.c@internal_validate_images
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
  g_book.images[0].pixel_format = (uint8_t)k_ra8_book_pixfmt_gray8;
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
 * - libs/ra8_book/src/ra8_book_stream.c@internal_validate_svg
 * - libs/ra8_book/src/ra8_book_stream.c@internal_validate_images
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
  g_book.images[1].pixel_format = (uint8_t)k_ra8_book_pixfmt_gray8;
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
 * Decisions: libs/ra8_book/src/ra8_book_stream.c@ra8_book_validate_stream_strict
 */
RA8_INTERNAL static void internal_test_mcdc_stream_flat_guards(void)
{
  TEST_BEGIN("book stream flat guard MC/DC");
  priv_book_fixture_setup();
  stream_mem_t      mem = {.data = (uint8_t*)&g_book, .len = priv_book_fixture_flat_len()};
  ra8_book_header_t hdr = {.total_size = UINT32_MAX};
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    ra8_book_validate_stream_strict(priv_book_fixture_read, &mem, mem.len, nullptr, 1U, &hdr));
  TEST_ASSERT_EQ(0U, hdr.total_size);
  hdr.total_size = UINT32_MAX;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_book_validate_stream_strict(priv_book_fixture_read,
                                                 &mem,
                                                 (uint64_t)k_ra8_book_sizeof_header - 1U,
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
 * libs/ra8_book/src/ra8_book_chunked_validate.c@ra8_book_chunked_validate_strict
 */
RA8_INTERNAL static void internal_test_mcdc_chunked_argument_guards(void)
{
  TEST_BEGIN("book stream chunked guard MC/DC");
  stream_guard_fixture_t fixture = {};
  priv_book_fixture_open_packed(&fixture);
  ra8_book_header_t hdr = {.total_size = UINT32_MAX};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_book_chunked_validate_strict(&fixture.reader,
                                                  nullptr,
                                                  sizeof(g_reader_chunk),
                                                  g_validate_work,
                                                  sizeof(g_validate_work),
                                                  &hdr));
  TEST_ASSERT_EQ(0U, hdr.total_size);
  hdr.total_size = UINT32_MAX;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_book_chunked_validate_strict(&fixture.reader,
                                                  g_reader_chunk,
                                                  sizeof(g_reader_chunk),
                                                  nullptr,
                                                  sizeof(g_validate_work),
                                                  &hdr));
  TEST_ASSERT_EQ(0U, hdr.total_size);
  hdr.total_size = UINT32_MAX;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_book_chunked_validate_strict(&fixture.reader,
                                                  g_reader_chunk,
                                                  0U,
                                                  g_validate_work,
                                                  sizeof(g_validate_work),
                                                  &hdr));
  TEST_ASSERT_EQ(0U, hdr.total_size);
  hdr.total_size = UINT32_MAX;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_book_chunked_validate_strict(&fixture.reader,
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
  internal_test_flat_happy_and_args();
  internal_test_header_table_string_crc_corruption();
  internal_test_dom_ownership_and_renderer_safety();
  internal_test_image_corruption();
  internal_test_production_rbkc_round_trip_and_chunk_corruption();
  internal_test_chunked_workspace_and_reader_guards();
  internal_test_mcdc_stream_header_string_and_dom_operands();
  internal_test_mcdc_stream_raster_operands();
  internal_test_mcdc_stream_svg_and_tiling_operands();
  internal_test_mcdc_stream_flat_guards();
  internal_test_mcdc_chunked_argument_guards();
  return 0;
}
