/**
 * @file test_epub_open_extract_cov.c
 * @brief Corrupt-stream extraction paths in epub_open.c and epub_chapter.c.
 *
 * @details
 * Split out of `test_epub_open_cov.c` to keep each test translation unit under
 * the repository file-size cap. That sibling drives the four guards AHEAD of
 * the extraction -- entry missing, stat, zip-bomb, size cap. This file owns
 * the one after them: an archive whose central directory is perfectly valid
 * and whose stored payload is not, so `mz_zip_reader_extract_to_mem` is the
 * first and only step that can fail. The second case opens a valid EPUB, then
 * corrupts its embedded-font payload to exercise the same boundary in the
 * chapter/resource extraction path. A third case supplies incomplete extended
 * ZIP metadata that name lookup can skip but the following file-stat call must
 * reject.
 *
 * The ZIP builder fixture is a private copy rather than a shared header,
 * matching the pattern the other split suites here use: everything has
 * internal linkage, so each executable owns its own scratch archive.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "epub.h"
#include "miniz.h"
#include "ra8_attributes.h"
#include "ra8_err.h"
#include "unity_minimal.h"

/**
 * @enum ext_cov_sizes_t
 * @brief Scratch capacity for the single archive this file builds.
 */
typedef enum : uint32_t {
  k_ext_zip_buf    = 65536U, /**< Max in-memory ZIP buffer.               */
  k_ext_font_bytes = 16U,    /**< Synthetic embedded-font payload length. */
} ext_cov_sizes_t;

/**
 * @enum ext_lfh_layout_t
 * @brief ZIP local-file-header fields the payload-corruption helper walks.
 *
 * @details
 * Damaging an entry's stored bytes while leaving its central-directory record
 * intact means walking past the local header: a fixed 30-byte part, then the
 * file name and the extra field, whose lengths are little-endian uint16s
 * inside that fixed part (APPNOTE.TXT 4.3.7).
 *
 * @invariant Every offset is relative to the entry's local header start,
 *            which only the central directory can supply.
 *
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_ext_lfh_size    = 30U,   /**< Fixed part of a local file header. */
  k_ext_fname_ofs   = 26U,   /**< uint16 LE file-name length.        */
  k_ext_extra_ofs   = 28U,   /**< uint16 LE extra-field length.      */
  k_ext_le_lo       = 0U,    /**< Little-endian low byte.            */
  k_ext_le_hi       = 1U,    /**< Little-endian high byte.           */
  k_ext_le_shift    = 8U,    /**< Shift applied to the high byte.    */
  k_ext_corrupt_xor = 0xFFU, /**< XOR mask; guarantees a change.     */
} ext_lfh_layout_t;

/**
 * @enum ext_cdh_layout_t
 * @brief Central-directory fields used by the incomplete-metadata fixture.
 */
typedef enum : uint8_t {
  k_ext_cdh_uncomp_size_ofs = 24U, /**< uint32 LE uncompressed-size field. */
  k_ext_le_u32_bytes        = 4U,  /**< Bytes in a little-endian uint32.   */
  k_ext_le_byte_shift       = 8U,  /**< Bits contributed by each byte.     */
} ext_cdh_layout_t;

/**
 * @enum ext_zip64_layout_t
 * @brief Extended-size metadata lengths used by the malformed fixture.
 */
typedef enum : uint8_t {
  k_ext_zip64_complete_bytes = 12U, /**< Header plus one uint64 value. */
  k_ext_zip64_short_bytes    = 8U,  /**< Header plus only four bytes.  */
} ext_zip64_layout_t;

/**
 * @var s_zip_buf
 * @brief Scratch buffer holding the archive this file builds.
 * @note Single-threaded; not thread-safe.
 * @since 0.1.0
 */
static uint8_t s_zip_buf[k_ext_zip_buf];

/**
 * @var s_zip_size
 * @brief Finalised length of the archive in ::s_zip_buf.
 * @since 0.1.0
 */
static size_t s_zip_size = 0U;

/**
 * @var s_container_good
 * @brief A well-formed container.xml naming OEBPS/content.opf.
 * @details Its CONTENT never matters here -- the point is that it locates,
 *          stats and passes the size guard, so only its extraction can fail.
 * @since 0.1.0
 */
static const char* const s_container_good =
  "<?xml version=\"1.0\"?>\n"
  "<container version=\"1.0\""
  " xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">\n"
  "  <rootfiles>\n"
  "    <rootfile full-path=\"OEBPS/content.opf\""
  " media-type=\"application/oebps-package+xml\"/>\n"
  "  </rootfiles>\n"
  "</container>\n";

/** @brief Minimal package document declaring one embedded font. */
static const char* const s_opf_with_font =
  "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
  "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\">\n"
  "  <metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">\n"
  "    <dc:title>Damaged font fixture</dc:title>\n"
  "  </metadata>\n"
  "  <manifest>\n"
  "    <item id=\"font\" href=\"font.ttf\" media-type=\"font/ttf\"/>\n"
  "  </manifest>\n"
  "  <spine/>\n"
  "</package>\n";

/** @brief Distinctive stored font bytes; contents need not form a valid TTF. */
static const uint8_t s_font_payload[k_ext_font_bytes] = {
  0x00U,
  0x01U,
  0x00U,
  0x00U,
  0xDEU,
  0xADU,
  0xBEU,
  0xEFU,
  0xCAU,
  0xFEU,
  0xBAU,
  0xBEU,
  0x12U,
  0x34U,
  0x56U,
  0x78U,
};

/**
 * @brief Finalise a heap-backed miniz archive into s_zip_buf / s_zip_size.
 *
 * @param[in] zip Initialised writer (not yet finalised).
 *
 * @pre @p zip was opened with mz_zip_writer_init_heap.
 * @pre s_zip_buf is writable for k_ext_zip_buf bytes.
 * @post s_zip_buf[0..s_zip_size-1] holds the final ZIP.
 * @post s_zip_size is greater than zero.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_finalise(mz_zip_archive* zip)
{
  void*  heap_buf  = nullptr;
  size_t heap_size = 0U;
  TEST_ASSERT(mz_zip_writer_finalize_heap_archive(zip, &heap_buf, &heap_size) == MZ_TRUE);
  TEST_ASSERT(heap_buf != nullptr);
  TEST_ASSERT(heap_size > 0U);
  TEST_ASSERT(heap_size <= (size_t)k_ext_zip_buf);
  (void)memcpy(s_zip_buf, heap_buf, heap_size);
  s_zip_size = heap_size;
  mz_free(heap_buf);
  mz_zip_writer_end(zip);
}

/**
 * @brief Corrupt one payload byte of an entry in ::s_zip_buf.
 *
 * @details
 * Locates the entry through the central directory (so the CD record stays
 * exactly as the writer produced it), walks past its local header, and XORs
 * the first payload byte. The result is an archive that locates, stats and
 * passes the zip-bomb guard, and whose extraction is the first step that can
 * possibly notice anything is wrong. Calling it twice repairs the byte.
 *
 * @param[in] name Archive entry name to damage.
 *
 * @pre ::internal_finalise has run, so s_zip_buf / s_zip_size are valid.
 * @pre @p name exists in the archive (asserted).
 * @post Exactly one payload byte of that entry is flipped.
 * @post The central directory and every other entry are untouched.
 *
 * @note Not thread-safe; mutates file-static fixture state.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_corrupt_entry_payload(const char* name)
{
  mz_zip_archive zip;
  (void)memset(&zip, 0, sizeof(zip));
  TEST_ASSERT(mz_zip_reader_init_mem(&zip, s_zip_buf, s_zip_size, 0U) == MZ_TRUE);
  const int32_t idx = mz_zip_reader_locate_file(&zip, name, nullptr, 0U);
  TEST_ASSERT(idx >= 0);
  mz_zip_archive_file_stat st;
  TEST_ASSERT(mz_zip_reader_file_stat(&zip, (mz_uint)idx, &st) == MZ_TRUE);
  (void)mz_zip_reader_end(&zip);

  const size_t lho   = (size_t)st.m_local_header_ofs;
  const size_t fname = (size_t)s_zip_buf[lho + (size_t)k_ext_fname_ofs + (size_t)k_ext_le_lo] |
                       ((size_t)s_zip_buf[lho + (size_t)k_ext_fname_ofs + (size_t)k_ext_le_hi]
                        << (size_t)k_ext_le_shift);
  const size_t extra = (size_t)s_zip_buf[lho + (size_t)k_ext_extra_ofs + (size_t)k_ext_le_lo] |
                       ((size_t)s_zip_buf[lho + (size_t)k_ext_extra_ofs + (size_t)k_ext_le_hi]
                        << (size_t)k_ext_le_shift);
  const size_t data  = lho + (size_t)k_ext_lfh_size + fname + extra;
  TEST_ASSERT(data < s_zip_size);
  s_zip_buf[data] ^= (uint8_t)k_ext_corrupt_xor;
}

/**
 * @brief Add a stored archive entry with caller-supplied central-only metadata.
 * @param[in,out] zip Writer receiving the entry.
 * @param[in] name Archive entry name.
 * @param[in] payload Entry bytes.
 * @param[in] payload_size Number of entry bytes.
 * @param[in] extra Central-directory extra-field bytes.
 * @param[in] extra_size Number of extra-field bytes.
 * @pre @p zip is an active heap writer.
 * @pre All pointer/size pairs identify readable storage.
 * @post The entry and its central-only metadata are owned by the writer.
 * @post The writer remains active.
 * @note Single-threaded fixture helper.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_add_with_central_extra(mz_zip_archive* zip,
                                            const char*     name,
                                            const void*     payload,
                                            size_t          payload_size,
                                            const uint8_t*  extra,
                                            mz_uint         extra_size)
{
  TEST_ASSERT(mz_zip_writer_add_mem_ex_v2(zip,
                                          name,
                                          payload,
                                          payload_size,
                                          nullptr,
                                          0U,
                                          MZ_NO_COMPRESSION,
                                          0U,
                                          0U,
                                          nullptr,
                                          nullptr,
                                          0U,
                                          (const char*)extra,
                                          extra_size) == MZ_TRUE);
}

/**
 * @brief Resolve one entry's central-directory record in ::s_zip_buf.
 * @param[in] name Existing archive entry name.
 * @return Byte offset of the entry's fixed central-directory header.
 * @pre ::internal_finalise has published a valid archive.
 * @pre @p name is present and has valid metadata before mutation.
 * @post The archive bytes are unchanged.
 * @post All temporary miniz reader state is released.
 * @note Single-threaded fixture helper.
 * @since 0.1.0
 */
RA8_INTERNAL static size_t internal_central_record_offset(const char* name)
{
  mz_zip_archive reader = {};
  TEST_ASSERT(mz_zip_reader_init_mem(&reader, s_zip_buf, s_zip_size, 0U) == MZ_TRUE);
  const int32_t index = mz_zip_reader_locate_file(&reader, name, nullptr, 0U);
  TEST_ASSERT(index >= 0);
  mz_zip_archive_file_stat stat = {};
  TEST_ASSERT(mz_zip_reader_file_stat(&reader, (mz_uint)index, &stat) == MZ_TRUE);
  const size_t offset =
    (size_t)reader.m_central_directory_file_ofs + (size_t)stat.m_central_dir_ofs;
  TEST_ASSERT((offset + (size_t)k_ext_cdh_uncomp_size_ofs + (size_t)k_ext_le_u32_bytes) <=
              s_zip_size);
  TEST_ASSERT(mz_zip_reader_end(&reader) == MZ_TRUE);
  return offset;
}

/**
 * @brief Write one little-endian uint32 into a central-directory record.
 * @param[in] record Offset returned by ::internal_central_record_offset.
 * @param[in] value Value to encode in the uncompressed-size field.
 * @pre @p record names a complete central-directory fixed header.
 * @pre ::s_zip_buf is writable.
 * @post Exactly four uncompressed-size bytes are replaced.
 * @post Archive length and every other byte are unchanged.
 * @note Single-threaded fixture helper.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_set_central_uncompressed_size(size_t record, uint32_t value)
{
  const size_t field = record + (size_t)k_ext_cdh_uncomp_size_ofs;
  for (uint8_t byte = 0U; byte < (uint8_t)k_ext_le_u32_bytes; ++byte) {
    const uint32_t shift    = (uint32_t)byte * (uint32_t)k_ext_le_byte_shift;
    s_zip_buf[field + byte] = (uint8_t)(value >> shift);
  }
}

/**
 * @test internal_test_open_corrupt_container_stream
 * @brief container.xml whose payload bytes are damaged -- with a perfectly
 *        valid central-directory record -- makes `internal_extract` report
 *        k_ra8_err_validation_failed from its extraction step.
 *
 * @details
 * Everything before the extraction succeeds by construction: the entry is
 * located, `mz_zip_reader_file_stat` reads the untouched CD record, the
 * zip-bomb guard sees consistent declared sizes, and the declared
 * uncompressed size is well under the 4096-byte scratch. Only
 * `mz_zip_reader_extract_to_mem` can fail here, and it does -- either the
 * deflate stream no longer decodes or the recomputed CRC32 disagrees with
 * the central directory. That makes this the only construction that reaches
 * the extraction rejection rather than one of the four guards ahead of it.
 *
 * @par MC/DC:
 * Decision: `if (mz_zip_reader_extract_to_mem(...) == MZ_FALSE)` in
 * apps/shared_libs/epub/src/epub_open.c@internal_extract (1 condition).
 * - V1: intact payload -> extraction succeeds -> false -> `*got` is set and
 *   open continues, failing later on the missing OPF (the repaired re-open at
 *   the end of this case).
 * - V2: one payload byte flipped -> extraction fails -> true -> internal_extract
 *   returns k_ra8_err_validation_failed and epub_open propagates it.
 * N = 1 condition, N+1 = 2 vectors. The two archives differ in exactly one
 * byte, and that byte is not read by any earlier guard, so the extraction
 * outcome independently affects the decision. The two different error codes
 * are what prove the failure moved past the extraction rather than merely
 * changing shape.
 *
 * @pre s_zip_buf is writable; the miniz writer is available.
 * @pre The container document is small enough to pass the size guard.
 * @post epub_open reported k_ra8_err_validation_failed for the damaged image.
 * @post No book state was published in either direction.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_open_corrupt_container_stream(void)
{
  TEST_BEGIN("epub_open: damaged container.xml payload -> validation_failed");

  mz_zip_archive zip;
  (void)memset(&zip, 0, sizeof(zip));
  TEST_ASSERT(mz_zip_writer_init_heap(&zip, 0U, (size_t)k_ext_zip_buf) == MZ_TRUE);
  TEST_ASSERT(mz_zip_writer_add_mem(&zip,
                                    "META-INF/container.xml",
                                    (const void*)s_container_good,
                                    strlen(s_container_good),
                                    MZ_DEFAULT_COMPRESSION) == MZ_TRUE);
  internal_finalise(&zip);

  internal_corrupt_entry_payload("META-INF/container.xml");

  epub_book_t            book  = {};
  const epub_mem_media_t media = {.data = s_zip_buf, .size = s_zip_size};
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, epub_open(&media, nullptr, &book));
  TEST_ASSERT_EQ(0, book.in_use);

  /* Control: repairing the same byte moves the failure past the extraction --
     the container now extracts, and open fails later on the missing OPF. */
  internal_corrupt_entry_payload("META-INF/container.xml");
  epub_book_t repaired = {};
  TEST_ASSERT_EQ(k_ra8_err_not_found, epub_open(&media, nullptr, &repaired));
  TEST_ASSERT_EQ(0, repaired.in_use);
  TEST_END("epub_open: damaged container.xml payload -> validation_failed");
}

/**
 * @test internal_test_embedded_font_corrupt_stream
 * @brief A damaged embedded-font payload is rejected after locate and stat.
 * @par MC/DC:
 * The extraction-result guard has one condition. The flipped stored byte
 * drives extraction failure; repairing the same byte drives success. N+1 = 2.
 * @pre The archive is opened while all central-directory records are intact.
 * @post Failure reports zero bytes and ::k_ra8_err_validation_failed.
 * @post The repaired payload extracts byte-exactly and the book closes cleanly.
 * @note Single-threaded host fixture; only the local payload byte is mutated.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_embedded_font_corrupt_stream(void)
{
  TEST_BEGIN("epub embedded font: damaged payload -> validation_failed");
  mz_zip_archive zip;
  (void)memset(&zip, 0, sizeof(zip));
  TEST_ASSERT(mz_zip_writer_init_heap(&zip, 0U, (size_t)k_ext_zip_buf) == MZ_TRUE);
  TEST_ASSERT(mz_zip_writer_add_mem(&zip,
                                    "META-INF/container.xml",
                                    (const void*)s_container_good,
                                    strlen(s_container_good),
                                    MZ_DEFAULT_COMPRESSION) == MZ_TRUE);
  TEST_ASSERT(mz_zip_writer_add_mem(&zip,
                                    "OEBPS/content.opf",
                                    (const void*)s_opf_with_font,
                                    strlen(s_opf_with_font),
                                    MZ_DEFAULT_COMPRESSION) == MZ_TRUE);
  TEST_ASSERT(mz_zip_writer_add_mem(&zip,
                                    "OEBPS/font.ttf",
                                    s_font_payload,
                                    sizeof(s_font_payload),
                                    MZ_NO_COMPRESSION) == MZ_TRUE);
  internal_finalise(&zip);

  epub_book_t            book  = {};
  const epub_mem_media_t media = {.data = s_zip_buf, .size = s_zip_size};
  TEST_ASSERT_EQ(k_ra8_ok, epub_open(&media, nullptr, &book));
  uint16_t count = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, epub_get_embedded_font_count(&book, &count));
  TEST_ASSERT_EQ(1U, count);

  internal_corrupt_entry_payload("OEBPS/font.ttf");
  uint8_t out[k_ext_font_bytes] = {};
  size_t  got                   = UINT16_MAX;
  TEST_ASSERT_EQ(k_ra8_err_validation_failed,
                 epub_get_embedded_font(&book, 0U, out, sizeof(out), &got));
  TEST_ASSERT_EQ(0U, got);

  internal_corrupt_entry_payload("OEBPS/font.ttf");
  TEST_ASSERT_EQ(k_ra8_ok, epub_get_embedded_font(&book, 0U, out, sizeof(out), &got));
  TEST_ASSERT_EQ(sizeof(s_font_payload), got);
  TEST_ASSERT(memcmp(out, s_font_payload, sizeof(s_font_payload)) == 0);
  TEST_ASSERT_EQ(k_ra8_ok, epub_close(&book));
  TEST_END("epub embedded font: damaged payload -> validation_failed");
}

/**
 * @brief Publish the extended-metadata fixture archive into ::s_zip_buf.
 *
 * @details
 * Four stored entries in a fixed order: a first entry whose central-only
 * extra field carries a COMPLETE extended-size record (that is what makes
 * miniz take the extended-metadata path at all), the container document, the
 * package document, and the font whose extra field carries only four of the
 * eight bytes that record requires. The two extra-field blobs are block-scope
 * statics: each is read by this builder alone, so file scope would make them
 * objects whose identifier appears in a single function.
 *
 * @pre ::s_zip_buf can hold the complete archive.
 * @pre The miniz heap writer is available.
 * @post ::s_zip_buf / ::s_zip_size hold a finalised four-entry archive.
 * @post Every central-directory record is exactly as the writer produced it.
 *
 * @note Not thread-safe; mutates file-static fixture state.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_build_extended_metadata_archive(void)
{
  mz_zip_archive zip = {};
  TEST_ASSERT(mz_zip_writer_init_heap(&zip, 0U, (size_t)k_ext_zip_buf) == MZ_TRUE);
  /* Distinctive first-entry payload; its contents never matter. */
  static const uint8_t s_dummy = 0xA5U;
  /* Complete extended-size field encoding an uncompressed size of one. */
  static const uint8_t s_zip64_complete[k_ext_zip64_complete_bytes] = {
    0x01U,
    0x00U,
    0x08U,
    0x00U,
    0x01U,
    0x00U,
    0x00U,
    0x00U,
    0x00U,
    0x00U,
    0x00U,
    0x00U,
  };
  /* Incomplete extended-size field with only half of its uint64 value. */
  static const uint8_t s_zip64_short[k_ext_zip64_short_bytes] = {
    0x01U,
    0x00U,
    0x04U,
    0x00U,
    (uint8_t)k_ext_font_bytes,
    0x00U,
    0x00U,
    0x00U,
  };
  internal_add_with_central_extra(&zip,
                                  "dummy.bin",
                                  &s_dummy,
                                  sizeof(s_dummy),
                                  s_zip64_complete,
                                  (mz_uint)sizeof(s_zip64_complete));
  TEST_ASSERT(mz_zip_writer_add_mem(&zip,
                                    "META-INF/container.xml",
                                    (const void*)s_container_good,
                                    strlen(s_container_good),
                                    MZ_DEFAULT_COMPRESSION) == MZ_TRUE);
  TEST_ASSERT(mz_zip_writer_add_mem(&zip,
                                    "OEBPS/content.opf",
                                    (const void*)s_opf_with_font,
                                    strlen(s_opf_with_font),
                                    MZ_DEFAULT_COMPRESSION) == MZ_TRUE);
  internal_add_with_central_extra(&zip,
                                  "OEBPS/font.ttf",
                                  s_font_payload,
                                  sizeof(s_font_payload),
                                  s_zip64_short,
                                  (mz_uint)sizeof(s_zip64_short));
  internal_finalise(&zip);
}

/**
 * @test internal_test_embedded_font_incomplete_extended_metadata
 * @brief A located font with incomplete extended-size metadata is rejected by
 *        the public embedded-font extractor.
 * @details A valid first entry carries a complete extended-size field and
 *          activates miniz's extended-metadata path. The font entry then
 *          advertises the sentinel uint32 size but supplies only four of the
 *          eight required bytes. Name lookup succeeds because it uses the
 *          indexed central-directory name; file stat rejects the incomplete
 *          size before extraction. Restoring the ordinary uint32 size and
 *          reopening the same bytes supplies the success control.
 * @par MC/DC:
 * Decision: `mz_zip_reader_file_stat(...) == MZ_FALSE` in
 * `internal_locate_extract` (one condition).
 * - V1: incomplete extended size -> true -> validation failure and zero bytes.
 * - V2: ordinary uint32 size -> false -> extraction succeeds byte-exactly.
 * N = 1, N+1 = 2; the target's four size bytes independently select the result.
 * @pre The heap ZIP writer and tracked EPUB test dependencies are available.
 * @pre ::s_zip_buf can hold the complete archive.
 * @post Both opened books are closed.
 * @post The success control reproduces ::s_font_payload byte-for-byte.
 * @note Single-threaded in-memory malformed-input fixture.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_embedded_font_incomplete_extended_metadata(void)
{
  TEST_BEGIN("epub embedded font: incomplete extended metadata -> validation_failed");
  internal_build_extended_metadata_archive();

  const size_t dummy_record = internal_central_record_offset("dummy.bin");
  const size_t font_record  = internal_central_record_offset("OEBPS/font.ttf");
  internal_set_central_uncompressed_size(dummy_record, UINT32_MAX);
  internal_set_central_uncompressed_size(font_record, UINT32_MAX);

  const epub_mem_media_t media          = {.data = s_zip_buf, .size = s_zip_size};
  epub_book_t            malformed_book = {};
  TEST_ASSERT_EQ(k_ra8_ok, epub_open(&media, nullptr, &malformed_book));
  uint8_t output[k_ext_font_bytes] = {};
  size_t  got                      = UINT16_MAX;
  TEST_ASSERT_EQ(k_ra8_err_validation_failed,
                 epub_get_embedded_font(&malformed_book, 0U, output, sizeof(output), &got));
  TEST_ASSERT_EQ(0U, got);
  TEST_ASSERT_EQ(k_ra8_ok, epub_close(&malformed_book));

  internal_set_central_uncompressed_size(font_record, (uint32_t)sizeof(s_font_payload));
  epub_book_t repaired_book = {};
  TEST_ASSERT_EQ(k_ra8_ok, epub_open(&media, nullptr, &repaired_book));
  TEST_ASSERT_EQ(k_ra8_ok,
                 epub_get_embedded_font(&repaired_book, 0U, output, sizeof(output), &got));
  TEST_ASSERT_EQ(sizeof(s_font_payload), got);
  TEST_ASSERT(memcmp(output, s_font_payload, sizeof(s_font_payload)) == 0);
  TEST_ASSERT_EQ(k_ra8_ok, epub_close(&repaired_book));
  TEST_END("epub embedded font: incomplete extended metadata -> validation_failed");
}

int main(void)
{
  internal_test_open_corrupt_container_stream();
  internal_test_embedded_font_corrupt_stream();
  internal_test_embedded_font_incomplete_extended_metadata();
  return 0;
}
