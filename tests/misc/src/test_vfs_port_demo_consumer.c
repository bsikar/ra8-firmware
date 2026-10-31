/**
 * @file test_vfs_port_demo_consumer.c
 * @brief Host consumer test: vfs_port_demo's binding legs, no board needed.
 *
 * @details
 * RA8FW-1009 asks each `if_ra8_vfs` consumer to be exercised on the host.
 * `vfs_port_demo` is the adapter's first example-app consumer, but it is a
 * board app: its observable surface is a console over SCI8, so CI could not
 * reach it. This test runs the SAME legs on the host over the same medium the
 * demo stands up itself (a RAM block device formatted FAT12), so every binding
 * path the app walks is asserted, byte for byte, with no card and no board:
 *
 *   1. Bind: RAM block device -> `ra8_fs` FAT12 format + mount -> the
 *      `ra8_io_vfs` named mount `ram` -> `fw_fs_ra8_vfs_init`.
 *   2. Capabilities: the bits the header promises (namespace, stream,
 *      transactions) present; the bits FAT cannot honour (durable sync,
 *      atomic replace, symlinks) absent; workspaces fit and sit on the
 *      alignments the caps report.
 *   3. Round trip: mkdir, write_truncate, byte-exact read back, stat size.
 *   4. Directory walk: the cursor finds the one entry exactly once.
 *   5. Staged publish: `create_new` transaction validates and commits, a
 *      second `create_new` to the same destination is refused, and the
 *      published file is untouched.
 *   6. Lexical guard: `/../escape` is refused as access-denied.
 *   7. Unwind: unlink, release the VFS name, unmount the volume.
 *
 * The legs and constants mirror examples/ek_ra8d2/hw_pending/vfs_port_demo,
 * whose main.c is the binding recipe; keep the two in step.
 *
 * @author Brighton Sikarskie
 * @date 2026-10-30
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "fw_if_fs.h"
#include "fw_if_fs_ra8_vfs.h"
#include "ra8_err.h"
#include "ra8_fs.h"
#include "ra8_io_blockdev.h"
#include "ra8_io_blockdev_ram.h"
#include "ra8_io_vfs.h"
#include "unity_minimal.h"

/** @brief Demo-mirrored constants (medium geometry, workspace ceilings). */
typedef enum : uint32_t {
  k_consumer_disk_blocks = 512U,   /**< RAM-disk sectors; FAT12 fits.         */
  k_consumer_file_work   = 64U,    /**< Backend file-handle workspace bytes.  */
  k_consumer_txn_work    = 2048U,  /**< Backend transaction workspace bytes.  */
  k_consumer_dir_work    = 1024U,  /**< Backend directory-cursor workspace.   */
} consumer_limits_t;

/** @brief Payload written through the portable stream API ("RA8D2!"). */
static const uint8_t k_consumer_payload[] = {0x52U, 0x41U, 0x38U, 0x44U, 0x32U, 0x21U};

/** @brief Payload published through the staged transaction. */
static const uint8_t k_consumer_staged[] = {0xDEU, 0xADU, 0xBEU, 0xEFU};

/** @brief Portable paths; the mount name never appears in them. */
static const char* const k_consumer_dir       = "/books";
static const char* const k_consumer_file      = "/books/a.bin";
static const char* const k_consumer_leaf      = "a.bin";
static const char* const k_consumer_published = "/books/staged.bin";
static const char* const k_consumer_escape    = "/../escape";

/** @brief Mount name registered with ra8_io_vfs (the demo's `ram`). */
static const char* const k_consumer_mount_name = "ram";

static uint8_t s_disk[(size_t)k_consumer_disk_blocks * (size_t)k_ra8_io_block_size_bytes];

static alignas(max_align_t) uint8_t s_file_work[k_consumer_file_work];
static alignas(max_align_t) uint8_t s_dir_work[k_consumer_dir_work];
static alignas(max_align_t) uint8_t s_txn_work[k_consumer_txn_work];

static ra8_io_blockdev_t           s_blockdev;
static ra8_io_blockdev_ram_state_t s_ram_state;
static ra8_fs_backend_t            s_backend;
static ra8_fs_mount_t*             s_mount;
static fw_fs_t                     s_fs;
static fw_fs_ra8_vfs_state_t       s_adapter;

/**
 * @brief Bind the demo's stack: RAM disk, FAT12 volume, `ram` mount, adapter.
 *
 * @return void
 */
static void internal_bind(void)
{
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_ram_init(&s_blockdev,
                                                    &s_ram_state,
                                                    s_disk,
                                                    (uint32_t)k_consumer_disk_blocks,
                                                    false));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_as_fs_backend(&s_blockdev, &s_backend));

  const ra8_fs_format_opts_t format
      = {.type = k_ra8_fs_type_fat12, .label = "PORT", .sectors_per_cluster = 0U};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_format(&s_backend, &format));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &s_mount));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_init());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_mount(k_consumer_mount_name, s_mount));

  const fw_fs_ra8_vfs_cfg_t cfg
      = {.mount_name = k_consumer_mount_name, .mount = s_mount, .removable_media = false};
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_ra8_vfs_init(&s_fs, &s_adapter, &cfg));
}

/**
 * @brief Check the adapter's advertised capability bits and workspace bounds.
 *
 * @return void
 */
static void test_caps_honest_and_fits(void)
{
  TEST_BEGIN("vfs_port_demo consumer: caps honest, workspaces fit and aligned");
  internal_bind();

  fw_fs_caps_t caps = {0};
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_get_caps(&s_fs, &caps));

  const uint32_t required = (uint32_t)k_fw_fs_cap_namespace | (uint32_t)k_fw_fs_cap_stream
                            | (uint32_t)k_fw_fs_cap_transactions;
  const uint32_t refused = (uint32_t)k_fw_fs_cap_durable_file_sync
                           | (uint32_t)k_fw_fs_cap_durable_directory_sync
                           | (uint32_t)k_fw_fs_cap_atomic_replace
                           | (uint32_t)k_fw_fs_cap_symlinks;

  TEST_ASSERT((caps.flags & required) == required);
  TEST_ASSERT((caps.flags & refused) == 0U);
  TEST_ASSERT(caps.file_workspace_bytes <= (uint32_t)k_consumer_file_work);
  TEST_ASSERT(caps.transaction_workspace_bytes <= (uint32_t)k_consumer_txn_work);
  TEST_ASSERT(caps.directory_workspace_bytes <= (uint32_t)k_consumer_dir_work);
  TEST_ASSERT(((uintptr_t)s_file_work % (uintptr_t)caps.file_workspace_align) == 0U);
  TEST_ASSERT(((uintptr_t)s_txn_work % (uintptr_t)caps.transaction_workspace_align) == 0U);
  TEST_ASSERT(((uintptr_t)s_dir_work % (uintptr_t)caps.directory_workspace_align) == 0U);
  TEST_END("vfs_port_demo consumer: caps honest, workspaces fit and aligned");
}

/**
 * @brief Round-trip one file through the portable namespace and stream API.
 *
 * @return void
 */
static void test_round_trip(void)
{
  TEST_BEGIN("vfs_port_demo consumer: write/read round trip byte-exact");
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_mkdir(&s_fs.names, k_consumer_dir));

  fw_fs_file_t file = {0};
  TEST_ASSERT_EQ(k_ra8_ok,
                 fw_fs_open(&s_fs.streams,
                            k_consumer_file,
                            k_fw_fs_open_write_truncate,
                            &file,
                            s_file_work,
                            (uint32_t)sizeof(s_file_work)));
  uint32_t written = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 fw_fs_write(&file, k_consumer_payload, (uint32_t)sizeof(k_consumer_payload), &written));
  TEST_ASSERT_EQ((uint32_t)sizeof(k_consumer_payload), written);
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_close(&file));

  TEST_ASSERT_EQ(k_ra8_ok,
                 fw_fs_open(&s_fs.streams,
                            k_consumer_file,
                            k_fw_fs_open_read,
                            &file,
                            s_file_work,
                            (uint32_t)sizeof(s_file_work)));
  uint8_t  actual[sizeof(k_consumer_payload)] = {0};
  uint32_t got                                = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 fw_fs_read(&file, actual, (uint32_t)sizeof(actual), &got));
  TEST_ASSERT_EQ((uint32_t)sizeof(k_consumer_payload), got);
  TEST_ASSERT(memcmp(actual, k_consumer_payload, sizeof(k_consumer_payload)) == 0);
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_close(&file));

  fw_fs_stat_t stat = {0};
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_stat(&s_fs.names, k_consumer_file, &stat));
  TEST_ASSERT(stat.exists);
  TEST_ASSERT(k_fw_fs_node_file == stat.type);
  TEST_ASSERT((uint64_t)sizeof(k_consumer_payload) == stat.size_bytes);
  TEST_END("vfs_port_demo consumer: write/read round trip byte-exact");
}

/**
 * @brief Walk /books with the cursor and find the leaf exactly once.
 *
 * @return void
 */
static void test_walk_dir(void)
{
  TEST_BEGIN("vfs_port_demo consumer: directory cursor finds the leaf once");
  fw_fs_dir_t cursor = {0};
  TEST_ASSERT_EQ(k_ra8_ok,
                 fw_fs_dir_open(&s_fs.names,
                                k_consumer_dir,
                                &cursor,
                                s_dir_work,
                                (uint32_t)sizeof(s_dir_work)));

  uint32_t matched = 0U;
  uint32_t seen    = 0U;
  for (;;) {
    fw_fs_dirent_value_t entry   = {0};
    bool                 present = false;
    const ra8_err_t err = fw_fs_dir_next(&cursor, &entry, &present);
    TEST_ASSERT_EQ(k_ra8_ok, err);
    if (!present) {
      break;
    }
    seen++;
    if (strcmp(entry.name, k_consumer_leaf) == 0) {
      matched++;
    }
  }
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_dir_close(&cursor));
  TEST_ASSERT_EQ(1U, matched);
  TEST_ASSERT(seen >= 1U);
  TEST_END("vfs_port_demo consumer: directory cursor finds the leaf once");
}

/**
 * @brief Validate the staged-read callback against the expected bytes.
 *
 * @param[out] staged Handle to the staged, not-yet-published file.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok The stage held the expected bytes.
 */
static ra8_err_t internal_validate_stage(void* ctx, fw_fs_file_t* staged)
{
  (void)ctx;
  uint8_t  actual[sizeof(k_consumer_staged)] = {0};
  uint32_t got                               = 0U;
  const ra8_err_t err = fw_fs_read(staged, actual, (uint32_t)sizeof(actual), &got);
  if (err != k_ra8_ok) {
    return err;
  }
  const bool exact = (got == (uint32_t)sizeof(k_consumer_staged))
                     && (memcmp(actual, k_consumer_staged, sizeof(k_consumer_staged)) == 0);
  return exact ? k_ra8_ok : k_ra8_err_invalid_arg;
}

/**
 * @brief Publish through create_new, then prove the collision guard holds.
 *
 * @return void
 */
static void test_publish_staged(void)
{
  TEST_BEGIN("vfs_port_demo consumer: staged publish, second create_new refused");
  fw_fs_transaction_t txn = {0};
  (void)memset(s_txn_work, 0, sizeof(s_txn_work));
  TEST_ASSERT_EQ(k_ra8_ok,
                 fw_fs_transaction_begin(&s_fs.transactions,
                                         k_consumer_published,
                                         k_fw_fs_txn_create_new,
                                         &txn,
                                         s_txn_work,
                                         (uint32_t)sizeof(s_txn_work)));

  uint32_t written = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 fw_fs_transaction_write(&txn,
                                         k_consumer_staged,
                                         (uint32_t)sizeof(k_consumer_staged),
                                         &written));
  TEST_ASSERT_EQ((uint32_t)sizeof(k_consumer_staged), written);
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_transaction_validate(&txn, internal_validate_stage, nullptr));

  bool published = false;
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_transaction_commit(&txn, &published));
  TEST_ASSERT(published);

  /* create_new must refuse a destination that now exists. */
  fw_fs_transaction_t again  = {0};
  const ra8_err_t     denied = fw_fs_transaction_begin(&s_fs.transactions,
                                                   k_consumer_published,
                                                   k_fw_fs_txn_create_new,
                                                   &again,
                                                   s_txn_work,
                                                   (uint32_t)sizeof(s_txn_work));
  TEST_ASSERT(k_ra8_ok != denied);
  if (k_ra8_ok == denied) {
    (void)fw_fs_transaction_abort(&again);
  }

  fw_fs_stat_t stat = {0};
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_stat(&s_fs.names, k_consumer_published, &stat));
  TEST_ASSERT(stat.exists);
  TEST_ASSERT((uint64_t)sizeof(k_consumer_staged) == stat.size_bytes);
  TEST_END("vfs_port_demo consumer: staged publish, second create_new refused");
}

/**
 * @brief The lexical guard refuses traversal before any backend sees it.
 *
 * @return void
 */
static void test_guard(void)
{
  TEST_BEGIN("vfs_port_demo consumer: /../escape refused access-denied");
  fw_fs_stat_t stat = {0};
  TEST_ASSERT_EQ(k_ra8_err_access_denied, fw_fs_stat(&s_fs.names, k_consumer_escape, &stat));
  TEST_END("vfs_port_demo consumer: /../escape refused access-denied");
}

/**
 * @brief Remove what the legs created and release the volume cleanly.
 *
 * @return void
 */
static void test_unwind(void)
{
  TEST_BEGIN("vfs_port_demo consumer: unlink, release mount, unmount volume");
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_unlink(&s_fs.names, k_consumer_file));
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_unlink(&s_fs.names, k_consumer_published));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_unmount(k_consumer_mount_name));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(s_mount));
  s_mount = nullptr;
  TEST_END("vfs_port_demo consumer: unlink, release mount, unmount volume");
}

/**
 * @brief Run every demo-mirrored leg in order.
 * @return 0 when all sections pass.
 */
int main(void)
{
  test_caps_honest_and_fits();
  test_round_trip();
  test_walk_dir();
  test_publish_staged();
  test_guard();
  test_unwind();
  return 0;
}
