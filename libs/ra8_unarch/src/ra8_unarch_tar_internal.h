/**
 * @file ra8_unarch_tar_internal.h
 * @brief Module-private tar field/record parsers shared across the tar TUs.
 *
 * @par Tag
 * [Ring 4 / Domain] {World: NS}
 *
 * @details
 * The tar walker's untrusted-byte parsers live in their own TU
 * (`ra8_unarch_tar_fields.c`) behind these TU-external declarations so the
 * host tests can drive every validation branch directly with hostile field
 * bytes (see CLAUDE.md "Test access to internal symbols"). Production
 * callers are exclusively `ra8_unarch_tar.c`; nothing outside the module
 * and `tests/` may include this header.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since Version 0.1.0
 *
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_unarch_tar.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @enum ra8_tar_layout_t
 * @brief Byte offsets / lengths of the ustar header fields this reader uses.
 * @details From the POSIX.1-2017 pax Interchange Format header block
 *          layout; named per the no-magic-numbers rule.
 * @since Version 0.1.0
 */
typedef enum : uint16_t {
  k_ra8_tar_off_name       = 0U,   /**< `name` field offset.             */
  k_ra8_tar_len_name       = 100U, /**< `name` field length.             */
  k_ra8_tar_off_size       = 124U, /**< `size` field offset.             */
  k_ra8_tar_len_size       = 12U,  /**< `size` field length.             */
  k_ra8_tar_off_chksum     = 148U, /**< `chksum` field offset.           */
  k_ra8_tar_len_chksum     = 8U,   /**< `chksum` field length.           */
  k_ra8_tar_off_type       = 156U, /**< `typeflag` byte offset.          */
  k_ra8_tar_off_magic      = 257U, /**< `magic` field offset.            */
  k_ra8_tar_len_magic      = 5U,   /**< Compared magic length ("ustar"). */
  k_ra8_tar_off_magic_term = 262U, /**< Byte after "ustar" (NUL or ' '). */
  k_ra8_tar_off_prefix     = 345U, /**< `prefix` field offset.           */
  k_ra8_tar_len_prefix     = 155U, /**< `prefix` field length.           */
} ra8_tar_layout_t;

/**
 * @enum ra8_tar_type_t
 * @brief Normalised classification of a header block's typeflag.
 * @details Collapses the tar typeflag byte onto the five shapes the walker
 *          handles distinctly; every unknown flag maps to
 *          ::k_ra8_tar_type_other (enumerated, skipped by size).
 * @since Version 0.1.0
 */
typedef enum : uint8_t {
  k_ra8_tar_type_file     = 0U, /**< Regular file ('0' or NUL).              */
  k_ra8_tar_type_dir      = 1U, /**< Directory ('5').                        */
  k_ra8_tar_type_pax      = 2U, /**< pax extended header ('x').              */
  k_ra8_tar_type_meta     = 3U, /**< Skipped meta: 'g' global, 'K' longlink. */
  k_ra8_tar_type_longname = 4U, /**< GNU longname data ('L').                */
  k_ra8_tar_type_other    = 5U, /**< Any other member kind (skipped).        */
} ra8_tar_type_t;

/**
 * @brief Decode a tar numeric field (octal ASCII or GNU base-256).
 *
 * @details Octal fields may carry leading spaces and are terminated by NUL
 *          or space; any other byte, an empty field, or 64-bit overflow is
 *          rejected. A field whose first byte has the top bit set is GNU
 *          base-256 (big-endian binary): negative values and values over
 *          64 bits are rejected.
 *
 * @param[in]  field Field bytes (non-NULL).
 * @param[in]  len   Field length in bytes (> 0).
 * @param[out] out   Receives the decoded value (non-NULL).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                    Value decoded into @p out.
 * @retval k_ra8_err_null_ptr          @p field or @p out was NULL.
 * @retval k_ra8_err_validation_failed Empty, malformed, negative, or
 *                                     overflowing field.
 *
 * @pre @p field holds @p len readable bytes.
 * @pre @p len is a real header-field length (bounded by the block size).
 * @post On k_ra8_ok, `*out` holds the exact field value.
 * @post On any error `*out` is 0.
 *
 * @note Thread-safe: pure read.
 * @par MC/DC:
 * Promoted to TU-external linkage so tests can drive the octal-digit
 * range compounds and the base-256 fit checks with hostile field bytes.
 * @since Version 0.1.0
 */
RA8_PRIV [[nodiscard]] ra8_err_t
priv_unarch_tar_num(const uint8_t* field, size_t len, uint64_t* out);

/**
 * @brief Whether a header block is all zero bytes (end-of-archive marker).
 *
 * @details tar terminates an archive with two zero blocks; this walker
 *          treats the first as a clean end (tolerant of single-block
 *          writers, harmless for hostile input -- end is end).
 *
 * @param[in] block One ::k_ra8_unarch_tar_block byte block (non-NULL).
 *
 * @return Whether every byte is zero.
 * @retval true  End-of-archive marker.
 * @retval false At least one non-zero byte.
 *
 * @pre @p block holds a full block of readable bytes.
 * @pre @p block is non-NULL (caller-guarded).
 * @post No state is modified (pure read).
 * @post The result depends only on the block bytes.
 *
 * @note Thread-safe: pure read.
 * @par MC/DC:
 * Promoted to TU-external linkage for direct test access (no compound
 * decisions; the scan is a single-condition loop).
 * @since Version 0.1.0
 */
RA8_PRIV [[nodiscard]] bool priv_unarch_tar_block_zero(const uint8_t* block);

/**
 * @brief Verify a header block's checksum (unsigned byte sum).
 *
 * @details Sums all block bytes with the chksum field replaced by spaces
 *          (per the tar specification) and compares against the octal
 *          value stored in the chksum field. A field that fails octal
 *          decode fails the check.
 *
 * @param[in] block One ::k_ra8_unarch_tar_block byte block (non-NULL).
 *
 * @return Whether the stored checksum matches the computed sum.
 * @retval true  Checksum holds.
 * @retval false Mismatch or undecodable chksum field.
 *
 * @pre @p block holds a full block of readable bytes.
 * @pre @p block is non-NULL (caller-guarded).
 * @post No state is modified (pure read).
 * @post The result depends only on the block bytes.
 *
 * @note Thread-safe: pure read.
 * @par MC/DC:
 * Promoted to TU-external linkage so tests can drive the mismatch and
 * undecodable-field arms directly.
 * @since Version 0.1.0
 */
RA8_PRIV [[nodiscard]] bool priv_unarch_tar_checksum_ok(const uint8_t* block);

/**
 * @brief Whether a header block carries the ustar / GNU magic.
 *
 * @details Requires "ustar" at the magic offset followed by NUL (POSIX)
 *          or space (old GNU). Pre-POSIX v7 headers carry no magic and
 *          are rejected -- every modern tar writer emits ustar.
 *
 * @param[in] block One ::k_ra8_unarch_tar_block byte block (non-NULL).
 *
 * @return Whether the magic matches either dialect.
 * @retval true  POSIX ustar or old-GNU magic.
 * @retval false Anything else (including v7 headers).
 *
 * @pre @p block holds a full block of readable bytes.
 * @pre @p block is non-NULL (caller-guarded).
 * @post No state is modified (pure read).
 * @post The result depends only on the magic bytes.
 *
 * @note Thread-safe: pure read.
 * @par MC/DC:
 * Promoted to TU-external linkage so tests can drive the NUL-vs-space
 * terminator compound directly.
 * @since Version 0.1.0
 */
RA8_PRIV [[nodiscard]] bool priv_unarch_tar_magic_ok(const uint8_t* block);

/**
 * @brief Classify a header block's typeflag byte.
 *
 * @details Maps the tar typeflag onto ::ra8_tar_type_t: regular file
 *          ('0' / NUL), directory ('5'), pax extended header ('x'),
 *          skipped meta ('g' global, 'K' GNU longlink), GNU longname
 *          ('L'), and everything else as an enumerated-but-skipped member.
 *
 * @param[in] typeflag The header's typeflag byte.
 *
 * @return The normalised ::ra8_tar_type_t class.
 * @retval k_ra8_tar_type_other For any flag this reader does not handle.
 *
 * @pre None -- every byte value maps to a class.
 * @pre The caller applies the class, not the raw flag.
 * @post No state is modified (pure mapping).
 * @post The result depends only on @p typeflag.
 *
 * @note Thread-safe: pure mapping.
 * @par MC/DC:
 * Promoted to TU-external linkage for direct test access (single-condition
 * comparisons only).
 * @since Version 0.1.0
 */
RA8_PRIV [[nodiscard]] ra8_tar_type_t priv_unarch_tar_classify(uint8_t typeflag);

/**
 * @brief Parse pax extended-header records, extracting path / size.
 *
 * @details Walks the "%d key=value\n" record stream fail-closed: a record
 *          whose declared length is non-decimal, too small, overruns the
 *          data, or lacks the '=' / trailing newline rejects the whole
 *          header. A `path` record is copied into @p name_buf (clamped to
 *          @p name_cap) and a `size` record is decimal-decoded with
 *          overflow checks; every other key is skipped.
 *
 * @param[in]  data      pax data bytes (non-NULL).
 * @param[in]  len       pax data length (<= ::k_ra8_unarch_tar_pax_max).
 * @param[out] name_buf  Buffer for a `path` value (may be NULL if
 *                       @p name_cap is 0).
 * @param[in]  name_cap  Capacity of @p name_buf in bytes.
 * @param[out] name_len  Receives the copied path length (non-NULL).
 * @param[out] have_path Set true when a `path` record was applied
 *                       (non-NULL).
 * @param[out] size_ovr  Receives a decoded `size` value (non-NULL).
 * @param[out] have_size Set true when a `size` record was applied
 *                       (non-NULL).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                    Records parsed; overrides reported.
 * @retval k_ra8_err_null_ptr          A required pointer was NULL.
 * @retval k_ra8_err_validation_failed A malformed record (bad length,
 *                                     missing '=', missing newline, bad
 *                                     size value).
 *
 * @pre @p data holds @p len readable bytes.
 * @pre The output flags are initialised by the caller (false / 0).
 * @post On k_ra8_ok the flags reflect exactly the records present.
 * @post On any error the walk must reject the member (fail-closed).
 *
 * @note Thread-safe: writes only through its out-parameters.
 * @par MC/DC:
 * Promoted to TU-external linkage so tests can drive every malformed
 * record shape (length, delimiter, terminator, value) directly.
 * @since Version 0.1.0
 */
RA8_PRIV [[nodiscard]] ra8_err_t priv_unarch_tar_pax_parse(const uint8_t* data,
                                                           size_t         len,
                                                           char*          name_buf,
                                                           uint16_t       name_cap,
                                                           uint16_t*      name_len,
                                                           bool*          have_path,
                                                           uint64_t*      size_ovr,
                                                           bool*          have_size);

#ifdef __cplusplus
}
#endif
