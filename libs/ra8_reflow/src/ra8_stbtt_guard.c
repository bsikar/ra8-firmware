/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_stbtt_guard.c
 * @brief sfnt table-directory bounds validator run before stbtt_InitFont().
 *
 * @details
 * See ra8_stbtt_guard.h for the threat model. This translation unit is a pure,
 * heap-free big-endian sfnt parser: it reads only the offset table and the
 * table directory, proving every access stays inside the caller-supplied
 * buffer, and returns a verdict without ever calling into stb_truetype.
 *
 *
 * [Ring 4 / Reflow] {World: NS}
 *
 * @since 0.1.0
 */

#include "ra8_stbtt_guard.h"

#include <stddef.h>
#include <stdint.h>

#include "ra8_attributes.h"

/**
 * @enum sfnt_layout_t
 * @brief Byte offsets and sizes of the sfnt offset table and table records.
 *
 * @details All values are fixed by the OpenType/TrueType `sfnt` container
 * format and are big-endian on disk. Named here so the parser carries no
 * bare structural literals.
 *
 * @see ra8_stbtt_sfnt_dir_in_bounds
 */
typedef enum : uint8_t {
  k_sfnt_offset_table_bytes = 12U, /**< sfnt offset table (header) size, bytes.         */
  k_sfnt_num_tables_off     = 4U,  /**< `uint16` numTables offset in the offset table.  */
  k_sfnt_table_record_bytes = 16U, /**< Size of one table directory record, bytes.      */
  k_sfnt_record_tag_off     = 0U,  /**< 4-byte table tag offset in a record.            */
  k_sfnt_record_offset_off  = 8U,  /**< `uint32` table-offset field offset in a record. */
  k_sfnt_record_length_off  = 12U, /**< `uint32` table-length field offset in a record. */
} sfnt_layout_t;

/**
 * @enum sfnt_tag_t
 * @brief Big-endian sfnt table tags whose internal layout `stbtt_InitFont()`
 *        reads with an attacker-controlled count or a fixed offset.
 *
 * @details Each value is the four ASCII tag bytes assembled most-significant
 * first, i.e. exactly what ::priv_rd_be_u32 returns for the record's tag field.
 *
 * @see priv_table_internal_in_bounds
 */
typedef enum : uint32_t {
  k_sfnt_tag_cmap = 0x636D6170U, /**< 'cmap' -- character-to-glyph mapping.     */
  k_sfnt_tag_head = 0x68656164U, /**< 'head' -- font header (indexToLocFormat). */
  k_sfnt_tag_maxp = 0x6D617870U, /**< 'maxp' -- maximum profile (numGlyphs).    */
} sfnt_tag_t;

/**
 * @enum sfnt_internal_layout_t
 * @brief Byte offsets `stbtt_InitFont()` reads *inside* cmap / head / maxp.
 *
 * @details The top-level directory guard proves each table's declared
 * `[offset, offset + length)` fits, but `stbtt_InitFont_internal()` then reads
 * fields inside those tables using values it trusts from the table itself --
 * most dangerously the cmap encoding-record count, which it multiplies by 8 to
 * stride the sub-directory with no length check. These constants name the exact
 * extents those reads require.
 */
typedef enum : uint8_t {
  k_cmap_header_bytes   = 4U,  /**< cmap: version(2) + numTables(2) before records.  */
  k_cmap_num_tables_off = 2U,  /**< cmap: `uint16` numTables offset in the table.    */
  k_cmap_record_bytes   = 8U,  /**< cmap: one encoding-record size, bytes.           */
  k_head_loc_format_end = 52U, /**< head: InitFont reads `uint16` at head+50 -> +52. */
  k_maxp_num_glyphs_end = 6U,  /**< maxp: InitFont reads `uint16` at maxp+4  -> +6.  */
} sfnt_internal_layout_t;

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
RA8_INTERNAL
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
RA8_INTERNAL
static uint32_t priv_rd_be_u32(const uint8_t* p)
{
  return ((uint32_t)p[0] << k_sfnt_shift_24) | ((uint32_t)p[1] << k_sfnt_shift_16) |
         ((uint32_t)p[2] << k_sfnt_shift_8) | (uint32_t)p[3];
}

/**
 * @brief Prove the reads `stbtt_InitFont()` makes *inside* a known table stay
 *        in-bounds, given the table's already-validated file offset.
 *
 * @details
 * The top-level directory guard only proves each table's declared extent fits.
 * `stbtt_InitFont_internal()` then reads fields inside three tables using values
 * it trusts from the table's own bytes -- none of which the declared length
 * constrains:
 *  - cmap: `numTables = ttUSHORT(cmap + 2)`, then strides the encoding-record
 *    sub-directory as `cmap + 4 + 8 * i` for `i` in `[0, numTables)`. A crafted
 *    numTables (up to 65535) walks ~512 KiB past the table -- the reliable
 *    out-of-bounds read this validator closes. Requires
 *    `t_off + 4 + 8 * numTables <= len`.
 *  - head: `ttUSHORT(head + 50)` (indexToLocFormat). Requires `t_off + 52 <= len`.
 *  - maxp: `ttUSHORT(maxp + 4)` (numGlyphs). Requires `t_off + 6 <= len`.
 * Any other tag needs no internal check here (its contents are read later, in
 * the glyph path, not by `stbtt_InitFont`).
 *
 * @param[in] data    First byte of the font buffer (caller-proven non-null).
 * @param[in] buf_len Buffer length in bytes.
 * @param[in] tag     Big-endian 4-byte table tag (see ::sfnt_tag_t).
 * @param[in] t_off   Table file offset, already proven `< buf_len` by the
 *                    per-record extent check in the caller.
 *
 * @return `true` if every InitFont read inside this table lies within the
 *         buffer (or the tag has no InitFont-time internal read).
 * @retval true  Table-internal reads are in-bounds, or tag is not cmap/head/maxp.
 * @retval false A cmap/head/maxp internal read would escape `[data, data+len)`.
 *
 * @pre @p data references at least @p buf_len readable bytes.
 * @pre @p t_off is `< buf_len` (the record's declared offset fits).
 * @post @p data and its buffer are unmodified (pure read).
 * @post No byte outside `[data, data + buf_len)` is accessed.
 *
 * @note Thread-safe and re-entrant: no shared or static state.
 * @since 0.1.0
 */
RA8_INTERNAL
static bool
priv_table_internal_in_bounds(const uint8_t* data, uint64_t buf_len, uint32_t tag, uint64_t t_off)
{
  if (tag == (uint32_t)k_sfnt_tag_cmap) {
    if ((t_off + (uint64_t)k_cmap_header_bytes) > buf_len) {
      return false;
    }
    const uint32_t sub_tables =
      priv_rd_be_u16(data + (size_t)(t_off + (uint64_t)k_cmap_num_tables_off));
    const uint64_t sub_end = t_off + (uint64_t)k_cmap_header_bytes +
                             ((uint64_t)sub_tables * (uint64_t)k_cmap_record_bytes);
    return sub_end <= buf_len;
  }
  if (tag == (uint32_t)k_sfnt_tag_head) {
    return (t_off + (uint64_t)k_head_loc_format_end) <= buf_len;
  }
  if (tag == (uint32_t)k_sfnt_tag_maxp) {
    return (t_off + (uint64_t)k_maxp_num_glyphs_end) <= buf_len;
  }
  return true;
}

bool ra8_stbtt_sfnt_dir_in_bounds(const uint8_t* data, size_t len, uint32_t fontstart)
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

  /* (3) Every table's declared [offset, offset + length) extent must fit, and
   * (4) the reads stbtt_InitFont makes INSIDE cmap / head / maxp must also stay
   * in-bounds -- the top-level extent alone does not bound cmap's internal
   * 8 * numTables sub-directory walk. Both record fields are uint32, so the
   * sum is at most 0x1FFFFFFFE and cannot overflow uint64_t. The record-header
   * reads at rec+0 / rec+8 / rec+12 are proven in-bounds by check (2) above
   * (rec + 16 <= dir_end <= len). */
  for (uint32_t i = 0U; i < num_tables; ++i) {
    const uint64_t rec = start + (uint64_t)k_sfnt_offset_table_bytes +
                         ((uint64_t)i * (uint64_t)k_sfnt_table_record_bytes);
    const uint32_t tag = priv_rd_be_u32(data + (size_t)(rec + (uint64_t)k_sfnt_record_tag_off));
    const uint64_t t_off =
      (uint64_t)priv_rd_be_u32(data + (size_t)(rec + (uint64_t)k_sfnt_record_offset_off));
    const uint64_t t_len =
      (uint64_t)priv_rd_be_u32(data + (size_t)(rec + (uint64_t)k_sfnt_record_length_off));
    if ((t_off + t_len) > buf_len) {
      return false;
    }
    if (!priv_table_internal_in_bounds(data, buf_len, tag, t_off)) {
      return false;
    }
  }

  return true;
}
