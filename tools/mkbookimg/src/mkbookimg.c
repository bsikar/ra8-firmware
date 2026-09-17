/**
 * @file src/mkbookimg.c
 * @brief Host tool that streams compiled books into an atomic FAT32 image
 * @details Streams a 64 MiB FAT32 image through firmware `ra8_fs` over a sparse sibling.
 * Fixed chunks are verified before sync/rename; no POSIX type crosses into reusable code.
 * This unit owns the host container: descriptor-level diagnostics, exact positioned
 * I/O, the block device, the atomic sibling-temporary publication, and `main`. The
 * population pass that streams and verifies each book is `src/mkbookimg_books.c`; the
 * seam between the two is inc/mkbookimg_internal.h.
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

#include "mkbookimg_internal.h"
#include "mkbookimg_names.h"
#include "ra8_attributes.h"
#include "ra8_fs.h"

/** @brief Caller-owned atomic-publication state. */
typedef struct {
  char final_name[k_host_name_cap]; /**< Destination leaf.             */
  char temp_name[k_host_name_cap];  /**< Owned temporary leaf.         */
  int  directory_fd;                /**< Destination-directory handle. */
  int  image_fd;                    /**< Open temporary image.         */
  bool temp_exists;                 /**< Temp leaf requires unlink.    */
} mkbookimg_output_t;

RA8_PRIV
void priv_mkbookimg_diag(const char* text)
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

RA8_PRIV
void priv_mkbookimg_diag_u64(uint64_t value)
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
  priv_mkbookimg_diag(forward);
}

RA8_PRIV
bool priv_mkbookimg_pread_exact(int fd, uint64_t offset, uint8_t* bytes, size_t length)
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
  if (!priv_mkbookimg_pread_exact(disk->fd, offset, buffer, bytes)) {
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
 * @brief Print the final success diagnostic line.
 * @details Emits the published path and the book count as one bounded run of
 *          diagnostic fragments through the same descriptor-level writer every
 *          failure message uses, so the two can never reach different sinks.
 * @param[in] out_path Published destination path.
 * @param[in] book_count Number of books written.
 * @return Nothing.
 * @pre @p out_path is a NUL-terminated string.
 * @pre The publication rename already succeeded, so the path names the image.
 * @post Exactly one diagnostic line has been written.
 * @post No descriptor, image, or publication state is inspected or changed.
 * @note Not thread-safe for a shared diagnostic sink.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_print_success(const char* out_path, int book_count)
{
  priv_mkbookimg_diag("mkbookimg: wrote ");
  priv_mkbookimg_diag(out_path);
  priv_mkbookimg_diag(" (");
  priv_mkbookimg_diag_u64((uint64_t)book_count);
  priv_mkbookimg_diag(" book(s), 64 MiB FAT32)\n");
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
    priv_mkbookimg_diag("usage: mkbookimg <out.img> <book1.rabook> [book2.rabook ...]\n");
    return 2;
  }
  const int book_count = argc - 2;
  if (book_count > (int)k_max_books) {
    priv_mkbookimg_diag("mkbookimg: too many books (max 32)\n");
    return 2;
  }

  mkbookimg_output_t output;
  if (!internal_output_begin(argv[1], &output)) {
    priv_mkbookimg_diag("mkbookimg: cannot create a safe sibling temporary for ");
    priv_mkbookimg_diag(argv[1]);
    priv_mkbookimg_diag("\n");
    return 1;
  }
  mkbookimg_disk_t       disk    = {.block_count = k_img_sectors,
                                    .block_size  = k_block_size,
                                    .fd          = output.image_fd};
  const ra8_fs_backend_t backend = {.read_block   = internal_disk_read,
                                    .write_block  = internal_disk_write,
                                    .get_capacity = internal_disk_capacity,
                                    .ctx          = &disk};
  bool                   ok      = priv_mkbookimg_build_image(&backend, argv, book_count, &disk);
  if (ok) {
    ok = internal_output_commit(&output);
  } else {
    internal_output_abort(&output);
  }
  if (!ok) {
    priv_mkbookimg_diag("mkbookimg: image generation failed; destination preserved\n");
    return 1;
  }
  internal_print_success(argv[1], book_count);
  return 0;
}
