/**
 * @file ra8_test_file.h
 * @brief Bounded raw-descriptor fixture I/O for host tests
 * @details Reads through caller-owned staging so failures preserve the
 * destination, and replaces optional dump files through a same-directory
 * transaction. The syscall table exists only for deterministic fault tests;
 * ordinary callers additionally include `ra8_test_file_posix.h` for the raw
 * POSIX adapters (not pulled in automatically, so this header stays a
 * one-directional dependency for the standalone-header build).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "ra8_attributes.h"

/** @brief Fixture path, retry, and transaction bounds. */
typedef enum : size_t {
  k_ra8_test_file_path_cap      = 4096U, /**< Maximum path including NUL.           */
  k_ra8_test_file_leaf_cap      = 240U,  /**< Maximum final-component bytes.        */
  k_ra8_test_file_temp_cap      = 256U,  /**< Maximum temporary leaf including NUL. */
  k_ra8_test_file_retry_limit   = 16U,   /**< EINTR retries per operation.          */
  k_ra8_test_file_temp_attempts = 16U,   /**< Candidate temporary names.            */
} ra8_test_file_limit_t;

/** @brief Result classes for bounded fixture operations. */
typedef enum : uint8_t {
  k_ra8_test_file_ok = 0U,    /**< The complete operation succeeded.         */
  k_ra8_test_file_invalid,    /**< An argument, bound, or callback failed.   */
  k_ra8_test_file_capacity,   /**< Caller storage was smaller than required. */
  k_ra8_test_file_nonregular, /**< The opened or destination node was special.
                               */
  k_ra8_test_file_short,      /**< Positioned I/O stopped making progress. */
  k_ra8_test_file_changed,    /**< Input identity or metadata changed.     */
  k_ra8_test_file_collision,  /**< All bounded temporary names existed.    */
  k_ra8_test_file_error,      /**< A host operation reported an error.     */
} ra8_test_file_status_t;

/** @brief Complete result of a fixture read or replacement. */
typedef struct {
  ra8_test_file_status_t status;        /**< Primary completion class.              */
  size_t                 required;      /**< Exact bytes required by the file.      */
  size_t                 supplied;      /**< Effective caller capacity.             */
  size_t                 transferred;   /**< Committed bytes; zero on read failure. */
  int                    os_error;      /**< Primary host error, or zero.           */
  int                    cleanup_error; /**< First cleanup/close error, or zero.    */
  bool                   published;     /**< Replacement became visible.            */
} ra8_test_file_result_t;

/** @brief Caller-injected raw host operations used by fixture I/O. */
typedef struct {
  void* context;                                                             /**< Fault context. */
  int (*open_path)(void* context, const char* path, int flags, mode_t mode); /**< `open`.        */
  int (*open_at)(void*       context,
                 int         dir_fd,
                 const char* path,
                 int         flags,
                 mode_t      mode);                                   /**< `openat`. */
  int (*stat_fd)(void* context, int descriptor, struct stat* status); /**< `fstat`.  */
  int (*stat_at)(void*        context,
                 int          dir_fd,
                 const char*  path,
                 struct stat* status,
                 int          flags); /**< `fstatat`. */
  ssize_t (*read_at)(void*  context,
                     int    descriptor,
                     void*  destination,
                     size_t length,
                     off_t  offset); /**< `pread`. */
  ssize_t (*write_at)(void*       context,
                      int         descriptor,
                      const void* source,
                      size_t      length,
                      off_t       offset);       /**< `pwrite`. */
  int (*sync_fd)(void* context, int descriptor); /**< `fsync`.  */
  int (*rename_at)(void*       context,
                   int         old_dir_fd,
                   const char* old_path,
                   int         new_dir_fd,
                   const char* new_path);                                   /**< `renameat`. */
  int (*unlink_at)(void* context, int dir_fd, const char* path, int flags); /**< `unlinkat`. */
  int (*close_fd)(void* context, int descriptor);                           /**< `close`.    */
} ra8_test_file_ops_t;

/** @brief Return the smaller of two capacities.
 * @details Performs one bounded, deterministic operation for this host test.
 * @param[in] left Argument for the bounded test operation.
 * @param[in] right Argument for the bounded test operation.
 * @return Function-specific result consumed by the calling test.
 * @retval 0 Zero or false result; nonzero values describe the alternate result.
 * @pre Pointer arguments, when present, address their documented test storage.
 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post The helper completes only the bounded test operation described above.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
 * @since 0.1.0
*/
RA8_INTERNAL static inline size_t internal_ra8_test_file_min(size_t left, size_t right)
{
  return (left < right) ? left : right;
}

/** @brief Validate and measure one bounded nonempty path.
 * @details Performs one bounded, deterministic operation for this host test.
 * @param[in] path Argument for the bounded test operation.
 * @param[in,out] length Argument for the bounded test operation.
 * @return Function-specific result consumed by the calling test.
 * @retval 0 Zero or false result; nonzero values describe the alternate result.
 * @pre Pointer arguments, when present, address their documented test storage.
 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post The helper completes only the bounded test operation described above.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
 * @since 0.1.0
*/
RA8_INTERNAL static inline bool internal_ra8_test_file_path_length(const char* path, size_t* length)
{
  if ((path == nullptr) || (length == nullptr)) {
    return false;
  }
  size_t count = 0U;
  while ((count < (size_t)k_ra8_test_file_path_cap) && (path[count] != '\0')) {
    ++count;
  }
  if ((count == 0U) || (count == (size_t)k_ra8_test_file_path_cap)) {
    return false;
  }
  *length = count;
  return true;
}

/** @brief Return whether all callbacks required by fixture operations exist.
 * @details Performs one bounded, deterministic operation for this host test.
 * @param[in] ops Argument for the bounded test operation.
 * @return Function-specific result consumed by the calling test.
 * @retval 0 Zero or false result; nonzero values describe the alternate result.
 * @pre Pointer arguments, when present, address their documented test storage.
 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post The helper completes only the bounded test operation described above.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
 * @since 0.1.0
*/
RA8_INTERNAL static inline bool internal_ra8_test_file_ops_valid(const ra8_test_file_ops_t* ops)
{
  return (ops != nullptr) && (ops->open_path != nullptr) && (ops->open_at != nullptr) &&
         (ops->stat_fd != nullptr) && (ops->stat_at != nullptr) && (ops->read_at != nullptr) &&
         (ops->write_at != nullptr) && (ops->sync_fd != nullptr) && (ops->rename_at != nullptr) &&
         (ops->unlink_at != nullptr) && (ops->close_fd != nullptr);
}

/** @brief Invoke open with at most sixteen interrupted retries.
 * @details Performs one bounded, deterministic operation for this host test.
 * @param[in] ops Argument for the bounded test operation.
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
RA8_INTERNAL static inline int internal_ra8_test_file_open(const ra8_test_file_ops_t* ops,
                                                           const char*                path,
                                                           int                        flags,
                                                           mode_t                     mode)
{
  size_t retries = 0U;
  int    result  = -1;
  do {
    result = ops->open_path(ops->context, path, flags, mode);
  } while ((result < 0) && (errno == EINTR) && (++retries <= k_ra8_test_file_retry_limit));
  return result;
}

/** @brief Invoke openat with at most sixteen interrupted retries.
 * @details Performs one bounded, deterministic operation for this host test.
 * @param[in] ops Argument for the bounded test operation.
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
RA8_INTERNAL static inline int internal_ra8_test_file_open_at(const ra8_test_file_ops_t* ops,
                                                              int                        dir_fd,
                                                              const char*                path,
                                                              int                        flags,
                                                              mode_t                     mode)
{
  size_t retries = 0U;
  int    result  = -1;
  do {
    result = ops->open_at(ops->context, dir_fd, path, flags, mode);
  } while ((result < 0) && (errno == EINTR) && (++retries <= k_ra8_test_file_retry_limit));
  return result;
}

/** @brief Invoke fstat with at most sixteen interrupted retries.
 * @details Performs one bounded, deterministic operation for this host test.
 * @param[in] ops Argument for the bounded test operation.
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
RA8_INTERNAL static inline int
internal_ra8_test_file_stat(const ra8_test_file_ops_t* ops, int descriptor, struct stat* status)
{
  size_t retries = 0U;
  int    result  = -1;
  do {
    result = ops->stat_fd(ops->context, descriptor, status);
  } while ((result < 0) && (errno == EINTR) && (++retries <= k_ra8_test_file_retry_limit));
  return result;
}

/** @brief Invoke fstatat with at most sixteen interrupted retries.
 * @details Performs one bounded, deterministic operation for this host test.
 * @param[in] ops Argument for the bounded test operation.
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
RA8_INTERNAL static inline int internal_ra8_test_file_stat_at(const ra8_test_file_ops_t* ops,
                                                              int                        dir_fd,
                                                              const char*                path,
                                                              struct stat*               status,
                                                              int                        flags)
{
  size_t retries = 0U;
  int    result  = -1;
  do {
    result = ops->stat_at(ops->context, dir_fd, path, status, flags);
  } while ((result < 0) && (errno == EINTR) && (++retries <= k_ra8_test_file_retry_limit));
  return result;
}

/** @brief Invoke one positioned read with bounded interrupted retries.
 * @details Performs one bounded, deterministic operation for this host test.
 * @param[in] ops Argument for the bounded test operation.
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
RA8_INTERNAL static inline ssize_t internal_ra8_test_file_pread(const ra8_test_file_ops_t* ops,
                                                                int    descriptor,
                                                                void*  destination,
                                                                size_t length,
                                                                off_t  offset)
{
  size_t  retries = 0U;
  ssize_t result  = -1;
  do {
    result = ops->read_at(ops->context, descriptor, destination, length, offset);
  } while ((result < 0) && (errno == EINTR) && (++retries <= k_ra8_test_file_retry_limit));
  return result;
}

/** @brief Invoke one positioned write with bounded interrupted retries.
 * @details Performs one bounded, deterministic operation for this host test.
 * @param[in] ops Argument for the bounded test operation.
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
RA8_INTERNAL static inline ssize_t internal_ra8_test_file_pwrite(const ra8_test_file_ops_t* ops,
                                                                 int         descriptor,
                                                                 const void* source,
                                                                 size_t      length,
                                                                 off_t       offset)
{
  size_t  retries = 0U;
  ssize_t result  = -1;
  do {
    result = ops->write_at(ops->context, descriptor, source, length, offset);
  } while ((result < 0) && (errno == EINTR) && (++retries <= k_ra8_test_file_retry_limit));
  return result;
}

/** @brief Invoke fsync with at most sixteen interrupted retries.
 * @details Performs one bounded, deterministic operation for this host test.
 * @param[in] ops Argument for the bounded test operation.
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
RA8_INTERNAL static inline int internal_ra8_test_file_sync(const ra8_test_file_ops_t* ops,
                                                           int                        descriptor)
{
  size_t retries = 0U;
  int    result  = -1;
  do {
    result = ops->sync_fd(ops->context, descriptor);
  } while ((result < 0) && (errno == EINTR) && (++retries <= k_ra8_test_file_retry_limit));
  return result;
}

/** @brief Invoke renameat with at most sixteen interrupted retries.
 * @details Performs one bounded, deterministic operation for this host test.
 * @param[in] ops Argument for the bounded test operation.
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
RA8_INTERNAL static inline int internal_ra8_test_file_rename(const ra8_test_file_ops_t* ops,
                                                             int                        old_dir_fd,
                                                             const char*                old_path,
                                                             int                        new_dir_fd,
                                                             const char*                new_path)
{
  size_t retries = 0U;
  int    result  = -1;
  do {
    result = ops->rename_at(ops->context, old_dir_fd, old_path, new_dir_fd, new_path);
  } while ((result < 0) && (errno == EINTR) && (++retries <= k_ra8_test_file_retry_limit));
  return result;
}

/** @brief Invoke unlinkat with at most sixteen interrupted retries.
 * @details Performs one bounded, deterministic operation for this host test.
 * @param[in] ops Argument for the bounded test operation.
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
RA8_INTERNAL static inline int internal_ra8_test_file_unlink(const ra8_test_file_ops_t* ops,
                                                             int                        dir_fd,
                                                             const char*                path,
                                                             int                        flags)
{
  size_t retries = 0U;
  int    result  = -1;
  do {
    result = ops->unlink_at(ops->context, dir_fd, path, flags);
  } while ((result < 0) && (errno == EINTR) && (++retries <= k_ra8_test_file_retry_limit));
  return result;
}

/** @brief Compare immutable identity and mutation-sensitive timestamps.
 * @details Performs one bounded, deterministic operation for this host test.
 * @param[in] before Argument for the bounded test operation.
 * @param[in] after Argument for the bounded test operation.
 * @return Function-specific result consumed by the calling test.
 * @retval 0 Zero or false result; nonzero values describe the alternate result.
 * @pre Pointer arguments, when present, address their documented test storage.
 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post The helper completes only the bounded test operation described above.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
 * @since 0.1.0
*/
RA8_INTERNAL static inline bool internal_ra8_test_file_same_stat(const struct stat* before,
                                                                 const struct stat* after)
{
#if defined(__APPLE__)
  const bool same_times = (before->st_mtimespec.tv_sec == after->st_mtimespec.tv_sec) &&
                          (before->st_mtimespec.tv_nsec == after->st_mtimespec.tv_nsec) &&
                          (before->st_ctimespec.tv_sec == after->st_ctimespec.tv_sec) &&
                          (before->st_ctimespec.tv_nsec == after->st_ctimespec.tv_nsec);
#else
  const bool same_times = (before->st_mtim.tv_sec == after->st_mtim.tv_sec) &&
                          (before->st_mtim.tv_nsec == after->st_mtim.tv_nsec) &&
                          (before->st_ctim.tv_sec == after->st_ctim.tv_sec) &&
                          (before->st_ctim.tv_nsec == after->st_ctim.tv_nsec);
#endif
  return (before->st_dev == after->st_dev) && (before->st_ino == after->st_ino) &&
         (before->st_mode == after->st_mode) && (before->st_size == after->st_size) && same_times;
}

/** @brief Record a primary failure without losing an earlier one.
 * @details Performs one bounded, deterministic operation for this host test.
 * @param[in,out] result Argument for the bounded test operation.
 * @param[in] status Argument for the bounded test operation.
 * @param[in] os_error Argument for the bounded test operation.
 * @pre Pointer arguments, when present, address their documented test storage.
 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post The helper completes only the bounded test operation described above.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
 * @since 0.1.0
*/
RA8_INTERNAL static inline void internal_ra8_test_file_fail(ra8_test_file_result_t* result,
                                                            ra8_test_file_status_t  status,
                                                            int                     os_error)
{
  if ((result != nullptr) && (result->status == k_ra8_test_file_ok)) {
    result->status   = status;
    result->os_error = os_error;
  }
}

/** @brief Close once and preserve primary-versus-cleanup failure precedence.
 * @details Performs one bounded, deterministic operation for this host test.
 * @param[in] ops Argument for the bounded test operation.
 * @param[in] descriptor Argument for the bounded test operation.
 * @param[in,out] result Argument for the bounded test operation.
 * @pre Pointer arguments, when present, address their documented test storage.
 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post The helper completes only the bounded test operation described above.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
 * @since 0.1.0
*/
RA8_INTERNAL static inline void internal_ra8_test_file_close(const ra8_test_file_ops_t* ops,
                                                             int                        descriptor,
                                                             ra8_test_file_result_t*    result)
{
  if ((descriptor >= 0) && (ops->close_fd(ops->context, descriptor) != 0)) {
    if (result->status == k_ra8_test_file_ok) {
      result->status   = k_ra8_test_file_error;
      result->os_error = errno;
    } else if (result->cleanup_error == 0) {
      result->cleanup_error = errno;
    }
  }
}

/** @brief Verify two nonoverlapping caller spans for a committed read.
 * @details Performs one bounded, deterministic operation for this host test.
 * @param[in] destination Argument for the bounded test operation.
 * @param[in] staging Argument for the bounded test operation.
 * @param[in] length Argument for the bounded test operation.
 * @return Function-specific result consumed by the calling test.
 * @retval 0 Zero or false result; nonzero values describe the alternate result.
 * @pre Pointer arguments, when present, address their documented test storage.
 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post The helper completes only the bounded test operation described above.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
 * @since 0.1.0
*/
RA8_INTERNAL static inline bool internal_ra8_test_file_spans_valid(const uint8_t* destination,
                                                                   const uint8_t* staging,
                                                                   size_t         length)
{
  if (length == 0U) {
    return true;
  }
  if ((destination == nullptr) || (staging == nullptr)) {
    return false;
  }
  const uintptr_t dst = (uintptr_t)destination;
  const uintptr_t tmp = (uintptr_t)staging;
  if ((length > (UINTPTR_MAX - dst)) || (length > (UINTPTR_MAX - tmp))) {
    return false;
  }
  return ((dst + length) <= tmp) || ((tmp + length) <= dst);
}

/** @brief Read the exact staged byte count or classify the first failure.
 * @details Performs one bounded, deterministic operation for this host test.
 * @param[in] ops Argument for the bounded test operation.
 * @param[in] descriptor Argument for the bounded test operation.
 * @param[in,out] staging Argument for the bounded test operation.
 * @param[in] length Argument for the bounded test operation.
 * @param[in,out] result Argument for the bounded test operation.
 * @pre Pointer arguments, when present, address their documented test storage.
 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post The helper completes only the bounded test operation described above.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
 * @since 0.1.0
*/
RA8_INTERNAL static inline void internal_ra8_test_file_read_exact(const ra8_test_file_ops_t* ops,
                                                                  int      descriptor,
                                                                  uint8_t* staging,
                                                                  size_t   length,
                                                                  ra8_test_file_result_t* result)
{
  size_t offset = 0U;
  while ((offset < length) && (result->status == k_ra8_test_file_ok)) {
    const size_t  remaining = length - offset;
    const ssize_t got =
      internal_ra8_test_file_pread(ops, descriptor, &staging[offset], remaining, (off_t)offset);
    if (got < 0) {
      internal_ra8_test_file_fail(result, k_ra8_test_file_error, errno);
    } else if (got == 0) {
      internal_ra8_test_file_fail(result, k_ra8_test_file_short, 0);
    } else if ((size_t)got > remaining) {
      internal_ra8_test_file_fail(result, k_ra8_test_file_invalid, 0);
    } else {
      offset += (size_t)got;
    }
  }
}

/** @brief Probe exactly at the stated EOF to detect growth during a read.
 * @details Performs one bounded, deterministic operation for this host test.
 * @param[in] ops Argument for the bounded test operation.
 * @param[in] descriptor Argument for the bounded test operation.
 * @param[in] length Argument for the bounded test operation.
 * @param[in,out] result Argument for the bounded test operation.
 * @pre Pointer arguments, when present, address their documented test storage.
 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post The helper completes only the bounded test operation described above.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
 * @since 0.1.0
*/
RA8_INTERNAL static inline void internal_ra8_test_file_probe_eof(const ra8_test_file_ops_t* ops,
                                                                 int                     descriptor,
                                                                 size_t                  length,
                                                                 ra8_test_file_result_t* result)
{
  uint8_t       extra = 0U;
  const ssize_t got   = internal_ra8_test_file_pread(ops, descriptor, &extra, 1U, (off_t)length);
  if (got < 0) {
    internal_ra8_test_file_fail(result, k_ra8_test_file_error, errno);
  } else if (got != 0) {
    internal_ra8_test_file_fail(result, k_ra8_test_file_changed, 0);
  }
}

/** @brief Re-stat an input and override a provisional capacity on mutation.
 * @details Performs one bounded, deterministic operation for this host test.
 * @param[in] ops Argument for the bounded test operation.
 * @param[in] descriptor Argument for the bounded test operation.
 * @param[in] before Argument for the bounded test operation.
 * @param[in,out] result Argument for the bounded test operation.
 * @pre Pointer arguments, when present, address their documented test storage.
 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post The helper completes only the bounded test operation described above.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
 * @since 0.1.0
*/
RA8_INTERNAL static inline void
internal_ra8_test_file_verify_identity(const ra8_test_file_ops_t* ops,
                                       int                        descriptor,
                                       const struct stat*         before,
                                       ra8_test_file_result_t*    result)
{
  struct stat after = {};
  if (internal_ra8_test_file_stat(ops, descriptor, &after) != 0) {
    result->status   = k_ra8_test_file_error;
    result->os_error = errno;
  } else if (!internal_ra8_test_file_same_stat(before, &after)) {
    result->status   = k_ra8_test_file_changed;
    result->os_error = 0;
  }
}

/**
 * @brief Stat, validate, read, and verify an already-open regular fixture.
 * @details Stats the descriptor and rejects a non-regular or oversized file,
 * rejects a required size beyond the caller's supplied capacity or an
 * overlapping destination/staging span, reads exactly `result->required`
 * bytes into @p staging, probes for a stable EOF, and re-stats to detect a
 * concurrent mutation. Every stage after the first failure is a no-op.
 * @param[in] ops Complete caller-owned operation table.
 * @param[in] descriptor Already-open descriptor being validated and read.
 * @param[in] destination Destination span, used only for the overlap check.
 * @param[in,out] staging Distinct caller-owned read staging area.
 * @param[in,out] result In: `status == k_ra8_test_file_ok` and `supplied`
 * capacity. Out: `required`, staged bytes in @p staging, and terminal status.
 * @return Nothing; every outcome is reported through @p result.
 * @pre @p descriptor is open and owned by this call.
 * @pre @p result->status is `k_ra8_test_file_ok` on entry.
 * @post @p result->status names the first failing stage, if any.
 * @post Success leaves the read bytes staged in @p staging, not yet committed
 * to @p destination.
 * @note Test-only helper with no production ABI.
 * @since 0.1.0
 */
RA8_INTERNAL static inline void internal_test_file_validate_and_read(const ra8_test_file_ops_t* ops,
                                                                     int            descriptor,
                                                                     const uint8_t* destination,
                                                                     uint8_t*       staging,
                                                                     ra8_test_file_result_t* result)
{
  struct stat before = {};
  if (internal_ra8_test_file_stat(ops, descriptor, &before) != 0) {
    internal_ra8_test_file_fail(result, k_ra8_test_file_error, errno);
  } else if (!S_ISREG(before.st_mode)) {
    internal_ra8_test_file_fail(result, k_ra8_test_file_nonregular, 0);
  } else if ((before.st_size < 0) || ((uintmax_t)before.st_size > (uintmax_t)SIZE_MAX)) {
    internal_ra8_test_file_fail(result, k_ra8_test_file_invalid, 0);
  } else {
    result->required = (size_t)before.st_size;
  }
  if ((result->status == k_ra8_test_file_ok) && (result->required > result->supplied)) {
    internal_ra8_test_file_fail(result, k_ra8_test_file_capacity, 0);
  }
  if ((result->status == k_ra8_test_file_ok) &&
      !internal_ra8_test_file_spans_valid(destination, staging, result->required)) {
    internal_ra8_test_file_fail(result, k_ra8_test_file_invalid, 0);
  }
  if (result->status == k_ra8_test_file_ok) {
    internal_ra8_test_file_read_exact(ops, descriptor, staging, result->required, result);
  }
  if (result->status == k_ra8_test_file_ok) {
    internal_ra8_test_file_probe_eof(ops, descriptor, result->required, result);
  }
  if ((result->status == k_ra8_test_file_ok) || (result->status == k_ra8_test_file_capacity)) {
    internal_ra8_test_file_verify_identity(ops, descriptor, &before, result);
  }
}

RA8_INTERNAL static inline ra8_test_file_result_t
internal_test_file_read_with_ops(const ra8_test_file_ops_t* ops,
                                 const char*                path,
                                 uint8_t*                   destination,
                                 size_t                     destination_capacity,
                                 uint8_t*                   staging,
                                 size_t                     staging_capacity)
{
  ra8_test_file_result_t result = {
    .status   = k_ra8_test_file_invalid,
    .supplied = internal_ra8_test_file_min(destination_capacity, staging_capacity)};
  size_t path_length = 0U;
  if (!internal_ra8_test_file_ops_valid(ops) ||
      !internal_ra8_test_file_path_length(path, &path_length)) {
    return result;
  }
  (void)path_length;
  result.status        = k_ra8_test_file_ok;
  const int descriptor = internal_ra8_test_file_open(ops,
                                                     path,
                                                     O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK,
                                                     (mode_t)0);
  if (descriptor < 0) {
    internal_ra8_test_file_fail(&result, k_ra8_test_file_error, errno);
    return result;
  }
  internal_test_file_validate_and_read(ops, descriptor, destination, staging, &result);
  internal_ra8_test_file_close(ops, descriptor, &result);
  if ((result.status == k_ra8_test_file_ok) && (result.required != 0U) &&
      (destination == nullptr)) {
    /* internal_ra8_test_file_spans_valid() already rejects a null
     * destination whenever required > 0, so this arm is unreachable in
     * practice -- but the static analyzer cannot see that guarantee across
     * the internal_test_file_validate_and_read() call boundary, and a
     * belt-and-suspenders check costs nothing on a path that is exercised
     * only by test fixtures. */
    internal_ra8_test_file_fail(&result, k_ra8_test_file_invalid, 0);
  }
  if (result.status == k_ra8_test_file_ok) {
    if (result.required != 0U) {
      memcpy(destination, staging, result.required);
    }
    result.transferred = result.required;
  }
  return result;
}
