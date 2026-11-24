/**
 * @file test_viewer_reflow_workspace.c
 * @brief Caller-workspace and headless pixel tests for EPUB and RABOOK reflow.
 * @details Synthesizes minimal valid `.epub` and `.rabook` fixtures in a
 * bounded scratch directory without dynamic allocation, proves workspace
 * shortfall reporting, verifies page and tile RGB565 rendering, checks
 * corrupted-input refusal, and checks clean reader close state.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <fcntl.h>
#include <stdalign.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "book.h"
#include "miniz.h"
#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_viewer_reader.h"

/** @brief Fixed test capacities and synthetic fixture sizes. */
typedef enum : uint32_t {
  k_test_reader_bytes = 18U * 1024U * 1024U,
  k_test_tile_bytes   = (uint32_t)k_ra8_viewer_fb_width * (uint32_t)k_ra8_viewer_fb_height * 2U,
  k_test_white_rgb565 = 0xFFFFU,
  k_test_zip_cap      = 8192U,
  k_test_book_cap     = 4096U,
  k_test_rbkc_cap     = 4096U,
  k_test_path_cap     = 256U,
  k_test_chunk_bytes  = 65536U,
  k_test_file_mode    = 0600U,
} viewer_reflow_test_budget_t;

/** @brief Reader workspace backing. */
alignas(max_align_t) static uint8_t s_reader[k_test_reader_bytes];
/** @brief Caller tile pixel backing. */
alignas(max_align_t) static uint8_t s_tile[k_test_tile_bytes];

/** @brief Check whether any rendered pixel differs from pure white. */
RA8_INTERNAL static bool internal_has_content(const uint16_t* pixels, size_t count)
{
  for (size_t idx = 0U; idx < count; ++idx) {
    if (pixels[idx] != (uint16_t)k_test_white_rgb565) {
      return true;
    }
  }
  return false;
}

/** @brief Write exact byte span to @p path. */
RA8_INTERNAL static bool internal_write_file(const char* path, const uint8_t* data, size_t len)
{
  const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, (mode_t)k_test_file_mode);
  if (fd < 0) {
    return false;
  }
  size_t off = 0U;
  while (off < len) {
    const ssize_t wrote = write(fd, &data[off], len - off);
    if (wrote <= 0) {
      (void)close(fd);
      return false;
    }
    off += (size_t)wrote;
  }
  return close(fd) == 0;
}

/** @brief Append 16-bit little-endian integer. */
RA8_INTERNAL static void internal_put_u16(uint8_t* dst, size_t* pos, uint16_t v)
{
  dst[(*pos)++] = (uint8_t)(v & 0xFFU);
  dst[(*pos)++] = (uint8_t)((v >> 8U) & 0xFFU);
}

/** @brief Append 32-bit little-endian integer. */
RA8_INTERNAL static void internal_put_u32(uint8_t* dst, size_t* pos, uint32_t v)
{
  for (uint32_t i = 0U; i < 4U; ++i) {
    dst[(*pos)++] = (uint8_t)((v >> (i * 8U)) & 0xFFU);
  }
}

/** @brief Append 64-bit little-endian integer. */
RA8_INTERNAL static void internal_put_u64(uint8_t* dst, size_t* pos, uint64_t v)
{
  for (uint32_t i = 0U; i < 8U; ++i) {
    dst[(*pos)++] = (uint8_t)((v >> (i * 8U)) & 0xFFU);
  }
}

/** @brief One uncompressed ZIP archive member. */
typedef struct {
  const char* name;    /**< Member name.       */
  const char* payload; /**< UTF-8 member body. */
} test_zip_entry_t;

/** @brief Append a local file header and stored payload; record its offset. */
RA8_INTERNAL static bool internal_append_zip_local(uint8_t*                buf,
                                                   size_t                  cap,
                                                   size_t*                 pos,
                                                   const test_zip_entry_t* ent,
                                                   uint32_t*               out_off,
                                                   uint32_t*               out_crc,
                                                   uint32_t*               out_len)
{
  const uint32_t nlen = (uint32_t)strlen(ent->name);
  const uint32_t dlen = (uint32_t)strlen(ent->payload);
  if ((*pos + 30U + nlen + dlen) > cap) {
    return false;
  }
  const uint32_t crc = (uint32_t)mz_crc32(MZ_CRC32_INIT, (const uint8_t*)ent->payload, dlen);
  *out_off           = (uint32_t)*pos;
  *out_crc           = crc;
  *out_len           = dlen;
  internal_put_u32(buf, pos, 0x04034B50U);
  internal_put_u16(buf, pos, 20U);
  internal_put_u16(buf, pos, 0U);
  internal_put_u16(buf, pos, 0U);
  internal_put_u16(buf, pos, 0U);
  internal_put_u16(buf, pos, 0U);
  internal_put_u32(buf, pos, crc);
  internal_put_u32(buf, pos, dlen);
  internal_put_u32(buf, pos, dlen);
  internal_put_u16(buf, pos, (uint16_t)nlen);
  internal_put_u16(buf, pos, 0U);
  (void)memcpy(&buf[*pos], ent->name, nlen);
  *pos += nlen;
  (void)memcpy(&buf[*pos], ent->payload, dlen);
  *pos += dlen;
  return true;
}

/** @brief Append a central-directory record for a stored ZIP member. */
RA8_INTERNAL static bool internal_append_zip_cd(uint8_t*    buf,
                                                size_t      cap,
                                                size_t*     pos,
                                                const char* name,
                                                uint32_t    crc,
                                                uint32_t    len,
                                                uint32_t    off)
{
  const uint32_t nlen = (uint32_t)strlen(name);
  if ((*pos + 46U + nlen) > cap) {
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
  internal_put_u32(buf, pos, len);
  internal_put_u32(buf, pos, len);
  internal_put_u16(buf, pos, (uint16_t)nlen);
  internal_put_u16(buf, pos, 0U);
  internal_put_u16(buf, pos, 0U);
  internal_put_u16(buf, pos, 0U);
  internal_put_u16(buf, pos, 0U);
  internal_put_u32(buf, pos, 0U);
  internal_put_u32(buf, pos, off);
  (void)memcpy(&buf[*pos], name, nlen);
  *pos += nlen;
  return true;
}

/** @brief Write a minimal stored EPUB ZIP file to @p path. */
RA8_INTERNAL static bool internal_build_epub_file(const char* path)
{
  static const test_zip_entry_t k_entries[4] = {
    {"mimetype", "application/epub+zip"},
    {"META-INF/container.xml",
     "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
     "<container version=\"1.0\" xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">\n"
     "<rootfiles><rootfile full-path=\"OEBPS/content.opf\" "
     "media-type=\"application/oebps-package+xml\"/></rootfiles></container>\n"},
    {"OEBPS/content.opf",
     "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
     "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\" unique-identifier=\"id\">\n"
     "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">"
     "<dc:identifier id=\"id\">urn:uuid:test</dc:identifier>"
     "<dc:title>Test EPUB</dc:title><dc:language>en</dc:language></metadata>\n"
     "<manifest><item id=\"c1\" href=\"ch1.xhtml\" media-type=\"application/xhtml+xml\"/>"
     "</manifest>\n"
     "<spine><itemref idref=\"c1\"/></spine></package>\n"},
    {"OEBPS/ch1.xhtml",
     "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
     "<html xmlns=\"http://www.w3.org/1999/xhtml\"><body>"
     "<h1>Chapter 1</h1><p>EPUB test paragraph for viewer.</p></body></html>\n"},
  };
  static uint8_t s_zip[k_test_zip_cap];
  uint32_t       offs[4] = {};
  uint32_t       crcs[4] = {};
  uint32_t       lens[4] = {};
  size_t         pos     = 0U;
  for (size_t i = 0U; i < 4U; ++i) {
    if (!internal_append_zip_local(s_zip,
                                   sizeof(s_zip),
                                   &pos,
                                   &k_entries[i],
                                   &offs[i],
                                   &crcs[i],
                                   &lens[i])) {
      return false;
    }
  }
  const uint32_t cd_off = (uint32_t)pos;
  for (size_t i = 0U; i < 4U; ++i) {
    if (!internal_append_zip_cd(s_zip,
                                sizeof(s_zip),
                                &pos,
                                k_entries[i].name,
                                crcs[i],
                                lens[i],
                                offs[i])) {
      return false;
    }
  }
  const uint32_t cd_len = (uint32_t)(pos - cd_off);
  if ((pos + 22U) > sizeof(s_zip)) {
    return false;
  }
  internal_put_u32(s_zip, &pos, 0x06054B50U);
  internal_put_u16(s_zip, &pos, 0U);
  internal_put_u16(s_zip, &pos, 0U);
  internal_put_u16(s_zip, &pos, 4U);
  internal_put_u16(s_zip, &pos, 4U);
  internal_put_u32(s_zip, &pos, cd_len);
  internal_put_u32(s_zip, &pos, cd_off);
  internal_put_u16(s_zip, &pos, 0U);
  return internal_write_file(path, s_zip, pos);
}

/** @brief Packed two-node synthetic flat RABOOK1 test fixture layout. */
typedef struct {
  book_header_t  hdr;         /**< Fixed 100-byte header.  */
  book_chapter_t chapters[1]; /**< Single spine chapter.   */
  book_node_t    nodes[3];    /**< body -> p -> text DOM.  */
  char           strings[64]; /**< NUL-terminated strings. */
} test_flat_book_t;

/** @brief Static miniz DEFLATE compressor state for zero-malloc test packing. */
static tdefl_compressor s_test_tdefl;

/** @brief Write a minimal compressed RBKC `.rabook` file to @p path. */
RA8_INTERNAL static bool internal_build_rabook_file(const char* path)
{
  static const char k_strs[] = "Test\0Author\0en\0urn:test\0ch1.xhtml\0body\0p\0RABOOK test page.";
  test_flat_book_t  blob     = {};
  (void)memcpy(blob.strings, k_strs, sizeof(k_strs));
  (void)memcpy(blob.hdr.magic, "RABOOK1\0", 8U);
  blob.hdr.format_version    = 1U;
  blob.hdr.total_size        = (uint32_t)sizeof(blob);
  blob.hdr.title_off         = 0U;
  blob.hdr.author_off        = 5U;
  blob.hdr.language_off      = 12U;
  blob.hdr.identifier_off    = 15U;
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
  blob.chapters[0]           = (book_chapter_t){.title_off = 0U, .href_off = 24U, .root_node = 0U};
  blob.nodes[0]              = (book_node_t){.kind         = k_book_node_element,
                                             .name_off     = 34U,
                                             .first_attr   = k_book_nil,
                                             .first_child  = 1U,
                                             .next_sibling = k_book_nil};
  blob.nodes[1]              = (book_node_t){.kind         = k_book_node_element,
                                             .name_off     = 39U,
                                             .first_attr   = k_book_nil,
                                             .first_child  = 2U,
                                             .next_sibling = k_book_nil};
  blob.nodes[2]              = (book_node_t){.kind         = k_book_node_text,
                                             .text_off     = 41U,
                                             .first_attr   = k_book_nil,
                                             .first_child  = k_book_nil,
                                             .next_sibling = k_book_nil};
  const uint8_t* body        = (const uint8_t*)&blob + sizeof(book_header_t);
  blob.hdr.crc32_val =
    (uint32_t)mz_crc32(MZ_CRC32_INIT, body, sizeof(blob) - sizeof(book_header_t));

  static uint8_t s_comp[k_test_book_cap];
  if (tdefl_init(&s_test_tdefl, nullptr, nullptr, TDEFL_WRITE_ZLIB_HEADER | 256) !=
      TDEFL_STATUS_OKAY) {
    return false;
  }
  size_t in_len  = sizeof(blob);
  size_t out_len = sizeof(s_comp);
  if ((tdefl_compress(&s_test_tdefl, &blob, &in_len, s_comp, &out_len, TDEFL_FINISH) !=
       TDEFL_STATUS_DONE) ||
      (in_len != sizeof(blob))) {
    return false;
  }
  static uint8_t s_rbkc[k_test_rbkc_cap];
  size_t         pos = 0U;
  (void)memcpy(&s_rbkc[pos], "RBKC", 4U);
  pos += 4U;
  internal_put_u32(s_rbkc, &pos, (uint32_t)k_test_chunk_bytes);
  internal_put_u64(s_rbkc, &pos, (uint64_t)sizeof(blob));
  internal_put_u32(s_rbkc, &pos, 1U);
  internal_put_u32(s_rbkc, &pos, 0U);
  internal_put_u64(s_rbkc, &pos, 0U);
  internal_put_u64(s_rbkc, &pos, (uint64_t)out_len);
  (void)memcpy(&s_rbkc[pos], s_comp, out_len);
  pos += out_len;
  return internal_write_file(path, s_rbkc, pos);
}

/** @brief Verify 1-byte-short workspace shortfall refusal. */
RA8_INTERNAL static bool internal_check_short_bind(const ra8_viewer_reader_requirements_t* need)
{
  if ((need->required_bytes == 0U) || (need->required_bytes > sizeof(s_reader))) {
    return false;
  }
  ra8_viewer_reader_t*          reader = (ra8_viewer_reader_t*)(uintptr_t)1U;
  ra8_viewer_workspace_report_t report = {};
  const ra8_err_t               error =
    ra8_viewer_reader_bind(&reader, s_reader, need->required_bytes - 1U, need, &report);
  return (error == k_ra8_err_invalid_size) && (reader == nullptr) &&
         (report.required_bytes == need->required_bytes) &&
         (report.supplied_bytes == (need->required_bytes - 1U));
}

/** @brief Bind, open, render page/tile, verify non-white ink, and close @p path. */
RA8_INTERNAL static bool internal_verify_document(const char* path)
{
  ra8_viewer_reader_requirements_t need = {};
  if ((ra8_viewer_reader_requirements(path, &need) != k_ra8_ok) ||
      (need.engine != (uint32_t)k_ra8_viewer_engine_reflow) || !internal_check_short_bind(&need)) {
    return false;
  }
  ra8_viewer_reader_t*          reader = nullptr;
  ra8_viewer_workspace_report_t report = {};
  if ((ra8_viewer_reader_bind(&reader, s_reader, sizeof(s_reader), &need, &report) != k_ra8_ok) ||
      (ra8_viewer_open(reader, path) != k_ra8_ok) || (ra8_viewer_page_count(reader) == 0U) ||
      (ra8_viewer_render_page(reader, 0U) != k_ra8_ok)) {
    ra8_viewer_close(reader);
    return false;
  }
  size_t required  = 0U;
  size_t alignment = 0U;
  if ((ra8_viewer_tile_requirements(reader, 0U, &required, &alignment) != k_ra8_ok) ||
      (required > sizeof(s_tile)) || (alignment > alignof(max_align_t))) {
    ra8_viewer_close(reader);
    return false;
  }
  uint32_t                      width       = 0U;
  uint32_t                      height      = 0U;
  uint16_t*                     pixels      = nullptr;
  ra8_viewer_workspace_report_t tile_report = {};
  const bool rendered = (ra8_viewer_render_tile565(reader,
                                                   0U,
                                                   s_tile,
                                                   sizeof(s_tile),
                                                   &width,
                                                   &height,
                                                   &pixels,
                                                   &tile_report) == k_ra8_ok) &&
                        (pixels == (uint16_t*)s_tile) &&
                        (width == (uint32_t)k_ra8_viewer_fb_width) &&
                        (height == (uint32_t)k_ra8_viewer_fb_height) &&
                        internal_has_content(pixels, (size_t)width * (size_t)height);
  ra8_viewer_close(reader);
  return rendered && (ra8_viewer_page_count(reader) == 0U);
}

/** @brief Verify malformed document fails open without crashing or claiming pages. */
RA8_INTERNAL static bool internal_verify_refusal(const char* path)
{
  ra8_viewer_reader_requirements_t need = {};
  if ((ra8_viewer_reader_requirements(path, &need) != k_ra8_ok) ||
      (need.required_bytes > sizeof(s_reader))) {
    return false;
  }
  ra8_viewer_reader_t*          reader = nullptr;
  ra8_viewer_workspace_report_t report = {};
  if (ra8_viewer_reader_bind(&reader, s_reader, sizeof(s_reader), &need, &report) != k_ra8_ok) {
    return false;
  }
  const ra8_err_t open_err = ra8_viewer_open(reader, path);
  ra8_viewer_close(reader);
  return (open_err != k_ra8_ok) && (ra8_viewer_page_count(reader) == 0U);
}

/** @brief Program entry point: generate EPUB/RABOOK test files and run checks. */
int main(void)
{
  char dir_tpl[] = "/tmp/ra8_viewer_reflow_XXXXXX";
  if (mkdtemp(dir_tpl) == nullptr) {
    return 1;
  }
  char epub_path[k_test_path_cap];
  char rabook_path[k_test_path_cap];
  char bad_epub[k_test_path_cap];
  char bad_rabook[k_test_path_cap];
  (void)snprintf(epub_path, sizeof(epub_path), "%s/sample.epub", dir_tpl);
  (void)snprintf(rabook_path, sizeof(rabook_path), "%s/sample.rabook", dir_tpl);
  (void)snprintf(bad_epub, sizeof(bad_epub), "%s/bad.epub", dir_tpl);
  (void)snprintf(bad_rabook, sizeof(bad_rabook), "%s/bad.rabook", dir_tpl);

  static const uint8_t k_bad_bytes[12] = {'B', 'A', 'D', '_', 'D', 'A', 'T', 'A', 0U, 1U, 2U, 3U};
  bool ok = internal_build_epub_file(epub_path) && internal_build_rabook_file(rabook_path) &&
            internal_write_file(bad_epub, k_bad_bytes, sizeof(k_bad_bytes)) &&
            internal_write_file(bad_rabook, k_bad_bytes, sizeof(k_bad_bytes));
  ok      = ok && internal_verify_document(epub_path) && internal_verify_document(rabook_path) &&
            internal_verify_refusal(bad_epub) && internal_verify_refusal(bad_rabook);

  (void)unlink(epub_path);
  (void)unlink(rabook_path);
  (void)unlink(bad_epub);
  (void)unlink(bad_rabook);
  (void)rmdir(dir_tpl);
  return ok ? 0 : 1;
}
