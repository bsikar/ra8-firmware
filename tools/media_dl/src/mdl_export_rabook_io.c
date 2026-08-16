/**
 * @file mdl_export_rabook_io.c
 * @brief Portable temporary EPUB and flat-blob adapters for RABOOK export.
 * @details Owns bounded collision handling plus exact random-read callbacks;
 *          it contains no compiler profile or final publication policy.
 *
 * [Ring 4 / Domain] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <string.h>

#include "mdl_export_rabook_internal.h"

/** @brief Bounded I/O and collision-attempt limits. */
typedef enum : uint32_t {
  k_rabook_temp_attempts = 16U,         /**< Create-new sibling candidates. */
  k_rabook_read_calls    = 1000000U,    /**< EPUB random-read ceiling.      */
  k_rabook_fnv_offset    = 2166136261U, /**< FNV-1a seed for temp names.    */
  k_rabook_fnv_prime     = 16777619U,   /**< FNV-1a multiplier.             */
  k_rabook_hex_digits    = 7U,          /**< Hex digits in a temp leaf.     */
  k_rabook_hex_mask      = 0x0FU,       /**< One hexadecimal nibble.        */
  k_rabook_suffix_bytes  = 5U,          /**< `.EPB` plus NUL terminator.    */
  k_rabook_value_mask    = 0x0FFFFFFFU, /**< Low 28 temporary-name bits.    */
} mdl_rabook_io_limit_t;

/**
 * @brief Hash one destination into a stable temporary-name seed.
 * @details Applies 32-bit FNV-1a to the complete NUL-terminated path.
 * @param[in] destination Stable final destination path.
 * @return Deterministic non-cryptographic path hash.
 * @retval other The 32-bit FNV-1a hash of the complete destination string.
 * @pre @p destination is non-NULL and NUL-terminated.
 * @pre The string remains stable for the call.
 * @post No caller-owned memory is changed.
 * @post Equal byte strings produce equal hashes.
 * @note Used only to spread collision candidates, never for trust.
 * @since 0.1.0
 */
RA8_INTERNAL static uint32_t internal_temp_seed(const char* destination)
{
  uint32_t hash = (uint32_t)k_rabook_fnv_offset;
  for (size_t index = 0U; destination[index] != '\0'; ++index) {
    hash ^= (uint8_t)destination[index];
    hash *= (uint32_t)k_rabook_fnv_prime;
  }
  return hash;
}

/**
 * @brief Render one collision candidate as an 8.3-compatible EPUB leaf.
 * @details Writes `R` plus seven uppercase hexadecimal digits and `.EPB`
 *          without a variadic formatter or hidden stream dependency.
 * @param[in] value Low 28 bits used for the hexadecimal suffix.
 * @param[out] leaf Writable thirteen-byte leaf buffer.
 * @pre @p leaf is non-NULL and spans exactly thirteen writable bytes.
 * @pre Higher bits of @p value are ignored by design.
 * @post @p leaf contains one complete NUL-terminated 8.3 name.
 * @post No global or filesystem state is changed.
 * @note Thread-safe across distinct destinations.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_temp_leaf(uint32_t value, char leaf[13])
{
  static const char k_hex[] = "0123456789ABCDEF";
  leaf[0]                   = 'R';
  for (uint32_t index = 0U; index < (uint32_t)k_rabook_hex_digits; ++index) {
    const uint32_t shift = ((uint32_t)k_rabook_hex_digits - 1U - index) * 4U;
    leaf[index + 1U]     = k_hex[(value >> shift) & (uint32_t)k_rabook_hex_mask];
  }
  (void)memcpy(&leaf[8], ".EPB", (size_t)k_rabook_suffix_bytes);
}

RA8_PRIV ra8_err_t priv_mdl_rabook_temp_begin(mdl_export_output_t* output,
                                              mdl_storage_t*       storage,
                                              const char*          directory,
                                              const char*          destination,
                                              char*                path,
                                              size_t               capacity)
{
  if ((output == nullptr) || (storage == nullptr) || (directory == nullptr) ||
      (destination == nullptr) || (path == nullptr) || (capacity == 0U)) {
    return k_ra8_err_invalid_arg;
  }
  const uint32_t seed = internal_temp_seed(destination) & (uint32_t)k_rabook_value_mask;
  for (uint32_t attempt = 0U; attempt < (uint32_t)k_rabook_temp_attempts; ++attempt) {
    char           leaf[13] = {};
    const uint32_t value    = (seed + attempt) & (uint32_t)k_rabook_value_mask;
    internal_temp_leaf(value, leaf);
    if (priv_mdl_export_path_join(path, capacity, directory, leaf) != k_ra8_ok) {
      return k_ra8_err_invalid_size;
    }
    const ra8_err_t error = priv_mdl_export_output_begin_new(output, storage, path, k_mdl_fmt_epub);
    if (error == k_ra8_ok) {
      return k_ra8_ok;
    }
    if (error != k_ra8_err_exists) {
      return error;
    }
  }
  return k_ra8_err_exists;
}

RA8_PRIV ra8_err_t priv_mdl_rabook_epub_open(mdl_rabook_epub_source_t* source,
                                             mdl_storage_t*            storage,
                                             const char*               path)
{
  if ((source == nullptr) || (storage == nullptr) || (storage->fs == nullptr) ||
      (path == nullptr)) {
    return k_ra8_err_invalid_arg;
  }
  fw_fs_stat_t stat  = {};
  ra8_err_t    error = fw_fs_stat(&storage->fs->names, path, &stat);
  if ((error == k_ra8_ok) &&
      (!stat.exists || (stat.type != k_fw_fs_node_file) || (stat.size_bytes == 0U))) {
    error = k_ra8_err_invalid_arg;
  }
  *source = (mdl_rabook_epub_source_t){};
  if (error == k_ra8_ok) {
    source->size_bytes = stat.size_bytes;
    error              = fw_fs_open(&storage->fs->streams,
                                    path,
                                    k_fw_fs_open_read,
                                    &source->file,
                                    storage->file_workspace,
                                    storage->file_workspace_bytes);
  }
  uint64_t open_size = 0U;
  if (error == k_ra8_ok) {
    error = fw_fs_file_size(&source->file, &open_size);
  }
  if ((error == k_ra8_ok) && (open_size != source->size_bytes)) {
    error = k_ra8_err_protocol_error;
  }
  if ((error != k_ra8_ok) && source->file.is_open) {
    (void)fw_fs_close(&source->file);
  }
  if (error != k_ra8_ok) {
    *source = (mdl_rabook_epub_source_t){};
  }
  return error;
}

RA8_PRIV ra8_err_t priv_mdl_rabook_epub_close(mdl_rabook_epub_source_t* source)
{
  if ((source == nullptr) || !source->file.is_open) {
    return k_ra8_err_invalid_arg;
  }
  const ra8_err_t error = fw_fs_close(&source->file);
  if (error == k_ra8_ok) {
    *source = (mdl_rabook_epub_source_t){};
  }
  return error;
}

RA8_PRIV size_t priv_mdl_rabook_epub_read(void*    opaque,
                                          uint64_t offset,
                                          void*    destination,
                                          size_t   length)
{
  mdl_rabook_epub_source_t* source = (mdl_rabook_epub_source_t*)opaque;
  if ((source == nullptr) || (destination == nullptr) || !source->file.is_open ||
      (source->error != k_ra8_ok) || (length > UINT32_MAX) || (offset > source->size_bytes) ||
      ((uint64_t)length > (source->size_bytes - offset))) {
    if ((source != nullptr) && (source->error == k_ra8_ok)) {
      source->error = k_ra8_err_out_of_range;
    }
    return 0U;
  }
  ra8_err_t error = fw_fs_seek(&source->file, offset);
  size_t    done  = 0U;
  while ((error == k_ra8_ok) && (done < length)) {
    if (source->calls >= (uint32_t)k_rabook_read_calls) {
      error = k_ra8_err_invalid_size;
      break;
    }
    uint32_t got = 0U;
    error =
      fw_fs_read(&source->file, &((uint8_t*)destination)[done], (uint32_t)(length - done), &got);
    ++source->calls;
    if ((error == k_ra8_ok) && (got == 0U)) {
      error = k_ra8_err_invalid_state;
    }
    done += got;
  }
  if (error != k_ra8_ok) {
    source->error = error;
    return 0U;
  }
  return done;
}

RA8_PRIV ra8_err_t priv_mdl_rabook_flat_read(void*     opaque,
                                             uint32_t  offset,
                                             uint8_t*  destination,
                                             uint32_t  requested,
                                             uint32_t* out_read)
{
  mdl_rabook_flat_source_t* source = (mdl_rabook_flat_source_t*)opaque;
  if (out_read != nullptr) {
    *out_read = 0U;
  }
  if ((source == nullptr) || (destination == nullptr) || (out_read == nullptr)) {
    return k_ra8_err_invalid_arg;
  }
  if ((offset > source->size_bytes) || (requested > (source->size_bytes - offset))) {
    return k_ra8_err_out_of_range;
  }
  (void)memcpy(destination, &source->bytes[offset], requested);
  *out_read = requested;
  return k_ra8_ok;
}
