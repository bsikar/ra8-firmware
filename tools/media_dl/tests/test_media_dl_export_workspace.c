/**
 * @file test_media_dl_export_workspace.c
 * @brief Exporter workspace-capacity and publication-preservation tests.
 * @details Isolates the large bounded-arena vectors from archive round trips
 *          while exercising the same production exporter and storage binding.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "mdl_export.h"
#include "mdl_export_epub_internal.h"
#include "mdl_export_internal.h"
#include "mdl_export_io_internal.h"
#include "mdl_test_storage.h"
#include "ra8_attributes.h"
#include "support/ra8_test_file.h"
#include "support/ra8_test_file_posix.h"
#include "test_media_dl_export_workspace_internal.h"
#include "tiny_jpeg_fixture.h"
#include "unity_minimal.h"

/** @brief Workspace fixture capacities and permissions. */
typedef enum : uint32_t {
  k_workspace_fixture_bytes = 4U,                 /**< Sentinel file bytes. */
  k_workspace_path_bytes    = 512U,               /**< Absolute path bytes. */
  k_workspace_arena_bytes   = 8U * 1024U * 1024U, /**< Export arena bytes.  */
  k_workspace_dir_mode      = 0755U,              /**< rwxr-xr-x.           */
} mdl_export_workspace_test_bound_t;

/** @brief Bounded page-table geometry used by the direct writer vectors. */
typedef enum : uint8_t {
  k_workspace_page_rows     = 1U,  /**< Page rows handed to a writer directly. */
  k_workspace_row_first     = 0U,  /**< First page row.                        */
  k_workspace_epub_carved   = 4U,  /**< Accumulators before the nav document.  */
  k_workspace_epub_docs     = 5U,  /**< Bounded EPUB accumulators in total.    */
  k_workspace_source_align  = 16U, /**< Slack covering the source alignment.   */
  k_workspace_arena_refusal = 3U,  /**< Workspace refusal vectors exercised.   */
} mdl_export_workspace_page_t;

/** @brief Caller-owned arena used by the focused exporter vectors. */
static uint8_t s_workspace_arena[k_workspace_arena_bytes];

/** @brief Bounded page-name table handed straight to a container writer. */
static char s_workspace_names[k_workspace_page_rows][k_name_max];

/**
 * @brief Transactionally replace one sentinel fixture with repeated bytes.
 * @details Uses the shared symlink-safe test file seam so the vector never
 *          truncates a destination before the replacement is complete.
 * @param[in] path Absolute destination path.
 * @param[in] fill Byte repeated across the fixture.
 * @pre @p path names a bounded path below the private test root.
 * @pre Its parent directory already exists.
 * @post The destination contains exactly four copies of @p fill.
 * @post No staging sibling remains visible.
 * @note Assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_workspace_write_fixture(const char* path, uint8_t fill)
{
  uint8_t bytes[k_workspace_fixture_bytes];
  (void)memset(bytes, (int)fill, sizeof(bytes));
  const ra8_test_file_result_t result = internal_test_file_replace(path, bytes, sizeof(bytes));
  TEST_ASSERT(result.status == k_ra8_test_file_ok);
  TEST_ASSERT(result.transferred == sizeof(bytes));
  TEST_ASSERT(result.published);
}

/**
 * @brief Assert that a bounded publication sentinel remains byte-identical.
 * @details Reads through the shared mutation-detecting fixture seam and checks
 *          every byte rather than relying on file length alone.
 * @param[in] path Absolute fixture path.
 * @param[in] expected Expected byte value.
 * @pre @p path names a regular four-byte fixture.
 * @pre The caller has finished mutating the artifact.
 * @post The exact four-byte payload has been compared.
 * @post No descriptor or staging resource remains active.
 * @note Assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_workspace_assert_fixture(const char* path, uint8_t expected)
{
  uint8_t                      bytes[k_workspace_fixture_bytes];
  uint8_t                      staging[k_workspace_fixture_bytes];
  const ra8_test_file_result_t result =
    internal_test_file_read(path, bytes, sizeof(bytes), staging, sizeof(staging));
  TEST_ASSERT(result.status == k_ra8_test_file_ok);
  TEST_ASSERT(result.transferred == sizeof(bytes));
  for (size_t index = 0U; index < sizeof(bytes); ++index) {
    TEST_ASSERT(bytes[index] == expected);
  }
}

/**
 * @brief Prove the CBZ writer's deterministic workspace boundary.
 * @details Exercises refusal before page-name allocation, records the exact
 *          successful high-water mark, then proves one-byte-short atomicity.
 * @param[in] directory Chapter directory containing one page.
 * @param[in] output CBZ destination path.
 * @param[in,out] workspace Caller-owned workspace descriptor.
 * @return Exact successful CBZ high-water mark.
 * @retval nonzero Workspace bytes consumed by the complete export.
 * @pre Paths are canonical and the page fixture exists.
 * @pre ::s_workspace_arena is exclusively owned by this vector.
 * @post One successful CBZ was produced before the sentinel check.
 * @post The one-byte-short failure preserved the replacement sentinel.
 * @note Assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static size_t internal_workspace_assert_cbz(const char*             directory,
                                                         const char*             output,
                                                         mdl_export_workspace_t* workspace)
{
  const size_t name_bytes = (size_t)k_max_pages * (size_t)k_name_max;
  mdl_export_workspace_init(workspace, s_workspace_arena, name_bytes - 1U);
  TEST_ASSERT(mdl_export_chapter_ws(mdl_test_storage_get(),
                                    k_ra8_mdl_format_cbz,
                                    directory,
                                    output,
                                    workspace) == k_ra8_err_invalid_size);
  TEST_ASSERT(workspace->high_water == 0U);
  mdl_export_workspace_init(workspace, s_workspace_arena, sizeof(s_workspace_arena));
  TEST_ASSERT(mdl_export_chapter_ws(mdl_test_storage_get(),
                                    k_ra8_mdl_format_cbz,
                                    directory,
                                    output,
                                    workspace) == k_ra8_ok);
  const size_t high_water = workspace->high_water;
  TEST_ASSERT(high_water > name_bytes);
  TEST_ASSERT(high_water <= sizeof(s_workspace_arena));
  internal_workspace_write_fixture(output, (uint8_t)'z');
  mdl_export_workspace_init(workspace, s_workspace_arena, high_water - 1U);
  TEST_ASSERT(mdl_export_chapter_ws(mdl_test_storage_get(),
                                    k_ra8_mdl_format_cbz,
                                    directory,
                                    output,
                                    workspace) == k_ra8_err_invalid_size);
  TEST_ASSERT(workspace->high_water <= high_water - 1U);
  internal_workspace_assert_fixture(output, (uint8_t)'z');
  return high_water;
}

/**
 * @brief Prove the EPUB writer's deterministic workspace boundary.
 * @details Records the successful EPUB high-water mark, compares it with CBZ,
 *          then proves one-byte-short refusal preserves an existing artifact.
 * @param[in] directory Chapter directory containing one page.
 * @param[in] output EPUB destination path.
 * @param[in] cbz_high_water Previously observed CBZ workspace use.
 * @param[in,out] workspace Caller-owned workspace descriptor.
 * @pre Paths are canonical and the page fixture exists.
 * @pre @p cbz_high_water came from the matching CBZ vector.
 * @post EPUB high-water exceeds CBZ and fits the fixed arena.
 * @post The one-byte-short failure preserved the replacement sentinel.
 * @note Assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_workspace_assert_epub(const char*             directory,
                                                        const char*             output,
                                                        size_t                  cbz_high_water,
                                                        mdl_export_workspace_t* workspace)
{
  mdl_export_workspace_init(workspace, s_workspace_arena, sizeof(s_workspace_arena));
  TEST_ASSERT(mdl_export_chapter_ws(mdl_test_storage_get(),
                                    k_ra8_mdl_format_epub,
                                    directory,
                                    output,
                                    workspace) == k_ra8_ok);
  const size_t high_water = workspace->high_water;
  TEST_ASSERT(high_water > cbz_high_water);
  TEST_ASSERT(high_water <= sizeof(s_workspace_arena));
  internal_workspace_write_fixture(output, (uint8_t)'e');
  mdl_export_workspace_init(workspace, s_workspace_arena, high_water - 1U);
  TEST_ASSERT(mdl_export_chapter_ws(mdl_test_storage_get(),
                                    k_ra8_mdl_format_epub,
                                    directory,
                                    output,
                                    workspace) == k_ra8_err_invalid_size);
  TEST_ASSERT(workspace->high_water <= high_water - 1U);
  internal_workspace_assert_fixture(output, (uint8_t)'e');
}

/**
 * @brief Assert one path names nothing at all.
 * @details Checks existence through the host namespace rather than the exporter,
 *          so a refused publication cannot be masked by exporter state.
 * @param[in] path Absolute path that must be absent.
 * @pre @p path is a NUL-terminated bounded absolute path.
 * @pre The caller has finished every operation that could create it.
 * @post Normal return proves the path does not exist.
 * @post No host resource is created or retained.
 * @note Assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_workspace_assert_absent(const char* path)
{
  TEST_ASSERT(access(path, F_OK) != 0);
}

/**
 * @brief Prove every EPUB accumulator shortfall refuses before staging bytes.
 * @details Hands the EPUB writer a page table directly so the bounded XML
 *          accumulators, not the page-name table, are what the arena cannot
 *          hold. Three shortfalls are exercised: too small for the first
 *          accumulator, large enough for four of the five, and large enough for
 *          all five but with nothing left for the archive writer's own state.
 *          Each must refuse with a capacity status, stage no byte at all, and
 *          leave the existing destination untouched.
 * @param[in] directory Chapter directory containing one page.
 * @param[in] output Existing EPUB destination holding the sentinel.
 * @param[in,out] workspace Caller-owned workspace descriptor.
 * @pre Paths are canonical and the page fixture exists.
 * @pre @p output already holds the four-byte replacement sentinel.
 * @post Every refusal left the sentinel byte-identical.
 * @post No transaction stage remains active.
 * @note Assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_workspace_assert_epub_carve(const char*             directory,
                                                              const char*             output,
                                                              mdl_export_workspace_t* workspace)
{
  const size_t accumulator =
    (size_t)k_epub_base_bytes + ((size_t)k_workspace_page_rows * (size_t)k_epub_per_page_bytes);
  const size_t arenas[k_workspace_arena_refusal] = {
    accumulator - 1U,
    ((size_t)k_workspace_epub_carved * accumulator) + 1U,
    (size_t)k_workspace_epub_docs * accumulator};
  (void)__builtin_snprintf(s_workspace_names[k_workspace_row_first],
                           (size_t)k_name_max,
                           "page_001.jpg");
  for (size_t i = 0U; i < (size_t)k_workspace_arena_refusal; ++i) {
    mdl_export_output_t stage = {};
    TEST_ASSERT_EQ(
      k_ra8_ok,
      priv_mdl_export_output_begin(&stage, mdl_test_storage_get(), output, k_ra8_mdl_format_epub));
    mdl_export_workspace_init(workspace, s_workspace_arena, arenas[i]);
    TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                   priv_mdl_export_epub(mdl_test_storage_get(),
                                        directory,
                                        s_workspace_names,
                                        (size_t)k_workspace_page_rows,
                                        &stage,
                                        nullptr,
                                        workspace));
    TEST_ASSERT_EQ(0U, stage.extent);
    TEST_ASSERT_EQ(k_ra8_ok, priv_mdl_export_output_abort(&stage));
    internal_workspace_assert_fixture(output, (uint8_t)'e');
  }
}

/**
 * @brief Prove the JOF exporter refuses undecodable and unsizable pages.
 * @details A page in no format the producer decodes must be reported as
 *          unsupported before any transaction opens, and a decodable page whose
 *          producer work arena does not fit the caller workspace must be
 *          reported as a capacity failure with its opened stage aborted. Both
 *          refusals must leave no `.jof` sibling behind, because a JOF chapter
 *          is exactly the set of siblings that were published.
 * @param[in] directory Chapter directory containing the four-byte page fixture.
 * @param[in,out] workspace Caller-owned workspace descriptor.
 * @pre @p directory holds `page_001.jpg` and is writable.
 * @pre ::s_workspace_arena is exclusively owned by this vector.
 * @post Neither refused page left a sibling atlas or a staged file.
 * @post The decodable fixture page is removed again.
 * @note Assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_workspace_assert_jof(const char*             directory,
                                                       mdl_export_workspace_t* workspace)
{
  char opaque_atlas[k_workspace_path_bytes];
  char coded_page[k_workspace_path_bytes];
  char coded_atlas[k_workspace_path_bytes];
  (void)__builtin_snprintf(opaque_atlas, sizeof(opaque_atlas), "%s/page_001.jof", directory);
  (void)__builtin_snprintf(coded_page, sizeof(coded_page), "%s/page_002.jpg", directory);
  (void)__builtin_snprintf(coded_atlas, sizeof(coded_atlas), "%s/page_002.jof", directory);

  (void)__builtin_snprintf(s_workspace_names[k_workspace_row_first],
                           (size_t)k_name_max,
                           "page_001.jpg");
  mdl_export_workspace_init(workspace, s_workspace_arena, sizeof(s_workspace_arena));
  TEST_ASSERT_EQ(k_ra8_err_not_supported,
                 priv_mdl_export_jof(mdl_test_storage_get(),
                                     directory,
                                     s_workspace_names,
                                     (size_t)k_workspace_page_rows,
                                     workspace));
  internal_workspace_assert_absent(opaque_atlas);

  const ra8_test_file_result_t written =
    internal_test_file_replace(coded_page, s_tiny_jpeg, (size_t)s_tiny_jpeg_len);
  TEST_ASSERT(written.status == k_ra8_test_file_ok);
  TEST_ASSERT(written.published);
  (void)__builtin_snprintf(s_workspace_names[k_workspace_row_first],
                           (size_t)k_name_max,
                           "page_002.jpg");
  /* Exactly the encoded source fits; the producer work arena cannot. */
  mdl_export_workspace_init(workspace,
                            s_workspace_arena,
                            (size_t)s_tiny_jpeg_len + (size_t)k_workspace_source_align);
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 priv_mdl_export_jof(mdl_test_storage_get(),
                                     directory,
                                     s_workspace_names,
                                     (size_t)k_workspace_page_rows,
                                     workspace));
  internal_workspace_assert_absent(coded_atlas);
  TEST_ASSERT(unlink(coded_page) == 0);
}

/**
 * @brief Run the exporter workspace-capacity test group.
 * @details Creates one isolated page fixture, runs the CBZ, EPUB, EPUB
 *          accumulator-carve, and JOF capacity vectors against it, and removes
 *          every resulting path.
 * @pre The process-local downloader storage binding is initialized.
 * @pre The `/tmp` fixture root can be created and removed.
 * @post Normal return means every workspace assertion passed.
 * @post All temporary paths and caller-owned workspace state are released.
 * @note Test-target-private and synchronous.
 * @since 0.1.0
 */
RA8_PRIV void priv_test_mdl_export_workspace_run(void)
{
  TEST_BEGIN("export workspace bounds");
  char root[] = "/tmp/mdl_ws.XXXXXX";
  char directory[k_workspace_path_bytes];
  char page[k_workspace_path_bytes];
  char cbz[k_workspace_path_bytes];
  char epub[k_workspace_path_bytes];
  TEST_ASSERT(mkdtemp(root) == root);
  const int directory_length = __builtin_snprintf(directory, sizeof(directory), "%s/chapter", root);
  const int page_length      = __builtin_snprintf(page, sizeof(page), "%s/page_001.jpg", directory);
  const int cbz_length       = __builtin_snprintf(cbz, sizeof(cbz), "%s/chapter.cbz", root);
  const int epub_length      = __builtin_snprintf(epub, sizeof(epub), "%s/chapter.epub", root);
  TEST_ASSERT((directory_length > 0) && ((size_t)directory_length < sizeof(directory)));
  TEST_ASSERT((page_length > 0) && ((size_t)page_length < sizeof(page)));
  TEST_ASSERT((cbz_length > 0) && ((size_t)cbz_length < sizeof(cbz)));
  TEST_ASSERT((epub_length > 0) && ((size_t)epub_length < sizeof(epub)));
  TEST_ASSERT(mkdir(directory, (mode_t)k_workspace_dir_mode) == 0);
  internal_workspace_write_fixture(page, (uint8_t)'a');
  mdl_export_workspace_t workspace;
  const size_t           cbz_high_water = internal_workspace_assert_cbz(directory, cbz, &workspace);
  internal_workspace_assert_epub(directory, epub, cbz_high_water, &workspace);
  internal_workspace_assert_epub_carve(directory, epub, &workspace);
  internal_workspace_assert_jof(directory, &workspace);
  TEST_ASSERT(unlink(page) == 0);
  TEST_ASSERT(unlink(cbz) == 0);
  TEST_ASSERT(unlink(epub) == 0);
  TEST_ASSERT(rmdir(directory) == 0);
  TEST_ASSERT(rmdir(root) == 0);
  TEST_END("export workspace bounds");
}
