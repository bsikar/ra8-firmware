/**
 * @file ra8_fmt_host_spool.c
 * @brief Owned anonymous raw-fd spools for bounded verifier composition.
 * @details Creates exclusive sibling scratch files, unlinks them immediately,
 * and exposes append, seal, and positioned-read callbacks over owned raw fds.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#define _POSIX_C_SOURCE (200809L)

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "ra8_attributes.h"
#include "ra8_fmt_host_fd_internal.h"
#include "ra8_fmt_host_spool_internal.h"

/** @brief Scratch creation and spelling bounds. */
typedef enum : uint32_t {
  k_spool_attempts = 16U,   /**< Exclusive-create collision ceiling. */
  k_spool_radix    = 10U,   /**< Decimal filename radix.             */
  k_spool_mode     = 0600U, /**< Owner-only scratch permissions.     */
  k_spool_digits   = 20U,   /**< Digits in one uint64_t spelling.    */
} spool_const_t;

/**
 * @brief Copy the anchor parent into fixed storage.
 * @details Resolves an explicit parent, root slash, or current-directory dot.
 * @param[in] path NUL-terminated anchor spelling.
 * @param[out] parent Receives a NUL-terminated parent path.
 * @return Bounded path status.
 * @retval k_ra8_ok The complete parent fits.
 * @retval k_ra8_err_invalid_size The spelling is empty or too long.
 * @pre @p parent spans ::k_ra8_fmt_host_path_cap bytes.
 * @pre @p path is null or points to a NUL-terminated spelling.
 * @post Success writes either the explicit parent, slash, or dot.
 * @post Failure does not create or open any filesystem object.
 * @note Pure apart from caller output.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_parent(const char* path, char parent[k_ra8_fmt_host_path_cap])
{
  const size_t len = (path == nullptr) ? 0U : strlen(path);
  if ((len == 0U) || (len >= (size_t)k_ra8_fmt_host_path_cap)) {
    return k_ra8_err_invalid_size;
  }
  const char* slash = strrchr(path, '/');
  size_t      take  = 1U;
  const char* text  = ".";
  if (slash != nullptr) {
    text = path;
    take = (slash == path) ? 1U : (size_t)(slash - path);
  }
  (void)memcpy(parent, text, take);
  parent[take] = '\0';
  return k_ra8_ok;
}

/**
 * @brief Append one unsigned decimal into a bounded scratch name.
 * @details Reverses base-ten digits locally, then appends them in display order.
 * @param[in,out] name NUL-terminated partial name.
 * @param[in,out] len Current and resulting payload length.
 * @param[in] value Value to append.
 * @return Capacity status.
 * @retval k_ra8_ok The complete decimal was appended.
 * @retval k_ra8_err_invalid_size The fixed spelling buffer is insufficient.
 * @pre @p name spans ::k_ra8_fmt_host_name_cap bytes.
 * @pre @p len names the current in-bounds NUL offset.
 * @post Success appends the complete decimal and NUL.
 * @post Failure leaves filesystem state untouched.
 * @note Pure apart from caller state.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_decimal(char name[k_ra8_fmt_host_name_cap], size_t* len, uint64_t value)
{
  char   reverse[k_spool_digits];
  size_t count = 0U;
  do {
    reverse[count++] = (char)('0' + (char)(value % k_spool_radix));
    value /= k_spool_radix;
  } while (value != 0U);
  if ((*len + count + 1U) > (size_t)k_ra8_fmt_host_name_cap) {
    return k_ra8_err_invalid_size;
  }
  while (count != 0U) {
    name[(*len)++] = reverse[--count];
  }
  name[*len] = '\0';
  return k_ra8_ok;
}

/**
 * @brief Form one collision-bounded private scratch leaf.
 * @details Combines the fixed prefix, process identifier, and attempt index.
 * @param[in] attempt Attempt index.
 * @param[out] name Receives the complete leaf.
 * @return Capacity status.
 * @retval k_ra8_ok The complete private leaf fits.
 * @retval k_ra8_err_invalid_size Decimal spelling exceeded fixed storage.
 * @pre @p name spans ::k_ra8_fmt_host_name_cap bytes.
 * @pre @p attempt is below ::k_spool_attempts.
 * @post Success emits a private process-and-attempt leaf.
 * @post No filesystem state is inspected or modified.
 * @note Thread safety inherits process-identifier stability.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_name(uint32_t attempt, char name[k_ra8_fmt_host_name_cap])
{
  static const char prefix[] = ".ra8spool.";
  size_t            len      = sizeof(prefix) - 1U;
  (void)memcpy(name, prefix, len);
  name[len]    = '\0';
  ra8_err_t rc = internal_decimal(name, &len, (uint64_t)getpid());
  if ((rc == k_ra8_ok) && ((len + 2U) <= (size_t)k_ra8_fmt_host_name_cap)) {
    name[len++] = '.';
    name[len]   = '\0';
    rc          = internal_decimal(name, &len, attempt);
  }
  return rc;
}

/**
 * @brief Append exactly to one unsealed anonymous descriptor.
 * @details Retries interrupted positioned writes without changing descriptor position.
 * @param[in,out] ctx Bound ::ra8_fmt_host_spool_t.
 * @param[in] bytes Source bytes.
 * @param[in] len Exact byte count.
 * @return Raw-fd or state status.
 * @retval k_ra8_ok Every byte was appended.
 * @retval k_ra8_err_invalid_state The binding, span, or state is invalid.
 * @retval k_ra8_fail A host write failed.
 * @pre Non-empty @p bytes spans @p len readable bytes.
 * @pre The bound spool is open and unsealed.
 * @post Success advances logical position by exactly @p len.
 * @post Failure never marks the spool sealed.
 * @note Not thread-safe through one spool state.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_append(void* ctx, const uint8_t* bytes, size_t len)
{
  ra8_fmt_host_spool_t* state = (ra8_fmt_host_spool_t*)ctx;
  if ((state == nullptr) || (state->fd < 0) || state->sealed ||
      ((bytes == nullptr) && (len != 0U)) || ((uint64_t)len > (UINT64_MAX - state->position))) {
    return k_ra8_err_invalid_state;
  }
  size_t done = 0U;
  while (done < len) {
    const ssize_t rc = pwrite(state->fd, &bytes[done], len - done, (off_t)(state->position + done));
    if (rc > 0) {
      done += (size_t)rc;
    } else if ((rc < 0) && (errno == EINTR)) {
      continue;
    } else {
      return k_ra8_fail;
    }
  }
  state->position += len;
  return k_ra8_ok;
}

/**
 * @brief Seal an exact spool extent before positioned reads.
 * @details Cross-checks logical and host extents, then synchronizes deferred writes.
 * @param[in,out] ctx Bound ::ra8_fmt_host_spool_t.
 * @param[in] expected_size Producer-reported complete byte count.
 * @return Exact-size, sync, or state status.
 * @retval k_ra8_ok The complete exact extent is durable and readable.
 * @retval k_ra8_err_validation_failed Logical or host size differs.
 * @retval other Invalid-state or host-sync status.
 * @pre The spool is open and unsealed.
 * @pre No append callback is running concurrently.
 * @post Success permits reads over exactly @p expected_size bytes.
 * @post Failure never exposes the spool as sealed.
 * @note Scratch syncing catches deferred host write errors before trust.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_seal(void* ctx, uint64_t expected_size)
{
  ra8_fmt_host_spool_t* state = (ra8_fmt_host_spool_t*)ctx;
  if ((state == nullptr) || (state->fd < 0) || state->sealed) {
    return k_ra8_err_invalid_state;
  }
  struct stat status = {};
  if ((state->position != expected_size) || (fstat(state->fd, &status) != 0) ||
      ((uint64_t)status.st_size != expected_size)) {
    return k_ra8_err_validation_failed;
  }
  if (fsync(state->fd) != 0) {
    return k_ra8_fail;
  }
  state->sealed = true;
  return k_ra8_ok;
}

/**
 * @brief Positioned-read one sealed anonymous spool.
 * @details Retries interrupted host reads and preserves descriptor position.
 * @param[in,out] ctx Bound ::ra8_fmt_host_spool_t.
 * @param[in] offset Absolute byte offset.
 * @param[out] bytes Destination bytes.
 * @param[in] len Requested bytes.
 * @param[out] got Receives actual bytes, including short EOF.
 * @return Raw-fd or state status.
 * @retval k_ra8_ok The request completed, possibly at EOF.
 * @retval k_ra8_err_invalid_state The binding, span, or state is invalid.
 * @retval k_ra8_fail A host read failed.
 * @pre Non-empty @p bytes spans @p len writable bytes.
 * @pre The bound spool was successfully sealed.
 * @post Success initializes @p got and never changes descriptor position.
 * @post No spool byte or logical extent is modified.
 * @note Thread-safe for independent destinations after sealing.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_read(void* ctx, uint64_t offset, uint8_t* bytes, size_t len, size_t* got)
{
  ra8_fmt_host_spool_t* state = (ra8_fmt_host_spool_t*)ctx;
  if ((state == nullptr) || (got == nullptr) || (state->fd < 0) || !state->sealed ||
      ((bytes == nullptr) && (len != 0U))) {
    return k_ra8_err_invalid_state;
  }
  *got = 0U;
  if ((offset >= state->position) || (len == 0U)) {
    return k_ra8_ok;
  }
  const uint64_t remain = state->position - offset;
  if ((uint64_t)len > remain) {
    len = (size_t)remain;
  }
  while (*got < len) {
    const ssize_t rc = pread(state->fd, &bytes[*got], len - *got, (off_t)(offset + *got));
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
 * @brief Create and immediately unlink one exclusive scratch leaf.
 * @details Tries a bounded private-name sequence and retains only the owned descriptor.
 * @param[in] parent_fd Open parent directory.
 * @param[out] state Receives the owned descriptor.
 * @return Creation or collision status.
 * @retval k_ra8_ok An exclusive file was opened and unlinked.
 * @retval k_ra8_fail Creation or immediate unlink failed.
 * @retval other Bounded-name status.
 * @pre @p state is initialized closed.
 * @pre @p parent_fd names an open directory owned by the caller.
 * @post Success leaves no directory entry and one open descriptor.
 * @post Failure leaves @p state closed and removes any created entry.
 * @note Collision work is bounded by ::k_spool_attempts.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_create(int parent_fd, ra8_fmt_host_spool_t* state)
{
  char name[k_ra8_fmt_host_name_cap];
  for (uint32_t attempt = 0U; attempt < k_spool_attempts; ++attempt) {
    ra8_err_t rc = internal_name(attempt, name);
    if (rc != k_ra8_ok) {
      return rc;
    }
    state->fd = openat(parent_fd,
                       name,
                       O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                       (mode_t)k_spool_mode);
    if (state->fd >= 0) {
      if (unlinkat(parent_fd, name, 0) == 0) {
        return k_ra8_ok;
      }
      (void)close(state->fd);
      state->fd = -1;
      (void)unlinkat(parent_fd, name, 0);
      return k_ra8_fail;
    }
    if (errno != EEXIST) {
      return k_ra8_fail;
    }
  }
  return k_ra8_err_exists;
}

RA8_PRIV ra8_err_t priv_fmt_host_spool_open(const char*           anchor_path,
                                            ra8_fmt_host_spool_t* state,
                                            ra8_fmt_spool_t*      out)
{
  if ((anchor_path == nullptr) || (state == nullptr) || (out == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  *state = (ra8_fmt_host_spool_t){.fd = -1};
  *out   = (ra8_fmt_spool_t){};
  char      parent[k_ra8_fmt_host_path_cap];
  ra8_err_t rc = internal_parent(anchor_path, parent);
  if (rc != k_ra8_ok) {
    return rc;
  }
  const int parent_fd = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (parent_fd < 0) {
    return k_ra8_fail;
  }
  rc = internal_create(parent_fd, state);
  (void)close(parent_fd);
  if (rc == k_ra8_ok) {
    *out = (ra8_fmt_spool_t){
      .read_at = internal_read,
      .append  = internal_append,
      .seal    = internal_seal,
      .ctx     = state,
    };
  }
  return rc;
}

RA8_PRIV void priv_fmt_host_spool_close(ra8_fmt_host_spool_t* state)
{
  if ((state != nullptr) && (state->fd >= 0)) {
    (void)close(state->fd);
    state->fd = -1;
  }
}
