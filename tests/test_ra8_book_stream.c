/**
 * @file test_ra8_book_stream.c
 * @brief Strict streamed RABOOK1 and production RBKC reader validation tests.
 *
 * @details
 * Exercises canonical flat and compressed round trips plus malformed headers,
 * layouts, DOM ownership, image extents, CRCs, callback failures, aliased
 * workspaces, and partial reader state. Fixed memory-backed fixtures keep every
 * validation and decompression boundary deterministic and allocation-free.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "miniz.h"
#include "ra8_attributes.h"
#include "ra8_book_chunked.h"
#include "ra8_book_stream.h"
#include "ra8_book_stream_internal.h"
#include "ra8_io_compress.h"
#include "ra8_rabook_container.h"
#include "unity_minimal.h"

/** @brief Fixed fixture and bounded workspace dimensions. */
typedef enum : uint32_t {
  k_stream_strings       = 64U,   /**< Exact string-pool bytes.           */
  k_stream_pool          = 6U,    /**< Two gray4 bytes plus four SVG.     */
  k_stream_chunk         = 64U,   /**< Writer/reader inflated chunk size. */
  k_stream_compressed    = 256U,  /**< One compressed-stream budget.      */
  k_stream_packed        = 2048U, /**< Complete RBKC destination budget.  */
  k_stream_offsets       = 8U,    /**< Chunk-table entry budget.          */
  k_stream_validate_work = 17U,   /**< CRC transfer + node mark budget.   */
} stream_limit_t;

/** @brief Wire-valid flat fixture with exact canonical segment ordering. */
typedef struct {
  ra8_book_header_t     hdr;                       /**< Flat header.      */
  ra8_book_chapter_t    chapters[1];               /**< One chapter.      */
  ra8_book_node_t       nodes[2];                  /**< Element + text.   */
  ra8_book_attr_t       attrs[1];                  /**< One class attr.   */
  ra8_book_stylesheet_t styles[1];                 /**< One global style. */
  ra8_book_image_t      images[2];                 /**< Raster + SVG.     */
  char                  strings[k_stream_strings]; /**< Interned strings. */
  uint8_t               pool[k_stream_pool];       /**< Raw image bytes.  */
} stream_book_t;

/** @brief Memory-backed exact reader with deterministic fault injection. */
typedef struct {
  uint8_t* data;      /**< Backing bytes.                         */
  uint64_t len;       /**< Exact readable/writable byte count.    */
  uint32_t calls;     /**< Read callback invocation count.        */
  uint32_t fail_call; /**< One-based failing call, zero disables. */
} stream_mem_t;

/** @brief Compressor arena with conservative host and target alignment. */
typedef union {
  /** @brief Forces alignment suitable for tdefl state. */
  max_align_t align;
  uint8_t     bytes[k_ra8_io_compress_scratch_bytes]; /**< Compressor bytes. */
} stream_compressor_t;

/** @brief One live packed-reader fixture for strict guard fault vectors. */
typedef struct {
  stream_mem_t       file;                    /**< Packed file callback state. */
  uint64_t           table[k_stream_offsets]; /**< Retained chunk table.       */
  ra8_book_chunked_t reader;                  /**< Open reader under test.     */
} stream_guard_fixture_t;

static stream_book_t       s_book;
static uint8_t             s_packed[k_stream_packed];
static uint8_t             s_chunk_in[k_stream_chunk];
static uint8_t             s_compressed[k_stream_compressed];
static uint64_t            s_offsets[k_stream_offsets];
static uint8_t             s_reader_staging[k_stream_compressed];
static uint8_t             s_reader_chunk[k_stream_chunk];
static uint8_t             s_validate_work[k_stream_validate_work];
static stream_compressor_t s_compressor;
static tinfl_decompressor  s_tinfl;

/** @brief Compute the fixture's logical length without trailing struct padding.
 */
RA8_INTERNAL static uint32_t internal_flat_len(void)
{
  return (uint32_t)(offsetof(stream_book_t, pool) + sizeof(s_book.pool));
}

/** @brief Compute the standard reflected CRC-32 used by the RABOOK header. */
RA8_INTERNAL static uint32_t internal_fixture_crc32(const uint8_t* data, uint32_t len)
{
  uint32_t crc = 0xFFFFFFFFU;
  for (uint32_t i = 0U; i < len; ++i) {
    crc ^= data[i];
    for (uint8_t bit = 0U; bit < 8U; ++bit) {
      const uint32_t mask = (uint32_t)(-(int32_t)(crc & 1U));
      crc                 = (crc >> 1U) ^ (0xEDB88320U & mask);
    }
  }
  return crc ^ 0xFFFFFFFFU;
}

/** @brief Refresh the body CRC after one semantic corruption vector. */
RA8_INTERNAL static void internal_refresh_crc(void)
{
  const uint8_t* body = (const uint8_t*)&s_book + k_ra8_book_sizeof_header;
  s_book.hdr.crc32 =
    internal_fixture_crc32(body, internal_flat_len() - (uint32_t)k_ra8_book_sizeof_header);
}

/** @brief Append one NUL-terminated string to the exact fixture pool. */
RA8_INTERNAL static uint32_t internal_fixture_intern(uint32_t* cursor, const char* text)
{
  const uint32_t at  = *cursor;
  const uint32_t len = (uint32_t)strlen(text) + 1U;
  TEST_ASSERT(len <= ((uint32_t)sizeof(s_book.strings) - at));
  (void)memcpy(&s_book.strings[at], text, len);
  *cursor += len;
  return at;
}

/** @brief Initialize canonical flat-header geometry over the fixture layout. */
RA8_INTERNAL
static void internal_setup_book_header(void)
{
  (void)memset(&s_book, 0, sizeof(s_book));
  (void)memcpy(s_book.hdr.magic, "RABOOK1", sizeof(s_book.hdr.magic));
  s_book.hdr.format_version   = (uint32_t)k_ra8_book_format_version;
  s_book.hdr.total_size       = internal_flat_len();
  s_book.hdr.chapter_count    = 1U;
  s_book.hdr.chapter_off      = offsetof(stream_book_t, chapters);
  s_book.hdr.node_count       = 2U;
  s_book.hdr.node_off         = offsetof(stream_book_t, nodes);
  s_book.hdr.attr_count       = 1U;
  s_book.hdr.attr_off         = offsetof(stream_book_t, attrs);
  s_book.hdr.stylesheet_count = 1U;
  s_book.hdr.stylesheet_off   = offsetof(stream_book_t, styles);
  s_book.hdr.image_count      = 2U;
  s_book.hdr.image_off        = offsetof(stream_book_t, images);
  s_book.hdr.string_off       = offsetof(stream_book_t, strings);
  s_book.hdr.string_size      = sizeof(s_book.strings);
  s_book.hdr.image_pool_off   = offsetof(stream_book_t, pool);
  s_book.hdr.image_pool_size  = sizeof(s_book.pool);
}

/** @brief Build one canonical flat blob accepted by every strict pass. */
RA8_INTERNAL
static void internal_setup_book(void)
{
  internal_setup_book_header();
  uint32_t       cursor        = 1U;
  const uint32_t body          = internal_fixture_intern(&cursor, "body");
  const uint32_t klass         = internal_fixture_intern(&cursor, "class");
  const uint32_t page          = internal_fixture_intern(&cursor, "page");
  const uint32_t text          = internal_fixture_intern(&cursor, "hello");
  const uint32_t title         = internal_fixture_intern(&cursor, "one");
  const uint32_t href          = internal_fixture_intern(&cursor, "one.xhtml");
  const uint32_t css           = internal_fixture_intern(&cursor, "p{}");
  const uint32_t raster        = internal_fixture_intern(&cursor, "p.gray");
  const uint32_t svg           = internal_fixture_intern(&cursor, "v.svg");
  s_book.hdr.title_off         = title;
  s_book.hdr.author_off        = 0U;
  s_book.hdr.language_off      = 0U;
  s_book.hdr.identifier_off    = 0U;
  s_book.hdr.cover_image_index = 0U;

  s_book.chapters[0] = (ra8_book_chapter_t){.title_off = title, .href_off = href, .root_node = 0U};
  s_book.nodes[0]    = (ra8_book_node_t){.kind         = (uint8_t)k_ra8_book_node_element,
                                         .attr_count   = 1U,
                                         .name_off     = body,
                                         .text_off     = 0U,
                                         .first_attr   = 0U,
                                         .first_child  = 1U,
                                         .next_sibling = (uint32_t)k_ra8_book_nil};
  s_book.nodes[1]    = (ra8_book_node_t){.kind         = (uint8_t)k_ra8_book_node_text,
                                         .attr_count   = 0U,
                                         .name_off     = 0U,
                                         .text_off     = text,
                                         .first_attr   = (uint32_t)k_ra8_book_nil,
                                         .first_child  = (uint32_t)k_ra8_book_nil,
                                         .next_sibling = (uint32_t)k_ra8_book_nil};
  s_book.attrs[0]    = (ra8_book_attr_t){.name_off = klass, .value_off = page};
  s_book.styles[0] =
    (ra8_book_stylesheet_t){.source_off = css, .scope_chapter = (uint32_t)k_ra8_book_nil};
  s_book.images[0] = (ra8_book_image_t){.id_off       = raster,
                                        .width        = 2U,
                                        .height       = 2U,
                                        .format       = (uint8_t)k_ra8_book_image_gray4,
                                        .pixel_format = (uint8_t)k_ra8_book_pixfmt_gray4,
                                        .data_off     = 0U,
                                        .data_size    = 2U,
                                        .raw_size     = 2U};
  s_book.images[1] = (ra8_book_image_t){.id_off       = svg,
                                        .width        = 0U,
                                        .height       = 0U,
                                        .format       = (uint8_t)k_ra8_book_image_svg,
                                        .pixel_format = (uint8_t)k_ra8_book_pixfmt_gray4,
                                        .data_off     = 2U,
                                        .data_size    = 4U,
                                        .raw_size     = 4U};
  s_book.pool[0]   = 0x1FU;
  s_book.pool[1]   = 0xA5U;
  (void)memcpy(&s_book.pool[2], "<s/>", 4U);
  internal_refresh_crc();
}

/** @brief Exact random read used by flat and packed-memory fixtures. */
RA8_INTERNAL static ra8_err_t
internal_memory_read(void* ctx, uint64_t offset, uint8_t* dst, uint32_t len)
{
  stream_mem_t* mem = (stream_mem_t*)ctx;
  mem->calls++;
  if ((mem->fail_call != 0U) && (mem->calls == mem->fail_call)) {
    return k_ra8_err_hw_timeout;
  }
  if ((offset > mem->len) || ((uint64_t)len > (mem->len - offset))) {
    return k_ra8_err_out_of_range;
  }
  (void)memcpy(dst, &mem->data[(size_t)offset], len);
  return k_ra8_ok;
}

/** @brief Validate the current flat fixture through the public strict API. */
RA8_INTERNAL static ra8_err_t internal_validate_flat(void)
{
  stream_mem_t      mem = {.data = (uint8_t*)&s_book, .len = internal_flat_len()};
  ra8_book_header_t hdr = {};
  return ra8_book_validate_stream_strict(internal_memory_read,
                                         &mem,
                                         mem.len,
                                         s_validate_work,
                                         sizeof(s_validate_work),
                                         &hdr);
}

/**
 * @brief Bind direct private-validator calls to the canonical flat fixture.
 * @param[out] mem Receives the exact memory-source descriptor.
 * @return Fully initialized private validation context.
 * @pre internal_setup_book() initialized ::s_book.
 * @pre @p mem addresses one writable descriptor.
 * @post The result reads from ::s_book and borrows ::s_validate_work.
 * @post No validation pass has run yet.
 * @note Test-only fixture helper with caller-owned storage.
 * @since Version 0.1.0
 */
RA8_INTERNAL static stream_validate_t internal_direct_context(stream_mem_t* mem)
{
  *mem = (stream_mem_t){.data = (uint8_t*)&s_book, .len = internal_flat_len()};
  return (stream_validate_t){.read        = internal_memory_read,
                             .read_ctx    = mem,
                             .source_size = mem->len,
                             .scratch     = s_validate_work,
                             .scratch_cap = sizeof(s_validate_work),
                             .hdr         = s_book.hdr};
}

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
  internal_setup_book();
  stream_mem_t      mem  = {};
  stream_validate_t ctx  = internal_direct_context(&mem);
  uint8_t           byte = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, priv_book_stream_read(&ctx, ctx.source_size, &byte, 0U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 priv_book_stream_read(&ctx, ctx.source_size + 1U, &byte, 0U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, priv_book_stream_read(&ctx, ctx.source_size, &byte, 1U));
  TEST_ASSERT_EQ(k_ra8_ok, priv_book_stream_validate_string_envelope(&ctx));
  s_book.strings[0] = 1;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, priv_book_stream_validate_string_envelope(&ctx));
  mem.calls     = 0U;
  mem.fail_call = 1U;
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, priv_book_stream_validate_string_envelope(&ctx));
  internal_setup_book();
  ctx                                         = internal_direct_context(&mem);
  s_book.strings[sizeof(s_book.strings) - 1U] = 1;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, priv_book_stream_validate_string_envelope(&ctx));
  internal_setup_book();
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
  internal_setup_book();
  stream_mem_t      mem = {};
  stream_validate_t ctx = internal_direct_context(&mem);
  TEST_ASSERT_EQ(k_ra8_ok, priv_book_stream_validate_metadata(&ctx));
  ctx.hdr.cover_image_index = (uint32_t)k_ra8_book_nil;
  TEST_ASSERT_EQ(k_ra8_ok, priv_book_stream_validate_metadata(&ctx));
  ctx.hdr.cover_image_index = ctx.hdr.image_count;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, priv_book_stream_validate_metadata(&ctx));
  ctx.hdr = s_book.hdr;
  TEST_ASSERT_EQ(k_ra8_ok, priv_book_stream_validate_styles(&ctx));
  s_book.styles[0].scope_chapter = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, priv_book_stream_validate_styles(&ctx));
  s_book.styles[0].scope_chapter = ctx.hdr.chapter_count;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, priv_book_stream_validate_styles(&ctx));
  s_book.styles[0].source_off = ctx.hdr.string_size;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, priv_book_stream_validate_styles(&ctx));
  internal_setup_book();
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
  internal_setup_book();
  stream_mem_t      mem    = {};
  stream_validate_t ctx    = internal_direct_context(&mem);
  uint32_t          cursor = 0U;
  TEST_ASSERT_EQ(
    k_ra8_ok,
    priv_book_stream_validate_element(&ctx, (const uint8_t*)&s_book.nodes[0], &cursor));
  ra8_book_node_t element = s_book.nodes[0];
  element.first_attr      = 1U;
  cursor                  = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 priv_book_stream_validate_element(&ctx, (const uint8_t*)&element, &cursor));
  element            = s_book.nodes[0];
  element.attr_count = 2U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 priv_book_stream_validate_element(&ctx, (const uint8_t*)&element, &cursor));
  const ra8_book_node_t canonical = s_book.nodes[1];
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

/** @brief Flat-source callback expected by the production RBKC writer. */
RA8_INTERNAL static ra8_err_t internal_writer_read(void*     ctx,
                                                   uint32_t  offset,
                                                   uint8_t*  dst,
                                                   uint32_t  requested,
                                                   uint32_t* out_read)
{
  stream_mem_t*   mem = (stream_mem_t*)ctx;
  const ra8_err_t err = internal_memory_read(mem, offset, dst, requested);
  if (err == k_ra8_ok) {
    *out_read = requested;
  }
  return err;
}

/** @brief Random-write callback expected by the production RBKC writer. */
RA8_INTERNAL static ra8_err_t internal_writer_write(void*          ctx,
                                                    uint64_t       offset,
                                                    const uint8_t* src,
                                                    uint32_t       requested,
                                                    uint32_t*      out_written)
{
  stream_mem_t* mem = (stream_mem_t*)ctx;
  if ((offset > mem->len) || ((uint64_t)requested > (mem->len - offset))) {
    return k_ra8_err_invalid_size;
  }
  (void)memcpy(&mem->data[(size_t)offset], src, requested);
  *out_written = requested;
  return k_ra8_ok;
}

/** @brief RFC 1950 inflater used by the production chunk reader. */
RA8_INTERNAL static ra8_err_t internal_fixture_inflate(const void* src,
                                                       size_t      src_len,
                                                       void*       dst,
                                                       size_t      dst_cap,
                                                       size_t*     out_len)
{
  tinfl_init(&s_tinfl);
  size_t             in_n   = src_len;
  size_t             out_n  = dst_cap;
  const tinfl_status status = tinfl_decompress(
    &s_tinfl,
    (const mz_uint8*)src,
    &in_n,
    (mz_uint8*)dst,
    (mz_uint8*)dst,
    &out_n,
    (mz_uint32)(TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF));
  if (status != TINFL_STATUS_DONE) {
    return k_ra8_err_invalid_size;
  }
  *out_len = out_n;
  return k_ra8_ok;
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
  internal_setup_book();
  TEST_ASSERT_EQ(k_ra8_ok, internal_validate_flat());
  char   xhtml[64] = {};
  size_t xhtml_len = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_book_chapter_to_xhtml(&s_book, 0U, xhtml, sizeof(xhtml) - 1U, &xhtml_len));
  TEST_ASSERT_EQ(strlen("<body class=\"page\">hello</body>"), xhtml_len);
  TEST_ASSERT_EQ(0, strcmp(xhtml, "<body class=\"page\">hello</body>"));
  stream_mem_t      mem = {.data = (uint8_t*)&s_book, .len = internal_flat_len()};
  ra8_book_header_t hdr = {.total_size = UINT32_MAX};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_book_validate_stream_strict(NULL,
                                                 &mem,
                                                 mem.len,
                                                 s_validate_work,
                                                 sizeof(s_validate_work),
                                                 &hdr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_book_validate_stream_strict(internal_memory_read,
                                                 &mem,
                                                 mem.len,
                                                 s_validate_work,
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
  internal_setup_book();
  s_book.hdr.magic[0] = 'X';
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, internal_validate_flat());
  internal_setup_book();
  s_book.hdr.flags = 0x80000000U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, internal_validate_flat());
  internal_setup_book();
  s_book.hdr.chapter_off++;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, internal_validate_flat());
  internal_setup_book();
  s_book.hdr.title_off++;
  internal_refresh_crc();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, internal_validate_flat());
  internal_setup_book();
  s_book.chapters[0].root_node = s_book.hdr.node_count;
  internal_refresh_crc();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, internal_validate_flat());
  internal_setup_book();
  s_book.hdr.crc32 ^= UINT32_MAX;
  TEST_ASSERT_EQ(k_ra8_err_range_check_failed, internal_validate_flat());
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
  internal_setup_book();
  s_book.nodes[1].next_sibling = 0U;
  internal_refresh_crc();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, internal_validate_flat());
  internal_setup_book();
  s_book.nodes[1].kind         = (uint8_t)k_ra8_book_node_element;
  s_book.nodes[1].name_off     = s_book.nodes[0].name_off;
  s_book.nodes[1].text_off     = 0U;
  s_book.chapters[0].root_node = 1U;
  internal_refresh_crc();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, internal_validate_flat());
  internal_setup_book();
  s_book.nodes[0].first_child = (uint32_t)k_ra8_book_nil;
  internal_refresh_crc();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, internal_validate_flat());
  internal_setup_book();
  s_book.nodes[0].name_off = 0U;
  internal_refresh_crc();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, internal_validate_flat());
  internal_setup_book();
  s_book.attrs[0].name_off = 0U;
  internal_refresh_crc();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, internal_validate_flat());
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
  internal_setup_book();
  s_book.images[0].format = UINT8_MAX;
  internal_refresh_crc();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, internal_validate_flat());
  internal_setup_book();
  s_book.images[0].data_size++;
  internal_refresh_crc();
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, internal_validate_flat());
  internal_setup_book();
  s_book.images[1].data_off++;
  internal_refresh_crc();
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, internal_validate_flat());
  internal_setup_book();
  s_book.images[1].data_size = 0U;
  s_book.images[1].raw_size  = 0U;
  s_book.hdr.image_pool_size = 2U;
  s_book.hdr.total_size      = s_book.hdr.image_pool_off + 2U;
  internal_refresh_crc();
  stream_mem_t      mem = {.data = (uint8_t*)&s_book, .len = s_book.hdr.total_size};
  ra8_book_header_t hdr = {};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_book_validate_stream_strict(internal_memory_read,
                                                 &mem,
                                                 mem.len,
                                                 s_validate_work,
                                                 sizeof(s_validate_work),
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
  internal_setup_book();
  (void)memset(s_packed, 0, sizeof(s_packed));
  stream_mem_t                     source = {.data = (uint8_t*)&s_book, .len = internal_flat_len()};
  stream_mem_t                     dest   = {.data = s_packed, .len = sizeof(s_packed)};
  ra8_rabook_container_workspace_t ws = {.input          = s_chunk_in,
                                         .compressed     = s_compressed,
                                         .compressor     = s_compressor.bytes,
                                         .offsets        = s_offsets,
                                         .input_cap      = sizeof(s_chunk_in),
                                         .compressed_cap = sizeof(s_compressed),
                                         .compressor_cap = sizeof(s_compressor.bytes),
                                         .offset_cap = sizeof(s_offsets) / sizeof(s_offsets[0])};
  uint64_t                         packed_len = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_rabook_container_write(internal_writer_read,
                                            &source,
                                            internal_flat_len(),
                                            k_stream_chunk,
                                            internal_writer_write,
                                            &dest,
                                            &ws,
                                            &packed_len));
  stream_mem_t       file                    = {.data = s_packed, .len = packed_len};
  uint64_t           table[k_stream_offsets] = {};
  ra8_book_chunked_t rd                      = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_book_chunked_open(&rd,
                                       internal_memory_read,
                                       &file,
                                       file.len,
                                       internal_fixture_inflate,
                                       table,
                                       sizeof(table) / sizeof(table[0]),
                                       s_reader_staging,
                                       sizeof(s_reader_staging)));
  ra8_book_header_t hdr = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_book_chunked_validate_strict(&rd,
                                                  s_reader_chunk,
                                                  sizeof(s_reader_chunk),
                                                  s_validate_work,
                                                  sizeof(s_validate_work),
                                                  &hdr));
  TEST_ASSERT_EQ(internal_flat_len(), hdr.total_size);
  const uint32_t middle     = rd.chunk_count / 2U;
  const uint64_t corrupt_at = rd.payload_off + rd.table[middle] + 2U;
  s_packed[corrupt_at] ^= 0x5AU;
  TEST_ASSERT(ra8_book_chunked_validate_strict(&rd,
                                               s_reader_chunk,
                                               sizeof(s_reader_chunk),
                                               s_validate_work,
                                               sizeof(s_validate_work),
                                               &hdr) != k_ra8_ok);
  TEST_ASSERT_EQ(0U, hdr.total_size);
  TEST_END("strict stream production RBKC round trip and chunk corruption");
}

/** @brief Build and open one canonical packed fixture for guard tests. */
RA8_INTERNAL
static void internal_open_chunk_guard_fixture(stream_guard_fixture_t* fixture)
{
  internal_setup_book();
  (void)memset(s_packed, 0, sizeof(s_packed));
  stream_mem_t                     source = {.data = (uint8_t*)&s_book, .len = internal_flat_len()};
  stream_mem_t                     dest   = {.data = s_packed, .len = sizeof(s_packed)};
  ra8_rabook_container_workspace_t ws = {.input          = s_chunk_in,
                                         .compressed     = s_compressed,
                                         .compressor     = s_compressor.bytes,
                                         .offsets        = s_offsets,
                                         .input_cap      = sizeof(s_chunk_in),
                                         .compressed_cap = sizeof(s_compressed),
                                         .compressor_cap = sizeof(s_compressor.bytes),
                                         .offset_cap = sizeof(s_offsets) / sizeof(s_offsets[0])};
  uint64_t                         packed_len = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_rabook_container_write(internal_writer_read,
                                            &source,
                                            internal_flat_len(),
                                            k_stream_chunk,
                                            internal_writer_write,
                                            &dest,
                                            &ws,
                                            &packed_len));
  fixture->file = (stream_mem_t){.data = s_packed, .len = packed_len};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_book_chunked_open(&fixture->reader,
                                       internal_memory_read,
                                       &fixture->file,
                                       fixture->file.len,
                                       internal_fixture_inflate,
                                       fixture->table,
                                       sizeof(fixture->table) / sizeof(fixture->table[0]),
                                       s_reader_staging,
                                       sizeof(s_reader_staging)));
}

/** @brief Prove every known reader/workspace/output alias fails before I/O. */
RA8_INTERNAL
static void internal_check_chunked_alias_guards(stream_guard_fixture_t* fixture)
{
  ra8_book_header_t hdr = {.total_size = UINT32_MAX};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_book_chunked_validate_strict(&fixture->reader,
                                                  s_reader_chunk,
                                                  sizeof(s_reader_chunk),
                                                  s_reader_chunk,
                                                  sizeof(s_reader_chunk),
                                                  &hdr));
  TEST_ASSERT_EQ(0U, hdr.total_size);
  hdr.total_size = UINT32_MAX;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_book_chunked_validate_strict(&fixture->reader,
                                                  s_reader_staging,
                                                  sizeof(s_reader_staging),
                                                  s_validate_work,
                                                  sizeof(s_validate_work),
                                                  &hdr));
  TEST_ASSERT_EQ(0U, hdr.total_size);
  hdr.total_size = UINT32_MAX;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_book_chunked_validate_strict(&fixture->reader,
                                                  (uint8_t*)fixture->table,
                                                  sizeof(s_reader_chunk),
                                                  s_validate_work,
                                                  sizeof(s_validate_work),
                                                  &hdr));
  TEST_ASSERT_EQ(0U, hdr.total_size);

  const ra8_book_chunked_t saved = fixture->reader;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_book_chunked_validate_strict(&fixture->reader,
                                                  s_reader_chunk,
                                                  sizeof(s_reader_chunk),
                                                  s_validate_work,
                                                  sizeof(s_validate_work),
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
                                                  s_validate_work,
                                                  sizeof(s_validate_work),
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
                                                  s_reader_chunk,
                                                  sizeof(s_reader_chunk),
                                                  s_validate_work,
                                                  sizeof(s_validate_work),
                                                  &hdr));
  TEST_ASSERT_EQ(0U, hdr.total_size);
  broken         = fixture->reader;
  broken.staging = nullptr;
  hdr.total_size = UINT32_MAX;
  TEST_ASSERT_EQ(k_ra8_err_invalid_state,
                 ra8_book_chunked_validate_strict(&broken,
                                                  s_reader_chunk,
                                                  sizeof(s_reader_chunk),
                                                  s_validate_work,
                                                  sizeof(s_validate_work),
                                                  &hdr));
  TEST_ASSERT_EQ(0U, hdr.total_size);

  uint64_t short_table[1]  = {};
  broken                   = fixture->reader;
  broken.table             = short_table;
  broken.table_cap_entries = (uint32_t)(sizeof(short_table) / sizeof(short_table[0]));
  hdr.total_size           = UINT32_MAX;
  TEST_ASSERT_EQ(k_ra8_err_invalid_state,
                 ra8_book_chunked_validate_strict(&broken,
                                                  s_reader_chunk,
                                                  sizeof(s_reader_chunk),
                                                  s_validate_work,
                                                  sizeof(s_validate_work),
                                                  &hdr));
  TEST_ASSERT_EQ(0U, hdr.total_size);
}

/** @test Aliased workspaces and partially initialized readers fail before I/O. */
RA8_INTERNAL
static void internal_test_chunked_workspace_and_reader_guards(void)
{
  TEST_BEGIN("strict chunked workspace and reader guards");
  stream_guard_fixture_t fixture = {};
  internal_open_chunk_guard_fixture(&fixture);
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
  internal_setup_book();
  s_book.hdr.format_version = (uint32_t)k_ra8_book_format_version + 1U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, internal_validate_flat());

  internal_setup_book();
  s_book.hdr.image_pool_size = (uint32_t)sizeof(s_book.pool) - 1U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, internal_validate_flat());

  internal_setup_book();
  stream_mem_t      mem    = {};
  stream_validate_t ctx    = internal_direct_context(&mem);
  uint32_t          cursor = 0U;
  mem.calls                = 0U;
  mem.fail_call            = 1U;
  TEST_ASSERT_EQ(
    k_ra8_err_hw_timeout,
    priv_book_stream_validate_element(&ctx, (const uint8_t*)&s_book.nodes[0], &cursor));

  internal_setup_book();
  s_book.chapters[0].title_off = s_book.hdr.string_size;
  internal_refresh_crc();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, internal_validate_flat());

  internal_setup_book();
  s_book.chapters[0].root_node = 1U;
  internal_refresh_crc();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, internal_validate_flat());

  internal_setup_book();
  s_book.nodes[0].next_sibling = 1U;
  internal_refresh_crc();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, internal_validate_flat());

  internal_setup_book();
  s_book.nodes[0].first_child = s_book.hdr.node_count;
  internal_refresh_crc();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, internal_validate_flat());

  internal_setup_book();
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
  internal_setup_book();
  s_book.images[0].width = 0U;
  internal_refresh_crc();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, internal_validate_flat());

  internal_setup_book();
  s_book.images[0].height = 0U;
  internal_refresh_crc();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, internal_validate_flat());

  internal_setup_book();
  s_book.images[0].pixel_format = UINT8_MAX;
  internal_refresh_crc();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, internal_validate_flat());

  internal_setup_book();
  s_book.images[0].width        = 2U;
  s_book.images[0].height       = 1U;
  s_book.images[0].pixel_format = (uint8_t)k_ra8_book_pixfmt_gray8;
  internal_refresh_crc();
  TEST_ASSERT_EQ(k_ra8_ok, internal_validate_flat());

  internal_setup_book();
  s_book.images[0].raw_size = s_book.images[0].data_size + 1U;
  internal_refresh_crc();
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, internal_validate_flat());

  internal_setup_book();
  s_book.images[0].reserved2 = 1U;
  internal_refresh_crc();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, internal_validate_flat());

  internal_setup_book();
  s_book.images[0].id_off = 0U;
  internal_refresh_crc();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, internal_validate_flat());

  internal_setup_book();
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
  internal_setup_book();
  s_book.images[1].width = 1U;
  internal_refresh_crc();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, internal_validate_flat());

  internal_setup_book();
  s_book.images[1].height = 1U;
  internal_refresh_crc();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, internal_validate_flat());

  internal_setup_book();
  s_book.images[1].pixel_format = (uint8_t)k_ra8_book_pixfmt_gray8;
  internal_refresh_crc();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, internal_validate_flat());

  internal_setup_book();
  s_book.images[1].raw_size = s_book.images[1].data_size + 1U;
  internal_refresh_crc();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, internal_validate_flat());

  internal_setup_book();
  s_book.images[1].data_size = s_book.hdr.image_pool_size;
  s_book.images[1].raw_size  = s_book.hdr.image_pool_size;
  internal_refresh_crc();
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, internal_validate_flat());

  internal_setup_book();
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
  internal_setup_book();
  stream_mem_t      mem = {.data = (uint8_t*)&s_book, .len = internal_flat_len()};
  ra8_book_header_t hdr = {.total_size = UINT32_MAX};
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    ra8_book_validate_stream_strict(internal_memory_read, &mem, mem.len, nullptr, 1U, &hdr));
  TEST_ASSERT_EQ(0U, hdr.total_size);
  hdr.total_size = UINT32_MAX;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_book_validate_stream_strict(internal_memory_read,
                                                 &mem,
                                                 (uint64_t)k_ra8_book_sizeof_header - 1U,
                                                 s_validate_work,
                                                 sizeof(s_validate_work),
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
  internal_open_chunk_guard_fixture(&fixture);
  ra8_book_header_t hdr = {.total_size = UINT32_MAX};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_book_chunked_validate_strict(&fixture.reader,
                                                  nullptr,
                                                  sizeof(s_reader_chunk),
                                                  s_validate_work,
                                                  sizeof(s_validate_work),
                                                  &hdr));
  TEST_ASSERT_EQ(0U, hdr.total_size);
  hdr.total_size = UINT32_MAX;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_book_chunked_validate_strict(&fixture.reader,
                                                  s_reader_chunk,
                                                  sizeof(s_reader_chunk),
                                                  nullptr,
                                                  sizeof(s_validate_work),
                                                  &hdr));
  TEST_ASSERT_EQ(0U, hdr.total_size);
  hdr.total_size = UINT32_MAX;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_book_chunked_validate_strict(&fixture.reader,
                                                  s_reader_chunk,
                                                  0U,
                                                  s_validate_work,
                                                  sizeof(s_validate_work),
                                                  &hdr));
  TEST_ASSERT_EQ(0U, hdr.total_size);
  hdr.total_size = UINT32_MAX;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_book_chunked_validate_strict(&fixture.reader,
                                                  s_reader_chunk,
                                                  sizeof(s_reader_chunk),
                                                  s_validate_work,
                                                  0U,
                                                  &hdr));
  TEST_ASSERT_EQ(0U, hdr.total_size);
  internal_setup_book();
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
