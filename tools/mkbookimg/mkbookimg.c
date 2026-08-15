/**
 * @file mkbookimg.c
 * @brief Host tool that streams compiled books into an atomic FAT32 image
 * @details Streams a 64 MiB FAT32 image through firmware `ra8_fs` over a sparse sibling.
 * Fixed chunks are verified before sync/rename; no POSIX type crosses into reusable code.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h> /* POSIX renameat declaration; no hosted stream is used. */
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "mkbookimg_names.h"
#include "ra8_attributes.h"
#include "ra8_fs.h"

#ifndef O_CLOEXEC
/** @brief No-op close-on-exec fallback for hosts lacking the flag. */
#define O_CLOEXEC (0)
#endif

#ifndef O_DIRECTORY
/** @brief No-op directory-open fallback for hosts lacking the flag. */
#define O_DIRECTORY (0)
#endif

#ifndef O_NOFOLLOW
/** @brief No-op no-follow fallback for hosts lacking the flag. */
#define O_NOFOLLOW (0)
#endif

/** @brief Fixed image geometry and bounded host-I/O capacities. */
typedef enum : uint32_t {
  k_block_size           = 512U,        /**< Bytes per image sector.       */
  k_img_sectors          = 131072U,     /**< Sectors in the 64 MiB image.  */
  k_max_books            = 32U,         /**< Maximum input-book count.     */
  k_stream_chunk_bytes   = 16U * 1024U, /**< Bounded copy chunk bytes.     */
  k_host_path_cap        = 4096U,       /**< Hosted path capacity.         */
  k_host_name_cap        = 256U,        /**< Hosted leaf capacity.         */
  k_temp_create_attempts = 128U,        /**< Exclusive-create retry bound. */
  k_decimal_u64_digits   = 20U,         /**< Maximum `uint64_t` digits.    */
  k_decimal_base         = 10U,         /**< Decimal conversion radix.     */
  k_output_create_mode   = 0666U,       /**< Hosted output creation mode.  */
} mkbookimg_limit_t;

/** @brief Host descriptor-backed block device bound into `ra8_fs`. */
typedef struct {
  uint64_t block_count; /**< Addressable sectors.               */
  uint32_t block_size;  /**< Bytes in one sector.               */
  int      fd;          /**< Open sibling-temporary descriptor. */
  bool     io_failed;   /**< Sticky positioned-I/O failure.     */
} mkbookimg_disk_t;

/** @brief Caller-owned atomic-publication state. */
typedef struct {
  char final_name[k_host_name_cap]; /**< Destination leaf.             */
  char temp_name[k_host_name_cap];  /**< Owned temporary leaf.         */
  int  directory_fd;                /**< Destination-directory handle. */
  int  image_fd;                    /**< Open temporary image.         */
  bool temp_exists;                 /**< Temp leaf requires unlink.    */
} mkbookimg_output_t;

/** @brief Stable facts used to detect input replacement or mutation. */
typedef struct {
  dev_t    device;        /**< Host device identity.        */
  ino_t    inode;         /**< Host inode identity.         */
  uint64_t size;          /**< Captured input length.       */
  time_t   modified_sec;  /**< Modification seconds.        */
  long     modified_nsec; /**< Modification nanoseconds.    */
  time_t   changed_sec;   /**< Metadata-change seconds.     */
  long     changed_nsec;  /**< Metadata-change nanoseconds. */
} mkbookimg_input_identity_t;

/**
 * @brief Write a complete diagnostic fragment to the standard-error descriptor.
 * @details Retries interrupted and short writes without involving stdio streams.
 * @param[in] text NUL-terminated fragment.
 * @pre @p text is non-NULL and NUL-terminated.
 * @pre Standard error may be written or may reject the diagnostic.
 * @post The complete fragment was attempted.
 * @post No application state changed.
 * @note Fragments may interleave with another process's diagnostics.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_diag(const char* text)
{
  size_t offset = 0U;
  size_t length = strlen(text);
  while (offset < length) {
    const ssize_t written = write(STDERR_FILENO, &text[offset], length - offset);
    if ((written < 0) && (errno == EINTR)) {
      continue;
    }
    if (written <= 0) {
      return;
    }
    offset += (size_t)written;
  }
}

/**
 * @brief Emit an unsigned decimal value without a formatting runtime.
 * @details Converts into fixed local buffers and forwards one bounded string.
 * @param[in] value Value to emit.
 * @pre Standard error may be written or may reject the diagnostic.
 * @pre ::k_decimal_u64_digits holds every uint64 decimal spelling.
 * @post The decimal spelling was attempted on standard error.
 * @post No application state changed.
 * @note Fragments may interleave with another process's diagnostics.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_diag_u64(uint64_t value)
{
  char   reverse[k_decimal_u64_digits];
  char   forward[k_decimal_u64_digits + 1U];
  size_t digits = 0U;
  do {
    reverse[digits++] = (char)('0' + (value % (uint64_t)k_decimal_base));
    value /= (uint64_t)k_decimal_base;
  } while (value != 0U);
  for (size_t i = 0U; i < digits; ++i) {
    forward[i] = reverse[digits - i - 1U];
  }
  forward[digits] = '\0';
  internal_diag(forward);
}

/**
 * @brief Read exactly one bounded positioned byte range.
 * @details Retries interrupted calls and rejects EOF before the requested end.
 * @param[in]  fd     Open regular-file descriptor.
 * @param[in]  offset Absolute starting byte offset.
 * @param[out] bytes  Destination spanning @p length bytes.
 * @param[in]  length Exact byte count.
 * @return true only when every byte was read.
 * @retval true  The destination contains the complete requested range.
 * @retval false EOF, overflow, or an unrecoverable read error occurred.
 * @pre @p bytes spans @p length writable bytes.
 * @pre The descriptor remains open for this call.
 * @post Descriptor position is unchanged.
 * @post On false @p bytes may hold a strict prefix.
 * @note Thread-safe for independent buffers while the descriptor remains open.
 * @since 0.1.0
 */
RA8_INTERNAL static bool
internal_pread_exact(int fd, uint64_t offset, uint8_t* bytes, size_t length)
{
  if (offset > (uint64_t)INT64_MAX || length > ((uint64_t)INT64_MAX - offset)) {
    return false;
  }
  size_t done = 0U;
  while (done < length) {
    size_t request = length - done;
    if (request > (size_t)SSIZE_MAX) {
      request = (size_t)SSIZE_MAX;
    }
    const ssize_t got = pread(fd, &bytes[done], request, (off_t)(offset + done));
    if ((got < 0) && (errno == EINTR)) {
      continue;
    }
    if (got <= 0) {
      return false;
    }
    done += (size_t)got;
  }
  return true;
}

/**
 * @brief Write exactly one bounded positioned byte range.
 * @details Retries interrupted calls and treats a zero write as an I/O failure.
 * @param[in] fd     Open regular-file descriptor.
 * @param[in] offset Absolute starting byte offset.
 * @param[in] bytes  Source spanning @p length bytes.
 * @param[in] length Exact byte count.
 * @return true only when every byte was written.
 * @retval true  The complete range reached the page cache.
 * @retval false Overflow or an unrecoverable/zero write occurred.
 * @pre @p bytes spans @p length readable bytes.
 * @pre The descriptor remains open for this call.
 * @post Descriptor position is unchanged.
 * @post On false the file may contain a strict prefix in the temporary image.
 * @note Not thread-safe for overlapping ranges.
 * @since 0.1.0
 */
RA8_INTERNAL static bool
internal_pwrite_exact(int fd, uint64_t offset, const uint8_t* bytes, size_t length)
{
  if (offset > (uint64_t)INT64_MAX || length > ((uint64_t)INT64_MAX - offset)) {
    return false;
  }
  size_t done = 0U;
  while (done < length) {
    size_t request = length - done;
    if (request > (size_t)SSIZE_MAX) {
      request = (size_t)SSIZE_MAX;
    }
    const ssize_t put = pwrite(fd, &bytes[done], request, (off_t)(offset + done));
    if ((put < 0) && (errno == EINTR)) {
      continue;
    }
    if (put <= 0) {
      return false;
    }
    done += (size_t)put;
  }
  return true;
}

/**
 * @brief Validate and translate a block range to a host byte range.
 * @details Checks geometry, multiplication, host-offset, and size limits first.
 * @param[in]  disk       Descriptor-backed device geometry.
 * @param[in]  lba        First sector.
 * @param[in]  count      Sector count.
 * @param[out] out_offset Host byte offset.
 * @param[out] out_bytes  Host byte count.
 * @return true when the whole range is in bounds and representable.
 * @retval true  Both outputs describe the requested range.
 * @retval false The range exceeds geometry or host integer limits.
 * @pre All pointers are non-NULL.
 * @pre @p disk carries non-zero valid geometry.
 * @post On true both outputs are initialized.
 * @post On false both outputs are untouched.
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_block_range(const mkbookimg_disk_t* disk,
                                              uint64_t                lba,
                                              uint32_t                count,
                                              uint64_t*               out_offset,
                                              size_t*                 out_bytes)
{
  if (lba > disk->block_count || (uint64_t)count > (disk->block_count - lba)) {
    return false;
  }
  if (lba > (UINT64_MAX / disk->block_size) || count > (SIZE_MAX / disk->block_size)) {
    return false;
  }
  const uint64_t offset = lba * disk->block_size;
  const size_t   bytes  = (size_t)count * disk->block_size;
  if (offset > (uint64_t)INT64_MAX || bytes > ((uint64_t)INT64_MAX - offset)) {
    return false;
  }
  *out_offset = offset;
  *out_bytes  = bytes;
  return true;
}

/**
 * @brief Read sectors from the temporary image for `ra8_fs`.
 * @details Translates the whole request before issuing exact positioned I/O.
 * @param[in,out] ctx    Bound ::mkbookimg_disk_t.
 * @param[in]     lba    First sector.
 * @param[in]     count  Sector count.
 * @param[out]    buffer Destination for exactly @p count sectors.
 * @return `ra8_fs` block-device status.
 * @retval k_ra8_ok               Every sector was read exactly.
 * @retval k_ra8_err_out_of_range Invalid state, pointer, or range.
 * @retval k_ra8_fail             Positioned host I/O failed.
 * @pre @p ctx and @p buffer are non-NULL.
 * @pre The bound descriptor remains open.
 * @post On success @p buffer contains the requested sectors.
 * @post A host I/O failure is sticky in the disk context.
 * @note Not thread-safe through the sticky failure flag.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_disk_read(void* ctx, uint64_t lba, uint32_t count, uint8_t* buffer)
{
  mkbookimg_disk_t* disk = (mkbookimg_disk_t*)ctx;
  uint64_t          offset;
  size_t            bytes;
  if (disk == nullptr || buffer == nullptr || disk->io_failed ||
      !internal_block_range(disk, lba, count, &offset, &bytes)) {
    return k_ra8_err_out_of_range;
  }
  if (!internal_pread_exact(disk->fd, offset, buffer, bytes)) {
    disk->io_failed = true;
    return k_ra8_fail;
  }
  return k_ra8_ok;
}

/**
 * @brief Write sectors to the temporary image for `ra8_fs`.
 * @details Translates the whole request before issuing exact positioned I/O.
 * @param[in,out] ctx    Bound ::mkbookimg_disk_t.
 * @param[in]     lba    First sector.
 * @param[in]     count  Sector count.
 * @param[in]     buffer Source containing exactly @p count sectors.
 * @return `ra8_fs` block-device status.
 * @retval k_ra8_ok               Every sector was written exactly.
 * @retval k_ra8_err_out_of_range Invalid state, pointer, or range.
 * @retval k_ra8_fail             Positioned host I/O failed.
 * @pre @p ctx and @p buffer are non-NULL.
 * @pre The bound descriptor remains open.
 * @post On success the temporary image contains the requested sectors.
 * @post A host I/O failure is sticky in the disk context.
 * @note Not thread-safe through the sticky failure flag.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_disk_write(void* ctx, uint64_t lba, uint32_t count, const uint8_t* buffer)
{
  mkbookimg_disk_t* disk = (mkbookimg_disk_t*)ctx;
  uint64_t          offset;
  size_t            bytes;
  if (disk == nullptr || buffer == nullptr || disk->io_failed ||
      !internal_block_range(disk, lba, count, &offset, &bytes)) {
    return k_ra8_err_out_of_range;
  }
  if (!internal_pwrite_exact(disk->fd, offset, buffer, bytes)) {
    disk->io_failed = true;
    return k_ra8_fail;
  }
  return k_ra8_ok;
}

/**
 * @brief Report fixed image geometry to `ra8_fs`.
 * @details Copies immutable descriptor-backed geometry into caller outputs.
 * @param[in]  ctx         Bound ::mkbookimg_disk_t.
 * @param[out] block_count Receives the sector count.
 * @param[out] block_size  Receives bytes per sector.
 * @return `ra8_fs` block-device status.
 * @retval k_ra8_ok           Geometry was copied to both outputs.
 * @retval k_ra8_err_null_ptr Any argument was NULL.
 * @pre All arguments are non-NULL.
 * @pre The disk geometry was initialized by `main`.
 * @post On success both outputs are populated.
 * @post The disk context is unchanged.
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_disk_capacity(void* ctx, uint64_t* block_count, uint32_t* block_size)
{
  const mkbookimg_disk_t* disk = (const mkbookimg_disk_t*)ctx;
  if (disk == nullptr || block_count == nullptr || block_size == nullptr) {
    return k_ra8_err_null_ptr;
  }
  *block_count = disk->block_count;
  *block_size  = disk->block_size;
  return k_ra8_ok;
}

/**
 * @brief Split an output path into an existing parent directory and safe leaf.
 * @details Rejects truncation, empty leaves, and dot traversal components.
 * @param[in]  path       Requested destination path.
 * @param[out] parent     Parent path buffer.
 * @param[out] final_name Destination leaf buffer.
 * @return true when both components fit and the leaf is usable.
 * @retval true  Both outputs are populated.
 * @retval false The path is empty, too long, or names `.`/`..`/a directory.
 * @pre Output buffers have their declared fixed capacities.
 * @pre @p path is non-NULL and NUL-terminated.
 * @post On true the two outputs reconstruct @p path.
 * @post On false no filesystem object was touched.
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_split_output(const char* path,
                                               char        parent[k_host_path_cap],
                                               char        final_name[k_host_name_cap])
{
  const size_t length = strlen(path);
  if (length == 0U || length >= (size_t)k_host_path_cap) {
    return false;
  }
  size_t slash = length;
  while (slash > 0U && path[slash - 1U] != '/') {
    --slash;
  }
  const size_t leaf_start = slash;
  const size_t leaf_bytes = length - leaf_start;
  if (leaf_bytes == 0U || leaf_bytes >= (size_t)k_host_name_cap) {
    return false;
  }
  (void)memcpy(final_name, &path[leaf_start], leaf_bytes);
  final_name[leaf_bytes] = '\0';
  if (strcmp(final_name, ".") == 0 || strcmp(final_name, "..") == 0) {
    return false;
  }
  if (slash == 0U) {
    parent[0] = '.';
    parent[1] = '\0';
  } else if (slash == 1U) {
    parent[0] = '/';
    parent[1] = '\0';
  } else {
    const size_t parent_bytes = slash - 1U;
    (void)memcpy(parent, path, parent_bytes);
    parent[parent_bytes] = '\0';
  }
  return true;
}

/**
 * @brief Build one hidden sibling-temporary leaf from process id and attempt.
 * @details Uses fixed decimal buffers so collision retries require no allocation.
 * @param[out] out     Temporary-name buffer.
 * @param[in]  process Positive process identifier.
 * @param[in]  attempt Collision retry index.
 * @return true when the name fits.
 * @retval true  @p out contains a complete NUL-terminated name.
 * @retval false The fixed output capacity was insufficient.
 * @pre @p out has ::k_host_name_cap bytes.
 * @pre @p process and @p attempt are finite unsigned values.
 * @post No filesystem object was touched.
 * @post The resulting name contains no slash.
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static bool
internal_temp_name(char out[k_host_name_cap], uint64_t process, uint32_t attempt)
{
  static const char s_prefix[] = ".mkbookimg.tmp.";
  char              reverse[k_decimal_u64_digits];
  size_t            offset = sizeof(s_prefix) - 1U;
  (void)memcpy(out, s_prefix, offset);
  uint64_t values[2] = {process, attempt};
  for (uint8_t field = 0U; field < 2U; ++field) {
    size_t digits = 0U;
    do {
      reverse[digits++] = (char)('0' + (values[field] % (uint64_t)k_decimal_base));
      values[field] /= (uint64_t)k_decimal_base;
    } while (values[field] != 0U);
    if (offset + digits + 2U > (size_t)k_host_name_cap) {
      return false;
    }
    while (digits > 0U) {
      out[offset++] = reverse[--digits];
    }
    out[offset++] = (field == 0U) ? '.' : '\0';
  }
  return true;
}

/**
 * @brief Create and size a private sibling temporary for atomic publication.
 * @details Uses exclusive no-follow creation and unwinds every partial resource.
 * @param[in]  final_path Requested output path.
 * @param[out] output     Caller-owned publication state.
 * @return true when an empty sparse image is open and owned by @p output.
 * @retval true  The temporary descriptor has exact image length.
 * @retval false Validation, directory open, create, or truncate failed.
 * @pre @p output is writable and owns no descriptors.
 * @pre @p final_path is NUL-terminated.
 * @post On true @p output owns one same-directory temporary.
 * @post On false no temporary or descriptor remains owned.
 * @note Not thread-safe for the same destination name.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_output_begin(const char* final_path, mkbookimg_output_t* output)
{
  *output = (mkbookimg_output_t){.directory_fd = -1, .image_fd = -1};
  char parent[k_host_path_cap];
  if (!internal_split_output(final_path, parent, output->final_name)) {
    return false;
  }
  output->directory_fd = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (output->directory_fd < 0) {
    return false;
  }
  for (uint32_t attempt = 0U; attempt < (uint32_t)k_temp_create_attempts; ++attempt) {
    if (!internal_temp_name(output->temp_name, (uint64_t)getpid(), attempt)) {
      break;
    }
    output->image_fd = openat(output->directory_fd,
                              output->temp_name,
                              O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                              (mode_t)k_output_create_mode);
    if (output->image_fd >= 0) {
      output->temp_exists = true;
      break;
    }
    if (errno != EEXIST) {
      break;
    }
  }
  const uint64_t image_bytes = (uint64_t)k_img_sectors * (uint64_t)k_block_size;
  if (output->image_fd < 0 || image_bytes > (uint64_t)INT64_MAX ||
      ftruncate(output->image_fd, (off_t)image_bytes) != 0) {
    if (output->image_fd >= 0) {
      (void)close(output->image_fd);
      output->image_fd = -1;
    }
    if (output->temp_exists) {
      (void)unlinkat(output->directory_fd, output->temp_name, 0);
      output->temp_exists = false;
    }
    (void)close(output->directory_fd);
    output->directory_fd = -1;
    return false;
  }
  return true;
}

/**
 * @brief Close and remove an unpublished temporary image.
 * @details Idempotently releases the descriptor, private leaf, and parent handle.
 * @param[in,out] output Publication state from ::internal_output_begin.
 * @pre @p output is non-NULL and may be partially initialized.
 * @pre No mounted filesystem still uses `output->image_fd`.
 * @post No descriptor or temporary leaf remains owned.
 * @post An existing final destination is unchanged.
 * @note Not thread-safe for the same state object.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_output_abort(mkbookimg_output_t* output)
{
  if (output->image_fd >= 0) {
    (void)close(output->image_fd);
    output->image_fd = -1;
  }
  if (output->temp_exists && output->directory_fd >= 0) {
    (void)unlinkat(output->directory_fd, output->temp_name, 0);
    output->temp_exists = false;
  }
  if (output->directory_fd >= 0) {
    (void)close(output->directory_fd);
    output->directory_fd = -1;
  }
}

/**
 * @brief Durably publish the complete image with one same-directory rename.
 * @details Syncs file data before rename and parent metadata after publication.
 * @param[in,out] output Publication state owning the complete temporary image.
 * @return true when file sync, close, rename, and directory sync all succeed.
 * @retval true  The final leaf atomically names the complete image.
 * @retval false Publication failed; before rename the old final is preserved.
 * @pre No mount or backend call will use `output->image_fd` again.
 * @pre @p output owns a complete temporary image.
 * @post The temporary descriptor and directory descriptor are closed.
 * @post No temporary leaf remains after a successful rename.
 * @note A directory-sync failure after rename reports false although the complete
 *       image is already visible; no API can safely roll that rename back.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_output_commit(mkbookimg_output_t* output)
{
  bool ok = fsync(output->image_fd) == 0;
  if (close(output->image_fd) != 0) {
    ok = false;
  }
  output->image_fd = -1;
  if (ok &&
      renameat(output->directory_fd, output->temp_name, output->directory_fd, output->final_name) ==
        0) {
    output->temp_exists = false;
    if (fsync(output->directory_fd) != 0) {
      ok = false;
    }
  } else {
    ok = false;
  }
  if (output->temp_exists) {
    (void)unlinkat(output->directory_fd, output->temp_name, 0);
    output->temp_exists = false;
  }
  if (close(output->directory_fd) != 0) {
    ok = false;
  }
  output->directory_fd = -1;
  return ok;
}

/**
 * @brief Extract nanosecond-resolution identity fields portably.
 * @details Normalizes Darwin and POSIX timestamp layouts into one owned value.
 * @param[in] metadata Successful `fstat` result for a regular input.
 * @return Stable identity value copied from @p metadata.
 * @retval all-fields The input's device, inode, length, mtime, and ctime.
 * @pre @p metadata is non-NULL and has non-negative size.
 * @pre Platform timestamp members follow the selected compile-time branch.
 * @post No state is changed.
 * @post The returned value owns no resources.
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static mkbookimg_input_identity_t internal_input_identity(const struct stat* metadata)
{
#if defined(__APPLE__)
  const long modified_nsec = metadata->st_mtimespec.tv_nsec;
  const long changed_nsec  = metadata->st_ctimespec.tv_nsec;
#else
  const long modified_nsec = metadata->st_mtim.tv_nsec;
  const long changed_nsec  = metadata->st_ctim.tv_nsec;
#endif
  return (mkbookimg_input_identity_t){.device        = metadata->st_dev,
                                      .inode         = metadata->st_ino,
                                      .size          = (uint64_t)metadata->st_size,
                                      .modified_sec  = metadata->st_mtime,
                                      .modified_nsec = modified_nsec,
                                      .changed_sec   = metadata->st_ctime,
                                      .changed_nsec  = changed_nsec};
}

/**
 * @brief Compare a captured identity with a fresh `fstat` result.
 * @details Detects replacement, resizing, and timestamp-visible input mutation.
 * @param[in] expected Previously captured input identity.
 * @param[in] actual   Fresh metadata from the same open descriptor.
 * @return true only when every mutation-sensitive field matches.
 * @retval true  The observed descriptor still names the same unchanged bytes.
 * @retval false Size or any stable identity/timestamp field differs.
 * @pre Both arguments are non-NULL.
 * @pre @p expected came from ::internal_input_identity.
 * @post No state is changed.
 * @post The result depends on every represented identity field.
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_input_same(const mkbookimg_input_identity_t* expected,
                                             const struct stat*                actual)
{
  if (actual->st_size < 0) {
    return false;
  }
  const mkbookimg_input_identity_t observed = internal_input_identity(actual);
  return expected->device == observed.device && expected->inode == observed.inode &&
         expected->size == observed.size && expected->modified_sec == observed.modified_sec &&
         expected->modified_nsec == observed.modified_nsec &&
         expected->changed_sec == observed.changed_sec &&
         expected->changed_nsec == observed.changed_nsec;
}

/**
 * @brief Compare two captured host-input identities field by field.
 * @details Avoids padding comparisons while checking every mutation-sensitive field.
 * @param[in] left  First identity.
 * @param[in] right Second identity.
 * @return true only when every field matches.
 * @retval true  Both values describe the same unchanged input.
 * @retval false At least one identity or timestamp field differs.
 * @pre Both arguments are non-NULL and fully initialized.
 * @pre Both values came from ::internal_input_identity.
 * @post No state is changed.
 * @post Structure padding is never inspected.
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_identity_equal(const mkbookimg_input_identity_t* left,
                                                 const mkbookimg_input_identity_t* right)
{
  return left->device == right->device && left->inode == right->inode &&
         left->size == right->size && left->modified_sec == right->modified_sec &&
         left->modified_nsec == right->modified_nsec && left->changed_sec == right->changed_sec &&
         left->changed_nsec == right->changed_nsec;
}

/**
 * @brief Open and identify one immutable regular input without following its leaf.
 * @details Rejects links, empty inputs, non-regular objects, and oversized books.
 * @param[in]  path     Input path.
 * @param[out] out_fd   Receives owned read descriptor.
 * @param[out] identity Receives initial stable identity.
 * @return true for a non-empty regular file within FAT32 size limits.
 * @retval true  Descriptor and identity are populated.
 * @retval false Open/stat/type/size validation failed.
 * @pre All arguments are non-NULL.
 * @pre @p path is NUL-terminated.
 * @post On true caller owns @p out_fd.
 * @post On false no descriptor remains open.
 * @note Thread-safe for distinct output arguments.
 * @since 0.1.0
 */
RA8_INTERNAL static bool
internal_input_open(const char* path, int* out_fd, mkbookimg_input_identity_t* identity)
{
  const int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) {
    return false;
  }
  struct stat metadata = {};
  if (fstat(fd, &metadata) != 0 || !S_ISREG(metadata.st_mode) || metadata.st_size <= 0 ||
      (uint64_t)metadata.st_size > UINT32_MAX) {
    (void)close(fd);
    return false;
  }
  *identity = internal_input_identity(&metadata);
  *out_fd   = fd;
  return true;
}

/**
 * @brief Stream one host input into a newly truncated filesystem file.
 * @details Copies through a bounded stack chunk and rechecks identity before close.
 * @param[in,out] mount Mounted FAT32 image.
 * @param[in]     name  Destination basename.
 * @param[in]     path  Host input path.
 * @param[out]    identity Identity used by the verification pass.
 * @return true only when the input was stable and the card file closed cleanly.
 * @retval true  The full input is present under @p name.
 * @retval false Host read, mutation check, filesystem write, or close failed.
 * @pre @p mount is mounted and all strings are NUL-terminated.
 * @pre @p identity is writable.
 * @post On true the card file length equals `identity->size`.
 * @post All host/card handles opened here are closed on every path.
 * @note Not thread-safe; `ra8_fs` owns shared static mount/file slots.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_stream_book(ra8_fs_mount_t*             mount,
                                              const char*                 name,
                                              const char*                 path,
                                              mkbookimg_input_identity_t* identity)
{
  int input_fd = -1;
  if (!internal_input_open(path, &input_fd, identity)) {
    return false;
  }
  ra8_fs_file_t* card = nullptr;
  if (ra8_fs_open(mount, name, k_ra8_fs_mode_write, &card) != k_ra8_ok) {
    (void)close(input_fd);
    return false;
  }
  uint8_t  chunk[k_stream_chunk_bytes];
  uint64_t offset = 0U;
  bool     ok     = true;
  while (ok && offset < identity->size) {
    size_t take = (size_t)(identity->size - offset);
    if (take > sizeof(chunk)) {
      take = sizeof(chunk);
    }
    ok = internal_pread_exact(input_fd, offset, chunk, take) &&
         ra8_fs_write(card, chunk, (uint32_t)take) == k_ra8_ok;
    offset += take;
  }
  struct stat after = {};
  if (fstat(input_fd, &after) != 0 || !internal_input_same(identity, &after)) {
    ok = false;
  }
  if (close(input_fd) != 0) {
    ok = false;
  }
  if (ra8_fs_close(card) != k_ra8_ok) {
    ok = false;
  }
  return ok;
}

/**
 * @brief Re-read host and card bytes to prove the streamed copy and input stability.
 * @details Compares bounded chunks and repeats the input identity check afterward.
 * @param[in,out] mount    Mounted FAT32 image.
 * @param[in]     name     Card basename written by ::internal_stream_book.
 * @param[in]     path     Original host path.
 * @param[in]     identity Identity observed before the write pass.
 * @return true only when every byte and all identity fields still match.
 * @retval true  Host and card contents match exactly and remained stable.
 * @retval false Reopen, metadata, read, compare, or close failed.
 * @pre All strings and @p identity are valid.
 * @pre @p mount remains mounted.
 * @post Every opened handle is closed.
 * @post The card filesystem is not modified.
 * @note Not thread-safe; `ra8_fs` owns shared static mount/file slots.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_verify_book(ra8_fs_mount_t*                   mount,
                                              const char*                       name,
                                              const char*                       path,
                                              const mkbookimg_input_identity_t* identity)
{
  int                        input_fd = -1;
  mkbookimg_input_identity_t again;
  if (!internal_input_open(path, &input_fd, &again)) {
    if (input_fd >= 0) {
      (void)close(input_fd);
    }
    return false;
  }
  if (!internal_identity_equal(identity, &again)) {
    (void)close(input_fd);
    return false;
  }
  ra8_fs_file_t* card = nullptr;
  uint64_t       card_size;
  if (ra8_fs_open(mount, name, k_ra8_fs_mode_read, &card) != k_ra8_ok ||
      ra8_fs_size(card, &card_size) != k_ra8_ok || card_size != identity->size) {
    if (card != nullptr) {
      (void)ra8_fs_close(card);
    }
    (void)close(input_fd);
    return false;
  }
  uint8_t  source[k_stream_chunk_bytes];
  uint8_t  copied[k_stream_chunk_bytes];
  uint64_t offset = 0U;
  bool     ok     = true;
  while (ok && offset < identity->size) {
    uint32_t take = (uint32_t)(identity->size - offset);
    if (take > (uint32_t)sizeof(source)) {
      take = (uint32_t)sizeof(source);
    }
    uint32_t got = 0U;
    ok           = internal_pread_exact(input_fd, offset, source, take) &&
                   ra8_fs_read(card, copied, take, &got) == k_ra8_ok && got == take &&
                   memcmp(source, copied, take) == 0;
    offset += take;
  }
  struct stat after = {};
  if (fstat(input_fd, &after) != 0 || !internal_input_same(identity, &after)) {
    ok = false;
  }
  if (close(input_fd) != 0 || ra8_fs_close(card) != k_ra8_ok) {
    ok = false;
  }
  return ok;
}

/**
 * @brief Detect whether one input would overwrite an earlier basename.
 * @details Compares only normalized destination basenames from earlier arguments.
 * @param[in] arguments Program argument vector with inputs from index two.
 * @param[in] index     Zero-based input index to compare with its predecessors.
 * @return true when an earlier input has the same card basename.
 * @retval true  Publishing this input would replace an earlier book.
 * @retval false Its basename is unique among preceding inputs.
 * @pre @p arguments contains valid paths through input @p index.
 * @pre @p index is non-negative.
 * @post No argument string is changed.
 * @post Only earlier inputs participate in the result.
 * @note Pure; thread-safe for immutable arguments.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_duplicate_name(char** arguments, int index)
{
  const char* name = mkbookimg_basename(arguments[2 + index]);
  for (int prior = 0; prior < index; ++prior) {
    if (strcmp(name, mkbookimg_basename(arguments[2 + prior])) == 0) {
      return true;
    }
  }
  return false;
}

/**
 * @brief Format and mount the descriptor-backed FAT32 image.
 * @details Applies deterministic format policy before claiming one mount slot.
 * @param[in]  backend Block-device facade.
 * @param[out] mount   Receives mounted volume.
 * @return true only when both formatter and mount succeed.
 * @retval true  The fresh FAT32 image is mounted.
 * @retval false Formatting or mount validation failed.
 * @pre Both arguments are non-NULL.
 * @pre Backend capacity is an empty exact-size image.
 * @post On true caller must unmount @p mount.
 * @post On false no mount handle remains claimed.
 * @note Not thread-safe through global `ra8_fs` slots.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_format_mount(const ra8_fs_backend_t* backend,
                                               ra8_fs_mount_t**        mount)
{
  const ra8_fs_format_opts_t options = {.type = k_ra8_fs_type_fat32, .label = "RABOOKS"};
  if (ra8_fs_format(backend, &options) != k_ra8_ok) {
    internal_diag("mkbookimg: ra8_fs_format failed\n");
    return false;
  }
  if (ra8_fs_mount(backend, mount) != k_ra8_ok) {
    internal_diag("mkbookimg: ra8_fs_mount failed\n");
    return false;
  }
  return true;
}

/**
 * @brief Stream and verify every requested book through the mounted filesystem.
 * @details Rejects name collisions and requires a second byte-for-byte read pass.
 * @param[in,out] mount      Mounted FAT32 volume.
 * @param[in]     arguments  Program argument vector.
 * @param[in]     book_count Number of input paths beginning at argv[2].
 * @return true only when every source is stable and exactly reproduced.
 * @retval true  Every named card file passed byte-for-byte verification.
 * @retval false Name, collision, host I/O, mutation, or filesystem I/O failed.
 * @pre @p book_count is in 1..::k_max_books.
 * @pre @p mount and @p arguments are valid.
 * @post No host or card file handle remains open.
 * @post Failure affects only the unpublished temporary image.
 * @note Not thread-safe through global `ra8_fs` slots.
 * @since 0.1.0
 */
RA8_INTERNAL static bool
internal_write_books(ra8_fs_mount_t* mount, char** arguments, int book_count)
{
  for (int index = 0; index < book_count; ++index) {
    char name[k_mkbookimg_name_cap];
    if (!mkbookimg_dest_name(arguments[2 + index], name, sizeof(name))) {
      internal_diag("mkbookimg: unusable card name for ");
      internal_diag(arguments[2 + index]);
      internal_diag("\n");
      return false;
    }
    if (internal_duplicate_name(arguments, index)) {
      internal_diag("mkbookimg: duplicate card name ");
      internal_diag(name);
      internal_diag("\n");
      return false;
    }
    mkbookimg_input_identity_t identity;
    if (!internal_stream_book(mount, name, arguments[2 + index], &identity) ||
        !internal_verify_book(mount, name, arguments[2 + index], &identity)) {
      internal_diag("mkbookimg: input changed or could not be copied exactly: ");
      internal_diag(arguments[2 + index]);
      internal_diag("\n");
      return false;
    }
    internal_diag("mkbookimg: + ");
    internal_diag(name);
    internal_diag("  (");
    internal_diag_u64(identity.size);
    internal_diag(" bytes)  <- ");
    internal_diag(arguments[2 + index]);
    internal_diag("\n");
  }
  return true;
}

/**
 * @brief Validate CLI input, construct, verify, and atomically publish one image.
 * @param[in] argc Argument count.
 * @param[in] argv Output path followed by one or more book paths.
 * @return Process status: zero success, one runtime failure, two usage error.
 * @retval 0 A complete verified image was atomically published.
 * @retval 1 Generation or durable publication failed.
 * @retval 2 The command line did not contain a supported input count.
 * @pre @p argv contains @p argc entries as required by the hosted C runtime.
 * @pre The process may open the explicitly supplied paths.
 * @post On zero the destination is a complete deterministic FAT32 image.
 * @post Before the publication rename, every failure preserves the destination.
 * @note Not thread-safe through global `ra8_fs` slots.
 * @since 0.1.0
 */
int main(int argc, char** argv)
{
  if (argc < 3) {
    internal_diag("usage: mkbookimg <out.img> <book1.rabook> [book2.rabook ...]\n");
    return 2;
  }
  const int book_count = argc - 2;
  if (book_count > (int)k_max_books) {
    internal_diag("mkbookimg: too many books (max 32)\n");
    return 2;
  }

  mkbookimg_output_t output;
  if (!internal_output_begin(argv[1], &output)) {
    internal_diag("mkbookimg: cannot create a safe sibling temporary for ");
    internal_diag(argv[1]);
    internal_diag("\n");
    return 1;
  }
  mkbookimg_disk_t       disk    = {.block_count = k_img_sectors,
                                    .block_size  = k_block_size,
                                    .fd          = output.image_fd};
  const ra8_fs_backend_t backend = {.read_block   = internal_disk_read,
                                    .write_block  = internal_disk_write,
                                    .get_capacity = internal_disk_capacity,
                                    .ctx          = &disk};
  ra8_fs_mount_t*        mount   = nullptr;
  bool                   ok      = internal_format_mount(&backend, &mount);
  if (ok) {
    ok = internal_write_books(mount, argv, book_count);
    if (ra8_fs_unmount(mount) != k_ra8_ok) {
      ok = false;
    }
  }
  if (disk.io_failed) {
    ok = false;
  }
  if (ok) {
    ok = internal_output_commit(&output);
  } else {
    internal_output_abort(&output);
  }
  if (!ok) {
    internal_diag("mkbookimg: image generation failed; destination preserved\n");
    return 1;
  }
  internal_diag("mkbookimg: wrote ");
  internal_diag(argv[1]);
  internal_diag(" (");
  internal_diag_u64((uint64_t)book_count);
  internal_diag(" book(s), 64 MiB FAT32)\n");
  return 0;
}
