/**
 * @file mdl_hash.c
 * @brief FNV-1a 64 content-identity hashing (host stdio for the file path).
 *
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT

 */
#include "mdl_hash.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ra8_attributes.h"

/** @brief Streaming-read chunk size for ::mdl_hash_file (stack, no alloc). */
typedef enum : uint16_t {
  k_hash_chunk_bytes = 8192, /**< Bytes read per fread() into the fold buffer. */
} mdl_hash_chunk_t;

/** @brief Loop bound on ::mdl_hash_file's read loop (files fit far under this). */
typedef enum : uint32_t {
  k_hash_max_chunks = 1000000U, /**< 8 GiB ceiling: any real page is tiny. */
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

/** @brief Fold one file into a running FNV state; false on a read error. */
RA8_INTERNAL static bool hash_stream(FILE* fp, uint64_t* h)
{
  uint8_t buf[k_hash_chunk_bytes];
  for (uint32_t chunk = 0U; chunk < (uint32_t)k_hash_max_chunks; ++chunk) {
    const size_t n = fread(buf, 1U, sizeof(buf), fp);
    if (n > 0U) {
      *h = mdl_hash_bytes_seed(buf, n, *h);
    }
    if (n < sizeof(buf)) {
      return ferror(fp) == 0; /* short read: EOF (ok) vs a real error */
    }
  }
  return true; /* hit the (absurd) chunk ceiling: treat as complete */
}

ra8_err_t mdl_hash_file(const char* path, uint64_t* out)
{
  if ((path == nullptr) || (out == nullptr)) {
    return k_ra8_err_invalid_arg;
  }
  FILE* fp = fopen(path, "rb");
  if (fp == nullptr) {
    if (fp) {
      (void)fclose(fp);
    }
    return k_ra8_fail;
  }
  uint64_t   h  = (uint64_t)k_mdl_fnv_offset;
  const bool ok = hash_stream(fp, &h);
  (void)fclose(fp);
  if (!ok) {
    return k_ra8_fail;
  }
  *out = h;
  return k_ra8_ok;
}
