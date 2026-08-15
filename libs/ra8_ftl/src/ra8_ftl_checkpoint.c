/**
 * @file ra8_ftl_checkpoint.c
 * @brief Canonical persistent metadata for the flash translation layer.
 * @ingroup grp_storage
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * Encodes FTL mapping state field-by-field in a versioned little-endian wire
 * format. Restore is transactional: header, checksum, geometry, ranges,
 * duplicate mappings, and map/state invariants are checked with the FTL's
 * caller-owned 512-byte scratch block before either live table is modified.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_ftl.h"

/** @brief Module log tag. */
static const char* const s_tag = "ra8_ftl_checkpoint";

/**
 * @enum ra8_ftl_checkpoint_const_t
 * @brief Canonical wire-layout and validation constants.
 * @details All offsets are byte offsets from the checkpoint beginning. The
 *          trailer is CRC-32/ISO-HDLC over every preceding byte.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ck_magic                = 0x4C544652U, /**< LE bytes `RFTL`.                     */
  k_ck_legacy_magic_le      = 0x46544C31U, /**< Legacy native LE `FTL1` value.       */
  k_ck_legacy_magic_swapped = 0x314C5446U, /**< Legacy native BE `FTL1` bytes.       */
  k_ck_version              = 1U,          /**< Canonical format version.            */
  k_ck_header_bytes         = 20U,         /**< Fixed version-1 header bytes.        */
  k_ck_crc_bytes            = 4U,          /**< CRC-32 trailer bytes.                */
  k_ck_fixed_bytes          = 24U,         /**< Header plus trailer.                 */
  k_ck_map_entry_bytes      = 2U,          /**< One LE16 physical-map entry.         */
  k_ck_pblock_entry_bytes   = 5U,          /**< LE32 erase count plus state byte.    */
  k_ck_pblock_state_offset  = 4U,          /**< State byte within one pblock record. */
  k_ck_off_version          = 4U,          /**< LE16 format version.                 */
  k_ck_off_header_bytes     = 6U,          /**< LE16 fixed-header size.              */
  k_ck_off_total_bytes      = 8U,          /**< LE32 exact blob length.              */
  k_ck_off_logical_blocks   = 12U,         /**< LE32 logical geometry.               */
  k_ck_off_physical_blocks  = 16U,         /**< LE32 physical geometry.              */
  k_ck_scratch_bytes        = 512U,        /**< Caller workspace guaranteed by FTL.  */
  k_ck_bits_per_byte        = 8U,          /**< Bitmap packing factor.               */
  k_ck_byte_3_shift         = 24U,         /**< Shift of byte three in a LE32.       */
  k_ck_bitmap_blocks        = 4096U,       /**< Physical indices per scratch window. */
  k_ck_crc_seed             = 0xFFFFFFFFU, /**< CRC initial/final XOR.               */
  k_ck_crc_poly             = 0xEDB88320U, /**< Reflected ISO-HDLC polynomial.       */
} ra8_ftl_checkpoint_const_t;

/** @brief Decode one canonical little-endian 16-bit field. */
RA8_INTERNAL static uint16_t internal_get_le16(const uint8_t* in)
{
  return (uint16_t)((uint16_t)in[0] | ((uint16_t)in[1] << 8U));
}

/** @brief Decode one canonical little-endian 32-bit field. */
RA8_INTERNAL static uint32_t internal_get_le32(const uint8_t* in)
{
  return (uint32_t)in[0] | ((uint32_t)in[1] << 8U) | ((uint32_t)in[2] << 16U) |
         ((uint32_t)in[3] << (uint32_t)k_ck_byte_3_shift);
}

/** @brief Encode one canonical little-endian 16-bit field. */
RA8_INTERNAL static void internal_put_le16(uint8_t* out, uint16_t value)
{
  out[0] = (uint8_t)value;
  out[1] = (uint8_t)(value >> 8U);
}

/** @brief Encode one canonical little-endian 32-bit field. */
RA8_INTERNAL static void internal_put_le32(uint8_t* out, uint32_t value)
{
  out[0] = (uint8_t)value;
  out[1] = (uint8_t)(value >> 8U);
  out[2] = (uint8_t)(value >> 16U);
  out[3] = (uint8_t)(value >> (uint32_t)k_ck_byte_3_shift);
}

/** @brief Compute CRC-32/ISO-HDLC over a bounded byte span. */
RA8_INTERNAL static uint32_t internal_crc32(const uint8_t* data, uint32_t length)
{
  uint32_t crc = (uint32_t)k_ck_crc_seed;
  for (uint32_t i = 0U; i < length; ++i) {
    crc ^= data[i];
    for (uint8_t bit = 0U; bit < (uint8_t)k_ck_bits_per_byte; ++bit) {
      const uint32_t mask = (uint32_t)(0U - (crc & 1U));
      crc                 = (crc >> 1U) ^ ((uint32_t)k_ck_crc_poly & mask);
    }
  }
  return crc ^ (uint32_t)k_ck_crc_seed;
}

/** @brief Fail closed when two byte ranges overlap or their endpoints wrap. */
RA8_INTERNAL static bool internal_ranges_overlap(const void* first,
                                                 uint32_t    first_bytes,
                                                 const void* second,
                                                 uint32_t    second_bytes)
{
  const uintptr_t first_start  = (uintptr_t)first;
  const uintptr_t second_start = (uintptr_t)second;
  if (first_bytes == 0U || second_bytes == 0U) {
    return false;
  }
  if (first_start > UINTPTR_MAX - ((uintptr_t)first_bytes - 1U)) {
    return true;
  }
  if (second_start > UINTPTR_MAX - ((uintptr_t)second_bytes - 1U)) {
    return true;
  }
  const uintptr_t first_end  = first_start + (uintptr_t)first_bytes - 1U;
  const uintptr_t second_end = second_start + (uintptr_t)second_bytes - 1U;
  return (first_start <= second_end) && (second_start <= first_end);
}

/** @brief Validate that a checkpoint-capable FTL handle is fully bound. */
RA8_INTERNAL static ra8_err_t internal_ready(const ra8_ftl_t* ftl)
{
  if (ftl->raw == nullptr) {
    return k_ra8_err_not_initialized;
  }
  if (ftl->map == nullptr) {
    return k_ra8_err_not_initialized;
  }
  if (ftl->pblocks == nullptr) {
    return k_ra8_err_not_initialized;
  }
  return (ftl->scratch == nullptr) ? k_ra8_err_not_initialized : k_ra8_ok;
}

/** @brief Calculate the exact wire length with checked 32-bit arithmetic. */
RA8_INTERNAL static ra8_err_t
internal_size_values(uint32_t logical_blocks, uint32_t physical_blocks, uint32_t* out)
{
  if (logical_blocks == 0U || physical_blocks > (uint32_t)k_ra8_ftl_max_pblocks) {
    return k_ra8_err_invalid_size;
  }
  if (physical_blocks <= logical_blocks) {
    return k_ra8_err_invalid_size;
  }
  uint32_t total = (uint32_t)k_ck_fixed_bytes;
  if (logical_blocks > (UINT32_MAX - total) / (uint32_t)k_ck_map_entry_bytes) {
    return k_ra8_err_invalid_size;
  }
  total += logical_blocks * (uint32_t)k_ck_map_entry_bytes;
  if (physical_blocks > (UINT32_MAX - total) / (uint32_t)k_ck_pblock_entry_bytes) {
    return k_ra8_err_invalid_size;
  }
  *out = total + (physical_blocks * (uint32_t)k_ck_pblock_entry_bytes);
  return k_ra8_ok;
}

/** @brief Validate workspace and checkpoint-buffer non-aliasing. */
RA8_INTERNAL static ra8_err_t
internal_disjoint(const ra8_ftl_t* ftl, const void* buffer, uint32_t buffer_bytes)
{
  const uint32_t map_bytes = ftl->logical_blocks * (uint32_t)sizeof(ftl->map[0]);
  const uint32_t pb_bytes  = ftl->physical_blocks * (uint32_t)sizeof(ftl->pblocks[0]);
  if (internal_ranges_overlap(ftl->scratch, (uint32_t)k_ck_scratch_bytes, ftl->map, map_bytes)) {
    return k_ra8_err_invalid_state;
  }
  if (internal_ranges_overlap(ftl->scratch, (uint32_t)k_ck_scratch_bytes, ftl->pblocks, pb_bytes)) {
    return k_ra8_err_invalid_state;
  }
  if (internal_ranges_overlap(buffer, buffer_bytes, ftl->map, map_bytes)) {
    return k_ra8_err_invalid_arg;
  }
  if (internal_ranges_overlap(buffer, buffer_bytes, ftl->pblocks, pb_bytes)) {
    return k_ra8_err_invalid_arg;
  }
  return internal_ranges_overlap(buffer, buffer_bytes, ftl->scratch, (uint32_t)k_ck_scratch_bytes)
           ? k_ra8_err_invalid_arg
           : k_ra8_ok;
}

/** @brief Set one scratch-bitmap bit and report whether it was already set. */
RA8_INTERNAL static bool internal_bit_was_set(uint8_t* bitmap, uint32_t bit)
{
  const uint32_t byte = bit / (uint32_t)k_ck_bits_per_byte;
  const uint8_t  mask = (uint8_t)(1U << (bit % (uint32_t)k_ck_bits_per_byte));
  const bool     set  = (bitmap[byte] & mask) != 0U;
  bitmap[byte] |= mask;
  return set;
}

/** @brief Read one bit from the bounded scratch bitmap. */
RA8_INTERNAL static bool internal_bit_is_set(const uint8_t* bitmap, uint32_t bit)
{
  const uint32_t byte = bit / (uint32_t)k_ck_bits_per_byte;
  const uint8_t  mask = (uint8_t)(1U << (bit % (uint32_t)k_ck_bits_per_byte));
  return (bitmap[byte] & mask) != 0U;
}

/** @brief Validate one window of the live native mapping into scratch. */
RA8_INTERNAL static ra8_err_t
internal_native_window(const ra8_ftl_t* ftl, uint32_t base, uint32_t count)
{
  (void)memset(ftl->scratch, 0, (size_t)k_ck_scratch_bytes);
  for (uint32_t lbn = 0U; lbn < ftl->logical_blocks; ++lbn) {
    const uint16_t phys = ftl->map[lbn];
    if (phys == (uint16_t)k_ra8_ftl_unmapped) {
      continue;
    }
    if ((uint32_t)phys >= ftl->physical_blocks) {
      return k_ra8_err_invalid_state;
    }
    if ((uint32_t)phys >= base && (uint32_t)phys - base < count) {
      if (internal_bit_was_set(ftl->scratch, (uint32_t)phys - base)) {
        return k_ra8_err_invalid_state;
      }
    }
  }
  for (uint32_t rel = 0U; rel < count; ++rel) {
    const uint8_t state = ftl->pblocks[base + rel].state;
    if (state > (uint8_t)k_ra8_ftl_pstate_stale) {
      return k_ra8_err_invalid_state;
    }
    if (internal_bit_is_set(ftl->scratch, rel) != (state == (uint8_t)k_ra8_ftl_pstate_live)) {
      return k_ra8_err_invalid_state;
    }
  }
  return k_ra8_ok;
}

/** @brief Validate every live map/state invariant using bounded scratch
 * windows. */
RA8_INTERNAL static ra8_err_t internal_validate_native(const ra8_ftl_t* ftl)
{
  for (uint32_t base = 0U; base < ftl->physical_blocks; base += (uint32_t)k_ck_bitmap_blocks) {
    const uint32_t left = ftl->physical_blocks - base;
    const uint32_t count =
      (left < (uint32_t)k_ck_bitmap_blocks) ? left : (uint32_t)k_ck_bitmap_blocks;
    const ra8_err_t valid = internal_native_window(ftl, base, count);
    if (valid != k_ra8_ok) {
      return valid;
    }
  }
  return k_ra8_ok;
}

/** @brief Validate one canonical wire-map window into caller scratch. */
RA8_INTERNAL static ra8_err_t internal_wire_window(const ra8_ftl_t* ftl,
                                                   const uint8_t*   buf,
                                                   uint32_t         map_offset,
                                                   uint32_t         pb_offset,
                                                   uint32_t         base,
                                                   uint32_t         count)
{
  (void)memset(ftl->scratch, 0, (size_t)k_ck_scratch_bytes);
  for (uint32_t lbn = 0U; lbn < ftl->logical_blocks; ++lbn) {
    const uint32_t entry = map_offset + (lbn * (uint32_t)k_ck_map_entry_bytes);
    const uint16_t phys  = internal_get_le16(&buf[entry]);
    if (phys == (uint16_t)k_ra8_ftl_unmapped) {
      continue;
    }
    if ((uint32_t)phys >= ftl->physical_blocks) {
      return k_ra8_err_invalid_state;
    }
    if ((uint32_t)phys >= base && (uint32_t)phys - base < count) {
      if (internal_bit_was_set(ftl->scratch, (uint32_t)phys - base)) {
        return k_ra8_err_invalid_state;
      }
    }
  }
  for (uint32_t rel = 0U; rel < count; ++rel) {
    const uint32_t entry = pb_offset + ((base + rel) * (uint32_t)k_ck_pblock_entry_bytes);
    const uint8_t  state = buf[entry + (uint32_t)k_ck_pblock_state_offset];
    if (state > (uint8_t)k_ra8_ftl_pstate_stale) {
      return k_ra8_err_invalid_state;
    }
    if (internal_bit_is_set(ftl->scratch, rel) != (state == (uint8_t)k_ra8_ftl_pstate_live)) {
      return k_ra8_err_invalid_state;
    }
  }
  return k_ra8_ok;
}

/** @brief Validate every canonical payload invariant without live mutation. */
RA8_INTERNAL static ra8_err_t internal_validate_wire(const ra8_ftl_t* ftl, const uint8_t* buf)
{
  const uint32_t map_offset = (uint32_t)k_ck_header_bytes;
  const uint32_t pb_offset  = map_offset + (ftl->logical_blocks * (uint32_t)k_ck_map_entry_bytes);
  for (uint32_t base = 0U; base < ftl->physical_blocks; base += (uint32_t)k_ck_bitmap_blocks) {
    const uint32_t left = ftl->physical_blocks - base;
    const uint32_t count =
      (left < (uint32_t)k_ck_bitmap_blocks) ? left : (uint32_t)k_ck_bitmap_blocks;
    const ra8_err_t valid = internal_wire_window(ftl, buf, map_offset, pb_offset, base, count);
    if (valid != k_ra8_ok) {
      return valid;
    }
  }
  return k_ra8_ok;
}

/** @brief Validate the canonical header, exact length, geometry, and CRC. */
RA8_INTERNAL static ra8_err_t
internal_validate_header(const ra8_ftl_t* ftl, const uint8_t* buf, uint32_t buf_len, uint32_t need)
{
  if (buf_len < (uint32_t)k_ck_crc_bytes) {
    return k_ra8_err_invalid_size;
  }
  const uint32_t magic = internal_get_le32(buf);
  if (magic == (uint32_t)k_ck_legacy_magic_le || magic == (uint32_t)k_ck_legacy_magic_swapped) {
    return k_ra8_err_not_supported;
  }
  if (magic != (uint32_t)k_ck_magic) {
    return k_ra8_err_invalid_state;
  }
  if (buf_len < (uint32_t)k_ck_fixed_bytes) {
    return k_ra8_err_invalid_size;
  }
  if (internal_get_le16(&buf[k_ck_off_version]) != (uint16_t)k_ck_version) {
    return k_ra8_err_not_supported;
  }
  if (internal_get_le16(&buf[k_ck_off_header_bytes]) != (uint16_t)k_ck_header_bytes) {
    return k_ra8_err_invalid_size;
  }
  if (internal_get_le32(&buf[k_ck_off_total_bytes]) != buf_len || buf_len != need) {
    return k_ra8_err_invalid_size;
  }
  if (internal_get_le32(&buf[k_ck_off_logical_blocks]) != ftl->logical_blocks) {
    return k_ra8_err_invalid_arg;
  }
  if (internal_get_le32(&buf[k_ck_off_physical_blocks]) != ftl->physical_blocks) {
    return k_ra8_err_invalid_arg;
  }
  const uint32_t crc_offset = buf_len - (uint32_t)k_ck_crc_bytes;
  return (internal_crc32(buf, crc_offset) == internal_get_le32(&buf[crc_offset]))
           ? k_ra8_ok
           : k_ra8_err_crc_mismatch;
}

/** @brief Encode a validated live state into the canonical byte layout. */
RA8_INTERNAL static void internal_encode(const ra8_ftl_t* ftl, uint8_t* buf, uint32_t need)
{
  internal_put_le32(buf, (uint32_t)k_ck_magic);
  internal_put_le16(&buf[k_ck_off_version], (uint16_t)k_ck_version);
  internal_put_le16(&buf[k_ck_off_header_bytes], (uint16_t)k_ck_header_bytes);
  internal_put_le32(&buf[k_ck_off_total_bytes], need);
  internal_put_le32(&buf[k_ck_off_logical_blocks], ftl->logical_blocks);
  internal_put_le32(&buf[k_ck_off_physical_blocks], ftl->physical_blocks);
  uint32_t offset = (uint32_t)k_ck_header_bytes;
  for (uint32_t lbn = 0U; lbn < ftl->logical_blocks; ++lbn) {
    internal_put_le16(&buf[offset], ftl->map[lbn]);
    offset += (uint32_t)k_ck_map_entry_bytes;
  }
  for (uint32_t phys = 0U; phys < ftl->physical_blocks; ++phys) {
    internal_put_le32(&buf[offset], ftl->pblocks[phys].erase_count);
    buf[offset + (uint32_t)k_ck_pblock_state_offset] = ftl->pblocks[phys].state;
    offset += (uint32_t)k_ck_pblock_entry_bytes;
  }
  internal_put_le32(&buf[offset], internal_crc32(buf, offset));
}

/** @brief Commit an already validated canonical payload to both live tables. */
RA8_INTERNAL static void internal_decode_commit(ra8_ftl_t* ftl, const uint8_t* buf)
{
  uint32_t offset = (uint32_t)k_ck_header_bytes;
  for (uint32_t lbn = 0U; lbn < ftl->logical_blocks; ++lbn) {
    ftl->map[lbn] = internal_get_le16(&buf[offset]);
    offset += (uint32_t)k_ck_map_entry_bytes;
  }
  for (uint32_t phys = 0U; phys < ftl->physical_blocks; ++phys) {
    ftl->pblocks[phys].erase_count = internal_get_le32(&buf[offset]);
    ftl->pblocks[phys].state       = buf[offset + (uint32_t)k_ck_pblock_state_offset];
    offset += (uint32_t)k_ck_pblock_entry_bytes;
  }
}

ra8_err_t ra8_ftl_checkpoint_size(const ra8_ftl_t* ftl, uint32_t* size_out)
{
  RA8_CHECK_NULL_PTR(ftl, s_tag, "ftl must not be nullptr");
  RA8_CHECK_NULL_PTR(size_out, s_tag, "size_out must not be nullptr");
  const ra8_err_t ready = internal_ready(ftl);
  if (ready != k_ra8_ok) {
    return ready;
  }
  uint32_t        size  = 0U;
  const ra8_err_t sized = internal_size_values(ftl->logical_blocks, ftl->physical_blocks, &size);
  if (sized != k_ra8_ok) {
    return sized;
  }
  *size_out = size;
  return k_ra8_ok;
}

ra8_err_t ra8_ftl_checkpoint_save(const ra8_ftl_t* ftl, uint8_t* buf, uint32_t buf_len)
{
  RA8_CHECK_NULL_PTR(ftl, s_tag, "ftl must not be nullptr");
  RA8_CHECK_NULL_PTR(buf, s_tag, "buf must not be nullptr");
  uint32_t        need  = 0U;
  const ra8_err_t sized = ra8_ftl_checkpoint_size(ftl, &need);
  if (sized != k_ra8_ok) {
    return sized;
  }
  if (buf_len < need) {
    return k_ra8_err_invalid_size;
  }
  const ra8_err_t disjoint = internal_disjoint(ftl, buf, need);
  if (disjoint != k_ra8_ok) {
    return disjoint;
  }
  const ra8_err_t valid = internal_validate_native(ftl);
  if (valid != k_ra8_ok) {
    return valid;
  }
  internal_encode(ftl, buf, need);
  return k_ra8_ok;
}

ra8_err_t ra8_ftl_checkpoint_load(ra8_ftl_t* ftl, const uint8_t* buf, uint32_t buf_len)
{
  RA8_CHECK_NULL_PTR(ftl, s_tag, "ftl must not be nullptr");
  RA8_CHECK_NULL_PTR(buf, s_tag, "buf must not be nullptr");
  uint32_t        need  = 0U;
  const ra8_err_t sized = ra8_ftl_checkpoint_size(ftl, &need);
  if (sized != k_ra8_ok) {
    return sized;
  }
  const ra8_err_t disjoint = internal_disjoint(ftl, buf, buf_len);
  if (disjoint != k_ra8_ok) {
    return disjoint;
  }
  const ra8_err_t header = internal_validate_header(ftl, buf, buf_len, need);
  if (header != k_ra8_ok) {
    return header;
  }
  const ra8_err_t valid = internal_validate_wire(ftl, buf);
  if (valid != k_ra8_ok) {
    return valid;
  }
  internal_decode_commit(ftl, buf);
  return k_ra8_ok;
}
