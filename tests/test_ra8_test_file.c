/**
 * @file test_ra8_test_file.c
 * @brief Fault and real-POSIX tests for bounded host fixture I/O
 * @details Proves caller-state preservation, bounded retries, nanosecond
 * mutation detection, transactional replacement, publication truth, and
 * symlink/special-node refusal without using allocator-backed streams.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "support/ra8_test_file.h"
#include "support/ra8_test_file_posix.h"
#include "support/ra8_test_output.h"
#include "unity_minimal.h"

/** @brief Mock descriptor identities and buffer capacities. */
typedef enum : size_t {
  k_mock_read_fd   = 40,  /**< Injected source descriptor.           */
  k_mock_parent_fd = 41,  /**< Injected parent descriptor.           */
  k_mock_temp_fd   = 42,  /**< Injected staging descriptor.          */
  k_mock_data_cap  = 32U, /**< Bytes in the injected fixture buffer. */
} mock_limit_t;

/** @brief Fault-injectable raw operation identifiers. */
typedef enum : uint8_t {
  k_mock_open_path = 0U, /**< Open a complete path.           */
  k_mock_open_at,        /**< Open a parent-relative leaf.    */
  k_mock_stat_fd,        /**< Inspect an open descriptor.     */
  k_mock_stat_at,        /**< Inspect a parent-relative leaf. */
  k_mock_read_at,        /**< Read at a fixed offset.         */
  k_mock_write_at,       /**< Write at a fixed offset.        */
  k_mock_sync_fd,        /**< Synchronize a descriptor.       */
  k_mock_rename_at,      /**< Publish a staged leaf.          */
  k_mock_unlink_at,      /**< Remove an unpublished leaf.     */
  k_mock_close_fd,       /**< Close one descriptor.           */
  k_mock_op_count,       /**< Number of injected operations.  */
} mock_op_t;

/** @brief Caller-owned model backing one injected operation table. */
typedef struct {
  size_t      calls[k_mock_op_count];           /**< Per-operation call counts.    */
  size_t      eintr_remaining[k_mock_op_count]; /**< Interrupted calls remaining.  */
  size_t      fail_call[k_mock_op_count];       /**< One failing call, or zero.    */
  int         fail_error[k_mock_op_count];      /**< Error for that failing call.  */
  struct stat before;                           /**< Initial input metadata.       */
  struct stat after;                            /**< Final input metadata.         */
  uint8_t     source[k_mock_data_cap];          /**< Injected input bytes.         */
  size_t      source_length;                    /**< Injected input size.          */
  uint8_t     written[k_mock_data_cap];         /**< Captured staged output.       */
  size_t      written_length;                   /**< Captured staged size.         */
  size_t      read_chunk;                       /**< Positive short-read cap.      */
  size_t      write_chunk;                      /**< Positive short-write cap.     */
  size_t      collisions;                       /**< Initial temp collisions.      */
  size_t      read_stat_calls;                  /**< Input fstat sequence.         */
  int         open_path_flags;                  /**< Captured path-open flags.     */
  int         open_at_flags;                    /**< Captured temp-open flags.     */
  mode_t      open_at_mode;                     /**< Captured temp mode.           */
  mode_t      destination_mode;                 /**< Existing destination mode.    */
  bool        destination_exists;               /**< Destination stat succeeds.    */
  bool        read_zero;                        /**< Next successful read stalls.  */
  bool        write_zero;                       /**< Next successful write stalls. */
  bool        extra_byte;                       /**< EOF probe sees growth.        */
  bool        final_size_wrong;                 /**< Final staged size mismatches. */
  bool        renamed;                          /**< Transaction published.        */
  bool        unlinked;                         /**< Temp cleanup ran.             */
  unsigned    generation;                       /**< Visible target generation.    */
} mock_file_t;

/** @brief Set portable nanosecond timestamps on one metadata value.
 * @details Performs one bounded, deterministic operation for this host test.
 * @param[in,out] status Argument for the bounded test operation.
 * @param[in] nanoseconds Argument for the bounded test operation.
 * @pre Pointer arguments, when present, address their documented test storage.
 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post The helper completes only the bounded test operation described above.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
 * @since 0.1.0
*/
RA8_INTERNAL static void internal_set_stat_times(struct stat* status, long nanoseconds)
{
#if defined(__APPLE__)
  status->st_mtimespec = (struct timespec){.tv_sec = 100, .tv_nsec = nanoseconds};
  status->st_ctimespec = (struct timespec){.tv_sec = 101, .tv_nsec = nanoseconds};
#else
  status->st_mtim = (struct timespec){.tv_sec = 100, .tv_nsec = nanoseconds};
  status->st_ctim = (struct timespec){.tv_sec = 101, .tv_nsec = nanoseconds};
#endif
}

/** @brief Initialize one independent regular-file model.
 * @details Performs one bounded, deterministic operation for this host test.
 * @param[in,out] mock Argument for the bounded test operation.
 * @pre Pointer arguments, when present, address their documented test storage.
 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post The helper completes only the bounded test operation described above.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
 * @since 0.1.0
*/
RA8_INTERNAL static void internal_mock_init(mock_file_t* mock)
{
  memset(mock, 0, sizeof(*mock));
  memcpy(mock->source, "fixture", 7U);
  mock->source_length      = 7U;
  mock->before.st_dev      = (dev_t)1;
  mock->before.st_ino      = (ino_t)2;
  mock->before.st_mode     = S_IFREG | (mode_t)0600;
  mock->before.st_size     = (off_t)mock->source_length;
  mock->after              = mock->before;
  mock->destination_exists = true;
  mock->destination_mode   = S_IFREG | (mode_t)0600;
  mock->generation         = 1U;
  internal_set_stat_times(&mock->before, 10L);
  internal_set_stat_times(&mock->after, 10L);
}

/** @brief Apply common EINTR and one-shot error scripting.
 * @details Performs one bounded, deterministic operation for this host test.
 * @param[in,out] mock Argument for the bounded test operation.
 * @param[in] operation Argument for the bounded test operation.
 * @return Function-specific result consumed by the calling test.
 * @retval 0 Zero or false result; nonzero values describe the alternate result.
 * @pre Pointer arguments, when present, address their documented test storage.
 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post The helper completes only the bounded test operation described above.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
 * @since 0.1.0
*/
RA8_INTERNAL static bool internal_mock_gate(mock_file_t* mock, mock_op_t operation)
{
  ++mock->calls[operation];
  if (mock->eintr_remaining[operation] > 0U) {
    --mock->eintr_remaining[operation];
    errno = EINTR;
    return false;
  }
  if ((mock->fail_call[operation] != 0U) &&
      (mock->calls[operation] == mock->fail_call[operation])) {
    errno = mock->fail_error[operation];
    return false;
  }
  return true;
}

/** @brief Mock path-open operation.
 * @details Performs one bounded, deterministic operation for this host test.
 * @param[in,out] context Argument for the bounded test operation.
 * @param[in] path Argument for the bounded test operation.
 * @param[in] flags Argument for the bounded test operation.
 * @param[in] mode Argument for the bounded test operation.
 * @return Function-specific result consumed by the calling test.
 * @retval 0 Zero or false result; nonzero values describe the alternate result.
 * @pre Pointer arguments, when present, address their documented test storage.
 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post The helper completes only the bounded test operation described above.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
 * @since 0.1.0
*/
RA8_INTERNAL static int
internal_mock_open_path(void* context, const char* path, int flags, mode_t mode)
{
  mock_file_t* mock = context;
  (void)path;
  (void)mode;
  mock->open_path_flags = flags;
  if (!internal_mock_gate(mock, k_mock_open_path)) {
    return -1;
  }
  return ((flags & O_DIRECTORY) != 0) ? (int)k_mock_parent_fd : (int)k_mock_read_fd;
}

/** @brief Mock sibling-create operation with bounded collisions.
 * @details Performs one bounded, deterministic operation for this host test.
 * @param[in,out] context Argument for the bounded test operation.
 * @param[in] dir_fd Argument for the bounded test operation.
 * @param[in] path Argument for the bounded test operation.
 * @param[in] flags Argument for the bounded test operation.
 * @param[in] mode Argument for the bounded test operation.
 * @return Function-specific result consumed by the calling test.
 * @retval 0 Zero or false result; nonzero values describe the alternate result.
 * @pre Pointer arguments, when present, address their documented test storage.
 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post The helper completes only the bounded test operation described above.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
 * @since 0.1.0
*/
RA8_INTERNAL static int
internal_mock_open_at(void* context, int dir_fd, const char* path, int flags, mode_t mode)
{
  mock_file_t* mock = context;
  (void)dir_fd;
  (void)path;
  mock->open_at_flags = flags;
  mock->open_at_mode  = mode;
  if (!internal_mock_gate(mock, k_mock_open_at)) {
    return -1;
  }
  if (mock->collisions > 0U) {
    --mock->collisions;
    errno = EEXIST;
    return -1;
  }
  return (int)k_mock_temp_fd;
}

/** @brief Mock descriptor metadata for input or staged output.
 * @details Performs one bounded, deterministic operation for this host test.
 * @param[in,out] context Argument for the bounded test operation.
 * @param[in] descriptor Argument for the bounded test operation.
 * @param[in,out] status Argument for the bounded test operation.
 * @return Function-specific result consumed by the calling test.
 * @retval 0 Zero or false result; nonzero values describe the alternate result.
 * @pre Pointer arguments, when present, address their documented test storage.
 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post The helper completes only the bounded test operation described above.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
 * @since 0.1.0
*/
RA8_INTERNAL static int internal_mock_stat_fd(void* context, int descriptor, struct stat* status)
{
  mock_file_t* mock = context;
  if (!internal_mock_gate(mock, k_mock_stat_fd)) {
    return -1;
  }
  if (descriptor == (int)k_mock_read_fd) {
    *status = (mock->read_stat_calls++ == 0U) ? mock->before : mock->after;
  } else {
    memset(status, 0, sizeof(*status));
    status->st_mode = S_IFREG | (mode_t)0600;
    status->st_size = (off_t)(mock->written_length + (mock->final_size_wrong ? 1U : 0U));
  }
  return 0;
}

/** @brief Mock no-follow destination inspection.
 * @details Performs one bounded, deterministic operation for this host test.
 * @param[in,out] context Argument for the bounded test operation.
 * @param[in] dir_fd Argument for the bounded test operation.
 * @param[in] path Argument for the bounded test operation.
 * @param[in,out] status Argument for the bounded test operation.
 * @param[in] flags Argument for the bounded test operation.
 * @return Function-specific result consumed by the calling test.
 * @retval 0 Zero or false result; nonzero values describe the alternate result.
 * @pre Pointer arguments, when present, address their documented test storage.
 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post The helper completes only the bounded test operation described above.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
 * @since 0.1.0
*/
RA8_INTERNAL static int
internal_mock_stat_at(void* context, int dir_fd, const char* path, struct stat* status, int flags)
{
  mock_file_t* mock = context;
  (void)dir_fd;
  (void)path;
  TEST_ASSERT((flags & AT_SYMLINK_NOFOLLOW) != 0);
  if (!internal_mock_gate(mock, k_mock_stat_at)) {
    return -1;
  }
  if (!mock->destination_exists) {
    errno = ENOENT;
    return -1;
  }
  memset(status, 0, sizeof(*status));
  status->st_mode = mock->destination_mode;
  return 0;
}

/** @brief Mock bounded positioned input.
 * @details Performs one bounded, deterministic operation for this host test.
 * @param[in,out] context Argument for the bounded test operation.
 * @param[in] descriptor Argument for the bounded test operation.
 * @param[in,out] destination Argument for the bounded test operation.
 * @param[in] length Argument for the bounded test operation.
 * @param[in] offset Argument for the bounded test operation.
 * @return Function-specific result consumed by the calling test.
 * @retval 0 Zero or false result; nonzero values describe the alternate result.
 * @pre Pointer arguments, when present, address their documented test storage.
 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post The helper completes only the bounded test operation described above.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
 * @since 0.1.0
*/
RA8_INTERNAL static ssize_t
internal_mock_read_at(void* context, int descriptor, void* destination, size_t length, off_t offset)
{
  mock_file_t* mock = context;
  (void)descriptor;
  if (!internal_mock_gate(mock, k_mock_read_at)) {
    return -1;
  }
  if (mock->read_zero) {
    return 0;
  }
  if ((size_t)offset >= mock->source_length) {
    if (mock->extra_byte) {
      *(uint8_t*)destination = 0xEEU;
      return 1;
    }
    return 0;
  }
  size_t amount = mock->source_length - (size_t)offset;
  amount        = (amount < length) ? amount : length;
  if ((mock->read_chunk != 0U) && (amount > mock->read_chunk)) {
    amount = mock->read_chunk;
  }
  memcpy(destination, &mock->source[(size_t)offset], amount);
  return (ssize_t)amount;
}

/** @brief Mock bounded positioned output.
 * @details Performs one bounded, deterministic operation for this host test.
 * @param[in,out] context Argument for the bounded test operation.
 * @param[in] descriptor Argument for the bounded test operation.
 * @param[in] source Argument for the bounded test operation.
 * @param[in] length Argument for the bounded test operation.
 * @param[in] offset Argument for the bounded test operation.
 * @return Function-specific result consumed by the calling test.
 * @retval 0 Zero or false result; nonzero values describe the alternate result.
 * @pre Pointer arguments, when present, address their documented test storage.
 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post The helper completes only the bounded test operation described above.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
 * @since 0.1.0
*/
RA8_INTERNAL static ssize_t internal_mock_write_at(void*       context,
                                                   int         descriptor,
                                                   const void* source,
                                                   size_t      length,
                                                   off_t       offset)
{
  mock_file_t* mock = context;
  (void)descriptor;
  if (!internal_mock_gate(mock, k_mock_write_at)) {
    return -1;
  }
  if (mock->write_zero) {
    return 0;
  }
  size_t amount = length;
  if ((mock->write_chunk != 0U) && (amount > mock->write_chunk)) {
    amount = mock->write_chunk;
  }
  TEST_ASSERT(((size_t)offset + amount) <= sizeof(mock->written));
  memcpy(&mock->written[(size_t)offset], source, amount);
  if (((size_t)offset + amount) > mock->written_length) {
    mock->written_length = (size_t)offset + amount;
  }
  return (ssize_t)amount;
}

/** @brief Mock descriptor synchronization.
 * @details Performs one bounded, deterministic operation for this host test.
 * @param[in,out] context Argument for the bounded test operation.
 * @param[in] descriptor Argument for the bounded test operation.
 * @return Function-specific result consumed by the calling test.
 * @retval 0 Zero or false result; nonzero values describe the alternate result.
 * @pre Pointer arguments, when present, address their documented test storage.
 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post The helper completes only the bounded test operation described above.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
 * @since 0.1.0
*/
RA8_INTERNAL static int internal_mock_sync_fd(void* context, int descriptor)
{
  mock_file_t* mock = context;
  (void)descriptor;
  return internal_mock_gate(mock, k_mock_sync_fd) ? 0 : -1;
}

/** @brief Mock atomic publication.
 * @details Performs one bounded, deterministic operation for this host test.
 * @param[in,out] context Argument for the bounded test operation.
 * @param[in] old_dir_fd Argument for the bounded test operation.
 * @param[in] old_path Argument for the bounded test operation.
 * @param[in] new_dir_fd Argument for the bounded test operation.
 * @param[in] new_path Argument for the bounded test operation.
 * @return Function-specific result consumed by the calling test.
 * @retval 0 Zero or false result; nonzero values describe the alternate result.
 * @pre Pointer arguments, when present, address their documented test storage.
 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post The helper completes only the bounded test operation described above.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
 * @since 0.1.0
*/
RA8_INTERNAL static int internal_mock_rename_at(void*       context,
                                                int         old_dir_fd,
                                                const char* old_path,
                                                int         new_dir_fd,
                                                const char* new_path)
{
  mock_file_t* mock = context;
  (void)old_dir_fd;
  (void)old_path;
  (void)new_dir_fd;
  (void)new_path;
  if (!internal_mock_gate(mock, k_mock_rename_at)) {
    return -1;
  }
  mock->renamed = true;
  ++mock->generation;
  return 0;
}

/** @brief Mock temporary cleanup.
 * @details Performs one bounded, deterministic operation for this host test.
 * @param[in,out] context Argument for the bounded test operation.
 * @param[in] dir_fd Argument for the bounded test operation.
 * @param[in] path Argument for the bounded test operation.
 * @param[in] flags Argument for the bounded test operation.
 * @return Function-specific result consumed by the calling test.
 * @retval 0 Zero or false result; nonzero values describe the alternate result.
 * @pre Pointer arguments, when present, address their documented test storage.
 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post The helper completes only the bounded test operation described above.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
 * @since 0.1.0
*/
RA8_INTERNAL static int
internal_mock_unlink_at(void* context, int dir_fd, const char* path, int flags)
{
  mock_file_t* mock = context;
  (void)dir_fd;
  (void)path;
  (void)flags;
  if (!internal_mock_gate(mock, k_mock_unlink_at)) {
    return -1;
  }
  mock->unlinked = true;
  return 0;
}

/** @brief Mock exact one-shot close.
 * @details Performs one bounded, deterministic operation for this host test.
 * @param[in,out] context Argument for the bounded test operation.
 * @param[in] descriptor Argument for the bounded test operation.
 * @return Function-specific result consumed by the calling test.
 * @retval 0 Zero or false result; nonzero values describe the alternate result.
 * @pre Pointer arguments, when present, address their documented test storage.
 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post The helper completes only the bounded test operation described above.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
 * @since 0.1.0
*/
RA8_INTERNAL static int internal_mock_close_fd(void* context, int descriptor)
{
  mock_file_t* mock = context;
  (void)descriptor;
  return internal_mock_gate(mock, k_mock_close_fd) ? 0 : -1;
}

/** @brief Bind a complete operation table to one independent model.
 * @details Performs one bounded, deterministic operation for this host test.
 * @param[in,out] mock Argument for the bounded test operation.
 * @return Function-specific result consumed by the calling test.
 * @retval 0 Zero or false result; nonzero values describe the alternate result.
 * @pre Pointer arguments, when present, address their documented test storage.
 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post The helper completes only the bounded test operation described above.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
 * @since 0.1.0
*/
RA8_INTERNAL static ra8_test_file_ops_t internal_mock_ops(mock_file_t* mock)
{
  return (ra8_test_file_ops_t){.context   = mock,
                               .open_path = internal_mock_open_path,
                               .open_at   = internal_mock_open_at,
                               .stat_fd   = internal_mock_stat_fd,
                               .stat_at   = internal_mock_stat_at,
                               .read_at   = internal_mock_read_at,
                               .write_at  = internal_mock_write_at,
                               .sync_fd   = internal_mock_sync_fd,
                               .rename_at = internal_mock_rename_at,
                               .unlink_at = internal_mock_unlink_at,
                               .close_fd  = internal_mock_close_fd};
}

/** @test Successful staged reads retry and isolate independent bindings.
 * @brief Test read success and bindings.
 * @details Performs one bounded, deterministic operation for this host test.
 * @pre Pointer arguments, when present, address their documented test storage.
 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post The helper completes only the bounded test operation described above.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
 * @since 0.1.0
*/
RA8_INTERNAL static void internal_test_read_success_and_bindings(void)
{
  mock_file_t first;
  mock_file_t second;
  internal_mock_init(&first);
  internal_mock_init(&second);
  first.eintr_remaining[k_mock_read_at] = 1U;
  first.read_chunk                      = 2U;
  memcpy(second.source, "second!", 7U);
  uint8_t                      first_out[8]  = {0xA5U};
  uint8_t                      first_tmp[8]  = {};
  uint8_t                      second_out[8] = {0x5AU};
  uint8_t                      second_tmp[8] = {};
  const ra8_test_file_ops_t    first_ops     = internal_mock_ops(&first);
  const ra8_test_file_ops_t    second_ops    = internal_mock_ops(&second);
  const ra8_test_file_result_t a =
    internal_test_file_read_with_ops(&first_ops, "a", first_out, 8U, first_tmp, 8U);
  const ra8_test_file_result_t b =
    internal_test_file_read_with_ops(&second_ops, "b", second_out, 8U, second_tmp, 8U);
  TEST_ASSERT_EQ(k_ra8_test_file_ok, a.status);
  TEST_ASSERT_EQ(7U, a.transferred);
  TEST_ASSERT_EQ(0, memcmp(first_out, "fixture", 7U));
  TEST_ASSERT_EQ(0, memcmp(second_out, "second!", 7U));
  TEST_ASSERT_EQ(1U, second.calls[k_mock_open_path]);
  TEST_ASSERT((first.open_path_flags & (O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK)) ==
              (O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
  TEST_ASSERT_EQ(k_ra8_test_file_ok, b.status);
}

/** @test Capacity, zero progress, retry exhaustion, and close preserve output.
 * @brief Test read failures preserve destination.
 * @details Performs one bounded, deterministic operation for this host test.
 * @pre Pointer arguments, when present, address their documented test storage.
 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post The helper completes only the bounded test operation described above.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
 * @since 0.1.0
*/
RA8_INTERNAL static void internal_test_read_failures_preserve_destination(void)
{
  const uint8_t original[8] = {0xA5U, 1U, 2U, 3U, 4U, 5U, 6U, 7U};
  uint8_t       output[8];
  uint8_t       staging[8] = {};
  mock_file_t   mock;
  internal_mock_init(&mock);
  memcpy(output, original, sizeof(output));
  const ra8_test_file_ops_t ops = internal_mock_ops(&mock);
  ra8_test_file_result_t    result =
    internal_test_file_read_with_ops(&ops, "x", output, 6U, staging, 8U);
  TEST_ASSERT_EQ(k_ra8_test_file_capacity, result.status);
  TEST_ASSERT_EQ(7U, result.required);
  TEST_ASSERT_EQ(6U, result.supplied);
  TEST_ASSERT_EQ(0, memcmp(output, original, sizeof(output)));

  internal_mock_init(&mock);
  mock.read_zero = true;
  result         = internal_test_file_read_with_ops(&ops, "x", output, 8U, staging, 8U);
  TEST_ASSERT_EQ(k_ra8_test_file_short, result.status);
  TEST_ASSERT_EQ(0, memcmp(output, original, sizeof(output)));

  internal_mock_init(&mock);
  mock.eintr_remaining[k_mock_read_at] = k_ra8_test_file_retry_limit + 1U;
  result = internal_test_file_read_with_ops(&ops, "x", output, 8U, staging, 8U);
  TEST_ASSERT_EQ(k_ra8_test_file_error, result.status);
  TEST_ASSERT_EQ(EINTR, result.os_error);
  TEST_ASSERT_EQ(k_ra8_test_file_retry_limit + 1U, mock.calls[k_mock_read_at]);
  TEST_ASSERT_EQ(0, memcmp(output, original, sizeof(output)));
}

/** @test Same-second nanosecond mutation and close failure block commit.
 * @brief Test read mutation and close.
 * @details Performs one bounded, deterministic operation for this host test.
 * @pre Pointer arguments, when present, address their documented test storage.
 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post The helper completes only the bounded test operation described above.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
 * @since 0.1.0
*/
RA8_INTERNAL static void internal_test_read_mutation_and_close(void)
{
  const uint8_t original[8] = {9U, 8U, 7U, 6U, 5U, 4U, 3U, 2U};
  uint8_t       output[8];
  uint8_t       staging[8] = {};
  mock_file_t   mock;
  internal_mock_init(&mock);
  internal_set_stat_times(&mock.after, 11L);
  memcpy(output, original, sizeof(output));
  const ra8_test_file_ops_t ops = internal_mock_ops(&mock);
  ra8_test_file_result_t    result =
    internal_test_file_read_with_ops(&ops, "x", output, sizeof(output), staging, sizeof(staging));
  TEST_ASSERT_EQ(k_ra8_test_file_changed, result.status);
  TEST_ASSERT_EQ(0, memcmp(output, original, sizeof(output)));

  internal_mock_init(&mock);
  mock.fail_call[k_mock_close_fd]  = 1U;
  mock.fail_error[k_mock_close_fd] = EIO;
  result =
    internal_test_file_read_with_ops(&ops, "x", output, sizeof(output), staging, sizeof(staging));
  TEST_ASSERT_EQ(k_ra8_test_file_error, result.status);
  TEST_ASSERT_EQ(EIO, result.os_error);
  TEST_ASSERT_EQ(0, memcmp(output, original, sizeof(output)));

  internal_mock_init(&mock);
  mock.extra_byte = true;
  result =
    internal_test_file_read_with_ops(&ops, "x", output, sizeof(output), staging, sizeof(staging));
  TEST_ASSERT_EQ(k_ra8_test_file_changed, result.status);
}

/** @test Successful replacement is exact, synchronized, and published once.
 * @brief Test replace success.
 * @details Performs one bounded, deterministic operation for this host test.
 * @pre Pointer arguments, when present, address their documented test storage.
 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post The helper completes only the bounded test operation described above.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
 * @since 0.1.0
*/
RA8_INTERNAL static void internal_test_replace_success(void)
{
  mock_file_t mock;
  internal_mock_init(&mock);
  mock.eintr_remaining[k_mock_write_at]  = 1U;
  mock.write_chunk                       = 2U;
  const ra8_test_file_ops_t    ops       = internal_mock_ops(&mock);
  const uint8_t                payload[] = {1U, 2U, 3U, 4U, 5U};
  const ra8_test_file_result_t result =
    internal_test_file_replace_with_ops(&ops, "/tmp/out.img", payload, sizeof(payload));
  TEST_ASSERT_EQ(k_ra8_test_file_ok, result.status);
  TEST_ASSERT(result.published);
  TEST_ASSERT_EQ(sizeof(payload), result.transferred);
  TEST_ASSERT_EQ(0, memcmp(mock.written, payload, sizeof(payload)));
  TEST_ASSERT_EQ(2U, mock.calls[k_mock_sync_fd]);
  TEST_ASSERT_EQ(2U, mock.calls[k_mock_close_fd]);
  TEST_ASSERT_EQ(2U, mock.generation);
  TEST_ASSERT((mock.open_path_flags & (O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)) ==
              (O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
  TEST_ASSERT(
    (mock.open_at_flags & (O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK)) ==
    (O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
  TEST_ASSERT_EQ((mode_t)0600, mock.open_at_mode);
}

/** @test Prepublication failures preserve the old target and clean staging.
 * @brief Test replace prepublish failures.
 * @details Performs one bounded, deterministic operation for this host test.
 * @pre Pointer arguments, when present, address their documented test storage.
 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post The helper completes only the bounded test operation described above.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
 * @since 0.1.0
*/
RA8_INTERNAL static void internal_test_replace_prepublish_failures(void)
{
  const uint8_t payload[] = {1U, 2U, 3U};
  mock_file_t   mock;
  internal_mock_init(&mock);
  mock.collisions               = k_ra8_test_file_temp_attempts;
  const ra8_test_file_ops_t ops = internal_mock_ops(&mock);
  ra8_test_file_result_t    result =
    internal_test_file_replace_with_ops(&ops, "out", payload, sizeof(payload));
  TEST_ASSERT_EQ(k_ra8_test_file_collision, result.status);
  TEST_ASSERT_EQ(k_ra8_test_file_temp_attempts, mock.calls[k_mock_open_at]);
  TEST_ASSERT_EQ(0U, mock.calls[k_mock_unlink_at]);
  TEST_ASSERT_EQ(1U, mock.generation);

  internal_mock_init(&mock);
  mock.write_zero = true;
  result          = internal_test_file_replace_with_ops(&ops, "out", payload, sizeof(payload));
  TEST_ASSERT_EQ(k_ra8_test_file_short, result.status);
  TEST_ASSERT(mock.unlinked);
  TEST_ASSERT_EQ(1U, mock.generation);

  internal_mock_init(&mock);
  mock.final_size_wrong = true;
  result = internal_test_file_replace_with_ops(&ops, "out", payload, sizeof(payload));
  TEST_ASSERT_EQ(k_ra8_test_file_short, result.status);
  TEST_ASSERT(mock.unlinked);

  internal_mock_init(&mock);
  mock.fail_call[k_mock_sync_fd]  = 1U;
  mock.fail_error[k_mock_sync_fd] = EIO;
  result = internal_test_file_replace_with_ops(&ops, "out", payload, sizeof(payload));
  TEST_ASSERT_EQ(k_ra8_test_file_error, result.status);
  TEST_ASSERT(!result.published && mock.unlinked);
}

/** @test Parent-open, inspect, final-stat, and temp-close failures stay private.
 * @brief Test replace operation faults.
 * @details Performs one bounded, deterministic operation for this host test.
 * @pre Pointer arguments, when present, address their documented test storage.
 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post The helper completes only the bounded test operation described above.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
 * @since 0.1.0
*/
RA8_INTERNAL static void internal_test_replace_operation_faults(void)
{
  const uint8_t payload[] = {3U, 2U, 1U};
  mock_file_t   mock;
  internal_mock_init(&mock);
  mock.fail_call[k_mock_open_path]  = 1U;
  mock.fail_error[k_mock_open_path] = EACCES;
  const ra8_test_file_ops_t ops     = internal_mock_ops(&mock);
  ra8_test_file_result_t    result =
    internal_test_file_replace_with_ops(&ops, "out", payload, sizeof(payload));
  TEST_ASSERT_EQ(EACCES, result.os_error);
  TEST_ASSERT_EQ(0U, mock.calls[k_mock_open_at]);

  internal_mock_init(&mock);
  mock.fail_call[k_mock_stat_at]  = 1U;
  mock.fail_error[k_mock_stat_at] = EIO;
  result = internal_test_file_replace_with_ops(&ops, "out", payload, sizeof(payload));
  TEST_ASSERT_EQ(EIO, result.os_error);
  TEST_ASSERT_EQ(0U, mock.calls[k_mock_open_at]);

  internal_mock_init(&mock);
  mock.fail_call[k_mock_stat_fd]  = 1U;
  mock.fail_error[k_mock_stat_fd] = EOVERFLOW;
  result = internal_test_file_replace_with_ops(&ops, "out", payload, sizeof(payload));
  TEST_ASSERT_EQ(EOVERFLOW, result.os_error);
  TEST_ASSERT(mock.unlinked && !result.published);

  internal_mock_init(&mock);
  mock.fail_call[k_mock_close_fd]  = 1U;
  mock.fail_error[k_mock_close_fd] = EIO;
  result = internal_test_file_replace_with_ops(&ops, "out", payload, sizeof(payload));
  TEST_ASSERT_EQ(EIO, result.os_error);
  TEST_ASSERT(mock.unlinked && !result.published);
  TEST_ASSERT_EQ(1U, mock.generation);
}

/** @test Rename/cleanup and postpublication failures keep truthful precedence.
 * @brief Test replace publication truth.
 * @details Performs one bounded, deterministic operation for this host test.
 * @pre Pointer arguments, when present, address their documented test storage.
 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post The helper completes only the bounded test operation described above.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
 * @since 0.1.0
*/
RA8_INTERNAL static void internal_test_replace_publication_truth(void)
{
  const uint8_t payload[] = {7U, 8U};
  mock_file_t   mock;
  internal_mock_init(&mock);
  mock.fail_call[k_mock_rename_at]  = 1U;
  mock.fail_error[k_mock_rename_at] = EACCES;
  mock.fail_call[k_mock_unlink_at]  = 1U;
  mock.fail_error[k_mock_unlink_at] = EIO;
  const ra8_test_file_ops_t ops     = internal_mock_ops(&mock);
  ra8_test_file_result_t    result =
    internal_test_file_replace_with_ops(&ops, "out", payload, sizeof(payload));
  TEST_ASSERT_EQ(k_ra8_test_file_error, result.status);
  TEST_ASSERT_EQ(EACCES, result.os_error);
  TEST_ASSERT_EQ(EIO, result.cleanup_error);
  TEST_ASSERT(!result.published);
  TEST_ASSERT_EQ(1U, mock.generation);

  internal_mock_init(&mock);
  mock.fail_call[k_mock_sync_fd]   = 2U;
  mock.fail_error[k_mock_sync_fd]  = ENOSPC;
  mock.fail_call[k_mock_close_fd]  = 2U;
  mock.fail_error[k_mock_close_fd] = EIO;
  result = internal_test_file_replace_with_ops(&ops, "out", payload, sizeof(payload));
  TEST_ASSERT_EQ(k_ra8_test_file_error, result.status);
  TEST_ASSERT_EQ(ENOSPC, result.os_error);
  TEST_ASSERT_EQ(EIO, result.cleanup_error);
  TEST_ASSERT(result.published && mock.renamed);
  TEST_ASSERT_EQ(2U, mock.generation);
}

/** @brief Join one temporary directory and leaf without formatting.
 * @details Performs one bounded, deterministic operation for this host test.
 * @param[in,out] output Argument for the bounded test operation.
 * @param[in] capacity Argument for the bounded test operation.
 * @param[in] directory Argument for the bounded test operation.
 * @param[in] leaf Argument for the bounded test operation.
 * @return Function-specific result consumed by the calling test.
 * @retval 0 Zero or false result; nonzero values describe the alternate result.
 * @pre Pointer arguments, when present, address their documented test storage.
 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post The helper completes only the bounded test operation described above.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
 * @since 0.1.0
*/
RA8_INTERNAL static bool
internal_test_join(char* output, size_t capacity, const char* directory, const char* leaf)
{
  const size_t directory_length = strlen(directory);
  const size_t leaf_length      = strlen(leaf);
  if ((directory_length + 1U + leaf_length + 1U) > capacity) {
    return false;
  }
  memcpy(output, directory, directory_length + 1U);
  output[directory_length] = '/';
  memcpy(&output[directory_length + 1U], leaf, leaf_length + 1U);
  return true;
}

/** @test Real POSIX paths preserve targets, reject links/specials, and leave no stages.
 * @brief Test real posix parity.
 * @details Performs one bounded, deterministic operation for this host test.
 * @pre Pointer arguments, when present, address their documented test storage.
 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post The helper completes only the bounded test operation described above.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
 * @since 0.1.0
*/
RA8_INTERNAL static void internal_test_real_posix_parity(void)
{
  char directory[] = "/tmp/ra8-test-file.XXXXXX";
  TEST_ASSERT_NOT_NULL(mkdtemp(directory));
  char path[256]      = {};
  char link_path[256] = {};
  char fifo_path[256] = {};
  TEST_ASSERT(internal_test_join(path, sizeof(path), directory, "fixture.bin"));
  TEST_ASSERT(internal_test_join(link_path, sizeof(link_path), directory, "fixture.link"));
  TEST_ASSERT(internal_test_join(fifo_path, sizeof(fifo_path), directory, "fixture.fifo"));
  const uint8_t old_data[] = {1U, 2U, 3U};
  const uint8_t new_data[] = {4U, 5U, 6U, 7U};
  TEST_ASSERT_EQ(k_ra8_test_file_ok,
                 internal_test_file_replace(path, old_data, sizeof(old_data)).status);
  TEST_ASSERT_EQ(k_ra8_test_file_ok,
                 internal_test_file_replace(path, new_data, sizeof(new_data)).status);
  uint8_t                output[8]  = {0xA5U};
  uint8_t                staging[8] = {};
  ra8_test_file_result_t result =
    internal_test_file_read(path, output, sizeof(output), staging, sizeof(staging));
  TEST_ASSERT_EQ(k_ra8_test_file_ok, result.status);
  TEST_ASSERT_EQ(sizeof(new_data), result.transferred);
  TEST_ASSERT_EQ(0, memcmp(output, new_data, sizeof(new_data)));
  memset(output, 0xA5, sizeof(output));
  result = internal_test_file_read(path, output, 2U, staging, sizeof(staging));
  TEST_ASSERT_EQ(k_ra8_test_file_capacity, result.status);
  TEST_ASSERT_EQ(sizeof(new_data), result.required);
  TEST_ASSERT_EQ(2U, result.supplied);
  TEST_ASSERT_EQ(0xA5U, output[0]);
  TEST_ASSERT_EQ(0, symlink(path, link_path));
  TEST_ASSERT_EQ(k_ra8_test_file_nonregular,
                 internal_test_file_replace(link_path, old_data, sizeof(old_data)).status);
  TEST_ASSERT_EQ(0, mkfifo(fifo_path, (mode_t)0600));
  TEST_ASSERT_EQ(
    k_ra8_test_file_nonregular,
    internal_test_file_read(fifo_path, output, sizeof(output), staging, sizeof(staging)).status);
  char temporary[k_ra8_test_file_temp_cap] = {};
  TEST_ASSERT(internal_ra8_test_file_temp_name("fixture.bin", 0U, temporary));
  char temp_path[256] = {};
  TEST_ASSERT(internal_test_join(temp_path, sizeof(temp_path), directory, temporary));
  TEST_ASSERT_EQ(-1, access(temp_path, F_OK));
  TEST_ASSERT_EQ(ENOENT, errno);
  TEST_ASSERT_EQ(0, unlink(fifo_path));
  TEST_ASSERT_EQ(0, unlink(link_path));
  TEST_ASSERT_EQ(0, unlink(path));
  TEST_ASSERT_EQ(0, rmdir(directory));
}

/** @test Real temp collision and symlink-parent cases preserve unrelated nodes.
 * @brief Test real posix collision and parent.
 * @details Performs one bounded, deterministic operation for this host test.
 * @pre Pointer arguments, when present, address their documented test storage.
 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post The helper completes only the bounded test operation described above.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
 * @since 0.1.0
*/
RA8_INTERNAL static void internal_test_real_posix_collision_and_parent(void)
{
  char directory[] = "/tmp/ra8-test-file-more.XXXXXX";
  TEST_ASSERT_NOT_NULL(mkdtemp(directory));
  char path[256] = {};
  TEST_ASSERT(internal_test_join(path, sizeof(path), directory, "target"));
  const uint8_t old_data[] = {1U};
  const uint8_t new_data[] = {2U, 3U};
  TEST_ASSERT_EQ(k_ra8_test_file_ok,
                 internal_test_file_replace(path, old_data, sizeof(old_data)).status);
  char candidate[k_ra8_test_file_temp_cap] = {};
  TEST_ASSERT(internal_ra8_test_file_temp_name("target", 0U, candidate));
  char candidate_path[256] = {};
  TEST_ASSERT(internal_test_join(candidate_path, sizeof(candidate_path), directory, candidate));
  const int collision_fd =
    open(candidate_path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, (mode_t)0600);
  TEST_ASSERT(collision_fd >= 0);
  TEST_ASSERT_EQ(0, close(collision_fd));
  TEST_ASSERT_EQ(k_ra8_test_file_ok,
                 internal_test_file_replace(path, new_data, sizeof(new_data)).status);
  TEST_ASSERT_EQ(0, access(candidate_path, F_OK));
  TEST_ASSERT(internal_ra8_test_file_temp_name("target", 1U, candidate));
  char unused_path[256] = {};
  TEST_ASSERT(internal_test_join(unused_path, sizeof(unused_path), directory, candidate));
  TEST_ASSERT_EQ(-1, access(unused_path, F_OK));
  TEST_ASSERT_EQ(ENOENT, errno);

  char real_parent[256] = {};
  char link_parent[256] = {};
  TEST_ASSERT(internal_test_join(real_parent, sizeof(real_parent), directory, "real"));
  TEST_ASSERT(internal_test_join(link_parent, sizeof(link_parent), directory, "link"));
  TEST_ASSERT_EQ(0, mkdir(real_parent, (mode_t)0700));
  TEST_ASSERT_EQ(0, symlink(real_parent, link_parent));
  char through_link[256] = {};
  TEST_ASSERT(internal_test_join(through_link, sizeof(through_link), link_parent, "blocked"));
  const ra8_test_file_result_t result =
    internal_test_file_replace(through_link, new_data, sizeof(new_data));
  TEST_ASSERT_EQ(k_ra8_test_file_error, result.status);
  char blocked[256] = {};
  TEST_ASSERT(internal_test_join(blocked, sizeof(blocked), real_parent, "blocked"));
  TEST_ASSERT_EQ(-1, access(blocked, F_OK));
  TEST_ASSERT_EQ(0, unlink(link_parent));
  TEST_ASSERT_EQ(0, rmdir(real_parent));
  TEST_ASSERT_EQ(0, unlink(candidate_path));
  TEST_ASSERT_EQ(0, unlink(path));
  TEST_ASSERT_EQ(0, rmdir(directory));
}

/** @brief Run the bounded fixture helper proof suite. */
int main(void)
{
  internal_test_read_success_and_bindings();
  internal_test_read_failures_preserve_destination();
  internal_test_read_mutation_and_close();
  internal_test_replace_success();
  internal_test_replace_prepublish_failures();
  internal_test_replace_operation_faults();
  internal_test_replace_publication_truth();
  internal_test_real_posix_parity();
  internal_test_real_posix_collision_and_parent();
  TEST_ASSERT_EQ(k_ra8_test_output_ok,
                 internal_test_output_fd_text(STDERR_FILENO, "[OK ] test_ra8_test_file.c\n"));
  return 0;
}
