/**
 * @file test_media_dl_metadata.c
 * @brief Host tests for publication metadata and cover typing.
 *
 * @details Exercises bounded metadata parsing, ComicInfo and OPF generation,
 * deterministic UUIDs, external cover canonicalization, and image MIME sniffing.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "mdl_export.h"
#include "mdl_export_internal.h"
#include "mdl_test_storage.h"
#include "mdl_urlname.h"
#include "miniz.h"
#include "ra8_attributes.h"
#include "unity_minimal.h"
/** @brief Permission bits for metadata test scratch directories. */
typedef enum : uint16_t {
  k_mdl_test_dir_mode = 0755U, /**< rwxr-xr-x. */
} mdl_metadata_test_mode_t;
/** @brief Named fixture and metadata-test capacities. */
typedef enum : uint32_t {
  k_fixture_bytes           = 4U,                 /**< Synthetic page bytes.   */
  k_meta_line_test_slack    = 16U,                /**< Key/delimiter overhead. */
  k_test_export_arena_bytes = 8U * 1024U * 1024U, /**< Test exporter arena.    */
} mdl_metadata_test_bound_t;
/** @brief Caller-owned workspace backing every metadata export. */
static uint8_t s_test_export_arena[k_test_export_arena_bytes];
/** @brief Second arena used by the simultaneous deterministic-reader check. */
static uint8_t s_test_export_arena_two[k_test_export_arena_bytes];
/** @brief Raw descriptor state borrowed by one bounded miniz reader. */
typedef struct {
  int       descriptor; /**< Open read-only artifact descriptor. */
  uint64_t  size_bytes; /**< Immutable artifact extent.          */
  ra8_err_t read_error; /**< First positioned-read failure.      */
} mdl_test_zip_io_t;
/** @brief Complete caller-owned state for one allocation-free ZIP reader. */
typedef struct {
  mz_zip_archive         zip;       /**< Miniz reader descriptor.       */
  mdl_zip_allocator_t    allocator; /**< Bounded miniz allocator state. */
  mdl_export_workspace_t workspace; /**< Arena descriptor.              */
  mdl_test_zip_io_t      io;        /**< Positioned raw-file reader.    */
} mdl_test_zip_reader_t;
/** @brief Adapt bounded positioned descriptor reads to miniz.
 * @details Exercises the zip read scenario through production media-downloader interfaces and checks its observable success, rejection, and boundary results.
 * @param[in,out] opaque Opaque reader context supplied by the archive seam.
 * @param[in] offset Zero-based source byte offset.
 * @param[out] destination Caller-owned destination for the requested bytes.
 * @param[in] length Exact requested byte count.
 * @return Value produced by the bounded test helper.
 * @retval 0 The helper produced its zero-valued boundary result.
 * @retval nonzero The helper produced its documented nonzero result.
 * @pre The host test process exclusively owns its fixture state.
 * @pre Required fakes and bounded buffers are initialized before use.
 * @post Normal return means every scenario assertion passed.
 * @post No fixture resource ownership is transferred by this test.
 * @note Host-only and synchronous; assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static size_t
internal_test_zip_read(void* opaque, mz_uint64 offset, void* destination, size_t length)
{
  mdl_test_zip_io_t* io = (mdl_test_zip_io_t*)opaque;
  if ((io->read_error != k_ra8_ok) || (offset > io->size_bytes) || (length > (size_t)SSIZE_MAX) ||
      ((uint64_t)length > (io->size_bytes - offset))) {
    return 0U;
  }
  size_t total = 0U;
  while (total < length) {
    const ssize_t got = pread(io->descriptor,
                              &((uint8_t*)destination)[total],
                              length - total,
                              (off_t)(offset + total));
    if (got > 0) {
      total += (size_t)got;
    } else if ((got < 0) && (errno == EINTR)) {
      continue;
    } else {
      io->read_error = k_ra8_err_protocol_error;
      break;
    }
  }
  return total;
}

/**
 * @brief Open a ZIP with raw reads and one explicit caller arena.
 * @param[out] reader Reader state to initialize.
 * @param[in] path Artifact path.
 * @param[in,out] arena Writable miniz arena.
 * @param[in] arena_bytes Arena extent.
 * @return Whether descriptor and miniz initialization succeeded.
 * @pre @p reader is inactive and the arena remains live through close.
 * @post Failure retains no descriptor; success leaves one active reader.
 * @note Test-only and allocation-free.
 * @since 0.1.0
 * @details Exercises the zip open scenario through production media-downloader interfaces and checks its observable success, rejection, and boundary results.
 * @retval true The requested fixture condition succeeded.
 * @retval false The helper rejected or could not complete the condition.
 * @pre The host test process exclusively owns its fixture state.
 * @post Normal return means every scenario assertion passed.
 */
RA8_INTERNAL static bool internal_test_zip_open(mdl_test_zip_reader_t* reader,
                                                const char*            path,
                                                uint8_t*               arena,
                                                size_t                 arena_bytes)
{
  *reader               = (mdl_test_zip_reader_t){};
  reader->io.descriptor = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (reader->io.descriptor < 0) {
    return false;
  }
  const off_t end = lseek(reader->io.descriptor, 0, SEEK_END);
  if (end <= 0) {
    (void)close(reader->io.descriptor);
    reader->io.descriptor = -1;
    return false;
  }
  reader->io.size_bytes = (uint64_t)end;
  mdl_export_workspace_init(&reader->workspace, arena, arena_bytes);
  priv_mdl_zip_workspace_bind(&reader->zip, &reader->allocator, &reader->workspace);
  reader->zip.m_pRead      = internal_test_zip_read;
  reader->zip.m_pIO_opaque = &reader->io;
  if (mz_zip_reader_init(&reader->zip, reader->io.size_bytes, 0) == MZ_FALSE) {
    priv_mdl_zip_workspace_release(&reader->allocator);
    (void)close(reader->io.descriptor);
    reader->io.descriptor = -1;
    return false;
  }
  return true;
}

/** @brief End one initialized test ZIP reader and release its resources.
 * @details Exercises the zip close scenario through production media-downloader interfaces and checks its observable success, rejection, and boundary results.
 * @param[in,out] reader Caller-owned archive reader to close.
 * @return True when the helper condition succeeds; otherwise false.
 * @retval true The requested fixture condition succeeded.
 * @retval false The helper rejected or could not complete the condition.
 * @pre The host test process exclusively owns its fixture state.
 * @pre Required fakes and bounded buffers are initialized before use.
 * @post Normal return means every scenario assertion passed.
 * @post No fixture resource ownership is transferred by this test.
 * @note Host-only and synchronous; assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_test_zip_close(mdl_test_zip_reader_t* reader)
{
  const bool ended = mz_zip_reader_end(&reader->zip) != MZ_FALSE;
  priv_mdl_zip_workspace_release(&reader->allocator);
  const bool closed     = close(reader->io.descriptor) == 0;
  reader->io.descriptor = -1;
  return ended && closed && (reader->io.read_error == k_ra8_ok);
}

/**
 * @brief Extract one ZIP member as bounded NUL-terminated text.
 * @param[in,out] reader Active reader.
 * @param[in] name Exact member name.
 * @param[out] destination Text destination.
 * @param[in] capacity Destination capacity including NUL.
 * @return Whether the complete member fit and was extracted.
 * @pre Every pointer is non-NULL and @p capacity is nonzero.
 * @post Success initializes one NUL-terminated member body.
 * @note Binary members are unsupported by this helper.
 * @since 0.1.0
 * @details Exercises the zip text scenario through production media-downloader interfaces and checks its observable success, rejection, and boundary results.
 * @retval true The requested fixture condition succeeded.
 * @retval false The helper rejected or could not complete the condition.
 * @pre The host test process exclusively owns its fixture state.
 * @post Normal return means every scenario assertion passed.
 */
RA8_INTERNAL static bool internal_test_zip_text(mdl_test_zip_reader_t* reader,
                                                const char*            name,
                                                char*                  destination,
                                                size_t                 capacity)
{
  const int                index = mz_zip_reader_locate_file(&reader->zip, name, nullptr, 0);
  mz_zip_archive_file_stat member;
  if ((index < 0) || (mz_zip_reader_file_stat(&reader->zip, (mz_uint)index, &member) == MZ_FALSE) ||
      (member.m_uncomp_size >= (mz_uint64)capacity)) {
    return false;
  }
  const size_t length = (size_t)member.m_uncomp_size;
  if (mz_zip_reader_extract_to_mem(&reader->zip, (mz_uint)index, destination, length, 0) ==
      MZ_FALSE) {
    return false;
  }
  destination[length] = '\0';
  return true;
}
/**
 * @brief Write one complete binary fixture through a raw descriptor.
 * @param[in] path Absolute fixture path.
 * @param[in] data Bytes to write.
 * @param[in] length Byte count.
 * @return Whether create, write, and close all succeeded.
 * @pre @p data spans @p length readable bytes.
 * @post Success leaves exactly @p length bytes at @p path.
 * @note Test-only POSIX fixture helper.
 * @since 0.1.0
 * @details Implements this test-only seam with caller-owned fixtures, bounded storage, and explicit propagation of the result observed by its caller.
 * @retval true The requested fixture condition succeeded.
 * @retval false The helper rejected or could not complete the condition.
 * @pre Pointer arguments satisfy their documented readable and writable extents.
 * @post Documented outputs reflect the processed fixture on success.
 */
RA8_INTERNAL static bool internal_write_bytes(const char* path, const uint8_t* data, size_t length)
{
  const int descriptor = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (descriptor < 0) {
    return false;
  }
  size_t offset = 0U;
  while (offset < length) {
    const ssize_t written = write(descriptor, &data[offset], length - offset);
    if (written > 0) {
      offset += (size_t)written;
    } else if ((written < 0) && (errno == EINTR)) {
      continue;
    } else {
      break;
    }
  }
  return (close(descriptor) == 0) && (offset == length);
}

/**
 * @brief Perform the export chapter meta step.
 * @details Executes the export chapter meta scenario through production interfaces and checks its observable success, rejection, and boundary results.
 * @param[in] fmt Selected output format.
 * @param[in] chapter_dir Chapter dir value for this operation.
 * @param[in] out_path Out path value for this operation.
 * @param[in] meta Metadata record to read or update.
 * @return Canonical downloader status.
 * @retval k_ra8_ok The operation completed.
 * @retval other Validation, capacity, network, or storage failed.
 * @pre Assertions are enabled for contract verification.
 * @pre The test fixture workspace is isolated for this scenario.
 * @post Normal return means every reached contract assertion passed.
 * @post The caller receives no transferred fixture ownership.
 * @note Test helper; an assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_export_chapter_meta(ra8_mdl_format_t         fmt,
                                                           const char*              chapter_dir,
                                                           const char*              out_path,
                                                           const mdl_export_meta_t* meta)
{
  mdl_export_workspace_t ws;
  mdl_export_workspace_init(&ws, s_test_export_arena, sizeof(s_test_export_arena));
  return mdl_export_chapter_meta_ws(mdl_test_storage_get(), fmt, chapter_dir, out_path, meta, &ws);
}

/**
 * @brief Write `k_fixture_bytes` of `fill` to `path`.
 * @details Executes the write fixture scenario through production interfaces and checks its observable success, rejection, and boundary results.
 * @param[in] path Filesystem path for the operation.
 * @param[in] fill Fill value for this operation.
 * @pre Assertions are enabled for contract verification.
 * @pre The test fixture workspace is isolated for this scenario.
 * @post Normal return means every reached contract assertion passed.
 * @post The caller receives no transferred fixture ownership.
 * @note Test helper; an assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_write_fixture(const char* path, char fill)
{
  uint8_t bytes[k_fixture_bytes];
  memset(bytes, fill, sizeof(bytes));
  TEST_ASSERT(internal_write_bytes(path, bytes, sizeof(bytes)));
}

/**
 * @brief Write an exact binary fixture to `path`.
 * @details Executes the write binary fixture scenario through production interfaces and checks its observable success, rejection, and boundary results.
 * @param[in] path Filesystem path for the operation.
 * @param[in] data Input byte sequence.
 * @param[in] len Number of input bytes.
 * @pre Assertions are enabled for contract verification.
 * @pre The test fixture workspace is isolated for this scenario.
 * @post Normal return means every reached contract assertion passed.
 * @post The caller receives no transferred fixture ownership.
 * @note Test helper; an assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void
internal_write_binary_fixture(const char* path, const void* data, size_t len)
{
  TEST_ASSERT(internal_write_bytes(path, (const uint8_t*)data, len));
}

/**
 * @brief Assert complete key-value metadata parsing.
 * @details Executes the assert kv metadata scenario through production interfaces and checks its observable success, rejection, and boundary results.
 * @param[in,out] meta Metadata record to read or update.
 * @pre Assertions are enabled for contract verification.
 * @pre The test fixture workspace is isolated for this scenario.
 * @post Normal return means every reached contract assertion passed.
 * @post The caller receives no transferred fixture ownership.
 * @note Test helper; an assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_assert_kv_metadata(mdl_export_meta_t* meta)
{
  static const char kv[] = "series = Test Series & Saga\n"
                           "title = Chapter 12: Beginning & End\n"
                           "writer = Author & Writer\n"
                           "artist = Illustrator & Artist\n"
                           "number = 12.5\n"
                           "summary = A great story <start>\n"
                           "source_url = https://example.test/series?a=1&title=<Origins>\n"
                           "cover = page_002.jpg\n"
                           "cover_index = 1\n"
                           "language = ja\n"
                           "reading_direction = rtl\n"
                           "identifier = book:test\n"
                           "modified = 2025-02-03T04:05:06Z\n";
  TEST_ASSERT(mdl_meta_parse(meta, kv) == k_ra8_ok);
  TEST_ASSERT(strcmp(meta->series_title, "Test Series & Saga") == 0);
  TEST_ASSERT(strcmp(meta->chapter_title, "Chapter 12: Beginning & End") == 0);
  TEST_ASSERT(strcmp(meta->writer, "Author & Writer") == 0);
  TEST_ASSERT(strcmp(meta->artist, "Illustrator & Artist") == 0);
  TEST_ASSERT(meta->chapter_number == 12.5);
  TEST_ASSERT(strcmp(meta->summary, "A great story <start>") == 0);
  TEST_ASSERT(strcmp(meta->source_url, "https://example.test/series?a=1&title=<Origins>") == 0);
  TEST_ASSERT(strcmp(meta->cover_path, "page_002.jpg") == 0);
  TEST_ASSERT_EQ(1, meta->cover_index);
  TEST_ASSERT(strcmp(meta->language, "ja") == 0);
  TEST_ASSERT(meta->reading_direction == k_mdl_read_rtl);
  TEST_ASSERT(strcmp(meta->identifier, "book:test") == 0);
  TEST_ASSERT(strcmp(meta->modified, "2025-02-03T04:05:06Z") == 0);
}

/**
 * @brief Assert exact and overflowing metadata path bounds.
 * @details Executes the assert metadata bounds scenario through production interfaces and checks its observable success, rejection, and boundary results.
 * @param[in,out] meta Metadata record to read or update.
 * @pre Assertions are enabled for contract verification.
 * @pre The test fixture workspace is isolated for this scenario.
 * @post Normal return means every reached contract assertion passed.
 * @post The caller receives no transferred fixture ownership.
 * @note Test helper; an assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_assert_metadata_bounds(mdl_export_meta_t* meta)
{
  char exact_cover[k_mdl_meta_path_max];
  memset(exact_cover, 'a', sizeof(exact_cover) - 1U);
  exact_cover[sizeof(exact_cover) - 1U] = '\0';
  char cover_line[k_mdl_meta_path_max + k_meta_line_test_slack];
  (void)__builtin_snprintf(cover_line, sizeof(cover_line), "cover=%s\n", exact_cover);
  mdl_meta_init(meta);
  TEST_ASSERT(mdl_meta_parse(meta, cover_line) == k_ra8_ok);
  TEST_ASSERT(strlen(meta->cover_path) == sizeof(exact_cover) - 1U);
  char overlong_cover[k_mdl_meta_path_max + 1U];
  memset(overlong_cover, 'b', sizeof(overlong_cover) - 1U);
  overlong_cover[sizeof(overlong_cover) - 1U] = '\0';
  (void)__builtin_snprintf(cover_line, sizeof(cover_line), "cover=%s\n", overlong_cover);
  mdl_meta_init(meta);
  TEST_ASSERT(mdl_meta_parse(meta, cover_line) == k_ra8_err_invalid_size);
  TEST_ASSERT(meta->cover_path[0] == '\0');

  char exact_source[k_mdl_meta_url_max];
  memset(exact_source, 'a', sizeof(exact_source) - 1U);
  static const char source_prefix[] = "https://example.test/";
  memcpy(exact_source, source_prefix, sizeof(source_prefix) - 1U);
  exact_source[sizeof(exact_source) - 1U] = '\0';
  char source_line[k_mdl_meta_url_max + k_meta_line_test_slack];
  (void)__builtin_snprintf(source_line, sizeof(source_line), "source_url=%s\n", exact_source);
  mdl_meta_init(meta);
  TEST_ASSERT(mdl_meta_parse(meta, source_line) == k_ra8_ok);
  TEST_ASSERT(strlen(meta->source_url) == sizeof(exact_source) - 1U);
  char overlong_source[k_mdl_meta_url_max + 1U];
  memset(overlong_source, 'b', sizeof(overlong_source) - 1U);
  memcpy(overlong_source, source_prefix, sizeof(source_prefix) - 1U);
  overlong_source[sizeof(overlong_source) - 1U] = '\0';
  (void)__builtin_snprintf(source_line, sizeof(source_line), "source_url=%s\n", overlong_source);
  mdl_meta_init(meta);
  TEST_ASSERT(mdl_meta_parse(meta, source_line) == k_ra8_err_invalid_size);
  TEST_ASSERT(meta->source_url[0] == '\0');
  mdl_meta_init(meta);
  TEST_ASSERT(mdl_meta_parse(meta, "source_url=file:///tmp/private\n") == k_ra8_err_invalid_arg);
  TEST_ASSERT(meta->source_url[0] == '\0');
}

/**
 * @brief Assert ComicInfo-style XML metadata parsing.
 * @details Executes the assert xml metadata scenario through production interfaces and checks its observable success, rejection, and boundary results.
 * @param[in,out] meta Metadata record to read or update.
 * @pre Assertions are enabled for contract verification.
 * @pre The test fixture workspace is isolated for this scenario.
 * @post Normal return means every reached contract assertion passed.
 * @post The caller receives no transferred fixture ownership.
 * @note Test helper; an assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_assert_xml_metadata(mdl_export_meta_t* meta)
{
  static const char xml[] = "<?xml version=\"1.0\"?>\n"
                            "<ComicInfo>\n"
                            "  <Series>XML &amp; Series</Series>\n"
                            "  <Title>XML &lt;Title&gt;</Title>\n"
                            "  <Writer>XML Writer</Writer>\n"
                            "  <Artist>XML Artist</Artist>\n"
                            "  <Number>5</Number>\n"
                            "  <Summary>XML Summary &quot;Quote&quot;</Summary>\n"
                            "  <Web>https://example.test/xml?a=1&amp;title=&lt;Origins&gt;</Web>\n"
                            "  <CoverImage>cover.jpg</CoverImage>\n"
                            "</ComicInfo>";
  mdl_meta_init(meta);
  TEST_ASSERT(mdl_meta_parse(meta, xml) == k_ra8_ok);
  TEST_ASSERT(strcmp(meta->series_title, "XML & Series") == 0);
  TEST_ASSERT(strcmp(meta->chapter_title, "XML <Title>") == 0);
  TEST_ASSERT(strcmp(meta->writer, "XML Writer") == 0);
  TEST_ASSERT(strcmp(meta->artist, "XML Artist") == 0);
  TEST_ASSERT(meta->chapter_number == 5.0);
  TEST_ASSERT(strcmp(meta->summary, "XML Summary \"Quote\"") == 0);
  TEST_ASSERT(strcmp(meta->source_url, "https://example.test/xml?a=1&title=<Origins>") == 0);
  TEST_ASSERT(strcmp(meta->cover_path, "cover.jpg") == 0);
}

/**
 * @brief Assert directory metadata auto-discovery.
 * @details Executes the assert metadata load scenario through production interfaces and checks its observable success, rejection, and boundary results.
 * @param[in,out] meta Metadata record to read or update.
 * @pre Assertions are enabled for contract verification.
 * @pre The test fixture workspace is isolated for this scenario.
 * @post Normal return means every reached contract assertion passed.
 * @post The caller receives no transferred fixture ownership.
 * @note Test helper; an assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_assert_metadata_load(mdl_export_meta_t* meta)
{
  const char* dir = "/tmp/mdl_meta_dir";
  (void)mkdir(dir, (mode_t)k_mdl_test_dir_mode);
  static const uint8_t metadata[] = "series: Auto Discovered Series\nwriter: Auto Writer\n";
  TEST_ASSERT(
    internal_write_bytes("/tmp/mdl_meta_dir/metadata.txt", metadata, sizeof(metadata) - 1U));
  mdl_meta_init(meta);
  TEST_ASSERT(mdl_meta_load_dir(mdl_test_storage_get(), meta, dir) == k_ra8_ok);
  TEST_ASSERT(strcmp(meta->series_title, "Auto Discovered Series") == 0);
  TEST_ASSERT(strcmp(meta->writer, "Auto Writer") == 0);
  (void)unlink("/tmp/mdl_meta_dir/metadata.txt");
  (void)rmdir(dir);
}

/**
 * @test Rich metadata init, key-value parsing, XML parsing, and dir auto-discovery.
 * @brief Exercise the meta init parse load regression scenario.
 * @details Executes the meta init parse load scenario through production interfaces and checks its observable success, rejection, and boundary results.
 * @pre Assertions are enabled for contract verification.
 * @pre The test fixture workspace is isolated for this scenario.
 * @post Normal return means every reached contract assertion passed.
 * @post The caller receives no transferred fixture ownership.
 * @note Test helper; an assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_meta_init_parse_load(void)
{
  TEST_BEGIN("metadata init, parse, and load");
  mdl_export_meta_t meta;
  mdl_meta_init(&meta);
  TEST_ASSERT_EQ(-1, meta.cover_index);
  TEST_ASSERT(meta.series_title[0] == '\0');
  internal_assert_kv_metadata(&meta);
  internal_assert_metadata_bounds(&meta);
  internal_assert_xml_metadata(&meta);
  internal_assert_metadata_load(&meta);
  TEST_END("metadata init, parse, and load");
}

/**
 * @brief Initialize the rich ComicInfo fixture metadata.
 * @details Executes the init comicinfo meta scenario through production interfaces and checks its observable success, rejection, and boundary results.
 * @param[in,out] meta Metadata record to read or update.
 * @pre Assertions are enabled for contract verification.
 * @pre The test fixture workspace is isolated for this scenario.
 * @post Normal return means every reached contract assertion passed.
 * @post The caller receives no transferred fixture ownership.
 * @note Test helper; an assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_init_comicinfo_meta(mdl_export_meta_t* meta)
{
  mdl_meta_init(meta);
  (void)__builtin_snprintf(meta->series_title, sizeof(meta->series_title), "Comic & Series");
  (void)__builtin_snprintf(meta->chapter_title, sizeof(meta->chapter_title), "Ch 1: <Origin>");
  meta->chapter_number = 1.0;
  (void)__builtin_snprintf(meta->writer, sizeof(meta->writer), "Writer & Author");
  (void)__builtin_snprintf(meta->artist, sizeof(meta->artist), "Artist & Penciller");
  (void)__builtin_snprintf(meta->summary, sizeof(meta->summary), "Summary & Description");
  (void)__builtin_snprintf(meta->source_url,
                           sizeof(meta->source_url),
                           "https://example.test/series?a=1&title=<Origins>");
  (void)__builtin_snprintf(meta->language, sizeof(meta->language), "ja");
  meta->reading_direction = k_mdl_read_rtl;
  meta->cover_index       = 0;
}

/**
 * @brief Assert the complete generated ComicInfo document.
 * @details Executes the assert comicinfo document scenario through production interfaces and checks its observable success, rejection, and boundary results.
 * @param[in] meta Metadata record to read or update.
 * @pre Assertions are enabled for contract verification.
 * @pre The test fixture workspace is isolated for this scenario.
 * @post Normal return means every reached contract assertion passed.
 * @post The caller receives no transferred fixture ownership.
 * @note Test helper; an assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_assert_comicinfo_document(const mdl_export_meta_t* meta)
{
  char xml_buf[4096];
  TEST_ASSERT(mdl_export_build_comicinfo_pages(meta, 3U, xml_buf, sizeof(xml_buf)) == k_ra8_ok);
  TEST_ASSERT(strstr(xml_buf, "<Series>Comic &amp; Series</Series>") != nullptr);
  TEST_ASSERT(strstr(xml_buf, "<Title>Ch 1: &lt;Origin&gt;</Title>") != nullptr);
  TEST_ASSERT(strstr(xml_buf, "<Number>1</Number>") != nullptr);
  TEST_ASSERT(strstr(xml_buf, "<Writer>Writer &amp; Author</Writer>") != nullptr);
  TEST_ASSERT(strstr(xml_buf, "<Artist>Artist &amp; Penciller</Artist>") != nullptr);
  TEST_ASSERT(strstr(xml_buf, "<Summary>Summary &amp; Description</Summary>") != nullptr);
  TEST_ASSERT(
    strstr(xml_buf, "<Web>https://example.test/series?a=1&amp;title=&lt;Origins&gt;</Web>") !=
    nullptr);
  TEST_ASSERT(strstr(xml_buf, "<PageCount>3</PageCount>") != nullptr);
  TEST_ASSERT(strstr(xml_buf, "<LanguageISO>ja</LanguageISO>") != nullptr);
  TEST_ASSERT(strstr(xml_buf, "<Manga>YesAndRightToLeft</Manga>") != nullptr);
  TEST_ASSERT(strstr(xml_buf, "Image=\"0\" Type=\"FrontCover\"") != nullptr);
}

/**
 * @brief Assert CBZ metadata insertion and bounded source rejection.
 * @details Executes the assert cbz comicinfo scenario through production interfaces and checks its observable success, rejection, and boundary results.
 * @param[in] meta Metadata record to read or update.
 * @pre Assertions are enabled for contract verification.
 * @pre The test fixture workspace is isolated for this scenario.
 * @post Normal return means every reached contract assertion passed.
 * @post The caller receives no transferred fixture ownership.
 * @note Test helper; an assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_assert_cbz_comicinfo(const mdl_export_meta_t* meta)
{
  const char* dir = "/tmp/mdl_cbz_meta_chap";
  const char* out = "/tmp/mdl_cbz_meta_chap.cbz";
  (void)mkdir(dir, (mode_t)k_mdl_test_dir_mode);
  internal_write_fixture("/tmp/mdl_cbz_meta_chap/page_001.jpg", 'a');
  TEST_ASSERT(internal_export_chapter_meta(k_ra8_mdl_format_cbz, dir, out, meta) == k_ra8_ok);
  mdl_test_zip_reader_t reader;
  TEST_ASSERT(
    internal_test_zip_open(&reader, out, s_test_export_arena, sizeof(s_test_export_arena)));
  char content[4096];
  TEST_ASSERT(internal_test_zip_text(&reader, "ComicInfo.xml", content, sizeof(content)));
  TEST_ASSERT(strstr(content, "<Series>Comic &amp; Series</Series>") != nullptr);
  TEST_ASSERT(strstr(content, "<PageCount>1</PageCount>") != nullptr);
  TEST_ASSERT(strstr(content, "<LanguageISO>ja</LanguageISO>") != nullptr);
  TEST_ASSERT(
    strstr(content, "<Web>https://example.test/series?a=1&amp;title=&lt;Origins&gt;</Web>") !=
    nullptr);
  TEST_ASSERT(internal_test_zip_close(&reader));
  mdl_export_meta_t overlong = *meta;
  memset(overlong.source_url, 'x', sizeof(overlong.source_url));
  const char* rejected = "/tmp/mdl_cbz_meta_rejected.cbz";
  (void)unlink(rejected);
  TEST_ASSERT(internal_export_chapter_meta(k_ra8_mdl_format_cbz, dir, rejected, &overlong) ==
              k_ra8_err_invalid_size);
  TEST_ASSERT(access(rejected, F_OK) != 0);
  (void)unlink("/tmp/mdl_cbz_meta_chap/page_001.jpg");
  (void)unlink(out);
  (void)unlink(rejected);
  (void)rmdir(dir);
}

/**
 * @test Build ComicInfo.xml string and verify CBZ metadata insertion.
 * @brief Exercise the comicinfo xml generation regression scenario.
 * @details Executes the comicinfo xml generation scenario through production interfaces and checks its observable success, rejection, and boundary results.
 * @pre Assertions are enabled for contract verification.
 * @pre The test fixture workspace is isolated for this scenario.
 * @post Normal return means every reached contract assertion passed.
 * @post The caller receives no transferred fixture ownership.
 * @note Test helper; an assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_comicinfo_xml_generation(void)
{
  TEST_BEGIN("ComicInfo.xml generation & CBZ metadata");
  mdl_export_meta_t meta;
  internal_init_comicinfo_meta(&meta);
  internal_assert_comicinfo_document(&meta);
  internal_assert_cbz_comicinfo(&meta);
  TEST_END("ComicInfo.xml generation & CBZ metadata");
}

/**
 * @brief Initialize the rich EPUB metadata fixture.
 * @details Executes the init epub meta scenario through production interfaces and checks its observable success, rejection, and boundary results.
 * @param[in,out] meta Metadata record to read or update.
 * @pre Assertions are enabled for contract verification.
 * @pre The test fixture workspace is isolated for this scenario.
 * @post Normal return means every reached contract assertion passed.
 * @post The caller receives no transferred fixture ownership.
 * @note Test helper; an assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_init_epub_meta(mdl_export_meta_t* meta)
{
  mdl_meta_init(meta);
  (void)__builtin_snprintf(meta->series_title, sizeof(meta->series_title), "EPUB Series");
  (void)__builtin_snprintf(meta->chapter_title, sizeof(meta->chapter_title), "EPUB Chapter 1");
  (void)__builtin_snprintf(meta->writer, sizeof(meta->writer), "EPUB Writer");
  (void)__builtin_snprintf(meta->artist, sizeof(meta->artist), "EPUB Artist");
  (void)__builtin_snprintf(meta->summary, sizeof(meta->summary), "EPUB Summary");
  (void)__builtin_snprintf(meta->source_url,
                           sizeof(meta->source_url),
                           "https://example.test/series?a=1&title=<Origins>");
  meta->cover_index = 1;
  (void)__builtin_snprintf(meta->language, sizeof(meta->language), "ja");
  meta->reading_direction = k_mdl_read_rtl;
}

/**
 * @brief Assert EPUB OPF metadata and cover-image semantics.
 * @details Executes the assert epub opf scenario through production interfaces and checks its observable success, rejection, and boundary results.
 * @param[in] meta Metadata record to read or update.
 * @pre Assertions are enabled for contract verification.
 * @pre The test fixture workspace is isolated for this scenario.
 * @post Normal return means every reached contract assertion passed.
 * @post The caller receives no transferred fixture ownership.
 * @note Test helper; an assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_assert_epub_opf(const mdl_export_meta_t* meta)
{
  const char* dir = "/tmp/mdl_epub_meta_chap";
  const char* out = "/tmp/mdl_epub_meta_chap.epub";
  TEST_ASSERT(internal_export_chapter_meta(k_ra8_mdl_format_epub, dir, out, meta) == k_ra8_ok);
  mdl_test_zip_reader_t reader;
  TEST_ASSERT(
    internal_test_zip_open(&reader, out, s_test_export_arena, sizeof(s_test_export_arena)));
  char opf[4096];
  TEST_ASSERT(internal_test_zip_text(&reader, "OEBPS/content.opf", opf, sizeof(opf)));
  TEST_ASSERT(strstr(opf, "<dc:identifier id=\"bookid\">urn:uuid:") != nullptr);
  TEST_ASSERT(strstr(opf, "<dc:title>EPUB Chapter 1</dc:title>") != nullptr);
  TEST_ASSERT(strstr(opf, "<dc:creator opf:role=\"aut\">EPUB Writer</dc:creator>") != nullptr);
  TEST_ASSERT(strstr(opf, "<dc:creator opf:role=\"art\">EPUB Artist</dc:creator>") != nullptr);
  TEST_ASSERT(strstr(opf, "<dc:description>EPUB Summary</dc:description>") != nullptr);
  TEST_ASSERT(
    strstr(opf,
           "<dc:source>https://example.test/series?a=1&amp;title=&lt;Origins&gt;</dc:source>") !=
    nullptr);
  TEST_ASSERT(strstr(opf, "<dc:language>ja</dc:language>") != nullptr);
  TEST_ASSERT(strstr(opf, "<meta property=\"schema:numberOfPages\">2</meta>") != nullptr);
  TEST_ASSERT(strstr(opf, "<spine page-progression-direction=\"rtl\">") != nullptr);
  TEST_ASSERT(strstr(opf,
                     "id=\"img1\" href=\"images/page_002.jpg\" media-type=\"image/jpeg\" "
                     "properties=\"cover-image\"") != nullptr);
  TEST_ASSERT(internal_test_zip_close(&reader));
}

/**
 * @brief Assert different chapters receive distinct EPUB identifiers.
 * @details Changes the canonical chapter title and source URL, then proves the
 * generated identifier field differs from the first chapter's identifier.
 * @param[in] meta Metadata record to read or update.
 * @pre Assertions are enabled for contract verification.
 * @pre The test fixture workspace is isolated for this scenario.
 * @post Normal return means every reached contract assertion passed.
 * @post The caller receives no transferred fixture ownership.
 * @note Test helper; an assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_assert_epub_distinct_identifiers(const mdl_export_meta_t* meta)
{
  const char*       dir    = "/tmp/mdl_epub_meta_chap";
  const char*       first  = "/tmp/mdl_epub_meta_chap.epub";
  const char*       second = "/tmp/mdl_epub_meta_chap_2.epub";
  mdl_export_meta_t other  = *meta;
  (void)__builtin_snprintf(other.chapter_title, sizeof(other.chapter_title), "EPUB Chapter 2");
  (void)__builtin_snprintf(other.source_url,
                           sizeof(other.source_url),
                           "https://example.test/chapter/2");
  TEST_ASSERT(internal_export_chapter_meta(k_ra8_mdl_format_epub, dir, second, &other) == k_ra8_ok);
  mdl_test_zip_reader_t one;
  mdl_test_zip_reader_t two;
  TEST_ASSERT(
    internal_test_zip_open(&one, first, s_test_export_arena, sizeof(s_test_export_arena)));
  TEST_ASSERT(
    internal_test_zip_open(&two, second, s_test_export_arena_two, sizeof(s_test_export_arena_two)));
  char opf_one[4096];
  char opf_two[4096];
  TEST_ASSERT(internal_test_zip_text(&one, "OEBPS/content.opf", opf_one, sizeof(opf_one)));
  TEST_ASSERT(internal_test_zip_text(&two, "OEBPS/content.opf", opf_two, sizeof(opf_two)));
  const char* id_one = strstr(opf_one, "<dc:identifier id=\"bookid\">");
  const char* id_two = strstr(opf_two, "<dc:identifier id=\"bookid\">");
  TEST_ASSERT((id_one != nullptr) && (id_two != nullptr));
  TEST_ASSERT(strncmp(id_one, id_two, 80U) != 0);
  TEST_ASSERT(internal_test_zip_close(&one));
  TEST_ASSERT(internal_test_zip_close(&two));
}

/**
 * @brief Assert overlong EPUB source metadata fails without publication.
 * @details Executes the assert epub rejection scenario through production interfaces and checks its observable success, rejection, and boundary results.
 * @param[in] meta Metadata record to read or update.
 * @pre Assertions are enabled for contract verification.
 * @pre The test fixture workspace is isolated for this scenario.
 * @post Normal return means every reached contract assertion passed.
 * @post The caller receives no transferred fixture ownership.
 * @note Test helper; an assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_assert_epub_rejection(const mdl_export_meta_t* meta)
{
  mdl_export_meta_t overlong = *meta;
  memset(overlong.source_url, 'x', sizeof(overlong.source_url));
  const char* rejected = "/tmp/mdl_epub_meta_rejected.epub";
  (void)unlink(rejected);
  TEST_ASSERT(internal_export_chapter_meta(k_ra8_mdl_format_epub,
                                           "/tmp/mdl_epub_meta_chap",
                                           rejected,
                                           &overlong) == k_ra8_err_invalid_size);
  TEST_ASSERT(access(rejected, F_OK) != 0);
}

/**
 * @test EPUB metadata generation, unique UUID identifier, and cover image property.
 * @brief Exercise the epub metadata and uuid regression scenario.
 * @details Executes the epub metadata and uuid scenario through production interfaces and checks its observable success, rejection, and boundary results.
 * @pre Assertions are enabled for contract verification.
 * @pre The test fixture workspace is isolated for this scenario.
 * @post Normal return means every reached contract assertion passed.
 * @post The caller receives no transferred fixture ownership.
 * @note Test helper; an assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_epub_metadata_and_uuid(void)
{
  TEST_BEGIN("EPUB metadata, UUID, and cover image");
  const char* dir = "/tmp/mdl_epub_meta_chap";
  (void)mkdir(dir, (mode_t)k_mdl_test_dir_mode);
  internal_write_fixture("/tmp/mdl_epub_meta_chap/page_001.jpg", 'a');
  internal_write_fixture("/tmp/mdl_epub_meta_chap/page_002.jpg", 'b');
  mdl_export_meta_t meta;
  internal_init_epub_meta(&meta);
  internal_assert_epub_opf(&meta);
  internal_assert_epub_distinct_identifiers(&meta);
  internal_assert_epub_rejection(&meta);
  (void)unlink("/tmp/mdl_epub_meta_chap/page_001.jpg");
  (void)unlink("/tmp/mdl_epub_meta_chap/page_002.jpg");
  (void)unlink("/tmp/mdl_epub_meta_chap.epub");
  (void)unlink("/tmp/mdl_epub_meta_chap_2.epub");
  (void)unlink("/tmp/mdl_epub_meta_rejected.epub");
  (void)rmdir(dir);
  TEST_END("EPUB metadata, UUID, and cover image");
}

/**
 * @brief Assert canonical external-cover insertion in a CBZ.
 * @details Executes the assert external cbz scenario through production interfaces and checks its observable success, rejection, and boundary results.
 * @param[in] meta Metadata record to read or update.
 * @param[in] expected Expected value for this operation.
 * @param[in] expected_bytes Expected bytes value for this operation.
 * @pre Assertions are enabled for contract verification.
 * @pre The test fixture workspace is isolated for this scenario.
 * @post Normal return means every reached contract assertion passed.
 * @post The caller receives no transferred fixture ownership.
 * @note Test helper; an assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_assert_external_cbz(const mdl_export_meta_t* meta,
                                                      const void*              expected,
                                                      size_t                   expected_bytes)
{
  const char* out = "/tmp/mdl_external_cover_chap.cbz";
  TEST_ASSERT(
    internal_export_chapter_meta(k_ra8_mdl_format_cbz, "/tmp/mdl_external_cover_chap", out, meta) ==
    k_ra8_ok);
  mdl_test_zip_reader_t reader;
  TEST_ASSERT(
    internal_test_zip_open(&reader, out, s_test_export_arena, sizeof(s_test_export_arena)));
  char embedded[64];
  TEST_ASSERT(expected_bytes <= sizeof(embedded));
  TEST_ASSERT(
    mz_zip_reader_extract_file_to_mem(&reader.zip, "cover/cover.png", embedded, expected_bytes, 0));
  TEST_ASSERT(memcmp(embedded, expected, expected_bytes) == 0);
  char comic_info[4096];
  TEST_ASSERT(internal_test_zip_text(&reader, "ComicInfo.xml", comic_info, sizeof(comic_info)));
  TEST_ASSERT(strstr(comic_info, "<PageCount>2</PageCount>") != nullptr);
  TEST_ASSERT(strstr(comic_info, "Image=\"0\" Type=\"FrontCover\"") != nullptr);
  TEST_ASSERT(internal_test_zip_close(&reader));
}

/**
 * @brief Assert canonical external-cover insertion in an EPUB.
 * @details Executes the assert external epub scenario through production interfaces and checks its observable success, rejection, and boundary results.
 * @param[in] meta Metadata record to read or update.
 * @param[in] expected Expected value for this operation.
 * @param[in] expected_bytes Expected bytes value for this operation.
 * @pre Assertions are enabled for contract verification.
 * @pre The test fixture workspace is isolated for this scenario.
 * @post Normal return means every reached contract assertion passed.
 * @post The caller receives no transferred fixture ownership.
 * @note Test helper; an assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_assert_external_epub(const mdl_export_meta_t* meta,
                                                       const void*              expected,
                                                       size_t                   expected_bytes)
{
  const char* out = "/tmp/mdl_external_cover_chap.epub";
  TEST_ASSERT(internal_export_chapter_meta(k_ra8_mdl_format_epub,
                                           "/tmp/mdl_external_cover_chap",
                                           out,
                                           meta) == k_ra8_ok);
  mdl_test_zip_reader_t reader;
  TEST_ASSERT(
    internal_test_zip_open(&reader, out, s_test_export_arena, sizeof(s_test_export_arena)));
  char embedded[64];
  TEST_ASSERT(expected_bytes <= sizeof(embedded));
  TEST_ASSERT(mz_zip_reader_extract_file_to_mem(&reader.zip,
                                                "OEBPS/cover/cover.png",
                                                embedded,
                                                expected_bytes,
                                                0));
  TEST_ASSERT(memcmp(embedded, expected, expected_bytes) == 0);
  char opf[4096];
  TEST_ASSERT(internal_test_zip_text(&reader, "OEBPS/content.opf", opf, sizeof(opf)));
  TEST_ASSERT(strstr(opf,
                     "id=\"cover-image\" href=\"cover/cover.png\" media-type=\"image/png\" "
                     "properties=\"cover-image\"") != nullptr);
  TEST_ASSERT(internal_test_zip_close(&reader));
}

/**
 * @brief Assert an untyped external cover fails without publication.
 * @details Executes the assert external cover rejected scenario through production interfaces and checks its observable success, rejection, and boundary results.
 * @param[in,out] meta Metadata record to read or update.
 * @pre Assertions are enabled for contract verification.
 * @pre The test fixture workspace is isolated for this scenario.
 * @post Normal return means every reached contract assertion passed.
 * @post The caller receives no transferred fixture ownership.
 * @note Test helper; an assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_assert_external_cover_rejected(mdl_export_meta_t* meta)
{
  const char* bad_path = "/tmp/mdl_series_cover_bad.jpg";
  const char* bad_out  = "/tmp/mdl_external_cover_bad.cbz";
  internal_write_fixture(bad_path, 'x');
  (void)__builtin_snprintf(meta->cover_path, sizeof(meta->cover_path), "%s", bad_path);
  (void)unlink(bad_out);
  TEST_ASSERT(internal_export_chapter_meta(k_ra8_mdl_format_cbz,
                                           "/tmp/mdl_external_cover_chap",
                                           bad_out,
                                           meta) == k_ra8_err_validation_failed);
  TEST_ASSERT(access(bad_out, F_OK) != 0);
}

/**
 * @test An external series cover is typed by bytes and embedded canonically.
 * @brief Exercise the external cover embedding regression scenario.
 * @details Executes the external cover embedding scenario through production interfaces and checks its observable success, rejection, and boundary results.
 * @pre Assertions are enabled for contract verification.
 * @pre The test fixture workspace is isolated for this scenario.
 * @post Normal return means every reached contract assertion passed.
 * @post The caller receives no transferred fixture ownership.
 * @note Test helper; an assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_external_cover_embedding(void)
{
  TEST_BEGIN("external cover embedding");
  static const char cover_bytes[] = "\x89PNG\r\n\x1a\nseries-cover";
  const char*       dir           = "/tmp/mdl_external_cover_chap";
  const char*       cover_path    = "/tmp/mdl_series_cover.jpg";
  (void)mkdir(dir, (mode_t)k_mdl_test_dir_mode);
  internal_write_fixture("/tmp/mdl_external_cover_chap/page_001.jpg", 'p');
  internal_write_binary_fixture(cover_path, cover_bytes, sizeof(cover_bytes) - 1U);
  mdl_export_meta_t meta;
  mdl_meta_init(&meta);
  (void)__builtin_snprintf(meta.cover_path, sizeof(meta.cover_path), "%s", cover_path);
  internal_assert_external_cbz(&meta, cover_bytes, sizeof(cover_bytes) - 1U);
  internal_assert_external_epub(&meta, cover_bytes, sizeof(cover_bytes) - 1U);
  internal_assert_external_cover_rejected(&meta);
  (void)unlink("/tmp/mdl_external_cover_chap/page_001.jpg");
  (void)unlink(cover_path);
  (void)unlink("/tmp/mdl_series_cover_bad.jpg");
  (void)unlink("/tmp/mdl_external_cover_chap.cbz");
  (void)unlink("/tmp/mdl_external_cover_chap.epub");
  (void)rmdir(dir);
  TEST_END("external cover embedding");
}

/**
 * @brief Assert one successful magic or Content-Type classification.
 * @details Executes the assert image type scenario through production interfaces and checks its observable success, rejection, and boundary results.
 * @param[in] bytes Bytes value for this operation.
 * @param[in] count Count value for this operation.
 * @param[in] content_type Content type value for this operation.
 * @param[in] expected_ext Expected ext value for this operation.
 * @param[in] expected_mime Expected mime value for this operation.
 * @pre Assertions are enabled for contract verification.
 * @pre The test fixture workspace is isolated for this scenario.
 * @post Normal return means every reached contract assertion passed.
 * @post The caller receives no transferred fixture ownership.
 * @note Test helper; an assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_assert_image_type(const uint8_t* bytes,
                                                    size_t         count,
                                                    const char*    content_type,
                                                    const char*    expected_ext,
                                                    const char*    expected_mime)
{
  char ext[16];
  char mime[32];
  TEST_ASSERT(
    mdl_urlname_sniff_image_type(bytes, count, content_type, ext, sizeof(ext), mime, sizeof(mime)));
  TEST_ASSERT(strcmp(ext, expected_ext) == 0);
  TEST_ASSERT(strcmp(mime, expected_mime) == 0);
}

/**
 * @test internal_test_image_magic_bytes
 * @brief Unit tests for image magic and MIME typing, including BMP consistency.
 * @details Executes the image magic bytes scenario through production interfaces and checks its observable success, rejection, and boundary results.
 * @pre Assertions are enabled for contract verification.
 * @pre The test fixture workspace is isolated for this scenario.
 * @post Normal return means every reached contract assertion passed.
 * @post The caller receives no transferred fixture ownership.
 * @note Test helper; an assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_image_magic_bytes(void)
{
  TEST_BEGIN("image magic byte & Content-Type typing");
  static const uint8_t jpeg[]  = {0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10};
  static const uint8_t png[]   = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  static const uint8_t webp[]  = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'E', 'B', 'P'};
  static const uint8_t gif87[] = {'G', 'I', 'F', '8', '7', 'a', 0, 0};
  static const uint8_t gif89[] = {'G', 'I', 'F', '8', '9', 'a', 0, 0};
  static const uint8_t bmp[]   = {'B', 'M', 0, 0, 0, 0};
  internal_assert_image_type(jpeg, sizeof(jpeg), nullptr, "jpg", "image/jpeg");
  internal_assert_image_type(png, sizeof(png), nullptr, "png", "image/png");
  internal_assert_image_type(webp, sizeof(webp), nullptr, "webp", "image/webp");
  internal_assert_image_type(gif87, sizeof(gif87), nullptr, "gif", "image/gif");
  internal_assert_image_type(gif89, sizeof(gif89), nullptr, "gif", "image/gif");
  internal_assert_image_type(bmp, sizeof(bmp), nullptr, "bmp", "image/bmp");
  internal_assert_image_type(nullptr, 0U, "image/jpeg", "jpg", "image/jpeg");
  internal_assert_image_type(nullptr, 0U, "image/png; charset=utf-8", "png", "image/png");
  internal_assert_image_type(nullptr, 0U, "IMAGE/WEBP", "webp", "image/webp");
  internal_assert_image_type(nullptr, 0U, "image/gif", "gif", "image/gif");
  internal_assert_image_type(nullptr, 0U, "image/bmp", "bmp", "image/bmp");
  internal_assert_image_type(png, sizeof(png), "image/jpeg", "png", "image/png");
  static const uint8_t junk[] = {0x00, 0x00, 0x00, 0x00};
  char                 ext[16];
  char                 mime[32];
  TEST_ASSERT(!mdl_urlname_sniff_image_type(junk,
                                            sizeof(junk),
                                            "text/html",
                                            ext,
                                            sizeof(ext),
                                            mime,
                                            sizeof(mime)));
  TEST_END("image magic byte & Content-Type typing");
}

/**
 * @brief Run publication-metadata tests.
 * @return Zero after every assertion passes.
 * @retval 0 Every metadata test passed.
 * @pre Test fixtures are writable below `/tmp`.
 * @pre The unity-minimal assertion process is initialized.
 * @post Every invoked test completed.
 * @post Each test removes its scratch output.
 * @note Runs serially in one process.
 * @since 0.1.0
 */
int main(void)
{
  TEST_ASSERT_EQ(k_ra8_ok, mdl_test_storage_init());
  internal_test_meta_init_parse_load();
  internal_test_comicinfo_xml_generation();
  internal_test_epub_metadata_and_uuid();
  internal_test_external_cover_embedding();
  internal_test_image_magic_bytes();
  TEST_ASSERT_EQ(k_ra8_ok, mdl_test_storage_deinit());
  (void)write(STDERR_FILENO,
              "[OK  ] test_media_dl_metadata.c\n",
              sizeof("[OK  ] test_media_dl_metadata.c\n") - 1U);
  return 0;
}
