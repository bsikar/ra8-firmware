/**
 * @file src/mkbookimg_books.c
 * @brief Stream every requested book into the mounted image and prove the copy.
 *
 * @details
 * Owns the population pass of an image build: opening each immutable regular
 * input, capturing the identity fields that expose replacement or mutation,
 * streaming the bytes through one bounded stack chunk into an `ra8_fs` file,
 * then re-reading both sides so no book is published unless it was reproduced
 * byte-for-byte from an input that never moved under the tool. The host
 * container that owns the image file, its block device, and the atomic
 * publication rename is `src/mkbookimg.c`; see inc/mkbookimg_internal.h for the seam.
 *
 * [Ring 7 / Tooling] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "mkbookimg_internal.h"
#include "mkbookimg_names.h"
#include "ra8_attributes.h"
#include "ra8_fs.h"

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
#ifdef __APPLE__
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
    ok = priv_mkbookimg_pread_exact(input_fd, offset, chunk, take) &&
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
 * @brief Reopen host and card handles and confirm identity and size.
 * @details Reopens the host input, confirms it is unchanged since the write
 *          pass, then opens the card copy and confirms its size matches.
 * @param[in,out] mount Mounted FAT32 image.
 * @param[in] name Card basename written by ::internal_stream_book.
 * @param[in] path Original host path.
 * @param[in] identity Identity observed before the write pass.
 * @param[out] out_input_fd Receives the reopened host descriptor, or -1.
 * @param[out] out_card Receives the opened card file, or NULL.
 * @return Whether both handles are open and identity/size checks pass.
 * @retval true @p out_input_fd and @p out_card are open and consistent.
 * @retval false Reopen, identity, open, or size check failed; every handle
 *         this call itself opened has already been closed.
 * @pre All strings and @p identity are valid.
 * @pre @p mount remains mounted.
 * @post On false, no handle this call opened remains open.
 * @post On true, both handles are the caller's to close.
 * @note Not thread-safe; `ra8_fs` owns shared static mount/file slots.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_verify_book_reopen(ra8_fs_mount_t*                   mount,
                                                     const char*                       name,
                                                     const char*                       path,
                                                     const mkbookimg_input_identity_t* identity,
                                                     int*                              out_input_fd,
                                                     ra8_fs_file_t**                   out_card)
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
  *out_input_fd = input_fd;
  *out_card     = card;
  return true;
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
  int            input_fd = -1;
  ra8_fs_file_t* card     = nullptr;
  if (!internal_verify_book_reopen(mount, name, path, identity, &input_fd, &card)) {
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
    ok           = priv_mkbookimg_pread_exact(input_fd, offset, source, take) &&
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
    priv_mkbookimg_diag("mkbookimg: ra8_fs_format failed\n");
    return false;
  }
  if (ra8_fs_mount(backend, mount) != k_ra8_ok) {
    priv_mkbookimg_diag("mkbookimg: ra8_fs_mount failed\n");
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
      priv_mkbookimg_diag("mkbookimg: unusable card name for ");
      priv_mkbookimg_diag(arguments[2 + index]);
      priv_mkbookimg_diag("\n");
      return false;
    }
    if (internal_duplicate_name(arguments, index)) {
      priv_mkbookimg_diag("mkbookimg: duplicate card name ");
      priv_mkbookimg_diag(name);
      priv_mkbookimg_diag("\n");
      return false;
    }
    mkbookimg_input_identity_t identity;
    if (!internal_stream_book(mount, name, arguments[2 + index], &identity) ||
        !internal_verify_book(mount, name, arguments[2 + index], &identity)) {
      priv_mkbookimg_diag("mkbookimg: input changed or could not be copied exactly: ");
      priv_mkbookimg_diag(arguments[2 + index]);
      priv_mkbookimg_diag("\n");
      return false;
    }
    priv_mkbookimg_diag("mkbookimg: + ");
    priv_mkbookimg_diag(name);
    priv_mkbookimg_diag("  (");
    priv_mkbookimg_diag_u64(identity.size);
    priv_mkbookimg_diag(" bytes)  <- ");
    priv_mkbookimg_diag(arguments[2 + index]);
    priv_mkbookimg_diag("\n");
  }
  return true;
}

RA8_PRIV
bool priv_mkbookimg_build_image(const ra8_fs_backend_t* backend,
                                char**                  argv,
                                int                     book_count,
                                const mkbookimg_disk_t* disk)
{
  ra8_fs_mount_t* mount = nullptr;
  bool            ok    = internal_format_mount(backend, &mount);
  if (ok) {
    ok = internal_write_books(mount, argv, book_count);
    if (ra8_fs_unmount(mount) != k_ra8_ok) {
      ok = false;
    }
  }
  if (disk->io_failed) {
    ok = false;
  }
  return ok;
}
