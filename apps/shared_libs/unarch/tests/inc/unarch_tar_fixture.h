/**
 * @file unarch_tar_fixture.h
 * @brief In-memory ustar fixture construction support for the TAR unit test.
 *
 * @details
 * Owns the test-only POSIX field geometry, fixed scratch buffers, and bounded
 * header/member builders used by `test_unarch_tar.c`. Keeping fixture
 * construction separate from behavioral vectors makes the archive encoding
 * responsibility reviewable without weakening the production API boundary.
 * Every helper has internal linkage in the one including test translation
 * unit; this header is not a production or shared-test interface.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since Version 0.1.0
 */
#pragma once

/**
 * @enum t_tar_num_t
 * @brief Leading bytes that select a tar numeric field's encoding.
 *
 * @details
 * A tar numeric field is normally space- or NUL-terminated octal. GNU extends
 * it with a base-256 form flagged by bit 7 of byte 0: 0x80 for a positive
 * value, 0xC0 for a negative one. The parser must accept the first and reject
 * the rest.
 */
typedef enum : uint8_t {
  k_t_b256_positive = 0x80U, /**< GNU base-256 marker, positive value. */
  k_t_b256_negative = 0xC0U, /**< Base-256 marker with the sign bit set; a
                                  negative size is not representable.          */
  k_t_b256_payload  = 0x81U, /**< Marker with a payload bit already in byte 0,
                                  making the value exceed 64 bits.             */
  k_t_corrupt_mask  = 0xFFU, /**< XOR mask that flips a byte to break the
                                  header checksum, then restores it.           */
} t_tar_num_t;

/**
 * @enum t_tar_field_t
 * @brief Offsets and widths the numeric-field arms index with.
 */
typedef enum : uint16_t {
  k_t_num_overlong = 24U,  /**< Numeric field twice the legal width.           */
  k_t_b256_hi_byte = 10U,  /**< High payload byte of a 12-byte base-256 field. */
  k_t_b256_lo_byte = 11U,  /**< Its low payload byte.                          */
  k_t_nonzero_off  = 300U, /**< An offset inside a block, made non-zero to
                                defeat the all-zero end-of-archive test.       */
} t_tar_field_t;

/**
 * @enum t_tar_fixture_t
 * @brief Sizes of the crafted archives and their members.
 */
typedef enum : uint32_t {
  k_t_pax_flood    = 5U,      /**< Pax blocks emitted past the per-member cap. */
  k_t_pax_oversize = 4096U,   /**< Pax payload past k_unarch_tar_pax_max.      */
  k_t_member_len   = 600U,    /**< Data length of the truncation-arm member.   */
  k_t_truncate_by  = 700U,    /**< Bytes cut off the archive: past the block
                                   padding and into the 600-byte data area.    */
  k_t_size_lie     = 100000U, /**< Declared member size far past the archive. */
} t_tar_fixture_t;

/**
 * @enum unarch_tar_tb_octal_0644_t
 * @brief Named octal mode bits used by this file.
 */
typedef enum : uint16_t {
  k_unarch_tar_tb_octal_0644 = 0644U, /**< tar member mode: rw-r--r--. */
} unarch_tar_tb_octal_0644_t;

/**
 * @enum tt_tar_layout_t
 * @brief POSIX ustar header field offsets and widths, within one 512-byte block.
 *
 * @details
 * Offsets and widths are fixed by the ustar format, so they are named by the
 * FIELD they address rather than by their value. Two pairs share a value but
 * not a role and must not be confused: 100 is both the name width and the mode
 * offset, and 155 is both the prefix width and the offset of the checksum
 * field's trailing space.
 *
 * @see internal_tb_header  Builds a header block using these offsets.
 * @see internal_tb_finish  Computes the checksum over the completed block.
 */
typedef enum : uint16_t {
  k_tt_tar_block        = 512U,  /**< Tar block size, bytes.                  */
  k_tt_end_marker_bytes = 1024U, /**< End-of-archive marker: two zero blocks. */
  k_tt_off_name         = 0U,    /**< name field offset.                      */
  k_tt_len_name         = 100U,  /**< name field width.                       */
  k_tt_off_mode         = 100U,  /**< mode field offset.                      */
  k_tt_off_uid          = 108U,  /**< uid field offset.                       */
  k_tt_off_gid          = 116U,  /**< gid field offset.                       */
  k_tt_len_id           = 8U,    /**< mode/uid/gid field width.               */
  k_tt_off_size         = 124U,  /**< size field offset.                      */
  k_tt_len_size         = 12U,   /**< size field width.                       */
  k_tt_off_mtime        = 136U,  /**< mtime field offset.                     */
  k_tt_off_chksum       = 148U,  /**< checksum field offset.                  */
  k_tt_len_chksum       = 8U,    /**< checksum field width.                   */
  k_tt_chksum_digits    = 7U,    /**< Octal digits written into the checksum. */
  k_tt_off_chksum_pad   = 155U,  /**< Trailing space of the checksum field.   */
  k_tt_off_type         = 156U,  /**< typeflag field offset.                  */
  k_tt_off_magic        = 257U,  /**< "ustar" magic offset.                   */
  k_tt_off_magic_nul    = 262U,  /**< Magic terminator byte offset.           */
  k_tt_off_version      = 263U,  /**< version field offset.                   */
  k_tt_off_prefix       = 345U,  /**< prefix field offset.                    */
  k_tt_len_prefix       = 155U,  /**< prefix field width.                     */
} tt_tar_layout_t;

/**
 * @enum tt_dim_t
 * @brief Archive / buffer budgets for the in-memory fixtures.
 */
typedef enum : uint32_t {
  k_tt_arc_cap  = 64U * 1024U, /**< In-memory archive build buffer. */
  k_tt_name_cap = 128U,        /**< Per-query name buffer.          */
  k_tt_out_cap  = 4096U,       /**< Extraction buffer.              */
} tt_dim_t;

/** @brief The archive under test. */
static uint8_t s_arc[k_tt_arc_cap];
/** @brief Length of the built archive. */
static size_t s_arc_len = 0U;
/** @brief Extraction buffer. */
static uint8_t s_out[k_tt_out_cap];
/** @brief Name buffer for the walk. */
static char s_name[k_tt_name_cap];

/** @brief Write a NUL-terminated octal numeric field.
 *
 * @details Writes a fixed-width, NUL-terminated octal field from least-significant digit to most-significant digit.
 * @param[out] f Tar numeric field to overwrite.
 * @param[in] len Field width, including terminator space.
 * @param[in] v Value to encode in octal.
 * @pre All input spans and fixture capacities satisfy this helper contract.
 * @pre The caller initialized state consumed or advanced by this helper.
 * @post All writes remain within caller-owned fixture storage.
 * @post No heap allocation or external I/O is performed.
 * @note Test-only and not reentrant because fixtures use file-scope scratch storage.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static void internal_tb_octal(uint8_t* f, size_t len, uint64_t v)
{
  (void)memset(f, 0, len);
  uint64_t remaining = v;
  size_t   i         = len - 2U; /* last byte stays NUL */
  f[i + 1U]          = 0U;
  do {
    f[i] = (uint8_t)('0' + (remaining % 8U));
    remaining /= 8U;
    if (i == 0U) {
      break;
    }
    i -= 1U;
  } while (true);
}

/** @brief Compute and store the header checksum ("%06o\0 " form).
 *
 * @details Replaces the checksum field with spaces, sums the complete header, and stores the canonical checksum form.
 * @param[in,out] block Complete header whose checksum field is updated.
 * @pre All input spans and fixture capacities satisfy this helper contract.
 * @pre The caller initialized state consumed or advanced by this helper.
 * @post All writes remain within caller-owned fixture storage.
 * @post No heap allocation or external I/O is performed.
 * @note Test-only and not reentrant because fixtures use file-scope scratch storage.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static void internal_tb_finish(uint8_t* block)
{
  (void)memset(&block[k_tt_off_chksum], (int)' ', k_tt_len_chksum);
  uint32_t sum = 0U;
  for (uint32_t i = 0U; i < (uint32_t)k_tt_tar_block; ++i) {
    sum += block[i];
  }
  internal_tb_octal(&block[k_tt_off_chksum], k_tt_chksum_digits, (uint64_t)sum);
  block[k_tt_off_chksum_pad] = (uint8_t)' ';
}

/** @brief Build one header block (ustar magic, octal size, checksum).
 *
 * @details Constructs one deterministic ustar header with bounded name/prefix copies and a valid checksum.
 * @param[out] block Writable 512-byte header block.
 * @param[in] name NUL-terminated member name.
 * @param[in] prefix Optional NUL-terminated ustar prefix.
 * @param[in] type Tar typeflag byte.
 * @param[in] dsize Declared payload length.
 * @pre All input spans and fixture capacities satisfy this helper contract.
 * @pre The caller initialized state consumed or advanced by this helper.
 * @post All writes remain within caller-owned fixture storage.
 * @post No heap allocation or external I/O is performed.
 * @note Test-only and not reentrant because fixtures use file-scope scratch storage.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static void internal_tb_header(uint8_t*    block,
                               const char* name,
                               const char* prefix,
                               uint8_t     type,
                               uint64_t    dsize)
{
  static const uint8_t ustar_magic[]   = {'u', 's', 't', 'a', 'r'};
  static const uint8_t ustar_version[] = {'0', '0'};
  (void)memset(block, 0, (size_t)k_tt_tar_block);
  (void)strncpy((char*)&block[k_tt_off_name], name, (size_t)k_tt_len_name);
  internal_tb_octal(&block[k_tt_off_mode], k_tt_len_id, k_unarch_tar_tb_octal_0644); /* mode  */
  internal_tb_octal(&block[k_tt_off_uid], k_tt_len_id, 0U);                          /* uid   */
  internal_tb_octal(&block[k_tt_off_gid], k_tt_len_id, 0U);                          /* gid   */
  internal_tb_octal(&block[k_tt_off_size], k_tt_len_size, dsize);                    /* size  */
  internal_tb_octal(&block[k_tt_off_mtime], k_tt_len_size, 0U);                      /* mtime */
  block[k_tt_off_type] = type;
  (void)memcpy(&block[k_tt_off_magic], ustar_magic, sizeof(ustar_magic));
  block[k_tt_off_magic_nul] = 0U;
  (void)memcpy(&block[k_tt_off_version], ustar_version, sizeof(ustar_version));
  if (prefix != nullptr) {
    (void)strncpy((char*)&block[k_tt_off_prefix], prefix, (size_t)k_tt_len_prefix);
  }
  internal_tb_finish(block);
}

/** @brief Append one member (header + padded data); returns the new offset.
 *
 * @details Appends a header and block-padded payload to the fixed archive fixture without allocating.
 * @param[in] off Current archive end offset.
 * @param[in] name Member name.
 * @param[in] prefix Optional ustar prefix.
 * @param[in] type Member typeflag.
 * @param[in] data Payload bytes; NULL only when size is zero.
 * @param[in] dsize Payload length.
 * @return New archive end offset.
 * @retval >0 The header and padded data were appended.
 * @pre All input spans and fixture capacities satisfy this helper contract.
 * @pre The caller initialized state consumed or advanced by this helper.
 * @post All writes remain within caller-owned fixture storage.
 * @post No heap allocation or external I/O is performed.
 * @note Test-only and not reentrant because fixtures use file-scope scratch storage.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static size_t internal_tb_add(size_t      off,
                              const char* name,
                              const char* prefix,
                              uint8_t     type,
                              const void* data,
                              size_t      dsize)
{
  size_t cursor = off;
  internal_tb_header(&s_arc[cursor], name, prefix, type, (uint64_t)dsize);
  cursor += k_tt_tar_block;
  if (dsize > 0U) {
    (void)memcpy(&s_arc[cursor], data, dsize);
    const size_t padded = ((dsize + 511U) / 512U) * 512U;
    (void)memset(&s_arc[cursor + dsize], 0, padded - dsize);
    cursor += padded;
  }
  return cursor;
}

/** @brief Append the end-of-archive marker (two zero blocks).
 *
 * @details Appends the two all-zero blocks that terminate the in-memory archive.
 * @param[in] off Current archive end offset.
 * @return New archive end offset.
 * @retval >0 Two zero blocks were appended.
 * @pre All input spans and fixture capacities satisfy this helper contract.
 * @pre The caller initialized state consumed or advanced by this helper.
 * @post All writes remain within caller-owned fixture storage.
 * @post No heap allocation or external I/O is performed.
 * @note Test-only and not reentrant because fixtures use file-scope scratch storage.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static size_t internal_tb_end(size_t off)
{
  (void)memset(&s_arc[off], 0, (size_t)k_tt_end_marker_bytes);
  return off + (size_t)k_tt_end_marker_bytes;
}

/** @brief Open a walker over the built archive with optional limits.
 *
 * @details Binds the current archive fixture to a memory reader and opens a production TAR walker.
 * @param[out] t Walker initialized on success.
 * @param[out] mem Memory source bound to the fixture.
 * @param[in] lim Optional policy override.
 * @return TAR open status.
 * @retval k_ra8_ok The walker was bound to the complete fixture.
 * @pre All input spans and fixture capacities satisfy this helper contract.
 * @pre The caller initialized state consumed or advanced by this helper.
 * @post All writes remain within caller-owned fixture storage.
 * @post No heap allocation or external I/O is performed.
 * @note Test-only and not reentrant because fixtures use file-scope scratch storage.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static ra8_err_t
internal_tt_open(unarch_tar_t* t, unarch_mem_t* mem, const ra8_decomp_limits_t* lim)
{
  mem->base = s_arc;
  mem->len  = s_arc_len;
  return unarch_tar_open(t, unarch_mem_read, mem, (uint64_t)s_arc_len, lim);
}
