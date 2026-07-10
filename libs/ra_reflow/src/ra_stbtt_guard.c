/**
 * @file ra_stbtt_guard.c
 * @brief sfnt table-directory bounds validator run before stbtt_InitFont().
 *
 * @details
 * See ra_stbtt_guard.h for the threat model. This translation unit is a pure,
 * heap-free big-endian sfnt parser: it reads only the offset table and the
 * table directory, proving every access stays inside the caller-supplied
 * buffer, and returns a verdict without ever calling into stb_truetype.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * [Ring 4 / Reflow] {World: NS}
 *
 * @since 0.1.0
 */

#include "ra_stbtt_guard.h"

#include <stddef.h>
#include <stdint.h>

/**
 * @enum sfnt_layout_t
 * @brief Byte offsets and sizes of the sfnt offset table and table records.
 *
 * @details All values are fixed by the OpenType/TrueType `sfnt` container
 * format and are big-endian on disk. Named here so the parser carries no
 * bare structural literals.
 *
 * @see ra_stbtt_sfnt_dir_in_bounds
 */
typedef enum : uint8_t {
  k_sfnt_offset_table_bytes = 12U, /**< sfnt offset table (header) size, bytes.         */
  k_sfnt_num_tables_off     = 4U,  /**< `uint16` numTables offset in the offset table.  */
  k_sfnt_table_record_bytes = 16U, /**< Size of one table directory record, bytes.      */
  k_sfnt_record_offset_off  = 8U,  /**< `uint32` table-offset field offset in a record. */
  k_sfnt_record_length_off  = 12U, /**< `uint32` table-length field offset in a record. */
} sfnt_layout_t;

/**
 * @enum sfnt_shift_t
 * @brief Bit-shift distances for assembling big-endian scalars.
 *
 * @details Named to keep the byte-swap helpers free of magic shift counts.
 */
typedef enum : uint8_t {
  k_sfnt_shift_8  = 8U,  /**< One-byte shift.   */
  k_sfnt_shift_16 = 16U, /**< Two-byte shift.   */
  k_sfnt_shift_24 = 24U, /**< Three-byte shift. */
} sfnt_shift_t;

/**
 * @brief Read a big-endian `uint16` from a two-byte, in-bounds location.
 *
 * @details Assembles `p[0]` (most-significant) and `p[1]` into a host-order
 * 16-bit value via a shift-and-or. Pure computation on two bytes the caller
 * has already proven readable and in-bounds; performs no bound check itself.
 *
 * @param[in] p Pointer to the first (most-significant) byte to read.
 * @return The assembled 16-bit value.
 * @retval 0 Both source bytes were zero.
 * @pre @p p is non-null and points at two readable bytes proven in-bounds by
 *      the caller.
 * @pre The caller has range-checked `p[0]` and `p[1]` against the buffer.
 * @post No byte outside `p[0..1]` is accessed.
 * @post `*p` and `p[1]` are unmodified (pure read).
 * @note Thread-safe: no shared state.
 * @since 0.1.0
 */
static uint16_t priv_rd_be_u16(const uint8_t* p)
{
  return (uint16_t)(((uint32_t)p[0] << k_sfnt_shift_8) | (uint32_t)p[1]);
}

/**
 * @brief Read a big-endian `uint32` from a four-byte, in-bounds location.
 *
 * @details Assembles `p[0..3]` most-significant-byte-first into a host-order
 * 32-bit value via shift-and-or. Pure computation on four bytes the caller has
 * already proven readable and in-bounds; performs no bound check itself.
 *
 * @param[in] p Pointer to the first (most-significant) byte to read.
 * @return The assembled 32-bit value.
 * @retval 0 All four source bytes were zero.
 * @pre @p p is non-null and points at four readable bytes proven in-bounds by
 *      the caller.
 * @pre The caller has range-checked `p[0..3]` against the buffer.
 * @post No byte outside `p[0..3]` is accessed.
 * @post `p[0..3]` are unmodified (pure read).
 * @note Thread-safe: no shared state.
 * @since 0.1.0
 */
static uint32_t priv_rd_be_u32(const uint8_t* p)
{
  return ((uint32_t)p[0] << k_sfnt_shift_24) | ((uint32_t)p[1] << k_sfnt_shift_16) |
         ((uint32_t)p[2] << k_sfnt_shift_8) | (uint32_t)p[3];
}

bool ra_stbtt_sfnt_dir_in_bounds(const uint8_t* data, size_t len, uint32_t fontstart)
{
  if (data == nullptr) {
    return false;
  }

  const uint64_t buf_len = (uint64_t)len;
  const uint64_t start   = (uint64_t)fontstart;

  /* (1) The 12-byte offset table must lie within the buffer before we may
   * read numTables from it. Computed in uint64_t so a hostile fontstart
   * (up to UINT32_MAX) cannot wrap the sum. */
  if ((start + (uint64_t)k_sfnt_offset_table_bytes) > buf_len) {
    return false;
  }
  const uint32_t num_tables =
    priv_rd_be_u16(data + (size_t)(start + (uint64_t)k_sfnt_num_tables_off));

  /* (2) The whole table directory (num_tables records of 16 bytes) must fit.
   * num_tables is a uint16 (<= 65535), so the product is bounded. */
  const uint64_t dir_end = start + (uint64_t)k_sfnt_offset_table_bytes +
                           ((uint64_t)num_tables * (uint64_t)k_sfnt_table_record_bytes);
  if (dir_end > buf_len) {
    return false;
  }

  /* (3) Every table's declared [offset, offset + length) extent must fit.
   * Both fields are uint32, so the sum is at most 0x1FFFFFFFE and cannot
   * overflow uint64_t. The record-header reads at rec+8 / rec+12 are proven
   * in-bounds by check (2) above (rec + 16 <= dir_end <= len). */
  for (uint32_t i = 0U; i < num_tables; ++i) {
    const uint64_t rec = start + (uint64_t)k_sfnt_offset_table_bytes +
                         ((uint64_t)i * (uint64_t)k_sfnt_table_record_bytes);
    const uint64_t t_off =
      (uint64_t)priv_rd_be_u32(data + (size_t)(rec + (uint64_t)k_sfnt_record_offset_off));
    const uint64_t t_len =
      (uint64_t)priv_rd_be_u32(data + (size_t)(rec + (uint64_t)k_sfnt_record_length_off));
    if ((t_off + t_len) > buf_len) {
      return false;
    }
  }

  return true;
}
