/**
 * @file mdl_hash.c
 * @brief FNV-1a 64 content hashing over an injected portable filesystem.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#include "mdl_hash.h"

#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"

/** @brief Exact regular-file bound accepted by ::mdl_hash_file. */
typedef enum : uint64_t {
  k_hash_max_read_calls = 2000001ULL, /**< Short-read + EOF call ceiling. */
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
 * @param[in,out] file Open generic file positioned at byte zero.
 * @param[in] file_size Immutable size snapshot obtained from ::fw_fs_stat.
 * @param[out] buffer Caller-owned read scratch.
 * @param[in] buffer_bytes Nonzero extent of @p buffer.
 * @param[out] out Digest written only after exact EOF validation.
 * @return Canonical stream or content-stability status.
 * @pre @p file is a valid open regular file and @p out is non-NULL.
 * @pre @p file_size is at most ::k_mdl_hash_max_file_bytes.
 * @post Success advances @p file through EOF and writes @p out.
 * @post Failure leaves @p out untouched.
 * @note Not thread-safe against a concurrent writer of the same file.
 * @since 0.1.0
 */
ra8_err_t mdl_hash_stream(fw_fs_file_t* file,
                          uint64_t      file_size,
                          uint8_t*      buffer,
                          uint32_t      buffer_bytes,
                          uint64_t*     out)
{
  if ((file == nullptr) || (buffer == nullptr) || (buffer_bytes == 0U) || (out == nullptr)) {
    return k_ra8_err_invalid_arg;
  }
  if (file_size > (uint64_t)k_mdl_hash_max_file_bytes) {
    return k_ra8_err_invalid_size;
  }
  uint64_t remaining = file_size;
  uint64_t hash      = (uint64_t)k_mdl_fnv_offset;
  for (uint64_t call = 0U; call < (uint64_t)k_hash_max_read_calls; ++call) {
    const uint32_t want =
      (remaining == 0U)
        ? 1U
        : ((remaining < (uint64_t)buffer_bytes) ? (uint32_t)remaining : buffer_bytes);
    uint32_t        got  = 0U;
    const ra8_err_t read = fw_fs_read(file, buffer, want, &got);
    if (read != k_ra8_ok) {
      return read;
    }
    if (remaining == 0U) {
      if (got != 0U) {
        return k_ra8_fail;
      }
      *out = hash;
      return k_ra8_ok;
    }
    if ((got == 0U) || ((uint64_t)got > remaining)) {
      return k_ra8_fail;
    }
    hash = mdl_hash_bytes_seed(buffer, (size_t)got, hash);
    remaining -= (uint64_t)got;
  }
  return k_ra8_err_invalid_size;
}

ra8_err_t mdl_hash_file(mdl_storage_t* storage, const char* path, uint64_t* out)
{
  if ((storage == nullptr) || (storage->fs == nullptr) || (path == nullptr) || (out == nullptr)) {
    return k_ra8_err_invalid_arg;
  }
  fw_fs_stat_t stat = {};
  ra8_err_t    err  = fw_fs_stat(&storage->fs->names, path, &stat);
  if (err != k_ra8_ok) {
    return err;
  }
  if (!stat.exists) {
    return k_ra8_err_not_found;
  }
  if (stat.type != k_fw_fs_node_file) {
    return k_ra8_err_invalid_arg;
  }
  if (stat.size_bytes > (uint64_t)k_mdl_hash_max_file_bytes) {
    return k_ra8_err_invalid_size;
  }
  fw_fs_file_t file = {};
  err               = fw_fs_open(&storage->fs->streams,
                                 path,
                                 k_fw_fs_open_read,
                                 &file,
                                 storage->file_workspace,
                                 storage->file_workspace_bytes);
  if (err != k_ra8_ok) {
    return err;
  }
  uint64_t digest = 0U;
  err =
    mdl_hash_stream(&file, stat.size_bytes, storage->io_buffer, storage->io_buffer_bytes, &digest);
  const ra8_err_t closed = fw_fs_close(&file);
  if (err == k_ra8_ok) {
    err = closed;
  }
  if (err == k_ra8_ok) {
    *out = digest;
  }
  return err;
}
