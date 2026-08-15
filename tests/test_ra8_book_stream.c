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
static uint32_t flat_len(void)
{
  return (uint32_t)(offsetof(stream_book_t, pool) + sizeof(s_book.pool));
}

/** @brief Compute the standard reflected CRC-32 used by the RABOOK header. */
static uint32_t fixture_crc32(const uint8_t* data, uint32_t len)
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
static void refresh_crc(void)
{
  const uint8_t* body = (const uint8_t*)&s_book + k_ra8_book_sizeof_header;
  s_book.hdr.crc32    = fixture_crc32(body, flat_len() - (uint32_t)k_ra8_book_sizeof_header);
}

/** @brief Append one NUL-terminated string to the exact fixture pool. */
static uint32_t fixture_intern(uint32_t* cursor, const char* text)
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
  s_book.hdr.total_size       = flat_len();
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
  const uint32_t body          = fixture_intern(&cursor, "body");
  const uint32_t klass         = fixture_intern(&cursor, "class");
  const uint32_t page          = fixture_intern(&cursor, "page");
  const uint32_t text          = fixture_intern(&cursor, "hello");
  const uint32_t title         = fixture_intern(&cursor, "one");
  const uint32_t href          = fixture_intern(&cursor, "one.xhtml");
  const uint32_t css           = fixture_intern(&cursor, "p{}");
  const uint32_t raster        = fixture_intern(&cursor, "p.gray");
  const uint32_t svg           = fixture_intern(&cursor, "v.svg");
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
  refresh_crc();
}

/** @brief Exact random read used by flat and packed-memory fixtures. */
static ra8_err_t memory_read(void* ctx, uint64_t offset, uint8_t* dst, uint32_t len)
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
static ra8_err_t validate_flat(void)
{
  stream_mem_t      mem = {.data = (uint8_t*)&s_book, .len = flat_len()};
  ra8_book_header_t hdr = {};
  return ra8_book_validate_stream_strict(memory_read,
                                         &mem,
                                         mem.len,
                                         s_validate_work,
                                         sizeof(s_validate_work),
                                         &hdr);
}

/** @brief Flat-source callback expected by the production RBKC writer. */
static ra8_err_t
writer_read(void* ctx, uint32_t offset, uint8_t* dst, uint32_t requested, uint32_t* out_read)
{
  stream_mem_t*   mem = (stream_mem_t*)ctx;
  const ra8_err_t err = memory_read(mem, offset, dst, requested);
  if (err == k_ra8_ok) {
    *out_read = requested;
  }
  return err;
}

/** @brief Random-write callback expected by the production RBKC writer. */
static ra8_err_t writer_write(void*          ctx,
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
static ra8_err_t
fixture_inflate(const void* src, size_t src_len, void* dst, size_t dst_cap, size_t* out_len)
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

/** @test A canonical flat fixture is decoded, validated, and published. */
static void test_flat_happy_and_args(void)
{
  TEST_BEGIN("strict stream flat happy and arguments");
  internal_setup_book();
  TEST_ASSERT_EQ(k_ra8_ok, validate_flat());
  char   xhtml[64] = {};
  size_t xhtml_len = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_book_chapter_to_xhtml(&s_book, 0U, xhtml, sizeof(xhtml) - 1U, &xhtml_len));
  TEST_ASSERT_EQ(strlen("<body class=\"page\">hello</body>"), xhtml_len);
  TEST_ASSERT_EQ(0, strcmp(xhtml, "<body class=\"page\">hello</body>"));
  stream_mem_t      mem = {.data = (uint8_t*)&s_book, .len = flat_len()};
  ra8_book_header_t hdr = {.total_size = UINT32_MAX};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_book_validate_stream_strict(NULL,
                                                 &mem,
                                                 mem.len,
                                                 s_validate_work,
                                                 sizeof(s_validate_work),
                                                 &hdr));
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_size,
    ra8_book_validate_stream_strict(memory_read, &mem, mem.len, s_validate_work, 0U, &hdr));
  TEST_ASSERT_EQ(0U, hdr.total_size);
  TEST_END("strict stream flat happy and arguments");
}

/** @test Header, layout, string, index, and CRC corruption is rejected. */
static void test_header_table_string_crc_corruption(void)
{
  TEST_BEGIN("strict stream header table string and CRC corruption");
  internal_setup_book();
  s_book.hdr.magic[0] = 'X';
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, validate_flat());
  internal_setup_book();
  s_book.hdr.flags = 0x80000000U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, validate_flat());
  internal_setup_book();
  s_book.hdr.chapter_off++;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, validate_flat());
  internal_setup_book();
  s_book.hdr.title_off++;
  refresh_crc();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, validate_flat());
  internal_setup_book();
  s_book.chapters[0].root_node = s_book.hdr.node_count;
  refresh_crc();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, validate_flat());
  internal_setup_book();
  s_book.hdr.crc32 ^= UINT32_MAX;
  TEST_ASSERT_EQ(k_ra8_err_range_check_failed, validate_flat());
  TEST_END("strict stream header table string and CRC corruption");
}

/** @test Cycles, duplicate ownership, orphan nodes, and empty names fail. */
static void test_dom_ownership_and_renderer_safety(void)
{
  TEST_BEGIN("strict stream DOM ownership and renderer safety");
  internal_setup_book();
  s_book.nodes[1].next_sibling = 0U;
  refresh_crc();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, validate_flat());
  internal_setup_book();
  s_book.nodes[1].kind         = (uint8_t)k_ra8_book_node_element;
  s_book.nodes[1].name_off     = s_book.nodes[0].name_off;
  s_book.nodes[1].text_off     = 0U;
  s_book.chapters[0].root_node = 1U;
  refresh_crc();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, validate_flat());
  internal_setup_book();
  s_book.nodes[0].first_child = (uint32_t)k_ra8_book_nil;
  refresh_crc();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, validate_flat());
  internal_setup_book();
  s_book.nodes[0].name_off = 0U;
  refresh_crc();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, validate_flat());
  internal_setup_book();
  s_book.attrs[0].name_off = 0U;
  refresh_crc();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, validate_flat());
  TEST_END("strict stream DOM ownership and renderer safety");
}

/** @test Image formats, depths, dimensions, extents, and pool tiling fail
 * closed. */
static void test_image_corruption(void)
{
  TEST_BEGIN("strict stream image corruption");
  internal_setup_book();
  s_book.images[0].format = UINT8_MAX;
  refresh_crc();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, validate_flat());
  internal_setup_book();
  s_book.images[0].data_size++;
  refresh_crc();
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, validate_flat());
  internal_setup_book();
  s_book.images[1].data_off++;
  refresh_crc();
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, validate_flat());
  internal_setup_book();
  s_book.images[1].data_size = 0U;
  s_book.images[1].raw_size  = 0U;
  s_book.hdr.image_pool_size = 2U;
  s_book.hdr.total_size      = s_book.hdr.image_pool_off + 2U;
  refresh_crc();
  stream_mem_t      mem = {.data = (uint8_t*)&s_book, .len = s_book.hdr.total_size};
  ra8_book_header_t hdr = {};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_book_validate_stream_strict(memory_read,
                                                 &mem,
                                                 mem.len,
                                                 s_validate_work,
                                                 sizeof(s_validate_work),
                                                 &hdr));
  TEST_END("strict stream image corruption");
}

/** @test Production writer -> reader validation checks every compressed chunk.
 */
static void test_production_rbkc_round_trip_and_chunk_corruption(void)
{
  TEST_BEGIN("strict stream production RBKC round trip and chunk corruption");
  internal_setup_book();
  (void)memset(s_packed, 0, sizeof(s_packed));
  stream_mem_t                     source = {.data = (uint8_t*)&s_book, .len = flat_len()};
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
                 ra8_rabook_container_write(writer_read,
                                            &source,
                                            flat_len(),
                                            k_stream_chunk,
                                            writer_write,
                                            &dest,
                                            &ws,
                                            &packed_len));
  stream_mem_t       file                    = {.data = s_packed, .len = packed_len};
  uint64_t           table[k_stream_offsets] = {};
  ra8_book_chunked_t rd                      = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_book_chunked_open(&rd,
                                       memory_read,
                                       &file,
                                       file.len,
                                       fixture_inflate,
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
  TEST_ASSERT_EQ(flat_len(), hdr.total_size);
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
  stream_mem_t                     source = {.data = (uint8_t*)&s_book, .len = flat_len()};
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
                 ra8_rabook_container_write(writer_read,
                                            &source,
                                            flat_len(),
                                            k_stream_chunk,
                                            writer_write,
                                            &dest,
                                            &ws,
                                            &packed_len));
  fixture->file = (stream_mem_t){.data = s_packed, .len = packed_len};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_book_chunked_open(&fixture->reader,
                                       memory_read,
                                       &fixture->file,
                                       fixture->file.len,
                                       fixture_inflate,
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
    ra8_book_chunked_t reader;
    uint8_t            chunk[k_stream_chunk];
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

int main(void)
{
  test_flat_happy_and_args();
  test_header_table_string_crc_corruption();
  test_dom_ownership_and_renderer_safety();
  test_image_corruption();
  test_production_rbkc_round_trip_and_chunk_corruption();
  internal_test_chunked_workspace_and_reader_guards();
  return 0;
}
