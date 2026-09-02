/**
 * @file test_mdl_export_mtime.c
 * @brief Wall-clock-independent page-timestamp derivation tests for exports.
 *
 * @details When a caller supplies no explicit `modified` field, the exporter
 * derives one by statting every selected page and keeping the newest civil
 * second. Reaching the "strictly newer than every earlier page" arm of that
 * scan needs two pages whose modification times land in different seconds --
 * something no fixture that merely writes files back to back can promise. This
 * unit pins an explicit modification time onto each page instead, so all three
 * outcomes of the scan are reached identically on a loaded and an idle host.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "mdl_export.h"
#include "mdl_export_internal.h"
#include "mdl_test_storage.h"
#include "miniz.h"
#include "ra8_attributes.h"
#include "ra8_test_file.h"
#include "ra8_test_file_posix.h"
#include "test_mdl_export_mtime_internal.h"
#include "unity_minimal.h"

/** @brief Fixture geometry for the page-timestamp derivation vectors. */
typedef enum : uint32_t {
  k_mtime_fixture_bytes = 4U,                 /**< Bytes per synthetic page.  */
  k_mtime_dir_mode      = 0755U,              /**< rwxr-xr-x scratch mode.    */
  k_mtime_opf_bytes     = 4096U,              /**< OPF text probe capacity.   */
  k_mtime_archive_bytes = 65536U,             /**< Whole-EPUB probe capacity. */
  k_mtime_arena_bytes   = 8U * 1024U * 1024U, /**< Export/read arena bytes.   */
} mdl_export_mtime_bound_t;

/**
 * @brief Modification times pinned onto the page fixtures, in epoch seconds.
 * @details Chosen far apart so the derived civil second is unambiguous, and
 *          ordered so that the middle page in sorted name order is the newest:
 *          that is what forces the scan to replace an already-valid candidate.
 *          The POSIX storage adapter converts with `gmtime_r()` and reports a
 *          zero UTC offset, so each value maps to one fixed ISO-8601 string on
 *          every host regardless of the machine's time zone.
 */
typedef enum : int64_t {
  k_mtime_epoch_first  = 1000000000, /**< page_001: 2001-09-09T01:46:40Z. */
  k_mtime_epoch_newest = 1700000000, /**< page_002: 2023-11-14T22:13:20Z. */
  k_mtime_epoch_oldest = 900000000,  /**< page_003: 1998-07-09T16:00:00Z. */
} mdl_export_mtime_epoch_t;

/** @brief Caller-owned arena backing both the writer and the reader. */
static uint8_t s_mtime_arena[k_mtime_arena_bytes];

/** @brief Bounded OPF text probe extracted from the produced EPUB. */
static char s_mtime_opf[k_mtime_opf_bytes];

/** @brief Whole published EPUB, slurped so miniz never needs host stdio. */
static uint8_t s_mtime_archive[k_mtime_archive_bytes];

/** @brief Distinct staging storage required by the shared fixture read seam. */
static uint8_t s_mtime_staging[k_mtime_archive_bytes];

/**
 * @brief Publish one page fixture and pin its modification time.
 * @details Writes the payload through the shared symlink-safe replacement seam,
 *          then stamps both timestamps with `utimensat()` and reads the value
 *          back so a filesystem that silently coarsens or ignores the request
 *          fails the vector instead of reintroducing wall-clock dependence.
 * @param[in] path Absolute page path below the private test root.
 * @param[in] seconds Modification time to pin, in epoch seconds.
 * @pre @p path is NUL-terminated and its parent directory exists.
 * @pre The host filesystem records at least whole-second modification times.
 * @post The page holds exactly ::k_mtime_fixture_bytes bytes.
 * @post `stat()` reports @p seconds as the page's modification second.
 * @note Assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_mtime_publish(const char* path, int64_t seconds)
{
  uint8_t bytes[k_mtime_fixture_bytes];
  (void)memset(bytes, (int)'x', sizeof(bytes));
  const ra8_test_file_result_t written = internal_test_file_replace(path, bytes, sizeof(bytes));
  TEST_ASSERT(written.status == k_ra8_test_file_ok);
  TEST_ASSERT(written.published);

  const struct timespec stamps[2] = {{.tv_sec = (time_t)seconds, .tv_nsec = 0},
                                     {.tv_sec = (time_t)seconds, .tv_nsec = 0}};
  TEST_ASSERT(utimensat(AT_FDCWD, path, stamps, AT_SYMLINK_NOFOLLOW) == 0);
  struct stat probe = {};
  TEST_ASSERT(stat(path, &probe) == 0);
  TEST_ASSERT((int64_t)probe.st_mtime == seconds);
}

/**
 * @brief Extract the OPF package document of an EPUB as bounded text.
 * @details Slurps the whole archive through the shared fixture read seam --
 *          this build compiles miniz with `MINIZ_NO_STDIO`, so the reader is
 *          fed from memory -- and binds it to the caller-owned exporter arena,
 *          keeping the probe as allocation-free as the production writer.
 * @param[in] path Absolute EPUB path.
 * @param[out] destination NUL-terminated OPF text destination.
 * @param[in] capacity Destination capacity, including the terminator.
 * @pre @p path names a complete EPUB produced by this process.
 * @pre @p capacity exceeds the uncompressed OPF length.
 * @post Success leaves one NUL-terminated package document in @p destination.
 * @post No miniz allocation and no open descriptor survive the call.
 * @note Assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void
internal_mtime_read_opf(const char* path, char* destination, size_t capacity)
{
  const ra8_test_file_result_t slurped = internal_test_file_read(path,
                                                                 s_mtime_archive,
                                                                 sizeof(s_mtime_archive),
                                                                 s_mtime_staging,
                                                                 sizeof(s_mtime_staging));
  TEST_ASSERT(slurped.status == k_ra8_test_file_ok);
  TEST_ASSERT(slurped.transferred > 0U);

  mz_zip_archive         zip = {};
  mdl_zip_allocator_t    allocator;
  mdl_export_workspace_t workspace;
  mdl_export_workspace_init(&workspace, s_mtime_arena, sizeof(s_mtime_arena));
  priv_mdl_zip_workspace_bind(&zip, &allocator, &workspace);
  TEST_ASSERT(mz_zip_reader_init_mem(&zip, s_mtime_archive, slurped.transferred, 0) != MZ_FALSE);

  const int index = mz_zip_reader_locate_file(&zip, "OEBPS/content.opf", nullptr, 0);
  TEST_ASSERT(index >= 0);
  mz_zip_archive_file_stat member;
  TEST_ASSERT(mz_zip_reader_file_stat(&zip, (mz_uint)index, &member) != MZ_FALSE);
  TEST_ASSERT(member.m_uncomp_size < (mz_uint64)capacity);
  const size_t length = (size_t)member.m_uncomp_size;
  TEST_ASSERT(mz_zip_reader_extract_to_mem(&zip, (mz_uint)index, destination, length, 0) !=
              MZ_FALSE);
  destination[length] = '\0';

  TEST_ASSERT(mz_zip_reader_end(&zip) != MZ_FALSE);
  priv_mdl_zip_workspace_release(&allocator);
}

/**
 * @test An absent `modified` field resolves to the newest page's civil second.
 *
 * @brief Drive every outcome of the exporter's newest-page-timestamp scan.
 *
 * @details Three pages carry pinned, deliberately out-of-order modification
 * times: the first page is old, the second is the newest of the three, and the
 * third is older than both. Walking them in sorted name order therefore takes
 * the scan through its seed, its replacement, and its rejection in one export.
 * The derived value is observed through the production EPUB writer, which is
 * the only container that publishes the field, as
 * `<meta property="dcterms:modified">`.
 *
 * @par MC/DC:
 * Decision: `if (!latest.valid || (key > latest_key))` (2 conditions)
 * - Vector 1: page_001, latest.valid=false, key=2001 -> true  (control: seeds
 *   the candidate; `key > latest_key` is not evaluated)
 * - Vector 2: page_002, latest.valid=true,  key=2023 -> true  (varies
 *   `key > latest_key` only)
 * - Vector 3: page_003, latest.valid=true,  key=1998 -> false (both conditions
 *   false)
 * Vectors 1+3 prove `!latest.valid` independently affects the outcome; vectors
 * 2+3 prove the same for `key > latest_key`. N+1 = 3 vectors for N=2
 * conditions: minimal MC/DC, and every vector is fixed by a pinned timestamp
 * rather than by how fast the host happened to write the fixtures.
 *
 * @pre The process-local downloader storage binding is initialized.
 * @pre No other vector owns the `/tmp/mdl_mtime_chap` fixture root.
 * @post The published EPUB names the newest page's UTC second.
 * @post Every fixture page, the archive, and the directory are removed.
 * @note Host-only and synchronous; assertion failure terminates the process.
 * @since 0.1.0
 */
RA8_PRIV void priv_test_mdl_export_mtime_run(void)
{
  TEST_BEGIN("export derives the newest page timestamp");
  const char* dir   = "/tmp/mdl_mtime_chap";
  const char* out   = "/tmp/mdl_mtime_chap.epub";
  const char* page  = "/tmp/mdl_mtime_chap/page_001.jpg";
  const char* newer = "/tmp/mdl_mtime_chap/page_002.jpg";
  const char* older = "/tmp/mdl_mtime_chap/page_003.jpg";
  (void)mkdir(dir, (mode_t)k_mtime_dir_mode);
  internal_mtime_publish(page, k_mtime_epoch_first);
  internal_mtime_publish(newer, k_mtime_epoch_newest);
  internal_mtime_publish(older, k_mtime_epoch_oldest);

  mdl_export_workspace_t ws;
  mdl_export_workspace_init(&ws, s_mtime_arena, sizeof(s_mtime_arena));
  TEST_ASSERT(
    mdl_export_chapter_meta_ws(mdl_test_storage_get(), k_mdl_format_epub, dir, out, nullptr, &ws) ==
    k_ra8_ok);

  internal_mtime_read_opf(out, s_mtime_opf, sizeof(s_mtime_opf));
  /* The newest page wins, so the seed and the older trailing page must not. */
  TEST_ASSERT(strstr(s_mtime_opf,
                     "<meta property=\"dcterms:modified\">2023-11-14T22:13:20Z</meta>") != nullptr);
  TEST_ASSERT(strstr(s_mtime_opf, "2001-09-09T01:46:40Z") == nullptr);
  TEST_ASSERT(strstr(s_mtime_opf, "1998-07-09T16:00:00Z") == nullptr);

  (void)unlink(page);
  (void)unlink(newer);
  (void)unlink(older);
  (void)unlink(out);
  (void)rmdir(dir);
  TEST_END("export derives the newest page timestamp");
}
