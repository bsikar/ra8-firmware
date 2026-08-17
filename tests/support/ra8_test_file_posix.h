/**
 * @file ra8_test_file_posix.h
 * @brief POSIX adapters and transactional replacement for test fixtures
 * @details Extends the injected fixture core with raw-descriptor adapters and
 * same-directory publication. Self-contained: includes `ra8_test_file.h`
 * itself, so a caller that only needs the raw POSIX adapters may include
 * this header directly instead of the core header.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include "ra8_test_file.h"

/* Some POSIX libraries declare renameat only through stdio.h. This
 * raw-descriptor-only helper does not otherwise need or admit that surface. */
/**
 * @brief Rename one directory-relative path.
 * @details Declares the raw POSIX function without importing C stream types.
 * @param[in] old_dir_fd Source parent descriptor.
 * @param[in] old_path Source leaf name.
 * @param[in] new_dir_fd Destination parent descriptor.
 * @param[in] new_path Destination leaf name.
 * @return Zero on success or a negative host failure result.
 * @retval 0 The path was renamed.
 * @pre Both descriptors name open directories.
 * @pre Both paths are non-NULL bounded leaf names.
 * @post Success publishes the destination name atomically.
 * @post Failure leaves publication state defined by the host result.
 * @note This declaration avoids the unrelated C stream surface.
 * @since 0.1.0
 */
extern int renameat(int old_dir_fd, const char* old_path, int new_dir_fd, const char* new_path);

/** @brief Split one host path into bounded parent and final component.
 * @details Performs one bounded, deterministic operation for this host test.
 * @param[in] path Argument for the bounded test operation.
 * @param[in,out] parent Argument for the bounded test operation.
 * @param[in,out] leaf Argument for the bounded test operation.
 * @return Function-specific result consumed by the calling test.
 * @retval 0 Zero or false result; nonzero values describe the alternate result.
 * @pre Pointer arguments, when present, address their documented test storage.
 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post The helper completes only the bounded test operation described above.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
 * @since 0.1.0
*/
RA8_INTERNAL static inline bool
internal_ra8_test_file_split(const char* path, char* parent, char* leaf)
{
  size_t length = 0U;
  if (!internal_ra8_test_file_path_length(path, &length) || (parent == nullptr) ||
      (leaf == nullptr) || (path[length - 1U] == '/')) {
    return false;
  }
  size_t slash = length;
  while ((slash > 0U) && (path[slash - 1U] != '/')) {
    --slash;
  }
  const size_t leaf_length = length - slash;
  if ((leaf_length == 0U) || (leaf_length >= (size_t)k_ra8_test_file_leaf_cap)) {
    return false;
  }
  memcpy(leaf, &path[slash], leaf_length);
  leaf[leaf_length] = '\0';
  if ((strcmp(leaf, ".") == 0) || (strcmp(leaf, "..") == 0)) {
    return false;
  }
  size_t parent_length;
  if (slash <= 1U) {
    parent_length = 1U;
  } else {
    parent_length = slash - 1U;
  }
  if (slash == 0U) {
    parent[0] = '.';
  } else {
    memcpy(parent, path, parent_length);
  }
  parent[parent_length] = '\0';
  return true;
}

/** @brief Render a deterministic same-directory temporary leaf.
 * @details Performs one bounded, deterministic operation for this host test.
 * @param[in] leaf Argument for the bounded test operation.
 * @param[in] attempt Argument for the bounded test operation.
 * @param[in,out] temporary Argument for the bounded test operation.
 * @return Function-specific result consumed by the calling test.
 * @retval 0 Zero or false result; nonzero values describe the alternate result.
 * @pre Pointer arguments, when present, address their documented test storage.
 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post The helper completes only the bounded test operation described above.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
 * @since 0.1.0
*/
RA8_INTERNAL static inline bool
internal_ra8_test_file_temp_name(const char* leaf, size_t attempt, char* temporary)
{
  const char*  suffix     = ".ra8tmp.";
  const char*  hex        = "0123456789abcdef";
  const size_t leaf_len   = strlen(leaf);
  const size_t suffix_len = strlen(suffix);
  const size_t total      = 1U + leaf_len + suffix_len + 1U;
  if ((attempt >= k_ra8_test_file_temp_attempts) || (total >= (size_t)k_ra8_test_file_temp_cap)) {
    return false;
  }
  temporary[0] = '.';
  memcpy(&temporary[1], leaf, leaf_len);
  memcpy(&temporary[1U + leaf_len], suffix, suffix_len);
  temporary[1U + leaf_len + suffix_len] = hex[attempt & 0x0FU];
  temporary[total]                      = '\0';
  return true;
}

/** @brief Inspect an existing destination without following it.
 * @details Performs one bounded, deterministic operation for this host test.
 * @param[in] ops Argument for the bounded test operation.
 * @param[in] parent_fd Argument for the bounded test operation.
 * @param[in] leaf Argument for the bounded test operation.
 * @param[in,out] result Argument for the bounded test operation.
 * @pre Pointer arguments, when present, address their documented test storage.
 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post The helper completes only the bounded test operation described above.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
 * @since 0.1.0
*/
RA8_INTERNAL static inline void
internal_ra8_test_file_inspect_destination(const ra8_test_file_ops_t* ops,
                                           int                        parent_fd,
                                           const char*                leaf,
                                           ra8_test_file_result_t*    result)
{
  struct stat destination = {};
  if (internal_ra8_test_file_stat_at(ops, parent_fd, leaf, &destination, AT_SYMLINK_NOFOLLOW) ==
      0) {
    if (!S_ISREG(destination.st_mode)) {
      internal_ra8_test_file_fail(result, k_ra8_test_file_nonregular, 0);
    }
  } else if (errno != ENOENT) {
    internal_ra8_test_file_fail(result, k_ra8_test_file_error, errno);
  }
}

/** @brief Create one of the bounded sibling temporary candidates.
 * @details Performs one bounded, deterministic operation for this host test.
 * @param[in] ops Argument for the bounded test operation.
 * @param[in] parent_fd Argument for the bounded test operation.
 * @param[in] leaf Argument for the bounded test operation.
 * @param[in,out] temporary Argument for the bounded test operation.
 * @param[in,out] result Argument for the bounded test operation.
 * @return Function-specific result consumed by the calling test.
 * @retval 0 Zero or false result; nonzero values describe the alternate result.
 * @pre Pointer arguments, when present, address their documented test storage.
 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post The helper completes only the bounded test operation described above.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
 * @since 0.1.0
*/
RA8_INTERNAL static inline int internal_ra8_test_file_create_temp(const ra8_test_file_ops_t* ops,
                                                                  int                     parent_fd,
                                                                  const char*             leaf,
                                                                  char*                   temporary,
                                                                  ra8_test_file_result_t* result)
{
  for (size_t attempt = 0U; attempt < k_ra8_test_file_temp_attempts; ++attempt) {
    if (!internal_ra8_test_file_temp_name(leaf, attempt, temporary)) {
      break;
    }
    const int descriptor = internal_ra8_test_file_open_at(ops,
                                                          parent_fd,
                                                          temporary,
                                                          O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC |
                                                            O_NOFOLLOW | O_NONBLOCK,
                                                          (mode_t)0600);
    if (descriptor >= 0) {
      return descriptor;
    }
    if (errno != EEXIST) {
      internal_ra8_test_file_fail(result, k_ra8_test_file_error, errno);
      return -1;
    }
  }
  internal_ra8_test_file_fail(result, k_ra8_test_file_collision, EEXIST);
  return -1;
}

/** @brief Write the exact source bytes to a staged descriptor.
 * @details Performs one bounded, deterministic operation for this host test.
 * @param[in] ops Argument for the bounded test operation.
 * @param[in] descriptor Argument for the bounded test operation.
 * @param[in] source Argument for the bounded test operation.
 * @param[in] length Argument for the bounded test operation.
 * @param[in,out] result Argument for the bounded test operation.
 * @pre Pointer arguments, when present, address their documented test storage.
 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post The helper completes only the bounded test operation described above.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
 * @since 0.1.0
*/
RA8_INTERNAL static inline void internal_ra8_test_file_write_exact(const ra8_test_file_ops_t* ops,
                                                                   int            descriptor,
                                                                   const uint8_t* source,
                                                                   size_t         length,
                                                                   ra8_test_file_result_t* result)
{
  size_t offset = 0U;
  while ((offset < length) && (result->status == k_ra8_test_file_ok)) {
    const size_t  remaining = length - offset;
    const ssize_t wrote =
      internal_ra8_test_file_pwrite(ops, descriptor, &source[offset], remaining, (off_t)offset);
    if (wrote < 0) {
      internal_ra8_test_file_fail(result, k_ra8_test_file_error, errno);
    } else if (wrote == 0) {
      internal_ra8_test_file_fail(result, k_ra8_test_file_short, 0);
    } else if ((size_t)wrote > remaining) {
      internal_ra8_test_file_fail(result, k_ra8_test_file_invalid, 0);
    } else {
      offset += (size_t)wrote;
    }
  }
}

/** @brief Verify staged length, sync contents, and close before publication.
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
RA8_INTERNAL static inline void internal_ra8_test_file_finish_temp(const ra8_test_file_ops_t* ops,
                                                                   int    descriptor,
                                                                   size_t length,
                                                                   ra8_test_file_result_t* result)
{
  struct stat staged = {};
  if ((result->status == k_ra8_test_file_ok) &&
      (internal_ra8_test_file_stat(ops, descriptor, &staged) != 0)) {
    internal_ra8_test_file_fail(result, k_ra8_test_file_error, errno);
  } else if ((result->status == k_ra8_test_file_ok) &&
             (!S_ISREG(staged.st_mode) || (staged.st_size < 0) ||
              ((uintmax_t)staged.st_size != (uintmax_t)length))) {
    internal_ra8_test_file_fail(result, k_ra8_test_file_short, 0);
  }
  if ((result->status == k_ra8_test_file_ok) &&
      (internal_ra8_test_file_sync(ops, descriptor) != 0)) {
    internal_ra8_test_file_fail(result, k_ra8_test_file_error, errno);
  }
  internal_ra8_test_file_close(ops, descriptor, result);
}

/** @brief Remove an unpublished temporary and preserve cleanup errors.
 * @details Performs one bounded, deterministic operation for this host test.
 * @param[in] ops Argument for the bounded test operation.
 * @param[in] parent_fd Argument for the bounded test operation.
 * @param[in] temporary Argument for the bounded test operation.
 * @param[in,out] result Argument for the bounded test operation.
 * @pre Pointer arguments, when present, address their documented test storage.
 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post The helper completes only the bounded test operation described above.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
 * @since 0.1.0
*/
RA8_INTERNAL static inline void internal_ra8_test_file_unlink_temp(const ra8_test_file_ops_t* ops,
                                                                   int         parent_fd,
                                                                   const char* temporary,
                                                                   ra8_test_file_result_t* result)
{
  if ((temporary[0] != '\0') &&
      (internal_ra8_test_file_unlink(ops, parent_fd, temporary, 0) != 0) &&
      (result->cleanup_error == 0)) {
    result->cleanup_error = errno;
  }
}

/**
 * @brief Atomically replace one regular dump through injected raw operations.
 * @param[in] ops Complete caller-owned operation table.
 * @param[in] path Bounded destination path.
 * @param[in] source Readable source bytes.
 * @param[in] length Exact source length.
 * @return Exact result including whether publication occurred.
 * @pre @p source spans @p length bytes when length is nonzero.
 * @post Every prepublication failure preserves the old destination.
 * @post `published` stays true when a post-rename directory sync/close fails.
 * @note Success synchronizes file contents and the parent-directory entry.
 * @since 0.1.0

 * @details Performs one bounded, deterministic operation for this host test.
 * @retval 0 Zero or false result; nonzero values describe the alternate result.
 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
*/
RA8_INTERNAL static inline ra8_test_file_result_t
internal_test_file_replace_with_ops(const ra8_test_file_ops_t* ops,
                                    const char*                path,
                                    const uint8_t*             source,
                                    size_t                     length)
{
  ra8_test_file_result_t result                           = {.status   = k_ra8_test_file_invalid,
                                                             .required = length,
                                                             .supplied = length};
  char                   parent[k_ra8_test_file_path_cap] = {};
  char                   leaf[k_ra8_test_file_leaf_cap]   = {};
  if (!internal_ra8_test_file_ops_valid(ops) || ((source == nullptr) && (length != 0U)) ||
      ((uintmax_t)length > (uintmax_t)INT64_MAX) ||
      !internal_ra8_test_file_split(path, parent, leaf)) {
    return result;
  }
  result.status       = k_ra8_test_file_ok;
  const int parent_fd = internal_ra8_test_file_open(ops,
                                                    parent,
                                                    O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW,
                                                    (mode_t)0);
  if (parent_fd < 0) {
    internal_ra8_test_file_fail(&result, k_ra8_test_file_error, errno);
    return result;
  }
  internal_ra8_test_file_inspect_destination(ops, parent_fd, leaf, &result);
  char temporary[k_ra8_test_file_temp_cap] = {};
  int  staged_fd                           = -1;
  if (result.status == k_ra8_test_file_ok) {
    staged_fd = internal_ra8_test_file_create_temp(ops, parent_fd, leaf, temporary, &result);
  }
  if (staged_fd >= 0) {
    internal_ra8_test_file_write_exact(ops, staged_fd, source, length, &result);
    internal_ra8_test_file_finish_temp(ops, staged_fd, length, &result);
  }
  if (result.status == k_ra8_test_file_ok) {
    if (internal_ra8_test_file_rename(ops, parent_fd, temporary, parent_fd, leaf) != 0) {
      internal_ra8_test_file_fail(&result, k_ra8_test_file_error, errno);
    } else {
      result.published   = true;
      result.transferred = length;
    }
  }
  if (!result.published && (staged_fd >= 0)) {
    internal_ra8_test_file_unlink_temp(ops, parent_fd, temporary, &result);
  } else if (internal_ra8_test_file_sync(ops, parent_fd) != 0) {
    internal_ra8_test_file_fail(&result, k_ra8_test_file_error, errno);
  }
  internal_ra8_test_file_close(ops, parent_fd, &result);
  return result;
}

/** @brief Raw POSIX adapter for open.
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
RA8_INTERNAL static inline int
internal_ra8_test_file_posix_open(void* context, const char* path, int flags, mode_t mode)
{
  (void)context;
  return open(path, flags, mode);
}

/** @brief Raw POSIX adapter for openat.
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
RA8_INTERNAL static inline int internal_ra8_test_file_posix_open_at(void*       context,
                                                                    int         dir_fd,
                                                                    const char* path,
                                                                    int         flags,
                                                                    mode_t      mode)
{
  (void)context;
  return openat(dir_fd, path, flags, mode);
}

/** @brief Raw POSIX adapter for fstat.
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
RA8_INTERNAL static inline int
internal_ra8_test_file_posix_stat(void* context, int descriptor, struct stat* status)
{
  (void)context;
  return fstat(descriptor, status);
}

/** @brief Raw POSIX adapter for fstatat.
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
RA8_INTERNAL static inline int internal_ra8_test_file_posix_stat_at(void*        context,
                                                                    int          dir_fd,
                                                                    const char*  path,
                                                                    struct stat* status,
                                                                    int          flags)
{
  (void)context;
  return fstatat(dir_fd, path, status, flags);
}

/** @brief Raw POSIX adapter for pread.
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
RA8_INTERNAL static inline ssize_t internal_ra8_test_file_posix_pread(void*  context,
                                                                      int    descriptor,
                                                                      void*  destination,
                                                                      size_t length,
                                                                      off_t  offset)
{
  (void)context;
  return pread(descriptor, destination, length, offset);
}

/** @brief Raw POSIX adapter for pwrite.
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
RA8_INTERNAL static inline ssize_t internal_ra8_test_file_posix_pwrite(void*       context,
                                                                       int         descriptor,
                                                                       const void* source,
                                                                       size_t      length,
                                                                       off_t       offset)
{
  (void)context;
  return pwrite(descriptor, source, length, offset);
}

/** @brief Raw POSIX adapter for fsync.
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
RA8_INTERNAL static inline int internal_ra8_test_file_posix_sync(void* context, int descriptor)
{
  (void)context;
  return fsync(descriptor);
}

/** @brief Raw POSIX adapter for renameat.
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
RA8_INTERNAL static inline int internal_ra8_test_file_posix_rename(void*       context,
                                                                   int         old_dir_fd,
                                                                   const char* old_path,
                                                                   int         new_dir_fd,
                                                                   const char* new_path)
{
  (void)context;
  return renameat(old_dir_fd, old_path, new_dir_fd, new_path);
}

/** @brief Raw POSIX adapter for unlinkat.
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
RA8_INTERNAL static inline int
internal_ra8_test_file_posix_unlink(void* context, int dir_fd, const char* path, int flags)
{
  (void)context;
  return unlinkat(dir_fd, path, flags);
}

/** @brief Raw POSIX adapter for close.
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
RA8_INTERNAL static inline int internal_ra8_test_file_posix_close(void* context, int descriptor)
{
  (void)context;
  return close(descriptor);
}

/** @brief Build the complete raw POSIX operation table by value.
 * @details Performs one bounded, deterministic operation for this host test.
 * @return Function-specific result consumed by the calling test.
 * @retval 0 Zero or false result; nonzero values describe the alternate result.
 * @pre Pointer arguments, when present, address their documented test storage.
 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post The helper completes only the bounded test operation described above.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
 * @since 0.1.0
*/
RA8_INTERNAL static inline ra8_test_file_ops_t internal_ra8_test_file_posix_ops(void)
{
  return (ra8_test_file_ops_t){.context   = nullptr,
                               .open_path = internal_ra8_test_file_posix_open,
                               .open_at   = internal_ra8_test_file_posix_open_at,
                               .stat_fd   = internal_ra8_test_file_posix_stat,
                               .stat_at   = internal_ra8_test_file_posix_stat_at,
                               .read_at   = internal_ra8_test_file_posix_pread,
                               .write_at  = internal_ra8_test_file_posix_pwrite,
                               .sync_fd   = internal_ra8_test_file_posix_sync,
                               .rename_at = internal_ra8_test_file_posix_rename,
                               .unlink_at = internal_ra8_test_file_posix_unlink,
                               .close_fd  = internal_ra8_test_file_posix_close};
}

/**
 * @brief Read one regular fixture through raw POSIX descriptors.
 * @param[in] path Bounded host path.
 * @param[out] destination Success-only destination.
 * @param[in] destination_capacity Destination capacity.
 * @param[in,out] staging Distinct caller-owned staging storage.
 * @param[in] staging_capacity Staging capacity.
 * @return Exact bounded read result.
 * @since 0.1.0

 * @details Performs one bounded, deterministic operation for this host test.
 * @retval 0 Zero or false result; nonzero values describe the alternate result.
 * @pre Pointer arguments, when present, address their documented test storage.
 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post The helper completes only the bounded test operation described above.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
*/
RA8_INTERNAL static inline ra8_test_file_result_t
internal_test_file_read(const char* path,
                        uint8_t*    destination,
                        size_t      destination_capacity,
                        uint8_t*    staging,
                        size_t      staging_capacity)
{
  const ra8_test_file_ops_t ops = internal_ra8_test_file_posix_ops();
  return internal_test_file_read_with_ops(&ops,
                                          path,
                                          destination,
                                          destination_capacity,
                                          staging,
                                          staging_capacity);
}

/**
 * @brief Transactionally replace one regular dump through raw descriptors.
 * @param[in] path Bounded destination path.
 * @param[in] source Readable source bytes.
 * @param[in] length Exact source length.
 * @return Exact replacement result and truthful publication flag.
 * @since 0.1.0

 * @details Performs one bounded, deterministic operation for this host test.
 * @retval 0 Zero or false result; nonzero values describe the alternate result.
 * @pre Pointer arguments, when present, address their documented test storage.
 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post The helper completes only the bounded test operation described above.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
*/
RA8_INTERNAL static inline ra8_test_file_result_t
internal_test_file_replace(const char* path, const uint8_t* source, size_t length)
{
  const ra8_test_file_ops_t ops = internal_ra8_test_file_posix_ops();
  return internal_test_file_replace_with_ops(&ops, path, source, length);
}
