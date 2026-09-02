/**
 * @file test_mdl_rabook_verify.c
 * @brief Positive and corruption tests for the downloader's structural readers.
 *
 * @details Produces a real flat RABOOK1 with the firmware builder, wraps it with
 * the production RBKC writer, publishes it through portable storage, and proves
 * the downloader validator accepts the exact file and rejects a corrupted copy.
 * The streamed USTAR and gzip fixtures live in `test_mdl_verify_stream.c`.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "book.h"
#include "mdl_export.h"
#include "mdl_storage.h"
#include "mdl_test_storage.h"
#include "mdl_verify.h"
#include "miniz.h"
#include "ra8_attributes.h"
#include "ra8_rabook_container.h"
#include "rabook_compile_test_fixture.h"
#include "tiny_jpeg_fixture.h"
#include "unity_minimal.h"

/** @brief Small complete-book fixture capacities. */
typedef enum : uint32_t {
  k_test_rbkc_bytes         = 8192U,               /**< Complete compressed container.  */
  k_test_chunk_bytes        = 1024U,               /**< Inflated bytes per chunk.       */
  k_test_compressed_bytes   = 2048U,               /**< One compressed stream.          */
  k_test_offset_entries     = 16U,                 /**< Chunk-table entries.            */
  k_test_verify_arena_bytes = 96U * 1024U * 1024U, /**< Writer and strict-reader arena. */
  k_test_small_arena_bytes  = 1U * 1024U * 1024U,  /**< Forced profile rejection.       */
  k_test_dir_work_bytes     = 8192U,               /**< Portable directory cursor.      */
} mdl_rabook_verify_limit_t;

/** @brief Maximally aligned miniz compressor storage. */
typedef struct {
  alignas(max_align_t) tdefl_compressor compressor; /**< Production compressor state. */
} mdl_rabook_compressor_t;

/** @brief Maximally aligned portable directory workspace. */
typedef struct {
  /** @brief Opaque directory cursor state. */
  alignas(max_align_t) uint8_t bytes[k_test_dir_work_bytes];
} mdl_rabook_dir_workspace_t;

/** @brief Memory source/destination for one container write. */
typedef struct {
  const uint8_t* flat;      /**< Complete flat RABOOK1 bytes. */
  uint32_t       flat_size; /**< Flat source extent.          */
  uint8_t*       rbkc;      /**< RBKC destination bytes.      */
  uint32_t       rbkc_cap;  /**< RBKC destination capacity.   */
} mdl_rabook_memory_t;

static ra8_test_rabook_fixture_t  s_fixture;
static uint8_t                    s_rbkc[k_test_rbkc_bytes];
static uint8_t                    s_chunk[k_test_chunk_bytes];
static uint8_t                    s_compressed[k_test_compressed_bytes];
static uint64_t                   s_offsets[k_test_offset_entries];
static mdl_rabook_compressor_t    s_compressor;
static uint8_t                    s_verify_arena[k_test_verify_arena_bytes];
static uint8_t                    s_small_arena[k_test_small_arena_bytes];
static mdl_rabook_dir_workspace_t s_dir_workspace;

/**
 * @brief Copy one exact flat range. @details Bounds the interval before copying it out.
 * @param[in] opaque Bound ::mdl_rabook_memory_t. @param[in] offset Source offset. @param[out] destination Read destination. @param[in] requested Exact bytes. @param[out] out_read Copied bytes. @return Status. @retval k_ra8_ok The complete range was copied. @retval k_ra8_err_out_of_range The range exceeds the blob.
 * @pre All pointers are valid for their extents. @pre The flat fixture stays immutable.
 * @post Success initializes every requested byte. @post Failure sets @p out_read to zero. @note Test-only and synchronous. @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_flat_read(void*     opaque,
                                    uint32_t  offset,
                                    uint8_t*  destination,
                                    uint32_t  requested,
                                    uint32_t* out_read)
{
  mdl_rabook_memory_t* memory = (mdl_rabook_memory_t*)opaque;
  *out_read                   = 0U;
  if ((offset > memory->flat_size) || (requested > (memory->flat_size - offset))) {
    return k_ra8_err_out_of_range;
  }
  (void)memcpy(destination, &memory->flat[offset], requested);
  *out_read = requested;
  return k_ra8_ok;
}

/**
 * @brief Copy one exact RBKC range. @details Implements the writer's random-write contract.
 * @param[in] opaque Bound ::mdl_rabook_memory_t. @param[in] offset Destination offset. @param[in] source Source bytes. @param[in] requested Exact bytes. @param[out] out_written Copied bytes. @return Status. @retval k_ra8_ok The complete range was copied. @retval k_ra8_err_invalid_size The range exceeds its cap.
 * @pre All pointers are valid for their extents. @pre The destination stays exclusively mutable.
 * @post Success initializes the requested range. @post Failure sets @p out_written to zero. @note Test-only and synchronous. @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_rbkc_write(void*          opaque,
                                     uint64_t       offset,
                                     const uint8_t* source,
                                     uint32_t       requested,
                                     uint32_t*      out_written)
{
  mdl_rabook_memory_t* memory = (mdl_rabook_memory_t*)opaque;
  *out_written                = 0U;
  if ((offset > memory->rbkc_cap) ||
      ((uint64_t)requested > ((uint64_t)memory->rbkc_cap - offset))) {
    return k_ra8_err_invalid_size;
  }
  (void)memcpy(&memory->rbkc[(size_t)offset], source, requested);
  *out_written = requested;
  return k_ra8_ok;
}

/**
 * @brief Remove one fixture path when present. @details A missing path is already success.
 * @param[in] path Canonical fixture path. @param[in] directory Remove it as a directory. @return Status. @retval k_ra8_ok The path is now absent. @retval other The namespace operation failed.
 * @pre Test storage is initialized and @p path is non-NULL. @pre No cursor retains the node.
 * @post Success leaves the path absent. @post Unrelated entries are unchanged. @note Test-only and serial. @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_remove_path(const char* path, bool directory)
{
  mdl_storage_t* storage = mdl_test_storage_get();
  fw_fs_stat_t   stat    = {};
  ra8_err_t      error   = fw_fs_stat(&storage->fs->names, path, &stat);
  if ((error == k_ra8_ok) && stat.exists) {
    error =
      directory ? fw_fs_rmdir(&storage->fs->names, path) : fw_fs_unlink(&storage->fs->names, path);
  }
  return error;
}

/**
 * @brief Prove the private EPUB intermediate was removed. @details Accepts only the original page.
 * @param[in] directory Canonical fixture directory. @return Status. @retval k_ra8_ok Exactly the original page remains. @retval k_ra8_err_validation_failed A leaked or unexpected entry remains.
 * @pre Test storage is initialized and @p directory exists. @pre No concurrent mutation occurs.
 * @post Every entry remains unchanged. @post The cursor is closed on every opened path. @note Test-only and serial. @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_check_no_temp(const char* directory)
{
  mdl_storage_t* storage = mdl_test_storage_get();
  fw_fs_dir_t    cursor  = {};
  ra8_err_t      error   = fw_fs_dir_open(&storage->fs->names,
                                          directory,
                                          &cursor,
                                          s_dir_workspace.bytes,
                                          sizeof(s_dir_workspace.bytes));
  uint32_t       entries = 0U;
  bool           more    = true;
  while ((error == k_ra8_ok) && more) {
    fw_fs_dirent_value_t value = {};
    error                      = fw_fs_dir_next(&cursor, &value, &more);
    if ((error == k_ra8_ok) && more) {
      ++entries;
      if (strcmp(value.name, "001.jpg") != 0) {
        error = k_ra8_err_validation_failed;
      }
    }
  }
  if (cursor.is_open) {
    const ra8_err_t closed = fw_fs_dir_close(&cursor);
    if ((error == k_ra8_ok) && (closed != k_ra8_ok)) {
      error = closed;
    }
  }
  return ((error == k_ra8_ok) && (entries != 1U)) ? k_ra8_err_validation_failed : error;
}

/**
 * @brief Build the flat book and wrap it in a real RBKC. @details Runs the production writers.
 * @param[out] rbkc_size Receives the encoded extent. @return Status. @retval k_ra8_ok The container occupies @p s_rbkc. @retval other Finalization or container encoding failed.
 * @pre The shared fixture arrays are exclusively owned. @pre @p rbkc_size is writable.
 * @post Success publishes an extent no larger than @p s_rbkc. @post Failure claims nothing about the output. @note Test-only and serial. @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_build_rbkc(uint64_t* rbkc_size)
{
  ra8_rabook_ctx_t            builder   = {};
  ra8_test_rabook_roundtrip_t roundtrip = {};
  ra8_test_rabook_init(&s_fixture, &builder);
  ra8_test_rabook_populate(&s_fixture, &builder, &roundtrip, false);
  ra8_err_t error = ra8_rabook_finalize(&builder, &roundtrip.blob, &roundtrip.blob_len);

  mdl_rabook_memory_t              memory    = {.flat      = s_fixture.out,
                                                .flat_size = roundtrip.blob_len,
                                                .rbkc      = s_rbkc,
                                                .rbkc_cap  = sizeof(s_rbkc)};
  ra8_rabook_container_workspace_t workspace = {
    .input          = s_chunk,
    .compressed     = s_compressed,
    .compressor     = &s_compressor.compressor,
    .offsets        = s_offsets,
    .input_cap      = sizeof(s_chunk),
    .compressed_cap = sizeof(s_compressed),
    .compressor_cap = sizeof(s_compressor.compressor),
    .offset_cap     = (uint32_t)k_test_offset_entries,
  };
  if (error == k_ra8_ok) {
    error = ra8_rabook_container_write(internal_flat_read,
                                       &memory,
                                       roundtrip.blob_len,
                                       (uint32_t)k_test_chunk_bytes,
                                       internal_rbkc_write,
                                       &memory,
                                       &workspace,
                                       rbkc_size);
  }
  return error;
}

/**
 * @test internal_test_strict_rabook_verify
 * @brief A real RBKC passes and a one-byte-corrupted stream is rejected.
 * @details Builds through the production flat and container writers, publishes through portable
 * transactions, checks the reported counts, then flips a compressed payload byte.
 * @pre The shared fixture arrays and `/tmp` paths are exclusively owned. @pre Storage is initialized.
 * @post The valid report matches the builder fixture. @post Both temporary files are removed. @note Test-only; assertion failure terminates the process. @since 0.1.0
 */
RA8_INTERNAL static void internal_test_strict_rabook_verify(void)
{
  TEST_BEGIN("media rabook strict verify");
  uint64_t rbkc_size = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, internal_build_rbkc(&rbkc_size));
  TEST_ASSERT(rbkc_size <= UINT32_MAX);
  const char* const valid_path = "/tmp/mdl-rabook-valid.rabook";
  const char* const bad_path   = "/tmp/mdl-rabook-bad.rabook";
  TEST_ASSERT_EQ(k_ra8_ok, mdl_test_storage_publish(valid_path, s_rbkc, (uint32_t)rbkc_size));

  mdl_export_workspace_t verify_workspace;
  mdl_export_workspace_init(&verify_workspace, s_verify_arena, sizeof(s_verify_arena));
  mdl_verify_report_t report = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 mdl_verify_file(mdl_test_storage_get(),
                                 k_mdl_format_rabook,
                                 valid_path,
                                 &verify_workspace,
                                 &report));
  TEST_ASSERT_EQ(1U, report.page_count);
  TEST_ASSERT_EQ(1U, report.member_count);
  TEST_ASSERT(report.metadata_present);

  s_rbkc[(size_t)rbkc_size - 1U] ^= UINT8_C(0x01);
  TEST_ASSERT_EQ(k_ra8_ok, mdl_test_storage_publish(bad_path, s_rbkc, (uint32_t)rbkc_size));
  mdl_export_workspace_init(&verify_workspace, s_verify_arena, sizeof(s_verify_arena));
  TEST_ASSERT(mdl_verify_file(mdl_test_storage_get(),
                              k_mdl_format_rabook,
                              bad_path,
                              &verify_workspace,
                              &report) != k_ra8_ok);

  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_unlink(&mdl_test_storage_get()->fs->names, valid_path));
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_unlink(&mdl_test_storage_get()->fs->names, bad_path));
  TEST_END("media rabook strict verify");
}

/**
 * @test internal_test_rabook_writer
 * @brief A real page directory exports to strict RBKC with failure preservation.
 * @details Writes a deterministic JPEG, runs the public chapter exporter, verifies the reader
 * report, proves no private EPUB remains, then forces workspace exhaustion.
 * @pre Test storage and shared arenas are exclusively owned. @pre The `/tmp` fixtures are writable.
 * @post The writer output and all sources are removed. @post The failed export leaves the prior artifact unchanged. @note Test-only; assertion failure terminates the process. @since 0.1.0
 */
RA8_INTERNAL static void internal_test_rabook_writer(void)
{
  TEST_BEGIN("media rabook writer");
  const char* const directory = "/tmp/mdl-rabook-export";
  const char* const page      = "/tmp/mdl-rabook-export/001.jpg";
  const char* const output    = "/tmp/mdl-rabook-export.rabook";
  TEST_ASSERT_EQ(k_ra8_ok, internal_remove_path(output, false));
  TEST_ASSERT_EQ(k_ra8_ok, internal_remove_path(page, false));
  TEST_ASSERT_EQ(k_ra8_ok, internal_remove_path(directory, true));
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_mkdir(&mdl_test_storage_get()->fs->names, directory));
  TEST_ASSERT_EQ(k_ra8_ok, mdl_test_storage_publish(page, s_tiny_jpeg, s_tiny_jpeg_len));

  mdl_export_meta_t meta;
  mdl_meta_init(&meta);
  (void)memcpy(meta.series_title, "Writer Fixture", sizeof("Writer Fixture"));
  (void)memcpy(meta.chapter_title, "Chapter One", sizeof("Chapter One"));
  (void)memcpy(meta.identifier, "urn:ra8:test:rabook-writer", sizeof("urn:ra8:test:rabook-writer"));
  mdl_export_workspace_t workspace;
  mdl_export_workspace_init(&workspace, s_verify_arena, sizeof(s_verify_arena));
  TEST_ASSERT_EQ(k_ra8_ok,
                 mdl_export_chapter_meta_ws(mdl_test_storage_get(),
                                            k_mdl_format_rabook,
                                            directory,
                                            output,
                                            &meta,
                                            &workspace));
  TEST_ASSERT(workspace.high_water <= sizeof(s_verify_arena));
  TEST_ASSERT_EQ(k_ra8_ok, internal_check_no_temp(directory));

  mdl_verify_report_t report = {};
  mdl_export_workspace_init(&workspace, s_verify_arena, sizeof(s_verify_arena));
  TEST_ASSERT_EQ(
    k_ra8_ok,
    mdl_verify_file(mdl_test_storage_get(), k_mdl_format_rabook, output, &workspace, &report));
  TEST_ASSERT_EQ(1U, report.page_count);
  TEST_ASSERT_EQ(1U, report.member_count);
  TEST_ASSERT(report.metadata_present);

  mdl_export_workspace_init(&workspace, s_small_arena, sizeof(s_small_arena));
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 mdl_export_chapter_meta_ws(mdl_test_storage_get(),
                                            k_mdl_format_rabook,
                                            directory,
                                            output,
                                            &meta,
                                            &workspace));
  mdl_export_workspace_init(&workspace, s_verify_arena, sizeof(s_verify_arena));
  TEST_ASSERT_EQ(
    k_ra8_ok,
    mdl_verify_file(mdl_test_storage_get(), k_mdl_format_rabook, output, &workspace, &report));
  TEST_ASSERT_EQ(k_ra8_ok, internal_check_no_temp(directory));

  TEST_ASSERT_EQ(k_ra8_ok, internal_remove_path(output, false));
  TEST_ASSERT_EQ(k_ra8_ok, internal_remove_path(page, false));
  TEST_ASSERT_EQ(k_ra8_ok, internal_remove_path(directory, true));
  TEST_END("media rabook writer");
}

/**
 * @brief Run the RBKC verifier regressions.
 * @return Zero after all assertions pass. @retval 0 Every registered vector behaved exactly.
 * @pre The root-confined portable test storage can be initialized. @pre The assertion process is active.
 * @post Test storage is deinitialized and the fixture paths are absent. @post No ownership escapes the process. @note Host-only and serial. @since 0.1.0
 */
int main(void)
{
  TEST_ASSERT_EQ(k_ra8_ok, mdl_test_storage_init());
  internal_test_strict_rabook_verify();
  internal_test_rabook_writer();
  TEST_ASSERT_EQ(k_ra8_ok, mdl_test_storage_deinit());
  return 0;
}
