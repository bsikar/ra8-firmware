/**
 * @file tools/exfat_mkimage/exfat_mkimage.c
 * @brief Build an atomic deterministic exFAT showcase image through ra8_fs
 *
 * @par Tag
 * [Ring 7 / TOOL] {World: NS}
 *
 * @details
 * Formats and populates a sparse sibling-temporary block device through the
 * same callback-driven `ra8_fs` code used by firmware. The 64 MiB card is never
 * resident: exact positioned descriptor I/O backs the block callbacks, and a
 * complete synced temporary is atomically renamed over the destination only
 * after the filesystem unmounts cleanly. The UTF-8 showcase content and final
 * bytes remain identical to the legacy RAM-backed generator.
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

/** @brief Image geometry and bounded hosted-storage constants. */
typedef enum : uint32_t {
  k_mk_block_size     = 512U,    /**< Bytes per image block.        */
  k_mk_block_count    = 131072U, /**< Blocks in the 64 MiB image.   */
  k_mk_payload        = 32U,     /**< Bytes in each showcase file.  */
  k_mk_alphabet       = 26U,     /**< ASCII payload cycle length.   */
  k_mk_path_cap       = 4096U,   /**< Hosted path capacity.         */
  k_mk_name_cap       = 256U,    /**< Hosted leaf capacity.         */
  k_mk_temp_attempts  = 128U,    /**< Exclusive-create retry bound. */
  k_mk_decimal_digits = 20U,     /**< Maximum `uint64_t` digits.    */
  k_mk_decimal_base   = 10U,     /**< Decimal conversion radix.     */
  k_mk_create_mode    = 0666U,   /**< Hosted output creation mode.  */
} mk_limit_t;

/** @brief Descriptor-backed card state bound into `ra8_fs`. */
typedef struct {
  uint64_t block_count; /**< Addressable logical blocks.      */
  uint32_t block_size;  /**< Bytes per logical block.         */
  int      fd;          /**< Open temporary image descriptor. */
  bool     io_failed;   /**< Sticky positioned-I/O failure.   */
} mk_disk_t;

/** @brief Caller-owned atomic-publication state. */
typedef struct {
  char final_name[k_mk_name_cap]; /**< Destination leaf.        */
  char temp_name[k_mk_name_cap];  /**< Hidden temporary leaf.   */
  int  directory_fd;              /**< Parent directory handle. */
  int  image_fd;                  /**< Temporary image handle.  */
  bool temp_exists;               /**< Temp unlink guard.       */
} mk_output_t;

/** @brief One showcase volume entry. */
typedef struct {
  const char* path;    /**< Absolute UTF-8 volume path. */
  const char* meaning; /**< Human-readable description. */
  bool        is_dir;  /**< True for a directory.       */
} mk_entry_t;

/** @brief Deterministic UTF-8 showcase contents. */
static const mk_entry_t s_entries[] = {
  {"/\xD0\x9F\xD0\xB0\xD0\xBF\xD0\xBA\xD0\xB0", "Cyrillic directory (Papka)", true},
  {"/\xE4\xBD\xA0\xE5\xA5\xBD", "CJK directory (ni hao)", true},
  {"/Caf\xC3\xA9.txt", "Latin-1 accent (Cafe.txt)", false},
  {"/\xE4\xBD\xA0\xE5\xA5\xBD.txt", "CJK filename", false},
  {"/\xF0\x9F\x98\x80.txt", "astral-plane emoji, a surrogate pair on disk", false},
  {"/\xD0\x9F\xD0\xB0\xD0\xBF\xD0\xBA\xD0\xB0/n\xC3\xB6te.txt",
   "non-ASCII file inside a non-ASCII directory",
   false},
  {"/README.txt", "plain ASCII control", false},
};

/**
 * @brief Write one complete diagnostic fragment to a raw descriptor.
 * @details Retries interrupted and short writes without using stdio streams.
 * @param[in] fd   Standard output or standard error descriptor.
 * @param[in] text NUL-terminated fragment.
 * @pre @p text is non-NULL and NUL-terminated.
 * @pre @p fd may accept or reject the write.
 * @post The complete fragment was attempted with short/EINTR handling.
 * @post No tool state changed.
 * @note Fragments may interleave with another process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_log(int fd, const char* text)
{
  size_t offset = 0U;
  size_t length = strlen(text);
  while (offset < length) {
    const ssize_t written = write(fd, &text[offset], length - offset);
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
 * @brief Log one unsigned decimal value without a formatting stream.
 * @details Converts through fixed local arrays before one bounded log operation.
 * @param[in] fd    Standard output or standard error descriptor.
 * @param[in] value Value to render.
 * @pre Fixed digit capacity covers every uint64 value.
 * @pre @p fd may accept or reject the write.
 * @post Decimal spelling was attempted.
 * @post No tool state changed.
 * @note Fragments may interleave with another process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_log_u64(int fd, uint64_t value)
{
  char   reverse[k_mk_decimal_digits];
  char   forward[k_mk_decimal_digits + 1U];
  size_t digits = 0U;
  do {
    reverse[digits++] = (char)('0' + (value % (uint64_t)k_mk_decimal_base));
    value /= (uint64_t)k_mk_decimal_base;
  } while (value != 0U);
  for (size_t i = 0U; i < digits; ++i) {
    forward[i] = reverse[digits - i - 1U];
  }
  forward[digits] = '\0';
  internal_log(fd, forward);
}

/**
 * @brief Read an exact positioned range from a regular descriptor.
 * @details Retries interruptions and rejects EOF before the requested length.
 * @param[in]  fd     Open image descriptor.
 * @param[in]  offset Absolute byte offset.
 * @param[out] bytes  Exact-size destination.
 * @param[in]  length Required bytes.
 * @return true only when every byte was read.
 * @retval true  Destination is complete.
 * @retval false Overflow, EOF, or hosted I/O failure occurred.
 * @pre @p bytes spans @p length writable bytes.
 * @pre @p fd remains open.
 * @post Descriptor position is unchanged.
 * @post On false destination may hold a strict prefix.
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
 * @brief Write an exact positioned range to a regular descriptor.
 * @details Retries interruptions and treats a zero write as terminal failure.
 * @param[in] fd     Open image descriptor.
 * @param[in] offset Absolute byte offset.
 * @param[in] bytes  Exact-size source.
 * @param[in] length Required bytes.
 * @return true only when every byte was written.
 * @retval true  Complete range reached the page cache.
 * @retval false Overflow or hosted I/O failure occurred.
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
 * @brief Translate one sector range into bounded hosted byte coordinates.
 * @details Checks geometry and every multiplication and host conversion first.
 * @param[in]  disk       Bound geometry.
 * @param[in]  lba        First block.
 * @param[in]  count      Block count.
 * @param[out] out_offset Byte offset.
 * @param[out] out_bytes  Byte count.
 * @return true only when the complete range is representable.
 * @retval true  Both outputs are populated.
 * @retval false Geometry or host integer bounds reject the range.
 * @pre All pointers are non-NULL and geometry is non-zero.
 * @pre Count is the callback's requested extent.
 * @post Outputs change only on true.
 * @post Disk state remains unchanged.
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_block_range(const mk_disk_t* disk,
                                              uint64_t         lba,
                                              uint32_t         count,
                                              uint64_t*        out_offset,
                                              size_t*          out_bytes)
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
 * @brief Read blocks from the sparse unpublished image.
 * @details Validates the complete range, performs exact positioned I/O, and
 * records the first host failure in the transaction context.
 * @param[in,out] ctx Bound ::mk_disk_t context.
 * @param[in] lba First block to read.
 * @param[in] count Number of blocks to read.
 * @param[out] buffer Destination spanning `count * block_size` bytes.
 * @return Canonical block-device status.
 * @retval k_ra8_ok Every requested block was read.
 * @retval k_ra8_err_out_of_range State, pointer, or geometry was invalid.
 * @retval k_ra8_fail Exact host I/O failed.
 * @pre @p ctx and @p buffer are non-null.
 * @pre The bound descriptor remains open.
 * @post Success initializes the complete destination range.
 * @post Host failure leaves the disk error flag set.
 * @note Not thread-safe through the shared sticky-error flag.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_disk_read(void* ctx, uint64_t lba, uint32_t count, uint8_t* buffer)
{
  mk_disk_t* disk = (mk_disk_t*)ctx;
  uint64_t   offset;
  size_t     bytes;
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
 * @brief Write blocks into the sparse unpublished image.
 * @details Validates the complete range, performs exact positioned I/O, and
 * records the first host failure in the transaction context.
 * @param[in,out] ctx Bound ::mk_disk_t context.
 * @param[in] lba First block to write.
 * @param[in] count Number of blocks to write.
 * @param[in] buffer Source spanning `count * block_size` bytes.
 * @return Canonical block-device status.
 * @retval k_ra8_ok Every requested block was written.
 * @retval k_ra8_err_out_of_range State, pointer, or geometry was invalid.
 * @retval k_ra8_fail Exact host I/O failed.
 * @pre @p ctx and @p buffer are non-null.
 * @pre The bound descriptor remains open.
 * @post Success updates only the requested byte range.
 * @post Host failure leaves the disk error flag set.
 * @note Not thread-safe for overlapping writes or shared error state.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_disk_write(void* ctx, uint64_t lba, uint32_t count, const uint8_t* buffer)
{
  mk_disk_t* disk = (mk_disk_t*)ctx;
  uint64_t   offset;
  size_t     bytes;
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
 * @brief Report the fixed card-image geometry.
 * @details Copies immutable block count and size fields into caller outputs.
 * @param[in] ctx Bound ::mk_disk_t context.
 * @param[out] block_count Receives the number of blocks.
 * @param[out] block_size Receives bytes per block.
 * @return Canonical block-device status.
 * @retval k_ra8_ok Both outputs were initialized.
 * @retval k_ra8_err_null_ptr A required pointer was null.
 * @pre All three pointers are non-null.
 * @pre Context geometry was initialized before backend binding.
 * @post Success populates both outputs.
 * @post Context and descriptor state are unchanged.
 * @note Read-only and thread-safe while the context remains alive.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_disk_capacity(void* ctx, uint64_t* block_count, uint32_t* block_size)
{
  const mk_disk_t* disk = (const mk_disk_t*)ctx;
  if (disk == nullptr || block_count == nullptr || block_size == nullptr) {
    return k_ra8_err_null_ptr;
  }
  *block_count = disk->block_count;
  *block_size  = disk->block_size;
  return k_ra8_ok;
}

/**
 * @brief Split a destination into bounded parent and leaf components.
 * @details Rejects truncation, empty leaves, and dot traversal components.
 * @param[in]  path   Requested output.
 * @param[out] parent Parent path.
 * @param[out] leaf   Destination leaf.
 * @return true only for a bounded usable leaf.
 * @retval true  Outputs are populated.
 * @retval false Path is empty, long, or names `.`/`..`/a directory.
 * @pre All arguments are non-NULL and outputs have fixed capacities.
 * @pre @p path is NUL-terminated.
 * @post No filesystem object is touched.
 * @post On true parent and leaf reconstruct @p path.
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static bool
internal_split_output(const char* path, char parent[k_mk_path_cap], char leaf[k_mk_name_cap])
{
  const size_t length = strlen(path);
  if (length == 0U || length >= (size_t)k_mk_path_cap) {
    return false;
  }
  size_t slash = length;
  while (slash > 0U && path[slash - 1U] != '/') {
    --slash;
  }
  const size_t leaf_bytes = length - slash;
  if (leaf_bytes == 0U || leaf_bytes >= (size_t)k_mk_name_cap) {
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
 * @brief Build one hidden temporary leaf from process id and retry index.
 * @details Uses fixed decimal buffers so collision handling allocates nothing.
 * @param[out] out     Temporary leaf.
 * @param[in]  process Process identifier.
 * @param[in]  attempt Collision retry index.
 * @return true only when the complete name fits.
 * @retval true  @p out is NUL-terminated and slash-free.
 * @retval false Fixed capacity is insufficient.
 * @pre Output has ::k_mk_name_cap bytes.
 * @pre Numeric inputs are finite.
 * @post No filesystem object is touched.
 * @post Output depends only on arguments.
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static bool
internal_temp_name(char out[k_mk_name_cap], uint64_t process, uint32_t attempt)
{
  static const char s_prefix[] = ".exfat_mkimage.tmp.";
  char              reverse[k_mk_decimal_digits];
  size_t            offset = sizeof(s_prefix) - 1U;
  (void)memcpy(out, s_prefix, offset);
  uint64_t values[2] = {process, attempt};
  for (uint8_t field = 0U; field < 2U; ++field) {
    size_t digits = 0U;
    do {
      reverse[digits++] = (char)('0' + (values[field] % (uint64_t)k_mk_decimal_base));
      values[field] /= (uint64_t)k_mk_decimal_base;
    } while (values[field] != 0U);
    if (offset + digits + 2U > (size_t)k_mk_name_cap) {
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
 * @brief Create and exact-size one private sibling temporary.
 * @details Uses exclusive no-follow creation and unwinds partial acquisition.
 * @param[in]  path   Requested final path.
 * @param[out] output Receives publication state.
 * @return true when the sparse temporary is open.
 * @retval true  Caller owns the temporary and descriptors.
 * @retval false Validation or hosted setup failed without leakage.
 * @pre Both arguments are non-NULL and @p path is NUL-terminated.
 * @pre Fixed image geometry is representable by `off_t`.
 * @post On true image length is exactly 64 MiB.
 * @post On false an existing destination is unchanged.
 * @note Not thread-safe for the same destination path.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_output_begin(const char* path, mk_output_t* output)
{
  *output = (mk_output_t){.directory_fd = -1, .image_fd = -1};
  char parent[k_mk_path_cap];
  if (!internal_split_output(path, parent, output->final_name)) {
    return false;
  }
  output->directory_fd = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (output->directory_fd < 0) {
    return false;
  }
  for (uint32_t attempt = 0U; attempt < (uint32_t)k_mk_temp_attempts; ++attempt) {
    if (!internal_temp_name(output->temp_name, (uint64_t)getpid(), attempt)) {
      break;
    }
    output->image_fd = openat(output->directory_fd,
                              output->temp_name,
                              O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                              (mode_t)k_mk_create_mode);
    if (output->image_fd >= 0 || errno != EEXIST) {
      break;
    }
  }
  if (output->image_fd < 0) {
    (void)close(output->directory_fd);
    output->directory_fd = -1;
    return false;
  }
  output->temp_exists  = true;
  const uint64_t bytes = (uint64_t)k_mk_block_count * (uint64_t)k_mk_block_size;
  if (bytes > (uint64_t)INT64_MAX || ftruncate(output->image_fd, (off_t)bytes) != 0) {
    (void)close(output->image_fd);
    output->image_fd = -1;
    (void)unlinkat(output->directory_fd, output->temp_name, 0);
    output->temp_exists = false;
    (void)close(output->directory_fd);
    output->directory_fd = -1;
    return false;
  }
  return true;
}

/**
 * @brief Close and remove an unpublished temporary.
 * @details Idempotently releases the image, private leaf, and parent descriptor.
 * @param[in,out] output Partially initialized output state.
 * @pre @p output is non-NULL and no mount uses its descriptor.
 * @pre State came from ::internal_output_begin.
 * @post Every owned descriptor is closed.
 * @post Any owned temporary leaf is removed.
 * @note Not thread-safe through @p output.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_output_abort(mk_output_t* output)
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
 * @brief Sync and atomically publish one complete sibling temporary.
 * @details Syncs file data before rename and parent metadata after publication.
 * @param[in,out] output Complete output state no longer used by a mount.
 * @return true when file sync, close, rename, and directory sync succeed.
 * @retval true  Final leaf names the complete image.
 * @retval false Publication failed or durability is unconfirmed.
 * @pre @p output owns a complete unmounted image.
 * @pre No further block callback will use its descriptor.
 * @post All descriptors are closed.
 * @post Successful rename leaves no temporary leaf.
 * @note Directory-sync failure can follow a complete visible rename.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_output_commit(mk_output_t* output)
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
 * @brief Populate every deterministic UTF-8 showcase entry.
 * @details Creates entries in table order with one fixed bounded payload.
 * @param[in,out] mount Mounted exFAT volume.
 * @return `ra8_fs` status.
 * @retval k_ra8_ok Every entry was created.
 * @retval other    First filesystem failure.
 * @pre @p mount is live and writable.
 * @pre Entry table ordering remains deterministic.
 * @post On success the complete showcase exists.
 * @post Each file holds the same fixed payload.
 * @note Not thread-safe through `ra8_fs` global slots.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_populate(ra8_fs_mount_t* mount)
{
  uint8_t payload[k_mk_payload];
  for (uint32_t i = 0U; i < (uint32_t)k_mk_payload; ++i) {
    payload[i] = (uint8_t)('A' + (i % (uint32_t)k_mk_alphabet));
  }
  for (size_t i = 0U; i < (sizeof(s_entries) / sizeof(s_entries[0])); ++i) {
    const mk_entry_t* entry = &s_entries[i];
    const ra8_err_t   error =
      entry->is_dir ? ra8_fs_mkdir(mount, entry->path)
                    : ra8_fs_write_file(mount, entry->path, payload, (uint32_t)k_mk_payload);
    if (error != k_ra8_ok) {
      internal_log(STDERR_FILENO, "exfat_mkimage: cannot create ");
      internal_log(STDERR_FILENO, entry->meaning);
      internal_log(STDERR_FILENO, " (error ");
      internal_log_u64(STDERR_FILENO, (uint64_t)error);
      internal_log(STDERR_FILENO, ")\n");
      return error;
    }
    internal_log(STDOUT_FILENO, "  created ");
    internal_log(STDOUT_FILENO, entry->path);
    internal_log(STDOUT_FILENO, "  ");
    internal_log(STDOUT_FILENO, entry->meaning);
    internal_log(STDOUT_FILENO, "\n");
  }
  return k_ra8_ok;
}

/**
 * @brief Format, populate, verify closure, and atomically publish one image.
 * @param[in] argc Argument count with at most one optional output path.
 * @param[in] argv Argument vector.
 * @return Process status.
 * @retval 0 Complete deterministic exFAT image was published.
 * @retval 1 Hosted storage or filesystem operation failed.
 * @retval 2 Too many command-line arguments were supplied.
 * @pre Hosted runtime supplies @p argc valid strings in @p argv.
 * @pre Destination parent directory exists.
 * @post On zero output has exact fixed image length.
 * @post Before rename, failure preserves an existing destination.
 * @note Not thread-safe through `ra8_fs` global slots.
 * @since 0.1.0
 */
int main(int argc, char** argv)
{
  if (argc > 2) {
    internal_log(STDERR_FILENO, "usage: exfat_mkimage [output.img]\n");
    return 2;
  }
  const char* out_path = (argc == 2) ? argv[1] : "exfat_showcase.img";
  mk_output_t output;
  if (!internal_output_begin(out_path, &output)) {
    internal_log(STDERR_FILENO, "exfat_mkimage: cannot create safe sibling temporary\n");
    return 1;
  }
  mk_disk_t                  disk    = {.block_count = k_mk_block_count,
                                        .block_size  = k_mk_block_size,
                                        .fd          = output.image_fd};
  const ra8_fs_backend_t     backend = {.read_block   = internal_disk_read,
                                        .write_block  = internal_disk_write,
                                        .get_capacity = internal_disk_capacity,
                                        .ctx          = &disk};
  const ra8_fs_format_opts_t options = {.type = k_ra8_fs_type_exfat, .label = "RA8IMAGE"};
  ra8_fs_mount_t*            mount   = nullptr;
  ra8_err_t                  error   = ra8_fs_format(&backend, &options);
  if (error == k_ra8_ok) {
    error = ra8_fs_mount(&backend, &mount);
  }
  if (error == k_ra8_ok) {
    error = internal_populate(mount);
  }
  if (mount != nullptr && ra8_fs_unmount(mount) != k_ra8_ok) {
    error = k_ra8_fail;
  }
  bool ok = error == k_ra8_ok && !disk.io_failed;
  if (ok) {
    ok = internal_output_commit(&output);
  } else {
    internal_output_abort(&output);
  }
  if (!ok) {
    internal_log(STDERR_FILENO, "exfat_mkimage: generation failed; destination preserved\n");
    return 1;
  }
  internal_log(STDOUT_FILENO, "wrote ");
  internal_log(STDOUT_FILENO, out_path);
  internal_log(STDOUT_FILENO, " (");
  internal_log_u64(STDOUT_FILENO, (uint64_t)k_mk_block_count * (uint64_t)k_mk_block_size);
  internal_log(STDOUT_FILENO, " bytes) -- the card exactly as ra8_fs left it\n");
  return 0;
}
