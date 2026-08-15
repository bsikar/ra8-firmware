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

/** @brief Request hosted POSIX extension declarations used by this test. */
#define _GNU_SOURCE

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
#include "mdl_hash.h"
#include "mdl_pathfs.h"
#include "mdl_storage.h"
#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_fs.h"
#include "ra8_io_blockdev.h"
#include "ra8_io_blockdev_ram.h"
#include "ra8_io_vfs.h"
#include "support/fw_if_fs_contract_test.h"
#include "support/fw_if_fs_cursor_test.h"
#include "unity_minimal.h"

/** @brief Fixed test bounds and the FAT12 RAM-disk size. */
typedef enum : uint32_t {
  k_test_disk_blocks  = 1024U, /**< FAT12 RAM-disk sectors.             */
  k_test_file_work    = 64U,   /**< Maximum file backend workspace.     */
  k_test_txn_work     = 2048U, /**< Maximum transaction workspace.      */
  k_test_fill_chunk   = 4096U, /**< Bytes written per full-media probe. */
  k_test_fill_attempt = 256U,  /**< Bounded full-media write attempts.  */
} test_limits_t;

/** @brief Maximally aligned caller-owned backend workspace. */
typedef union {
  max_align_t alignment;              /**< Force maximum C alignment. */
  uint8_t     bytes[k_test_txn_work]; /**< Opaque backend state.      */
} test_workspace_t;

/** @brief Validator cookie for exact staged content. */
typedef struct {
  const uint8_t* bytes;  /**< Expected stage bytes.      */
  uint32_t       length; /**< Expected stage byte count. */
  bool           reject; /**< Force validator rejection. */
} validator_ctx_t;

/** @brief List callback tally. */
typedef struct {
  uint32_t count;         /**< Delivered entry count.        */
  bool     saw_directory; /**< Whether a directory appeared. */
} list_ctx_t;

/** @brief FAT12 storage and caller-owned adapter objects. */
static uint8_t s_disk[(size_t)k_test_disk_blocks * (size_t)k_ra8_io_block_size_bytes];
static ra8_io_blockdev_ram_state_t s_ram_state;
static ra8_io_blockdev_t           s_blockdev;
static ra8_fs_backend_t            s_backend;
static ra8_fs_mount_t*             s_mount;

/** @brief Read and compare the complete contents of one portable file. @details Implements the bounded expect file fixture step using caller-owned state. @param[in] fs Caller-owned fixture or filesystem state. @param[in] path Validated fixture path or name value. @param[in] expected Value required by this filesystem vector. @param[in] length Caller-supplied bounded extent or quantity. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0 */
RA8_INTERNAL static void
internal_expect_file(const fw_fs_t* fs, const char* path, const uint8_t* expected, uint32_t length)
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

/** @brief Exact transaction validator, optionally forced to reject. @details Implements the bounded validate exact fixture step using caller-owned state. @param[in,out] ctx Caller-owned fixture or filesystem state. @param[in,out] staged Value required by this filesystem vector. @return Status, selected object, or bounded value produced by the named operation. @retval k_ra8_ok The requested operation completed. @retval k_ra8_err_* Validation or backend work failed. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0 */
RA8_INTERNAL static ra8_err_t internal_validate_exact(void* ctx, fw_fs_file_t* staged)
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

/** @brief Count directory entries and observe at least one directory. @details Implements the bounded count entry fixture step using caller-owned state. @param[in,out] ctx Caller-owned fixture or filesystem state. @param[in] entry Value required by this filesystem vector. @param[out] out_continue Caller-owned output populated on success. @return Status, selected object, or bounded value produced by the named operation. @retval k_ra8_ok The requested operation completed. @retval k_ra8_err_* Validation or backend work failed. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0 */
RA8_INTERNAL static ra8_err_t
internal_count_entry(void* ctx, const fw_fs_dirent_t* entry, bool* out_continue)
{
  list_ctx_t* list = (list_ctx_t*)ctx;
  ++list->count;
  if (entry->type == k_fw_fs_node_directory) {
    list->saw_directory = true;
  }
  *out_continue = true;
  return k_ra8_ok;
}

/** @brief Create/truncate and write one small file. @details Implements the bounded write file fixture step using caller-owned state. @param[in] fs Caller-owned fixture or filesystem state. @param[in] path Validated fixture path or name value. @param[in] bytes Caller-supplied bounded extent or quantity. @param[in] length Caller-supplied bounded extent or quantity. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0 */
RA8_INTERNAL static void
internal_write_file(const fw_fs_t* fs, const char* path, const uint8_t* bytes, uint32_t length)
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

/** @brief Exercise path grammar before any backend sees the argument. @details Implements the bounded check path policy fixture step using caller-owned state. @param[in] fs Caller-owned fixture or filesystem state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0 */
RA8_INTERNAL static void internal_check_path_policy(const fw_fs_t* fs)
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

/** @brief Check one advertised timestamp field and its civil invariants. @details Implements the bounded check timestamp fixture step using caller-owned state. @param[in] flags Value required by this filesystem vector. @param[in] capability Value required by this filesystem vector. @param[in] stamp Value required by this filesystem vector. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0 */
RA8_INTERNAL static void
internal_check_timestamp(uint32_t flags, uint32_t capability, const fw_fs_timestamp_t* stamp)
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

/** @brief Exercise missing/root metadata and one-level directory creation. @details Implements the bounded check namespace setup fixture step using caller-owned state. @param[in] fs Caller-owned fixture or filesystem state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0 */
RA8_INTERNAL static void internal_check_namespace_setup(const fw_fs_t* fs)
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

/** @brief Exercise append, offsets, size, sync, readback, stat, and timestamps. @details Implements the bounded check stream roundtrip fixture step using caller-owned state. @param[in] fs Caller-owned fixture or filesystem state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL static void internal_check_stream_roundtrip(const fw_fs_t* fs)
{
  static const uint8_t first[] = {1U, 2U, 3U, 4U};
  static const uint8_t tail[]  = {5U, 6U};
  static const uint8_t whole[] = {1U, 2U, 3U, 4U, 5U, 6U};
  internal_write_file(fs, "/books/a.bin", first, sizeof(first));
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
  internal_expect_file(fs, "/books/a.bin", whole, sizeof(whole));
  fw_fs_stat_t stat = {};
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_stat(&fs->names, "/books/a.bin", &stat));
  TEST_ASSERT(stat.exists && stat.type == k_fw_fs_node_file && stat.size_bytes == sizeof(whole));
  internal_check_timestamp(fs->caps.flags, (uint32_t)k_fw_fs_cap_created_time, &stat.created);
  internal_check_timestamp(fs->caps.flags, (uint32_t)k_fw_fs_cap_modified_time, &stat.modified);
  internal_check_timestamp(fs->caps.flags, (uint32_t)k_fw_fs_cap_accessed_time, &stat.accessed);
}

/** @brief Exercise bounded listing, rename collision, unlink, and rmdir. @details Implements the bounded check namespace cleanup fixture step using caller-owned state. @param[in] fs Caller-owned fixture or filesystem state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0 */
RA8_INTERNAL static void internal_check_namespace_cleanup(const fw_fs_t* fs)
{
  static const uint8_t first[]  = {1U, 2U, 3U, 4U};
  list_ctx_t           list     = {};
  uint32_t             count    = 0U;
  bool                 complete = true;
  TEST_ASSERT_EQ(
    k_ra8_ok,
    fw_fs_listdir(&fs->names, "/books", 1U, internal_count_entry, &list, &count, &complete));
  TEST_ASSERT_EQ(1U, count);
  TEST_ASSERT(!complete);
  list = (list_ctx_t){};
  TEST_ASSERT_EQ(
    k_ra8_ok,
    fw_fs_listdir(&fs->names, "/books", 16U, internal_count_entry, &list, &count, &complete));
  TEST_ASSERT(complete && count >= 1U && list.saw_directory);

  TEST_ASSERT_EQ(k_ra8_err_not_empty, fw_fs_rmdir(&fs->names, "/books"));
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_rename(&fs->names, "/books/a.bin", "/books/b.bin", false));
  internal_write_file(fs, "/books/a.bin", first, sizeof(first));
  TEST_ASSERT_EQ(k_ra8_err_exists, fw_fs_rename(&fs->names, "/books/a.bin", "/books/b.bin", false));
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_unlink(&fs->names, "/books/a.bin"));
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_unlink(&fs->names, "/books/b.bin"));
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_rmdir(&fs->names, "/books/sub"));
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_rmdir(&fs->names, "/books"));
}

/** @brief Exercise file/dir metadata, one-level mkdir, bounded list, and
 * streams. @details Implements the bounded check namespace and streams fixture step using caller-owned state. @param[in] fs Caller-owned fixture or filesystem state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL static void internal_check_namespace_and_streams(const fw_fs_t* fs)
{
  internal_check_namespace_setup(fs);
  internal_check_stream_roundtrip(fs);
  internal_check_namespace_cleanup(fs);
}

/** @brief Exercise exclusive-create capability without assuming it exists. @details Implements the bounded check exclusive create fixture step using caller-owned state. @param[in] fs Caller-owned fixture or filesystem state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0 */
RA8_INTERNAL static void internal_check_exclusive_create(const fw_fs_t* fs)
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

/** @brief Prove no-replace requests are rejected before an unqualified backend. @details Implements the bounded check atomic noreplace gate fixture step using caller-owned state. @param[in] fs Caller-owned fixture or filesystem state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL static void internal_check_atomic_noreplace_gate(const fw_fs_t* fs)
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

/** @brief Run one staged transaction through write/validate/commit. @details Implements the bounded commit transaction fixture step using caller-owned state. @param[in] fs Caller-owned fixture or filesystem state. @param[in] destination Caller-owned bounded byte storage. @param[in] policy Value required by this filesystem vector. @param[in] validator Value required by this filesystem vector. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0 */
RA8_INTERNAL static void internal_commit_transaction(const fw_fs_t*             fs,
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
  TEST_ASSERT_EQ(k_ra8_ok,
                 fw_fs_transaction_validate(&txn, internal_validate_exact, (void*)validator));
  bool published = false;
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_transaction_commit(&txn, &published));
  TEST_ASSERT(published);
}

/** @brief Prove staged writers can reserve then backfill without sparse seeks. @details Implements the bounded check transaction backfill fixture step using caller-owned state. @param[in] fs Caller-owned fixture or filesystem state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL static void internal_check_transaction_backfill(const fw_fs_t* fs)
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
  TEST_ASSERT_EQ(k_ra8_ok,
                 fw_fs_transaction_validate(&txn, internal_validate_exact, (void*)&validator));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, fw_fs_transaction_seek(&txn, 0U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state,
                 fw_fs_transaction_write(&txn, payload, sizeof(payload), &written));
  bool published = false;
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_transaction_commit(&txn, &published));
  TEST_ASSERT(published);
  internal_expect_file(fs, "/backfill.bin", expected, sizeof(expected));
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_unlink(&fs->names, "/backfill.bin"));
}

/** @brief Simulate a destination appearing after begin; it must win untouched. @details Implements the bounded check transaction destination race fixture step using caller-owned state. @param[in] fs Caller-owned fixture or filesystem state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL static void internal_check_transaction_destination_race(const fw_fs_t* fs)
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
  TEST_ASSERT_EQ(k_ra8_ok,
                 fw_fs_transaction_validate(&txn, internal_validate_exact, (void*)&validator));
  internal_write_file(fs, "/race.bin", competitor, sizeof(competitor));
  bool published = true;
  TEST_ASSERT_EQ(k_ra8_err_exists, fw_fs_transaction_commit(&txn, &published));
  TEST_ASSERT(!published);
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_transaction_abort(&txn));
  internal_expect_file(fs, "/race.bin", competitor, sizeof(competitor));
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_unlink(&fs->names, "/race.bin"));
}

/** @brief Exercise create-new publication, collision, and abort cleanup. @details Implements the bounded check transaction abort cycle fixture step using caller-owned state. @param[in] fs Caller-owned fixture or filesystem state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0 */
RA8_INTERNAL static void internal_check_transaction_abort_cycle(const fw_fs_t* fs)
{
  static const uint8_t  good[]         = {'g', 'o', 'o', 'd'};
  const validator_ctx_t good_validator = {.bytes = good, .length = sizeof(good), .reject = false};
  internal_commit_transaction(fs, "/artifact.bin", k_fw_fs_txn_create_new, &good_validator);
  internal_expect_file(fs, "/artifact.bin", good, sizeof(good));

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
 * preservation. @details Implements the bounded check transaction replace fixture step using caller-owned state. @param[in] fs Caller-owned fixture or filesystem state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL static void internal_check_transaction_replace(const fw_fs_t* fs)
{
  static const uint8_t old[]   = {'o', 'l', 'd'};
  static const uint8_t newer[] = {'n', 'e', 'w'};
  test_workspace_t     work    = {};
  fw_fs_transaction_t  txn     = {};
  uint32_t             written = 0U;
  internal_write_file(fs, "/keep.bin", old, sizeof(old));
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
                   fw_fs_transaction_validate(&txn, internal_validate_exact, (void*)&reject));
    TEST_ASSERT_EQ(k_ra8_ok, fw_fs_transaction_abort(&txn));
    internal_expect_file(fs, "/keep.bin", old, sizeof(old));
    const validator_ctx_t replacement = {.bytes = newer, .length = sizeof(newer), .reject = false};
    internal_commit_transaction(fs, "/keep.bin", k_fw_fs_txn_replace_atomic, &replacement);
    internal_expect_file(fs, "/keep.bin", newer, sizeof(newer));
  } else {
    TEST_ASSERT_EQ(k_ra8_err_not_supported,
                   fw_fs_transaction_begin(&fs->transactions,
                                           "/keep.bin",
                                           k_fw_fs_txn_replace_atomic,
                                           &txn,
                                           work.bytes,
                                           sizeof(work.bytes)));
    internal_expect_file(fs, "/keep.bin", old, sizeof(old));
  }
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_unlink(&fs->names, "/keep.bin"));
}

/** @brief Exercise publication, abort, rejection, race, and backfill. @details Implements the bounded check transactions fixture step using caller-owned state. @param[in] fs Caller-owned fixture or filesystem state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0 */
RA8_INTERNAL static void internal_check_transactions(const fw_fs_t* fs)
{
  internal_check_transaction_abort_cycle(fs);
  internal_check_transaction_replace(fs);
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_unlink(&fs->names, "/artifact.bin"));
  internal_check_transaction_backfill(fs);
  internal_check_transaction_destination_race(fs);
}

/** @brief Shared backend-neutral conformance suite. @details Implements the bounded run conformance fixture step using caller-owned state. @param[in] label Validated fixture path or name value. @param[in] fs Caller-owned fixture or filesystem state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0 */
RA8_INTERNAL static void internal_run_conformance(const char* label, const fw_fs_t* fs)
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
  internal_check_path_policy(fs);
  internal_check_namespace_and_streams(fs);
  fw_if_fs_test_directory_cursors(fs);
  internal_check_exclusive_create(fs);
  internal_check_atomic_noreplace_gate(fs);
  internal_check_transactions(fs);
  TEST_END(label);
}

/** @brief Complete caller-owned storage fixture used by portability vectors. */
typedef struct internal_mdl_storage_fixture_t {
  test_workspace_t file_work;                         /**< Stream backend workspace.      */
  test_workspace_t transaction_work;                  /**< Transaction backend workspace. */
  uint8_t          io_buffer[k_mdl_storage_io_bytes]; /**< Copy/hash buffer.              */
  mdl_storage_t    storage;                           /**< Bound portable storage facade. */
} internal_mdl_storage_fixture_t;

/** @brief Canonical bytes copied and hashed by the portability vectors. */
static const uint8_t s_mdl_source[] = {'m', 'e', 'd', 'i', 'a'};

/** @brief Pre-existing destination bytes used to prove preservation. */
static const uint8_t s_mdl_old[] = {'o', 'l', 'd'};

/** @brief Reject overlapping workspaces, then bind disjoint portable storage. @details Implements the bounded init mdl storage fixture step using caller-owned state. @param[in] fs Caller-owned fixture or filesystem state. @param[in,out] fixture Value required by this filesystem vector. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0 */
RA8_INTERNAL
static void internal_init_mdl_storage(const fw_fs_t* fs, internal_mdl_storage_fixture_t* fixture)
{
  fixture->storage = (mdl_storage_t){.fs                    = (const fw_fs_t*)(uintptr_t)UINT32_MAX,
                                     .file_workspace        = (void*)(uintptr_t)UINT32_MAX,
                                     .transaction_workspace = (void*)(uintptr_t)UINT32_MAX,
                                     .io_buffer             = (uint8_t*)(uintptr_t)UINT32_MAX,
                                     .file_workspace_bytes  = UINT32_MAX,
                                     .transaction_workspace_bytes = UINT32_MAX,
                                     .io_buffer_bytes             = UINT32_MAX};
  const mdl_storage_t preserved = fixture->storage;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 mdl_storage_init(&fixture->storage,
                                  fs,
                                  fixture->file_work.bytes,
                                  sizeof(fixture->file_work.bytes),
                                  fixture->file_work.bytes,
                                  sizeof(fixture->file_work.bytes),
                                  fixture->io_buffer,
                                  sizeof(fixture->io_buffer)));
  TEST_ASSERT(memcmp(&fixture->storage, &preserved, sizeof(fixture->storage)) == 0);
  TEST_ASSERT_EQ(k_ra8_ok,
                 mdl_storage_init(&fixture->storage,
                                  fs,
                                  fixture->file_work.bytes,
                                  sizeof(fixture->file_work.bytes),
                                  fixture->transaction_work.bytes,
                                  sizeof(fixture->transaction_work.bytes),
                                  fixture->io_buffer,
                                  sizeof(fixture->io_buffer)));
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_mkdir(&fs->names, "/media"));
  internal_write_file(fs, "/media/source.bin", s_mdl_source, sizeof(s_mdl_source));
}

/**
 * @brief Run downloader storage behavior against either filesystem binding
 * @details Proves transactional copy, exact portable hashing, guarded child
 * creation, overlap rejection, output preservation, and truthful replacement
 * capability behavior with one backend-independent vector.
 * @param[in] label Test runner label.
 * @param[in] fs Initialized POSIX or RAM/FAT/VFS filesystem facade.
 * @pre @p label and @p fs are non-null and remain live for the test.
 * @pre The backend root is empty and supports namespace, stream, transactions.
 * @post Every created file and directory is removed.
 * @post Assertions expose any behavioral difference between the two adapters.
 * @note Test-local and single-threaded.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_check_mdl_storage_portability(const char*    label,
                                                                const fw_fs_t* fs)
{
  internal_mdl_storage_fixture_t fixture = {};
  TEST_BEGIN(label);
  internal_init_mdl_storage(fs, &fixture);

  uint64_t hash = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, mdl_hash_file(&fixture.storage, "/media/source.bin", &hash));
  TEST_ASSERT_EQ(mdl_hash_bytes(s_mdl_source, sizeof(s_mdl_source)), hash);
  hash = UINT64_MAX;
  TEST_ASSERT_EQ(k_ra8_err_not_found, mdl_hash_file(&fixture.storage, "/media/missing.bin", &hash));
  TEST_ASSERT_EQ(UINT64_MAX, hash);
  fw_fs_file_t bounded_file = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 fw_fs_open(&fs->streams,
                            "/media/source.bin",
                            k_fw_fs_open_read,
                            &bounded_file,
                            fixture.file_work.bytes,
                            sizeof(fixture.file_work.bytes)));
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 mdl_hash_stream(&bounded_file,
                                 (uint64_t)k_mdl_hash_max_file_bytes + 1U,
                                 fixture.io_buffer,
                                 sizeof(fixture.io_buffer),
                                 &hash));
  TEST_ASSERT_EQ(UINT64_MAX, hash);
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_close(&bounded_file));

  char directory[k_fw_fs_path_cap] = {};
  TEST_ASSERT(
    mdl_join_dir_under(&fixture.storage, "/media", "chapter-1", directory, sizeof(directory)));
  TEST_ASSERT(strcmp(directory, "/media/chapter-1") == 0);
  TEST_ASSERT(!mdl_join_dir_under(&fixture.storage, "/media", "..", directory, sizeof(directory)));
  TEST_ASSERT_EQ(k_ra8_ok,
                 mdl_storage_copy_atomic(&fixture.storage, "/media/source.bin", "/media/copy.bin"));
  internal_expect_file(fs, "/media/copy.bin", s_mdl_source, sizeof(s_mdl_source));

  internal_write_file(fs, "/media/existing.bin", s_mdl_old, sizeof(s_mdl_old));
  const ra8_err_t replaced =
    mdl_storage_copy_atomic(&fixture.storage, "/media/source.bin", "/media/existing.bin");
  if ((fs->caps.flags & (uint32_t)k_fw_fs_cap_atomic_replace) != 0U) {
    TEST_ASSERT_EQ(k_ra8_ok, replaced);
    internal_expect_file(fs, "/media/existing.bin", s_mdl_source, sizeof(s_mdl_source));
  } else {
    TEST_ASSERT_EQ(k_ra8_err_not_supported, replaced);
    internal_expect_file(fs, "/media/existing.bin", s_mdl_old, sizeof(s_mdl_old));
  }
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_unlink(&fs->names, "/media/existing.bin"));
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_unlink(&fs->names, "/media/copy.bin"));
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_unlink(&fs->names, "/media/source.bin"));
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_rmdir(&fs->names, "/media/chapter-1"));
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_rmdir(&fs->names, "/media"));
  TEST_END(label);
}

/** @brief Fill a staged VFS file and prove failure never publishes it. @details Implements the bounded check vfs full media fixture step using caller-owned state. @param[in] fs Caller-owned fixture or filesystem state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0 */
RA8_INTERNAL static void internal_check_vfs_full_media(const fw_fs_t* fs)
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
 * followed. @details Implements the bounded check posix symlinks fixture step using caller-owned state. @param[in] fs Caller-owned fixture or filesystem state. @param[in] root Value required by this filesystem vector. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL static void internal_check_posix_symlinks(const fw_fs_t* fs, const char* root)
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

/**
 * @brief Run the shared suite against the secure POSIX root adapter.
 *
 * @par MC/DC:
 * `fw_fs_bind` evaluates `file_sync_capable && (streams->sync == nullptr)`.
 * This test supplies the minimal three-vector set: POSIX initialization uses
 * `(true, false) -> false`, the optional binding uses `(false, true) -> false`,
 * and the dishonest binding uses `(true, true) -> true`. The latter two prove
 * the capability condition independently; the first and last prove the sync
 * pointer condition independently. @details Runs the posix conformance vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL static void internal_test_posix_conformance(void)
{
  char root[] = "/tmp/fw_fs_port_XXXXXX";
  TEST_ASSERT(mkdtemp(root) != nullptr);
  fw_fs_t                 fs    = {};
  fw_fs_posix_state_t     state = {.root_fd = -1};
  const fw_fs_posix_cfg_t cfg   = {.root_path = root, .removable_media = false};
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_posix_init(&fs, &state, &cfg));
  ra8_test_fw_if_fs_check_contract_guards(&fs);
  internal_run_conformance("fw_if_fs POSIX conformance", &fs);
  internal_check_mdl_storage_portability("media_dl POSIX storage", &fs);
  internal_check_posix_symlinks(&fs, root);
  TEST_ASSERT_EQ(k_ra8_ok, fw_fs_posix_deinit(&state));
  TEST_ASSERT_EQ(0, rmdir(root));
}

/**
 * @brief Set up and run the shared suite over RAM blockdev -> FAT -> VFS.
 *
 * @par MC/DC:
 * The VFS binding contributes `(file_sync_capable=false, streams->sync=null) ->
 * false` to the `fw_fs_bind` capability/operation decision. Together with the
 * preceding POSIX test's `(true, nonnull) -> false` and `(true, null) -> true`
 * vectors, each condition independently changes the decision; N=2, N+1=3. @details Runs the vfs conformance vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @pre Required fixture and backend state is initialized before the call. @post No access exceeds a caller-advertised capacity. @post The return value or assertions describe the observed filesystem state. @note Test-only helpers retain no hidden ownership beyond documented fixture state. @since 0.1.0
 */
RA8_INTERNAL static void internal_test_vfs_conformance(void)
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
  internal_run_conformance("fw_if_fs RAM/FAT/VFS conformance", &fs);
  internal_check_mdl_storage_portability("media_dl RAM/FAT/VFS storage", &fs);
  internal_check_vfs_full_media(&fs);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_io_vfs_unmount("ram"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(s_mount));
  s_mount = nullptr;
}

int main(void)
{
  internal_test_posix_conformance();
  internal_test_vfs_conformance();
  return 0;
}
