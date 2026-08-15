/**
 * @file test_ra8_mdl_storage_vfs.c
 * @brief Real FAT/VFS qualification for downloader transactional storage
 *
 * @details Exercises the production adapter over a RAM block device, real
 * `ra8_fs`, and named VFS routing. No mock filesystem or POSIX call sits in the
 * success path. A narrow backend wrapper injects media-removal, capacity, and
 * rename-write faults without changing the production filesystem.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8_fs.h"
#include "ra8_io_blockdev.h"
#include "ra8_io_blockdev_ram.h"
#include "ra8_io_vfs.h"
#include "ra8_log.h"
#include "ra8_mdl_storage_vfs.h"
#include "unity_minimal.h"

/** @brief Fixture capacities and payload constants. */
typedef enum : uint32_t {
  k_disk_blocks        = 16384U, /**< 8 MiB FAT16 RAM disk.             */
  k_payload_len        = 300U,   /**< Crosses several transfer writes.  */
  k_compare_chunk_len  = 64U,    /**< Bounded validation read chunk.    */
  k_payload_multiplier = 29U,    /**< Deterministic payload multiplier. */
  k_payload_offset     = 7U,     /**< Deterministic payload offset.     */
  k_byte_mask          = 0xFFU,  /**< Low-byte payload mask.            */
  k_digest_tag         = 0xA5U,  /**< Test validator digest marker.     */
  k_written_sentinel   = 99U,    /**< Detect failure-path output reset. */
} test_limits_t;

/** @brief Backend fault controller wrapped around the real RAM bridge. */
typedef struct {
  ra8_fs_backend_t inner;       /**< Real blockdev-to-fs bridge.   */
  ra8_err_t        write_error; /**< Injected write failure or OK. */
  bool             removed;     /**< All access fails while true.  */
} fault_backend_t;

/** @brief Artifact-validator fixture. */
typedef struct {
  const uint8_t* expected;     /**< Expected staged bytes.    */
  uint32_t       expected_len; /**< Expected length.          */
  uint32_t       calls;        /**< Invocation count.         */
  bool           fail;         /**< Force validation failure. */
} validator_t;

static uint8_t                     s_disk[(size_t)k_disk_blocks * k_ra8_io_block_size_bytes];
static ra8_io_blockdev_ram_state_t s_ram_state;
static ra8_io_blockdev_t           s_blockdev;
static ra8_fs_backend_t            s_raw_backend;
static fault_backend_t             s_fault;
static ra8_fs_backend_t            s_backend;
static ra8_fs_mount_t*             s_mount;

/** @brief Discard expected negative-path logs without host ITM MMIO. */
static void discard_log_byte(void* ctx, uint8_t byte)
{
  (void)ctx;
  (void)byte;
}

/** @brief Forward declaration for the staged-file test validator. */
static ra8_err_t validate_artifact(void*         ctx,
                                   const char*   staging_path,
                                   uint64_t      total_bytes,
                                   const uint8_t sha256[k_ra8_mdl_sha256_bytes]);

/** @brief Delegate a read unless media removal is injected. */
static ra8_err_t fault_read(void* ctx, uint64_t lba, uint32_t count, uint8_t* buf)
{
  fault_backend_t* const fault = ctx;
  if (fault->removed) {
    return k_ra8_err_hw_error;
  }
  return fault->inner.read_block(fault->inner.ctx, lba, count, buf);
}

/** @brief Delegate a write unless a capacity/removal fault is injected. */
static ra8_err_t fault_write(void* ctx, uint64_t lba, uint32_t count, const uint8_t* buf)
{
  fault_backend_t* const fault = ctx;
  if (fault->removed) {
    return k_ra8_err_hw_error;
  }
  if (fault->write_error != k_ra8_ok) {
    return fault->write_error;
  }
  return fault->inner.write_block(fault->inner.ctx, lba, count, buf);
}

/** @brief Delegate capacity unless media removal is injected. */
static ra8_err_t fault_capacity(void* ctx, uint64_t* blocks, uint32_t* block_size)
{
  fault_backend_t* const fault = ctx;
  if (fault->removed) {
    return k_ra8_err_hw_error;
  }
  return fault->inner.get_capacity(fault->inner.ctx, blocks, block_size);
}

/** @brief Delegate erase unless a capacity/removal fault is injected. */
static ra8_err_t fault_erase(void* ctx, uint64_t lba, uint64_t count)
{
  fault_backend_t* const fault = ctx;
  if (fault->removed) {
    return k_ra8_err_hw_error;
  }
  if (fault->write_error != k_ra8_ok) {
    return fault->write_error;
  }
  return fault->inner.erase_blocks(fault->inner.ctx, lba, count);
}

/** @brief Format and mount a fresh real FAT16 volume behind the fault wrapper. */
static void setup_volume(void)
{
  if (s_mount != nullptr) {
    TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(s_mount));
    s_mount = nullptr;
  }
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_init());
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_io_blockdev_ram_init(&s_blockdev, &s_ram_state, s_disk, k_disk_blocks, false));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_as_fs_backend(&s_blockdev, &s_raw_backend));
  ra8_fs_format_opts_t opts = {};
  opts.type                 = k_ra8_fs_type_fat16;
  opts.label                = "MDLVFS";
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_format(&s_raw_backend, &opts));
  s_fault   = (fault_backend_t){.inner = s_raw_backend};
  s_backend = (ra8_fs_backend_t){.read_block   = fault_read,
                                 .write_block  = fault_write,
                                 .get_capacity = fault_capacity,
                                 .erase_blocks = fault_erase,
                                 .ctx          = &s_fault};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &s_mount));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_mount("ram", s_mount));
}

/** @brief Bind an adapter with one test-reserved staging leaf. */
static void
bind_adapter(ra8_mdl_storage_vfs_t* storage, ra8_mdl_storage_iface_t* iface, validator_t* validator)
{
  const ra8_mdl_storage_vfs_config_t config = {
    .stage_leaf   = "MDLSTAGE.TMP",
    .validate     = validator == nullptr ? nullptr : validate_artifact,
    .validate_ctx = validator,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mdl_storage_vfs_init(storage, &config, iface));
}

/** @brief Read and compare a complete VFS file. */
static ra8_err_t compare_file(const char* path, const uint8_t* expected, uint32_t expected_len)
{
  ra8_fs_file_t* file = nullptr;
  ra8_err_t      err  = ra8_io_vfs_open(path, k_ra8_fs_mode_read, &file);
  if (err != k_ra8_ok) {
    return err;
  }
  uint8_t  buf[k_compare_chunk_len] = {};
  uint32_t offset                   = 0U;
  while (offset < expected_len) {
    uint32_t want = expected_len - offset;
    if (want > sizeof(buf)) {
      want = sizeof(buf);
    }
    uint32_t got = 0U;
    err          = ra8_fs_read(file, buf, want, &got);
    if (err != k_ra8_ok) {
      break;
    }
    if (got != want) {
      err = k_ra8_err_invalid_size;
      break;
    }
    if (memcmp(buf, &expected[offset], want) != 0) {
      err = k_ra8_err_validation_failed;
      break;
    }
    offset += got;
  }
  const ra8_err_t close_err = ra8_fs_close(file);
  return (err == k_ra8_ok) ? close_err : err;
}

/** @brief Validator that reads the closed private object through the real VFS. */
static ra8_err_t validate_artifact(void*         ctx,
                                   const char*   staging_path,
                                   uint64_t      total_bytes,
                                   const uint8_t sha256[k_ra8_mdl_sha256_bytes])
{
  validator_t* const validator = ctx;
  validator->calls++;
  if (validator->fail) {
    return k_ra8_err_validation_failed;
  }
  if (total_bytes != validator->expected_len) {
    return k_ra8_err_invalid_size;
  }
  if (sha256[0] != k_digest_tag) {
    return k_ra8_err_checksum_mismatch;
  }
  return compare_file(staging_path, validator->expected, validator->expected_len);
}

/** @brief Produce a deterministic nontrivial payload. */
static void fill_payload(uint8_t payload[k_payload_len])
{
  for (uint32_t i = 0U; i < k_payload_len; ++i) {
    payload[i] = (uint8_t)(((i * k_payload_multiplier) + k_payload_offset) & k_byte_mask);
  }
}

/** @brief Stat helper asserting path absence. */
static void assert_absent(const char* path)
{
  ra8_io_vfs_stat_t stat = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_stat(path, &stat));
  TEST_ASSERT(!stat.exists);
}

/** @brief Initialisation rejects bad staging ownership without modifying VFS. */
static void test_init_guards(void)
{
  TEST_BEGIN("media VFS storage init guards");
  ra8_mdl_storage_vfs_t        storage = {};
  ra8_mdl_storage_iface_t      iface   = {};
  ra8_mdl_storage_vfs_config_t config  = {.stage_leaf = "MDLSTAGE.TMP"};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_mdl_storage_vfs_init(nullptr, &config, &iface));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_mdl_storage_vfs_init(&storage, nullptr, &iface));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_mdl_storage_vfs_init(&storage, &config, nullptr));
  config.stage_leaf = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_mdl_storage_vfs_init(&storage, &config, &iface));
  config.stage_leaf = ".";
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_mdl_storage_vfs_init(&storage, &config, &iface));
  config.stage_leaf = "../BAD";
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_mdl_storage_vfs_init(&storage, &config, &iface));
  char unterminated[k_ra8_mdl_storage_vfs_stage_leaf_capacity];
  (void)memset(unterminated, 'A', sizeof(unterminated));
  config.stage_leaf = unterminated;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_mdl_storage_vfs_init(&storage, &config, &iface));
  TEST_END("media VFS storage init guards");
}

/** @brief Real multi-chunk transaction is byte-identical and idempotent. */
static void test_multichunk_commit(void)
{
  TEST_BEGIN("media VFS storage real multi-chunk commit");
  setup_volume();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_mkdir("ram:/BOOKS"));
  uint8_t payload[k_payload_len];
  fill_payload(payload);
  validator_t             validator = {.expected = payload, .expected_len = k_payload_len};
  ra8_mdl_storage_vfs_t   storage   = {};
  ra8_mdl_storage_iface_t iface     = {};
  bind_adapter(&storage, &iface, &validator);
  TEST_ASSERT_EQ(k_ra8_ok, iface.begin(iface.ctx, "ram:/BOOKS/BOOK.RBK"));
  uint16_t written = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, iface.write(iface.ctx, payload, 73U, &written));
  TEST_ASSERT_EQ(73U, written);
  TEST_ASSERT_EQ(k_ra8_ok,
                 iface.write(iface.ctx, &payload[73], (uint16_t)(k_payload_len - 73U), &written));
  TEST_ASSERT_EQ(k_payload_len - 73U, written);
  uint8_t digest[k_ra8_mdl_sha256_bytes] = {[0] = k_digest_tag};
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, iface.validate(iface.ctx, k_payload_len - 1U, digest));
  TEST_ASSERT_EQ(k_ra8_ok, iface.validate(iface.ctx, k_payload_len, digest));
  TEST_ASSERT_EQ(1U, validator.calls);
  TEST_ASSERT_EQ(k_ra8_ok, iface.commit(iface.ctx));
  TEST_ASSERT_EQ(k_ra8_ok, iface.commit(iface.ctx));
  TEST_ASSERT_EQ(k_ra8_ok, iface.abort(iface.ctx));
  TEST_ASSERT_EQ(k_ra8_ok, compare_file("ram:/BOOKS/BOOK.RBK", payload, k_payload_len));
  assert_absent("ram:/BOOKS/MDLSTAGE.TMP");
  TEST_END("media VFS storage real multi-chunk commit");
}

/** @brief Failed validation publishes nothing and repeated abort is harmless. */
static void test_validation_failure_abort(void)
{
  TEST_BEGIN("media VFS storage validation failure and abort");
  setup_volume();
  const uint8_t payload[] = {1U, 2U, 3U, 4U};
  validator_t   validator = {.expected = payload, .expected_len = sizeof(payload), .fail = true};
  ra8_mdl_storage_vfs_t   storage = {};
  ra8_mdl_storage_iface_t iface   = {};
  bind_adapter(&storage, &iface, &validator);
  TEST_ASSERT_EQ(k_ra8_ok, iface.begin(iface.ctx, "ram:/BAD.RBK"));
  uint16_t written = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, iface.write(iface.ctx, payload, sizeof(payload), &written));
  uint8_t digest[k_ra8_mdl_sha256_bytes] = {[0] = k_digest_tag};
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, iface.validate(iface.ctx, sizeof(payload), digest));
  assert_absent("ram:/BAD.RBK");
  TEST_ASSERT_EQ(k_ra8_ok, iface.abort(iface.ctx));
  TEST_ASSERT_EQ(k_ra8_ok, iface.abort(iface.ctx));
  assert_absent("ram:/BAD.RBK");
  assert_absent("ram:/MDLSTAGE.TMP");
  TEST_END("media VFS storage validation failure and abort");
}

/** @brief Begin recovers its owned stale file but never removes a directory. */
static void test_stale_stage_recovery(void)
{
  TEST_BEGIN("media VFS storage owned stale stage recovery");
  setup_volume();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_mkdir("ram:/BOOKS"));
  ra8_mdl_storage_vfs_t   old_storage = {};
  ra8_mdl_storage_iface_t old_iface   = {};
  bind_adapter(&old_storage, &old_iface, nullptr);
  TEST_ASSERT_EQ(k_ra8_ok, old_iface.begin(old_iface.ctx, "ram:/BOOKS/OLD.RBK"));
  const uint8_t stale[] = {9U, 8U, 7U};
  uint16_t      written = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, old_iface.write(old_iface.ctx, stale, sizeof(stale), &written));
  uint8_t digest[k_ra8_mdl_sha256_bytes] = {};
  TEST_ASSERT_EQ(k_ra8_ok, old_iface.validate(old_iface.ctx, sizeof(stale), digest));

  ra8_mdl_storage_vfs_t   new_storage = {};
  ra8_mdl_storage_iface_t new_iface   = {};
  bind_adapter(&new_storage, &new_iface, nullptr);
  TEST_ASSERT_EQ(k_ra8_ok, new_iface.begin(new_iface.ctx, "ram:/BOOKS/NEW.RBK"));
  ra8_io_vfs_stat_t stat = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_stat(new_storage.staging_path, &stat));
  TEST_ASSERT(stat.exists);
  TEST_ASSERT_EQ(0U, stat.size_bytes);
  TEST_ASSERT_EQ(k_ra8_ok, new_iface.abort(new_iface.ctx));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_mkdir("ram:/BOOKS/MDLSTAGE.TMP"));
  ra8_mdl_storage_vfs_t   dir_storage = {};
  ra8_mdl_storage_iface_t dir_iface   = {};
  bind_adapter(&dir_storage, &dir_iface, nullptr);
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, dir_iface.begin(dir_iface.ctx, "ram:/BOOKS/DIR.RBK"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_stat("ram:/BOOKS/MDLSTAGE.TMP", &stat));
  TEST_ASSERT(stat.exists);
  TEST_ASSERT(stat.is_directory);
  TEST_END("media VFS storage owned stale stage recovery");
}

/** @brief Canonical path checks reject files, dirs, traversal, and bad mounts. */
static void test_path_and_existing_final_guards(void)
{
  TEST_BEGIN("media VFS storage path and existing-final guards");
  setup_volume();
  const uint8_t original[] = {0x31U, 0x32U, 0x33U};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(s_mount, "/KEEP.RBK", original, sizeof(original)));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_mkdir("ram:/DIR.RBK"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(s_mount, "/PARENT", original, sizeof(original)));
  ra8_mdl_storage_vfs_t   storage = {};
  ra8_mdl_storage_iface_t iface   = {};
  bind_adapter(&storage, &iface, nullptr);
  TEST_ASSERT_EQ(k_ra8_err_exists, iface.begin(iface.ctx, "ram:/KEEP.RBK"));
  TEST_ASSERT_EQ(k_ra8_ok, compare_file("ram:/KEEP.RBK", original, sizeof(original)));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, iface.begin(iface.ctx, "ram:/DIR.RBK"));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, iface.begin(iface.ctx, "ram:/PARENT/X.RBK"));
  TEST_ASSERT_EQ(k_ra8_err_not_found, iface.begin(iface.ctx, "ram:/MISSING/X.RBK"));
  TEST_ASSERT_EQ(k_ra8_err_not_found, iface.begin(iface.ctx, "sd:/X.RBK"));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, iface.begin(iface.ctx, "ram:/A/../X.RBK"));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, iface.begin(iface.ctx, "ram:/A//X.RBK"));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, iface.begin(iface.ctx, "ram:/"));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, iface.begin(iface.ctx, "ram:X.RBK"));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, iface.begin(iface.ctx, "ram:/A\\X.RBK"));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, iface.begin(iface.ctx, "ra\\m:/X.RBK"));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, iface.begin(iface.ctx, "ram:/MDLSTAGE.TMP"));
  char too_long[k_ra8_mdl_storage_vfs_path_capacity + 1U];
  (void)memset(too_long, 'A', sizeof(too_long));
  too_long[0]                                   = 'r';
  too_long[1]                                   = 'a';
  too_long[2]                                   = 'm';
  too_long[3]                                   = ':';
  too_long[4]                                   = '/';
  too_long[k_ra8_mdl_storage_vfs_path_capacity] = '\0';
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, iface.begin(iface.ctx, too_long));
  TEST_END("media VFS storage path and existing-final guards");
}

/** @brief Capacity/removal faults publish nothing and cleanup remains retryable. */
static void test_write_and_removal_faults(void)
{
  TEST_BEGIN("media VFS storage capacity and removal faults");
  setup_volume();
  const uint8_t           payload[] = {0xA1U, 0xB2U, 0xC3U, 0xD4U};
  ra8_mdl_storage_vfs_t   storage   = {};
  ra8_mdl_storage_iface_t iface     = {};
  bind_adapter(&storage, &iface, nullptr);

  s_fault.removed = true;
  TEST_ASSERT_EQ(k_ra8_err_hw_error, iface.begin(iface.ctx, "ram:/REMOVED.RBK"));
  s_fault.removed = false;
  TEST_ASSERT_EQ(k_ra8_ok, iface.begin(iface.ctx, "ram:/FULL.RBK"));
  s_fault.write_error = k_ra8_err_no_mem;
  uint16_t written    = k_written_sentinel;
  TEST_ASSERT_EQ(k_ra8_err_no_mem, iface.write(iface.ctx, payload, sizeof(payload), &written));
  TEST_ASSERT_EQ(0U, written);
  s_fault.write_error = k_ra8_ok;
  TEST_ASSERT_EQ(k_ra8_ok, iface.abort(iface.ctx));
  assert_absent("ram:/FULL.RBK");

  TEST_ASSERT_EQ(k_ra8_ok, iface.begin(iface.ctx, "ram:/RETRY.RBK"));
  TEST_ASSERT_EQ(k_ra8_ok, iface.write(iface.ctx, payload, sizeof(payload), &written));
  s_fault.removed = true;
  TEST_ASSERT_EQ(k_ra8_err_hw_error, iface.abort(iface.ctx));
  s_fault.removed = false;
  TEST_ASSERT_EQ(k_ra8_ok, iface.abort(iface.ctx));
  TEST_ASSERT_EQ(k_ra8_ok, iface.abort(iface.ctx));
  assert_absent("ram:/RETRY.RBK");
  TEST_END("media VFS storage capacity and removal faults");
}

/** @brief Rename faults and destination races preserve no-replace semantics. */
static void test_rename_fault_and_no_replace(void)
{
  TEST_BEGIN("media VFS storage rename fault and no-replace");
  setup_volume();
  const uint8_t           payload[]                      = {0xA1U, 0xB2U, 0xC3U, 0xD4U};
  uint8_t                 digest[k_ra8_mdl_sha256_bytes] = {};
  ra8_mdl_storage_vfs_t   storage                        = {};
  ra8_mdl_storage_iface_t iface                          = {};
  bind_adapter(&storage, &iface, nullptr);
  uint16_t written = 0U;

  TEST_ASSERT_EQ(k_ra8_ok, iface.begin(iface.ctx, "ram:/RENAME.RBK"));
  TEST_ASSERT_EQ(k_ra8_ok, iface.write(iface.ctx, payload, sizeof(payload), &written));
  TEST_ASSERT_EQ(k_ra8_ok, iface.validate(iface.ctx, sizeof(payload), digest));
  s_fault.write_error = k_ra8_err_hw_error;
  TEST_ASSERT_EQ(k_ra8_err_hw_error, iface.commit(iface.ctx));
  s_fault.write_error = k_ra8_ok;
  assert_absent("ram:/RENAME.RBK");
  TEST_ASSERT_EQ(k_ra8_ok, iface.commit(iface.ctx));
  TEST_ASSERT_EQ(k_ra8_ok, compare_file("ram:/RENAME.RBK", payload, sizeof(payload)));

  TEST_ASSERT_EQ(k_ra8_ok, iface.begin(iface.ctx, "ram:/RACE.RBK"));
  TEST_ASSERT_EQ(k_ra8_ok, iface.write(iface.ctx, payload, sizeof(payload), &written));
  TEST_ASSERT_EQ(k_ra8_ok, iface.validate(iface.ctx, sizeof(payload), digest));
  const uint8_t winner[] = {0x55U, 0x66U};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(s_mount, "/RACE.RBK", winner, sizeof(winner)));
  TEST_ASSERT_EQ(k_ra8_err_exists, iface.commit(iface.ctx));
  TEST_ASSERT_EQ(k_ra8_ok, compare_file("ram:/RACE.RBK", winner, sizeof(winner)));
  TEST_ASSERT_EQ(k_ra8_ok, iface.abort(iface.ctx));
  TEST_ASSERT_EQ(k_ra8_ok, compare_file("ram:/RACE.RBK", winner, sizeof(winner)));
  TEST_END("media VFS storage rename fault and no-replace");
}

int32_t main(void)
{
  ra8_log_set_byte_sink(discard_log_byte, nullptr);
  test_init_guards();
  test_multichunk_commit();
  test_validation_failure_abort();
  test_stale_stage_recovery();
  test_path_and_existing_final_guards();
  test_write_and_removal_faults();
  test_rename_fault_and_no_replace();
  (void)fprintf(stderr, "[OK  ] test_ra8_mdl_storage_vfs.c\n");
  return 0;
}
