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

/**
 * @brief Decode one canonical little-endian 16-bit field.
 * @details Combines two octets explicitly without alignment or host-endian assumptions.
 * @param[in] in Readable two-byte wire field.
 * @return Host-order unsigned value.
 * @retval UINT16_C(0) Both source bytes are zero.
 * @retval UINT16_MAX Both source bytes are 0xff.
 * @pre @p in addresses at least two readable bytes.
 * @pre The source remains stable during both byte reads.
 * @post No memory is modified.
 * @post The result is the exact canonical little-endian decoding.
 * @note Pure and thread-safe for immutable input.
 * @since Version 0.1.0
 */
RA8_INTERNAL static uint16_t internal_get_le16(const uint8_t* in)
{
  return (uint16_t)((uint16_t)in[0] | ((uint16_t)in[1] << 8U));
}

/**
 * @brief Decode one canonical little-endian 32-bit field.
 * @details Combines four octets explicitly without alignment or host-endian assumptions.
 * @param[in] in Readable four-byte wire field.
 * @return Host-order unsigned value.
 * @retval UINT32_C(0) All source bytes are zero.
 * @retval UINT32_MAX All source bytes are 0xff.
 * @pre @p in addresses at least four readable bytes.
 * @pre The source remains stable during all byte reads.
 * @post No memory is modified.
 * @post The result is the exact canonical little-endian decoding.
 * @note Pure and thread-safe for immutable input.
 * @since Version 0.1.0
 */
RA8_INTERNAL static uint32_t internal_get_le32(const uint8_t* in)
{
  return (uint32_t)in[0] | ((uint32_t)in[1] << 8U) | ((uint32_t)in[2] << 16U) |
         ((uint32_t)in[3] << (uint32_t)k_ck_byte_3_shift);
}

/**
 * @brief Encode one canonical little-endian 16-bit field.
 * @details Stores the low octet first to make the checkpoint host independent.
 * @param[out] out Writable two-byte wire field.
 * @param[in] value Host-order value to encode.
 * @pre @p out addresses at least two writable bytes.
 * @pre The destination is not concurrently accessed.
 * @post Exactly two destination bytes contain @p value in little-endian order.
 * @post Bytes outside the two-byte field are unchanged.
 * @note Thread-safe for disjoint caller-owned destinations.
 * @since Version 0.1.0
 */
RA8_INTERNAL static void internal_put_le16(uint8_t* out, uint16_t value)
{
  out[0] = (uint8_t)value;
  out[1] = (uint8_t)(value >> 8U);
}

/**
 * @brief Encode one canonical little-endian 32-bit field.
 * @details Stores four low-to-high octets to make the checkpoint host independent.
 * @param[out] out Writable four-byte wire field.
 * @param[in] value Host-order value to encode.
 * @pre @p out addresses at least four writable bytes.
 * @pre The destination is not concurrently accessed.
 * @post Exactly four destination bytes contain @p value in little-endian order.
 * @post Bytes outside the four-byte field are unchanged.
 * @note Thread-safe for disjoint caller-owned destinations.
 * @since Version 0.1.0
 */
RA8_INTERNAL static void internal_put_le32(uint8_t* out, uint32_t value)
{
  out[0] = (uint8_t)value;
  out[1] = (uint8_t)(value >> 8U);
  out[2] = (uint8_t)(value >> 16U);
  out[3] = (uint8_t)(value >> (uint32_t)k_ck_byte_3_shift);
}

/**
 * @brief Compute CRC-32/ISO-HDLC over a bounded byte span.
 * @details Applies the reflected polynomial bit-by-bit from the standard seed
 *          and final complement, avoiding any mutable lookup table.
 * @param[in] data Readable bytes to hash.
 * @param[in] length Number of bytes in @p data.
 * @return Finalized CRC-32 value.
 * @retval UINT32_C(0) The span's finalized checksum is zero.
 * @retval UINT32_MAX The span's finalized checksum has every bit set.
 * @pre @p data addresses @p length readable bytes when non-zero.
 * @pre The source remains stable throughout the bounded scan.
 * @post No source bytes or external state are modified.
 * @post Each source byte contributes exactly once in increasing address order.
 * @note Pure and thread-safe for immutable input.
 * @since Version 0.1.0
 */
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

/**
 * @brief Fail closed when two byte ranges overlap or their endpoints wrap.
 * @details Uses inclusive integer-address endpoints after checking subtraction
 *          and addition bounds; an unrepresentable non-empty span is unsafe.
 * @param[in] first Base of the first candidate span.
 * @param[in] first_bytes Length of the first span in bytes.
 * @param[in] second Base of the second candidate span.
 * @param[in] second_bytes Length of the second span in bytes.
 * @return Whether the spans overlap or cannot be represented safely.
 * @retval true The spans overlap or at least one endpoint would wrap.
 * @retval false One span is empty or both non-empty spans are disjoint.
 * @pre Non-zero lengths truthfully describe accessible object spans.
 * @pre Pointer-to-integer conversion preserves address ordering on the target.
 * @post No memory or external state is modified.
 * @post A false result proves both non-empty endpoints were representable.
 * @note Pure and thread-safe.
 * @since Version 0.1.0
 */
RA8_INTERNAL static bool internal_ranges_overlap(const void* first,
                                                 uint32_t    first_bytes,
                                                 const void* second,
                                                 uint32_t    second_bytes)
{
  const uintptr_t first_start  = (uintptr_t)first;
  const uintptr_t second_start = (uintptr_t)second;
  if (first_bytes == 0U) {
    return false;
  }
  if (second_bytes == 0U) {
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
  if (first_start > second_end) {
    return false;
  }
  return second_start <= first_end;
}

/**
 * @brief Validate that a checkpoint-capable FTL handle is fully bound.
 * @details Requires the raw block device, logical map, physical-state table,
 *          and fixed scratch workspace installed by FTL initialization.
 * @param[in] ftl FTL handle to inspect.
 * @return Readiness status.
 * @retval k_ra8_ok Every checkpoint dependency is bound.
 * @retval k_ra8_err_not_initialized At least one required pointer is NULL.
 * @pre @p ftl addresses a readable FTL descriptor.
 * @pre The descriptor is not concurrently initialized or deinitialized.
 * @post No FTL or backing storage state is modified.
 * @post Success establishes non-NULL storage for later validation passes.
 * @note Thread-safe only with external FTL lifecycle synchronization.
 * @since Version 0.1.0
 */
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

/**
 * @brief Calculate the exact wire length with checked 32-bit arithmetic.
 * @details Adds fixed bytes, logical-map entries, and physical-state records
 *          only after proving both multiplications and sums fit in uint32_t.
 * @param[in] logical_blocks Logical block count to encode.
 * @param[in] physical_blocks Physical block count to encode.
 * @param[out] out Receives the exact checkpoint length.
 * @return Geometry sizing status.
 * @retval k_ra8_ok The geometry is valid and @p out was written.
 * @retval k_ra8_err_invalid_size Geometry is invalid or the length overflows.
 * @pre @p out addresses a writable uint32_t object.
 * @pre Counts are candidate FTL geometry expressed in blocks.
 * @post Success stores the exact canonical wire length in @p out.
 * @post Failure leaves @p out unchanged.
 * @note Pure apart from caller output and thread-safe for disjoint output.
 * @since Version 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_size_values(uint32_t logical_blocks, uint32_t physical_blocks, uint32_t* out)
{
  if (logical_blocks == 0U) {
    return k_ra8_err_invalid_size;
  }
  if (physical_blocks > (uint32_t)k_ra8_ftl_max_pblocks) {
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

/**
 * @brief Validate workspace and checkpoint-buffer non-aliasing.
 * @details Proves scratch, live map, live physical table, and the candidate
 *          checkpoint span are disjoint before transactional encode or decode.
 * @param[in] ftl Initialized FTL descriptor and caller-owned spans.
 * @param[in] buffer Candidate checkpoint source or destination.
 * @param[in] buffer_bytes Bytes the operation will access in @p buffer.
 * @return Alias validation status.
 * @retval k_ra8_ok Every accessed span is disjoint.
 * @retval k_ra8_err_invalid_state Internal FTL workspaces overlap.
 * @retval k_ra8_err_invalid_arg The checkpoint buffer aliases FTL state.
 * @pre FTL geometry has been validated and sizes both live tables.
 * @pre @p buffer addresses @p buffer_bytes accessible bytes.
 * @post No buffer or FTL state is modified.
 * @post Success permits scratch use without corrupting input or live tables.
 * @note Pure and thread-safe for immutable descriptors.
 * @since Version 0.1.0
 */
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

/**
 * @brief Set one scratch-bitmap bit and report whether it was already set.
 * @details Derives the byte and mask from the physical-relative bit index,
 *          samples prior ownership, and then records ownership unconditionally.
 * @param[in,out] bitmap Caller-owned packed ownership bitmap.
 * @param[in] bit Zero-based bit index within the bitmap.
 * @return Prior state of the selected bit.
 * @retval true The bit was already set, indicating duplicate ownership.
 * @retval false The bit was clear before this call.
 * @pre @p bitmap contains at least `bit / 8 + 1` writable bytes.
 * @pre @p bit is relative to the current bounded scratch window.
 * @post The selected bit is set.
 * @post Every other bitmap bit is unchanged.
 * @note Not thread-safe for concurrent access to the same bitmap byte.
 * @since Version 0.1.0
 */
RA8_INTERNAL static bool internal_bit_was_set(uint8_t* bitmap, uint32_t bit)
{
  const uint32_t byte = bit / (uint32_t)k_ck_bits_per_byte;
  const uint8_t  mask = (uint8_t)(1U << (bit % (uint32_t)k_ck_bits_per_byte));
  const bool     set  = (bitmap[byte] & mask) != 0U;
  bitmap[byte] |= mask;
  return set;
}

/**
 * @brief Read one bit from the bounded scratch bitmap.
 * @details Derives a byte and mask from the physical-relative bit index.
 * @param[in] bitmap Caller-owned packed ownership bitmap.
 * @param[in] bit Zero-based bit index within the bitmap.
 * @return State of the selected bit.
 * @retval true The selected bit is set.
 * @retval false The selected bit is clear.
 * @pre @p bitmap contains at least `bit / 8 + 1` readable bytes.
 * @pre @p bit is relative to the current bounded scratch window.
 * @post No bitmap or external state is modified.
 * @post The result depends only on the selected input bit.
 * @note Pure and thread-safe for immutable bitmap storage.
 * @since Version 0.1.0
 */
RA8_INTERNAL static bool internal_bit_is_set(const uint8_t* bitmap, uint32_t bit)
{
  const uint32_t byte = bit / (uint32_t)k_ck_bits_per_byte;
  const uint8_t  mask = (uint8_t)(1U << (bit % (uint32_t)k_ck_bits_per_byte));
  return (bitmap[byte] & mask) != 0U;
}

/**
 * @brief Mark one mapped physical index against a bounded scratch window.
 * @details Rejects an index outside the physical geometry outright; an index
 *          outside the current window is left for a later window's pass and
 *          reported as legal here. An index inside the window is rejected
 *          only if some earlier map entry already claimed it.
 * @param[in] ftl Initialized FTL geometry and scratch workspace.
 * @param[in] base First physical-block index in the window.
 * @param[in] count Number of physical blocks represented by scratch.
 * @param[in] phys Physical block index read from one map entry.
 * @return Map-entry validation status.
 * @retval k_ra8_ok The index is legal; scratch gained a mark when in-window.
 * @retval k_ra8_err_invalid_state The index is out of range or a duplicate.
 * @pre @p phys was read from a live or canonical wire map entry.
 * @pre ftl->scratch addresses the same window as every other call this pass.
 * @post Scratch gains one marked bit when @p phys falls in the window.
 * @post No FTL geometry, map, or media state outside the scratch bitmap is modified.
 * @note Not thread-safe; shares the caller's scratch workspace.
 * @since Version 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_window_mark(const ra8_ftl_t* ftl, uint32_t base, uint32_t count, uint16_t phys)
{
  if ((uint32_t)phys >= ftl->physical_blocks) {
    return k_ra8_err_invalid_state;
  }
  if ((uint32_t)phys < base) {
    return k_ra8_ok;
  }
  if ((uint32_t)phys - base >= count) {
    return k_ra8_ok;
  }
  if (internal_bit_was_set(ftl->scratch, (uint32_t)phys - base)) {
    return k_ra8_err_invalid_state;
  }
  return k_ra8_ok;
}

/**
 * @brief Validate one window of the live native mapping into scratch.
 * @details Marks mapped physical blocks within the window, rejects duplicates
 *          and out-of-range indices, then cross-checks every physical state.
 * @param[in] ftl Initialized live FTL state.
 * @param[in] base First physical-block index in the window.
 * @param[in] count Number of physical blocks represented by scratch.
 * @return Live-window validation status.
 * @retval k_ra8_ok Map ownership and physical states agree in the window.
 * @retval k_ra8_err_invalid_state A map index, duplicate, or state is invalid.
 * @pre @p count is non-zero and at most ::k_ck_bitmap_blocks.
 * @pre `base + count` does not exceed ftl->physical_blocks.
 * @post Scratch contains the final ownership bitmap for this window.
 * @post Live map and physical-state tables are unchanged.
 * @note Not thread-safe; overwrites the shared FTL scratch workspace.
 * @since Version 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_native_window(const ra8_ftl_t* ftl, uint32_t base, uint32_t count)
{
  (void)memset(ftl->scratch, 0, (size_t)k_ck_scratch_bytes);
  for (uint32_t lbn = 0U; lbn < ftl->logical_blocks; ++lbn) {
    const uint16_t phys = ftl->map[lbn];
    if (phys == (uint16_t)k_ra8_ftl_unmapped) {
      continue;
    }
    const ra8_err_t marked = internal_window_mark(ftl, base, count, phys);
    if (marked != k_ra8_ok) {
      return marked;
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

/**
 * @brief Validate every live map/state invariant using bounded scratch windows.
 * @details Partitions physical geometry into fixed 4096-block bitmap windows,
 *          allowing duplicate and live-state checks with exactly 512 bytes.
 * @param[in] ftl Initialized live FTL state.
 * @return Complete live-state validation status.
 * @retval k_ra8_ok Every physical window satisfies the mapping invariants.
 * @retval k_ra8_err_invalid_state A window contains invalid ownership or state.
 * @pre FTL geometry and all live-table capacities are valid.
 * @pre ftl->scratch addresses at least ::k_ck_scratch_bytes writable bytes.
 * @post Live map and physical-state tables are unchanged.
 * @post Scratch contains data from the final validated or failing window.
 * @note Not thread-safe; serial access to FTL scratch is required.
 * @since Version 0.1.0
 */
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

/**
 * @brief Validate one canonical wire-map window into caller scratch.
 * @details Decodes map entries and physical-state bytes directly from canonical
 *          wire storage, marking only the requested physical window in scratch.
 * @param[in] ftl Initialized FTL geometry and scratch workspace.
 * @param[in] buf Validated canonical checkpoint bytes.
 * @param[in] map_offset Byte offset of the logical map.
 * @param[in] pb_offset Byte offset of the physical-state table.
 * @param[in] base First physical-block index in the window.
 * @param[in] count Physical blocks represented by the window.
 * @return Wire-window validation status.
 * @retval k_ra8_ok Wire ownership and state agree in the window.
 * @retval k_ra8_err_invalid_state An index, duplicate, or state is invalid.
 * @pre Header validation proved all derived wire offsets are in range.
 * @pre @p count is non-zero, bounded by scratch, and within geometry.
 * @post Scratch contains the wire ownership bitmap for this window.
 * @post Checkpoint bytes and live FTL tables are unchanged.
 * @note Not thread-safe; overwrites shared FTL scratch.
 * @since Version 0.1.0
 */
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
    const ra8_err_t marked = internal_window_mark(ftl, base, count, phys);
    if (marked != k_ra8_ok) {
      return marked;
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

/**
 * @brief Validate every canonical payload invariant without live mutation.
 * @details Walks the full physical geometry through bounded scratch windows,
 *          proving map uniqueness and exact live-state correspondence.
 * @param[in] ftl Initialized FTL geometry and scratch workspace.
 * @param[in] buf Header-validated canonical checkpoint bytes.
 * @return Complete wire-payload validation status.
 * @retval k_ra8_ok Every map entry and physical state is canonical.
 * @retval k_ra8_err_invalid_state A payload invariant fails.
 * @pre Header and exact wire length were validated for this FTL geometry.
 * @pre @p buf remains readable throughout all bounded passes.
 * @post Live map and physical-state tables are unchanged.
 * @post Scratch contains data from the final validated or failing window.
 * @note Not thread-safe; serial access to FTL scratch is required.
 * @since Version 0.1.0
 */
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

/**
 * @brief Validate the canonical header, exact length, geometry, and CRC.
 * @details Rejects legacy byte orders, unknown version/header geometry, length
 *          mismatch, different FTL geometry, and any trailer checksum mismatch.
 * @param[in] ftl Initialized destination FTL geometry.
 * @param[in] buf Candidate checkpoint bytes.
 * @param[in] buf_len Accessible candidate length in bytes.
 * @param[in] need Exact length computed for @p ftl.
 * @return Header and integrity validation status.
 * @retval k_ra8_ok Header, geometry, length, and CRC are valid.
 * @retval k_ra8_err_not_supported The checkpoint uses a legacy or unknown version.
 * @retval k_ra8_err_invalid_state Magic is not the canonical format magic.
 * @retval k_ra8_err_invalid_size Header or total length is inconsistent.
 * @retval k_ra8_err_invalid_arg Stored geometry differs from @p ftl.
 * @retval k_ra8_err_crc_mismatch Payload integrity verification failed.
 * @pre @p buf addresses @p buf_len readable bytes.
 * @pre @p need was computed by ::internal_size_values for @p ftl.
 * @post Neither checkpoint bytes nor live FTL state are modified.
 * @post Success permits semantic payload validation at the derived offsets.
 * @note Pure and thread-safe for immutable inputs.
 * @since Version 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_validate_header(const ra8_ftl_t* ftl, const uint8_t* buf, uint32_t buf_len, uint32_t need)
{
  if (buf_len < (uint32_t)k_ck_crc_bytes) {
    return k_ra8_err_invalid_size;
  }
  const uint32_t magic = internal_get_le32(buf);
  if (magic == (uint32_t)k_ck_legacy_magic_le) {
    return k_ra8_err_not_supported;
  }
  if (magic == (uint32_t)k_ck_legacy_magic_swapped) {
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
  if (internal_get_le32(&buf[k_ck_off_total_bytes]) != buf_len) {
    return k_ra8_err_invalid_size;
  }
  if (buf_len != need) {
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

/**
 * @brief Encode a validated live state into the canonical byte layout.
 * @details Writes the fixed header, logical map, physical records, and final
 *          CRC explicitly in little-endian order into disjoint caller storage.
 * @param[in] ftl Fully validated live FTL state.
 * @param[out] buf Disjoint checkpoint destination.
 * @param[in] need Exact destination and wire length in bytes.
 * @pre @p buf addresses at least @p need writable bytes.
 * @pre Live map/state validation succeeded and all spans are disjoint.
 * @post Exactly @p need bytes contain one canonical checkpoint.
 * @post Live FTL map and physical-state tables are unchanged.
 * @note Not thread-safe with concurrent mutation of the live FTL state.
 * @since Version 0.1.0
 */
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

/**
 * @brief Commit an already validated canonical payload to both live tables.
 * @details Decodes all logical-map and physical-state records only after every
 *          structural and semantic validation pass has succeeded.
 * @param[in,out] ftl Destination FTL whose live tables receive the checkpoint.
 * @param[in] buf Fully validated, disjoint canonical checkpoint bytes.
 * @pre Wire header, CRC, mapping, state, and non-alias checks all succeeded.
 * @pre Live table capacities match the geometry stored in @p ftl.
 * @post Every live map and physical-state entry matches @p buf.
 * @post Checkpoint bytes and unrelated FTL descriptor fields are unchanged.
 * @note Not thread-safe; callers must exclude concurrent FTL operations.
 * @since Version 0.1.0
 */
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
