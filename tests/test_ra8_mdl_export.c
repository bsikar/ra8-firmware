/**
 * @file test_ra8_mdl_export.c
 * @brief Exporter parity over POSIX and real RAM/FAT/VFS storage.
 *
 * @details Runs every implemented media export format through the same domain
 * code and caller-owned workspaces, independently verifies each publication,
 * and compares byte identities across both concrete filesystem adapters.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../tools/media_dl/tests/tiny_jpeg_fixture.h"
#include "fw_if_fs_posix.h"
#include "fw_if_fs_ra8_vfs.h"
#include "mdl_export.h"
#include "mdl_hash.h"
#include "mdl_storage.h"
#include "mdl_verify.h"
#include "ra8_attributes.h"
#include "ra8_fs.h"
#include "ra8_io_blockdev.h"
#include "ra8_io_blockdev_ram.h"
#include "ra8_io_vfs.h"
#include "unity_minimal.h"

/** @brief Fixed caller-owned parity fixture capacities. */
typedef enum : uint32_t {
  k_export_disk_blocks = 2048U,              /**< One MiB FAT12 RAM disk. */
  k_export_work_bytes  = 8192U,              /**< Backend object space.   */
  k_export_arena_bytes = 8U * 1024U * 1024U, /**< Export/verify arena.    */
} mdl_export_parity_limit_t;

/** @brief Maximally aligned generic backend workspace. */
typedef union {
  max_align_t alignment;                  /**< Force maximum alignment. */
  uint8_t     bytes[k_export_work_bytes]; /**< Backend-private state.   */
} internal_export_workspace_t;

/** @brief Complete storage instance and its caller-owned operational bytes. */
typedef struct {
  mdl_storage_t               storage;                    /**< Downloader storage binding. */
  internal_export_workspace_t file;                       /**< One source-file state.      */
  internal_export_workspace_t transaction;                /**< One publication state.      */
  uint8_t                     io[k_mdl_storage_io_bytes]; /**< Stream scratch.             */
} internal_export_storage_t;

static uint8_t s_export_disk[(size_t)k_export_disk_blocks * k_ra8_io_block_size_bytes];
static uint8_t s_export_arena[k_export_arena_bytes];
static ra8_io_blockdev_ram_state_t s_export_ram_state;
static ra8_io_blockdev_t           s_export_blockdev;
static ra8_fs_backend_t            s_export_backend;
static ra8_fs_mount_t*             s_export_mount;

/**
 * @brief Bind one downloader storage object to a complete filesystem
 * @details Supplies distinct aligned file, transaction, and stream workspaces
 *          to the production storage initializer and asserts capacity errors.
 * @param[out] binding Caller-owned downloader binding and workspaces.
 * @param[in] fs Complete portable filesystem.
 * @pre Both pointers are valid and @p fs outlives @p binding.
 * @pre @p binding owns writable workspace fields with maximum alignment.
 * @post Normal return leaves one initialized exclusive storage instance.
 * @post No workspace ownership transfers from the caller.
 * @note Test helper; capacity failure asserts.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_bind_storage(internal_export_storage_t* binding,
                                               const fw_fs_t*             fs)
{
  TEST_ASSERT_EQ(k_ra8_ok,
                 mdl_storage_init(&binding->storage,
                                  fs,
                                  binding->file.bytes,
                                  sizeof(binding->file.bytes),
                                  binding->transaction.bytes,
                                  sizeof(binding->transaction.bytes),
                                  binding->io,
                                  sizeof(binding->io)));
}

/**
 * @brief Write one complete portable fixture file
 * @details Opens through the injected stream port, retries bounded short
 *          writes until complete, and asserts the final close result.
 * @param[in,out] storage Initialized exclusive storage binding.
 * @param[in] path Canonical destination path.
 * @param[in] bytes Fixture bytes.
 * @param[in] length Fixture extent.
 * @pre Inputs are valid and the parent directory exists.
 * @pre @p bytes remains readable for @p length bytes.
 * @post Normal return leaves exactly @p length bytes at @p path.
 * @post The portable file handle is closed.
 * @note Test helper; short writes are retried.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_write_fixture(mdl_storage_t* storage,
                                                const char*    path,
                                                const uint8_t* bytes,
                                                uint32_t       length)
{
  fw_fs_file_t file = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 fw_fs_open(&storage->fs->streams,
                            path,
                            k_fw_fs_open_write_truncate,
                            &file,
                            storage->file_workspace,
                            storage->file_workspace_bytes));
  uint32_t offset = 0U;
  while (offset < length) {
    uint32_t written = 0U;
    TEST_ASSERT_EQ(k_ra8_ok, fw_fs_write(&file, &bytes[offset], length - offset, &written));
    TEST_ASSERT(written > 0U);
    offset += written;
  }
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_close(&file));
}

/**
 * @brief Export, independently verify, and hash one format
 * @details Reinitializes the shared caller arena between producer and verifier,
 *          checks the canonical reader report, then hashes the published bytes.
 * @param[in,out] storage Initialized storage binding.
 * @param[in] format Selected format.
 * @param[in] output Container path or chapter directory for JOF.
 * @param[in] verify_path Exact produced artifact path.
 * @param[in] metadata Explicit deterministic metadata.
 * @return Complete artifact FNV identity.
 * @retval uint64_t Stable complete-file identity of the verified artifact.
 * @pre The canonical `/chapter` source exists and inputs are stable.
 * @pre The global test arena is exclusively owned by this sequential helper.
 * @post Normal return proves publication and independent reader acceptance.
 * @post The returned identity covers the complete published artifact.
 * @note Reuses one caller arena sequentially, never concurrently.
 * @since 0.1.0
 */
RA8_INTERNAL static uint64_t internal_export_hash(mdl_storage_t*           storage,
                                                  mdl_format_t             format,
                                                  const char*              output,
                                                  const char*              verify_path,
                                                  const mdl_export_meta_t* metadata)
{
  mdl_export_workspace_t workspace;
  mdl_export_workspace_init(&workspace, s_export_arena, sizeof(s_export_arena));
  TEST_ASSERT_EQ(
    k_ra8_ok,
    mdl_export_chapter_meta_ws(storage, format, "/chapter", output, metadata, &workspace));
  mdl_verify_report_t report = {};
  mdl_export_workspace_init(&workspace, s_export_arena, sizeof(s_export_arena));
  TEST_ASSERT_EQ(k_ra8_ok, mdl_verify_file(storage, format, verify_path, &workspace, &report));
  TEST_ASSERT_EQ(1U, report.page_count);
  uint64_t hash = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, mdl_hash_file(storage, verify_path, &hash));
  return hash;
}

/**
 * @brief Run every implemented exporter and retain artifact identities
 * @details Creates one tiny canonical page and applies identical deterministic
 *          metadata to CBZ, CBT, CBT.GZ, EPUB, and page-wise JOF.
 * @param[in,out] storage Initialized storage binding.
 * @param[out] hashes Five output identities in format-table order.
 * @pre The binding root is empty and writable.
 * @pre @p hashes provides five writable uint64 rows.
 * @post Success leaves five independently verified artifacts.
 * @post Every hash row contains its complete artifact identity.
 * @note Thread-safe only across distinct bindings and backing volumes.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_run_formats(mdl_storage_t* storage, uint64_t hashes[5])
{
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_mkdir(&storage->fs->names, "/chapter"));
  internal_write_fixture(storage, "/chapter/001.jpg", k_tiny_jpeg, k_tiny_jpeg_len);
  mdl_export_meta_t metadata;
  mdl_meta_init(&metadata);
  (void)memcpy(metadata.series_title, "Portable Series", sizeof("Portable Series"));
  (void)memcpy(metadata.chapter_title, "Portable Chapter", sizeof("Portable Chapter"));
  (void)memcpy(metadata.modified, "2026-08-15T00:00:00Z", sizeof("2026-08-15T00:00:00Z"));
  const struct {
    mdl_format_t format; /**< Selected exporter/verifier format. */
    const char*  output; /**< Exporter destination path.         */
    const char*  verify; /**< Exact published artifact path.     */
  } cases[] = {{k_mdl_fmt_cbz, "/book.cbz", "/book.cbz"},
               {k_mdl_fmt_cbt, "/book.cbt", "/book.cbt"},
               {k_mdl_fmt_cbt_gz, "/book.cbt.gz", "/book.cbt.gz"},
               {k_mdl_fmt_epub, "/book.epub", "/book.epub"},
               {k_mdl_fmt_jof, "/chapter", "/chapter/001.jof"}};
  for (size_t i = 0U; i < (sizeof(cases) / sizeof(cases[0])); ++i) {
    hashes[i] =
      internal_export_hash(storage, cases[i].format, cases[i].output, cases[i].verify, &metadata);
  }
}

/**
 * @brief Remove parity artifacts from one binding root
 * @details Deletes every known container and page sibling before removing the
 *          now-empty chapter directory through the injected name port.
 * @param[in] storage Initialized storage binding.
 * @pre The parity format suite completed successfully.
 * @pre @p storage is initialized and exclusive to cleanup.
 * @post Normal return leaves the root empty.
 * @post Every delete status is asserted rather than ignored.
 * @note Test cleanup helper.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_cleanup(mdl_storage_t* storage)
{
  static const char* const files[] = {"/book.cbz",
                                      "/book.cbt",
                                      "/book.cbt.gz",
                                      "/book.epub",
                                      "/chapter/001.jof",
                                      "/chapter/001.jpg"};
  for (size_t i = 0U; i < (sizeof(files) / sizeof(files[0])); ++i) {
    TEST_ASSERT_EQ(k_ra8_ok, fw_fs_unlink(&storage->fs->names, files[i]));
  }
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_rmdir(&storage->fs->names, "/chapter"));
}

/**
 * @brief Build the real RAM-blockdev, FAT12, and firmware-VFS test stack.
 * @details Initializes each production layer in dependency order over the
 *          caller-owned static disk and publishes one complete filesystem binding.
 * @param[out] fs Complete portable filesystem binding.
 * @param[out] state Caller-owned VFS adapter state.
 * @pre Both pointers are non-NULL and the static backing objects are unmounted.
 * @pre The static RAM disk is writable for ::k_export_disk_blocks blocks.
 * @post Normal return leaves a mounted `export` VFS binding in @p fs.
 * @post All backing objects remain caller-owned for explicit reverse-order cleanup.
 * @note Test-only composition root; every initialization status is asserted.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_bind_vfs(fw_fs_t* fs, fw_fs_ra8_vfs_state_t* state)
{
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_io_blockdev_ram_init(&s_export_blockdev,
                                          &s_export_ram_state,
                                          s_export_disk,
                                          k_export_disk_blocks,
                                          false));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_as_fs_backend(&s_export_blockdev, &s_export_backend));
  const ra8_fs_format_opts_t format = {.type = k_ra8_fs_type_fat12, .label = "MDLEXPORT"};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_format(&s_export_backend, &format));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_export_backend, &s_export_mount));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_init());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_mount("export", s_export_mount));
  const fw_fs_ra8_vfs_cfg_t cfg = {.mount_name      = "export",
                                   .mount           = s_export_mount,
                                   .removable_media = false};
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_ra8_vfs_init(fs, state, &cfg));
}

/**
 * @brief Prove exporter bytes and validation match across both real adapters.
 * @details Produces all supported export formats once through root-bound POSIX
 *          storage and once through RAM blockdev, FAT12, and firmware VFS, then
 *          compares independent verifier results and complete-file hashes.
 * @pre The POSIX adapter can create an isolated root directory.
 * @pre The caller-owned RAM disk, mount state, and exporter arena are writable.
 * @post All five formats validate and hash identically on both adapters.
 * @post Unsupported VFS replacement preserves the prior archive and all mounts
 * close.
 * @note Test-only; no power-loss durability claim is made for the VFS adapter.
 * @test Export bytes and reader semantics match across POSIX and RAM/FAT/VFS.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_export_portability(void)
{
  TEST_BEGIN("media exporter POSIX/RAM-FAT-VFS parity");
  char posix_root[] = "/tmp/mdl_export_port_XXXXXX";
  TEST_ASSERT(mkdtemp(posix_root) == posix_root);
  fw_fs_t                 posix_fs    = {};
  fw_fs_posix_state_t     posix_state = {.root_fd = -1};
  const fw_fs_posix_cfg_t posix_cfg   = {.root_path = posix_root, .removable_media = false};
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_posix_init(&posix_fs, &posix_state, &posix_cfg));
  internal_export_storage_t posix_storage = {};
  internal_bind_storage(&posix_storage, &posix_fs);

  fw_fs_t               vfs_fs    = {};
  fw_fs_ra8_vfs_state_t vfs_state = {};
  internal_bind_vfs(&vfs_fs, &vfs_state);
  internal_export_storage_t vfs_storage = {};
  internal_bind_storage(&vfs_storage, &vfs_fs);

  uint64_t posix_hashes[5] = {};
  uint64_t vfs_hashes[5]   = {};
  internal_run_formats(&posix_storage.storage, posix_hashes);
  internal_run_formats(&vfs_storage.storage, vfs_hashes);
  for (size_t i = 0U; i < (sizeof(posix_hashes) / sizeof(posix_hashes[0])); ++i) {
    TEST_ASSERT_EQ(posix_hashes[i], vfs_hashes[i]);
  }

  const uint64_t    preserved = vfs_hashes[0];
  mdl_export_meta_t metadata;
  mdl_meta_init(&metadata);
  (void)memcpy(metadata.modified, "2026-08-15T00:00:00Z", sizeof("2026-08-15T00:00:00Z"));
  mdl_export_workspace_t workspace;
  mdl_export_workspace_init(&workspace, s_export_arena, sizeof(s_export_arena));
  TEST_ASSERT_EQ(k_ra8_err_not_supported,
                 mdl_export_chapter_meta_ws(&vfs_storage.storage,
                                            k_mdl_fmt_cbz,
                                            "/chapter",
                                            "/book.cbz",
                                            &metadata,
                                            &workspace));
  uint64_t after = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, mdl_hash_file(&vfs_storage.storage, "/book.cbz", &after));
  TEST_ASSERT_EQ(preserved, after);

  internal_cleanup(&posix_storage.storage);
  internal_cleanup(&vfs_storage.storage);
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_posix_deinit(&posix_state));
  TEST_ASSERT_EQ(0, rmdir(posix_root));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_unmount("export"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(s_export_mount));
  s_export_mount = nullptr;
  TEST_END("media exporter POSIX/RAM-FAT-VFS parity");
}

int main(void)
{
  internal_test_export_portability();
  return 0;
}
