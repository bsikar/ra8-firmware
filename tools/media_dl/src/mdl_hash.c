/**
 * @file mdl_hash.c
 * @brief FNV-1a 64 content-identity hashing over bounded POSIX file reads.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#include "mdl_hash.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ra8_attributes.h"

/** @brief Streaming-read chunk size for ::mdl_hash_file (stack, no alloc). */
typedef enum : uint16_t {
  k_hash_chunk_bytes = 8192, /**< Bytes read per fread() into the fold buffer. */
} mdl_hash_chunk_t;

/** @brief Exact regular-file bound accepted by ::mdl_hash_file. */
typedef enum : uint64_t {
  k_hash_max_file_bytes = 8192000000ULL, /**< 1,000,000 fixed 8 KiB chunks.   */
  k_hash_max_read_calls = 2000001ULL,    /**< Short-read/EINTR + EOF ceiling. */
} mdl_hash_bound_t;

uint64_t mdl_hash_bytes_seed(const void* data, size_t len, uint64_t seed)
{
  if ((data == nullptr) || (len == 0U)) {
    return seed;
  }
  const uint8_t* p = (const uint8_t*)data;
  uint64_t       h = seed;
  for (size_t i = 0U; i < len; ++i) {
    h ^= (uint64_t)p[i];
    h *= (uint64_t)k_mdl_fnv_prime;
  }
  return h;
}

uint64_t mdl_hash_bytes(const void* data, size_t len)
{
  return mdl_hash_bytes_seed(data, len, (uint64_t)k_mdl_fnv_offset);
}

uint64_t mdl_hash_str(const char* s)
{
  if (s == nullptr) {
    return (uint64_t)k_mdl_fnv_offset;
  }
  return mdl_hash_bytes(s, strlen(s));
}

/**
 * @brief Fold an exact regular-file extent into a running FNV state.
 * @param[in] fd Open regular-file descriptor positioned at byte zero.
 * @param[in] file_size Immutable size snapshot obtained from fstat.
 * @param[in,out] h Running FNV digest.
 * @return Whether exactly @p file_size bytes were read and EOF followed.
 * @retval true The complete snapshotted file was hashed without I/O error.
 * @retval false The file shrank, grew, or produced an I/O error while hashing.
 * @pre @p fd is a valid open regular-file descriptor and @p h is non-NULL.
 * @pre @p file_size is at most ::k_hash_max_file_bytes.
 * @post Success advances @p fd through EOF and updates @p h for every byte.
 * @post Failure leaves @p h partial and unsuitable for publication.
 * @note Not thread-safe against a concurrent writer of the same file.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_hash_stream(int fd, uint64_t file_size, uint64_t* h)
{
  uint8_t  buf[k_hash_chunk_bytes];
  uint64_t remaining = file_size;
  for (uint64_t call = 0U; call < (uint64_t)k_hash_max_read_calls; ++call) {
    const size_t want = (remaining == 0U)
                          ? 1U
                          : ((remaining < (uint64_t)sizeof(buf)) ? (size_t)remaining : sizeof(buf));
    const ssize_t got = read(fd, buf, want);
    if (got < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    if (remaining == 0U) {
      return got == 0;
    }
    if ((got == 0) || ((uint64_t)got > remaining)) {
      return false;
    }
    *h = mdl_hash_bytes_seed(buf, (size_t)got, *h);
    remaining -= (uint64_t)got;
  }
  return false;
}

/** @brief Map filesystem acquisition failures onto the shared error contract. */
RA8_INTERNAL static ra8_err_t internal_hash_open_error(int error_number)
{
  if ((error_number == ENOENT) || (error_number == ENOTDIR)) {
    return k_ra8_err_not_found;
  }
  if ((error_number == EACCES) || (error_number == EPERM)) {
    return k_ra8_err_access_denied;
  }
  if (error_number == ELOOP) {
    return k_ra8_err_invalid_arg;
  }
  return k_ra8_fail;
}

ra8_err_t mdl_hash_file(const char* path, uint64_t* out)
{
  if ((path == nullptr) || (out == nullptr)) {
    return k_ra8_err_invalid_arg;
  }
  int flags = O_RDONLY | O_NONBLOCK | O_NOFOLLOW;
#if defined(O_CLOEXEC)
  flags |= O_CLOEXEC;
#endif
  const int fd = open(path, flags);
  if (fd < 0) {
    return internal_hash_open_error(errno);
  }
  struct stat st = {};
  if (fstat(fd, &st) != 0) {
    (void)close(fd);
    return k_ra8_fail;
  }
  if (!S_ISREG(st.st_mode)) {
    (void)close(fd);
    return k_ra8_err_invalid_arg;
  }
  if ((st.st_size < 0) || ((uint64_t)st.st_size > (uint64_t)k_hash_max_file_bytes)) {
    (void)close(fd);
    return k_ra8_err_invalid_size;
  }
  uint64_t h  = (uint64_t)k_mdl_fnv_offset;
  bool     ok = internal_hash_stream(fd, (uint64_t)st.st_size, &h);
  ok          = (close(fd) == 0) && ok;
  if (!ok) {
    return k_ra8_fail;
  }
  *out = h;
  return k_ra8_ok;
}
