/**
 * @file test_ra8_io_fsfmt.c
 * @brief End-to-end filesystem-format dispatch tests (issue #159).
 *
 * @details Proves native FAT/exFAT byte identity through the operations layer
 * and mounts a registered read-only foreign stub that serves one file without
 * any FAT-core participation. Capability-gated mutations are independently
 * exercised and must return not-supported before a foreign callback runs.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_fs.h"
#include "ra8_io_blockdev.h"
#include "ra8_io_blockdev_ram.h"
#include "ra8_io_fsfmt.h"
#include "ra8_io_vfs.h"
#include "ra8_log.h"
#include "unity_minimal.h"

/** @brief Test fixture limits and foreign-volume marker. */
typedef enum : uint32_t {
  k_t_native_blocks = 131072U, /**< 64 MiB: valid for FAT16 and exFAT. */
  k_t_stub_blocks   = 8U,      /**< Tiny fixed-layout foreign volume.  */
  k_t_stub_magic    = 0xA7U,   /**< Foreign volume marker.             */
  k_t_stub_name_max = 31U,     /**< Foreign UTF-8 name-byte limit.     */
} t_fsfmt_const_t;

/** @brief Identical native-volume candidates used for raw-image comparison. */
static uint8_t s_native_a[(size_t)k_t_native_blocks * (size_t)k_ra8_io_block_size_bytes];
static uint8_t s_native_b[(size_t)k_t_native_blocks * (size_t)k_ra8_io_block_size_bytes];
/** @brief Foreign fixed-layout volume storage. */
static uint8_t s_stub_disk[(size_t)k_t_stub_blocks * (size_t)k_ra8_io_block_size_bytes];

/** @brief The only foreign file's immutable contents. */
static const uint8_t s_stub_payload[] = "registered-format-data";

/** @brief Foreign mount state (one statically allocated context). */
typedef struct {
  bool mounted; /**< true between mount and unmount. */
} stub_mount_t;

/** @brief Foreign file state (one statically allocated context). */
typedef struct {
  uint32_t offset; /**< Current read offset.         */
  bool     open;   /**< true between open and close. */
} stub_file_t;

static stub_mount_t s_stub_mount;
static stub_file_t  s_stub_file;
static uint32_t     s_stub_open_calls;
static uint32_t     s_stub_unmount_calls;

/** @brief True when block zero carries the foreign marker. @details Exercises the stub probe path with bounded caller-owned fixture state and verifies its documented result. @param[in] backend Backend descriptor or operation table under test. @return Whether the named fixture condition holds. @retval true The named fixture condition holds. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static bool internal_stub_probe(const ra8_fs_backend_t* backend)
{
  if (backend == nullptr) {
    return false;
  }
  if (backend->read_block == nullptr) {
    return false;
  }
  uint8_t block[(size_t)k_ra8_io_block_size_bytes];
  if (backend->read_block(backend->ctx, 0U, 1U, block) != k_ra8_ok) {
    return false;
  }
  return block[0] == (uint8_t)k_t_stub_magic;
}

/** @brief Mount the single static foreign context. @details Exercises the stub mount path with bounded caller-owned fixture state and verifies its documented result. @param[in] backend Backend descriptor or operation table under test. @param[out] out_mount Receives the opened mount context. @return RA8 status from the exercised fixture operation. @retval k_ra8_ok The fixture operation completed successfully. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static ra8_err_t internal_stub_mount(const ra8_fs_backend_t* backend, void** out_mount)
{
  if (!internal_stub_probe(backend)) {
    return k_ra8_err_not_found;
  }
  if (s_stub_mount.mounted) {
    return k_ra8_err_busy;
  }
  s_stub_mount.mounted = true;
  *out_mount           = &s_stub_mount;
  return k_ra8_ok;
}

/** @brief Release the single static foreign context. @details Exercises the stub unmount path with bounded caller-owned fixture state and verifies its documented result. @param[in,out] mount_ctx Injected filesystem-mount context retained by the fixture. @return RA8 status from the exercised fixture operation. @retval k_ra8_ok The fixture operation completed successfully. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static ra8_err_t internal_stub_unmount(void* mount_ctx)
{
  if (mount_ctx != &s_stub_mount) {
    return k_ra8_err_invalid_arg;
  }
  s_stub_mount.mounted = false;
  s_stub_unmount_calls++;
  return k_ra8_ok;
}

/** @brief Open `/README.TXT` for reading from the foreign format. @details Exercises the stub open path with bounded caller-owned fixture state and verifies its documented result. @param[in,out] mount_ctx Injected filesystem-mount context retained by the fixture. @param[in] path Normalized path supplied to the fake filesystem backend. @param[in] mode Open or seek mode exercised by the fixture. @param[out] out_file Receives the opened file context. @return RA8 status from the exercised fixture operation. @retval k_ra8_ok The fixture operation completed successfully. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static ra8_err_t
internal_stub_open(void* mount_ctx, const char* path, ra8_fs_mode_t mode, void** out_file)
{
  s_stub_open_calls++;
  if (mount_ctx != &s_stub_mount) {
    return k_ra8_err_invalid_state;
  }
  if (mode != k_ra8_fs_mode_read) {
    return k_ra8_err_not_supported;
  }
  if (strcmp(path, "/README.TXT") != 0) {
    return k_ra8_err_not_found;
  }
  if (s_stub_file.open) {
    return k_ra8_err_busy;
  }
  s_stub_file.offset = 0U;
  s_stub_file.open   = true;
  *out_file          = &s_stub_file;
  return k_ra8_ok;
}

/** @brief Close the static foreign file. @details Exercises the stub close path with bounded caller-owned fixture state and verifies its documented result. @param[in,out] file_ctx Injected open-file context retained by the fixture. @return RA8 status from the exercised fixture operation. @retval k_ra8_ok The fixture operation completed successfully. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static ra8_err_t internal_stub_close(void* file_ctx)
{
  if (file_ctx != &s_stub_file) {
    return k_ra8_err_invalid_arg;
  }
  s_stub_file.open = false;
  return k_ra8_ok;
}

/** @brief Read the immutable foreign payload. @details Exercises the stub read path with bounded caller-owned fixture state and verifies its documented result. @param[in,out] file_ctx Injected open-file context retained by the fixture. @param[out] buf Caller-owned byte buffer read or written by the backend. @param[in] bytes Readable byte sequence length or storage size. @param[out] out_read Receives the number of bytes actually read. @return RA8 status from the exercised fixture operation. @retval k_ra8_ok The fixture operation completed successfully. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static ra8_err_t
internal_stub_read(void* file_ctx, void* buf, uint32_t bytes, uint32_t* out_read)
{
  if (file_ctx != &s_stub_file) {
    return k_ra8_err_invalid_arg;
  }
  uint32_t remaining = (uint32_t)sizeof(s_stub_payload) - 1U - s_stub_file.offset;
  if (bytes < remaining) {
    remaining = bytes;
  }
  (void)memcpy(buf, &s_stub_payload[s_stub_file.offset], remaining);
  s_stub_file.offset += remaining;
  *out_read = remaining;
  return k_ra8_ok;
}

/** @brief Seek within the immutable foreign payload, clamped at EOF. @details Exercises the stub seek path with bounded caller-owned fixture state and verifies its documented result. @param[in,out] file_ctx Injected open-file context retained by the fixture. @param[in] offset_bytes Byte offset applied to the I/O request. @return RA8 status from the exercised fixture operation. @retval k_ra8_ok The fixture operation completed successfully. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static ra8_err_t internal_stub_seek(void* file_ctx, uint64_t offset_bytes)
{
  if (file_ctx != &s_stub_file) {
    return k_ra8_err_invalid_arg;
  }
  const uint32_t size = (uint32_t)sizeof(s_stub_payload) - 1U;
  if (offset_bytes > size) {
    s_stub_file.offset = size;
  } else {
    s_stub_file.offset = (uint32_t)offset_bytes;
  }
  return k_ra8_ok;
}

/** @brief Report the foreign stream offset. @details Exercises the stub tell path with bounded caller-owned fixture state and verifies its documented result. @param[in] file_ctx Injected open-file context retained by the fixture. @param[out] out_offset Receives the resulting stream offset. @return RA8 status from the exercised fixture operation. @retval k_ra8_ok The fixture operation completed successfully. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static ra8_err_t internal_stub_tell(const void* file_ctx, uint64_t* out_offset)
{
  if (file_ctx != &s_stub_file) {
    return k_ra8_err_invalid_arg;
  }
  *out_offset = s_stub_file.offset;
  return k_ra8_ok;
}

/** @brief Report the foreign stream size. @details Exercises the stub size path with bounded caller-owned fixture state and verifies its documented result. @param[in] file_ctx Injected open-file context retained by the fixture. @param[out] out_bytes Receives the number of bytes transferred. @return RA8 status from the exercised fixture operation. @retval k_ra8_ok The fixture operation completed successfully. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static ra8_err_t internal_stub_size(const void* file_ctx, uint64_t* out_bytes)
{
  if (file_ctx != &s_stub_file) {
    return k_ra8_err_invalid_arg;
  }
  *out_bytes = (uint64_t)sizeof(s_stub_payload) - 1U;
  return k_ra8_ok;
}

/** @brief Report root/file metadata in the foreign format. @details Exercises the stub stat path with bounded caller-owned fixture state and verifies its documented result. @param[in,out] mount_ctx Injected filesystem-mount context retained by the fixture. @param[in] path Normalized path supplied to the fake filesystem backend. @param[out] out Caller-owned result object populated by the callback. @return RA8 status from the exercised fixture operation. @retval k_ra8_ok The fixture operation completed successfully. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static ra8_err_t
internal_stub_stat(void* mount_ctx, const char* path, ra8_fs_stat_t* out)
{
  if (mount_ctx != &s_stub_mount) {
    return k_ra8_err_invalid_state;
  }
  *out = (ra8_fs_stat_t){};
  if (strcmp(path, "/") == 0) {
    out->attr         = (uint8_t)k_ra8_fs_attr_directory;
    out->is_directory = true;
    return k_ra8_ok;
  }
  if (strcmp(path, "/README.TXT") == 0) {
    out->attr       = (uint8_t)k_ra8_fs_attr_read_only;
    out->size_bytes = (uint64_t)sizeof(s_stub_payload) - 1U;
    return k_ra8_ok;
  }
  return k_ra8_err_not_found;
}

/** @brief Enumerate the foreign root's one file. @details Exercises the stub listdir path with bounded caller-owned fixture state and verifies its documented result. @param[in,out] mount_ctx Injected filesystem-mount context retained by the fixture. @param[in] path Normalized path supplied to the fake filesystem backend. @param[in] cb Callback invoked by the enumeration fixture. @param[in,out] cb_ctx Injected callback context passed through unchanged. @return RA8 status from the exercised fixture operation. @retval k_ra8_ok The fixture operation completed successfully. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static ra8_err_t
internal_stub_listdir(void* mount_ctx, const char* path, ra8_fs_listdir_cb_t cb, void* cb_ctx)
{
  if (mount_ctx != &s_stub_mount) {
    return k_ra8_err_invalid_state;
  }
  if (strcmp(path, "/") != 0) {
    return k_ra8_err_not_found;
  }
  cb("README.TXT", (uint8_t)k_ra8_fs_attr_read_only, (uint64_t)sizeof(s_stub_payload) - 1U, cb_ctx);
  return k_ra8_ok;
}

/** @brief Complete read-side ops; every mutation is intentionally absent. */
static const ra8_io_fsfmt_ops_t s_stub_ops = {
  .probe      = internal_stub_probe,
  .mount      = internal_stub_mount,
  .unmount    = internal_stub_unmount,
  .open       = internal_stub_open,
  .close      = internal_stub_close,
  .read       = internal_stub_read,
  .write      = nullptr,
  .seek       = internal_stub_seek,
  .tell       = internal_stub_tell,
  .size       = internal_stub_size,
  .sync       = nullptr,
  .stat       = internal_stub_stat,
  .listdir    = internal_stub_listdir,
  .unlink     = nullptr,
  .rename     = nullptr,
  .mkdir      = nullptr,
  .rmdir      = nullptr,
  .free_space = nullptr,
};

/** @brief Registered read-only foreign format. */
static const ra8_io_fsfmt_t s_stub_format = {
  .name = "stubfs",
  .caps =
    {
      .max_name_len             = (uint16_t)k_t_stub_name_max,
      .read_only                = true,
      .supports_mkdir           = false,
      .supports_rmdir           = false,
      .supports_streaming_write = false,
      .supports_timestamps      = false,
      .supports_free_space      = false,
      .supports_sync            = false,
      .atomic_rename            = false,
      .durable_sync             = false,
      .unicode_names            = false,
      .case_sensitive           = true,
    },
  .ops = &s_stub_ops,
};

/** @brief Count one directory callback. @details Exercises the count entry path with bounded caller-owned fixture state and verifies its documented result. @param[in] name Directory-entry name reported by the callback. @param[in] attr Filesystem attributes reported for the directory entry. @param[in] size Entry or storage size in bytes. @param[in,out] ctx Injected callback context whose ownership remains with the test. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static void
internal_count_entry(const char* name, uint8_t attr, uint64_t size, void* ctx)
{
  TEST_ASSERT(strcmp(name, "README.TXT") == 0);
  TEST_ASSERT_EQ(k_ra8_fs_attr_read_only, attr);
  TEST_ASSERT_EQ(sizeof(s_stub_payload) - 1U, size);
  uint32_t* count = (uint32_t*)ctx;
  (*count)++;
}

/** @brief Mount the registered foreign fixture through automatic probing. @details Exercises the mount foreign path with bounded caller-owned fixture state and verifies its documented result. @param[in,out] state Caller-owned fake backend state. @param[in,out] blockdev Block-device facade exercised by the operation. @param[in,out] backend Backend descriptor or operation table under test. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL
static void internal_mount_foreign(ra8_io_blockdev_ram_state_t* state,
                                   ra8_io_blockdev_t*           blockdev,
                                   ra8_fs_backend_t*            backend)
{
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_io_blockdev_ram_init(blockdev, state, s_stub_disk, k_t_stub_blocks, false));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_erase(blockdev, 0U, k_t_stub_blocks));
  uint8_t block[(size_t)k_ra8_io_block_size_bytes] = {};
  block[0]                                         = (uint8_t)k_t_stub_magic;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_write(blockdev, 0U, 1U, block));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_as_fs_backend(blockdev, backend));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_mount_auto("foreign", backend));
}

/** @brief Qualify foreign namespace metadata and read-only stream behavior. @details Exercises the expect foreign reads path with bounded caller-owned fixture state and verifies its documented result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL
static void internal_expect_foreign_reads(void)
{
  ra8_io_fsfmt_caps_t caps = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_get_caps("foreign", &caps));
  TEST_ASSERT(caps.read_only);
  TEST_ASSERT(caps.case_sensitive);
  TEST_ASSERT(!caps.supports_mkdir);
  TEST_ASSERT(!caps.supports_dir_cursor);

  uint32_t cursor_bytes = UINT32_MAX;
  uint8_t  cursor_align = UINT8_MAX;
  uint16_t cursor_count = UINT16_MAX;
  TEST_ASSERT_EQ(
    k_ra8_err_not_supported,
    ra8_io_vfs_dir_requirements("foreign:/", &cursor_bytes, &cursor_align, &cursor_count));
  TEST_ASSERT_EQ(0U, cursor_bytes);
  TEST_ASSERT_EQ(0U, cursor_align);
  TEST_ASSERT_EQ(0U, cursor_count);
  max_align_t      cursor_workspace = {};
  ra8_io_vfs_dir_t directory        = {};
  TEST_ASSERT_EQ(
    k_ra8_err_not_supported,
    ra8_io_vfs_dir_open("foreign:/", &directory, &cursor_workspace, sizeof(cursor_workspace)));
  TEST_ASSERT(!directory.is_open);

  uint32_t listed = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_listdir("foreign:/", internal_count_entry, &listed));
  TEST_ASSERT_EQ(1U, listed);
  ra8_io_vfs_stat_t stat = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_stat("foreign:/README.TXT", &stat));
  TEST_ASSERT(stat.exists);
  TEST_ASSERT_EQ(sizeof(s_stub_payload) - 1U, stat.size_bytes);

  ra8_io_vfs_file_t* file = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_file_open("foreign:/README.TXT", k_ra8_fs_mode_read, &file));
  TEST_ASSERT_EQ(k_ra8_err_busy, ra8_io_vfs_unmount("foreign"));
  uint64_t size = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_file_size(file, &size));
  TEST_ASSERT_EQ(sizeof(s_stub_payload) - 1U, size);
  uint8_t  got[sizeof(s_stub_payload)] = {};
  uint32_t got_len                     = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_file_read(file, got, sizeof(got), &got_len));
  TEST_ASSERT_EQ(sizeof(s_stub_payload) - 1U, got_len);
  TEST_ASSERT(memcmp(got, s_stub_payload, got_len) == 0);
  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_io_vfs_file_sync(file));
  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_io_vfs_file_write(file, got, 1U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_file_close(file));
}

/** @brief Prove every foreign-format mutation is rejected before dispatch. @details Exercises the expect foreign mutation guards path with bounded caller-owned fixture state and verifies its documented result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL
static void internal_expect_foreign_mutation_guards(void)
{
  ra8_io_vfs_file_t* file         = nullptr;
  const uint32_t     opens_before = s_stub_open_calls;
  TEST_ASSERT_EQ(k_ra8_err_not_supported,
                 ra8_io_vfs_file_open("foreign:/README.TXT", k_ra8_fs_mode_write, &file));
  TEST_ASSERT_EQ(opens_before, s_stub_open_calls);
  ra8_fs_file_t* native_file = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_not_supported,
                 ra8_io_vfs_open("foreign:/README.TXT", k_ra8_fs_mode_read, &native_file));
  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_io_vfs_unlink("foreign:/README.TXT"));
  TEST_ASSERT_EQ(k_ra8_err_not_supported,
                 ra8_io_vfs_rename("foreign:/README.TXT", "foreign:/RENAMED.TXT"));
  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_io_vfs_mkdir("foreign:/DIR"));
  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_io_vfs_rmdir("foreign:/DIR"));
  ra8_fs_space_t space = {};
  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_io_vfs_free_space("foreign", &space));
}

/** @brief Count any native directory entry. @details Exercises the count any path with bounded caller-owned fixture state and verifies its documented result. @param[in] name Directory-entry name reported by the callback. @param[in] attr Filesystem attributes reported for the directory entry. @param[in] size Entry or storage size in bytes. @param[in,out] ctx Injected callback context whose ownership remains with the test. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static void
internal_count_any(const char* name, uint8_t attr, uint64_t size, void* ctx)
{
  (void)name;
  (void)attr;
  (void)size;
  uint32_t* count = (uint32_t*)ctx;
  (*count)++;
}

/** @brief Initialize a RAM block device/backend over caller-provided storage. @details Exercises the make backend path with bounded caller-owned fixture state and verifies its documented result. @param[in,out] storage Caller-owned storage backing the fake object. @param[in,out] state Caller-owned fake backend state. @param[in,out] blockdev Block-device facade exercised by the operation. @param[in,out] backend Backend descriptor or operation table under test. @return RA8 status from the exercised fixture operation. @retval k_ra8_ok The fixture operation completed successfully. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static ra8_err_t internal_make_backend(uint8_t*                     storage,
                                                    ra8_io_blockdev_ram_state_t* state,
                                                    ra8_io_blockdev_t*           blockdev,
                                                    ra8_fs_backend_t*            backend)
{
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_io_blockdev_ram_init(blockdev, state, storage, (uint64_t)k_t_native_blocks, false));
  return ra8_io_blockdev_as_fs_backend(blockdev, backend);
}

/** @brief Apply one deterministic mutation sequence directly through ra8_fs. @details Exercises the direct sequence path with bounded caller-owned fixture state and verifies its documented result. @param[in] backend Backend descriptor or operation table under test. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static void internal_direct_sequence(const ra8_fs_backend_t* backend)
{
  static const uint8_t payload[] = {1U, 3U, 5U, 7U, 9U};
  ra8_fs_mount_t*      mount     = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(backend, &mount));
  ra8_fs_file_t* file = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(mount, "/ALPHA.BIN", k_ra8_fs_mode_write, &file));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write(file, payload, (uint32_t)sizeof(payload)));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(file));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mkdir(mount, "/BOOKS"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_rename(mount, "/ALPHA.BIN", "/BETA.BIN"));
  ra8_fs_stat_t stat = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_stat(mount, "/BETA.BIN", &stat));
  TEST_ASSERT_EQ(sizeof(payload), stat.size_bytes);
  uint32_t listed = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_listdir(mount, "/", internal_count_any, &listed));
  TEST_ASSERT_EQ(2U, listed);
  ra8_fs_space_t space = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_free_space(mount, &space));
  TEST_ASSERT(space.free_bytes < space.total_bytes);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(mount, "/BETA.BIN", k_ra8_fs_mode_read, &file));
  uint64_t value = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_size(file, &value));
  TEST_ASSERT_EQ(sizeof(payload), value);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_tell(file, &value));
  TEST_ASSERT_EQ(0U, value);
  uint8_t  got[sizeof(payload)] = {};
  uint32_t got_len              = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_read(file, got, sizeof(got), &got_len));
  TEST_ASSERT(memcmp(got, payload, sizeof(payload)) == 0);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_seek(file, 1U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(file));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unlink(mount, "/BETA.BIN"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_rmdir(mount, "/BOOKS"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(mount));
}

/** @brief Apply the identical mutation sequence through format-neutral VFS ops. @details Exercises the dispatched sequence path with bounded caller-owned fixture state and verifies its documented result. @param[in] backend Backend descriptor or operation table under test. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static void internal_dispatched_sequence(const ra8_fs_backend_t* backend)
{
  static const uint8_t payload[] = {1U, 3U, 5U, 7U, 9U};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_fsfmt_init());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_init());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_mount_auto("vol", backend));
  ra8_io_vfs_file_t* file = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_file_open("vol:/ALPHA.BIN", k_ra8_fs_mode_write, &file));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_file_write(file, payload, (uint32_t)sizeof(payload)));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_file_close(file));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_mkdir("vol:/BOOKS"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_rename("vol:/ALPHA.BIN", "vol:/BETA.BIN"));
  ra8_io_vfs_stat_t stat = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_stat("vol:/BETA.BIN", &stat));
  TEST_ASSERT_EQ(sizeof(payload), stat.size_bytes);
  uint32_t listed = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_listdir("vol:/", internal_count_any, &listed));
  TEST_ASSERT_EQ(2U, listed);
  ra8_fs_space_t space = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_free_space("vol", &space));
  TEST_ASSERT(space.free_bytes < space.total_bytes);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_file_open("vol:/BETA.BIN", k_ra8_fs_mode_read, &file));
  uint64_t value = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_file_size(file, &value));
  TEST_ASSERT_EQ(sizeof(payload), value);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_file_tell(file, &value));
  TEST_ASSERT_EQ(0U, value);
  uint8_t  got[sizeof(payload)] = {};
  uint32_t got_len              = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_file_read(file, got, sizeof(got), &got_len));
  TEST_ASSERT(memcmp(got, payload, sizeof(payload)) == 0);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_file_seek(file, 1U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_file_close(file));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_unlink("vol:/BETA.BIN"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_rmdir("vol:/BOOKS"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_unmount("vol"));
}

/** @brief Format two volumes, run direct/dispatched sequences, compare every byte. @details Exercises the assert native byte identity path with bounded caller-owned fixture state and verifies its documented result. @param[in] type Entry or stream type discriminator. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static void internal_assert_native_byte_identity(ra8_fs_type_t type)
{
  ra8_io_blockdev_ram_state_t state_a = {};
  ra8_io_blockdev_ram_state_t state_b = {};
  ra8_io_blockdev_t           bd_a    = {};
  ra8_io_blockdev_t           bd_b    = {};
  ra8_fs_backend_t            be_a    = {};
  ra8_fs_backend_t            be_b    = {};
  TEST_ASSERT_EQ(k_ra8_ok, internal_make_backend(s_native_a, &state_a, &bd_a, &be_a));
  TEST_ASSERT_EQ(k_ra8_ok, internal_make_backend(s_native_b, &state_b, &bd_b, &be_b));
  ra8_fs_format_opts_t opts = {.type = type, .label = "DISPATCH"};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_format(&be_a, &opts));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_format(&be_b, &opts));
  internal_direct_sequence(&be_a);
  internal_dispatched_sequence(&be_b);
  TEST_ASSERT(memcmp(s_native_a, s_native_b, sizeof(s_native_a)) == 0);
}

/**
 * @brief FAT and exFAT pass through ops with byte-identical images and truthful caps.
 *
 * @par MC/DC:
 * Format dispatch contains no compound decision. In `internal_is_native`, FAT
 * makes the first N=1 identity decision true; exFAT makes it false and the
 * second identity decision true. The foreign-format test makes both identity
 * checks false, so the two tests together exercise both outcomes of each
 * single-condition decision. @details Executes the native dispatch scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static void internal_test_native_dispatch(void)
{
  TEST_BEGIN("fsfmt native dispatch byte identity");
  internal_assert_native_byte_identity(k_ra8_fs_type_fat16);
  internal_assert_native_byte_identity(k_ra8_fs_type_exfat);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_fsfmt_init());
  const ra8_io_fsfmt_t* fat = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_fsfmt_get_builtin(k_ra8_fs_type_fat32, &fat));
  TEST_ASSERT(strcmp(fat->name, "fat") == 0);
  TEST_ASSERT_EQ(741U, fat->caps.max_name_len);
  TEST_ASSERT(fat->caps.supports_mkdir);
  TEST_ASSERT(fat->caps.supports_rmdir);
  TEST_ASSERT(fat->caps.supports_timestamps);
  TEST_ASSERT(fat->caps.supports_free_space);
  TEST_ASSERT(fat->caps.unicode_names);
  TEST_ASSERT(!fat->caps.supports_sync);
  TEST_ASSERT(!fat->caps.atomic_rename);
  TEST_ASSERT(!fat->caps.durable_sync);

  const ra8_io_fsfmt_t* exfat = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_fsfmt_get_builtin(k_ra8_fs_type_exfat, &exfat));
  TEST_ASSERT(strcmp(exfat->name, "exfat") == 0);
  TEST_ASSERT_EQ(192U, exfat->caps.max_name_len);
  TEST_ASSERT(exfat->caps.supports_streaming_write);
  TEST_END("fsfmt native dispatch byte identity");
}

/**
 * @brief Registered foreign format mounts, serves data, and gates every mutation.
 *
 * @par MC/DC:
 * The format registry and VFS dispatch paths contain no compound decisions.
 * Builtin probes return false before the registered stub returns true, proving
 * both outcomes of the N=1 probe decision; read mode passes the mutation gate,
 * while write, sync, rename, mkdir, rmdir, and free-space requests take their
 * single-condition rejection paths. @details Executes the foreign end to end scenario with bounded fixture state and asserts the contract-specific result. @pre Fixed-capacity fixture storage required by this operation is available. @pre Arguments follow the interface contract exercised by this helper. @post Documented outputs contain the exercised result when the operation succeeds. @post Mutations remain confined to documented outputs and file-local fixture state. @note File-local helper; no ownership escapes this focused test executable. @since 0.1.0 */
RA8_INTERNAL static void internal_test_foreign_end_to_end(void)
{
  TEST_BEGIN("fsfmt registered foreign end-to-end");
  s_stub_mount         = (stub_mount_t){};
  s_stub_file          = (stub_file_t){};
  s_stub_open_calls    = 0U;
  s_stub_unmount_calls = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_fsfmt_init());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_fsfmt_register(&s_stub_format));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_init());

  ra8_io_blockdev_ram_state_t state   = {};
  ra8_io_blockdev_t           bd      = {};
  ra8_fs_backend_t            backend = {};
  internal_mount_foreign(&state, &bd, &backend);
  internal_expect_foreign_reads();
  internal_expect_foreign_mutation_guards();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_unmount("foreign"));
  TEST_ASSERT_EQ(1U, s_stub_unmount_calls);
  TEST_END("fsfmt registered foreign end-to-end");
}

/**
 * @brief Discard expected negative-path log bytes in the hosted test.
 * @param[in] ctx Unused sink context.
 * @param[in] byte Unused emitted byte.
 * @pre Installed before any tested error path executes.
 * @post No state is modified and no host MMIO is accessed.
 * @note Single-threaded hosted test sink.
 * @since 0.1.0 @details Exercises the log sink path with bounded caller-owned fixture state and verifies its documented result. @pre Fixed-capacity fixture storage required by this operation is available. @post Documented outputs contain the exercised result when the operation succeeds. */
RA8_INTERNAL static void internal_log_sink(void* ctx, uint8_t byte)
{
  (void)ctx;
  (void)byte;
}

/**
 * @brief Exercise the directory-requirements nullable operands.
 * @details Supplies each null operand independently, then verifies the native
 *          format's successful requirements tuple.
 * @pre The `cursor` mount is live and advertises a native directory cursor.
 * @pre Output objects are writable for the complete call sequence.
 * @post Every nullable operand has independently produced null-pointer status.
 * @post The all-present control reports the native cursor size and alignment.
 * @note Called only by the focused MC/DC vector below.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_assert_directory_requirements_guards(void)
{
  uint32_t cursor_bytes = 0U;
  uint8_t  cursor_align = 0U;
  uint16_t cursor_count = 0U;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_io_vfs_dir_requirements(nullptr, &cursor_bytes, &cursor_align, &cursor_count));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_io_vfs_dir_requirements("cursor:/", nullptr, &cursor_align, &cursor_count));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_io_vfs_dir_requirements("cursor:/", &cursor_bytes, nullptr, &cursor_count));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_io_vfs_dir_requirements("cursor:/", &cursor_bytes, &cursor_align, nullptr));
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_io_vfs_dir_requirements("cursor:/", &cursor_bytes, &cursor_align, &cursor_count));
  TEST_ASSERT_EQ(sizeof(ra8_fs_dir_t), cursor_bytes);
  TEST_ASSERT_EQ(_Alignof(ra8_fs_dir_t), cursor_align);
  TEST_ASSERT(cursor_count != 0U);
}

/**
 * @brief Exercise directory open, iteration, state, and close guards.
 * @details Opens one real native cursor while copied handles independently
 *          exercise the closed and missing-format state operands.
 * @pre The `cursor` mount is live and supports a native directory cursor.
 * @pre The root directory is safe to enumerate without retaining entry bytes.
 * @post Every nullable and invalid-state operand has both outcomes.
 * @post The real cursor is closed before return.
 * @note Invalid copied handles never own or release the real workspace.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_assert_directory_lifecycle_guards(void)
{
  ra8_fs_dir_t     workspace = {};
  ra8_io_vfs_dir_t directory = {};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_io_vfs_dir_open(nullptr, &directory, &workspace, sizeof(workspace)));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_io_vfs_dir_open("cursor:/", nullptr, &workspace, sizeof(workspace)));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_io_vfs_dir_open("cursor:/", &directory, nullptr, sizeof(workspace)));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_io_vfs_dir_open("cursor:/", &directory, &workspace, sizeof(workspace)));

  ra8_fs_dirent_t entry   = {};
  bool            present = false;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_io_vfs_dir_next(nullptr, &entry, &present));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_io_vfs_dir_next(&directory, nullptr, &present));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_io_vfs_dir_next(&directory, &entry, nullptr));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_dir_next(&directory, &entry, &present));

  ra8_io_vfs_dir_t invalid = directory;
  invalid.is_open          = false;
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_io_vfs_dir_next(&invalid, &entry, &present));
  invalid        = directory;
  invalid.format = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_io_vfs_dir_next(&invalid, &entry, &present));

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_io_vfs_dir_close(nullptr));
  invalid         = directory;
  invalid.is_open = false;
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_io_vfs_dir_close(&invalid));
  invalid        = directory;
  invalid.format = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_io_vfs_dir_close(&invalid));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_dir_close(&directory));
}

/**
 * @brief Prove every public directory-cursor guard independently.
 * @details Formats and mounts a native FAT volume, then supplies one invalid
 *          operand at a time before exercising the successful cursor lifecycle.
 *
 * @par MC/DC:
 * Covers `libs/ra8_io/src/ra8_io_vfs.c@ra8_io_vfs_dir_requirements`,
 * `libs/ra8_io/src/ra8_io_vfs.c@ra8_io_vfs_dir_open`,
 * `libs/ra8_io/src/ra8_io_vfs.c@ra8_io_vfs_dir_next`, and
 * `libs/ra8_io/src/ra8_io_vfs.c@ra8_io_vfs_dir_close`. The all-valid calls
 * make every guard operand false. Each preceding call makes exactly one null
 * operand true; copied directory handles independently make `!is_open` and
 * `format == nullptr` true while the other state operand remains false.
 *
 * @pre The native fixture storage is exclusively owned by this test.
 * @pre FAT16 formatting fits within ::k_t_native_blocks.
 * @post Every nullable operand and invalid-state operand has both outcomes.
 * @post The real cursor and mounted volume are closed before return.
 * @note No invalid handle reaches the format callback.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_mcdc_directory_cursor_guards(void)
{
  TEST_BEGIN("vfs directory cursor guards");
  ra8_io_blockdev_ram_state_t state    = {};
  ra8_io_blockdev_t           blockdev = {};
  ra8_fs_backend_t            backend  = {};
  TEST_ASSERT_EQ(k_ra8_ok, internal_make_backend(s_native_a, &state, &blockdev, &backend));
  const ra8_fs_format_opts_t opts = {.type = k_ra8_fs_type_fat16, .label = "CURSOR"};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_format(&backend, &opts));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_fsfmt_init());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_init());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_mount_auto("cursor", &backend));
  internal_assert_directory_requirements_guards();
  internal_assert_directory_lifecycle_guards();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_unmount("cursor"));
  TEST_END("vfs directory cursor guards");
}

int main(void)
{
  ra8_log_set_byte_sink(internal_log_sink, nullptr);
  internal_test_mcdc_directory_cursor_guards();
  internal_test_native_dispatch();
  internal_test_foreign_end_to_end();
  return 0;
}
