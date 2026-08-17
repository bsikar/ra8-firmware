/**
 * @file mdl_test_storage.c
 * @brief Hosted portable-storage composition for media tests.
 * @details Binds each media test process to a root-confined POSIX storage
 *          backend using fixed, caller-owned workspaces.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#include "mdl_test_storage.h"

#include <stddef.h>
#include <stdint.h>

#include "fw_if_fs_posix.h"

/** @brief Generic hosted workspace capacity for one backend object. */
typedef enum : uint16_t {
  k_test_storage_work_bytes = 4096U, /**< Opaque file/transaction state. */
} mdl_test_storage_limit_t;

/** @brief Maximally aligned generic backend workspace. */
typedef struct {
  alignas(max_align_t) uint8_t bytes[k_test_storage_work_bytes]; /**< Opaque backend storage. */
} mdl_test_storage_workspace_t;

static fw_fs_t                      s_test_fs;
static fw_fs_posix_state_t          s_test_posix = {.root_fd = -1};
static mdl_storage_t                s_test_storage;
static mdl_test_storage_workspace_t s_test_file_workspace;
static mdl_test_storage_workspace_t s_test_transaction_workspace;
static uint8_t                      s_test_io[k_mdl_storage_io_bytes];

ra8_err_t mdl_test_storage_init(void)
{
  const fw_fs_posix_cfg_t cfg = {.root_path = "/", .removable_media = false};
  ra8_err_t               err = fw_fs_posix_init(&s_test_fs, &s_test_posix, &cfg);
  if (err != k_ra8_ok) {
    return err;
  }
  err = mdl_storage_init(&s_test_storage,
                         &s_test_fs,
                         s_test_file_workspace.bytes,
                         sizeof(s_test_file_workspace.bytes),
                         s_test_transaction_workspace.bytes,
                         sizeof(s_test_transaction_workspace.bytes),
                         s_test_io,
                         sizeof(s_test_io));
  if (err != k_ra8_ok) {
    (void)fw_fs_posix_deinit(&s_test_posix);
  }
  return err;
}

mdl_storage_t* mdl_test_storage_get(void)
{
  return &s_test_storage;
}

ra8_err_t mdl_test_storage_publish(const char* path, const uint8_t* bytes, uint32_t length)
{
  mdl_storage_t* storage = mdl_test_storage_get();
  fw_fs_stat_t   stat    = {};
  ra8_err_t      error   = fw_fs_stat(&storage->fs->names, path, &stat);
  if ((error == k_ra8_ok) && stat.exists) {
    error = fw_fs_unlink(&storage->fs->names, path);
  }
  mdl_storage_txn_t writer = {};
  if (error == k_ra8_ok) {
    error = mdl_storage_txn_begin_new(&writer, storage, path);
  }
  if (error == k_ra8_ok) {
    error = mdl_storage_txn_write(&writer, bytes, length);
  }
  if (error == k_ra8_ok) {
    error = mdl_storage_txn_commit(&writer);
  } else if (writer.transaction.active) {
    const ra8_err_t aborted = mdl_storage_txn_abort(&writer);
    if (aborted != k_ra8_ok) {
      error = aborted;
    }
  }
  return error;
}

ra8_err_t mdl_test_storage_deinit(void)
{
  return fw_fs_posix_deinit(&s_test_posix);
}
