/**
 * @file book_stream_fixture.c
 * @brief Canonical RABOOK1 flat fixture and RBKC packed-container builder.
 *
 * @details
 * Builds one wire-valid RABOOK1 image over fixed storage, re-seals its body
 * CRC after a single-field corruption, serves it through an exact memory read
 * callback with deterministic fault injection, and packs it into a production
 * RBKC container opened as a live chunked reader. Every buffer is statically
 * sized, so no vector in either strict book-stream test unit allocates.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "book_stream_fixture_internal.h"
#include "miniz.h"
#include "ra8_attributes.h"
#include "ra8_book_chunked.h"
#include "ra8_book_stream.h"
#include "ra8_book_stream_internal.h"
#include "ra8_io_compress.h"
#include "ra8_rabook_container.h"
#include "unity_minimal.h"

/*
 * miniz.h publishes the zlib-compatible aliases, one of which is the
 * object-like macro `crc32`. Left defined it rewrites the RABOOK1 header's
 * crc32 FIELD at every use below. Vendored headers reached through the book
 * includes happen to #undef it again, so an alphabetically luckier include
 * order hides the collision entirely; drop it here so this translation unit
 * does not depend on that accident.
 */
#undef crc32

/** @brief Compressor arena with conservative host and target alignment. */
typedef union {
  /** @brief Forces alignment suitable for tdefl state. */
  max_align_t align;
  uint8_t     bytes[k_ra8_io_compress_scratch_bytes]; /**< Compressor bytes. */
} stream_compressor_t;

stream_book_t g_book;
uint8_t       g_validate_work[k_stream_validate_work];
uint8_t       g_reader_staging[k_stream_compressed];
uint8_t       g_reader_chunk[k_stream_chunk];

static uint8_t             s_packed[k_stream_packed];
static uint8_t             s_chunk_in[k_stream_chunk];
static uint8_t             s_compressed[k_stream_compressed];
static uint64_t            s_offsets[k_stream_offsets];
static stream_compressor_t s_compressor;
static tinfl_decompressor  s_tinfl;

/** @brief Implementation of `priv_book_fixture_flat_len()`. */
RA8_PRIV uint32_t priv_book_fixture_flat_len(void)
{
  return (uint32_t)(offsetof(stream_book_t, pool) + sizeof(g_book.pool));
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

/** @brief Implementation of `priv_book_fixture_refresh_crc()`. */
RA8_PRIV void priv_book_fixture_refresh_crc(void)
{
  const uint8_t* body = (const uint8_t*)&g_book + k_ra8_book_sizeof_header;
  g_book.hdr.crc32 =
    internal_fixture_crc32(body, priv_book_fixture_flat_len() - (uint32_t)k_ra8_book_sizeof_header);
}

/** @brief Append one NUL-terminated string to the exact fixture pool. */
RA8_INTERNAL static uint32_t internal_fixture_intern(uint32_t* cursor, const char* text)
{
  const uint32_t at  = *cursor;
  const uint32_t len = (uint32_t)strlen(text) + 1U;
  TEST_ASSERT(len <= ((uint32_t)sizeof(g_book.strings) - at));
  (void)memcpy(&g_book.strings[at], text, len);
  *cursor += len;
  return at;
}

/** @brief Initialize canonical flat-header geometry over the fixture layout. */
RA8_INTERNAL
static void internal_setup_book_header(void)
{
  (void)memset(&g_book, 0, sizeof(g_book));
  (void)memcpy(g_book.hdr.magic, "RABOOK1", sizeof(g_book.hdr.magic));
  g_book.hdr.format_version   = (uint32_t)k_ra8_book_format_version;
  g_book.hdr.total_size       = priv_book_fixture_flat_len();
  g_book.hdr.chapter_count    = 1U;
  g_book.hdr.chapter_off      = offsetof(stream_book_t, chapters);
  g_book.hdr.node_count       = 2U;
  g_book.hdr.node_off         = offsetof(stream_book_t, nodes);
  g_book.hdr.attr_count       = 1U;
  g_book.hdr.attr_off         = offsetof(stream_book_t, attrs);
  g_book.hdr.stylesheet_count = 1U;
  g_book.hdr.stylesheet_off   = offsetof(stream_book_t, styles);
  g_book.hdr.image_count      = 2U;
  g_book.hdr.image_off        = offsetof(stream_book_t, images);
  g_book.hdr.string_off       = offsetof(stream_book_t, strings);
  g_book.hdr.string_size      = sizeof(g_book.strings);
  g_book.hdr.image_pool_off   = offsetof(stream_book_t, pool);
  g_book.hdr.image_pool_size  = sizeof(g_book.pool);
}

/** @brief Implementation of `priv_book_fixture_setup()`. */
RA8_PRIV void priv_book_fixture_setup(void)
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
  g_book.hdr.title_off         = title;
  g_book.hdr.author_off        = 0U;
  g_book.hdr.language_off      = 0U;
  g_book.hdr.identifier_off    = 0U;
  g_book.hdr.cover_image_index = 0U;

  g_book.chapters[0] = (ra8_book_chapter_t){.title_off = title, .href_off = href, .root_node = 0U};
  g_book.nodes[0]    = (ra8_book_node_t){.kind         = (uint8_t)k_ra8_book_node_element,
                                         .attr_count   = 1U,
                                         .name_off     = body,
                                         .text_off     = 0U,
                                         .first_attr   = 0U,
                                         .first_child  = 1U,
                                         .next_sibling = (uint32_t)k_ra8_book_nil};
  g_book.nodes[1]    = (ra8_book_node_t){.kind         = (uint8_t)k_ra8_book_node_text,
                                         .attr_count   = 0U,
                                         .name_off     = 0U,
                                         .text_off     = text,
                                         .first_attr   = (uint32_t)k_ra8_book_nil,
                                         .first_child  = (uint32_t)k_ra8_book_nil,
                                         .next_sibling = (uint32_t)k_ra8_book_nil};
  g_book.attrs[0]    = (ra8_book_attr_t){.name_off = klass, .value_off = page};
  g_book.styles[0] =
    (ra8_book_stylesheet_t){.source_off = css, .scope_chapter = (uint32_t)k_ra8_book_nil};
  g_book.images[0] = (ra8_book_image_t){.id_off       = raster,
                                        .width        = 2U,
                                        .height       = 2U,
                                        .format       = (uint8_t)k_ra8_book_image_gray4,
                                        .pixel_format = (uint8_t)k_ra8_book_pixfmt_gray4,
                                        .data_off     = 0U,
                                        .data_size    = 2U,
                                        .raw_size     = 2U};
  g_book.images[1] = (ra8_book_image_t){.id_off       = svg,
                                        .width        = 0U,
                                        .height       = 0U,
                                        .format       = (uint8_t)k_ra8_book_image_svg,
                                        .pixel_format = (uint8_t)k_ra8_book_pixfmt_gray4,
                                        .data_off     = 2U,
                                        .data_size    = 4U,
                                        .raw_size     = 4U};
  g_book.pool[0]   = 0x1FU;
  g_book.pool[1]   = 0xA5U;
  (void)memcpy(&g_book.pool[2], "<s/>", 4U);
  priv_book_fixture_refresh_crc();
}

/** @brief Implementation of `priv_book_fixture_read()`. */
RA8_PRIV ra8_err_t priv_book_fixture_read(void* ctx, uint64_t offset, uint8_t* dst, uint32_t len)
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

/** @brief Implementation of `priv_book_fixture_validate()`. */
RA8_PRIV ra8_err_t priv_book_fixture_validate(void)
{
  stream_mem_t      mem = {.data = (uint8_t*)&g_book, .len = priv_book_fixture_flat_len()};
  ra8_book_header_t hdr = {};
  return ra8_book_validate_stream_strict(priv_book_fixture_read,
                                         &mem,
                                         mem.len,
                                         g_validate_work,
                                         sizeof(g_validate_work),
                                         &hdr);
}

/** @brief Implementation of `priv_book_fixture_context()`. */
RA8_PRIV stream_validate_t priv_book_fixture_context(stream_mem_t* mem)
{
  *mem = (stream_mem_t){.data = (uint8_t*)&g_book, .len = priv_book_fixture_flat_len()};
  return (stream_validate_t){.read        = priv_book_fixture_read,
                             .read_ctx    = mem,
                             .source_size = mem->len,
                             .scratch     = g_validate_work,
                             .scratch_cap = sizeof(g_validate_work),
                             .hdr         = g_book.hdr};
}

/** @brief Flat-source callback expected by the production RBKC writer. */
RA8_INTERNAL static ra8_err_t internal_writer_read(void*     ctx,
                                                   uint32_t  offset,
                                                   uint8_t*  dst,
                                                   uint32_t  requested,
                                                   uint32_t* out_read)
{
  stream_mem_t*   mem = (stream_mem_t*)ctx;
  const ra8_err_t err = priv_book_fixture_read(mem, offset, dst, requested);
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

/** @brief Implementation of `priv_book_fixture_open_packed()`. */
RA8_PRIV void priv_book_fixture_open_packed(stream_guard_fixture_t* fixture)
{
  priv_book_fixture_setup();
  (void)memset(s_packed, 0, sizeof(s_packed));
  stream_mem_t source = {.data = (uint8_t*)&g_book, .len = priv_book_fixture_flat_len()};
  stream_mem_t dest   = {.data = s_packed, .len = sizeof(s_packed)};
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
                                            priv_book_fixture_flat_len(),
                                            k_stream_chunk,
                                            internal_writer_write,
                                            &dest,
                                            &ws,
                                            &packed_len));
  fixture->file = (stream_mem_t){.data = s_packed, .len = packed_len};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_book_chunked_open(&fixture->reader,
                                       priv_book_fixture_read,
                                       &fixture->file,
                                       fixture->file.len,
                                       internal_fixture_inflate,
                                       fixture->table,
                                       sizeof(fixture->table) / sizeof(fixture->table[0]),
                                       g_reader_staging,
                                       sizeof(g_reader_staging)));
}
