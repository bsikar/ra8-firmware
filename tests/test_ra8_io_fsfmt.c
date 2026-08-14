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

#include <stdint.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_fs.h"
#include "ra8_io_blockdev.h"
#include "ra8_io_blockdev_ram.h"
#include "ra8_io_fsfmt.h"
#include "ra8_io_vfs.h"
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
static const uint8_t k_stub_payload[] = "registered-format-data";

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

/** @brief True when block zero carries the foreign marker. */
static bool stub_probe(const ra8_fs_backend_t* backend)
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

/** @brief Mount the single static foreign context. */
static ra8_err_t stub_mount(const ra8_fs_backend_t* backend, void** out_mount)
{
  if (!stub_probe(backend)) {
    return k_ra8_err_not_found;
  }
  if (s_stub_mount.mounted) {
    return k_ra8_err_busy;
  }
  s_stub_mount.mounted = true;
  *out_mount           = &s_stub_mount;
  return k_ra8_ok;
}

/** @brief Release the single static foreign context. */
static ra8_err_t stub_unmount(void* mount_ctx)
{
  if (mount_ctx != &s_stub_mount) {
    return k_ra8_err_invalid_arg;
  }
  s_stub_mount.mounted = false;
  s_stub_unmount_calls++;
  return k_ra8_ok;
}

/** @brief Open `/README.TXT` for reading from the foreign format. */
static ra8_err_t stub_open(void* mount_ctx, const char* path, ra8_fs_mode_t mode, void** out_file)
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

/** @brief Close the static foreign file. */
static ra8_err_t stub_close(void* file_ctx)
{
  if (file_ctx != &s_stub_file) {
    return k_ra8_err_invalid_arg;
  }
  s_stub_file.open = false;
  return k_ra8_ok;
}

/** @brief Read the immutable foreign payload. */
static ra8_err_t stub_read(void* file_ctx, void* buf, uint32_t bytes, uint32_t* out_read)
{
  if (file_ctx != &s_stub_file) {
    return k_ra8_err_invalid_arg;
  }
  uint32_t remaining = (uint32_t)sizeof(k_stub_payload) - 1U - s_stub_file.offset;
  if (bytes < remaining) {
    remaining = bytes;
  }
  (void)memcpy(buf, &k_stub_payload[s_stub_file.offset], remaining);
  s_stub_file.offset += remaining;
  *out_read = remaining;
  return k_ra8_ok;
}

/** @brief Seek within the immutable foreign payload, clamped at EOF. */
static ra8_err_t stub_seek(void* file_ctx, uint64_t offset_bytes)
{
  if (file_ctx != &s_stub_file) {
    return k_ra8_err_invalid_arg;
  }
  const uint32_t size = (uint32_t)sizeof(k_stub_payload) - 1U;
  if (offset_bytes > size) {
    s_stub_file.offset = size;
  } else {
    s_stub_file.offset = (uint32_t)offset_bytes;
  }
  return k_ra8_ok;
}

/** @brief Report the foreign stream offset. */
static ra8_err_t stub_tell(const void* file_ctx, uint64_t* out_offset)
{
  if (file_ctx != &s_stub_file) {
    return k_ra8_err_invalid_arg;
  }
  *out_offset = s_stub_file.offset;
  return k_ra8_ok;
}

/** @brief Report the foreign stream size. */
static ra8_err_t stub_size(const void* file_ctx, uint64_t* out_bytes)
{
  if (file_ctx != &s_stub_file) {
    return k_ra8_err_invalid_arg;
  }
  *out_bytes = (uint64_t)sizeof(k_stub_payload) - 1U;
  return k_ra8_ok;
}

/** @brief Report root/file metadata in the foreign format. */
static ra8_err_t stub_stat(void* mount_ctx, const char* path, ra8_fs_stat_t* out)
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
    out->size_bytes = (uint64_t)sizeof(k_stub_payload) - 1U;
    return k_ra8_ok;
  }
  return k_ra8_err_not_found;
}

/** @brief Enumerate the foreign root's one file. */
static ra8_err_t
stub_listdir(void* mount_ctx, const char* path, ra8_fs_listdir_cb_t cb, void* cb_ctx)
{
  if (mount_ctx != &s_stub_mount) {
    return k_ra8_err_invalid_state;
  }
  if (strcmp(path, "/") != 0) {
    return k_ra8_err_not_found;
  }
  cb("README.TXT", (uint8_t)k_ra8_fs_attr_read_only, (uint64_t)sizeof(k_stub_payload) - 1U, cb_ctx);
  return k_ra8_ok;
}

/** @brief Complete read-side ops; every mutation is intentionally absent. */
static const ra8_io_fsfmt_ops_t k_stub_ops = {
  .probe      = stub_probe,
  .mount      = stub_mount,
  .unmount    = stub_unmount,
  .open       = stub_open,
  .close      = stub_close,
  .read       = stub_read,
  .write      = nullptr,
  .seek       = stub_seek,
  .tell       = stub_tell,
  .size       = stub_size,
  .sync       = nullptr,
  .stat       = stub_stat,
  .listdir    = stub_listdir,
  .unlink     = nullptr,
  .rename     = nullptr,
  .mkdir      = nullptr,
  .rmdir      = nullptr,
  .free_space = nullptr,
};

/** @brief Registered read-only foreign format. */
static const ra8_io_fsfmt_t k_stub_format = {
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
  .ops = &k_stub_ops,
};

/** @brief Count one directory callback. */
static void count_entry(const char* name, uint8_t attr, uint64_t size, void* ctx)
{
  TEST_ASSERT(strcmp(name, "README.TXT") == 0);
  TEST_ASSERT_EQ(k_ra8_fs_attr_read_only, attr);
  TEST_ASSERT_EQ(sizeof(k_stub_payload) - 1U, size);
  uint32_t* count = (uint32_t*)ctx;
  (*count)++;
}

/** @brief Count any native directory entry. */
static void count_any(const char* name, uint8_t attr, uint64_t size, void* ctx)
{
  (void)name;
  (void)attr;
  (void)size;
  uint32_t* count = (uint32_t*)ctx;
  (*count)++;
}

/** @brief Initialize a RAM block device/backend over caller-provided storage. */
static ra8_err_t make_backend(uint8_t*                     storage,
                              ra8_io_blockdev_ram_state_t* state,
                              ra8_io_blockdev_t*           blockdev,
                              ra8_fs_backend_t*            backend)
{
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_io_blockdev_ram_init(blockdev, state, storage, (uint64_t)k_t_native_blocks, false));
  return ra8_io_blockdev_as_fs_backend(blockdev, backend);
}

/** @brief Apply one deterministic mutation sequence directly through ra8_fs. */
static void direct_sequence(const ra8_fs_backend_t* backend)
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
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_listdir(mount, "/", count_any, &listed));
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

/** @brief Apply the identical mutation sequence through format-neutral VFS ops. */
static void dispatched_sequence(const ra8_fs_backend_t* backend)
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
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_listdir("vol:/", count_any, &listed));
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

/** @brief Format two volumes, run direct/dispatched sequences, compare every byte. */
static void assert_native_byte_identity(ra8_fs_type_t type)
{
  ra8_io_blockdev_ram_state_t state_a = {};
  ra8_io_blockdev_ram_state_t state_b = {};
  ra8_io_blockdev_t           bd_a    = {};
  ra8_io_blockdev_t           bd_b    = {};
  ra8_fs_backend_t            be_a    = {};
  ra8_fs_backend_t            be_b    = {};
  TEST_ASSERT_EQ(k_ra8_ok, make_backend(s_native_a, &state_a, &bd_a, &be_a));
  TEST_ASSERT_EQ(k_ra8_ok, make_backend(s_native_b, &state_b, &bd_b, &be_b));
  ra8_fs_format_opts_t opts = {.type = type, .label = "DISPATCH"};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_format(&be_a, &opts));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_format(&be_b, &opts));
  direct_sequence(&be_a);
  dispatched_sequence(&be_b);
  TEST_ASSERT(memcmp(s_native_a, s_native_b, sizeof(s_native_a)) == 0);
}

/** @brief FAT and exFAT pass through ops with byte-identical images and truthful caps. */
static void test_native_dispatch(void)
{
  TEST_BEGIN("fsfmt native dispatch byte identity");
  assert_native_byte_identity(k_ra8_fs_type_fat16);
  assert_native_byte_identity(k_ra8_fs_type_exfat);

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

/** @brief Registered foreign format mounts, serves data, and gates every mutation. */
static void test_foreign_end_to_end(void)
{
  TEST_BEGIN("fsfmt registered foreign end-to-end");
  s_stub_mount         = (stub_mount_t){};
  s_stub_file          = (stub_file_t){};
  s_stub_open_calls    = 0U;
  s_stub_unmount_calls = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_fsfmt_init());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_fsfmt_register(&k_stub_format));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_init());

  ra8_io_blockdev_ram_state_t state = {};
  ra8_io_blockdev_t           bd    = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_io_blockdev_ram_init(&bd, &state, s_stub_disk, k_t_stub_blocks, false));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_erase(&bd, 0U, k_t_stub_blocks));
  uint8_t block[(size_t)k_ra8_io_block_size_bytes] = {};
  block[0]                                         = (uint8_t)k_t_stub_magic;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_write(&bd, 0U, 1U, block));
  ra8_fs_backend_t backend = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_as_fs_backend(&bd, &backend));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_mount_auto("foreign", &backend));

  ra8_io_fsfmt_caps_t caps = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_get_caps("foreign", &caps));
  TEST_ASSERT(caps.read_only);
  TEST_ASSERT(caps.case_sensitive);
  TEST_ASSERT(!caps.supports_mkdir);

  uint32_t listed = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_listdir("foreign:/", count_entry, &listed));
  TEST_ASSERT_EQ(1U, listed);
  ra8_io_vfs_stat_t stat = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_stat("foreign:/README.TXT", &stat));
  TEST_ASSERT(stat.exists);
  TEST_ASSERT_EQ(sizeof(k_stub_payload) - 1U, stat.size_bytes);

  ra8_io_vfs_file_t* file = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_file_open("foreign:/README.TXT", k_ra8_fs_mode_read, &file));
  TEST_ASSERT_EQ(k_ra8_err_busy, ra8_io_vfs_unmount("foreign"));
  uint64_t size = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_file_size(file, &size));
  TEST_ASSERT_EQ(sizeof(k_stub_payload) - 1U, size);
  uint8_t  got[sizeof(k_stub_payload)] = {};
  uint32_t got_len                     = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_file_read(file, got, sizeof(got), &got_len));
  TEST_ASSERT_EQ(sizeof(k_stub_payload) - 1U, got_len);
  TEST_ASSERT(memcmp(got, k_stub_payload, got_len) == 0);
  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_io_vfs_file_sync(file));
  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_io_vfs_file_write(file, got, 1U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_file_close(file));

  const uint32_t opens_before = s_stub_open_calls;
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
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_unmount("foreign"));
  TEST_ASSERT_EQ(1U, s_stub_unmount_calls);
  TEST_END("fsfmt registered foreign end-to-end");
}

int32_t main(void)
{
  test_native_dispatch();
  test_foreign_end_to_end();
  (void)fprintf(stderr, "[OK  ] test_ra8_io_fsfmt.c\n");
  return 0;
}
