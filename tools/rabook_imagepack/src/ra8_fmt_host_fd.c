/**
 * @file ra8_fmt_host_fd.c
 * @brief Fail-closed raw-fd host composition for portable format engines.
 * @details Adapts bounded positioned reads, exact writes, and same-directory
 * durable transactions at the host edge without exposing hosted streams.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#define _POSIX_C_SOURCE (200809L)

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "ra8_attributes.h"
#include "ra8_fmt_host_fd_internal.h"

/* The platform headers omit renameat() without a hosted-stream include. Keep
 * the narrow system prototype at this raw-descriptor boundary instead. */
extern int renameat(int old_dir_fd, const char* old_path, int new_dir_fd, const char* new_path);

/** @brief Host adapter bounds. */
typedef enum : uint32_t {
  k_host_create_attempts = 16U,   /**< Collision-bounded stage creation. */
  k_host_decimal_radix   = 10U,   /**< Decimal name digit radix.         */
  k_host_mode_private    = 0600U, /**< Staging-file permissions.         */
} host_const_t;

/**
 * @brief Perform positioned reads with exact offset handling and legal short EOF.
 * @details Clamps requests to the captured regular-file size, retries interruptions,
 * and reports bytes read without changing descriptor position.
 * @param[in,out] ctx Bound ::ra8_fmt_host_source_t.
 * @param[in] offset Absolute source offset.
 * @param[out] bytes Destination spanning @p len bytes when non-empty.
 * @param[in] len Requested byte count.
 * @param[out] got Receives actual bytes read.
 * @return Canonical source status.
 * @retval k_ra8_ok Read succeeded, including bounded EOF short reads.
 * @retval k_ra8_err_null_ptr Required context/output or non-empty buffer was null.
 * @retval k_ra8_fail Host positioned read failed.
 * @pre @p ctx and @p got are non-null and @p bytes is valid for non-zero @p len.
 * @pre Source descriptor remains open and size is its captured regular-file extent.
 * @post @p got is initialized to the bytes transferred on every non-null path.
 * @post Descriptor stream position is unchanged.
 * @note Thread-safe for independent destinations while the descriptor remains open.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_pread(void* ctx, uint64_t offset, uint8_t* bytes, size_t len, size_t* got)
{
  ra8_fmt_host_source_t* source = (ra8_fmt_host_source_t*)ctx;
  if ((source == nullptr) || (got == nullptr) || ((bytes == nullptr) && (len != 0U))) {
    return k_ra8_err_null_ptr;
  }
  *got = 0U;
  if ((offset >= source->source.size) || (len == 0U)) {
    return k_ra8_ok;
  }
  uint64_t remain = source->source.size - offset;
  if ((uint64_t)len > remain) {
    len = (size_t)remain;
  }
  while (*got < len) {
    const ssize_t rc = pread(source->fd, &bytes[*got], len - *got, (off_t)(offset + *got));
    if (rc > 0) {
      *got += (size_t)rc;
    } else if (rc == 0) {
      break;
    } else if (errno != EINTR) {
      return k_ra8_fail;
    }
  }
  return k_ra8_ok;
}

/**
 * @brief Append all requested bytes to one descriptor.
 * @details Retries interruptions and short writes until the whole span is accepted.
 * @param[in,out] ctx Bound ::ra8_fmt_host_fd_sink_t.
 * @param[in] bytes Source spanning @p len bytes when non-empty.
 * @param[in] len Exact requested byte count.
 * @return Canonical sink status.
 * @retval k_ra8_ok Complete span was written.
 * @retval k_ra8_err_null_ptr Required context or non-empty source was null.
 * @retval k_ra8_fail Host write failed or returned zero.
 * @pre @p ctx is non-null and @p bytes is valid for non-zero @p len.
 * @pre Bound descriptor is open and writable.
 * @post Success advances descriptor position by exactly @p len.
 * @post Failure may leave a strict prefix in the caller-owned destination.
 * @note Not thread-safe through one shared descriptor position.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_fd_write(void* ctx, const uint8_t* bytes, size_t len)
{
  ra8_fmt_host_fd_sink_t* sink = (ra8_fmt_host_fd_sink_t*)ctx;
  if ((sink == nullptr) || ((bytes == nullptr) && (len != 0U))) {
    return k_ra8_err_null_ptr;
  }
  size_t done = 0U;
  while (done < len) {
    const ssize_t rc = write(sink->fd, &bytes[done], len - done);
    if (rc > 0) {
      done += (size_t)rc;
    } else if ((rc < 0) && (errno == EINTR)) {
      continue;
    } else {
      return k_ra8_fail;
    }
  }
  return k_ra8_ok;
}

/**
 * @brief Copy a bounded string slice and append NUL.
 * @details Rejects truncation before copying the exact payload and one terminator.
 * @param[out] out Destination buffer.
 * @param[in] cap Destination byte capacity.
 * @param[in] text Source slice.
 * @param[in] len Source payload bytes excluding NUL.
 * @return Canonical bounded-copy status.
 * @retval k_ra8_ok Complete slice and terminator were stored.
 * @retval k_ra8_err_invalid_size Destination capacity is insufficient.
 * @pre @p out spans @p cap bytes and @p text spans @p len bytes.
 * @pre Source and destination ranges do not overlap.
 * @post Success sets `out[len]` to NUL without touching later bytes.
 * @post Failure leaves destination unchanged.
 * @note Pure apart from caller output and thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_copy(char* out, size_t cap, const char* text, size_t len)
{
  if ((len + 1U) > cap) {
    return k_ra8_err_invalid_size;
  }
  (void)memcpy(out, text, len);
  out[len] = '\0';
  return k_ra8_ok;
}

/**
 * @brief Split a destination into a parent path and safe leaf name.
 * @details Rejects empty, overlong, directory-only, and dot traversal leaves.
 * @param[in] path Requested destination path.
 * @param[out] parent Receives bounded parent path.
 * @param[out] leaf Receives bounded final leaf.
 * @return Canonical path status.
 * @retval k_ra8_ok Both components were initialized.
 * @retval k_ra8_err_invalid_size Path/component is empty or exceeds capacity.
 * @retval k_ra8_err_invalid_arg Final leaf is `.` or `..`.
 * @pre Output arrays have their declared capacities.
 * @pre @p path is null or NUL-terminated.
 * @post Success outputs reconstruct the requested path.
 * @post Failure touches no filesystem object.
 * @note Pure and thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_split_path(const char* path,
                                     char        parent[k_ra8_fmt_host_path_cap],
                                     char        leaf[k_ra8_fmt_host_name_cap])
{
  const size_t len = (path == nullptr) ? 0U : strlen(path);
  if ((len == 0U) || (len >= (size_t)k_ra8_fmt_host_path_cap)) {
    return k_ra8_err_invalid_size;
  }
  const char*  slash    = strrchr(path, '/');
  const char*  name     = (slash == nullptr) ? path : &slash[1];
  const size_t name_len = len - (size_t)(name - path);
  if ((name_len == 0U) || ((name_len == 1U) && (name[0] == '.')) ||
      ((name_len == 2U) && (name[0] == '.') && (name[1] == '.'))) {
    return k_ra8_err_invalid_arg;
  }
  ra8_err_t rc = internal_copy(leaf, k_ra8_fmt_host_name_cap, name, name_len);
  if (rc != k_ra8_ok) {
    return rc;
  }
  if (slash == nullptr) {
    return internal_copy(parent, k_ra8_fmt_host_path_cap, ".", 1U);
  }
  const size_t parent_len = (slash == path) ? 1U : (size_t)(slash - path);
  return internal_copy(parent, k_ra8_fmt_host_path_cap, path, parent_len);
}

/**
 * @brief Append one unsigned decimal to a bounded stage name.
 * @details Converts through a fixed reverse buffer and refuses name truncation.
 * @param[in,out] name Existing bounded name buffer.
 * @param[in] cap Total name capacity.
 * @param[in,out] len Current length and resulting length.
 * @param[in] value Unsigned value to append in base ten.
 * @return Canonical formatting status.
 * @retval k_ra8_ok Digits and NUL terminator were appended.
 * @retval k_ra8_err_invalid_size Complete spelling does not fit.
 * @pre @p name spans @p cap bytes and is NUL-terminated at `name[*len]`.
 * @pre @p len is non-null and `*len < cap`.
 * @post Success advances @p len by the exact digit count.
 * @post Failure leaves @p len and name bytes unchanged.
 * @note Pure apart from caller-owned name state.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_name_u64(char* name, size_t cap, size_t* len, uint64_t value)
{
  char   reverse[20U];
  size_t count = 0U;
  do {
    reverse[count++] = (char)('0' + (char)(value % k_host_decimal_radix));
    value /= k_host_decimal_radix;
  } while (value != 0U);
  if ((*len + count + 1U) > cap) {
    return k_ra8_err_invalid_size;
  }
  while (count != 0U) {
    name[(*len)++] = reverse[--count];
  }
  name[*len] = '\0';
  return k_ra8_ok;
}

/**
 * @brief Build a bounded sibling staging name.
 * @details Combines final leaf, fixed marker, process identifier, and attempt index.
 * @param[in,out] state Transaction state holding final and staging buffers.
 * @param[in] attempt Exclusive-create collision retry index.
 * @return Canonical name-construction status.
 * @retval k_ra8_ok Complete hidden sibling leaf was built.
 * @retval k_ra8_err_invalid_size Fixed name capacity is insufficient.
 * @pre @p state is non-null and `final_name` is bounded and NUL-terminated.
 * @pre @p attempt is below the fixed creation-attempt ceiling.
 * @post Success NUL-terminates `state->stage_name` with no slash.
 * @post No filesystem object is created.
 * @note Not thread-safe through @p state.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_stage_name(ra8_fmt_host_transaction_t* state, uint32_t attempt)
{
  static const char suffix[] = ".ra8tmp.";
  size_t            len      = 0U;
  state->stage_name[len++]   = '.';
  const size_t leaf_len      = strlen(state->final_name);
  if ((1U + leaf_len + sizeof(suffix)) > sizeof(state->stage_name)) {
    return k_ra8_err_invalid_size;
  }
  (void)memcpy(&state->stage_name[len], state->final_name, leaf_len);
  len += leaf_len;
  (void)memcpy(&state->stage_name[len], suffix, sizeof(suffix) - 1U);
  len += sizeof(suffix) - 1U;
  ra8_err_t rc =
    internal_name_u64(state->stage_name, sizeof(state->stage_name), &len, (uint64_t)getpid());
  if ((rc == k_ra8_ok) && ((len + 2U) <= sizeof(state->stage_name))) {
    state->stage_name[len++] = '.';
    state->stage_name[len]   = '\0';
    rc = internal_name_u64(state->stage_name, sizeof(state->stage_name), &len, attempt);
  }
  return rc;
}

/**
 * @brief Reject existing non-regular or symlink destinations.
 * @details Performs a no-follow directory-relative stat and accepts absence or a regular file.
 * @param[in] parent_fd Open destination-parent descriptor.
 * @param[in] leaf Bounded final filename.
 * @return Canonical destination-policy status.
 * @retval k_ra8_ok Destination is absent or an existing regular file.
 * @retval k_ra8_err_access_denied Destination is symlink or non-regular.
 * @retval k_ra8_fail Host metadata lookup failed unexpectedly.
 * @pre @p parent_fd is an open directory descriptor.
 * @pre @p leaf is non-null, NUL-terminated, and contains no slash.
 * @post No filesystem object is changed.
 * @post Success is only a point-in-time policy observation.
 * @note Caller keeps the directory handle through publication.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_validate_destination(int parent_fd, const char* leaf)
{
  struct stat status = {};
  if (fstatat(parent_fd, leaf, &status, AT_SYMLINK_NOFOLLOW) == 0) {
    return S_ISREG(status.st_mode) ? k_ra8_ok : k_ra8_err_access_denied;
  }
  return (errno == ENOENT) ? k_ra8_ok : k_ra8_fail;
}

/**
 * @brief Append transaction bytes exactly once.
 * @details Writes the entire span through the raw descriptor sink and advances
 * transaction position only after complete success.
 * @param[in,out] ctx Active ::ra8_fmt_host_transaction_t.
 * @param[in] bytes Source span, nullable only when @p len is zero.
 * @param[in] len Exact append byte count.
 * @return Canonical transaction or sink status.
 * @retval k_ra8_ok Complete span was appended and accounted.
 * @retval k_ra8_err_invalid_state Transaction/source state was invalid.
 * @retval other Raw descriptor write failed.
 * @pre @p ctx points to an active transaction and stage descriptor is open.
 * @pre @p bytes spans @p len readable bytes when non-empty.
 * @post Success advances `position` by exactly @p len.
 * @post Failure does not advance logical transaction position.
 * @note Not thread-safe through one transaction.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_transaction_append(void* ctx, const uint8_t* bytes, size_t len)
{
  ra8_fmt_host_transaction_t* state = (ra8_fmt_host_transaction_t*)ctx;
  if ((state == nullptr) || !state->active || ((bytes == nullptr) && (len != 0U))) {
    return k_ra8_err_invalid_state;
  }
  ra8_fmt_host_fd_sink_t sink = {.fd = state->stage_fd};
  const ra8_err_t        rc   = internal_fd_write(&sink, bytes, len);
  if (rc == k_ra8_ok) {
    state->position += len;
  }
  return rc;
}

/**
 * @brief Discard transaction-owned staging resources.
 * @details Idempotently closes descriptors and removes only the owned private leaf.
 * @param[in,out] ctx Possibly partial ::ra8_fmt_host_transaction_t.
 * @pre @p ctx is null or points to caller-owned transaction state.
 * @pre No append or commit uses the same transaction concurrently.
 * @post Every owned descriptor is closed and active state is false.
 * @post Any owned stage is unlinked; the final destination is unchanged.
 * @note Best-effort cleanup intentionally has no return channel.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_transaction_abort(void* ctx)
{
  ra8_fmt_host_transaction_t* state = (ra8_fmt_host_transaction_t*)ctx;
  if (state == nullptr) {
    return;
  }
  if (state->stage_fd >= 0) {
    (void)close(state->stage_fd);
    state->stage_fd = -1;
  }
  if (state->stage_exists && (state->parent_fd >= 0)) {
    (void)unlinkat(state->parent_fd, state->stage_name, 0);
  }
  if (state->parent_fd >= 0) {
    (void)close(state->parent_fd);
    state->parent_fd = -1;
  }
  state->stage_exists = false;
  state->active       = false;
}

/**
 * @brief Sync, install, and directory-sync one staged artifact.
 * @details Syncs and closes staged bytes, performs one same-directory rename,
 * then syncs the parent metadata and releases transaction ownership.
 * @param[in,out] ctx Active ::ra8_fmt_host_transaction_t.
 * @return Canonical durable-publication status.
 * @retval k_ra8_ok File sync, close, rename, and parent sync succeeded.
 * @retval k_ra8_err_invalid_state Transaction is not active with an open stage.
 * @retval k_ra8_fail A host durability or publication operation failed.
 * @pre @p ctx points to a complete active transaction.
 * @pre No further append will use its stage descriptor.
 * @post Every owned descriptor is closed and active state is false.
 * @post Success leaves final path naming the complete staged bytes.
 * @note Parent-sync failure may follow a visible successful rename.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_transaction_commit(void* ctx)
{
  ra8_fmt_host_transaction_t* state = (ra8_fmt_host_transaction_t*)ctx;
  if ((state == nullptr) || !state->active || (state->stage_fd < 0)) {
    return k_ra8_err_invalid_state;
  }
  const int stage_sync_rc  = fsync(state->stage_fd);
  const int stage_close_rc = close(state->stage_fd);
  state->stage_fd          = -1;
  if ((stage_sync_rc != 0) || (stage_close_rc != 0)) {
    internal_transaction_abort(state);
    return k_ra8_fail;
  }
  if (renameat(state->parent_fd, state->stage_name, state->parent_fd, state->final_name) != 0) {
    internal_transaction_abort(state);
    return k_ra8_fail;
  }
  state->stage_exists = false;
  const int sync_rc   = fsync(state->parent_fd);
  (void)close(state->parent_fd);
  state->parent_fd = -1;
  state->active    = false;
  return (sync_rc == 0) ? k_ra8_ok : k_ra8_fail;
}

/**
 * @brief Capture portable-width identity and mutation fields from host metadata.
 * @details Normalizes platform stat timestamps into the immutable source snapshot.
 * @param[in] status Successful regular-file metadata.
 * @param[out] out Receives every snapshot field.
 * @pre @p status and @p out are non-null.
 * @pre @p status comes from fstat on the descriptor being bound.
 * @post @p out describes the same metadata observation.
 * @post No host descriptor or metadata object is modified.
 * @note Pure apart from the caller output.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_snapshot(const struct stat* status, ra8_fmt_host_snapshot_t* out)
{
#if defined(__APPLE__)
  const struct timespec modified = status->st_mtimespec;
  const struct timespec changed  = status->st_ctimespec;
#else
  const struct timespec modified = status->st_mtim;
  const struct timespec changed  = status->st_ctim;
#endif
  *out = (ra8_fmt_host_snapshot_t){
    .device     = (uint64_t)status->st_dev,
    .inode      = (uint64_t)status->st_ino,
    .size       = (uint64_t)status->st_size,
    .mtime_sec  = (int64_t)modified.tv_sec,
    .mtime_nsec = (int64_t)modified.tv_nsec,
    .ctime_sec  = (int64_t)changed.tv_sec,
    .ctime_nsec = (int64_t)changed.tv_nsec,
  };
}

/**
 * @brief Compare complete captured regular-file evidence.
 * @details Compares identity, size, and nanosecond modification/change times.
 * @param[in] first First snapshot.
 * @param[in] second Second snapshot.
 * @return Whether every captured field matches.
 * @retval true The snapshots describe the same unchanged object.
 * @retval false Identity, extent, or one timestamp differs.
 * @pre Both pointers are non-null.
 * @pre Both snapshots were completely initialized.
 * @post Neither snapshot changes.
 * @post No external state is observed or modified.
 * @note Pure and thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static bool internal_snapshot_same(const ra8_fmt_host_snapshot_t* first,
                                   const ra8_fmt_host_snapshot_t* second)
{
  return (first->device == second->device) && (first->inode == second->inode) &&
         (first->size == second->size) && (first->mtime_sec == second->mtime_sec) &&
         (first->mtime_nsec == second->mtime_nsec) && (first->ctime_sec == second->ctime_sec) &&
         (first->ctime_nsec == second->ctime_nsec);
}

/**
 * @brief Portable source-validation callback over host snapshot evidence.
 * @details Re-stats the open descriptor and compares it with its captured view.
 * @param[in] ctx Bound ::ra8_fmt_host_source_t.
 * @param[in] expected_size Size captured in the portable view.
 * @return Stability status.
 * @retval k_ra8_ok The descriptor remains unchanged.
 * @retval k_ra8_err_validation_failed Size or metadata changed.
 * @retval other Host metadata validation status.
 * @pre @p ctx points at an open source.
 * @pre @p expected_size is the bound portable-view extent.
 * @post Descriptor position and snapshot remain unchanged.
 * @post Success proves all captured identity and mutation fields still match.
 * @note Thread-safe against independent positioned reads.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_source_validate(void* ctx, uint64_t expected_size)
{
  const ra8_fmt_host_source_t* source = (const ra8_fmt_host_source_t*)ctx;
  if ((source == nullptr) || (source->source.size != expected_size)) {
    return k_ra8_err_validation_failed;
  }
  return priv_fmt_host_source_unchanged(source);
}

static const ra8_fmt_transaction_ops_t s_transaction_ops = {
  .append = internal_transaction_append,
  .commit = internal_transaction_commit,
  .abort  = internal_transaction_abort,
};

RA8_PRIV ra8_err_t priv_fmt_host_source_open(const char*            path,
                                             uint64_t               max_size,
                                             ra8_fmt_host_source_t* out)
{
  if ((path == nullptr) || (out == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  *out         = (ra8_fmt_host_source_t){.fd = -1};
  const int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) {
    return (errno == ELOOP) ? k_ra8_err_access_denied : k_ra8_fail;
  }
  struct stat status = {};
  if ((fstat(fd, &status) != 0) || !S_ISREG(status.st_mode) || (status.st_size <= 0) ||
      ((uint64_t)status.st_size > max_size)) {
    (void)close(fd);
    return k_ra8_err_invalid_size;
  }
  out->fd              = fd;
  out->source.read_at  = internal_pread;
  out->source.validate = internal_source_validate;
  out->source.ctx      = out;
  out->source.size     = (uint64_t)status.st_size;
  internal_snapshot(&status, &out->snapshot);
  return k_ra8_ok;
}

RA8_PRIV bool priv_fmt_host_sources_same(const ra8_fmt_host_source_t* first,
                                         const ra8_fmt_host_source_t* second)
{
  if ((first == nullptr) || (second == nullptr) || (first == second) || (first->fd < 0) ||
      (second->fd < 0) || (first->fd == second->fd)) {
    return false;
  }
  return internal_snapshot_same(&first->snapshot, &second->snapshot);
}

RA8_PRIV ra8_err_t priv_fmt_host_source_unchanged(const ra8_fmt_host_source_t* source)
{
  if ((source == nullptr) || (source->fd < 0)) {
    return k_ra8_err_invalid_state;
  }
  struct stat status = {};
  if (fstat(source->fd, &status) != 0) {
    return k_ra8_fail;
  }
  if (!S_ISREG(status.st_mode)) {
    return k_ra8_err_validation_failed;
  }
  ra8_fmt_host_snapshot_t current;
  internal_snapshot(&status, &current);
  return internal_snapshot_same(&source->snapshot, &current) ? k_ra8_ok
                                                             : k_ra8_err_validation_failed;
}

RA8_PRIV void priv_fmt_host_source_close(ra8_fmt_host_source_t* source)
{
  if ((source != nullptr) && (source->fd >= 0)) {
    (void)close(source->fd);
    source->fd = -1;
  }
}

RA8_PRIV ra8_fmt_sink_t priv_fmt_host_fd_sink(ra8_fmt_host_fd_sink_t* state)
{
  return (ra8_fmt_sink_t){.write = internal_fd_write, .ctx = state};
}

RA8_PRIV void priv_fmt_host_log_byte(void* ctx, uint8_t byte)
{
  (void)internal_fd_write(ctx, &byte, 1U);
}

RA8_PRIV ra8_err_t priv_fmt_host_transaction_begin(const char*                 path,
                                                   ra8_fmt_host_transaction_t* state,
                                                   ra8_fmt_transaction_t*      out)
{
  if ((path == nullptr) || (state == nullptr) || (out == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  *state = (ra8_fmt_host_transaction_t){.parent_fd = -1, .stage_fd = -1};
  *out   = (ra8_fmt_transaction_t){};
  char      parent[k_ra8_fmt_host_path_cap];
  ra8_err_t rc = internal_split_path(path, parent, state->final_name);
  if (rc != k_ra8_ok) {
    return rc;
  }
  state->parent_fd = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (state->parent_fd < 0) {
    return k_ra8_fail;
  }
  rc = internal_validate_destination(state->parent_fd, state->final_name);
  for (uint32_t attempt = 0U; (attempt < k_host_create_attempts) && (rc == k_ra8_ok); ++attempt) {
    rc = internal_stage_name(state, attempt);
    if (rc == k_ra8_ok) {
      state->stage_fd = openat(state->parent_fd,
                               state->stage_name,
                               O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                               (mode_t)k_host_mode_private);
      if (state->stage_fd >= 0) {
        state->stage_exists = true;
        state->active       = true;
        out->ops            = &s_transaction_ops;
        out->ctx            = state;
        return k_ra8_ok;
      }
      rc = (errno == EEXIST) ? k_ra8_ok : k_ra8_fail;
    }
  }
  internal_transaction_abort(state);
  return (rc == k_ra8_ok) ? k_ra8_err_exists : rc;
}
