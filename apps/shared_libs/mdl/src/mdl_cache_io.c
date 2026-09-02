/**
 * @file mdl_cache_io.c
 * @brief Checksummed binary index and immutable body I/O for mdl_cache.
 * @details Owns portable namespace, stream, and transaction operations while
 *          the policy state machine remains in mdl_cache.c.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#include <limits.h>
#include <stdint.h>
#include <string.h>

#include "mdl_cache_internal.h"
#include "mdl_hash.h"
#include "mdl_sanitize.h"
#include "mdl_url_guard.h"

/** @brief Canonical binary layout and bounded I/O constants. */
typedef enum : uint32_t {
  k_cache_header_bytes    = 32U,      /**< Fixed index header width.       */
  k_cache_record_bytes    = 36U,      /**< Fixed record header width.      */
  k_cache_trailer_bytes   = 8U,       /**< Payload-hash trailer width.     */
  k_cache_io_call_max     = 2000000U, /**< Short-I/O progress ceiling.     */
  k_cache_status_min      = 100U,     /**< Smallest retained HTTP code.    */
  k_cache_status_max      = 599U,     /**< Largest retained HTTP code.     */
  k_cache_hex_high_shift  = 60U,      /**< Shift of the first hash nibble. */
  k_cache_nibble_mask     = 0x0FU,    /**< Low hexadecimal nibble mask.    */
  k_cache_u64_high_shift  = 56U,      /**< Shift of the first uint64 byte. */
  k_cache_record_status   = 24U,      /**< Record HTTP-status offset.      */
  k_cache_record_url_len  = 26U,      /**< Record URL-length offset.       */
  k_cache_record_path_len = 28U,      /**< Record path-length offset.      */
  k_cache_record_etag_len = 30U,      /**< Record ETag-length offset.      */
  k_cache_record_time_len = 32U,      /**< Record modified-length offset.  */
  k_cache_record_reserved = 34U,      /**< Record reserved-field offset.   */
  k_cache_header_count    = 12U,      /**< Header record-count offset.     */
  k_cache_header_host     = 24U,      /**< Header host-hash offset.        */
} mdl_cache_binary_limit_t;

/** @brief Canonical cache index magic. */
static const uint8_t s_cache_magic[k_cache_trailer_bytes] =
  {'M', 'D', 'L', 'C', 'A', 'C', 'H', '1'};

/**
 * @brief Encode exactly sixteen lowercase hexadecimal digits.
 * @details Emits one digit per nibble from most significant to least so cache
 *          namespace spelling is fixed regardless of host endianness.
 * @param[out] destination Writable sixteen-byte destination.
 * @param[in] value Unsigned value to encode.
 * @pre @p destination spans at least sixteen bytes.
 * @pre Integer input uses its declared width.
 * @post Every destination byte is an ASCII hexadecimal digit.
 * @post Leading zeroes are retained.
 * @note No libc formatting or locale is involved.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_cache_hex16(char* destination, uint64_t value)
{
  static const char k_hex[] = "0123456789abcdef";
  for (uint8_t i = 0U; i < 16U; ++i) {
    const uint8_t shift = (uint8_t)(k_cache_hex_high_shift - (4U * i));
    destination[i]      = k_hex[(value >> shift) & k_cache_nibble_mask];
  }
}

/**
 * @brief Format one host-directory leaf without stdio.
 * @details Concatenates the fixed prefix and the complete host hash only after
 *          proving the destination can hold the canonical spelling.
 * @param[out] destination Writable destination.
 * @param[in] capacity Destination extent.
 * @param[in] host_hash Host identity.
 * @return Whether the exact leaf fit.
 * @retval true The complete leaf and NUL were written.
 * @retval false The destination capacity was insufficient.
 * @pre @p destination is non-NULL.
 * @pre Capacity describes the complete writable span.
 * @post Success writes `host-` plus sixteen digits and NUL.
 * @post Failure leaves the destination unspecified.
 * @note The representation is stable across platforms.
 * @since 0.1.0
 */
RA8_INTERNAL static bool
internal_cache_host_leaf(char* destination, size_t capacity, uint64_t host_hash)
{
  static const char k_prefix[] = "host-";
  const size_t      required   = (sizeof(k_prefix) - 1U) + 16U + 1U;
  if (capacity < required) {
    return false;
  }
  memcpy(destination, k_prefix, sizeof(k_prefix) - 1U);
  internal_cache_hex16(&destination[sizeof(k_prefix) - 1U], host_hash);
  destination[required - 1U] = '\0';
  return true;
}

/**
 * @brief Format one immutable body leaf without stdio.
 * @details Combines full URL and content identities so a new entity never
 *          truncates or overwrites an older cache-body generation.
 * @param[out] destination Writable destination.
 * @param[in] capacity Destination extent.
 * @param[in] url_hash URL identity.
 * @param[in] content_hash Body identity.
 * @return Whether the exact leaf fit.
 * @retval true The canonical leaf and NUL were written.
 * @retval false The destination capacity was insufficient.
 * @pre @p destination is non-NULL.
 * @pre Capacity describes the complete writable span.
 * @post Success writes the canonical content-derived leaf and NUL.
 * @post Failure leaves the destination unspecified.
 * @note Both hashes retain all sixteen hexadecimal digits.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_cache_body_leaf(char*    destination,
                                                  size_t   capacity,
                                                  uint64_t url_hash,
                                                  uint64_t content_hash)
{
  static const char k_prefix[] = "body-";
  static const char k_suffix[] = ".cache";
  const size_t      required   = (sizeof(k_prefix) - 1U) + 16U + 1U + 16U + sizeof(k_suffix);
  if (capacity < required) {
    return false;
  }
  size_t cursor = 0U;
  memcpy(&destination[cursor], k_prefix, sizeof(k_prefix) - 1U);
  cursor += sizeof(k_prefix) - 1U;
  internal_cache_hex16(&destination[cursor], url_hash);
  cursor += 16U;
  destination[cursor++] = '-';
  internal_cache_hex16(&destination[cursor], content_hash);
  cursor += 16U;
  memcpy(&destination[cursor], k_suffix, sizeof(k_suffix));
  return true;
}

/**
 * @brief Encode one big-endian uint16.
 * @details Writes the most-significant byte first to define a host-independent
 *          persistent representation.
 * @param[out] out Two writable bytes.
 * @param[in] value Value to encode.
 * @pre @p out is non-NULL.
 * @pre Two bytes are writable.
 * @post Both bytes are initialized canonically.
 * @post Decoding recovers @p value.
 * @note Host endianness is irrelevant.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_cache_put_u16(uint8_t* out, uint16_t value)
{
  out[0] = (uint8_t)(value >> 8U);
  out[1] = (uint8_t)value;
}

/**
 * @brief Encode one big-endian uint64.
 * @details Walks every byte from the most-significant position down to define
 *          a stable binary representation without alignment assumptions.
 * @param[out] out Eight writable bytes.
 * @param[in] value Value to encode.
 * @pre @p out is non-NULL.
 * @pre Eight bytes are writable.
 * @post Every byte is initialized canonically.
 * @post Decoding recovers @p value.
 * @note Host endianness is irrelevant.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_cache_put_u64(uint8_t* out, uint64_t value)
{
  for (uint8_t i = 0U; i < 8U; ++i) {
    out[i] = (uint8_t)(value >> (k_cache_u64_high_shift - (8U * i)));
  }
}

/**
 * @brief Decode one big-endian uint16.
 * @details Reassembles both bytes explicitly so unaligned file data never
 *          depends on host representation.
 * @param[in] in Two readable bytes.
 * @return Decoded unsigned value.
 * @retval uint16_t The exact represented value.
 * @pre @p in is non-NULL.
 * @pre Two bytes are readable.
 * @post Input is unchanged.
 * @post Result is independent of alignment.
 * @note Host endianness is irrelevant.
 * @since 0.1.0
 */
RA8_INTERNAL static uint16_t internal_cache_get_u16(const uint8_t* in)
{
  return (uint16_t)(((uint16_t)in[0] << 8U) | (uint16_t)in[1]);
}

/**
 * @brief Decode one big-endian uint64.
 * @details Folds each byte into the accumulator in encoded order without an
 *          unaligned integer load.
 * @param[in] in Eight readable bytes.
 * @return Decoded unsigned value.
 * @retval uint64_t The exact represented value.
 * @pre @p in is non-NULL.
 * @pre Eight bytes are readable.
 * @post Input is unchanged.
 * @post Result is independent of alignment.
 * @note Host endianness is irrelevant.
 * @since 0.1.0
 */
RA8_INTERNAL static uint64_t internal_cache_get_u64(const uint8_t* in)
{
  uint64_t value = 0U;
  for (uint8_t i = 0U; i < 8U; ++i) {
    value = (value << 8U) | (uint64_t)in[i];
  }
  return value;
}

/**
 * @brief Read exactly one bounded span from an open file.
 * @details Retries positive short reads while bounding the total backend-call
 *          count and rejecting zero progress.
 * @param[in,out] file Open readable file.
 * @param[out] destination Writable destination bytes.
 * @param[in] length Exact requested extent.
 * @param[in,out] calls Shared operation-call counter.
 * @return Canonical read or progress status.
 * @retval k_ra8_ok Exactly the requested bytes were read.
 * @retval k_ra8_err_invalid_state The backend reported zero progress.
 * @retval k_ra8_err_invalid_size The call-count ceiling was exhausted.
 * @retval other The backend read failed.
 * @pre Every pointer is non-NULL.
 * @pre Destination spans @p length bytes.
 * @post Success initializes exactly @p length bytes.
 * @post Zero progress and call exhaustion fail closed.
 * @note Short successful reads are retried.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_cache_read_all(fw_fs_file_t* file, uint8_t* destination, uint32_t length, uint32_t* calls)
{
  uint32_t offset = 0U;
  while (offset < length) {
    if (*calls >= (uint32_t)k_cache_io_call_max) {
      return k_ra8_err_invalid_size;
    }
    uint32_t        received = 0U;
    const ra8_err_t error    = fw_fs_read(file, destination + offset, length - offset, &received);
    ++(*calls);
    if (error != k_ra8_ok) {
      return error;
    }
    if (received == 0U) {
      return k_ra8_err_invalid_state;
    }
    offset += received;
  }
  return k_ra8_ok;
}

/**
 * @brief Ensure a cache namespace component is a real directory.
 * @details Stats before and after optional creation so symlinks and special
 *          nodes can never satisfy the cache-directory contract.
 * @param[in,out] storage Bound portable filesystem.
 * @param[in] path Canonical absolute directory path.
 * @return Canonical stat or mkdir status.
 * @retval k_ra8_ok A real directory exists at the path.
 * @retval k_ra8_err_invalid_state The path resolves to another node type.
 * @retval other Namespace inspection or creation failed.
 * @pre Both pointers are non-NULL.
 * @pre @p path is confined by the filesystem binding.
 * @post Success proves a directory exists at @p path.
 * @post A symlink or special node is never accepted.
 * @note Safe for an existing directory.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_cache_ensure_directory(mdl_storage_t* storage,
                                                              const char*    path)
{
  fw_fs_stat_t node  = {};
  ra8_err_t    error = fw_fs_stat(&storage->fs->names, path, &node);
  if (error != k_ra8_ok) {
    return error;
  }
  if (!node.exists) {
    error = fw_fs_mkdir(&storage->fs->names, path);
    if (error != k_ra8_ok) {
      return error;
    }
    node  = (fw_fs_stat_t){};
    error = fw_fs_stat(&storage->fs->names, path, &node);
  }
  return ((error == k_ra8_ok) && node.exists && (node.type == k_fw_fs_node_directory))
           ? k_ra8_ok
           : k_ra8_err_invalid_state;
}

/**
 * @brief Derive and prepare one host-specific cache namespace.
 * @details Parses the canonical host, hashes its exact spelling, derives fixed
 *          leaves, and verifies both cache directory components.
 * @param[in,out] cache Cache binding.
 * @param[in] url Absolute URL.
 * @param[out] paths Derived paths and identity.
 * @return Canonical URL, path, or namespace status.
 * @retval k_ra8_ok Every path and the host identity were initialized.
 * @retval k_ra8_err_invalid_arg The URL or cache binding was invalid.
 * @retval k_ra8_err_invalid_size A derived path exceeded its bound.
 * @retval other Directory inspection or creation failed.
 * @pre All pointers are non-NULL.
 * @pre Cache root is canonical and absolute.
 * @post Success initializes every @p paths field.
 * @post Success proves root and host directory are real directories.
 * @note Host names are represented by a fixed hash leaf.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_cache_paths(mdl_cache_t* cache, const char* url, mdl_cache_paths_t* paths)
{
  *paths = (mdl_cache_paths_t){};
  if ((cache == nullptr) || (cache->storage == nullptr) || (cache->root == nullptr) ||
      (cache->root[0] != '/') || !mdl_url_host(url, paths->host, sizeof(paths->host))) {
    return k_ra8_err_invalid_arg;
  }
  paths->host_hash = mdl_hash_str(paths->host);
  char leaf[32];
  if (!internal_cache_host_leaf(leaf, sizeof(leaf), paths->host_hash) ||
      !mdl_path_join(cache->root, leaf, paths->directory, sizeof(paths->directory)) ||
      !mdl_path_join(paths->directory, "index.v1", paths->index_path, sizeof(paths->index_path))) {
    return k_ra8_err_invalid_size;
  }
  ra8_err_t error = internal_cache_ensure_directory(cache->storage, cache->root);
  return (error == k_ra8_ok) ? internal_cache_ensure_directory(cache->storage, paths->directory)
                             : error;
}

/**
 * @brief Validate one decoded persistent record.
 * @details Checks bounded strings, status/time domains, exact URL hash, and the
 *          parsed host identity before a record becomes selectable.
 * @param[in] record Candidate record.
 * @param[in] paths Bound host identity.
 * @return Whether all fields and identities are canonical.
 * @retval true The record is safe to retain and select.
 * @retval false A field or derived identity was invalid.
 * @pre Both pointers are non-NULL.
 * @pre String fields are NUL-terminated within their arrays.
 * @post Inputs remain unchanged.
 * @post True implies exact URL and host hashes match.
 * @note Body existence is validated separately.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_cache_record_valid(const mdl_cache_record_t* record,
                                                     const mdl_cache_paths_t*  paths)
{
  char       host[k_mdl_gov_host_max];
  const bool fields = (record->url[0] != '\0') && (record->relative_path[0] != '\0') &&
                      (strpbrk(record->url, "\t\r\n") == nullptr) &&
                      (strpbrk(record->relative_path, "/\\\t\r\n") == nullptr) &&
                      (strpbrk(record->etag, "\r\n") == nullptr) &&
                      (strpbrk(record->last_modified, "\r\n") == nullptr) &&
                      (record->fetched_at >= 0) &&
                      (record->response_status >= (uint16_t)k_cache_status_min) &&
                      (record->response_status <= (uint16_t)k_cache_status_max);
  return fields && (record->url_hash == mdl_hash_str(record->url)) &&
         mdl_url_host(record->url, host, sizeof(host)) && (mdl_hash_str(host) == paths->host_hash);
}

/**
 * @brief Decode one variable-length record and update its payload hash.
 * @details Authenticates the fixed header and each bounded string while
 *          advancing the file and running payload identity in lockstep.
 * @param[in,out] file Open index file.
 * @param[in,out] calls Read-call counter.
 * @param[in,out] hash Running payload hash.
 * @param[out] record Destination record.
 * @param[in] paths Bound host identity.
 * @return Canonical parse or validation status.
 * @retval k_ra8_ok One complete canonical record was decoded.
 * @retval k_ra8_err_invalid_size An encoded field exceeded its destination.
 * @retval k_ra8_err_invalid_state Reserved or semantic fields were invalid.
 * @retval other Exact file reads failed.
 * @pre Every pointer is non-NULL.
 * @pre File is positioned at a record header.
 * @post Success initializes @p record and advances to the next record.
 * @post Hash covers the complete encoded record.
 * @note Lengths are checked before string reads.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_cache_read_record(fw_fs_file_t*            file,
                                                         uint32_t*                calls,
                                                         uint64_t*                hash,
                                                         mdl_cache_record_t*      record,
                                                         const mdl_cache_paths_t* paths)
{
  uint8_t   header[k_cache_record_bytes];
  ra8_err_t error = internal_cache_read_all(file, header, sizeof(header), calls);
  if (error != k_ra8_ok) {
    return error;
  }
  *hash                       = mdl_hash_bytes_seed(header, sizeof(header), *hash);
  const uint16_t lengths[]    = {internal_cache_get_u16(&header[k_cache_record_url_len]),
                                 internal_cache_get_u16(&header[k_cache_record_path_len]),
                                 internal_cache_get_u16(&header[k_cache_record_etag_len]),
                                 internal_cache_get_u16(&header[k_cache_record_time_len])};
  const size_t   capacities[] = {sizeof(record->url),
                                 sizeof(record->relative_path),
                                 sizeof(record->etag),
                                 sizeof(record->last_modified)};
  char* fields[] = {record->url, record->relative_path, record->etag, record->last_modified};
  if ((internal_cache_get_u16(&header[k_cache_record_reserved]) != 0U) || (lengths[0] == 0U)) {
    return k_ra8_err_invalid_state;
  }
  *record =
    (mdl_cache_record_t){.url_hash        = internal_cache_get_u64(&header[0]),
                         .content_hash    = internal_cache_get_u64(&header[8]),
                         .fetched_at      = (int64_t)internal_cache_get_u64(&header[16]),
                         .response_status = internal_cache_get_u16(&header[k_cache_record_status])};
  for (size_t i = 0U; i < 4U; ++i) {
    if ((size_t)lengths[i] >= capacities[i]) {
      return k_ra8_err_invalid_size;
    }
    error = internal_cache_read_all(file, (uint8_t*)fields[i], lengths[i], calls);
    if (error != k_ra8_ok) {
      return error;
    }
    fields[i][lengths[i]] = '\0';
    *hash                 = mdl_hash_bytes_seed(fields[i], lengths[i], *hash);
  }
  return internal_cache_record_valid(record, paths) ? k_ra8_ok : k_ra8_err_invalid_state;
}

/**
 * @brief Decode and authenticate one complete index file.
 * @details Validates magic, schema, lengths, host binding, every record, exact
 *          file extent, and the payload trailer before accepting the index.
 * @param[in,out] cache Cache binding and destination workspace.
 * @param[in] paths Bound host paths.
 * @param[in] size_bytes Snapshotted file extent.
 * @return Canonical open, parse, or authentication status.
 * @retval k_ra8_ok The complete index was authenticated.
 * @retval k_ra8_err_invalid_state Structural or payload identity checks failed.
 * @retval other Opening, reading, or closing the index failed.
 * @pre Index path is a regular file of @p size_bytes.
 * @pre Cache workspace is exclusively owned.
 * @post Success publishes a complete validated index.
 * @post Failure leaves the workspace unspecified for caller reset.
 * @note Exact extent and payload hash are both checked.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_cache_decode(mdl_cache_t* cache, const mdl_cache_paths_t* paths, uint64_t size_bytes)
{
  fw_fs_file_t file                         = {};
  ra8_err_t    error                        = fw_fs_open(&cache->storage->fs->streams,
                                                         paths->index_path,
                                                         k_fw_fs_open_read,
                                                         &file,
                                                         cache->storage->file_workspace,
                                                         cache->storage->file_workspace_bytes);
  uint32_t     calls                        = 0U;
  uint8_t      header[k_cache_header_bytes] = {};
  if (error == k_ra8_ok) {
    error = internal_cache_read_all(&file, header, sizeof(header), &calls);
  }
  uint16_t count = 0U;
  if (error == k_ra8_ok) {
    count                        = internal_cache_get_u16(&header[k_cache_header_count]);
    const uint64_t payload_bytes = internal_cache_get_u64(&header[16]);
    const uint64_t expected_size =
      (uint64_t)k_cache_header_bytes + payload_bytes + (uint64_t)k_cache_trailer_bytes;
    if ((memcmp(header, s_cache_magic, sizeof(s_cache_magic)) != 0) ||
        (internal_cache_get_u16(&header[8]) != (uint16_t)k_mdl_cache_schema_version) ||
        (internal_cache_get_u16(&header[10]) != (uint16_t)k_cache_header_bytes) ||
        (count > (uint16_t)k_mdl_cache_record_max) || (internal_cache_get_u16(&header[14]) != 0U) ||
        (expected_size != size_bytes) ||
        (internal_cache_get_u64(&header[k_cache_header_host]) != paths->host_hash)) {
      error = k_ra8_err_invalid_state;
    }
  }
  *cache->index = (mdl_cache_index_t){.host_hash      = paths->host_hash,
                                      .schema_version = k_mdl_cache_schema_version};
  uint64_t hash = (uint64_t)k_mdl_fnv_offset;
  for (uint16_t i = 0U; (error == k_ra8_ok) && (i < count); ++i) {
    error = internal_cache_read_record(&file, &calls, &hash, &cache->index->records[i], paths);
    cache->index->record_count = (uint16_t)(i + 1U);
  }
  uint8_t trailer[k_cache_trailer_bytes];
  if (error == k_ra8_ok) {
    error = internal_cache_read_all(&file, trailer, sizeof(trailer), &calls);
  }
  if ((error == k_ra8_ok) && (internal_cache_get_u64(trailer) != hash)) {
    error = k_ra8_err_invalid_state;
  }
  const ra8_err_t closed = file.is_open ? fw_fs_close(&file) : k_ra8_ok;
  return (error == k_ra8_ok) ? closed : error;
}

/**
 * @brief Remove one corrupt regular index.
 * @details Restats the rejected path and unlinks it only when it remains a
 *          regular file, preserving symlinks and special nodes fail-closed.
 * @param[in,out] cache Cache binding.
 * @param[in] path Index path.
 * @return Canonical inspection or unlink status.
 * @retval k_ra8_ok The path is absent after the operation.
 * @retval k_ra8_err_invalid_state The path is not a regular file.
 * @retval other Stat or unlink failed.
 * @pre Both pointers are non-NULL.
 * @pre Load or validation has already rejected the file.
 * @post Success leaves the path absent.
 * @post Non-regular objects are preserved and rejected.
 * @note Cache data is disposable; library state is not handled here.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_cache_discard_index(mdl_cache_t* cache, const char* path)
{
  fw_fs_stat_t    node  = {};
  const ra8_err_t error = fw_fs_stat(&cache->storage->fs->names, path, &node);
  if (error != k_ra8_ok) {
    return error;
  }
  if (!node.exists) {
    return k_ra8_ok;
  }
  if (node.type != k_fw_fs_node_file) {
    return k_ra8_err_invalid_state;
  }
  return fw_fs_unlink(&cache->storage->fs->names, path);
}

RA8_PRIV ra8_err_t priv_mdl_cache_load(mdl_cache_t*       cache,
                                       const char*        url,
                                       mdl_cache_paths_t* paths,
                                       bool*              rebuilt)
{
  if ((cache == nullptr) || (cache->storage == nullptr) || (cache->index == nullptr) ||
      (url == nullptr) || (paths == nullptr) || (rebuilt == nullptr)) {
    return k_ra8_err_invalid_arg;
  }
  *rebuilt        = false;
  ra8_err_t error = internal_cache_paths(cache, url, paths);
  if (error != k_ra8_ok) {
    return error;
  }
  fw_fs_stat_t node = {};
  error             = fw_fs_stat(&cache->storage->fs->names, paths->index_path, &node);
  if (error != k_ra8_ok) {
    return error;
  }
  if (!node.exists) {
    *cache->index = (mdl_cache_index_t){.host_hash      = paths->host_hash,
                                        .schema_version = k_mdl_cache_schema_version};
    return k_ra8_ok;
  }
  if (node.type != k_fw_fs_node_file) {
    return k_ra8_err_invalid_state;
  }
  error = internal_cache_decode(cache, paths, node.size_bytes);
  if (error == k_ra8_ok) {
    return k_ra8_ok;
  }
  error = internal_cache_discard_index(cache, paths->index_path);
  if (error != k_ra8_ok) {
    return error;
  }
  *cache->index = (mdl_cache_index_t){.host_hash      = paths->host_hash,
                                      .schema_version = k_mdl_cache_schema_version};
  *rebuilt      = true;
  return k_ra8_ok;
}

/**
 * @brief Encode one record header and compute exact string lengths.
 * @details Measures every persistent string before initializing the canonical
 *          fixed-width header, preventing truncated identities.
 * @param[in] record Validated record.
 * @param[out] header Canonical fixed header.
 * @param[out] lengths Four encoded string extents.
 * @return Whether every string extent fits uint16.
 * @retval true Header and field lengths are complete.
 * @retval false At least one encoded field exceeds uint16.
 * @pre Every pointer is non-NULL.
 * @pre Record strings are NUL-terminated.
 * @post Success initializes all header bytes and lengths.
 * @post Failure performs no I/O.
 * @note Signed time is retained bit-for-bit as uint64.
 * @since 0.1.0
 */
RA8_INTERNAL static bool
internal_cache_encode_record(const mdl_cache_record_t* record, uint8_t* header, uint16_t* lengths)
{
  const char* fields[] = {record->url, record->relative_path, record->etag, record->last_modified};
  for (size_t i = 0U; i < 4U; ++i) {
    const size_t length = strlen(fields[i]);
    if (length > UINT16_MAX) {
      return false;
    }
    lengths[i] = (uint16_t)length;
  }
  memset(header, 0, k_cache_record_bytes);
  internal_cache_put_u64(&header[0], record->url_hash);
  internal_cache_put_u64(&header[8], record->content_hash);
  internal_cache_put_u64(&header[16], (uint64_t)record->fetched_at);
  internal_cache_put_u16(&header[k_cache_record_status], record->response_status);
  internal_cache_put_u16(&header[k_cache_record_url_len], lengths[0]);
  internal_cache_put_u16(&header[k_cache_record_path_len], lengths[1]);
  internal_cache_put_u16(&header[k_cache_record_etag_len], lengths[2]);
  internal_cache_put_u16(&header[k_cache_record_time_len], lengths[3]);
  return true;
}

/**
 * @brief Compute payload extent and hash for the current index.
 * @details Runs the same canonical record encoder used by publication and
 *          folds every header and string byte without materializing a blob.
 * @param[in] index Validated cache index.
 * @param[out] out_bytes Exact encoded payload extent.
 * @param[out] out_hash Exact encoded payload FNV identity.
 * @return Canonical validation or overflow status.
 * @retval k_ra8_ok Exact payload extent and identity were computed.
 * @retval k_ra8_err_invalid_size A record field cannot be encoded.
 * @pre All pointers are non-NULL.
 * @pre Record count is within capacity.
 * @post Success initializes both outputs.
 * @post No storage operation occurs.
 * @note The same encoder is used by publication.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_cache_payload_identity(const mdl_cache_index_t* index,
                                                              uint64_t*                out_bytes,
                                                              uint64_t*                out_hash)
{
  uint64_t bytes = 0U;
  uint64_t hash  = (uint64_t)k_mdl_fnv_offset;
  for (uint16_t i = 0U; i < index->record_count; ++i) {
    uint8_t  header[k_cache_record_bytes];
    uint16_t lengths[4];
    if (!internal_cache_encode_record(&index->records[i], header, lengths)) {
      return k_ra8_err_invalid_size;
    }
    hash = mdl_hash_bytes_seed(header, sizeof(header), hash);
    bytes += sizeof(header);
    const char* fields[] = {index->records[i].url,
                            index->records[i].relative_path,
                            index->records[i].etag,
                            index->records[i].last_modified};
    for (size_t field = 0U; field < 4U; ++field) {
      hash = mdl_hash_bytes_seed(fields[field], lengths[field], hash);
      bytes += lengths[field];
    }
  }
  *out_bytes = bytes;
  *out_hash  = hash;
  return k_ra8_ok;
}

/**
 * @brief Stream every encoded record into an active transaction.
 * @details Writes each fixed record header followed by its four exact string
 *          spans, matching the prior payload-identity pass byte for byte.
 * @param[in,out] writer Active index transaction.
 * @param[in] index Validated cache index.
 * @return Canonical encoding or write status.
 * @retval k_ra8_ok Every record byte was staged.
 * @retval k_ra8_err_invalid_size A record field cannot be encoded.
 * @retval other Transaction writing failed.
 * @pre Both pointers are non-NULL.
 * @pre @p writer owns an active transaction.
 * @post Success writes the exact payload used for identity calculation.
 * @post Failure leaves the transaction active for caller abort.
 * @note Strings are written without terminating NUL bytes.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_cache_write_records(mdl_storage_txn_t*       writer,
                                                           const mdl_cache_index_t* index)
{
  for (uint16_t i = 0U; i < index->record_count; ++i) {
    uint8_t  header[k_cache_record_bytes];
    uint16_t lengths[4];
    if (!internal_cache_encode_record(&index->records[i], header, lengths)) {
      return k_ra8_err_invalid_size;
    }
    ra8_err_t   error    = mdl_storage_txn_write(writer, header, sizeof(header));
    const char* fields[] = {index->records[i].url,
                            index->records[i].relative_path,
                            index->records[i].etag,
                            index->records[i].last_modified};
    for (size_t field = 0U; (error == k_ra8_ok) && (field < 4U); ++field) {
      error = mdl_storage_txn_write(writer, (const uint8_t*)fields[field], lengths[field]);
    }
    if (error != k_ra8_ok) {
      return error;
    }
  }
  return k_ra8_ok;
}

RA8_PRIV ra8_err_t priv_mdl_cache_save(mdl_cache_t* cache, const mdl_cache_paths_t* paths)
{
  if ((cache == nullptr) || (cache->storage == nullptr) || (cache->index == nullptr) ||
      (paths == nullptr) || (cache->index->schema_version != k_mdl_cache_schema_version) ||
      (cache->index->host_hash != paths->host_hash) ||
      (cache->index->record_count > (uint16_t)k_mdl_cache_record_max)) {
    return k_ra8_err_invalid_arg;
  }
  for (uint16_t i = 0U; i < cache->index->record_count; ++i) {
    if (!internal_cache_record_valid(&cache->index->records[i], paths)) {
      return k_ra8_err_invalid_state;
    }
  }
  uint64_t  payload_bytes = 0U;
  uint64_t  payload_hash  = 0U;
  ra8_err_t error = internal_cache_payload_identity(cache->index, &payload_bytes, &payload_hash);
  uint8_t   header[k_cache_header_bytes] = {};
  memcpy(header, s_cache_magic, sizeof(s_cache_magic));
  internal_cache_put_u16(&header[8], (uint16_t)k_mdl_cache_schema_version);
  internal_cache_put_u16(&header[10], (uint16_t)k_cache_header_bytes);
  internal_cache_put_u16(&header[k_cache_header_count], cache->index->record_count);
  internal_cache_put_u64(&header[16], payload_bytes);
  internal_cache_put_u64(&header[k_cache_header_host], paths->host_hash);
  uint8_t trailer[k_cache_trailer_bytes];
  internal_cache_put_u64(trailer, payload_hash);
  mdl_storage_txn_t writer = {};
  if (error == k_ra8_ok) {
    error = mdl_storage_txn_begin(&writer, cache->storage, paths->index_path);
  }
  if (error == k_ra8_ok) {
    error = mdl_storage_txn_write(&writer, header, sizeof(header));
  }
  if (error == k_ra8_ok) {
    error = internal_cache_write_records(&writer, cache->index);
  }
  if (error == k_ra8_ok) {
    error = mdl_storage_txn_write(&writer, trailer, sizeof(trailer));
  }
  if (error == k_ra8_ok) {
    return mdl_storage_txn_commit(&writer);
  }
  const ra8_err_t aborted = mdl_storage_txn_abort(&writer);
  return (aborted == k_ra8_ok) ? error : aborted;
}

/**
 * @brief Read a body file's exact declared extent and confirm no trailer.
 * @details Opens @p body_path, reads exactly @p size_bytes into @p buffer,
 *          then probes for one more byte to reject a file that grew past its
 *          recorded size between stat and read, and always closes the
 *          handle even on a read failure.
 * @param[in,out] storage Bound filesystem interface and shared read workspace.
 * @param[in] body_path Complete path of the body file.
 * @param[out] buffer Destination of exactly @p size_bytes on success.
 * @param[in] size_bytes Exact expected body length in bytes.
 * @return Read-and-close status.
 * @retval k_ra8_ok Exactly @p size_bytes were read and the file closed cleanly.
 * @retval k_ra8_err_invalid_state A byte remained after @p size_bytes.
 * @retval other The open, read, or close call failed.
 * @pre @p storage, @p body_path, and @p buffer are non-NULL.
 * @pre @p buffer holds at least @p size_bytes writable bytes.
 * @post The file handle is closed on every return path.
 * @post On success exactly @p size_bytes sit in @p buffer; a failure leaves
 *       its contents unspecified.
 * @note Not thread-safe; shares the caller's file workspace.
 * @since Version 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_cache_read_body_exact(mdl_storage_t* storage,
                                                             const char*    body_path,
                                                             char*          buffer,
                                                             uint64_t       size_bytes)
{
  fw_fs_file_t file  = {};
  ra8_err_t    error = fw_fs_open(&storage->fs->streams,
                                  body_path,
                                  k_fw_fs_open_read,
                                  &file,
                                  storage->file_workspace,
                                  storage->file_workspace_bytes);
  uint32_t     calls = 0U;
  if (error == k_ra8_ok) {
    error = internal_cache_read_all(&file, (uint8_t*)buffer, (uint32_t)size_bytes, &calls);
  }
  uint8_t  extra = 0U;
  uint32_t got   = 0U;
  if (error == k_ra8_ok) {
    error = fw_fs_read(&file, &extra, 1U, &got);
  }
  if ((error == k_ra8_ok) && (got != 0U)) {
    error = k_ra8_err_invalid_state;
  }
  const ra8_err_t closed = file.is_open ? fw_fs_close(&file) : k_ra8_ok;
  if (error == k_ra8_ok) {
    error = closed;
  }
  return error;
}

RA8_PRIV ra8_err_t priv_mdl_cache_read_body(mdl_storage_t*            storage,
                                            const mdl_cache_paths_t*  paths,
                                            const mdl_cache_record_t* record,
                                            char*                     buffer,
                                            size_t                    capacity,
                                            size_t*                   out_length)
{
  if ((storage == nullptr) || (paths == nullptr) || (record == nullptr) || (buffer == nullptr) ||
      (capacity == 0U) || (out_length == nullptr)) {
    return k_ra8_err_invalid_arg;
  }
  *out_length = 0U;
  char body_path[k_fw_fs_path_cap];
  if (!mdl_path_join(paths->directory, record->relative_path, body_path, sizeof(body_path))) {
    return k_ra8_err_invalid_size;
  }
  fw_fs_stat_t node  = {};
  ra8_err_t    error = fw_fs_stat(&storage->fs->names, body_path, &node);
  if ((error != k_ra8_ok) || !node.exists || (node.type != k_fw_fs_node_file)) {
    return (error != k_ra8_ok) ? error : k_ra8_err_not_found;
  }
  if ((node.size_bytes == 0U) || (node.size_bytes > capacity) || (node.size_bytes > UINT32_MAX)) {
    return k_ra8_err_invalid_size;
  }
  error = internal_cache_read_body_exact(storage, body_path, buffer, node.size_bytes);
  if ((error == k_ra8_ok) &&
      (mdl_hash_bytes(buffer, (size_t)node.size_bytes) != record->content_hash)) {
    error = k_ra8_err_validation_failed;
  }
  if (error == k_ra8_ok) {
    *out_length = (size_t)node.size_bytes;
  }
  return error;
}

RA8_PRIV ra8_err_t priv_mdl_cache_publish_body(mdl_storage_t*           storage,
                                               const mdl_cache_paths_t* paths,
                                               uint64_t                 url_hash,
                                               uint64_t                 content_hash,
                                               const char*              buffer,
                                               size_t                   length,
                                               char*                    relative_path,
                                               size_t                   relative_capacity)
{
  if ((storage == nullptr) || (paths == nullptr) || (buffer == nullptr) || (length == 0U) ||
      (length > UINT32_MAX) || (relative_path == nullptr) || (relative_capacity == 0U)) {
    return k_ra8_err_invalid_arg;
  }
  char body_path[k_fw_fs_path_cap];
  if (!internal_cache_body_leaf(relative_path, relative_capacity, url_hash, content_hash) ||
      !mdl_path_join(paths->directory, relative_path, body_path, sizeof(body_path))) {
    return k_ra8_err_invalid_size;
  }
  mdl_storage_txn_t writer = {};
  ra8_err_t         error  = mdl_storage_txn_begin(&writer, storage, body_path);
  if (error == k_ra8_ok) {
    error = mdl_storage_txn_write(&writer, (const uint8_t*)buffer, (uint32_t)length);
  }
  if (error == k_ra8_ok) {
    return mdl_storage_txn_commit(&writer);
  }
  const ra8_err_t aborted = mdl_storage_txn_abort(&writer);
  return (aborted == k_ra8_ok) ? error : aborted;
}
