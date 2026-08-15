/**
 * @file mkfontimg_host.c
 * @brief Bounded descriptor-backed storage composition for mkfontimg
 *
 * @details
 * Implements exact positioned block I/O, immutable input streaming, card-file
 * reread verification, and durable same-directory atomic publication. This is
 * deliberately tool-private POSIX code; reusable filesystem code depends only
 * on the `ra8_fs_backend_t` callbacks bound here.
 *
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

#include "mkfontimg_internal.h"
#include "ra8_attributes.h"
#include "ra8_err.h"
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

/** @brief Fixed I/O, naming, and formatting bounds. */
typedef enum : uint32_t {
  k_host_stream_chunk   = 16U * 1024U, /**< Bounded copy chunk bytes.     */
  k_host_temp_attempts  = 128U,        /**< Exclusive-create retry bound. */
  k_host_decimal_digits = 20U,         /**< Maximum `uint64_t` digits.    */
  k_host_decimal_base   = 10U,         /**< Decimal conversion radix.     */
  k_host_create_mode    = 0666U,       /**< Hosted output creation mode.  */
} host_limit_t;

/** @brief Mutation-sensitive identity for one open regular input. */
typedef struct {
  dev_t    device;        /**< Host device identity.        */
  ino_t    inode;         /**< Host inode identity.         */
  uint64_t size;          /**< Captured input length.       */
  time_t   modified_sec;  /**< Modification seconds.        */
  long     modified_nsec; /**< Modification nanoseconds.    */
  time_t   changed_sec;   /**< Metadata-change seconds.     */
  long     changed_nsec;  /**< Metadata-change nanoseconds. */
} host_input_identity_t;

RA8_PRIV void priv_mkfontimg_diag(const char* text)
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

RA8_PRIV void priv_mkfontimg_diag_u64(uint64_t value)
{
  char   reverse[k_host_decimal_digits];
  char   forward[k_host_decimal_digits + 1U];
  size_t digits = 0U;
  do {
    reverse[digits++] = (char)('0' + (value % (uint64_t)k_host_decimal_base));
    value /= (uint64_t)k_host_decimal_base;
  } while (value != 0U);
  for (size_t i = 0U; i < digits; ++i) {
    forward[i] = reverse[digits - i - 1U];
  }
  forward[digits] = '\0';
  priv_mkfontimg_diag(forward);
}

/**
 * @brief Read an exact positioned byte range with bounded retries.
 * @details Retries interruptions and rejects EOF before the requested length.
 * @param[in]  fd     Open regular-file descriptor.
 * @param[in]  offset Absolute byte offset.
 * @param[out] bytes  Exact-size destination.
 * @param[in]  length Required byte count.
 * @return true only when every byte was read.
 * @retval true  The destination is complete.
 * @retval false Overflow, EOF, or host I/O failure occurred.
 * @pre @p bytes spans @p length writable bytes.
 * @pre @p fd remains open.
 * @post Descriptor position is unchanged.
 * @post On false the destination may hold a strict prefix.
 * @note Thread-safe for independent buffers.
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
 * @brief Write an exact positioned byte range with bounded retries.
 * @details Retries interruptions and treats a zero write as terminal failure.
 * @param[in] fd     Open regular-file descriptor.
 * @param[in] offset Absolute byte offset.
 * @param[in] bytes  Exact-size source.
 * @param[in] length Required byte count.
 * @return true only when every byte was written.
 * @retval true  The complete range reached the page cache.
 * @retval false Overflow or host I/O failure occurred.
 * @pre @p bytes spans @p length readable bytes.
 * @pre @p fd remains open.
 * @post Descriptor position is unchanged.
 * @post On false only the unpublished temporary may be partial.
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
 * @brief Translate one sector range to a representable byte range.
 * @details Checks device geometry and every host integer conversion before I/O.
 * @param[in]  disk       Bound image geometry.
 * @param[in]  lba        First sector.
 * @param[in]  count      Sector count.
 * @param[out] out_offset Host byte offset.
 * @param[out] out_bytes  Host byte count.
 * @return true only when the complete range is valid.
 * @retval true  Both outputs describe the requested range.
 * @retval false Geometry or integer bounds reject it.
 * @pre All pointers are non-NULL.
 * @pre Disk geometry is non-zero.
 * @post Outputs are populated only on true.
 * @post No state changes.
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_block_range(const mkfontimg_disk_t* disk,
                                              uint64_t                lba,
                                              uint32_t                count,
                                              uint64_t*               out_offset,
                                              size_t*                 out_bytes)
{
  if (lba > disk->block_count || (uint64_t)count > (disk->block_count - lba) ||
      lba > (UINT64_MAX / disk->block_size) || count > (SIZE_MAX / disk->block_size)) {
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
 * @brief Read sectors from the sparse unpublished image.
 * @details Validates the complete block range, performs exact positioned I/O,
 * and makes the first host failure sticky for the transaction.
 * @param[in,out] ctx Bound ::mkfontimg_disk_t context.
 * @param[in] lba First sector to read.
 * @param[in] count Number of sectors to read.
 * @param[out] buffer Destination spanning `count * block_size` bytes.
 * @return Canonical block-device status.
 * @retval k_ra8_ok Every requested sector was read.
 * @retval k_ra8_err_out_of_range State, pointer, or geometry was invalid.
 * @retval k_ra8_fail Exact host I/O failed.
 * @pre @p ctx and @p buffer are non-null.
 * @pre The bound image descriptor remains open.
 * @post Success initializes the complete destination range.
 * @post Host failure leaves the disk error flag set.
 * @note Not thread-safe through the shared sticky-error flag.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_disk_read(void* ctx, uint64_t lba, uint32_t count, uint8_t* buffer)
{
  mkfontimg_disk_t* disk = (mkfontimg_disk_t*)ctx;
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
 * @brief Write sectors into the sparse unpublished image.
 * @details Validates the complete block range, performs exact positioned I/O,
 * and makes the first host failure sticky for the transaction.
 * @param[in,out] ctx Bound ::mkfontimg_disk_t context.
 * @param[in] lba First sector to write.
 * @param[in] count Number of sectors to write.
 * @param[in] buffer Source spanning `count * block_size` bytes.
 * @return Canonical block-device status.
 * @retval k_ra8_ok Every requested sector was written.
 * @retval k_ra8_err_out_of_range State, pointer, or geometry was invalid.
 * @retval k_ra8_fail Exact host I/O failed.
 * @pre @p ctx and @p buffer are non-null.
 * @pre The bound image descriptor remains open.
 * @post Success updates only the requested byte range.
 * @post Host failure leaves the disk error flag set.
 * @note Not thread-safe for overlapping writes or shared error state.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_disk_write(void* ctx, uint64_t lba, uint32_t count, const uint8_t* buffer)
{
  mkfontimg_disk_t* disk = (mkfontimg_disk_t*)ctx;
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
 * @brief Report the fixed sparse-image geometry.
 * @details Copies immutable sector count and size fields into caller outputs.
 * @param[in] ctx Bound ::mkfontimg_disk_t context.
 * @param[out] block_count Receives the number of sectors.
 * @param[out] block_size Receives bytes per sector.
 * @return Canonical block-device status.
 * @retval k_ra8_ok Both geometry outputs were initialized.
 * @retval k_ra8_err_null_ptr A required pointer was null.
 * @pre All three pointers are non-null.
 * @pre The context geometry was initialized before binding the backend.
 * @post Success populates both outputs.
 * @post The context and descriptor are unchanged.
 * @note Read-only and thread-safe while the context remains alive.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_disk_capacity(void* ctx, uint64_t* block_count, uint32_t* block_size)
{
  const mkfontimg_disk_t* disk = (const mkfontimg_disk_t*)ctx;
  if (disk == nullptr || block_count == nullptr || block_size == nullptr) {
    return k_ra8_err_null_ptr;
  }
  *block_count = disk->block_count;
  *block_size  = disk->block_size;
  return k_ra8_ok;
}

/**
 * @brief Split one output path into bounded parent and leaf components.
 * @details Rejects truncation, empty leaves, and dot traversal components.
 * @param[in]  path   Requested output.
 * @param[out] parent Parent path.
 * @param[out] leaf   Destination leaf.
 * @return true when both components are safe and bounded.
 * @retval true  Outputs are populated.
 * @retval false Path is empty, overlong, or names `.`/`..`/a directory.
 * @pre All arguments are non-NULL.
 * @pre Output arrays have their declared capacities.
 * @post No filesystem object is touched.
 * @post On true parent plus leaf reconstruct @p path.
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_split_output(const char* path,
                                               char        parent[k_mkfontimg_host_path_cap],
                                               char        leaf[k_mkfontimg_host_name_cap])
{
  const size_t length = strlen(path);
  if (length == 0U || length >= (size_t)k_mkfontimg_host_path_cap) {
    return false;
  }
  size_t slash = length;
  while (slash > 0U && path[slash - 1U] != '/') {
    --slash;
  }
  const size_t leaf_bytes = length - slash;
  if (leaf_bytes == 0U || leaf_bytes >= (size_t)k_mkfontimg_host_name_cap) {
    return false;
  }
  (void)memcpy(leaf, &path[slash], leaf_bytes);
  leaf[leaf_bytes] = '\0';
  if (strcmp(leaf, ".") == 0 || strcmp(leaf, "..") == 0) {
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
 * @brief Build one collision-resistant hidden temporary leaf.
 * @details Uses fixed decimal conversion buffers and no process-global state.
 * @param[out] out     Temporary-name buffer.
 * @param[in]  process Process identifier.
 * @param[in]  attempt Collision retry index.
 * @return true when the fixed buffer holds the complete name.
 * @retval true  A NUL-terminated slash-free leaf was built.
 * @retval false Capacity was insufficient.
 * @pre @p out has ::k_mkfontimg_host_name_cap bytes.
 * @pre Numeric arguments are finite.
 * @post No filesystem object is touched.
 * @post Name contents depend only on the arguments.
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static bool
internal_temp_name(char out[k_mkfontimg_host_name_cap], uint64_t process, uint32_t attempt)
{
  static const char s_prefix[] = ".mkfontimg.tmp.";
  char              reverse[k_host_decimal_digits];
  size_t            offset = sizeof(s_prefix) - 1U;
  (void)memcpy(out, s_prefix, offset);
  uint64_t values[2] = {process, attempt};
  for (uint8_t field = 0U; field < 2U; ++field) {
    size_t digits = 0U;
    do {
      reverse[digits++] = (char)('0' + (values[field] % (uint64_t)k_host_decimal_base));
      values[field] /= (uint64_t)k_host_decimal_base;
    } while (values[field] != 0U);
    if (offset + digits + 2U > (size_t)k_mkfontimg_host_name_cap) {
      return false;
    }
    while (digits > 0U) {
      out[offset++] = reverse[--digits];
    }
    out[offset++] = (field == 0U) ? '.' : '\0';
  }
  return true;
}

RA8_PRIV bool priv_mkfontimg_host_begin(const char*       final_path,
                                        uint64_t          block_count,
                                        uint32_t          block_size,
                                        mkfontimg_host_t* host)
{
  *host = (mkfontimg_host_t){.directory_fd = -1, .image_fd = -1};
  char parent[k_mkfontimg_host_path_cap];
  if (block_count == 0U || block_size == 0U || block_count > (UINT64_MAX / block_size) ||
      !internal_split_output(final_path, parent, host->final_name)) {
    return false;
  }
  const uint64_t image_bytes = block_count * block_size;
  if (image_bytes > (uint64_t)INT64_MAX) {
    return false;
  }
  host->directory_fd = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (host->directory_fd < 0) {
    return false;
  }
  for (uint32_t attempt = 0U; attempt < (uint32_t)k_host_temp_attempts; ++attempt) {
    if (!internal_temp_name(host->temp_name, (uint64_t)getpid(), attempt)) {
      break;
    }
    host->image_fd = openat(host->directory_fd,
                            host->temp_name,
                            O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                            (mode_t)k_host_create_mode);
    if (host->image_fd >= 0 || errno != EEXIST) {
      break;
    }
  }
  if (host->image_fd < 0) {
    priv_mkfontimg_host_abort(host);
    return false;
  }
  host->temp_exists = true;
  if (ftruncate(host->image_fd, (off_t)image_bytes) != 0) {
    priv_mkfontimg_host_abort(host);
    return false;
  }
  host->disk =
    (mkfontimg_disk_t){.block_count = block_count, .block_size = block_size, .fd = host->image_fd};
  host->backend = (ra8_fs_backend_t){.read_block   = internal_disk_read,
                                     .write_block  = internal_disk_write,
                                     .get_capacity = internal_disk_capacity,
                                     .ctx          = &host->disk};
  return true;
}

RA8_PRIV bool
priv_mkfontimg_host_seed(mkfontimg_host_t* host, const uint8_t* bytes, uint32_t length)
{
  if (host == nullptr || bytes == nullptr || host->image_fd < 0 ||
      (uint64_t)length > (host->disk.block_count * host->disk.block_size)) {
    return false;
  }
  if (!internal_pwrite_exact(host->image_fd, 0U, bytes, length)) {
    host->disk.io_failed = true;
    return false;
  }
  return true;
}

/**
 * @brief Capture all mutation-sensitive fields from one successful `fstat`.
 * @details Normalizes Darwin and POSIX timestamp layouts into an owned value.
 * @param[in] metadata Successful metadata for a regular file.
 * @return Captured device, inode, size, and nanosecond timestamps.
 * @retval all-fields A complete identity value with no borrowed storage.
 * @pre @p metadata is non-null and has a non-negative size.
 * @pre The platform timestamp layout matches the selected compile-time branch.
 * @post No state or descriptor position changes.
 * @post The returned value owns no resource.
 * @note Pure and thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static host_input_identity_t internal_identity(const struct stat* metadata)
{
#if defined(__APPLE__)
  const long modified_nsec = metadata->st_mtimespec.tv_nsec;
  const long changed_nsec  = metadata->st_ctimespec.tv_nsec;
#else
  const long modified_nsec = metadata->st_mtim.tv_nsec;
  const long changed_nsec  = metadata->st_ctim.tv_nsec;
#endif
  return (host_input_identity_t){.device        = metadata->st_dev,
                                 .inode         = metadata->st_ino,
                                 .size          = (uint64_t)metadata->st_size,
                                 .modified_sec  = metadata->st_mtime,
                                 .modified_nsec = modified_nsec,
                                 .changed_sec   = metadata->st_ctime,
                                 .changed_nsec  = changed_nsec};
}

/**
 * @brief Compare two initialized input identities without inspecting padding.
 * @details Checks every mutation-sensitive value explicitly.
 * @param[in] left First captured identity.
 * @param[in] right Second captured identity.
 * @return Whether both identities describe the same stable file state.
 * @retval true Every represented field is equal.
 * @retval false At least one identity, size, or timestamp differs.
 * @pre Both pointers are non-null.
 * @pre Both values came from ::internal_identity.
 * @post Neither input is changed.
 * @post Structure padding is never read.
 * @note Pure and thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_identity_equal(const host_input_identity_t* left,
                                                 const host_input_identity_t* right)
{
  return left->device == right->device && left->inode == right->inode &&
         left->size == right->size && left->modified_sec == right->modified_sec &&
         left->modified_nsec == right->modified_nsec && left->changed_sec == right->changed_sec &&
         left->changed_nsec == right->changed_nsec;
}

/**
 * @brief Open and validate one non-symlink regular input.
 * @details Captures identity through the opened no-follow descriptor.
 * @param[in]  path          Input path.
 * @param[in]  minimum_bytes Inclusive lower size bound.
 * @param[in]  maximum_bytes Inclusive upper size bound.
 * @param[out] out_fd        Receives owned descriptor.
 * @param[out] identity      Receives stable metadata.
 * @return true only for a bounded regular file.
 * @retval true  Both outputs are populated.
 * @retval false Open, type, or size validation failed.
 * @pre All pointers are non-NULL.
 * @pre Minimum does not exceed maximum.
 * @post On true caller owns @p out_fd.
 * @post On false no descriptor remains open.
 * @note Thread-safe for independent outputs.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_input_open(const char*            path,
                                             uint64_t               minimum_bytes,
                                             uint64_t               maximum_bytes,
                                             int*                   out_fd,
                                             host_input_identity_t* identity)
{
  const int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) {
    return false;
  }
  struct stat metadata = {};
  if (fstat(fd, &metadata) != 0 || !S_ISREG(metadata.st_mode) || metadata.st_size < 0 ||
      (uint64_t)metadata.st_size < minimum_bytes || (uint64_t)metadata.st_size > maximum_bytes) {
    (void)close(fd);
    return false;
  }
  *identity = internal_identity(&metadata);
  *out_fd   = fd;
  return true;
}

/**
 * @brief Stream the first pass from host input to one new card file.
 * @details Copies bounded chunks and rechecks metadata before returning success.
 * @param[in]     input_fd Open host input.
 * @param[in]     identity Initial stable metadata.
 * @param[in,out] card     Open card file in write mode.
 * @return true only when every byte was written and metadata stayed stable.
 * @retval true  Card file received exactly `identity->size` bytes.
 * @retval false Read, write, or mutation validation failed.
 * @pre All arguments are valid and both handles remain open.
 * @pre Card offset and input positioned offset both begin at zero.
 * @post Host descriptor position remains unchanged.
 * @post On false only the unpublished image may contain a partial card file.
 * @note Not thread-safe through the card handle.
 * @since 0.1.0
 */
RA8_INTERNAL static bool
internal_stream(int input_fd, const host_input_identity_t* identity, ra8_fs_file_t* card)
{
  uint8_t  chunk[k_host_stream_chunk];
  uint64_t offset = 0U;
  bool     ok     = true;
  while (ok && offset < identity->size) {
    uint32_t take = (uint32_t)(identity->size - offset);
    if (take > (uint32_t)sizeof(chunk)) {
      take = (uint32_t)sizeof(chunk);
    }
    ok = internal_pread_exact(input_fd, offset, chunk, take) &&
         ra8_fs_write(card, chunk, take) == k_ra8_ok;
    offset += take;
  }
  struct stat after = {};
  if (fstat(input_fd, &after) != 0) {
    ok = false;
  } else {
    const host_input_identity_t observed = internal_identity(&after);
    if (!internal_identity_equal(identity, &observed)) {
      ok = false;
    }
  }
  return ok;
}

/**
 * @brief Compare a second host-input pass with the generated card file.
 * @details Verifies every byte and repeats the mutation-sensitive metadata check.
 * @param[in]     input_fd Open stable input.
 * @param[in]     identity Original identity.
 * @param[in,out] card     Open card file in read mode.
 * @return true only when both streams match exactly.
 * @retval true  Every byte and final metadata match.
 * @retval false Read, size, content, or mutation validation failed.
 * @pre All arguments are valid and both handles begin at offset zero.
 * @pre Card size already equals `identity->size`.
 * @post Host descriptor position remains unchanged.
 * @post Card offset advances by the verified length.
 * @note Not thread-safe through the card handle.
 * @since 0.1.0
 */
RA8_INTERNAL static bool
internal_compare(int input_fd, const host_input_identity_t* identity, ra8_fs_file_t* card)
{
  uint8_t  source[k_host_stream_chunk];
  uint8_t  copied[k_host_stream_chunk];
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
  if (fstat(input_fd, &after) != 0) {
    ok = false;
  } else {
    const host_input_identity_t observed = internal_identity(&after);
    if (!internal_identity_equal(identity, &observed)) {
      ok = false;
    }
  }
  return ok;
}

RA8_PRIV bool priv_mkfontimg_host_copy(mkfontimg_host_t* host,
                                       ra8_fs_mount_t*   mount,
                                       const char*       input_path,
                                       const char*       card_name,
                                       uint64_t          minimum_bytes,
                                       uint64_t          maximum_bytes,
                                       uint64_t*         out_bytes)
{
  int                   input_fd = -1;
  host_input_identity_t identity;
  if (!internal_input_open(input_path, minimum_bytes, maximum_bytes, &input_fd, &identity)) {
    return false;
  }
  ra8_fs_file_t* card = nullptr;
  bool           ok   = ra8_fs_open(mount, card_name, k_ra8_fs_mode_write, &card) == k_ra8_ok;
  if (ok) {
    ok = internal_stream(input_fd, &identity, card);
    if (ra8_fs_close(card) != k_ra8_ok) {
      ok = false;
    }
  }
  if (close(input_fd) != 0) {
    ok = false;
  }
  if (!ok) {
    return false;
  }

  host_input_identity_t again;
  if (!internal_input_open(input_path, minimum_bytes, maximum_bytes, &input_fd, &again) ||
      !internal_identity_equal(&identity, &again)) {
    if (input_fd >= 0) {
      (void)close(input_fd);
    }
    return false;
  }
  uint64_t card_size = 0U;
  card               = nullptr;
  ok                 = ra8_fs_open(mount, card_name, k_ra8_fs_mode_read, &card) == k_ra8_ok &&
                       ra8_fs_size(card, &card_size) == k_ra8_ok && card_size == identity.size;
  if (ok) {
    ok = internal_compare(input_fd, &identity, card);
  }
  if (card != nullptr && ra8_fs_close(card) != k_ra8_ok) {
    ok = false;
  }
  if (close(input_fd) != 0) {
    ok = false;
  }
  if (host->disk.io_failed) {
    ok = false;
  }
  if (ok) {
    *out_bytes = identity.size;
  }
  return ok;
}

RA8_PRIV bool priv_mkfontimg_host_commit(mkfontimg_host_t* host)
{
  bool ok = !host->disk.io_failed && fsync(host->image_fd) == 0;
  if (close(host->image_fd) != 0) {
    ok = false;
  }
  host->image_fd = -1;
  if (ok &&
      renameat(host->directory_fd, host->temp_name, host->directory_fd, host->final_name) == 0) {
    host->temp_exists = false;
    if (fsync(host->directory_fd) != 0) {
      ok = false;
    }
  } else {
    ok = false;
  }
  if (host->temp_exists) {
    (void)unlinkat(host->directory_fd, host->temp_name, 0);
    host->temp_exists = false;
  }
  if (close(host->directory_fd) != 0) {
    ok = false;
  }
  host->directory_fd = -1;
  return ok;
}

RA8_PRIV void priv_mkfontimg_host_abort(mkfontimg_host_t* host)
{
  if (host->image_fd >= 0) {
    (void)close(host->image_fd);
    host->image_fd = -1;
  }
  if (host->temp_exists && host->directory_fd >= 0) {
    (void)unlinkat(host->directory_fd, host->temp_name, 0);
    host->temp_exists = false;
  }
  if (host->directory_fd >= 0) {
    (void)close(host->directory_fd);
    host->directory_fd = -1;
  }
}
