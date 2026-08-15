#define _GNU_SOURCE

/**
 * @file test_fw_if_fs.c
 * @brief Shared conformance vectors for POSIX and RA8 VFS filesystem ports.
 *
 * @details One backend-neutral vector function is run against a confined POSIX
 * directory and a real FAT12 volume over the caller-owned RAM block-device,
 * `ra8_fs`, and named VFS stack. Adapter-specific setup is outside the vector;
 * behavioral assertions are selected only through advertised capabilities.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fw_if_fs.h"
#include "fw_if_fs_backend.h"
#include "fw_if_fs_posix.h"
#include "fw_if_fs_ra8_vfs.h"
#include "ra8_err.h"
#include "ra8_fs.h"
#include "ra8_io_blockdev.h"
#include "ra8_io_blockdev_ram.h"
#include "ra8_io_vfs.h"
#include "unity_minimal.h"

/** @brief Fixed test bounds and the FAT12 RAM-disk size. */
typedef enum : uint32_t {
  k_test_disk_blocks  = 1024U,
  k_test_file_work    = 64U,
  k_test_txn_work     = 2048U,
  k_test_fill_chunk   = 4096U,
  k_test_fill_attempt = 256U,
} test_limits_t;

/** @brief Maximally aligned caller-owned backend workspace. */
typedef union {
  max_align_t alignment;
  uint8_t     bytes[k_test_txn_work];
} test_workspace_t;

/** @brief Validator cookie for exact staged content. */
typedef struct {
  const uint8_t* bytes;
  uint32_t       length;
  bool           reject;
} validator_ctx_t;

/** @brief List callback tally. */
typedef struct {
  uint32_t count;
  bool     saw_directory;
} list_ctx_t;

/** @brief FAT12 storage and caller-owned adapter objects. */
static uint8_t s_disk[(size_t)k_test_disk_blocks * (size_t)k_ra8_io_block_size_bytes];
static ra8_io_blockdev_ram_state_t s_ram_state;
static ra8_io_blockdev_t           s_blockdev;
static ra8_fs_backend_t            s_backend;
static ra8_fs_mount_t*             s_mount;

/** @brief Return incoherent success metadata for facade fault injection. */
static ra8_err_t internal_contract_stat(void* ctx, const char* path, fw_fs_stat_t* out)
{
  (void)ctx;
  (void)path;
  out->exists     = false;
  out->type       = k_fw_fs_node_file;
  out->size_bytes = 1U;
  return k_ra8_ok;
}

/** @brief Report more directory entries than the caller permitted. */
static ra8_err_t internal_contract_list(void*           ctx,
                                        const char*     path,
                                        uint32_t        max_entries,
                                        fw_fs_list_fn_t callback,
                                        void*           callback_ctx,
                                        uint32_t*       out_count,
                                        bool*           out_complete)
{
  (void)ctx;
  (void)path;
  (void)callback;
  (void)callback_ctx;
  *out_count    = max_entries + 1U;
  *out_complete = true;
  return k_ra8_ok;
}

/** @brief Valid callback shape for the bounded-list fault. */
static ra8_err_t internal_contract_entry(void* ctx, const fw_fs_dirent_t* entry, bool* out_continue)
{
  (void)ctx;
  (void)entry;
  *out_continue = true;
  return k_ra8_ok;
}

/** @brief Return impossible volume accounting for facade fault injection. */
static ra8_err_t internal_contract_space(void* ctx, fw_fs_space_t* out)
{
  (void)ctx;
  out->total_bytes = 10U;
  out->free_bytes  = 11U;
  out->used_bytes  = 0U;
  return k_ra8_ok;
}

/** @brief Report one byte beyond the supplied read capacity. */
static ra8_err_t
internal_contract_read(void* ctx, void* file_state, uint8_t* dst, uint32_t cap, uint32_t* out_read)
{
  (void)ctx;
  (void)file_state;
  (void)dst;
  *out_read = cap + 1U;
  return k_ra8_ok;
}

/** @brief Report one byte beyond the supplied write length. */
static ra8_err_t internal_contract_write(void*          ctx,
                                         void*          state,
                                         const uint8_t* source,
                                         uint32_t       length,
                                         uint32_t*      out_written)
{
  (void)ctx;
  (void)state;
  (void)source;
  *out_written = length + 1U;
  return k_ra8_ok;
}

/** @brief Fail without touching a 64-bit output. */
static ra8_err_t internal_contract_u64_error(void* ctx, void* state, uint64_t* out)
{
  (void)ctx;
  (void)state;
  (void)out;
  return k_ra8_fail;
}

/** @brief Claim commit success without publishing anything. */
static ra8_err_t internal_contract_commit(void* ctx, void* state, bool* out_published)
{
  (void)ctx;
  (void)state;
  *out_published = false;
  return k_ra8_ok;
}

/** @brief Accept cleanup of the synthetic transaction state. */
static ra8_err_t internal_contract_abort(void* ctx, void* state)
{
  (void)ctx;
  (void)state;
  return k_ra8_ok;
}

/**
 * @test check_backend_contract_guards
 * @brief Prove optional binding and reject impossible backend outputs.
 * @details Mutates copies of a real backend's vtables so every facade guard is
 *          exercised without changing or invoking the real adapter state.
 * @param[in] fs Fully bound conformance filesystem used as a truthful baseline.
 * @pre @p fs and all three baseline vtables are valid.
 * @post Optional absent operations bind honestly; contradictory capabilities,
 *       over-bound counts, incoherent metadata/space, untouched scalar errors,
 *       and success-without-publication are all contained by the facade.
 * @note Host-only fault injection; no real filesystem operation is attempted.
 * @since 0.1.0
 */
static void check_backend_contract_guards(const fw_fs_t* fs)
{
  fw_fs_namespace_iface_t names         = *fs->names.iface;
  fw_fs_stream_iface_t    streams       = *fs->streams.iface;
  fw_fs_caps_t            optional_caps = fs->caps;
  optional_caps.flags &=
    ~((uint32_t)k_fw_fs_cap_file_sync | (uint32_t)k_fw_fs_cap_durable_file_sync |
      (uint32_t)k_fw_fs_cap_transactions);
  streams.sync  = nullptr;
  fw_fs_t bound = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 fw_fs_bind(&bound, &names, &streams, nullptr, (void*)fs, &optional_caps));
  fw_fs_file_t file = {.iface = &streams, .ctx = (void*)fs, .state = &bound, .is_open = true};
  TEST_ASSERT_EQ(k_ra8_err_not_supported, fw_fs_sync(&file));
  uint8_t             transaction_work = 0U;
  fw_fs_transaction_t transaction      = {};
  TEST_ASSERT_EQ(k_ra8_err_not_supported,
                 fw_fs_transaction_begin(&bound.transactions,
                                         "/artifact",
                                         k_fw_fs_txn_create_new,
                                         &transaction,
                                         &transaction_work,
                                         sizeof(transaction_work)));

  fw_fs_caps_t dishonest_caps = optional_caps;
  dishonest_caps.flags |= (uint32_t)k_fw_fs_cap_file_sync;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 fw_fs_bind(&bound, &names, &streams, nullptr, (void*)fs, &dishonest_caps));
  dishonest_caps.flags &= ~(uint32_t)k_fw_fs_cap_file_sync;
  dishonest_caps.flags |= (uint32_t)k_fw_fs_cap_durable_file_sync;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 fw_fs_bind(&bound, &names, &streams, nullptr, (void*)fs, &dishonest_caps));

  names.stat                       = internal_contract_stat;
  names.listdir                    = internal_contract_list;
  names.space                      = internal_contract_space;
  fw_fs_namespace_t namespace_port = {.iface = &names, .ctx = (void*)fs, .caps = fs->caps};
  fw_fs_stat_t      stat           = {};
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, fw_fs_stat(&namespace_port, "/bad", &stat));
  TEST_ASSERT(!stat.exists && (stat.type == k_fw_fs_node_none) && (stat.size_bytes == 0U));
  uint32_t count    = UINT32_MAX;
  bool     complete = true;
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_state,
    fw_fs_listdir(&namespace_port, "/", 1U, internal_contract_entry, nullptr, &count, &complete));
  TEST_ASSERT_EQ(0U, count);
  TEST_ASSERT(!complete);
  fw_fs_space_t space = {};
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, fw_fs_space(&namespace_port, &space));
  TEST_ASSERT((space.total_bytes == 0U) && (space.free_bytes == 0U) && (space.used_bytes == 0U));

  streams.read         = internal_contract_read;
  streams.write        = internal_contract_write;
  streams.tell         = internal_contract_u64_error;
  streams.size         = internal_contract_u64_error;
  uint8_t  byte        = 0U;
  uint32_t transferred = UINT32_MAX;
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, fw_fs_read(&file, &byte, 1U, &transferred));
  TEST_ASSERT_EQ(0U, transferred);
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, fw_fs_write(&file, &byte, 1U, &transferred));
  TEST_ASSERT_EQ(0U, transferred);
  uint64_t scalar = UINT64_MAX;
  TEST_ASSERT_EQ(k_ra8_fail, fw_fs_tell(&file, &scalar));
  TEST_ASSERT_EQ(0U, scalar);
  scalar = UINT64_MAX;
  TEST_ASSERT_EQ(k_ra8_fail, fw_fs_file_size(&file, &scalar));
  TEST_ASSERT_EQ(0U, scalar);

  fw_fs_transaction_iface_t transactions = *fs->transactions.iface;
  transactions.write                     = internal_contract_write;
  transactions.commit                    = internal_contract_commit;
  transactions.abort                     = internal_contract_abort;
  transaction                            = (fw_fs_transaction_t){.iface     = &transactions,
                                                                 .ctx       = (void*)fs,
                                                                 .state     = &bound,
                                                                 .active    = true,
                                                                 .validated = false};
  TEST_ASSERT_EQ(k_ra8_err_invalid_state,
                 fw_fs_transaction_write(&transaction, &byte, 1U, &transferred));
  TEST_ASSERT_EQ(0U, transferred);
  transaction.validated = true;
  bool published        = true;
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, fw_fs_transaction_commit(&transaction, &published));
  TEST_ASSERT(!published);
  TEST_ASSERT(transaction.active && transaction.validated);
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_transaction_abort(&transaction));
  TEST_ASSERT(!transaction.active && !transaction.validated);
}

/** @brief Read and compare the complete contents of one portable file. */
static void
expect_file(const fw_fs_t* fs, const char* path, const uint8_t* expected, uint32_t length)
{
  test_workspace_t file_work = {};
  fw_fs_file_t     file      = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 fw_fs_open(&fs->streams,
                            path,
                            k_fw_fs_open_read,
                            &file,
                            file_work.bytes,
                            sizeof(file_work.bytes)));
  uint8_t  actual[64] = {};
  uint32_t got        = 0U;
  TEST_ASSERT(length <= sizeof(actual));
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_read(&file, actual, sizeof(actual), &got));
  TEST_ASSERT_EQ(length, got);
  TEST_ASSERT_EQ(0, memcmp(actual, expected, length));
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_read(&file, actual, sizeof(actual), &got));
  TEST_ASSERT_EQ(0U, got);
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_close(&file));
}

/** @brief Exact transaction validator, optionally forced to reject. */
static ra8_err_t validate_exact(void* ctx, fw_fs_file_t* staged)
{
  const validator_ctx_t* expected = (const validator_ctx_t*)ctx;
  if (expected->reject) {
    return k_ra8_err_protocol_error;
  }
  uint8_t  actual[64] = {};
  uint32_t got        = 0U;
  if (expected->length > sizeof(actual)) {
    return k_ra8_err_invalid_size;
  }
  const ra8_err_t read = fw_fs_read(staged, actual, sizeof(actual), &got);
  if (read != k_ra8_ok) {
    return read;
  }
  if (got != expected->length) {
    return k_ra8_err_protocol_error;
  }
  return (memcmp(actual, expected->bytes, got) == 0) ? k_ra8_ok : k_ra8_err_protocol_error;
}

/** @brief Count directory entries and observe at least one directory. */
static ra8_err_t count_entry(void* ctx, const fw_fs_dirent_t* entry, bool* out_continue)
{
  list_ctx_t* list = (list_ctx_t*)ctx;
  ++list->count;
  if (entry->type == k_fw_fs_node_directory) {
    list->saw_directory = true;
  }
  *out_continue = true;
  return k_ra8_ok;
}

/** @brief Create/truncate and write one small file. */
static void write_file(const fw_fs_t* fs, const char* path, const uint8_t* bytes, uint32_t length)
{
  test_workspace_t file_work = {};
  fw_fs_file_t     file      = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 fw_fs_open(&fs->streams,
                            path,
                            k_fw_fs_open_write_truncate,
                            &file,
                            file_work.bytes,
                            sizeof(file_work.bytes)));
  uint32_t written = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_write(&file, bytes, length, &written));
  TEST_ASSERT_EQ(length, written);
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_close(&file));
}

/** @brief Exercise path grammar before any backend sees the argument. */
static void check_path_policy(const fw_fs_t* fs)
{
  fw_fs_stat_t stat = {};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, fw_fs_stat(&fs->names, "relative", &stat));
  TEST_ASSERT_EQ(k_ra8_err_access_denied, fw_fs_stat(&fs->names, "/../escape", &stat));
  TEST_ASSERT_EQ(k_ra8_err_access_denied, fw_fs_stat(&fs->names, "/a/./b", &stat));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, fw_fs_stat(&fs->names, "/a//b", &stat));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, fw_fs_stat(&fs->names, "/a/", &stat));
  TEST_ASSERT_EQ(k_ra8_err_access_denied, fw_fs_stat(&fs->names, "/sd:/x", &stat));
  TEST_ASSERT_EQ(k_ra8_err_access_denied, fw_fs_stat(&fs->names, "/a\\b", &stat));
}

/** @brief Check one advertised timestamp field and its civil invariants. */
static void check_timestamp(uint32_t flags, uint32_t capability, const fw_fs_timestamp_t* stamp)
{
  if ((flags & capability) == 0U) {
    TEST_ASSERT(!stamp->valid);
    TEST_ASSERT(!stamp->utc_offset_valid);
    return;
  }
  TEST_ASSERT(stamp->valid);
  TEST_ASSERT(stamp->value.month >= 1U && stamp->value.month <= 12U);
  TEST_ASSERT(stamp->value.day >= 1U && stamp->value.day <= 31U);
  TEST_ASSERT(stamp->value.hour <= 23U);
  TEST_ASSERT(stamp->value.minute <= 59U);
  TEST_ASSERT(stamp->value.second <= 59U);
  TEST_ASSERT(stamp->value.nanosecond <= 999999999UL);
  TEST_ASSERT(!stamp->utc_offset_valid || stamp->valid);
}

/** @brief Exercise missing/root metadata and one-level directory creation. */
static void check_namespace_setup(const fw_fs_t* fs)
{
  fw_fs_stat_t stat = {};
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_stat(&fs->names, "/", &stat));
  TEST_ASSERT(stat.exists && stat.type == k_fw_fs_node_directory);
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_stat(&fs->names, "/missing", &stat));
  TEST_ASSERT(!stat.exists && stat.type == k_fw_fs_node_none);
  TEST_ASSERT(!stat.created.valid && !stat.modified.valid && !stat.accessed.valid);
  TEST_ASSERT_EQ(k_ra8_err_not_found, fw_fs_mkdir(&fs->names, "/absent/child"));
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_mkdir(&fs->names, "/books"));
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_mkdir(&fs->names, "/books/sub"));
  TEST_ASSERT_EQ(k_ra8_err_exists, fw_fs_mkdir(&fs->names, "/books"));
}

/** @brief Exercise append, offsets, size, sync, readback, stat, and timestamps.
 */
static void check_stream_roundtrip(const fw_fs_t* fs)
{
  static const uint8_t first[] = {1U, 2U, 3U, 4U};
  static const uint8_t tail[]  = {5U, 6U};
  static const uint8_t whole[] = {1U, 2U, 3U, 4U, 5U, 6U};
  write_file(fs, "/books/a.bin", first, sizeof(first));
  test_workspace_t file_work = {};
  fw_fs_file_t     file      = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 fw_fs_open(&fs->streams,
                            "/books/a.bin",
                            k_fw_fs_open_append,
                            &file,
                            file_work.bytes,
                            sizeof(file_work.bytes)));
  uint32_t written = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_write(&file, tail, sizeof(tail), &written));
  uint64_t offset = 0U;
  uint64_t size   = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_tell(&file, &offset));
  TEST_ASSERT_EQ(sizeof(whole), offset);
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_file_size(&file, &size));
  TEST_ASSERT_EQ(sizeof(whole), size);
  const ra8_err_t sync = fw_fs_sync(&file);
  if ((fs->caps.flags & (uint32_t)k_fw_fs_cap_file_sync) != 0U) {
    TEST_ASSERT_EQ(k_ra8_ok, sync);
  } else {
    TEST_ASSERT_EQ(k_ra8_err_not_supported, sync);
  }
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_close(&file));
  expect_file(fs, "/books/a.bin", whole, sizeof(whole));
  fw_fs_stat_t stat = {};
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_stat(&fs->names, "/books/a.bin", &stat));
  TEST_ASSERT(stat.exists && stat.type == k_fw_fs_node_file && stat.size_bytes == sizeof(whole));
  check_timestamp(fs->caps.flags, (uint32_t)k_fw_fs_cap_created_time, &stat.created);
  check_timestamp(fs->caps.flags, (uint32_t)k_fw_fs_cap_modified_time, &stat.modified);
  check_timestamp(fs->caps.flags, (uint32_t)k_fw_fs_cap_accessed_time, &stat.accessed);
}

/** @brief Exercise bounded listing, rename collision, unlink, and rmdir. */
static void check_namespace_cleanup(const fw_fs_t* fs)
{
  static const uint8_t first[]  = {1U, 2U, 3U, 4U};
  list_ctx_t           list     = {};
  uint32_t             count    = 0U;
  bool                 complete = true;
  TEST_ASSERT_EQ(k_ra8_ok,
                 fw_fs_listdir(&fs->names, "/books", 1U, count_entry, &list, &count, &complete));
  TEST_ASSERT_EQ(1U, count);
  TEST_ASSERT(!complete);
  list = (list_ctx_t){};
  TEST_ASSERT_EQ(k_ra8_ok,
                 fw_fs_listdir(&fs->names, "/books", 16U, count_entry, &list, &count, &complete));
  TEST_ASSERT(complete && count >= 1U && list.saw_directory);

  TEST_ASSERT_EQ(k_ra8_err_not_empty, fw_fs_rmdir(&fs->names, "/books"));
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_rename(&fs->names, "/books/a.bin", "/books/b.bin", false));
  write_file(fs, "/books/a.bin", first, sizeof(first));
  TEST_ASSERT_EQ(k_ra8_err_exists, fw_fs_rename(&fs->names, "/books/a.bin", "/books/b.bin", false));
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_unlink(&fs->names, "/books/a.bin"));
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_unlink(&fs->names, "/books/b.bin"));
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_rmdir(&fs->names, "/books/sub"));
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_rmdir(&fs->names, "/books"));
}

/** @brief Exercise file/dir metadata, one-level mkdir, bounded list, and
 * streams. */
static void check_namespace_and_streams(const fw_fs_t* fs)
{
  check_namespace_setup(fs);
  check_stream_roundtrip(fs);
  check_namespace_cleanup(fs);
}

/** @brief Exercise exclusive-create capability without assuming it exists. */
static void check_exclusive_create(const fw_fs_t* fs)
{
  test_workspace_t work   = {};
  fw_fs_file_t     file   = {};
  const ra8_err_t  opened = fw_fs_open(&fs->streams,
                                       "/exclusive.bin",
                                       k_fw_fs_open_create_new,
                                       &file,
                                       work.bytes,
                                       sizeof(work.bytes));
  if ((fs->caps.flags & (uint32_t)k_fw_fs_cap_create_exclusive) == 0U) {
    TEST_ASSERT_EQ(k_ra8_err_not_supported, opened);
    return;
  }
  TEST_ASSERT_EQ(k_ra8_ok, opened);
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_close(&file));
  TEST_ASSERT_EQ(k_ra8_err_exists,
                 fw_fs_open(&fs->streams,
                            "/exclusive.bin",
                            k_fw_fs_open_create_new,
                            &file,
                            work.bytes,
                            sizeof(work.bytes)));
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_unlink(&fs->names, "/exclusive.bin"));
}

/** @brief Prove no-replace requests are rejected before an unqualified backend.
 */
static void check_atomic_noreplace_gate(const fw_fs_t* fs)
{
  fw_fs_namespace_t names = fs->names;
  names.caps.flags &= ~(uint32_t)k_fw_fs_cap_atomic_noreplace;
  TEST_ASSERT_EQ(k_ra8_err_not_supported,
                 fw_fs_rename(&names, "/never-source", "/never-destination", false));

  fw_fs_transaction_port_t transactions = fs->transactions;
  transactions.caps.flags &= ~(uint32_t)k_fw_fs_cap_atomic_noreplace;
  test_workspace_t    work = {};
  fw_fs_transaction_t txn  = {};
  TEST_ASSERT_EQ(k_ra8_err_not_supported,
                 fw_fs_transaction_begin(&transactions,
                                         "/never-published",
                                         k_fw_fs_txn_create_new,
                                         &txn,
                                         work.bytes,
                                         sizeof(work.bytes)));
}

/** @brief Run one staged transaction through write/validate/commit. */
static void commit_transaction(const fw_fs_t*             fs,
                               const char*                destination,
                               fw_fs_transaction_policy_t policy,
                               const validator_ctx_t*     validator)
{
  test_workspace_t    work = {};
  fw_fs_transaction_t txn  = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 fw_fs_transaction_begin(&fs->transactions,
                                         destination,
                                         policy,
                                         &txn,
                                         work.bytes,
                                         sizeof(work.bytes)));
  uint32_t written = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 fw_fs_transaction_write(&txn, validator->bytes, validator->length, &written));
  TEST_ASSERT_EQ(validator->length, written);
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_transaction_validate(&txn, validate_exact, (void*)validator));
  bool published = false;
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_transaction_commit(&txn, &published));
  TEST_ASSERT(published);
}

/** @brief Prove staged writers can reserve then backfill without sparse seeks.
 */
static void check_transaction_backfill(const fw_fs_t* fs)
{
  static const uint8_t  reserved[] = {0U, 0U, 0U, 0U};
  static const uint8_t  offset[]   = {4U, 0U, 0U, 0U};
  static const uint8_t  payload[]  = {'D', 'A', 'T', 'A'};
  static const uint8_t  expected[] = {4U, 0U, 0U, 0U, 'D', 'A', 'T', 'A'};
  const validator_ctx_t validator  = {.bytes  = expected,
                                      .length = sizeof(expected),
                                      .reject = false};
  test_workspace_t      work       = {};
  fw_fs_transaction_t   txn        = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 fw_fs_transaction_begin(&fs->transactions,
                                         "/backfill.bin",
                                         k_fw_fs_txn_create_new,
                                         &txn,
                                         work.bytes,
                                         sizeof(work.bytes)));
  uint32_t written = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_transaction_write(&txn, reserved, sizeof(reserved), &written));
  TEST_ASSERT_EQ(sizeof(reserved), written);
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_transaction_write(&txn, payload, sizeof(payload), &written));
  TEST_ASSERT_EQ(sizeof(payload), written);
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, fw_fs_transaction_seek(&txn, UINT64_MAX));
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_transaction_seek(&txn, 0U));
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_transaction_write(&txn, offset, sizeof(offset), &written));
  TEST_ASSERT_EQ(sizeof(offset), written);
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_transaction_validate(&txn, validate_exact, (void*)&validator));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, fw_fs_transaction_seek(&txn, 0U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state,
                 fw_fs_transaction_write(&txn, payload, sizeof(payload), &written));
  bool published = false;
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_transaction_commit(&txn, &published));
  TEST_ASSERT(published);
  expect_file(fs, "/backfill.bin", expected, sizeof(expected));
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_unlink(&fs->names, "/backfill.bin"));
}

/** @brief Simulate a destination appearing after begin; it must win untouched.
 */
static void check_transaction_destination_race(const fw_fs_t* fs)
{
  static const uint8_t  staged[]     = {'s', 't', 'a', 'g', 'e'};
  static const uint8_t  competitor[] = {'w', 'i', 'n'};
  const validator_ctx_t validator    = {.bytes = staged, .length = sizeof(staged), .reject = false};
  test_workspace_t      work         = {};
  fw_fs_transaction_t   txn          = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 fw_fs_transaction_begin(&fs->transactions,
                                         "/race.bin",
                                         k_fw_fs_txn_create_new,
                                         &txn,
                                         work.bytes,
                                         sizeof(work.bytes)));
  uint32_t written = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_transaction_write(&txn, staged, sizeof(staged), &written));
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_transaction_validate(&txn, validate_exact, (void*)&validator));
  write_file(fs, "/race.bin", competitor, sizeof(competitor));
  bool published = true;
  TEST_ASSERT_EQ(k_ra8_err_exists, fw_fs_transaction_commit(&txn, &published));
  TEST_ASSERT(!published);
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_transaction_abort(&txn));
  expect_file(fs, "/race.bin", competitor, sizeof(competitor));
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_unlink(&fs->names, "/race.bin"));
}

/** @brief Exercise create-new publication, collision, and abort cleanup. */
static void check_transaction_abort_cycle(const fw_fs_t* fs)
{
  static const uint8_t  good[]         = {'g', 'o', 'o', 'd'};
  const validator_ctx_t good_validator = {.bytes = good, .length = sizeof(good), .reject = false};
  commit_transaction(fs, "/artifact.bin", k_fw_fs_txn_create_new, &good_validator);
  expect_file(fs, "/artifact.bin", good, sizeof(good));

  test_workspace_t    work = {};
  fw_fs_transaction_t txn  = {};
  TEST_ASSERT_EQ(k_ra8_err_exists,
                 fw_fs_transaction_begin(&fs->transactions,
                                         "/artifact.bin",
                                         k_fw_fs_txn_create_new,
                                         &txn,
                                         work.bytes,
                                         sizeof(work.bytes)));
  TEST_ASSERT_EQ(k_ra8_ok,
                 fw_fs_transaction_begin(&fs->transactions,
                                         "/abort.bin",
                                         k_fw_fs_txn_create_new,
                                         &txn,
                                         work.bytes,
                                         sizeof(work.bytes)));
  uint32_t written = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_transaction_write(&txn, good, sizeof(good), &written));
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_transaction_abort(&txn));
  fw_fs_stat_t stat = {};
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_stat(&fs->names, "/abort.bin", &stat));
  TEST_ASSERT(!stat.exists);
}

/** @brief Exercise replacement capability, validator rejection, and
 * preservation. */
static void check_transaction_replace(const fw_fs_t* fs)
{
  static const uint8_t old[]   = {'o', 'l', 'd'};
  static const uint8_t newer[] = {'n', 'e', 'w'};
  test_workspace_t     work    = {};
  fw_fs_transaction_t  txn     = {};
  uint32_t             written = 0U;
  write_file(fs, "/keep.bin", old, sizeof(old));
  if ((fs->caps.flags & (uint32_t)k_fw_fs_cap_atomic_replace) != 0U) {
    const validator_ctx_t reject = {.bytes = newer, .length = sizeof(newer), .reject = true};
    TEST_ASSERT_EQ(k_ra8_ok,
                   fw_fs_transaction_begin(&fs->transactions,
                                           "/keep.bin",
                                           k_fw_fs_txn_replace_atomic,
                                           &txn,
                                           work.bytes,
                                           sizeof(work.bytes)));
    TEST_ASSERT_EQ(k_ra8_ok, fw_fs_transaction_write(&txn, newer, sizeof(newer), &written));
    TEST_ASSERT_EQ(k_ra8_err_protocol_error,
                   fw_fs_transaction_validate(&txn, validate_exact, (void*)&reject));
    TEST_ASSERT_EQ(k_ra8_ok, fw_fs_transaction_abort(&txn));
    expect_file(fs, "/keep.bin", old, sizeof(old));
    const validator_ctx_t replacement = {.bytes = newer, .length = sizeof(newer), .reject = false};
    commit_transaction(fs, "/keep.bin", k_fw_fs_txn_replace_atomic, &replacement);
    expect_file(fs, "/keep.bin", newer, sizeof(newer));
  } else {
    TEST_ASSERT_EQ(k_ra8_err_not_supported,
                   fw_fs_transaction_begin(&fs->transactions,
                                           "/keep.bin",
                                           k_fw_fs_txn_replace_atomic,
                                           &txn,
                                           work.bytes,
                                           sizeof(work.bytes)));
    expect_file(fs, "/keep.bin", old, sizeof(old));
  }
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_unlink(&fs->names, "/keep.bin"));
}

/** @brief Exercise publication, abort, rejection, race, and backfill. */
static void check_transactions(const fw_fs_t* fs)
{
  check_transaction_abort_cycle(fs);
  check_transaction_replace(fs);
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_unlink(&fs->names, "/artifact.bin"));
  check_transaction_backfill(fs);
  check_transaction_destination_race(fs);
}

/** @brief Shared backend-neutral conformance suite. */
static void run_conformance(const char* label, const fw_fs_t* fs)
{
  TEST_BEGIN(label);
  fw_fs_caps_t caps = {};
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_get_caps(fs, &caps));
  TEST_ASSERT((caps.flags & (uint32_t)k_fw_fs_cap_namespace) != 0U);
  TEST_ASSERT((caps.flags & (uint32_t)k_fw_fs_cap_stream) != 0U);
  TEST_ASSERT((caps.flags & (uint32_t)k_fw_fs_cap_atomic_noreplace) != 0U);
  TEST_ASSERT(caps.file_workspace_bytes <= k_test_file_work);
  TEST_ASSERT(caps.transaction_workspace_bytes <= k_test_txn_work);
  TEST_ASSERT(caps.file_workspace_align != 0U);
  TEST_ASSERT((caps.file_workspace_align & (caps.file_workspace_align - 1U)) == 0U);
  TEST_ASSERT(caps.transaction_workspace_align != 0U);
  TEST_ASSERT((caps.transaction_workspace_align & (caps.transaction_workspace_align - 1U)) == 0U);
  fw_fs_space_t space = {};
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_space(&fs->names, &space));
  TEST_ASSERT(space.total_bytes > 0U && space.free_bytes <= space.total_bytes);
  check_path_policy(fs);
  check_namespace_and_streams(fs);
  check_exclusive_create(fs);
  check_atomic_noreplace_gate(fs);
  check_transactions(fs);
  TEST_END(label);
}

/** @brief Fill a staged VFS file and prove failure never publishes it. */
static void check_vfs_full_media(const fw_fs_t* fs)
{
  static const uint8_t chunk[k_test_fill_chunk] = {0xA5U};
  test_workspace_t     work                     = {};
  fw_fs_transaction_t  txn                      = {};
  TEST_BEGIN("fw_if_fs VFS full-media transaction");
  TEST_ASSERT_EQ(k_ra8_ok,
                 fw_fs_transaction_begin(&fs->transactions,
                                         "/full.bin",
                                         k_fw_fs_txn_create_new,
                                         &txn,
                                         work.bytes,
                                         sizeof(work.bytes)));
  ra8_err_t result   = k_ra8_ok;
  uint32_t  attempts = 0U;
  for (; attempts < (uint32_t)k_test_fill_attempt; ++attempts) {
    uint32_t written = 0U;
    result           = fw_fs_transaction_write(&txn, chunk, sizeof(chunk), &written);
    if (result != k_ra8_ok) {
      TEST_ASSERT_EQ(0U, written);
      break;
    }
    TEST_ASSERT_EQ(sizeof(chunk), written);
  }
  TEST_ASSERT(attempts < (uint32_t)k_test_fill_attempt);
  TEST_ASSERT(result != k_ra8_ok);
  (void)fw_fs_transaction_abort(&txn);
  fw_fs_stat_t stat = {};
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_stat(&fs->names, "/full.bin", &stat));
  TEST_ASSERT(!stat.exists);
  TEST_END("fw_if_fs VFS full-media transaction");
}

/** @brief POSIX-specific proof that final/intermediate symlinks are never
 * followed. */
static void check_posix_symlinks(const fw_fs_t* fs, const char* root)
{
  char link_path[256];
  char target_path[256];
  TEST_ASSERT((size_t)snprintf(link_path, sizeof(link_path), "%s/link", root) < sizeof(link_path));
  TEST_ASSERT((size_t)snprintf(target_path, sizeof(target_path), "%s/target", root) <
              sizeof(target_path));
  TEST_ASSERT_EQ(0, mkdir(target_path, 0700));
  TEST_ASSERT_EQ(0, symlink("target", link_path));
  fw_fs_stat_t stat = {};
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_stat(&fs->names, "/link", &stat));
  TEST_ASSERT(stat.exists && stat.type == k_fw_fs_node_symlink);
  TEST_ASSERT_EQ(k_ra8_err_access_denied, fw_fs_stat(&fs->names, "/link/file", &stat));
  test_workspace_t work = {};
  fw_fs_file_t     file = {};
  TEST_ASSERT_EQ(
    k_ra8_err_access_denied,
    fw_fs_open(&fs->streams, "/link", k_fw_fs_open_read, &file, work.bytes, sizeof(work.bytes)));
  TEST_ASSERT_EQ(0, unlink(link_path));
  TEST_ASSERT_EQ(0, rmdir(target_path));
}

/** @brief Run the shared suite against the secure POSIX root adapter. */
static void test_posix_conformance(void)
{
  char root[] = "/tmp/fw_fs_port_XXXXXX";
  TEST_ASSERT(mkdtemp(root) != nullptr);
  fw_fs_t                 fs    = {};
  fw_fs_posix_state_t     state = {.root_fd = -1};
  const fw_fs_posix_cfg_t cfg   = {.root_path = root, .removable_media = false};
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_posix_init(&fs, &state, &cfg));
  check_backend_contract_guards(&fs);
  run_conformance("fw_if_fs POSIX conformance", &fs);
  check_posix_symlinks(&fs, root);
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_posix_deinit(&state));
  TEST_ASSERT_EQ(0, rmdir(root));
}

/** @brief Set up and run the shared suite over RAM blockdev -> FAT -> VFS. */
static void test_vfs_conformance(void)
{
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_io_blockdev_ram_init(&s_blockdev, &s_ram_state, s_disk, k_test_disk_blocks, false));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_blockdev_as_fs_backend(&s_blockdev, &s_backend));
  const ra8_fs_format_opts_t format = {.type                = k_ra8_fs_type_fat12,
                                       .label               = "PORT",
                                       .sectors_per_cluster = 0U};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_format(&s_backend, &format));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &s_mount));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_init());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_mount("ram", s_mount));
  fw_fs_t                   fs    = {};
  fw_fs_ra8_vfs_state_t     state = {};
  const fw_fs_ra8_vfs_cfg_t cfg = {.mount_name = "ram", .mount = s_mount, .removable_media = false};
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_ra8_vfs_init(&fs, &state, &cfg));
  run_conformance("fw_if_fs RAM/FAT/VFS conformance", &fs);
  check_vfs_full_media(&fs);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_unmount("ram"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(s_mount));
  s_mount = nullptr;
}

int main(void)
{
  test_posix_conformance();
  test_vfs_conformance();
  (void)fprintf(stderr, "[OK  ] test_fw_if_fs.c\n");
  return 0;
}
