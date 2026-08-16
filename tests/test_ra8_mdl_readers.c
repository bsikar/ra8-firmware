/**
 * @file test_ra8_mdl_readers.c
 * @brief Portable downloader config and image-reader backend qualification.
 * @details Runs identical bounded, short-I/O, and fault vectors over POSIX and
 *          the real RAM block-device to FAT12 to VFS filesystem stack.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "fw_if_fs_posix.h"
#include "fw_if_fs_ra8_vfs.h"
#include "mdl_config.h"
#include "mdl_state_fs_fault.h"
#include "mdl_storage.h"
#include "mdl_urlname.h"
#include "mdl_verify.h"
#include "miniz.h"
#include "ra8_fs.h"
#include "ra8_io_blockdev.h"
#include "ra8_io_blockdev_ram.h"
#include "ra8_io_vfs.h"
#include "ra8_log.h"
#include "unity_minimal.h"

/** @brief Fixed disk, workspace, and fixture capacities. */
typedef enum : uint32_t {
  k_reader_disk_blocks = 4096U,        /**< RAM block-device capacity.       */
  k_reader_work_bytes  = 8192U,        /**< Generic backend workspace size.  */
  k_reader_io_bytes    = 64U,          /**< Deliberately small read scratch. */
  k_reader_fixture_max = 768U,         /**< Largest reader fixture.          */
  k_reader_archive_max = 8192U,        /**< Generated TAR/gzip fixture cap.  */
  k_reader_verify_work = 128U * 1024U, /**< Miniz validation arena.          */
} mdl_reader_test_limit_t;

/** @brief Maximally aligned generic backend workspace. */
typedef union {
  max_align_t alignment;                  /**< Force maximum alignment. */
  uint8_t     bytes[k_reader_work_bytes]; /**< Backend workspace bytes. */
} mdl_reader_test_workspace_t;

/** @brief Maximally aligned verifier arena. */
typedef union {
  max_align_t alignment;                   /**< Force maximum alignment. */
  uint8_t     bytes[k_reader_verify_work]; /**< Miniz workspace bytes.   */
} mdl_reader_verify_workspace_t;

/** @brief One expected image signature classification. */
typedef struct {
  const uint8_t* bytes;  /**< Complete signature bytes. */
  uint32_t       length; /**< Signature extent.         */
  const char*    ext;    /**< Expected extension.       */
  const char*    mime;   /**< Expected MIME type.       */
} mdl_reader_image_case_t;

/** @brief Expected verifier format, path, and member count. */
typedef struct {
  const char*      path;    /**< Canonical fixture path.           */
  ra8_mdl_format_t format;  /**< Expected artifact format.         */
  size_t           members; /**< Expected structural member count. */
} mdl_reader_verify_case_t;

static uint8_t s_disk[(size_t)k_reader_disk_blocks * (size_t)k_ra8_io_block_size_bytes];
static ra8_io_blockdev_ram_state_t   s_ram_state;
static ra8_io_blockdev_t             s_blockdev;
static ra8_fs_backend_t              s_backend;
static ra8_fs_mount_t*               s_mount;
static mdl_reader_test_workspace_t   s_file_work;
static mdl_reader_test_workspace_t   s_transaction_work;
static uint8_t                       s_io[k_reader_io_bytes];
static uint8_t                       s_archive[k_reader_archive_max];
static uint8_t                       s_archive_copy[k_reader_archive_max];
static mdl_reader_verify_workspace_t s_verify_work;

/** @brief Valid artifact cases shared by parity and fault vectors. */
static const mdl_reader_verify_case_t s_verify_cases[] = {
  {"/book.cbz", k_ra8_mdl_format_cbz, 2U},
  {"/book.epub", k_ra8_mdl_format_epub, 5U},
  {"/book.cbt", k_ra8_mdl_format_cbt, 2U},
  {"/book.cbt.gz", k_ra8_mdl_format_cbt_gz, 2U},
  {"/book.jof", k_ra8_mdl_format_jof, 1U},
};

/** @brief Minimal valid CBZ with one image and ComicInfo.xml. */
static const uint8_t s_cbz[] = {
  0x50, 0x4b, 0x03, 0x04, 0x0a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x64, 0x16, 0x0f, 0x5d, 0x1f, 0x08,
  0xea, 0x46, 0x02, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x70, 0x61,
  0x67, 0x65, 0x2e, 0x6a, 0x70, 0x67, 0x78, 0x0a, 0x50, 0x4b, 0x03, 0x04, 0x0a, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x64, 0x16, 0x0f, 0x5d, 0x49, 0xc2, 0x80, 0x98, 0x0d, 0x00, 0x00, 0x00, 0x0d, 0x00,
  0x00, 0x00, 0x0d, 0x00, 0x00, 0x00, 0x43, 0x6f, 0x6d, 0x69, 0x63, 0x49, 0x6e, 0x66, 0x6f, 0x2e,
  0x78, 0x6d, 0x6c, 0x3c, 0x43, 0x6f, 0x6d, 0x69, 0x63, 0x49, 0x6e, 0x66, 0x6f, 0x2f, 0x3e, 0x0a,
  0x50, 0x4b, 0x01, 0x02, 0x1e, 0x03, 0x0a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x64, 0x16, 0x0f, 0x5d,
  0x1f, 0x08, 0xea, 0x46, 0x02, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xa4, 0x81, 0x00, 0x00, 0x00, 0x00, 0x70, 0x61,
  0x67, 0x65, 0x2e, 0x6a, 0x70, 0x67, 0x50, 0x4b, 0x01, 0x02, 0x1e, 0x03, 0x0a, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x64, 0x16, 0x0f, 0x5d, 0x49, 0xc2, 0x80, 0x98, 0x0d, 0x00, 0x00, 0x00, 0x0d, 0x00,
  0x00, 0x00, 0x0d, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xa4, 0x81,
  0x28, 0x00, 0x00, 0x00, 0x43, 0x6f, 0x6d, 0x69, 0x63, 0x49, 0x6e, 0x66, 0x6f, 0x2e, 0x78, 0x6d,
  0x6c, 0x50, 0x4b, 0x05, 0x06, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x02, 0x00, 0x71, 0x00, 0x00,
  0x00, 0x60, 0x00, 0x00, 0x00, 0x00, 0x00,
};

/** @brief Minimal valid EPUB with all required members and one image. */
static const uint8_t s_epub[] = {
  0x50, 0x4b, 0x03, 0x04, 0x0a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x64, 0x16, 0x0f, 0x5d, 0x3b, 0xd1,
  0xf6, 0xef, 0x15, 0x00, 0x00, 0x00, 0x15, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x6d, 0x69,
  0x6d, 0x65, 0x74, 0x79, 0x70, 0x65, 0x61, 0x70, 0x70, 0x6c, 0x69, 0x63, 0x61, 0x74, 0x69, 0x6f,
  0x6e, 0x2f, 0x65, 0x70, 0x75, 0x62, 0x2b, 0x7a, 0x69, 0x70, 0x0a, 0x50, 0x4b, 0x03, 0x04, 0x0a,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x64, 0x16, 0x0f, 0x5d, 0xe7, 0xcc, 0x42, 0x5e, 0x0d, 0x00, 0x00,
  0x00, 0x0d, 0x00, 0x00, 0x00, 0x16, 0x00, 0x00, 0x00, 0x4d, 0x45, 0x54, 0x41, 0x2d, 0x49, 0x4e,
  0x46, 0x2f, 0x63, 0x6f, 0x6e, 0x74, 0x61, 0x69, 0x6e, 0x65, 0x72, 0x2e, 0x78, 0x6d, 0x6c, 0x3c,
  0x63, 0x6f, 0x6e, 0x74, 0x61, 0x69, 0x6e, 0x65, 0x72, 0x2f, 0x3e, 0x0a, 0x50, 0x4b, 0x03, 0x04,
  0x0a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x64, 0x16, 0x0f, 0x5d, 0x22, 0x44, 0x30, 0x55, 0x0b, 0x00,
  0x00, 0x00, 0x0b, 0x00, 0x00, 0x00, 0x11, 0x00, 0x00, 0x00, 0x4f, 0x45, 0x42, 0x50, 0x53, 0x2f,
  0x63, 0x6f, 0x6e, 0x74, 0x65, 0x6e, 0x74, 0x2e, 0x6f, 0x70, 0x66, 0x3c, 0x70, 0x61, 0x63, 0x6b,
  0x61, 0x67, 0x65, 0x2f, 0x3e, 0x0a, 0x50, 0x4b, 0x03, 0x04, 0x0a, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x64, 0x16, 0x0f, 0x5d, 0x71, 0x2c, 0x56, 0xb8, 0x07, 0x00, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00,
  0x0f, 0x00, 0x00, 0x00, 0x4f, 0x45, 0x42, 0x50, 0x53, 0x2f, 0x6e, 0x61, 0x76, 0x2e, 0x78, 0x68,
  0x74, 0x6d, 0x6c, 0x3c, 0x6e, 0x61, 0x76, 0x2f, 0x3e, 0x0a, 0x50, 0x4b, 0x03, 0x04, 0x0a, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x64, 0x16, 0x0f, 0x5d, 0x1f, 0x08, 0xea, 0x46, 0x02, 0x00, 0x00, 0x00,
  0x02, 0x00, 0x00, 0x00, 0x0e, 0x00, 0x00, 0x00, 0x4f, 0x45, 0x42, 0x50, 0x53, 0x2f, 0x70, 0x61,
  0x67, 0x65, 0x2e, 0x6a, 0x70, 0x67, 0x78, 0x0a, 0x50, 0x4b, 0x01, 0x02, 0x1e, 0x03, 0x0a, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x64, 0x16, 0x0f, 0x5d, 0x3b, 0xd1, 0xf6, 0xef, 0x15, 0x00, 0x00, 0x00,
  0x15, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
  0xa4, 0x81, 0x00, 0x00, 0x00, 0x00, 0x6d, 0x69, 0x6d, 0x65, 0x74, 0x79, 0x70, 0x65, 0x50, 0x4b,
  0x01, 0x02, 0x1e, 0x03, 0x0a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x64, 0x16, 0x0f, 0x5d, 0xe7, 0xcc,
  0x42, 0x5e, 0x0d, 0x00, 0x00, 0x00, 0x0d, 0x00, 0x00, 0x00, 0x16, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xa4, 0x81, 0x3b, 0x00, 0x00, 0x00, 0x4d, 0x45, 0x54, 0x41,
  0x2d, 0x49, 0x4e, 0x46, 0x2f, 0x63, 0x6f, 0x6e, 0x74, 0x61, 0x69, 0x6e, 0x65, 0x72, 0x2e, 0x78,
  0x6d, 0x6c, 0x50, 0x4b, 0x01, 0x02, 0x1e, 0x03, 0x0a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x64, 0x16,
  0x0f, 0x5d, 0x22, 0x44, 0x30, 0x55, 0x0b, 0x00, 0x00, 0x00, 0x0b, 0x00, 0x00, 0x00, 0x11, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xa4, 0x81, 0x7c, 0x00, 0x00, 0x00,
  0x4f, 0x45, 0x42, 0x50, 0x53, 0x2f, 0x63, 0x6f, 0x6e, 0x74, 0x65, 0x6e, 0x74, 0x2e, 0x6f, 0x70,
  0x66, 0x50, 0x4b, 0x01, 0x02, 0x1e, 0x03, 0x0a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x64, 0x16, 0x0f,
  0x5d, 0x71, 0x2c, 0x56, 0xb8, 0x07, 0x00, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x0f, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xa4, 0x81, 0xb6, 0x00, 0x00, 0x00, 0x4f,
  0x45, 0x42, 0x50, 0x53, 0x2f, 0x6e, 0x61, 0x76, 0x2e, 0x78, 0x68, 0x74, 0x6d, 0x6c, 0x50, 0x4b,
  0x01, 0x02, 0x1e, 0x03, 0x0a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x64, 0x16, 0x0f, 0x5d, 0x1f, 0x08,
  0xea, 0x46, 0x02, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x0e, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xa4, 0x81, 0xea, 0x00, 0x00, 0x00, 0x4f, 0x45, 0x42, 0x50,
  0x53, 0x2f, 0x70, 0x61, 0x67, 0x65, 0x2e, 0x6a, 0x70, 0x67, 0x50, 0x4b, 0x05, 0x06, 0x00, 0x00,
  0x00, 0x00, 0x05, 0x00, 0x05, 0x00, 0x32, 0x01, 0x00, 0x00, 0x18, 0x01, 0x00, 0x00, 0x00, 0x00,
};

/**
 * @brief Discard one host-test log byte.
 * @details Keeps expected VFS error-path logging away from target-only ITM
 * MMIO, including under address-sanitizer shadow-memory layouts.
 * @param[in] ctx Unused logger context.
 * @param[in] byte Unused emitted byte.
 * @pre The callback is installed only for this single-threaded test process. @pre Both arguments may carry arbitrary values.
 * @post No memory, filesystem, or logger state is modified. @post The supplied byte is intentionally discarded.
 * @note Thread-safe because it performs no operation. @since 0.1.0
 */
RA8_INTERNAL static void internal_reader_log_sink(void* ctx, uint8_t byte)
{
  (void)ctx;
  (void)byte;
}
/**
 * @brief Bind one real backend through the deterministic fault facade.
 * @details Initializes the shared bounded workspaces once for a backend vector
 *          and leaves all deterministic fault injection disabled.
 * @param[in] inner Real filesystem under qualification.
 * @param[out] fault Fault wrapper to initialize.
 * @param[out] storage Downloader storage binding to initialize.
 * @pre All pointers are non-NULL and shared workspaces are idle. @pre @p inner implements the complete portable filesystem contract.
 * @post The binding is ready with every fault initially disabled. @post No backend file or transaction is left open.
 * @note Unity assertions terminate the vector on setup failure. @since 0.1.0
 */
RA8_INTERNAL static void
internal_reader_bind(const fw_fs_t* inner, mdl_state_fault_fs_t* fault, mdl_storage_t* storage)
{
  TEST_ASSERT_EQ(k_ra8_ok, mdl_state_fault_fs_init(fault, inner));
  TEST_ASSERT_EQ(k_ra8_ok,
                 mdl_storage_init(storage,
                                  &fault->fs,
                                  s_file_work.bytes,
                                  sizeof(s_file_work.bytes),
                                  s_transaction_work.bytes,
                                  sizeof(s_transaction_work.bytes),
                                  s_io,
                                  sizeof(s_io)));
}
/**
 * @brief Replace one portable fixture with exact bytes.
 * @details Truncates the canonical destination and retries bounded writes until
 *          the exact caller-provided extent has been persisted.
 * @param[in,out] storage Initialized storage binding.
 * @param[in] path Canonical destination path.
 * @param[in] bytes Readable fixture bytes.
 * @param[in] length Exact fixture extent.
 * @pre All pointers are non-NULL and @p bytes covers @p length bytes. @pre @p path is canonical for the bound portable filesystem.
 * @post The destination contains exactly the supplied bytes. @post The portable handle is closed before return.
 * @note Short writes are retried even though no write fault is armed. @since 0.1.0
 */
RA8_INTERNAL static void internal_reader_write(mdl_storage_t* storage,
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
    uint32_t count = 0U;
    TEST_ASSERT_EQ(k_ra8_ok, fw_fs_write(&file, bytes + offset, length - offset, &count));
    TEST_ASSERT(count > 0U);
    offset += count;
  }
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_close(&file));
}
/**
 * @brief Exercise valid, overlong, short-read, and injected config outcomes.
 * @details Proves final-line-without-newline parsing and independently injects
 *          short reads, read failure, close failure, missing paths, and
 * overflow.
 * @param[in,out] storage Initialized fault-wrapped storage.
 * @param[in,out] fault Active deterministic fault wrapper.
 * @pre Both pointers are non-NULL and no file is open. @pre @p storage is backed by the supplied @p fault facade.
 * @post Every opened config handle was consumed, including on failure. @post The config fixture remains available for later cleanup.
 * @note Runs identically for every real backend. @since 0.1.0
 */
RA8_INTERNAL static void internal_reader_config_vectors(mdl_storage_t*        storage,
                                                        mdl_state_fault_fs_t* fault)
{
  static const uint8_t valid[] = "host = test.example\nname = Portable\nburst = 2";
  internal_reader_write(storage, "/site.conf", valid, sizeof(valid) - 1U);
  mdl_site_t site = {};
  fault->flags    = (uint32_t)k_mdl_state_fault_short_read;
  TEST_ASSERT_EQ(k_ra8_ok, mdl_config_load(storage, "/site.conf", &site));
  TEST_ASSERT(strcmp(site.host, "test.example") == 0);
  TEST_ASSERT(strcmp(site.name, "Portable") == 0);
  TEST_ASSERT_EQ((uint32_t)2U, site.burst);

  fault->flags = (uint32_t)k_mdl_state_fault_read;
  TEST_ASSERT_EQ(k_ra8_err_hw_error, mdl_config_load(storage, "/site.conf", &site));
  fault->flags = (uint32_t)k_mdl_state_fault_close;
  TEST_ASSERT_EQ(k_ra8_err_hw_error, mdl_config_load(storage, "/site.conf", &site));
  fault->flags = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, mdl_config_load(storage, "/site.conf", &site));
  TEST_ASSERT_EQ(k_ra8_err_not_found, mdl_config_load(storage, "/missing.conf", &site));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, mdl_config_load(storage, "site.conf", &site));

  uint8_t           overlong[k_reader_fixture_max];
  static const char prefix[] = "host = test.example\nname = ";
  memcpy(overlong, prefix, sizeof(prefix) - 1U);
  memset(overlong + sizeof(prefix) - 1U, 'a', sizeof(overlong) - (sizeof(prefix) - 1U));
  internal_reader_write(storage, "/long.conf", overlong, sizeof(overlong));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, mdl_config_load(storage, "/long.conf", &site));
}
/**
 * @brief Assert one magic signature under one-byte short reads.
 * @details Rewrites the shared image fixture, enables deterministic short
 * reads, and compares both portable classification outputs exactly.
 * @param[in,out] storage Initialized fault-wrapped storage.
 * @param[in,out] fault Active deterministic fault wrapper.
 * @param[in] image Expected signature and classification.
 * @pre All pointers are non-NULL and no file is open. @pre @p image describes readable magic bytes and expected constant strings.
 * @post Extension and MIME exactly match the case. @post The injected short-read flag is cleared.
 * @note The common path is rewritten for each case. @since 0.1.0
 */
RA8_INTERNAL static void internal_reader_assert_image(mdl_storage_t*                 storage,
                                                      mdl_state_fault_fs_t*          fault,
                                                      const mdl_reader_image_case_t* image)
{
  internal_reader_write(storage, "/image.bin", image->bytes, image->length);
  char ext[8]   = "old";
  char mime[32] = "old";
  fault->flags  = (uint32_t)k_mdl_state_fault_short_read;
  TEST_ASSERT_EQ(
    k_ra8_ok,
    mdl_urlname_sniff_file(storage, "/image.bin", nullptr, ext, sizeof(ext), mime, sizeof(mime)));
  fault->flags = 0U;
  TEST_ASSERT(strcmp(ext, image->ext) == 0);
  TEST_ASSERT(strcmp(mime, image->mime) == 0);
}
/**
 * @brief Exercise every signature, EOF fallback, and injected read/close error.
 * @details Qualifies JPEG, PNG, GIF, WebP, and BMP magic plus partial-magic,
 *          content-type fallback, missing-path, and canonical-path contracts.
 * @param[in,out] storage Initialized fault-wrapped storage.
 * @param[in,out] fault Active deterministic fault wrapper.
 * @pre Both pointers are non-NULL and no file is open. @pre @p storage is backed by the supplied @p fault facade.
 * @post Failed reads and closes leave classification outputs unchanged. @post Every opened image handle is consumed before return.
 * @note Runs identically for every real backend. @since 0.1.0
 */
RA8_INTERNAL static void internal_reader_image_vectors(mdl_storage_t*        storage,
                                                       mdl_state_fault_fs_t* fault)
{
  static const uint8_t jpeg[] = {0xFFU, 0xD8U, 0xFFU};
  static const uint8_t png[]  = {0x89U, 'P', 'N', 'G'};
  static const uint8_t gif[]  = {'G', 'I', 'F', '8', '9', 'a'};
  static const uint8_t webp[] = {'R', 'I', 'F', 'F', 0U, 0U, 0U, 0U, 'W', 'E', 'B', 'P'};
  static const uint8_t bmp[]  = {'B', 'M'};
  static const mdl_reader_image_case_t cases[] = {
    {.bytes = jpeg, .length = sizeof(jpeg), .ext = "jpg", .mime = "image/jpeg"},
    {.bytes = png, .length = sizeof(png), .ext = "png", .mime = "image/png"},
    {.bytes = gif, .length = sizeof(gif), .ext = "gif", .mime = "image/gif"},
    {.bytes = webp, .length = sizeof(webp), .ext = "webp", .mime = "image/webp"},
    {.bytes = bmp, .length = sizeof(bmp), .ext = "bmp", .mime = "image/bmp"},
  };
  for (size_t index = 0U; index < (sizeof(cases) / sizeof(cases[0])); ++index) {
    internal_reader_assert_image(storage, fault, &cases[index]);
  }
  const uint8_t partial[] = {0x89U, 'P', 'N'};
  internal_reader_write(storage, "/image.bin", partial, sizeof(partial));
  char ext[8]   = "keep";
  char mime[32] = "keep";
  TEST_ASSERT_EQ(
    k_ra8_err_validation_failed,
    mdl_urlname_sniff_file(storage, "/image.bin", nullptr, ext, sizeof(ext), mime, sizeof(mime)));
  TEST_ASSERT(strcmp(ext, "keep") == 0);
  TEST_ASSERT_EQ(k_ra8_ok,
                 mdl_urlname_sniff_file(storage,
                                        "/image.bin",
                                        "image/png",
                                        ext,
                                        sizeof(ext),
                                        mime,
                                        sizeof(mime)));
  TEST_ASSERT(strcmp(ext, "png") == 0);
  fault->flags = (uint32_t)k_mdl_state_fault_read;
  (void)snprintf(ext, sizeof(ext), "keep");
  TEST_ASSERT_EQ(
    k_ra8_err_hw_error,
    mdl_urlname_sniff_file(storage, "/image.bin", nullptr, ext, sizeof(ext), mime, sizeof(mime)));
  TEST_ASSERT(strcmp(ext, "keep") == 0);
  fault->flags = (uint32_t)k_mdl_state_fault_close;
  TEST_ASSERT_EQ(
    k_ra8_err_hw_error,
    mdl_urlname_sniff_file(storage, "/image.bin", nullptr, ext, sizeof(ext), mime, sizeof(mime)));
  fault->flags = 0U;
  TEST_ASSERT_EQ(
    k_ra8_err_not_found,
    mdl_urlname_sniff_file(storage, "/missing.bin", nullptr, ext, sizeof(ext), mime, sizeof(mime)));
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    mdl_urlname_sniff_file(storage, "image.bin", nullptr, ext, sizeof(ext), mime, sizeof(mime)));
}
/**
 * @brief Store one little-endian word in a generated container.
 * @details Encodes exactly four bytes without host-endian assumptions.
 * @param[out] bytes Writable four-byte destination.
 * @param[in] value Word to encode.
 * @pre @p bytes addresses at least four writable bytes. @pre The destination is not aliased by a concurrent writer.
 * @post The four destination bytes contain @p value in little-endian order. @post No byte outside that extent changes.
 * @note Fixture-only primitive with no failure path. @since 0.1.0
 */
RA8_INTERNAL static void internal_reader_put_u32le(uint8_t* bytes, uint32_t value)
{
  bytes[0] = (uint8_t)value;
  bytes[1] = (uint8_t)(value >> 8U);
  bytes[2] = (uint8_t)(value >> 16U);
  bytes[3] = (uint8_t)(value >> 24U);
}
/**
 * @brief Append one regular member to a generated TAR fixture.
 * @details Writes a checksum-valid regular-file header, payload, and zero padding.
 * @param[in,out] tar Writable archive buffer.
 * @param[in] offset First free archive byte.
 * @param[in] name NUL-terminated member name.
 * @param[in] data Readable member payload.
 * @param[in] length Payload byte count.
 * @return First aligned byte after the member.
 * @retval nonzero Updated archive offset.
 * @pre Inputs fit the fixed archive capacity. @pre Name and payload extents are valid.
 * @post One regular member is complete. @post Returned offset is 512-byte aligned.
 * @note Assertions fail the process on a broken fixture assumption. @since 0.1.0
 */
RA8_INTERNAL static size_t internal_reader_tar_member(uint8_t*       tar,
                                                      size_t         offset,
                                                      const char*    name,
                                                      const uint8_t* data,
                                                      uint32_t       length)
{
  uint8_t* header = tar + offset;
  memset(header, 0, 512U);
  memcpy(header, name, strlen(name));
  (void)snprintf((char*)header + 124U, 12U, "%011o", (unsigned)length);
  memset(header + 148U, ' ', 8U);
  header[156]       = '0';
  unsigned checksum = 0U;
  for (size_t index = 0U; index < 512U; ++index) {
    checksum += header[index];
  }
  (void)snprintf((char*)header + 148U, 7U, "%06o", checksum);
  header[154] = '\0';
  header[155] = ' ';
  offset += 512U;
  memcpy(tar + offset, data, length);
  offset += length;
  const size_t padded = (offset + 511U) & ~(size_t)511U;
  memset(tar + offset, 0, padded - offset);
  return padded;
}
/**
 * @brief Generate one valid two-member TAR and its terminal records.
 * @details Builds page and metadata members followed by the two required zero blocks.
 * @param[out] tar Writable full-size archive fixture.
 * @return Exact generated TAR length.
 * @retval nonzero Byte count including terminal records.
 * @pre @p tar has `k_reader_archive_max` writable bytes. @pre Shared fixture construction is single-threaded.
 * @post The output is a structurally valid two-member TAR. @post Unused bytes begin zeroed.
 * @note The member contents are intentionally minimal. @since 0.1.0
 */
RA8_INTERNAL static uint32_t internal_reader_make_tar(uint8_t* tar)
{
  static const uint8_t page[] = {'x'};
  static const uint8_t meta[] = "<ComicInfo/>";
  memset(tar, 0, k_reader_archive_max);
  size_t offset = internal_reader_tar_member(tar, 0U, "page.jpg", page, sizeof(page));
  offset        = internal_reader_tar_member(tar, offset, "ComicInfo.xml", meta, sizeof(meta) - 1U);
  memset(tar + offset, 0, 1024U);
  return (uint32_t)(offset + 1024U);
}
/**
 * @brief Wrap a generated TAR in one fixed-header RFC 1952 member.
 * @details Deflates the supplied TAR and appends its CRC32 and original size trailer.
 * @param[in] tar Readable TAR bytes.
 * @param[in] tar_length Exact TAR length.
 * @param[out] gzip Writable gzip destination.
 * @return Exact generated gzip length.
 * @retval nonzero Header, deflate stream, and trailer byte count.
 * @pre Input and output extents fit the archive buffers. @pre The buffers do not overlap.
 * @post Output is one complete RFC 1952 member. @post Trailer authenticates the supplied TAR.
 * @note Compression failure terminates the test through Unity. @since 0.1.0
 */
RA8_INTERNAL static uint32_t
internal_reader_make_gzip(const uint8_t* tar, uint32_t tar_length, uint8_t* gzip)
{
  const uint8_t header[10] = {0x1FU, 0x8BU, 8U, 0U, 0U, 0U, 0U, 0U, 0U, 3U};
  memcpy(gzip, header, sizeof(header));
  const size_t compressed = tdefl_compress_mem_to_mem(gzip + sizeof(header),
                                                      k_reader_archive_max - 18U,
                                                      tar,
                                                      tar_length,
                                                      TDEFL_DEFAULT_MAX_PROBES);
  TEST_ASSERT(compressed != 0U);
  const uint32_t end = (uint32_t)(sizeof(header) + compressed);
  internal_reader_put_u32le(gzip + end, (uint32_t)mz_crc32(MZ_CRC32_INIT, tar, tar_length));
  internal_reader_put_u32le(gzip + end + 4U, tar_length);
  return end + 8U;
}
/**
 * @brief Generate the smallest structurally valid one-tile raw JOF.
 * @details Emits one page, one tile byte, its index records, and the terminal marker.
 * @param[out] jof Writable JOF fixture buffer.
 * @return Exact generated JOF length.
 * @retval 57 Length of the minimal fixture.
 * @pre @p jof addresses at least 57 writable bytes. @pre Fixture generation is single-threaded.
 * @post Output passes the raw-JOF structural validator. @post Bytes beyond the fixture are untouched.
 * @note Integer fields use the local endian encoder. @since 0.1.0
 */
RA8_INTERNAL static uint32_t internal_reader_make_jof(uint8_t* jof)
{
  memset(jof, 0, 57U);
  memcpy(jof, "JOF1", 4U);
  jof[4]  = 1U;
  jof[6]  = 1U;
  jof[8]  = 1U;
  jof[10] = 1U;
  jof[12] = 1U;
  internal_reader_put_u32le(jof + 16U, 1U);
  jof[32] = 0x5AU;
  internal_reader_put_u32le(jof + 33U, 32U);
  internal_reader_put_u32le(jof + 37U, 1U);
  internal_reader_put_u32le(jof + 41U, 33U);
  internal_reader_put_u32le(jof + 45U, 1U);
  internal_reader_put_u32le(jof + 49U, 57U);
  memcpy(jof + 53U, "JOFE", 4U);
  return 57U;
}
/**
 * @brief Invoke the portable verifier with an explicitly bounded arena.
 * @details Binds the shared verifier bytes to the requested cap and publishes its high-water use.
 * @param[in,out] storage Initialized portable storage.
 * @param[in] format Expected media format.
 * @param[in] path Canonical fixture path.
 * @param[in] capacity Supplied verifier workspace bytes.
 * @param[in,out] report Atomic verification report.
 * @param[out] high_water Optional observed workspace high-water mark.
 * @return Canonical verification status.
 * @retval k_ra8_ok The complete artifact validated.
 * @retval other Storage, structure, checksum, or capacity validation failed.
 * @pre Required pointers are valid. @pre No other verifier borrows the shared workspace.
 * @post Success publishes @p report. @post When supplied, @p high_water is initialized.
 * @note The helper performs no allocation. @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_reader_verify(mdl_storage_t*       storage,
                                                     ra8_mdl_format_t     format,
                                                     const char*          path,
                                                     size_t               capacity,
                                                     mdl_verify_report_t* report,
                                                     size_t*              high_water)
{
  mdl_export_workspace_t workspace = {.data       = s_verify_work.bytes,
                                      .cap        = capacity,
                                      .used       = 0U,
                                      .high_water = 0U};
  const ra8_err_t        error     = mdl_verify_file(storage, format, path, &workspace, report);
  if (high_water != nullptr) {
    *high_water = workspace.high_water;
  }
  return error;
}
/**
 * @brief Verify through a borrowed open handle and prove ownership is retained.
 * @details Opens one fixture, snapshots its size, validates it, then queries
 *          the still-open handle before closing it explicitly in this caller.
 * @param[in,out] storage Initialized portable storage.
 * @param[in] format Expected media format.
 * @param[in] path Canonical fixture path.
 * @param[in,out] report Atomic verification report.
 * @return Canonical verification or close status.
 * @retval k_ra8_ok Validation succeeded and the caller closed its handle.
 * @retval other Open, size, validation, ownership, or close failed.
 * @pre Required pointers are valid. @pre Shared file and verifier workspaces are idle.
 * @post The borrowed handle is closed only by this caller. @post Failure leaves @p report unchanged.
 * @note Proves `mdl_verify_open_file` does not take ownership. @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_reader_verify_borrowed(mdl_storage_t*       storage,
                                                              ra8_mdl_format_t     format,
                                                              const char*          path,
                                                              mdl_verify_report_t* report)
{
  fw_fs_file_t file       = {};
  ra8_err_t    error      = fw_fs_open(&storage->fs->streams,
                                       path,
                                       k_fw_fs_open_read,
                                       &file,
                                       storage->file_workspace,
                                       storage->file_workspace_bytes);
  uint64_t     size_bytes = 0U;
  if (error == k_ra8_ok) {
    error = fw_fs_file_size(&file, &size_bytes);
  }
  mdl_export_workspace_t workspace = {.data = s_verify_work.bytes,
                                      .cap  = sizeof(s_verify_work.bytes)};
  if (error == k_ra8_ok) {
    error = mdl_verify_open_file(storage, format, &file, size_bytes, &workspace, report);
  }
  uint64_t size_after = 0U;
  if (error == k_ra8_ok) {
    error = fw_fs_file_size(&file, &size_after);
    TEST_ASSERT_EQ(size_bytes, size_after);
  }
  const ra8_err_t close_error = fw_fs_close(&file);
  return (error == k_ra8_ok) ? close_error : error;
}
/**
 * @brief Assert one failed verification leaves a sentinel report unchanged.
 * @details Seeds every report field, invokes validation, and checks exact failure atomicity.
 * @param[in,out] storage Initialized portable storage.
 * @param[in] format Expected media format.
 * @param[in] path Canonical fixture path.
 * @param[in] expected Required error status.
 * @pre Required pointers are valid. @pre The fixture intentionally violates the selected contract.
 * @post The expected error is observed. @post Every sentinel report field remains unchanged.
 * @note Assertion failure terminates the test. @since 0.1.0
 */
RA8_INTERNAL static void internal_reader_expect_error(mdl_storage_t*   storage,
                                                      ra8_mdl_format_t format,
                                                      const char*      path,
                                                      ra8_err_t        expected)
{
  mdl_verify_report_t report = {.format           = k_ra8_mdl_format_cbr,
                                .page_count       = 17U,
                                .member_count     = 19U,
                                .metadata_present = true};
  TEST_ASSERT_EQ(
    expected,
    internal_reader_verify(storage, format, path, sizeof(s_verify_work.bytes), &report, nullptr));
  TEST_ASSERT_EQ(k_ra8_mdl_format_cbr, report.format);
  TEST_ASSERT_EQ((size_t)17U, report.page_count);
  TEST_ASSERT_EQ((size_t)19U, report.member_count);
  TEST_ASSERT(report.metadata_present);
}
/**
 * @brief Replace every equal-width archive filename occurrence.
 * @details Mutates matching archive spans without shifting surrounding structure.
 * @param[in,out] bytes Mutable archive bytes.
 * @param[in] length Archive byte count.
 * @param[in] before Equal-width source spelling.
 * @param[in] after Equal-width replacement spelling.
 * @return Number of replaced occurrences.
 * @retval 0 No source spelling was present.
 * @retval nonzero Number of nonoverlapping replacements.
 * @pre Inputs are valid and strings are NUL terminated. @pre Source and replacement widths match.
 * @post Every matched span contains @p after. @post Archive length is unchanged.
 * @note Unity enforces the equal-width fixture invariant. @since 0.1.0
 */
RA8_INTERNAL static uint32_t
internal_reader_replace(uint8_t* bytes, uint32_t length, const char* before, const char* after)
{
  const size_t width = strlen(before);
  TEST_ASSERT_EQ(width, strlen(after));
  uint32_t replaced = 0U;
  for (uint32_t offset = 0U; (offset + width) <= length; ++offset) {
    if (memcmp(bytes + offset, before, width) == 0) {
      memcpy(bytes + offset, after, width);
      ++replaced;
      offset += (uint32_t)width - 1U;
    }
  }
  return replaced;
}
/**
 * @brief Write all five valid verifier formats to the active backend.
 * @details Generates TAR, gzip, and JOF fixtures beside the fixed CBZ and EPUB fixtures.
 * @param[in,out] storage Initialized writable portable storage.
 * @param[out] tar_length Exact generated TAR length.
 * @param[out] gzip_length Exact generated gzip length.
 * @pre Required pointers are valid. @pre Shared archive buffers are idle.
 * @post Five canonical paths contain valid artifacts. @post Both output lengths are initialized.
 * @note The following vectors may deliberately overwrite these fixtures. @since 0.1.0
 */
RA8_INTERNAL static void
internal_reader_write_archives(mdl_storage_t* storage, uint32_t* tar_length, uint32_t* gzip_length)
{
  internal_reader_write(storage, "/book.cbz", s_cbz, sizeof(s_cbz));
  internal_reader_write(storage, "/book.epub", s_epub, sizeof(s_epub));
  *tar_length = internal_reader_make_tar(s_archive);
  internal_reader_write(storage, "/book.cbt", s_archive, *tar_length);
  *gzip_length = internal_reader_make_gzip(s_archive, *tar_length, s_archive_copy);
  internal_reader_write(storage, "/book.cbt.gz", s_archive_copy, *gzip_length);
  const uint32_t jof_length = internal_reader_make_jof(s_archive);
  internal_reader_write(storage, "/book.jof", s_archive, jof_length);
}
/**
 * @brief Exercise valid path and borrowed-handle validation for every format.
 * @details Qualifies short-read tolerance, report counts, and borrowed ownership for all fixtures.
 * @param[in,out] storage Initialized portable storage.
 * @param[in,out] fault Deterministic filesystem fault facade.
 * @pre Required pointers are valid. @pre No fault other than configured short reads is armed.
 * @post Every format validates through path and borrowed APIs. @post Fault flags are cleared.
 * @note Expected member counts are table driven. @since 0.1.0
 */
RA8_INTERNAL static void internal_reader_verify_valid(mdl_storage_t*        storage,
                                                      mdl_state_fault_fs_t* fault)
{
  uint32_t tar_length  = 0U;
  uint32_t gzip_length = 0U;
  internal_reader_write_archives(storage, &tar_length, &gzip_length);
  fault->flags = (uint32_t)k_mdl_state_fault_short_read;
  for (size_t index = 0U; index < (sizeof(s_verify_cases) / sizeof(s_verify_cases[0])); ++index) {
    mdl_verify_report_t report = {};
    TEST_ASSERT_EQ(k_ra8_ok,
                   internal_reader_verify(storage,
                                          s_verify_cases[index].format,
                                          s_verify_cases[index].path,
                                          sizeof(s_verify_work.bytes),
                                          &report,
                                          nullptr));
    TEST_ASSERT_EQ(s_verify_cases[index].format, report.format);
    TEST_ASSERT_EQ((size_t)1U, report.page_count);
    TEST_ASSERT_EQ(s_verify_cases[index].members, report.member_count);
    report = (mdl_verify_report_t){};
    TEST_ASSERT_EQ(k_ra8_ok,
                   internal_reader_verify_borrowed(storage,
                                                   s_verify_cases[index].format,
                                                   s_verify_cases[index].path,
                                                   &report));
    TEST_ASSERT_EQ(s_verify_cases[index].format, report.format);
  }
  fault->flags = 0U;
}
/**
 * @brief Exercise exact verifier workspace capacity and cap-minus-one failure.
 * @details Measures CBZ and gzip high-water use, then proves exact and deficient capacities.
 * @param[in,out] storage Initialized portable storage.
 * @pre @p storage is valid. @pre Valid archive fixtures are present.
 * @post Exact measured capacity succeeds. @post One byte less fails without changing the report.
 * @note Covers both ZIP and inflate-backed verifier paths. @since 0.1.0
 */
RA8_INTERNAL static void internal_reader_verify_capacity(mdl_storage_t* storage)
{
  for (size_t index = 0U; index < 2U; ++index) {
    const char*            path   = (index == 0U) ? "/book.cbz" : "/book.cbt.gz";
    const ra8_mdl_format_t format = (index == 0U) ? k_ra8_mdl_format_cbz : k_ra8_mdl_format_cbt_gz;
    mdl_verify_report_t    report = {};
    size_t                 high_water = 0U;
    TEST_ASSERT_EQ(k_ra8_ok,
                   internal_reader_verify(storage,
                                          format,
                                          path,
                                          sizeof(s_verify_work.bytes),
                                          &report,
                                          &high_water));
    TEST_ASSERT(high_water > 0U);
    TEST_ASSERT_EQ(k_ra8_ok,
                   internal_reader_verify(storage, format, path, high_water, &report, nullptr));
    mdl_export_workspace_t tiny = {.data = s_verify_work.bytes, .cap = high_water - 1U};
    report = (mdl_verify_report_t){.format = k_ra8_mdl_format_cbr, .page_count = 17U};
    TEST_ASSERT_EQ(k_ra8_err_invalid_size, mdl_verify_file(storage, format, path, &tiny, &report));
    TEST_ASSERT_EQ(k_ra8_mdl_format_cbr, report.format);
  }
}
/**
 * @brief Exercise ZIP, EPUB, and TAR corruption and containment rejection.
 * @details Mutates checksums, required members, path traversal, and terminal bytes independently.
 * @param[in,out] storage Initialized portable storage.
 * @pre @p storage is valid. @pre Shared archive mutation buffers are idle.
 * @post Every hostile archive is rejected. @post Failure reports remain atomic.
 * @note Valid fixtures are regenerated before each independent mutation. @since 0.1.0
 */
RA8_INTERNAL static void internal_reader_verify_archive_corrupt(mdl_storage_t* storage)
{
  memcpy(s_archive, s_cbz, sizeof(s_cbz));
  s_archive[38] ^= 1U;
  internal_reader_write(storage, "/book.cbz", s_archive, sizeof(s_cbz));
  internal_reader_expect_error(storage,
                               k_ra8_mdl_format_cbz,
                               "/book.cbz",
                               k_ra8_err_validation_failed);
  memcpy(s_archive, s_cbz, sizeof(s_cbz));
  TEST_ASSERT_EQ((uint32_t)2U,
                 internal_reader_replace(s_archive, sizeof(s_cbz), "page.jpg", "../x.jpg"));
  internal_reader_write(storage, "/book.cbz", s_archive, sizeof(s_cbz));
  internal_reader_expect_error(storage,
                               k_ra8_mdl_format_cbz,
                               "/book.cbz",
                               k_ra8_err_validation_failed);
  internal_reader_write(storage, "/book.cbz", s_cbz, sizeof(s_cbz) - 1U);
  internal_reader_expect_error(storage,
                               k_ra8_mdl_format_cbz,
                               "/book.cbz",
                               k_ra8_err_validation_failed);
  memcpy(s_archive, s_epub, sizeof(s_epub));
  TEST_ASSERT_EQ((uint32_t)2U,
                 internal_reader_replace(s_archive, sizeof(s_epub), "mimetype", "mimeXype"));
  internal_reader_write(storage, "/book.epub", s_archive, sizeof(s_epub));
  internal_reader_expect_error(storage,
                               k_ra8_mdl_format_epub,
                               "/book.epub",
                               k_ra8_err_validation_failed);
  internal_reader_write(storage, "/book.epub", s_epub, sizeof(s_epub) - 1U);
  internal_reader_expect_error(storage,
                               k_ra8_mdl_format_epub,
                               "/book.epub",
                               k_ra8_err_validation_failed);
  uint32_t tar_length = internal_reader_make_tar(s_archive);
  s_archive[148] ^= 1U;
  internal_reader_write(storage, "/book.cbt", s_archive, tar_length);
  internal_reader_expect_error(storage,
                               k_ra8_mdl_format_cbt,
                               "/book.cbt",
                               k_ra8_err_validation_failed);
  tar_length = internal_reader_make_tar(s_archive);
  memcpy(s_archive, "../x.jpg", 8U);
  internal_reader_write(storage, "/book.cbt", s_archive, tar_length);
  internal_reader_expect_error(storage,
                               k_ra8_mdl_format_cbt,
                               "/book.cbt",
                               k_ra8_err_validation_failed);
}
/**
 * @brief Exercise gzip framing, JOF structure, and truncated archive rejection.
 * @details Corrupts CRC, size, trailing, truncation, and JOF magic boundaries separately.
 * @param[in,out] storage Initialized portable storage.
 * @pre @p storage is valid. @pre Shared archive construction buffers are idle.
 * @post Every malformed stream is rejected. @post No failed validation publishes a report.
 * @note The underlying TAR is regenerated before gzip mutations. @since 0.1.0
 */
RA8_INTERNAL static void internal_reader_verify_stream_corrupt(mdl_storage_t* storage)
{
  uint32_t tar_length = internal_reader_make_tar(s_archive);
  internal_reader_write(storage, "/book.cbt", s_archive, tar_length - 1U);
  internal_reader_expect_error(storage,
                               k_ra8_mdl_format_cbt,
                               "/book.cbt",
                               k_ra8_err_validation_failed);
  uint32_t gzip_length = internal_reader_make_gzip(s_archive, tar_length, s_archive_copy);
  s_archive_copy[gzip_length - 8U] ^= 1U;
  internal_reader_write(storage, "/book.cbt.gz", s_archive_copy, gzip_length);
  internal_reader_expect_error(storage,
                               k_ra8_mdl_format_cbt_gz,
                               "/book.cbt.gz",
                               k_ra8_err_validation_failed);
  gzip_length = internal_reader_make_gzip(s_archive, tar_length, s_archive_copy);
  s_archive_copy[gzip_length - 4U] ^= 1U;
  internal_reader_write(storage, "/book.cbt.gz", s_archive_copy, gzip_length);
  internal_reader_expect_error(storage,
                               k_ra8_mdl_format_cbt_gz,
                               "/book.cbt.gz",
                               k_ra8_err_validation_failed);
  gzip_length                 = internal_reader_make_gzip(s_archive, tar_length, s_archive_copy);
  s_archive_copy[gzip_length] = 0U;
  internal_reader_write(storage, "/book.cbt.gz", s_archive_copy, gzip_length + 1U);
  internal_reader_expect_error(storage,
                               k_ra8_mdl_format_cbt_gz,
                               "/book.cbt.gz",
                               k_ra8_err_validation_failed);
  internal_reader_write(storage, "/book.cbt.gz", s_archive_copy, gzip_length - 1U);
  internal_reader_expect_error(storage,
                               k_ra8_mdl_format_cbt_gz,
                               "/book.cbt.gz",
                               k_ra8_err_validation_failed);
  const uint32_t jof_length = internal_reader_make_jof(s_archive);
  s_archive[0]              = 'X';
  internal_reader_write(storage, "/book.jof", s_archive, jof_length);
  internal_reader_expect_error(storage,
                               k_ra8_mdl_format_jof,
                               "/book.jof",
                               k_ra8_err_validation_failed);
  internal_reader_write(storage, "/book.jof", s_archive, jof_length - 1U);
  internal_reader_expect_error(storage,
                               k_ra8_mdl_format_jof,
                               "/book.jof",
                               k_ra8_err_validation_failed);
}
/**
 * @brief Prove borrowed validation propagates seek faults without taking ownership.
 * @details Injects a seek error into an open CBZ and then reuses and closes that handle.
 * @param[in,out] storage Initialized portable storage.
 * @param[in,out] fault Deterministic filesystem fault facade.
 * @pre Required pointers are valid. @pre A valid CBZ fixture is present.
 * @post Seek failure propagates with report atomicity. @post Caller retains and closes the handle.
 * @note Fault flags are cleared before the ownership probe. @since 0.1.0
 */
RA8_INTERNAL static void internal_reader_verify_borrowed_fault(mdl_storage_t*        storage,
                                                               mdl_state_fault_fs_t* fault)
{
  fw_fs_file_t borrowed = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 fw_fs_open(&storage->fs->streams,
                            "/book.cbz",
                            k_fw_fs_open_read,
                            &borrowed,
                            storage->file_workspace,
                            storage->file_workspace_bytes));
  uint64_t borrowed_size = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_file_size(&borrowed, &borrowed_size));
  mdl_export_workspace_t work   = {.data = s_verify_work.bytes, .cap = sizeof(s_verify_work.bytes)};
  mdl_verify_report_t    report = {.format = k_ra8_mdl_format_cbr, .page_count = 17U};
  fault->flags                  = (uint32_t)k_mdl_state_fault_stream_seek;
  TEST_ASSERT_EQ(
    k_ra8_err_hw_error,
    mdl_verify_open_file(storage, k_ra8_mdl_format_cbz, &borrowed, borrowed_size, &work, &report));
  TEST_ASSERT_EQ(k_ra8_mdl_format_cbr, report.format);
  TEST_ASSERT_EQ((size_t)17U, report.page_count);
  fault->flags = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_file_size(&borrowed, &borrowed_size));
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_close(&borrowed));
}
/**
 * @brief Exercise stream, close, media, path, and unsupported-format faults.
 * @details Drives each storage dependency and API-policy rejection across applicable formats.
 * @param[in,out] storage Initialized portable storage.
 * @param[in,out] fault Deterministic filesystem fault facade.
 * @pre Required pointers are valid. @pre Shared fixtures can be replaced.
 * @post Every injected status propagates exactly. @post Fault flags are cleared at clean boundaries.
 * @note Unsupported CBR is rejected before format-specific processing. @since 0.1.0
 */
RA8_INTERNAL static void internal_reader_verify_faults(mdl_storage_t*        storage,
                                                       mdl_state_fault_fs_t* fault)
{
  uint32_t tar_length  = 0U;
  uint32_t gzip_length = 0U;
  internal_reader_write_archives(storage, &tar_length, &gzip_length);
  for (size_t index = 0U; index < (sizeof(s_verify_cases) / sizeof(s_verify_cases[0])); ++index) {
    fault->flags = (uint32_t)k_mdl_state_fault_read;
    internal_reader_expect_error(storage,
                                 s_verify_cases[index].format,
                                 s_verify_cases[index].path,
                                 k_ra8_err_hw_error);
    fault->flags = (uint32_t)k_mdl_state_fault_close;
    internal_reader_expect_error(storage,
                                 s_verify_cases[index].format,
                                 s_verify_cases[index].path,
                                 k_ra8_err_hw_error);
  }
  fault->flags = (uint32_t)k_mdl_state_fault_stream_seek;
  internal_reader_expect_error(storage, k_ra8_mdl_format_cbz, "/book.cbz", k_ra8_err_hw_error);
  internal_reader_expect_error(storage, k_ra8_mdl_format_jof, "/book.jof", k_ra8_err_hw_error);
  fault->flags = 0U;
  internal_reader_verify_borrowed_fault(storage, fault);
  fault->flags = (uint32_t)k_mdl_state_fault_media;
  internal_reader_expect_error(storage, k_ra8_mdl_format_cbt, "/book.cbt", k_ra8_err_hw_not_ready);
  fault->flags = 0U;
  internal_reader_expect_error(storage, k_ra8_mdl_format_cbt, "book.cbt", k_ra8_err_invalid_arg);
  internal_reader_expect_error(storage, k_ra8_mdl_format_cbr, "/book.cbz", k_ra8_err_not_supported);
}
/**
 * @brief Exercise valid, bounded, corrupt, unsafe, and faulted validators.
 * @details Runs the complete verifier qualification sequence against one real backend.
 * @param[in,out] storage Initialized portable storage.
 * @param[in,out] fault Deterministic filesystem fault facade.
 * @pre Required pointers are valid. @pre Shared workspaces are idle.
 * @post Every verifier vector completed. @post No deterministic fault remains armed.
 * @note Backend setup and teardown remain caller-owned. @since 0.1.0
 */
RA8_INTERNAL static void internal_reader_verify_vectors(mdl_storage_t*        storage,
                                                        mdl_state_fault_fs_t* fault)
{
  internal_reader_verify_valid(storage, fault);
  internal_reader_verify_capacity(storage);
  internal_reader_verify_archive_corrupt(storage);
  internal_reader_verify_stream_corrupt(storage);
  internal_reader_verify_faults(storage, fault);
}
/**
 * @brief Run the complete common reader vector and remove its fixtures.
 * @details Applies the same config and image assertions to one real filesystem
 *          backend so host and embedded-style storage remain
 * behavior-identical.
 * @param[in] label Unity vector label.
 * @param[in] inner Real filesystem under qualification.
 * @pre Both pointers are non-NULL and the backend root is writable. @pre @p inner implements the complete portable filesystem contract.
 * @post All common fixtures are removed. @post No portable handle or transaction remains active.
 * @note The fault wrapper owns no backend resource. @since 0.1.0
 */
RA8_INTERNAL static void internal_reader_run(const char* label, const fw_fs_t* inner)
{
  TEST_BEGIN(label);
  mdl_state_fault_fs_t fault   = {};
  mdl_storage_t        storage = {};
  internal_reader_bind(inner, &fault, &storage);
  internal_reader_config_vectors(&storage, &fault);
  internal_reader_image_vectors(&storage, &fault);
  internal_reader_verify_vectors(&storage, &fault);
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_unlink(&storage.fs->names, "/site.conf"));
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_unlink(&storage.fs->names, "/long.conf"));
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_unlink(&storage.fs->names, "/image.bin"));
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_unlink(&storage.fs->names, "/book.cbz"));
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_unlink(&storage.fs->names, "/book.epub"));
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_unlink(&storage.fs->names, "/book.cbt"));
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_unlink(&storage.fs->names, "/book.cbt.gz"));
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_unlink(&storage.fs->names, "/book.jof"));
  TEST_END(label);
}
/**
 * @brief Qualify the hosted POSIX filesystem port.
 * @details Creates a private temporary root, runs the common reader vectors,
 *          then deinitializes the port and removes the empty root.
 * @pre The process can create a private directory under `/tmp`. @pre The POSIX filesystem adapter is available in this test build.
 * @post The adapter is deinitialized and its root directory is removed. @post No file descriptor or portable handle remains open.
 * @note Uses a private `mkdtemp` root to avoid collisions between test runs. @since 0.1.0
 */
RA8_INTERNAL static void internal_reader_test_posix(void)
{
  char root[] = "/tmp/mdl_reader_port_XXXXXX";
  TEST_ASSERT_NOT_NULL(mkdtemp(root));
  fw_fs_t                 fs    = {};
  fw_fs_posix_state_t     posix = {.root_fd = -1};
  const fw_fs_posix_cfg_t cfg   = {.root_path = root, .removable_media = false};
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_posix_init(&fs, &posix, &cfg));
  internal_reader_run("media readers POSIX vectors", &fs);
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_posix_deinit(&posix));
  TEST_ASSERT_EQ(0, rmdir(root));
}
/**
 * @brief Qualify the RAM block-device through FAT12 and the RA8 VFS port.
 * @details Formats the fixed RAM block device, mounts it through the RA8 VFS
 *          adapter, runs common reader vectors, and tears the mount down.
 * @pre The shared RAM disk and mount pointer are idle. @pre The block-device, FAT12, VFS, and portable adapters are linked.
 * @post The named VFS mount and filesystem mount are both released. @post ::s_mount is reset to NULL after successful teardown.
 * @note No heap storage is introduced by the reader test fixture. @since 0.1.0
 */
RA8_INTERNAL static void internal_reader_test_vfs(void)
{
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_io_blockdev_ram_init(&s_blockdev, &s_ram_state, s_disk, k_reader_disk_blocks, false));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_as_fs_backend(&s_blockdev, &s_backend));
  const ra8_fs_format_opts_t format = {.type = k_ra8_fs_type_fat12, .label = "MDLREAD"};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_format(&s_backend, &format));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &s_mount));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_init());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_mount("reader", s_mount));
  fw_fs_t                   fs    = {};
  fw_fs_ra8_vfs_state_t     state = {};
  const fw_fs_ra8_vfs_cfg_t cfg   = {.mount_name      = "reader",
                                     .mount           = s_mount,
                                     .removable_media = false};
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_ra8_vfs_init(&fs, &state, &cfg));
  internal_reader_run("media readers RAM/FAT/VFS vectors", &fs);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_unmount("reader"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(s_mount));
  s_mount = nullptr;
}

int main(void)
{
  ra8_log_set_byte_sink(internal_reader_log_sink, nullptr);
  internal_reader_test_posix();
  internal_reader_test_vfs();
  return 0;
}
