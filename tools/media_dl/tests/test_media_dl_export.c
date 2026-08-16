/**
 * @file test_media_dl_export.c
 * @brief Host round-trip and hardening tests for chapter containers.
 *
 * @details Exercises CBZ, CBT, EPUB, and JOF output structure, page filtering,
 * bounded workspace refusal, archive validation, and adversarial names.
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
#include "mdl_sanitize.h"
#include "mdl_test_storage.h"
#include "mdl_verify.h"
#include "miniz.h"
#include "ra8_jof.h"
#include "test_media_dl_export_xml_internal.h"
#include "tiny_jpeg_fixture.h"
#include "unity_minimal.h"

/** @brief Permission bits for test scratch directories. */
typedef enum : uint16_t {
  k_mdl_test_dir_mode = 0755U, /**< rwxr-xr-x. */
} mdl_export_test_mode_t;

/** @brief Expected archive fixture sizes and offsets. */
typedef enum : uint16_t {
  k_expect_pages      = 2,    /**< Pages written into the export fixture. */
  k_fixture_bytes     = 4,    /**< Bytes per synthetic page file.         */
  k_name_probe        = 256,  /**< ZIP entry-name probe buffer.           */
  k_epub_min_entries  = 6,    /**< Minimum required EPUB entries.         */
  k_tar_fixture_bytes = 4096, /**< Complete synthetic tar probe bytes.    */
  k_tar_member_stride = 1024, /**< Offset between expected tar members.   */
  k_tar_data_offset   = 512,  /**< Payload offset inside a tar member.    */
} mdl_export_test_expect_t;

/** @brief Named capacities for export hardening vectors. */
typedef enum : uint32_t {
  k_longname_len            = 120U,               /**< Name exceeding ustar's field.   */
  k_buf_256                 = 256U,               /**< Medium path buffer.             */
  k_buf_320                 = 320U,               /**< EPUB entry path buffer.         */
  k_buf_512                 = 512U,               /**< Large path buffer.              */
  k_buf_2k                  = 2048U,              /**< Escaped-name probe buffer.      */
  k_buf_4k                  = 4096U,              /**< Long escaped-href probe buffer. */
  k_long_amp_run            = 240U,               /**< Ampersands in long-name probe.  */
  k_amp_esc_len             = 5U,                 /**< Bytes in `&amp;`.               */
  k_names_bytes             = 2048U * 256U,       /**< Complete page-name table.       */
  k_test_export_arena_bytes = 8U * 1024U * 1024U, /**< Test exporter arena.            */
} mdl_export_test_bound_t;

/** @brief Caller-owned workspace backing every export in this process. */
static uint8_t s_test_export_arena[k_test_export_arena_bytes];

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

/**
 * @brief Write one complete byte span through a raw hosted descriptor.
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
 * @brief Read one complete bounded fixture into caller storage.
 * @param[in] path Absolute fixture path.
 * @param[out] destination Destination bytes.
 * @param[in] capacity Writable destination extent.
 * @param[out] out_length Exact file extent on success.
 * @return Whether the regular file fit and was read completely.
 * @pre Output pointers are non-NULL and @p destination spans @p capacity bytes.
 * @post Success initializes exactly `*out_length` bytes.
 * @note Test-only POSIX fixture helper.
 * @since 0.1.0
 * @details Implements this test-only seam with caller-owned fixtures, bounded storage, and explicit propagation of the result observed by its caller.
 * @retval true The requested fixture condition succeeded.
 * @retval false The helper rejected or could not complete the condition.
 * @pre Pointer arguments satisfy their documented readable and writable extents.
 * @post Documented outputs reflect the processed fixture on success.
 */
RA8_INTERNAL static bool
internal_read_file(const char* path, uint8_t* destination, size_t capacity, size_t* out_length)
{
  const int descriptor = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (descriptor < 0) {
    return false;
  }
  const off_t end = lseek(descriptor, 0, SEEK_END);
  bool        ok =
    (end >= 0) && ((uint64_t)end <= (uint64_t)capacity) && (lseek(descriptor, 0, SEEK_SET) == 0);
  size_t offset = 0U;
  while (ok && (offset < (size_t)end)) {
    const ssize_t got = read(descriptor, &destination[offset], (size_t)end - offset);
    if (got > 0) {
      offset += (size_t)got;
    } else if ((got < 0) && (errno == EINTR)) {
      continue;
    } else {
      ok = false;
    }
  }
  ok = (close(descriptor) == 0) && ok;
  if (ok) {
    *out_length = offset;
  }
  return ok;
}

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
 * @brief Open a ZIP through raw reads and the caller-owned exporter arena.
 * @param[out] reader Reader state to initialize.
 * @param[in] path Artifact path.
 * @return Whether the descriptor and miniz reader initialized.
 * @pre @p reader is inactive and @p path is NUL-terminated.
 * @post Success leaves one active reader; failure retains no descriptor.
 * @note Test-only and non-reentrant because it borrows the shared arena.
 * @since 0.1.0
 * @details Exercises the zip open scenario through production media-downloader interfaces and checks its observable success, rejection, and boundary results.
 * @retval true The requested fixture condition succeeded.
 * @retval false The helper rejected or could not complete the condition.
 * @pre The host test process exclusively owns its fixture state.
 * @post Normal return means every scenario assertion passed.
 */
RA8_INTERNAL static bool internal_test_zip_open(mdl_test_zip_reader_t* reader, const char* path)
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
  mdl_export_workspace_init(&reader->workspace, s_test_export_arena, sizeof(s_test_export_arena));
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
 * @param[in] name Exact archive member name.
 * @param[out] destination Text destination.
 * @param[in] capacity Destination capacity including NUL.
 * @return Whether the complete member fit and was extracted.
 * @pre Every pointer is non-NULL and @p capacity is nonzero.
 * @post Success initializes one NUL-terminated member body.
 * @note Binary members are unsupported by this text helper.
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
 * @brief Perform the export chapter step.
 * @details Executes the export chapter scenario through production interfaces and checks its observable success, rejection, and boundary results.
 * @param[in] fmt Selected output format.
 * @param[in] chapter_dir Chapter dir value for this operation.
 * @param[in] out_path Out path value for this operation.
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
RA8_INTERNAL static ra8_err_t
internal_export_chapter(mdl_format_t fmt, const char* chapter_dir, const char* out_path)
{
  mdl_export_workspace_t ws;
  mdl_export_workspace_init(&ws, s_test_export_arena, sizeof(s_test_export_arena));
  return mdl_export_chapter_ws(mdl_test_storage_get(), fmt, chapter_dir, out_path, &ws);
}

/**
 * @brief Perform the verify file step.
 * @details Executes the verify file scenario through production interfaces and checks its observable success, rejection, and boundary results.
 * @param[in] fmt Selected output format.
 * @param[in] path Filesystem path for the operation.
 * @param[out] report Report value for this operation.
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
RA8_INTERNAL static ra8_err_t
internal_verify_file(mdl_format_t fmt, const char* path, mdl_verify_report_t* report)
{
  mdl_export_workspace_t ws;
  mdl_export_workspace_init(&ws, s_test_export_arena, sizeof(s_test_export_arena));
  return mdl_verify_file(mdl_test_storage_get(), fmt, path, &ws, report);
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
RA8_INTERNAL static ra8_err_t internal_export_chapter_meta(mdl_format_t             fmt,
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
 * @test Export a folder to CBZ, then re-open it with the miniz reader.
 * @brief Exercise the export cbz roundtrip regression scenario.
 * @details Executes the export cbz roundtrip scenario through production interfaces and checks its observable success, rejection, and boundary results.
 * @pre Assertions are enabled for contract verification.
 * @pre The test fixture workspace is isolated for this scenario.
 * @post Normal return means every reached contract assertion passed.
 * @post The caller receives no transferred fixture ownership.
 * @note Test helper; an assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_export_cbz_roundtrip(void)
{
  TEST_BEGIN("export cbz round-trip");
  const char* dir = "/tmp/mdl_test_chap";
  const char* out = "/tmp/mdl_test_chap.cbz";
  (void)mkdir(dir, (mode_t)k_mdl_test_dir_mode);
  internal_write_fixture("/tmp/mdl_test_chap/page_001.jpg", 'a');
  internal_write_fixture("/tmp/mdl_test_chap/page_002.jpg", 'b');

  const ra8_err_t rc = internal_export_chapter(k_mdl_fmt_cbz, dir, out);
  TEST_ASSERT(rc == k_ra8_ok);
  mdl_verify_report_t verified = {};
  TEST_ASSERT(internal_verify_file(k_mdl_fmt_cbz, out, &verified) == k_ra8_ok);
  TEST_ASSERT(verified.page_count == k_expect_pages);

  mdl_test_zip_reader_t reader;
  TEST_ASSERT(internal_test_zip_open(&reader, out));
  TEST_ASSERT_EQ(k_expect_pages + 1U, (uint16_t)mz_zip_reader_get_num_files(&reader.zip));
  char name[k_name_probe];
  (void)mz_zip_reader_get_filename(&reader.zip, 0, name, sizeof(name));
  TEST_ASSERT(strcmp(name, "page_001.jpg") == 0);
  (void)mz_zip_reader_get_filename(&reader.zip, 2, name, sizeof(name));
  TEST_ASSERT(strcmp(name, "ComicInfo.xml") == 0);
  char page[k_fixture_bytes];
  TEST_ASSERT(
    mz_zip_reader_extract_file_to_mem(&reader.zip, "page_001.jpg", page, sizeof(page), 0));
  TEST_ASSERT(page[0] == 'a' && page[k_fixture_bytes - 1U] == 'a');
  TEST_ASSERT(internal_test_zip_close(&reader));

  (void)unlink("/tmp/mdl_test_chap/page_001.jpg");
  (void)unlink("/tmp/mdl_test_chap/page_002.jpg");
  (void)unlink(out);
  (void)rmdir(dir);
  TEST_END("export cbz round-trip");
}
/**
 * @test CBT contains sorted source bytes plus a semantic ComicInfo member.
 * @brief Exercise the export cbt structure regression scenario.
 * @details Executes the export cbt structure scenario through production interfaces and checks its observable success, rejection, and boundary results.
 * @pre Assertions are enabled for contract verification.
 * @pre The test fixture workspace is isolated for this scenario.
 * @post Normal return means every reached contract assertion passed.
 * @post The caller receives no transferred fixture ownership.
 * @note Test helper; an assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_export_cbt_structure(void)
{
  TEST_BEGIN("export cbt structure");
  const char* dir = "/tmp/mdl_cbt_chap";
  const char* out = "/tmp/mdl_cbt_chap.cbt";
  const char* gz  = "/tmp/mdl_cbt_chap.cbt.gz";
  (void)mkdir(dir, (mode_t)k_mdl_test_dir_mode);
  internal_write_fixture("/tmp/mdl_cbt_chap/page_001.jpg", 'a');
  internal_write_fixture("/tmp/mdl_cbt_chap/page_002.jpg", 'b');
  mdl_export_meta_t meta;
  mdl_meta_init(&meta);
  (void)__builtin_snprintf(meta.language, sizeof(meta.language), "fr");
  TEST_ASSERT(internal_export_chapter_meta(k_mdl_fmt_cbt, dir, out, &meta) == k_ra8_ok);
  mdl_verify_report_t verified = {};
  TEST_ASSERT(internal_verify_file(k_mdl_fmt_cbt, out, &verified) == k_ra8_ok);
  TEST_ASSERT(verified.page_count == k_expect_pages);
  TEST_ASSERT(internal_export_chapter_meta(k_mdl_fmt_cbt_gz, dir, gz, &meta) == k_ra8_ok);
  verified = (mdl_verify_report_t){};
  TEST_ASSERT(internal_verify_file(k_mdl_fmt_cbt_gz, gz, &verified) == k_ra8_ok);
  TEST_ASSERT(verified.page_count == k_expect_pages);

  uint8_t tar[k_tar_fixture_bytes];
  size_t  tar_length = 0U;
  TEST_ASSERT(internal_read_file(out, tar, sizeof(tar), &tar_length));
  TEST_ASSERT(tar_length == sizeof(tar));
  TEST_ASSERT(strcmp((const char*)&tar[0], "page_001.jpg") == 0);
  TEST_ASSERT(tar[k_tar_data_offset] == (uint8_t)'a');
  TEST_ASSERT(strcmp((const char*)&tar[k_tar_member_stride], "page_002.jpg") == 0);
  TEST_ASSERT(tar[k_tar_member_stride + k_tar_data_offset] == (uint8_t)'b');
  TEST_ASSERT(strcmp((const char*)&tar[2U * k_tar_member_stride], "ComicInfo.xml") == 0);
  const char* xml = (const char*)&tar[(2U * k_tar_member_stride) + k_tar_data_offset];
  TEST_ASSERT(strstr(xml, "<PageCount>2</PageCount>") != nullptr);
  TEST_ASSERT(strstr(xml, "<LanguageISO>fr</LanguageISO>") != nullptr);

  (void)unlink("/tmp/mdl_cbt_chap/page_001.jpg");
  (void)unlink("/tmp/mdl_cbt_chap/page_002.jpg");
  (void)unlink(out);
  (void)rmdir(dir);
  (void)unlink(gz);
  TEST_END("export cbt structure");
}

/**
 * @test Packaging ingests ONLY page images; sibling output/junk is skipped.
 * @brief Exercise the export skips non images regression scenario.
 * @details Executes the export skips non images scenario through production interfaces and checks its observable success, rejection, and boundary results.
 * @pre Assertions are enabled for contract verification.
 * @pre The test fixture workspace is isolated for this scenario.
 * @post Normal return means every reached contract assertion passed.
 * @post The caller receives no transferred fixture ownership.
 * @note Test helper; an assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_export_skips_non_images(void)
{
  TEST_BEGIN("export skips non-images");
  const char* dir = "/tmp/mdl_mixed_chap";
  const char* out = "/tmp/mdl_mixed_chap.cbz";
  (void)mkdir(dir, (mode_t)k_mdl_test_dir_mode);
  /* Two real pages... */
  internal_write_fixture("/tmp/mdl_mixed_chap/page_001.jpg", 'a');
  internal_write_fixture("/tmp/mdl_mixed_chap/page_002.PNG", 'b'); /* upper-case ext too */
  /* ...amid this tool's own prior output + OS junk, which must be ignored so a
   * re-packaged folder does not fold a non-image "page" into the archive (the
   * 0x107 the reader hits when it tries to decode one). */
  internal_write_fixture("/tmp/mdl_mixed_chap/page_001.jof", 'r');
  internal_write_fixture("/tmp/mdl_mixed_chap/mdl_mixed_chap.cbz", 'z');
  internal_write_fixture("/tmp/mdl_mixed_chap/notes.txt", 't');
  internal_write_fixture("/tmp/mdl_mixed_chap/.DS_Store", 'd');

  TEST_ASSERT(internal_export_chapter(k_mdl_fmt_cbz, dir, out) == k_ra8_ok);
  mdl_test_zip_reader_t reader;
  TEST_ASSERT(internal_test_zip_open(&reader, out));
  TEST_ASSERT_EQ(k_expect_pages + 1U, (uint16_t)mz_zip_reader_get_num_files(&reader.zip));
  char name[k_name_probe];
  (void)mz_zip_reader_get_filename(&reader.zip, 0, name, sizeof(name));
  TEST_ASSERT(strcmp(name, "page_001.jpg") == 0);
  (void)mz_zip_reader_get_filename(&reader.zip, 1, name, sizeof(name));
  TEST_ASSERT(strcmp(name, "page_002.PNG") == 0);
  TEST_ASSERT(internal_test_zip_close(&reader));

  (void)unlink("/tmp/mdl_mixed_chap/page_001.jpg");
  (void)unlink("/tmp/mdl_mixed_chap/page_002.PNG");
  (void)unlink("/tmp/mdl_mixed_chap/page_001.jof");
  (void)unlink("/tmp/mdl_mixed_chap/mdl_mixed_chap.cbz");
  (void)unlink("/tmp/mdl_mixed_chap/notes.txt");
  (void)unlink("/tmp/mdl_mixed_chap/.DS_Store");
  (void)unlink(out);
  (void)rmdir(dir);
  TEST_END("export skips non-images");
}

/**
 * @brief Read a little-endian u16 from `p`.
 * @details Executes the rd u16 scenario through production interfaces and checks its observable success, rejection, and boundary results.
 * @param[in] p P value for this operation.
 * @return The bounded result computed from the supplied input.
 * @retval other The computed result in the function's declared domain.
 * @pre Assertions are enabled for contract verification.
 * @pre The test fixture workspace is isolated for this scenario.
 * @post Normal return means every reached contract assertion passed.
 * @post The caller receives no transferred fixture ownership.
 * @note Test helper; an assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static uint16_t internal_read_u16(const uint8_t* p)
{
  return (uint16_t)((uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8U));
}

/**
 * @test Export a folder to EPUB; re-open it and assert the OCF layout.
 * @brief Exercise the export epub roundtrip regression scenario.
 * @details Executes the export epub roundtrip scenario through production interfaces and checks its observable success, rejection, and boundary results.
 * @pre Assertions are enabled for contract verification.
 * @pre The test fixture workspace is isolated for this scenario.
 * @post Normal return means every reached contract assertion passed.
 * @post The caller receives no transferred fixture ownership.
 * @note Test helper; an assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_export_epub_roundtrip(void)
{
  TEST_BEGIN("export epub round-trip");
  const char* dir = "/tmp/mdl_epub_chap";
  const char* out = "/tmp/mdl_epub_chap.epub";
  (void)mkdir(dir, (mode_t)k_mdl_test_dir_mode);
  internal_write_fixture("/tmp/mdl_epub_chap/page_001.jpg", 'a');
  internal_write_fixture("/tmp/mdl_epub_chap/page_002.jpg", 'b');
  TEST_ASSERT(internal_export_chapter(k_mdl_fmt_epub, dir, out) == k_ra8_ok);
  mdl_verify_report_t verified = {};
  TEST_ASSERT(internal_verify_file(k_mdl_fmt_epub, out, &verified) == k_ra8_ok);
  TEST_ASSERT(verified.page_count == k_expect_pages);

  mdl_test_zip_reader_t reader;
  TEST_ASSERT(internal_test_zip_open(&reader, out));
  char name[k_name_probe];
  (void)mz_zip_reader_get_filename(&reader.zip, 0, name, sizeof(name));
  TEST_ASSERT(strcmp(name, "mimetype") == 0); /* OCF: mimetype is first */
  TEST_ASSERT(mz_zip_reader_get_num_files(&reader.zip) >= (mz_uint)k_epub_min_entries);
  TEST_ASSERT(internal_test_zip_close(&reader));

  (void)unlink("/tmp/mdl_epub_chap/page_001.jpg");
  (void)unlink("/tmp/mdl_epub_chap/page_002.jpg");
  (void)unlink(out);
  (void)rmdir(dir);
  TEST_END("export epub round-trip");
}

/**
 * @test Transcode a real JPEG to JOF; assert magics + webtoon column geometry.
 * @brief Exercise the export jof roundtrip regression scenario.
 * @details Executes the export jof roundtrip scenario through production interfaces and checks its observable success, rejection, and boundary results.
 * @pre Assertions are enabled for contract verification.
 * @pre The test fixture workspace is isolated for this scenario.
 * @post Normal return means every reached contract assertion passed.
 * @post The caller receives no transferred fixture ownership.
 * @note Test helper; an assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_export_jof_roundtrip(void)
{
  TEST_BEGIN("export jof round-trip");
  const char* dir = "/tmp/mdl_jof_chap";
  const char* jpg = "/tmp/mdl_jof_chap/page_001.jpg";
  const char* jof = "/tmp/mdl_jof_chap/page_001.jof";
  (void)mkdir(dir, (mode_t)k_mdl_test_dir_mode);
  TEST_ASSERT(internal_write_bytes(jpg, s_tiny_jpeg, (size_t)s_tiny_jpeg_len));
  /* JOF is a directory-output format: it writes a `.jof` sibling per page into
   * the chapter dir and does not use out_path (no single container exists), so
   * the caller must report the directory, never a phantom archive file. */
  TEST_ASSERT(mdl_format_is_dir_output(k_mdl_fmt_jof));
  TEST_ASSERT(!mdl_format_is_dir_output(k_mdl_fmt_cbz));
  TEST_ASSERT(!mdl_format_is_dir_output(k_mdl_fmt_epub));
  TEST_ASSERT(internal_export_chapter(k_mdl_fmt_jof, dir, dir) == k_ra8_ok);
  mdl_verify_report_t verified = {};
  TEST_ASSERT(internal_verify_file(k_mdl_fmt_jof, jof, &verified) == k_ra8_ok);
  TEST_ASSERT(verified.page_count == 1U);

  /* The reported output -- the `.jof` sibling -- actually exists on disk. */
  size_t sz = 0U;
  TEST_ASSERT(internal_read_file(jof, s_test_export_arena, sizeof(s_test_export_arena), &sz));
  TEST_ASSERT(sz > (size_t)k_ra8_jof_hdr_bytes);

  const size_t mlen = (size_t)k_ra8_jof_magic_len;
  TEST_ASSERT(memcmp(s_test_export_arena + k_ra8_jof_ofs_magic, "JOF1", mlen) == 0);
  TEST_ASSERT(memcmp(s_test_export_arena + (sz - mlen), "JOFE", mlen) == 0);
  /* webtoon-native: a single full-width tile column (tile_w == width). */
  TEST_ASSERT_EQ(internal_read_u16(s_test_export_arena + k_ra8_jof_ofs_width),
                 internal_read_u16(s_test_export_arena + k_ra8_jof_ofs_tile_w));

  (void)unlink(jpg);
  (void)unlink(jof);
  (void)rmdir(dir);
  TEST_END("export jof round-trip");
}

/**
 * @test A ustar name that will not fit is rejected, a normal one packages.
 * @brief Exercise the tar rejects long name regression scenario.
 * @details Executes the tar rejects long name scenario through production interfaces and checks its observable success, rejection, and boundary results.
 * @pre Assertions are enabled for contract verification.
 * @pre The test fixture workspace is isolated for this scenario.
 * @post Normal return means every reached contract assertion passed.
 * @post The caller receives no transferred fixture ownership.
 * @note Test helper; an assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_tar_rejects_long_name(void)
{
  TEST_BEGIN("tar rejects over-long name");
  const char* dir = "/tmp/mdl_long_chap";
  const char* out = "/tmp/mdl_long_chap.cbt";
  (void)mkdir(dir, (mode_t)k_mdl_test_dir_mode);
  char name[k_buf_256];
  memset(name, 'p', (size_t)k_longname_len);
  (void)__builtin_snprintf(name + k_longname_len, sizeof(name) - (size_t)k_longname_len, ".jpg");
  char path[k_buf_320];
  (void)__builtin_snprintf(path, sizeof(path), "%s/%s", dir, name);
  internal_write_fixture(path, 'x');
  TEST_ASSERT(internal_export_chapter(k_mdl_fmt_cbt, dir, out) == k_ra8_err_invalid_size);
  (void)unlink(path);
  (void)unlink(out);
  (void)rmdir(dir);

  const char* okdir = "/tmp/mdl_okname_chap";
  const char* okout = "/tmp/mdl_okname_chap.cbt";
  (void)mkdir(okdir, (mode_t)k_mdl_test_dir_mode);
  internal_write_fixture("/tmp/mdl_okname_chap/page_001.jpg", 'x');
  TEST_ASSERT(internal_export_chapter(k_mdl_fmt_cbt, okdir, okout) == k_ra8_ok);
  (void)unlink("/tmp/mdl_okname_chap/page_001.jpg");
  (void)unlink(okout);
  (void)rmdir(okdir);
  TEST_END("tar rejects over-long name");
}

/**
 * @test A filename with XML metacharacters yields escaped, well-formed EPUB.
 * @brief Exercise the epub escapes name regression scenario.
 * @details Executes the epub escapes name scenario through production interfaces and checks its observable success, rejection, and boundary results.
 * @pre Assertions are enabled for contract verification.
 * @pre The test fixture workspace is isolated for this scenario.
 * @post Normal return means every reached contract assertion passed.
 * @post The caller receives no transferred fixture ownership.
 * @note Test helper; an assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_epub_escapes_name(void)
{
  TEST_BEGIN("epub escapes filename");
  const char* dir = "/tmp/mdl_xml_chap";
  const char* out = "/tmp/mdl_xml_chap.epub";
  const char* img = "/tmp/mdl_xml_chap/a&b<c>d.jpg";
  (void)mkdir(dir, (mode_t)k_mdl_test_dir_mode);
  internal_write_fixture(img, 'x');
  TEST_ASSERT(internal_export_chapter(k_mdl_fmt_epub, dir, out) == k_ra8_ok);

  mdl_test_zip_reader_t reader;
  TEST_ASSERT(internal_test_zip_open(&reader, out));
  char xhtml[k_buf_4k];
  TEST_ASSERT(internal_test_zip_text(&reader, "OEBPS/page_001.xhtml", xhtml, sizeof(xhtml)));
  TEST_ASSERT(strstr(xhtml, "a&amp;b&lt;c&gt;d.jpg") != nullptr); /* escaped */
  TEST_ASSERT(strstr(xhtml, "a&b<c>d.jpg") == nullptr);           /* not raw */
  char opf[k_buf_4k];
  TEST_ASSERT(internal_test_zip_text(&reader, "OEBPS/content.opf", opf, sizeof(opf)));
  TEST_ASSERT(strstr(opf, "images/a&amp;b&lt;c&gt;d.jpg") != nullptr);
  TEST_ASSERT(internal_test_zip_close(&reader));

  (void)unlink(img);
  (void)unlink(out);
  (void)rmdir(dir);
  TEST_END("epub escapes filename");
}

/**
 * @test An EPUB built from a maximal-length, XML-escaping filename is complete.
 * @brief Exercise the epub long filenames regression scenario.
 * @details A 240-`&` page name escapes to 1200+ bytes and is embedded in both
 *          the manifest fragment and the page xhtml. The pre-#308 512/1024-byte
 *          accumulators could not hold it, so the export truncated (or, after
 *          the fragment guard, failed) on a legitimate long name. Correct
 *          worst-case sizing must now package it, and the OPF must carry the
 *          whole escaped href plus its `</manifest>` / `</package>` closers --
 *          proving the manifest was not cut off mid-element.
 * @pre Assertions are enabled for contract verification.
 * @pre The test fixture workspace is isolated for this scenario.
 * @post Normal return means every reached contract assertion passed.
 * @post The caller receives no transferred fixture ownership.
 * @note Test helper; an assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_epub_long_filenames(void)
{
  TEST_BEGIN("epub long filenames");
  const char* dir = "/tmp/mdl_epublong_chap";
  const char* out = "/tmp/mdl_epublong_chap.epub";
  (void)mkdir(dir, (mode_t)k_mdl_test_dir_mode);

  /* raw name: 240 '&' + ".jpg" (244 bytes, under NAME_MAX). */
  char raw[k_buf_256];
  memset(raw, '&', (size_t)k_long_amp_run);
  memcpy(raw + k_long_amp_run, ".jpg", sizeof(".jpg"));
  /* escaped name: "&amp;" x240 + ".jpg" (1204 bytes). */
  char   esc[k_buf_2k];
  size_t p = 0U;
  for (uint16_t i = 0U; i < (uint16_t)k_long_amp_run; ++i) {
    memcpy(esc + p, "&amp;", (size_t)k_amp_esc_len);
    p += (size_t)k_amp_esc_len;
  }
  memcpy(esc + p, ".jpg", sizeof(".jpg"));

  char path[k_buf_320];
  (void)__builtin_snprintf(path, sizeof(path), "%s/%s", dir, raw);
  internal_write_fixture(path, 'x');
  TEST_ASSERT(internal_export_chapter(k_mdl_fmt_epub, dir, out) == k_ra8_ok);

  mdl_test_zip_reader_t reader;
  TEST_ASSERT(internal_test_zip_open(&reader, out));
  char opf[k_buf_4k];
  TEST_ASSERT(internal_test_zip_text(&reader, "OEBPS/content.opf", opf, sizeof(opf)));
  char href[k_buf_4k]; /* room for the "images/" prefix + a full k_buf_2k esc */
  (void)__builtin_snprintf(href, sizeof(href), "images/%s", esc);
  TEST_ASSERT(strstr(opf, href) != nullptr);          /* whole escaped name present */
  TEST_ASSERT(strstr(opf, "</manifest>") != nullptr); /* manifest not truncated     */
  TEST_ASSERT(strstr(opf, "</package>") != nullptr);  /* document closed            */
  /* the image entry is stored under the raw (unescaped) name, untruncated. */
  char zipname[k_buf_320];
  (void)__builtin_snprintf(zipname, sizeof(zipname), "OEBPS/images/%s", raw);
  TEST_ASSERT(mz_zip_reader_locate_file(&reader.zip, zipname, nullptr, 0) >= 0);
  TEST_ASSERT(internal_test_zip_close(&reader));

  (void)unlink(path);
  (void)unlink(out);
  (void)rmdir(dir);
  TEST_END("epub long filenames");
}

/**
 * @test A directory holding more than k_max_pages images fails, not truncates.
 * @brief Exercise the export page cap regression scenario.
 * @details ::list_pages fills a fixed ::k_max_pages table; before #308 it
 *          returned the capped count with no signal, so a larger chapter
 *          packaged short and successfully. It now flags the overflow and
 *          ::mdl_export_chapter fails instead of dropping the extra pages.
 * @pre Assertions are enabled for contract verification.
 * @pre The test fixture workspace is isolated for this scenario.
 * @post Normal return means every reached contract assertion passed.
 * @post The caller receives no transferred fixture ownership.
 * @note Test helper; an assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_export_page_cap(void)
{
  TEST_BEGIN("export page cap");
  const char*  dir  = "/tmp/mdl_cap_chap";
  const char*  out  = "/tmp/mdl_cap_chap.cbz";
  const size_t over = (size_t)k_max_pages + 1U;
  (void)mkdir(dir, (mode_t)k_mdl_test_dir_mode);
  for (size_t i = 0U; i < over; ++i) {
    char path[k_buf_256];
    (void)__builtin_snprintf(path, sizeof(path), "%s/page_%05zu.jpg", dir, i);
    internal_write_fixture(path, 'x');
  }
  /* One image too many -> refuse rather than package a short chapter. */
  TEST_ASSERT(internal_export_chapter(k_mdl_fmt_cbz, dir, out) == k_ra8_err_invalid_size);

  for (size_t i = 0U; i < over; ++i) {
    char path[k_buf_256];
    (void)__builtin_snprintf(path, sizeof(path), "%s/page_%05zu.jpg", dir, i);
    (void)unlink(path);
  }
  (void)unlink(out);
  (void)rmdir(dir);
  TEST_END("export page cap");
}

/**
 * @brief Assert that a bounded publication sentinel remains byte-identical.
 * @details Executes the assert fixture bytes scenario through production interfaces and checks its observable success, rejection, and boundary results.
 * @param[in] path Filesystem path for the operation.
 * @param[in] expected Expected value for this operation.
 * @pre Assertions are enabled for contract verification.
 * @pre The test fixture workspace is isolated for this scenario.
 * @post Normal return means every reached contract assertion passed.
 * @post The caller receives no transferred fixture ownership.
 * @note Test helper; an assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_assert_fixture_bytes(const char* path, int expected)
{
  uint8_t bytes[k_fixture_bytes];
  size_t  length = 0U;
  TEST_ASSERT(internal_read_file(path, bytes, sizeof(bytes), &length));
  TEST_ASSERT(length == sizeof(bytes));
  for (size_t i = 0U; i < (size_t)k_fixture_bytes; ++i) {
    TEST_ASSERT(bytes[i] == (uint8_t)expected);
  }
}

/**
 * @test Export and miniz writer arenas fail closed and report deterministic high-water.
 * @brief Exercise the export workspace bounds regression scenario.
 * @details Executes the export workspace bounds scenario through production interfaces and checks its observable success, rejection, and boundary results.
 * @pre Assertions are enabled for contract verification.
 * @pre The test fixture workspace is isolated for this scenario.
 * @post Normal return means every reached contract assertion passed.
 * @post The caller receives no transferred fixture ownership.
 * @note Test helper; an assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_export_workspace_bounds(void)
{
  TEST_BEGIN("export workspace bounds");
  char root[] = "/tmp/mdl_ws.XXXXXX";
  char dir[k_buf_512];
  char page[k_buf_512];
  char out[k_buf_512];
  char epub_out[k_buf_512];
  TEST_ASSERT(mkdtemp(root) == root);
  const int dir_len = __builtin_snprintf(dir, sizeof(dir), "%s/chapter", root);
  TEST_ASSERT((dir_len > 0) && ((size_t)dir_len < sizeof(dir)));
  const int page_len = __builtin_snprintf(page, sizeof(page), "%s/page_001.jpg", dir);
  TEST_ASSERT((page_len > 0) && ((size_t)page_len < sizeof(page)));
  const int out_len = __builtin_snprintf(out, sizeof(out), "%s/chapter.cbz", root);
  TEST_ASSERT((out_len > 0) && ((size_t)out_len < sizeof(out)));
  const int epub_len = __builtin_snprintf(epub_out, sizeof(epub_out), "%s/chapter.epub", root);
  TEST_ASSERT((epub_len > 0) && ((size_t)epub_len < sizeof(epub_out)));
  TEST_ASSERT(mkdir(dir, (mode_t)k_mdl_test_dir_mode) == 0);
  internal_write_fixture(page, 'a');

  mdl_export_workspace_t ws;
  mdl_export_workspace_init(&ws, s_test_export_arena, k_names_bytes - 1U);
  TEST_ASSERT(mdl_export_chapter_ws(mdl_test_storage_get(), k_mdl_fmt_cbz, dir, out, &ws) ==
              k_ra8_err_invalid_size);
  TEST_ASSERT(ws.high_water == 0U);

  mdl_export_workspace_init(&ws, s_test_export_arena, sizeof(s_test_export_arena));
  TEST_ASSERT(mdl_export_chapter_ws(mdl_test_storage_get(), k_mdl_fmt_cbz, dir, out, &ws) ==
              k_ra8_ok);
  const size_t cbz_high_water = ws.high_water;
  TEST_ASSERT(cbz_high_water > k_names_bytes);
  TEST_ASSERT(cbz_high_water <= sizeof(s_test_export_arena));

  internal_write_fixture(out, 'z');
  mdl_export_workspace_init(&ws, s_test_export_arena, cbz_high_water - 1U);
  TEST_ASSERT(mdl_export_chapter_ws(mdl_test_storage_get(), k_mdl_fmt_cbz, dir, out, &ws) ==
              k_ra8_err_invalid_size);
  TEST_ASSERT(ws.high_water <= cbz_high_water - 1U);
  internal_assert_fixture_bytes(out, 'z');

  mdl_export_workspace_init(&ws, s_test_export_arena, sizeof(s_test_export_arena));
  TEST_ASSERT(mdl_export_chapter_ws(mdl_test_storage_get(), k_mdl_fmt_epub, dir, epub_out, &ws) ==
              k_ra8_ok);
  const size_t epub_high_water = ws.high_water;
  TEST_ASSERT(epub_high_water > cbz_high_water);
  TEST_ASSERT(epub_high_water <= sizeof(s_test_export_arena));

  internal_write_fixture(epub_out, 'e');
  mdl_export_workspace_init(&ws, s_test_export_arena, epub_high_water - 1U);
  TEST_ASSERT(mdl_export_chapter_ws(mdl_test_storage_get(), k_mdl_fmt_epub, dir, epub_out, &ws) ==
              k_ra8_err_invalid_size);
  TEST_ASSERT(ws.high_water <= epub_high_water - 1U);
  internal_assert_fixture_bytes(epub_out, 'e');

  (void)unlink(page);
  (void)unlink(out);
  (void)unlink(epub_out);
  (void)rmdir(dir);
  (void)rmdir(root);
  TEST_END("export workspace bounds");
}

/**
 * @test Every advertised validator rejects a truncated or wrong container.
 * @brief Exercise the verify rejects truncation regression scenario.
 * @details Executes the verify rejects truncation scenario through production interfaces and checks its observable success, rejection, and boundary results.
 * @pre Assertions are enabled for contract verification.
 * @pre The test fixture workspace is isolated for this scenario.
 * @post Normal return means every reached contract assertion passed.
 * @post The caller receives no transferred fixture ownership.
 * @note Test helper; an assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_verify_rejects_truncation(void)
{
  TEST_BEGIN("verify rejects truncation");
  static const struct {
    const char*  path;   /**< Temporary malformed artifact path. */
    mdl_format_t format; /**< Expected verifier format.          */
  } cases[] = {{"/tmp/mdl_bad.cbz", k_mdl_fmt_cbz},
               {"/tmp/mdl_bad.cbt", k_mdl_fmt_cbt},
               {"/tmp/mdl_bad.cbt.gz", k_mdl_fmt_cbt_gz},
               {"/tmp/mdl_bad.epub", k_mdl_fmt_epub},
               {"/tmp/mdl_bad.jof", k_mdl_fmt_jof},
               {"/tmp/mdl_bad.rabook", k_mdl_fmt_rabook}};
  for (size_t i = 0U; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    internal_write_fixture(cases[i].path, 'x');
    mdl_verify_report_t report = {};
    TEST_ASSERT(internal_verify_file(cases[i].format, cases[i].path, &report) != k_ra8_ok);
    (void)unlink(cases[i].path);
  }
  mdl_format_t inferred = k_mdl_fmt_invalid;
  TEST_ASSERT(mdl_format_from_path("/tmp/book.cbt.gz.part", &inferred) == k_ra8_err_not_supported);
  TEST_ASSERT(inferred == k_mdl_fmt_invalid);
  TEST_END("verify rejects truncation");
}

/**
 * @brief Run chapter-container export tests.
 * @return Zero after every assertion passes.
 * @retval 0 Every export test passed.
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
  internal_test_export_cbz_roundtrip();
  internal_test_export_workspace_bounds();
  internal_test_export_cbt_structure();
  internal_test_verify_rejects_truncation();
  internal_test_export_skips_non_images();
  internal_test_export_epub_roundtrip();
  internal_test_export_jof_roundtrip();
  priv_test_mdl_export_xml_run();
  internal_test_tar_rejects_long_name();
  internal_test_epub_escapes_name();
  internal_test_epub_long_filenames();
  internal_test_export_page_cap();
  TEST_ASSERT_EQ(k_ra8_ok, mdl_test_storage_deinit());
  (void)write(STDERR_FILENO,
              "[OK  ] test_media_dl_export.c\n",
              sizeof("[OK  ] test_media_dl_export.c\n") - 1U);
  return 0;
}
