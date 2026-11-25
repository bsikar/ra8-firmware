/**
 * @file test_viewer_portable_arena.c
 * @brief Device-portable static-arena regression tests for rabook_viewer.
 * @details Exercises ::ra8_viewer_reader_requirements_source and
 * ::ra8_viewer_open_source over caller-owned static buffers without POSIX file
 * descriptors, hosted streams, or libc heap calls, covering JOF, comic CBZ,
 * EPUB reflow, compressed RBKC `.rabook`, exact 1-byte workspace shortfall
 * atomicity, faulted positioned reads, and ::ra8_stbtt_alloc_high_water.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdalign.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "book.h"
#include "miniz.h"
#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_fmt_io.h"
#include "ra8_imgdec_name.h"
#include "ra8_io_stream.h"
#include "ra8_io_stream_ram.h"
#include "ra8_stbtt_alloc.h"
#include "ra8_viewer_output_internal.h"
#include "ra8_viewer_reader.h"

/** @brief Fixed test-only caller storage capacities and protocol constants. */
typedef enum : uint32_t {
  k_pt_reader_bytes  = 18U * 1024U * 1024U,
  k_pt_tile_bytes    = (uint32_t)k_ra8_viewer_fb_width * (uint32_t)k_ra8_viewer_fb_height * 2U,
  k_pt_jof_cap       = 1024U,
  k_pt_zip_cap       = 8192U,
  k_pt_book_cap      = 4096U,
  k_pt_rbkc_cap      = 4096U,
  k_pt_report_cap    = 256U,
  k_pt_jof_w         = 16U,
  k_pt_jof_h         = 16U,
  k_pt_jof_bpp       = 3U,
  k_pt_jof_hdr_bytes = 32U,
  k_pt_jof_idx_bytes = 8U,
  k_pt_jof_ftr_bytes = 16U,
  k_pt_bmp_w         = 4U,
  k_pt_bmp_h         = 4U,
  k_pt_bmp_bpp_bits  = 24U,
  k_pt_bmp_file_hdr  = 14U,
  k_pt_bmp_info_hdr  = 40U,
  k_pt_bmp_hdr_bytes = k_pt_bmp_file_hdr + k_pt_bmp_info_hdr,
  k_pt_bmp_px_bytes  = k_pt_bmp_w * k_pt_bmp_h * 3U,
  k_pt_bmp_bytes     = k_pt_bmp_hdr_bytes + k_pt_bmp_px_bytes,
  k_pt_short_step    = 7U,
  k_pt_chunk_bytes   = 65536U,
  k_pt_sentinel      = 0xA5U,
  k_pt_white_565     = 0xFFFFU,
} portable_test_const_t;

/** @brief Byte-slice positioned source with short-read and fault injection. */
typedef struct {
  const uint8_t* data;         /**< Backing immutable document bytes.       */
  size_t         len;          /**< Document length in bytes.               */
  size_t         max_step;     /**< Maximum bytes returned per read, or 0.  */
  uint32_t       calls;        /**< Observed read callback invocations.     */
  uint32_t       fail_on_call; /**< Return failure at or past call N, or 0. */
} test_mem_source_t;

/** @brief Uncompressed ZIP archive entry descriptor. */
typedef struct {
  const char*    name;    /**< Archive member name. */
  const uint8_t* payload; /**< Member bytes.        */
  uint32_t       len;     /**< Byte length.         */
} test_zip_entry_t;

/** @brief Packed synthetic flat RABOOK1 test fixture layout. */
typedef struct {
  book_header_t  hdr;         /**< Fixed header.           */
  book_chapter_t chapters[1]; /**< Single spine chapter.   */
  book_node_t    nodes[3];    /**< body -> p -> text DOM.  */
  char           strings[64]; /**< NUL-terminated strings. */
} test_flat_book_t;

alignas(max_align_t) static uint8_t s_reader_ws[k_pt_reader_bytes];
alignas(max_align_t) static uint8_t s_tile_ws[k_pt_tile_bytes];
static uint8_t          s_jof_buf[k_pt_jof_cap];
static uint8_t          s_bmp_buf[k_pt_bmp_bytes];
static uint8_t          s_cbz_buf[k_pt_zip_cap];
static uint8_t          s_epub_buf[k_pt_zip_cap];
static uint8_t          s_comp_buf[k_pt_book_cap];
static uint8_t          s_rbkc_buf[k_pt_rbkc_cap];
static tdefl_compressor s_tdefl;

/** @brief Append a little-endian 16-bit integer. */
RA8_INTERNAL static void internal_put_u16(uint8_t* dst, size_t* pos, uint16_t v)
{
  dst[(*pos)++] = (uint8_t)(v & 0xFFU);
  dst[(*pos)++] = (uint8_t)((v >> 8U) & 0xFFU);
}

/** @brief Append a little-endian 32-bit integer. */
RA8_INTERNAL static void internal_put_u32(uint8_t* dst, size_t* pos, uint32_t v)
{
  for (uint32_t i = 0U; i < 4U; ++i) {
    dst[(*pos)++] = (uint8_t)((v >> (i * 8U)) & 0xFFU);
  }
}

/** @brief Append a little-endian 64-bit integer. */
RA8_INTERNAL static void internal_put_u64(uint8_t* dst, size_t* pos, uint64_t v)
{
  for (uint32_t i = 0U; i < 8U; ++i) {
    dst[(*pos)++] = (uint8_t)((v >> (i * 8U)) & 0xFFU);
  }
}

/** @brief Check whether any RGB565 pixel differs from pure white. */
RA8_INTERNAL static bool internal_has_ink(const uint16_t* pixels, size_t count)
{
  for (size_t idx = 0U; idx < count; ++idx) {
    if (pixels[idx] != (uint16_t)k_pt_white_565) {
      return true;
    }
  }
  return false;
}

/** @copydoc ra8_fmt_pread_fn */
RA8_INTERNAL static ra8_err_t
internal_mem_read(void* ctx, uint64_t offset, uint8_t* buffer, size_t length, size_t* out_read)
{
  if (out_read == nullptr) {
    return k_ra8_err_null_ptr;
  }
  *out_read              = 0U;
  test_mem_source_t* src = (test_mem_source_t*)ctx;
  if ((src == nullptr) || ((buffer == nullptr) && (length != 0U))) {
    return k_ra8_err_null_ptr;
  }
  src->calls++;
  if ((src->fail_on_call != 0U) && (src->calls >= src->fail_on_call)) {
    return k_ra8_fail;
  }
  if ((offset >= (uint64_t)src->len) || (length == 0U)) {
    return k_ra8_ok;
  }
  const size_t avail = src->len - (size_t)offset;
  size_t       step  = (length < avail) ? length : avail;
  if ((src->max_step != 0U) && (step > src->max_step)) {
    step = src->max_step;
  }
  (void)memcpy(buffer, &src->data[(size_t)offset], step);
  *out_read = step;
  return k_ra8_ok;
}

/** @brief Bind ::test_mem_source_t to ::ra8_fmt_source_t. */
RA8_INTERNAL static ra8_fmt_source_t
internal_bind_mem_source(test_mem_source_t* state, const uint8_t* data, size_t len)
{
  *state = (test_mem_source_t){
    .data         = data,
    .len          = len,
    .max_step     = (size_t)k_pt_short_step,
    .calls        = 0U,
    .fail_on_call = 0U,
  };
  return (ra8_fmt_source_t){
    .read_at = internal_mem_read,
    .ctx     = state,
    .size    = (uint64_t)len,
  };
}

/** @brief Construct an in-memory 16x16 raw JOF1 atlas with dark RGB pixels. */
RA8_INTERNAL static size_t internal_build_jof(void)
{
  const size_t payload_bytes = (size_t)k_pt_jof_w * (size_t)k_pt_jof_h * (size_t)k_pt_jof_bpp;
  const size_t idx_off       = (size_t)k_pt_jof_hdr_bytes + payload_bytes;
  const size_t total_bytes   = idx_off + (size_t)k_pt_jof_idx_bytes + (size_t)k_pt_jof_ftr_bytes;
  if (total_bytes > sizeof(s_jof_buf)) {
    return 0U;
  }
  (void)memset(s_jof_buf, 0, total_bytes);
  size_t pos = 0U;
  (void)memcpy(&s_jof_buf[pos], "JOF1", 4U);
  pos += 4U;
  internal_put_u16(s_jof_buf, &pos, (uint16_t)k_pt_jof_w);
  internal_put_u16(s_jof_buf, &pos, (uint16_t)k_pt_jof_h);
  internal_put_u16(s_jof_buf, &pos, (uint16_t)k_pt_jof_w);
  internal_put_u16(s_jof_buf, &pos, (uint16_t)k_pt_jof_h);
  s_jof_buf[pos++] = (uint8_t)k_pt_jof_bpp;
  s_jof_buf[pos++] = 0U;
  internal_put_u16(s_jof_buf, &pos, 0U);
  internal_put_u32(s_jof_buf, &pos, 1U);
  pos = (size_t)k_pt_jof_hdr_bytes;
  for (size_t i = 0U; i < payload_bytes; ++i) {
    s_jof_buf[pos++] = 0x24U;
  }
  internal_put_u32(s_jof_buf, &pos, (uint32_t)k_pt_jof_hdr_bytes);
  internal_put_u32(s_jof_buf, &pos, (uint32_t)payload_bytes);
  internal_put_u32(s_jof_buf, &pos, (uint32_t)idx_off);
  internal_put_u32(s_jof_buf, &pos, 1U);
  internal_put_u32(s_jof_buf, &pos, (uint32_t)total_bytes);
  (void)memcpy(&s_jof_buf[pos], "JOFE", 4U);
  return total_bytes;
}

/** @brief Construct an in-memory 4x4 24-bit BMP image in ::s_bmp_buf. */
RA8_INTERNAL static void internal_build_bmp(void)
{
  (void)memset(s_bmp_buf, 0, sizeof(s_bmp_buf));
  size_t pos    = 0U;
  s_bmp_buf[0U] = 'B';
  s_bmp_buf[1U] = 'M';
  pos           = 2U;
  internal_put_u32(s_bmp_buf, &pos, (uint32_t)k_pt_bmp_bytes);
  internal_put_u32(s_bmp_buf, &pos, 0U);
  internal_put_u32(s_bmp_buf, &pos, (uint32_t)k_pt_bmp_hdr_bytes);
  internal_put_u32(s_bmp_buf, &pos, (uint32_t)k_pt_bmp_info_hdr);
  internal_put_u32(s_bmp_buf, &pos, (uint32_t)k_pt_bmp_w);
  internal_put_u32(s_bmp_buf, &pos, (uint32_t)k_pt_bmp_h);
  internal_put_u16(s_bmp_buf, &pos, 1U);
  internal_put_u16(s_bmp_buf, &pos, (uint16_t)k_pt_bmp_bpp_bits);
  (void)memset(&s_bmp_buf[k_pt_bmp_hdr_bytes], 0x20, (size_t)k_pt_bmp_px_bytes);
}

/** @brief Append a stored ZIP local member record. */
RA8_INTERNAL static bool internal_zip_local(uint8_t*                buf,
                                            size_t                  cap,
                                            size_t*                 pos,
                                            const test_zip_entry_t* ent,
                                            uint32_t*               out_off,
                                            uint32_t*               out_crc)
{
  const uint32_t nlen = (uint32_t)strlen(ent->name);
  if ((*pos + 30U + (size_t)nlen + (size_t)ent->len) > cap) {
    return false;
  }
  const uint32_t crc = (uint32_t)mz_crc32(MZ_CRC32_INIT, ent->payload, ent->len);
  *out_off           = (uint32_t)*pos;
  *out_crc           = crc;
  internal_put_u32(buf, pos, 0x04034B50U);
  internal_put_u16(buf, pos, 20U);
  internal_put_u16(buf, pos, 0U);
  internal_put_u16(buf, pos, 0U);
  internal_put_u16(buf, pos, 0U);
  internal_put_u16(buf, pos, 0U);
  internal_put_u32(buf, pos, crc);
  internal_put_u32(buf, pos, ent->len);
  internal_put_u32(buf, pos, ent->len);
  internal_put_u16(buf, pos, (uint16_t)nlen);
  internal_put_u16(buf, pos, 0U);
  (void)memcpy(&buf[*pos], ent->name, nlen);
  *pos += nlen;
  (void)memcpy(&buf[*pos], ent->payload, ent->len);
  *pos += ent->len;
  return true;
}

/** @brief Append a stored ZIP central-directory record. */
RA8_INTERNAL static bool internal_zip_cd(uint8_t*                buf,
                                         size_t                  cap,
                                         size_t*                 pos,
                                         const test_zip_entry_t* ent,
                                         uint32_t                crc,
                                         uint32_t                off)
{
  const uint32_t nlen = (uint32_t)strlen(ent->name);
  if ((*pos + 46U + (size_t)nlen) > cap) {
    return false;
  }
  internal_put_u32(buf, pos, 0x02014B50U);
  internal_put_u16(buf, pos, 20U);
  internal_put_u16(buf, pos, 20U);
  internal_put_u16(buf, pos, 0U);
  internal_put_u16(buf, pos, 0U);
  internal_put_u16(buf, pos, 0U);
  internal_put_u16(buf, pos, 0U);
  internal_put_u32(buf, pos, crc);
  internal_put_u32(buf, pos, ent->len);
  internal_put_u32(buf, pos, ent->len);
  internal_put_u16(buf, pos, (uint16_t)nlen);
  internal_put_u16(buf, pos, 0U);
  internal_put_u16(buf, pos, 0U);
  internal_put_u16(buf, pos, 0U);
  internal_put_u16(buf, pos, 0U);
  internal_put_u32(buf, pos, 0U);
  internal_put_u32(buf, pos, off);
  (void)memcpy(&buf[*pos], ent->name, nlen);
  *pos += nlen;
  return true;
}

/** @brief Pack an array of stored entries into a complete ZIP buffer. */
RA8_INTERNAL static size_t
internal_pack_zip(uint8_t* buf, size_t cap, const test_zip_entry_t* entries, size_t count)
{
  uint32_t offs[4] = {};
  uint32_t crcs[4] = {};
  size_t   pos     = 0U;
  for (size_t i = 0U; i < count; ++i) {
    if (!internal_zip_local(buf, cap, &pos, &entries[i], &offs[i], &crcs[i])) {
      return 0U;
    }
  }
  const uint32_t cd_off = (uint32_t)pos;
  for (size_t i = 0U; i < count; ++i) {
    if (!internal_zip_cd(buf, cap, &pos, &entries[i], crcs[i], offs[i])) {
      return 0U;
    }
  }
  const uint32_t cd_len = (uint32_t)(pos - cd_off);
  if ((pos + 22U) > cap) {
    return 0U;
  }
  internal_put_u32(buf, &pos, 0x06054B50U);
  internal_put_u16(buf, &pos, 0U);
  internal_put_u16(buf, &pos, 0U);
  internal_put_u16(buf, &pos, (uint16_t)count);
  internal_put_u16(buf, &pos, (uint16_t)count);
  internal_put_u32(buf, &pos, cd_len);
  internal_put_u32(buf, &pos, cd_off);
  internal_put_u16(buf, &pos, 0U);
  return pos;
}

/** @brief Construct an in-memory one-page BMP CBZ archive. */
RA8_INTERNAL static size_t internal_build_cbz(void)
{
  internal_build_bmp();
  const test_zip_entry_t entries[1] = {{"01.bmp", s_bmp_buf, (uint32_t)k_pt_bmp_bytes}};
  return internal_pack_zip(s_cbz_buf, sizeof(s_cbz_buf), entries, 1U);
}

/** @brief Construct an in-memory EPUB archive. */
RA8_INTERNAL static size_t internal_build_epub(void)
{
  static const char k_mime[] = "application/epub+zip";
  static const char k_cnt[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<container version=\"1.0\" xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">\n"
    "<rootfiles><rootfile full-path=\"OEBPS/content.opf\" "
    "media-type=\"application/oebps-package+xml\"/></rootfiles></container>\n";
  static const char k_opf[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\" unique-identifier=\"id\">\n"
    "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">"
    "<dc:identifier id=\"id\">urn:uuid:test</dc:identifier>"
    "<dc:title>Portable EPUB</dc:title><dc:language>en</dc:language></metadata>\n"
    "<manifest><item id=\"c1\" href=\"ch1.xhtml\" media-type=\"application/xhtml+xml\"/>"
    "</manifest>\n"
    "<spine><itemref idref=\"c1\"/></spine></package>\n";
  static const char k_xhtml[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<html xmlns=\"http://www.w3.org/1999/xhtml\"><body>"
    "<h1>Portable</h1><p>Static arena EPUB reflow check.</p></body></html>\n";
  const test_zip_entry_t entries[4] = {
    {"mimetype", (const uint8_t*)k_mime, (uint32_t)(sizeof(k_mime) - 1U)},
    {"META-INF/container.xml", (const uint8_t*)k_cnt, (uint32_t)(sizeof(k_cnt) - 1U)},
    {"OEBPS/content.opf", (const uint8_t*)k_opf, (uint32_t)(sizeof(k_opf) - 1U)},
    {"OEBPS/ch1.xhtml", (const uint8_t*)k_xhtml, (uint32_t)(sizeof(k_xhtml) - 1U)},
  };
  return internal_pack_zip(s_epub_buf, sizeof(s_epub_buf), entries, 4U);
}

/** @brief Construct an in-memory compressed RBKC `.rabook` document. */
RA8_INTERNAL static size_t internal_build_rbkc(void)
{
  static const char k_strs[] = "Book\0Auth\0en\0urn:test\0ch1.xhtml\0body\0p\0Static RBKC page.";
  test_flat_book_t  blob     = {};
  (void)memcpy(blob.strings, k_strs, sizeof(k_strs));
  (void)memcpy(blob.hdr.magic, "RABOOK1\0", 8U);
  blob.hdr.format_version    = 1U;
  blob.hdr.total_size        = (uint32_t)sizeof(blob);
  blob.hdr.title_off         = 0U;
  blob.hdr.author_off        = 5U;
  blob.hdr.language_off      = 10U;
  blob.hdr.identifier_off    = 13U;
  blob.hdr.cover_image_index = (uint32_t)k_book_nil;
  blob.hdr.chapter_count     = 1U;
  blob.hdr.chapter_off       = (uint32_t)offsetof(test_flat_book_t, chapters);
  blob.hdr.node_count        = 3U;
  blob.hdr.node_off          = (uint32_t)offsetof(test_flat_book_t, nodes);
  blob.hdr.attr_off          = (uint32_t)offsetof(test_flat_book_t, strings);
  blob.hdr.stylesheet_off    = (uint32_t)offsetof(test_flat_book_t, strings);
  blob.hdr.image_off         = (uint32_t)offsetof(test_flat_book_t, strings);
  blob.hdr.string_off        = (uint32_t)offsetof(test_flat_book_t, strings);
  blob.hdr.string_size       = (uint32_t)sizeof(blob.strings);
  blob.hdr.image_pool_off    = (uint32_t)sizeof(blob);
  blob.chapters[0]           = (book_chapter_t){.title_off = 0U, .href_off = 22U, .root_node = 0U};
  blob.nodes[0]              = (book_node_t){.kind         = k_book_node_element,
                                             .name_off     = 32U,
                                             .first_attr   = k_book_nil,
                                             .first_child  = 1U,
                                             .next_sibling = k_book_nil};
  blob.nodes[1]              = (book_node_t){.kind         = k_book_node_element,
                                             .name_off     = 37U,
                                             .first_attr   = k_book_nil,
                                             .first_child  = 2U,
                                             .next_sibling = k_book_nil};
  blob.nodes[2]              = (book_node_t){.kind         = k_book_node_text,
                                             .text_off     = 39U,
                                             .first_attr   = k_book_nil,
                                             .first_child  = k_book_nil,
                                             .next_sibling = k_book_nil};
  const uint8_t* body        = (const uint8_t*)&blob + sizeof(book_header_t);
  blob.hdr.crc32_val =
    (uint32_t)mz_crc32(MZ_CRC32_INIT, body, sizeof(blob) - sizeof(book_header_t));

  if (tdefl_init(&s_tdefl, nullptr, nullptr, TDEFL_WRITE_ZLIB_HEADER | 256) != TDEFL_STATUS_OKAY) {
    return 0U;
  }
  size_t in_len  = sizeof(blob);
  size_t out_len = sizeof(s_comp_buf);
  if ((tdefl_compress(&s_tdefl, &blob, &in_len, s_comp_buf, &out_len, TDEFL_FINISH) !=
       TDEFL_STATUS_DONE) ||
      (in_len != sizeof(blob)) || ((40U + out_len) > sizeof(s_rbkc_buf))) {
    return 0U;
  }
  size_t pos = 0U;
  (void)memcpy(&s_rbkc_buf[pos], "RBKC", 4U);
  pos += 4U;
  internal_put_u32(s_rbkc_buf, &pos, (uint32_t)k_pt_chunk_bytes);
  internal_put_u64(s_rbkc_buf, &pos, (uint64_t)sizeof(blob));
  internal_put_u32(s_rbkc_buf, &pos, 1U);
  internal_put_u32(s_rbkc_buf, &pos, 0U);
  internal_put_u64(s_rbkc_buf, &pos, 0U);
  internal_put_u64(s_rbkc_buf, &pos, (uint64_t)out_len);
  (void)memcpy(&s_rbkc_buf[pos], s_comp_buf, out_len);
  return pos + out_len;
}

/** @brief Prove 1-byte-short workspace bind fails cleanly without touching memory. */
RA8_INTERNAL static bool internal_check_short_bind(const ra8_viewer_reader_requirements_t* need)
{
  if ((need->required_bytes == 0U) || (need->required_bytes > sizeof(s_reader_ws))) {
    return false;
  }
  (void)memset(s_reader_ws, (int)k_pt_sentinel, need->required_bytes);
  ra8_viewer_reader_t*          reader = (ra8_viewer_reader_t*)(uintptr_t)1U;
  ra8_viewer_workspace_report_t report = {};
  const ra8_err_t               error =
    ra8_viewer_reader_bind(&reader, s_reader_ws, need->required_bytes - 1U, need, &report);
  return (error == k_ra8_err_invalid_size) && (reader == nullptr) &&
         (report.required_bytes == need->required_bytes) &&
         (report.supplied_bytes == (need->required_bytes - 1U)) &&
         (s_reader_ws[0U] == (uint8_t)k_pt_sentinel) &&
         (s_reader_ws[need->required_bytes - 1U] == (uint8_t)k_pt_sentinel);
}

/** @brief Verify one document format strictly via static caller storage. */
RA8_INTERNAL static bool internal_verify_format(ra8_viewer_format_t format,
                                                const uint8_t*      data,
                                                size_t              len,
                                                bool                expect_bmp_container)
{
  if (len == 0U) {
    return false;
  }
  test_mem_source_t                src_state = {};
  const ra8_fmt_source_t           source    = internal_bind_mem_source(&src_state, data, len);
  ra8_viewer_reader_requirements_t need      = {};
  if ((ra8_viewer_reader_requirements_source(format, &source, &need) != k_ra8_ok) ||
      !internal_check_short_bind(&need)) {
    return false;
  }
  ra8_viewer_reader_t*          reader = nullptr;
  ra8_viewer_workspace_report_t report = {};
  if ((ra8_viewer_reader_bind(&reader, s_reader_ws, sizeof(s_reader_ws), &need, &report) !=
       k_ra8_ok) ||
      (ra8_viewer_open_source(reader, format, &source) != k_ra8_ok) ||
      (ra8_viewer_page_count(reader) == 0U) || (ra8_viewer_render_page(reader, 0U) != k_ra8_ok)) {
    ra8_viewer_close(reader);
    return false;
  }
  if (expect_bmp_container) {
    ra8_imgdec_name_t name = {};
    if ((ra8_viewer_page_container(reader, 0U, &name) != k_ra8_ok) ||
        (name.format != k_ra8_imgdec_format_bmp) || (strcmp(name.ext, "bmp") != 0) ||
        (strcmp(name.mime, "image/bmp") != 0)) {
      ra8_viewer_close(reader);
      return false;
    }
  }
  uint32_t                      width  = 0U;
  uint32_t                      height = 0U;
  uint16_t*                     pixels = nullptr;
  ra8_viewer_workspace_report_t trep   = {};
  const bool ok = (ra8_viewer_render_tile565(reader,
                                             0U,
                                             s_tile_ws,
                                             sizeof(s_tile_ws),
                                             &width,
                                             &height,
                                             &pixels,
                                             &trep) == k_ra8_ok) &&
                  (pixels == (uint16_t*)s_tile_ws) && (width > 0U) && (height > 0U) &&
                  internal_has_ink(pixels, (size_t)width * (size_t)height);
  ra8_viewer_close(reader);
  return ok && (ra8_viewer_page_count(reader) == 0U);
}

/** @brief Verify unsupported format, engine mismatch, read fault, and output sink. */
RA8_INTERNAL static bool internal_verify_guards(size_t jof_len)
{
  test_mem_source_t      src_state      = {};
  const ra8_fmt_source_t jof_src        = internal_bind_mem_source(&src_state, s_jof_buf, jof_len);
  ra8_viewer_reader_requirements_t need = {};
  if ((ra8_viewer_reader_requirements_source(k_ra8_viewer_format_comic_wrap, &jof_src, &need) !=
       k_ra8_err_not_supported) ||
      (ra8_viewer_classify_path("series.cbz") != k_ra8_viewer_format_comic) ||
      (ra8_viewer_classify_path(nullptr) != k_ra8_viewer_format_unknown)) {
    return false;
  }
  if (ra8_viewer_reader_requirements_source(k_ra8_viewer_format_jof, &jof_src, &need) != k_ra8_ok) {
    return false;
  }
  ra8_viewer_reader_t*          reader = nullptr;
  ra8_viewer_workspace_report_t report = {};
  if (ra8_viewer_reader_bind(&reader, s_reader_ws, sizeof(s_reader_ws), &need, &report) !=
      k_ra8_ok) {
    return false;
  }
  if (ra8_viewer_open_source(reader, k_ra8_viewer_format_comic, &jof_src) !=
      k_ra8_err_invalid_state) {
    ra8_viewer_close(reader);
    return false;
  }
  test_mem_source_t      fault_state = {};
  const ra8_fmt_source_t fault_src   = {.read_at = internal_mem_read,
                                        .ctx     = &fault_state,
                                        .size    = (uint64_t)jof_len};
  fault_state                        = (test_mem_source_t){
    .data         = s_jof_buf,
    .len          = jof_len,
    .max_step     = (size_t)k_pt_short_step,
    .calls        = 0U,
    .fail_on_call = 1U,
  };
  if (ra8_viewer_open_source(reader, k_ra8_viewer_format_jof, &fault_src) != k_ra8_fail) {
    ra8_viewer_close(reader);
    return false;
  }
  ra8_viewer_close(reader);

  static uint8_t            s_msg[k_pt_report_cap];
  ra8_io_stream_t           out   = {};
  ra8_io_stream_ram_state_t state = {};
  uint32_t                  used  = 0U;
  return (ra8_io_stream_ram_init(&out, &state, s_msg, sizeof(s_msg)) == k_ra8_ok) &&
         (priv_viewer_output_opened(&out, "mem.jof", 1U) == k_ra8_ok) &&
         (ra8_io_stream_ram_used(&state, &used) == k_ra8_ok) && (used > 0U);
}

/** @brief Entry point: exercise all four formats strictly over static arenas. */
int main(void)
{
  const size_t jof_len  = internal_build_jof();
  const size_t cbz_len  = internal_build_cbz();
  const size_t epub_len = internal_build_epub();
  const size_t rbk_len  = internal_build_rbkc();
  const bool ok = internal_verify_format(k_ra8_viewer_format_jof, s_jof_buf, jof_len, false) &&
                  internal_verify_format(k_ra8_viewer_format_comic, s_cbz_buf, cbz_len, true) &&
                  internal_verify_format(k_ra8_viewer_format_epub, s_epub_buf, epub_len, false) &&
                  internal_verify_format(k_ra8_viewer_format_rabook, s_rbkc_buf, rbk_len, false) &&
                  internal_verify_guards(jof_len);
  const size_t stbtt_hw = ra8_stbtt_alloc_high_water();
  return (ok && (stbtt_hw > 0U) && (stbtt_hw <= (96U * 1024U))) ? 0 : 1;
}
