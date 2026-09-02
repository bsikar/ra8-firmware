/**
 * @file test_mdl_app_storage.c
 * @brief Portable media application-storage parity and fault qualification.
 * @details Runs production directory, removal, and validated create-new site
 *          publication policies over POSIX and RAM blockdev/FAT/VFS, then
 *          injects every transaction phase around a real POSIX backend.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "fw_if_fs_posix.h"
#include "fw_if_fs_ra8_vfs.h"
#include "mdl_app_storage_internal.h"
#include "mdl_hash.h"
#include "mdl_state_fs_fault.h"
#include "mdl_storage.h"
#include "ra8_attributes.h"
#include "ra8_fs.h"
#include "ra8_io_blockdev.h"
#include "ra8_io_blockdev_ram.h"
#include "ra8_io_vfs.h"
#include "unity_minimal.h"

/** @brief Fixed test capacities. */
typedef enum : uint32_t {
  k_app_storage_disk_blocks = 2048U, /**< One MiB real FAT12 RAM volume. */
  k_app_storage_work_bytes  = 8192U, /**< Backend-private workspace.     */
  k_app_storage_site_bytes  = 2048U, /**< Complete descriptor capture.   */
} app_storage_test_limit_t;

/** @brief Maximally aligned backend workspace. */
typedef struct {
  alignas(max_align_t) uint8_t bytes[k_app_storage_work_bytes]; /**< Opaque backend state. */
} internal_app_storage_workspace_t;

/** @brief Complete downloader storage binding and owned workspaces. */
typedef struct {
  mdl_storage_t                    storage;                    /**< Production binding. */
  internal_app_storage_workspace_t file;                       /**< Open-file state.    */
  internal_app_storage_workspace_t transaction;                /**< Transaction state.  */
  uint8_t                          io[k_mdl_storage_io_bytes]; /**< I/O scratch.        */
} internal_app_storage_binding_t;

/** @brief Shared real VFS composition used by setup and teardown. */
typedef struct {
  /** RAM-disk bytes. */
  uint8_t disk[(size_t)k_app_storage_disk_blocks * k_ra8_io_block_size_bytes];
  /** RAM block-device state. */
  ra8_io_blockdev_ram_state_t ram_state;
  /** Block-device facade. */
  ra8_io_blockdev_t blockdev;
  /** Filesystem backend. */
  ra8_fs_backend_t backend;
  /** Active filesystem mount. */
  ra8_fs_mount_t* mount;
} internal_app_storage_vfs_t;

static internal_app_storage_vfs_t s_app_storage_vfs;

/**
 * @brief Bind production downloader storage to one complete filesystem.
 * @details Initializes the production storage aggregate with caller-owned
 *          file, transaction, and I/O workspaces for the selected filesystem.
 * @param[out] binding Storage binding and workspace owner to initialize.
 * @param[in] fs Complete injected filesystem contract used by the binding.
 * @pre `binding` and `fs` are non-null.
 * @pre Every workspace in `binding` has its declared complete capacity.
 * @post `binding->storage` is initialized when the assertion succeeds.
 * @post No filesystem object is opened by this binding operation.
 * @note A failed production initialization terminates the current test.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_app_storage_bind(internal_app_storage_binding_t* binding,
                                                   const fw_fs_t*                  fs)
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
 * @brief Write a complete regular fixture through the portable stream seam.
 * @details Opens `path` with truncate semantics and requires forward progress
 *          until all caller bytes have been written and the stream is closed.
 * @param[in,out] storage Initialized production storage binding.
 * @param[in] path Filesystem-relative destination path.
 * @param[in] bytes Complete fixture byte sequence.
 * @param[in] length Number of bytes to write from `bytes`.
 * @pre `storage`, `path`, and `bytes` are non-null.
 * @pre `length` is representable by the injected stream interface.
 * @post The destination contains exactly the requested prefix on success.
 * @post The opened stream has been closed when this helper returns.
 * @note Any stream error or zero-progress write terminates the current test.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_app_storage_write(mdl_storage_t* storage,
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
 * @brief Read a complete bounded fixture through the portable stream seam.
 * @details Opens `path` for reading and accumulates bytes until EOF or the
 *          caller-supplied capacity is exhausted, then closes the stream.
 * @param[in,out] storage Initialized production storage binding.
 * @param[in] path Filesystem-relative source path.
 * @param[out] bytes Caller-owned destination buffer.
 * @param[in] capacity Writable capacity of `bytes` in bytes.
 * @return Number of bytes stored in `bytes`.
 * @retval 0 The source is empty.
 * @pre `storage`, `path`, and `bytes` are non-null.
 * @pre `capacity` is representable by the injected stream interface.
 * @post The returned prefix of `bytes` contains source data in order.
 * @post The opened stream has been closed when this helper returns.
 * @note Stream failures terminate the current test instead of returning an error.
 * @since 0.1.0
 */
RA8_INTERNAL static uint32_t internal_app_storage_read(mdl_storage_t* storage,
                                                       const char*    path,
                                                       uint8_t*       bytes,
                                                       uint32_t       capacity)
{
  fw_fs_file_t file = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 fw_fs_open(&storage->fs->streams,
                            path,
                            k_fw_fs_open_read,
                            &file,
                            storage->file_workspace,
                            storage->file_workspace_bytes));
  uint32_t offset = 0U;
  while (offset < capacity) {
    uint32_t read = 0U;
    TEST_ASSERT_EQ(k_ra8_ok, fw_fs_read(&file, &bytes[offset], capacity - offset, &read));
    if (read == 0U) {
      break;
    }
    offset += read;
  }
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_close(&file));
  return offset;
}

/**
 * @brief Assert one path is absent through the injected namespace.
 * @details Queries the production namespace contract and verifies that the
 *          result truthfully reports a nonexistent node.
 * @param[in] storage Initialized production storage binding.
 * @param[in] path Filesystem-relative path expected to be absent.
 * @pre `storage` and `path` are non-null.
 * @pre The namespace contract is initialized and callable.
 * @post The assertion has confirmed that `path` does not exist.
 * @post No namespace mutation has been requested.
 * @note A stat error or present node terminates the current test.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_app_storage_assert_absent(const mdl_storage_t* storage,
                                                            const char*          path)
{
  fw_fs_stat_t node = {};
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_stat(&storage->fs->names, path, &node));
  TEST_ASSERT(!node.exists);
}

/**
 * @brief Run success, create-new preservation, and removal parity vectors.
 * @details Exercises idempotent directory creation, validated create-new site
 *          publication, destination preservation, and regular-file removal.
 * @param[in,out] storage Initialized production storage binding.
 * @param[out] descriptor Caller-owned buffer receiving the site descriptor.
 * @return Length of the published descriptor in bytes.
 * @retval 0 Reserved for an empty descriptor; the test rejects this result.
 * @pre `storage` and `descriptor` are non-null.
 * @pre `descriptor` has `k_app_storage_site_bytes` writable bytes.
 * @post The returned descriptor bytes are identical across conforming backends.
 * @post The create-new destination remains unchanged after a collision.
 * @note Assertion failures terminate the active parity vector.
 * @since 0.1.0
 */
RA8_INTERNAL static uint32_t internal_app_storage_run(mdl_storage_t* storage,
                                                      uint8_t descriptor[k_app_storage_site_bytes])
{
  TEST_ASSERT_EQ(k_ra8_ok, priv_mdl_app_storage_ensure_directory(storage, "/sites"));
  TEST_ASSERT_EQ(k_ra8_ok, priv_mdl_app_storage_ensure_directory(storage, "/sites"));
  TEST_ASSERT_EQ(k_ra8_ok,
                 priv_mdl_app_storage_publish_site(storage,
                                                   "/sites/example.conf",
                                                   "https://www.example.com/manga",
                                                   "www.example.com",
                                                   "Example"));
  const uint32_t length =
    internal_app_storage_read(storage, "/sites/example.conf", descriptor, k_app_storage_site_bytes);
  TEST_ASSERT(length > 0U);
  TEST_ASSERT(length < k_app_storage_site_bytes);
  descriptor[length] = '\0';
  TEST_ASSERT(strstr((const char*)descriptor, "name = Example\n") != nullptr);
  TEST_ASSERT(strstr((const char*)descriptor, "host = www.example.com\n") != nullptr);
  uint64_t before = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, mdl_hash_file(storage, "/sites/example.conf", &before));
  TEST_ASSERT_EQ(k_ra8_err_exists,
                 priv_mdl_app_storage_publish_site(storage,
                                                   "/sites/example.conf",
                                                   "https://evil.invalid/",
                                                   "evil.invalid",
                                                   "Changed"));
  uint64_t after = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, mdl_hash_file(storage, "/sites/example.conf", &after));
  TEST_ASSERT_EQ(before, after);
  uint8_t        preserved[k_app_storage_site_bytes] = {};
  const uint32_t preserved_length =
    internal_app_storage_read(storage, "/sites/example.conf", preserved, sizeof(preserved));
  TEST_ASSERT_EQ(length, preserved_length);
  TEST_ASSERT(memcmp(descriptor, preserved, length) == 0);

  static const uint8_t regular[] = {1U, 2U, 3U, 4U};
  internal_app_storage_write(storage, "/sites/stale.bin", regular, sizeof(regular));
  bool removed = false;
  TEST_ASSERT_EQ(k_ra8_ok,
                 priv_mdl_app_storage_unlink_regular(storage, "/sites/stale.bin", &removed));
  TEST_ASSERT(removed);
  TEST_ASSERT_EQ(k_ra8_ok,
                 priv_mdl_app_storage_unlink_regular(storage, "/sites/stale.bin", &removed));
  TEST_ASSERT(!removed);
  return length;
}

/**
 * @brief Format and bind the real firmware RAM/FAT/VFS stack.
 * @details Initializes the RAM block device, formats and mounts FAT12, binds
 *          the VFS mount name, and exposes it through the portable filesystem.
 * @param[out] fs Complete portable filesystem contract to initialize.
 * @param[out] state Caller-owned VFS adapter state.
 * @pre `fs` and `state` are non-null.
 * @pre The process-global test block device and mount are not already active.
 * @post `fs` addresses a mounted real RAM/FAT/VFS composition.
 * @post The shared fixture owns the mount released by the parity test.
 * @note Any formatting or mount failure terminates the current test.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_app_storage_bind_vfs(fw_fs_t* fs, fw_fs_ra8_vfs_state_t* state)
{
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_io_blockdev_ram_init(&s_app_storage_vfs.blockdev,
                                          &s_app_storage_vfs.ram_state,
                                          s_app_storage_vfs.disk,
                                          k_app_storage_disk_blocks,
                                          false));
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_io_blockdev_as_fs_backend(&s_app_storage_vfs.blockdev, &s_app_storage_vfs.backend));
  const ra8_fs_format_opts_t format = {.type = k_ra8_fs_type_fat12, .label = "MDLAPP"};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_format(&s_app_storage_vfs.backend, &format));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_app_storage_vfs.backend, &s_app_storage_vfs.mount));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_init());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_mount("app", s_app_storage_vfs.mount));
  const fw_fs_ra8_vfs_cfg_t config = {.mount_name      = "app",
                                      .mount           = s_app_storage_vfs.mount,
                                      .removable_media = false};
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_ra8_vfs_init(fs, state, &config));
}

/**
 * @brief Qualify site publication and removal parity across real backends.
 * @details Creates independent POSIX and RAM/FAT/VFS compositions, runs the
 *          same production policy vectors, compares bytes, and tears both down.
 * @test Site publication and regular removal match across POSIX and FAT/VFS.
 * @pre The host can create a private temporary directory.
 * @pre The firmware RAM block-device and VFS test adapters are available.
 * @post Both filesystem compositions are unmounted and deinitialized.
 * @post The temporary POSIX root has been removed.
 * @note Any parity or cleanup failure terminates the test executable.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_app_storage_parity(void)
{
  TEST_BEGIN("media app storage POSIX/RAM-FAT-VFS parity");
  char posix_root[] = {'/', 't', 'm', 'p', '/', 'm', 'd', 'l', '_', 'a', 'p', 'p', '_', 's',
                       't', 'o', 'r', 'a', 'g', 'e', '_', 'X', 'X', 'X', 'X', 'X', 'X', '\0'};
  TEST_ASSERT(mkdtemp(posix_root) == posix_root);
  fw_fs_t                 posix_fs     = {};
  fw_fs_posix_state_t     posix_state  = {.root_fd = -1};
  const fw_fs_posix_cfg_t posix_config = {.root_path = posix_root, .removable_media = false};
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_posix_init(&posix_fs, &posix_state, &posix_config));
  internal_app_storage_binding_t posix = {};
  internal_app_storage_bind(&posix, &posix_fs);

  fw_fs_t               vfs_fs    = {};
  fw_fs_ra8_vfs_state_t vfs_state = {};
  internal_app_storage_bind_vfs(&vfs_fs, &vfs_state);
  internal_app_storage_binding_t vfs = {};
  internal_app_storage_bind(&vfs, &vfs_fs);

  uint8_t        posix_descriptor[k_app_storage_site_bytes] = {};
  uint8_t        vfs_descriptor[k_app_storage_site_bytes]   = {};
  const uint32_t posix_length = internal_app_storage_run(&posix.storage, posix_descriptor);
  const uint32_t vfs_length   = internal_app_storage_run(&vfs.storage, vfs_descriptor);
  TEST_ASSERT_EQ(posix_length, vfs_length);
  TEST_ASSERT(memcmp(posix_descriptor, vfs_descriptor, posix_length) == 0);

  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_unlink(&posix.storage.fs->names, "/sites/example.conf"));
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_rmdir(&posix.storage.fs->names, "/sites"));
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_unlink(&vfs.storage.fs->names, "/sites/example.conf"));
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_rmdir(&vfs.storage.fs->names, "/sites"));
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_posix_deinit(&posix_state));
  TEST_ASSERT_EQ(0, rmdir(posix_root));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_unmount("app"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(s_app_storage_vfs.mount));
  s_app_storage_vfs.mount = nullptr;
  TEST_END("media app storage POSIX/RAM-FAT/VFS parity");
}

/**
 * @brief Publish one fault-selected site and assert prepublication absence.
 * @details Enables one injected transaction fault, requires publication to
 *          fail, clears the fault, and verifies that no destination appeared.
 * @param[in,out] fault Injected filesystem fault controller.
 * @param[in,out] binding Production storage binding using `fault`.
 * @param[in] flag Single transaction fault selection.
 * @param[in] path Filesystem-relative destination used by this vector.
 * @pre `fault`, `binding`, and `path` are non-null.
 * @pre `path` is absent before the publication attempt.
 * @post The selected fault is cleared.
 * @post `path` remains absent after the prepublication failure.
 * @note Assertion failures terminate the active fault vector.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_app_storage_fault_case(mdl_state_fault_fs_t*           fault,
                                                         internal_app_storage_binding_t* binding,
                                                         uint32_t                        flag,
                                                         const char*                     path)
{
  fault->flags = flag;
  TEST_ASSERT(priv_mdl_app_storage_publish_site(&binding->storage,
                                                path,
                                                "https://example.com/",
                                                "example.com",
                                                "Example") != k_ra8_ok);
  fault->flags = k_mdl_state_fault_none;
  internal_app_storage_assert_absent(&binding->storage, path);
}

/**
 * @brief Exercise link, special-node, stat, and unlink fault policy.
 * @details Verifies namespace rejection and destination truth for symlinks,
 *          directories, injected stat failures, and injected unlink failures.
 * @param[in,out] fault Injected filesystem fault controller.
 * @param[in,out] binding Production storage binding using `fault`.
 * @pre `fault` and `binding` are non-null.
 * @pre The `/link`, `/real`, and `/fault` fixture namespace is installed.
 * @post No directory is created by the injected stat failure.
 * @post The unlink-fault destination remains present for later cleanup.
 * @note The helper leaves fault selection cleared on its normal return path.
 * @since 0.1.0
 */
RA8_INTERNAL static void
internal_app_storage_namespace_faults(mdl_state_fault_fs_t*           fault,
                                      internal_app_storage_binding_t* binding)
{
  TEST_ASSERT_EQ(k_ra8_err_access_denied,
                 priv_mdl_app_storage_ensure_directory(&binding->storage, "/link"));
  bool removed = false;
  TEST_ASSERT_EQ(k_ra8_err_access_denied,
                 priv_mdl_app_storage_unlink_regular(&binding->storage, "/link", &removed));
  TEST_ASSERT(!removed);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 priv_mdl_app_storage_unlink_regular(&binding->storage, "/real", &removed));
  fault->flags = k_mdl_state_fault_stat;
  TEST_ASSERT(priv_mdl_app_storage_ensure_directory(&binding->storage, "/newdir") != k_ra8_ok);
  fault->flags = k_mdl_state_fault_none;
  internal_app_storage_assert_absent(&binding->storage, "/newdir");
  static const uint8_t byte = 7U;
  internal_app_storage_write(&binding->storage, "/fault/remove.bin", &byte, sizeof(byte));
  fault->flags = k_mdl_state_fault_unlink;
  removed      = false;
  TEST_ASSERT(priv_mdl_app_storage_unlink_regular(&binding->storage,
                                                  "/fault/remove.bin",
                                                  &removed) != k_ra8_ok);
  TEST_ASSERT(!removed);
  fault->flags = k_mdl_state_fault_none;
}

/**
 * @brief Exercise every publication phase and postpublication truth.
 * @details Injects begin, write, validation, commit, and abort failures, then
 *          distinguishes a published destination from prepublication failure.
 * @param[in,out] fault Injected filesystem fault controller.
 * @param[in,out] binding Production storage binding using `fault`.
 * @pre `fault` and `binding` are non-null.
 * @pre The `/fault` fixture directory exists and is writable.
 * @post Every prepublication fault destination remains absent.
 * @post The post-commit fault destination truthfully remains published.
 * @note The helper clears fault selection before its final namespace query.
 * @since 0.1.0
 */
RA8_INTERNAL static void
internal_app_storage_transaction_faults(mdl_state_fault_fs_t*           fault,
                                        internal_app_storage_binding_t* binding)
{
  internal_app_storage_fault_case(fault, binding, k_mdl_state_fault_begin, "/fault/begin.conf");
  internal_app_storage_fault_case(fault,
                                  binding,
                                  k_mdl_state_fault_transaction_write,
                                  "/fault/write.conf");
  internal_app_storage_fault_case(fault,
                                  binding,
                                  k_mdl_state_fault_validate,
                                  "/fault/validate.conf");
  internal_app_storage_fault_case(fault,
                                  binding,
                                  k_mdl_state_fault_commit_before,
                                  "/fault/commit.conf");
  internal_app_storage_fault_case(fault,
                                  binding,
                                  k_mdl_state_fault_transaction_write | k_mdl_state_fault_abort,
                                  "/fault/abort.conf");
  fault->flags = k_mdl_state_fault_short_write;
  TEST_ASSERT_EQ(k_ra8_ok,
                 priv_mdl_app_storage_publish_site(&binding->storage,
                                                   "/fault/short.conf",
                                                   "https://example.com/",
                                                   "example.com",
                                                   "Example"));
  fault->flags = k_mdl_state_fault_commit_after;
  TEST_ASSERT(priv_mdl_app_storage_publish_site(&binding->storage,
                                                "/fault/after.conf",
                                                "https://example.com/",
                                                "example.com",
                                                "Example") != k_ra8_ok);
  fault->flags      = k_mdl_state_fault_none;
  fw_fs_stat_t node = {};
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_stat(&binding->storage.fs->names, "/fault/after.conf", &node));
  TEST_ASSERT(node.exists && (node.type == k_fw_fs_node_file));
}

/**
 * @brief Qualify transaction preservation and publication truth under faults.
 * @details Builds a private POSIX composition with a symlink fixture, injects
 *          namespace and transaction failures, and removes all created state.
 * @test Transaction faults preserve existing data and expose publication truth.
 * @pre The host supports private directories, symlinks, and raw POSIX storage.
 * @pre The injected fault filesystem can wrap the complete POSIX contract.
 * @post All published fixtures and namespace nodes have been removed.
 * @post The POSIX adapter is deinitialized and its private root is removed.
 * @note Any fault-policy or cleanup mismatch terminates the test executable.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_app_storage_faults(void)
{
  TEST_BEGIN("media app storage transaction faults");
  char posix_root[] = {'/', 't', 'm', 'p', '/', 'm', 'd', 'l', '_', 'a', 'p', 'p', '_',
                       'f', 'a', 'u', 'l', 't', '_', 'X', 'X', 'X', 'X', 'X', 'X', '\0'};
  TEST_ASSERT(mkdtemp(posix_root) == posix_root);
  fw_fs_t                 inner  = {};
  fw_fs_posix_state_t     state  = {.root_fd = -1};
  const fw_fs_posix_cfg_t config = {.root_path = posix_root, .removable_media = false};
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_posix_init(&inner, &state, &config));
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_mkdir(&inner.names, "/fault"));
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_mkdir(&inner.names, "/real"));
  char         real_host[k_fw_fs_path_cap];
  char         link_host[k_fw_fs_path_cap];
  const size_t root_length = strlen(posix_root);
  TEST_ASSERT((root_length + sizeof("/real")) <= sizeof(real_host));
  TEST_ASSERT((root_length + sizeof("/link")) <= sizeof(link_host));
  TEST_ASSERT(__builtin_snprintf(real_host, sizeof(real_host), "%s/real", posix_root) > 0);
  TEST_ASSERT(__builtin_snprintf(link_host, sizeof(link_host), "%s/link", posix_root) > 0);
  TEST_ASSERT_EQ(0, symlink(real_host, link_host));
  mdl_state_fault_fs_t fault = {};
  TEST_ASSERT_EQ(k_ra8_ok, mdl_state_fault_fs_init(&fault, &inner));
  internal_app_storage_binding_t binding = {};
  internal_app_storage_bind(&binding, &fault.fs);
  internal_app_storage_namespace_faults(&fault, &binding);
  internal_app_storage_transaction_faults(&fault, &binding);

  static const char* const files[] = {"/fault/short.conf",
                                      "/fault/after.conf",
                                      "/fault/remove.bin"};
  for (size_t i = 0U; i < (sizeof(files) / sizeof(files[0])); ++i) {
    TEST_ASSERT_EQ(k_ra8_ok, fw_fs_unlink(&binding.storage.fs->names, files[i]));
  }
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_rmdir(&binding.storage.fs->names, "/fault"));
  TEST_ASSERT_EQ(0, unlink(link_host));
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_rmdir(&binding.storage.fs->names, "/real"));
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_posix_deinit(&state));
  TEST_ASSERT_EQ(0, rmdir(posix_root));
  TEST_END("media app storage transaction faults");
}

int main(void)
{
  internal_test_app_storage_parity();
  internal_test_app_storage_faults();
  return 0;
}
