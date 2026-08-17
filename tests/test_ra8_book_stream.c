/**
 * @file test_ra8_book_stream.c
 * @brief Strict streamed RABOOK1 and production RBKC reader validation tests.
 *
 * @details
 * Exercises canonical flat and compressed round trips plus malformed headers,
 * layouts, DOM ownership, image extents, CRCs, callback failures, aliased
 * workspaces, and partial reader state. The fixed memory-backed fixture in
 * book_stream_fixture.c keeps every validation and decompression boundary
 * deterministic and allocation-free; the per-operand MC/DC vectors that isolate
 * one condition at a time live beside these suites in
 * test_ra8_book_stream_mcdc.c.
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
#include "unity_minimal.h"

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
 * Decisions: libs/ra8_book/src/ra8_book_stream.c@internal_validate_one_node
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

/**
 * @brief Report whether a chunked reader still holds its saved fields.
 * @details Member-wise rather than byte-wise: ::ra8_book_chunked_t interleaves
 *          32-bit counts with pointers and 64-bit offsets, so it carries
 *          padding that the struct assignment snapshotting it is not required
 *          to copy. Comparing the object representation would therefore be
 *          asserting something about padding, not about the reader.
 * @param[in] reader Reader observed after the rejected call.
 * @param[in] saved Reader snapshot taken before it.
 * @return Whether every field is unchanged.
 * @retval true The rejected call wrote nothing.
 * @retval false At least one field moved.
 * @pre Both pointers address initialized readers.
 * @pre Neither pointer is null.
 * @post Neither reader is modified.
 * @post The result depends only on the reader fields.
 * @note Test-only helper with no production ABI.
 * @since 0.1.0
 */
RA8_INTERNAL
static bool internal_reader_unchanged(const ra8_book_chunked_t* reader,
                                      const ra8_book_chunked_t* saved)
{
  if ((reader->file_read != saved->file_read) || (reader->file_ctx != saved->file_ctx)) {
    return false;
  }
  if ((reader->inflate != saved->inflate) || (reader->table != saved->table)) {
    return false;
  }
  if ((reader->staging != saved->staging) || (reader->staging_cap != saved->staging_cap)) {
    return false;
  }
  if (reader->table_cap_entries != saved->table_cap_entries) {
    return false;
  }
  if ((reader->payload_off != saved->payload_off) ||
      (reader->inflated_total != saved->inflated_total)) {
    return false;
  }
  return (reader->chunk_bytes == saved->chunk_bytes) && (reader->chunk_count == saved->chunk_count);
}

/** @brief Prove every reader/workspace alias fails before I/O. */
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
}

/** @brief Prove an output aliasing the reader itself fails before I/O. */
RA8_INTERNAL
static void internal_check_chunked_output_aliases(const stream_guard_fixture_t* fixture)
{
  union {
    /** @brief Reader view used as the source object. */
    ra8_book_chunked_t reader;
    /** @brief Aliased header destination under test. */
    ra8_book_header_t header;
  } reader_header_alias          = {.reader = fixture->reader};
  const ra8_book_chunked_t saved = reader_header_alias.reader;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_book_chunked_validate_strict(&reader_header_alias.reader,
                                                  g_reader_chunk,
                                                  sizeof(g_reader_chunk),
                                                  g_validate_work,
                                                  sizeof(g_validate_work),
                                                  &reader_header_alias.header));
  TEST_ASSERT(internal_reader_unchanged(&reader_header_alias.reader, &saved));

  ra8_book_header_t hdr = {.total_size = UINT32_MAX};
  union {
    /** @brief Reader view used as the source object. */
    ra8_book_chunked_t reader;
    /** @brief Aliased transfer destination under test. */
    uint8_t chunk[k_stream_chunk];
  } reader_chunk_alias = {.reader = fixture->reader};
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
  internal_check_chunked_output_aliases(&fixture);
  internal_check_chunked_reader_guards(&fixture);
  TEST_END("strict chunked workspace and reader guards");
}

int main(void)
{
  internal_test_flat_happy_and_args();
  internal_test_header_table_string_crc_corruption();
  internal_test_dom_ownership_and_renderer_safety();
  internal_test_image_corruption();
  internal_test_production_rbkc_round_trip_and_chunk_corruption();
  internal_test_chunked_workspace_and_reader_guards();
  return 0;
}
