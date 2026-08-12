/**
 * @file ra8_fs_utf_internal.h
 * @brief UTF-8 <-> UTF-16LE conversion and case folding for `ra8_fs` names.
 * @ingroup grp_storage
 *
 * @details
 * Both on-disk name formats this adapter speaks store UTF-16LE: a VFAT
 * long-name chain carries thirteen units per slot, and an exFAT file-name entry
 * carries fifteen. The public API is UTF-8 `char*` and stays that way, so
 * exactly one seam has to exist between the two -- this one. It is a seam and
 * not a scattering of casts on purpose: the three separate places that used to
 * do their own byte-to-unit arithmetic each got it wrong in a different
 * direction (#606), and a single conversion cannot disagree with itself.
 *
 * ## What is representable
 *
 * The whole of Unicode that UTF-16 can express: the Basic Multilingual Plane in
 * one unit, and the supplementary planes as a surrogate PAIR. A four-byte UTF-8
 * sequence therefore costs two UTF-16 units on disk and comes back as the same
 * four bytes -- `NameLength` on exFAT and the VFAT group count are unit counts,
 * which is what the formats mean by "length" and what a host counts.
 *
 * ## What is refused, loudly
 *
 * Everything a decoder may not silently accept, because accepting it is how a
 * name stops round-tripping:
 *
 * - an over-long encoding (`C0 80` for NUL, `E0 80 80`, ...), which is a
 *   different byte string for a code point that already has one;
 * - a surrogate code point encoded directly in UTF-8 (`ED A0 80`), which is
 *   CESU-8 / WTF-8 and not UTF-8;
 * - a truncated sequence, a continuation byte where a lead byte belongs, or a
 *   lead byte announcing five or more bytes;
 * - anything above U+10FFFF.
 *
 * Each yields ::k_ra8_err_invalid_arg from the conversion, which every caller
 * turns into a failed create / open rather than a mangled name. There is no
 * substitution character anywhere in this module: `?` in place of a code point
 * is exactly the defect this file exists to remove.
 *
 * The one asymmetry is deliberate. ::priv_utf16_to_utf8() can be handed an
 * UNPAIRED surrogate, because that is a sequence of units that already exists
 * on someone else's volume, and no UTF-8 string encodes it. It reports
 * ::k_ra8_err_invalid_arg and the caller falls back to the entry's 8.3 alias,
 * so the invariant "a name this library reports is a name it can re-open" holds
 * even for a volume written by something that did not check.
 *
 * ## Case folding
 *
 * ::priv_utf16_ieq() folds through ::priv_exfat_upcase_unit(), the canonical
 * Microsoft up-case table this tree already embeds and writes at format time.
 * That is the table the exFAT specification's name hash is defined against, so
 * using it for FAT long names too means one fold serves both formats -- and
 * `Resume.txt` matching `RESUME.TXT` is the same mechanism as its accented
 * sibling matching, rather than a second rule that only covers ASCII.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_err.h"

/**
 * @enum ra8_fs_utf_t
 * @brief Bit patterns, thresholds and shifts of the UTF-8 / UTF-16 encodings.
 *
 * @details Named rather than inlined because every one of them appears in more
 *          than one direction of the codec: the mask that recognises a
 *          three-byte lead byte is the same mask that builds one. Unicode 15.0
 *          Table 3-6 ("UTF-8 Bit Distribution") and Table 3-5 ("UTF-16 Bit
 *          Distribution") are the source for the layouts.
 *
 * @invariant `k_utf_min_2byte`, `k_utf_min_3byte` and `k_utf_min_4byte` are the
 *            SMALLEST code point each sequence length may encode, so a value
 *            below its length's minimum is over-long by definition.
 * @invariant `k_utf8_max_per_unit` is 3: a BMP code point is one UTF-16 unit
 *            and at most three UTF-8 bytes, and a supplementary one is two
 *            units and four bytes, which is less per unit.
 * @see priv_utf8_to_utf16()
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_utf_ascii_max     = 0x7FU,     /**< Highest code point that is one UTF-8 byte.  */
  k_utf_cont_mask     = 0xC0U,     /**< Mask isolating a continuation byte's tag.   */
  k_utf_cont_tag      = 0x80U,     /**< Tag every continuation byte carries.        */
  k_utf_cont_payload  = 0x3FU,     /**< Payload bits in a continuation byte.        */
  k_utf_cont_shift    = 6U,        /**< Payload width of one continuation byte.     */
  k_utf_lead2_mask    = 0xE0U,     /**< Mask isolating a two-byte lead's tag.       */
  k_utf_lead2_tag     = 0xC0U,     /**< Tag of a two-byte lead byte.                */
  k_utf_lead2_payload = 0x1FU,     /**< Payload bits in a two-byte lead.            */
  k_utf_lead3_mask    = 0xF0U,     /**< Mask isolating a three-byte lead's tag.     */
  k_utf_lead3_tag     = 0xE0U,     /**< Tag of a three-byte lead byte.              */
  k_utf_lead3_payload = 0x0FU,     /**< Payload bits in a three-byte lead.          */
  k_utf_lead4_mask    = 0xF8U,     /**< Mask isolating a four-byte lead's tag.      */
  k_utf_lead4_tag     = 0xF0U,     /**< Tag of a four-byte lead byte.               */
  k_utf_lead4_payload = 0x07U,     /**< Payload bits in a four-byte lead.           */
  k_utf_len_1         = 1U,        /**< Sequence length: plain ASCII.               */
  k_utf_len_2         = 2U,        /**< Sequence length: two bytes.                 */
  k_utf_len_3         = 3U,        /**< Sequence length: three bytes.               */
  k_utf_len_4         = 4U,        /**< Sequence length: four bytes.                */
  k_utf_min_2byte     = 0x80U,     /**< Smallest code point a 2-byte form may hold. */
  k_utf_min_3byte     = 0x800U,    /**< Smallest code point a 3-byte form may hold. */
  k_utf_min_4byte     = 0x10000U,  /**< Smallest code point a 4-byte form may hold. */
  k_utf_sur_hi_first  = 0xD800U,   /**< First high (leading) surrogate unit.        */
  k_utf_sur_lo_first  = 0xDC00U,   /**< First low (trailing) surrogate unit.        */
  k_utf_sur_last      = 0xDFFFU,   /**< Last surrogate unit of either half.         */
  k_utf_sur_shift     = 10U,       /**< Payload width of one surrogate unit.        */
  k_utf_sur_mask      = 0x3FFU,    /**< Payload bits in one surrogate unit.         */
  k_utf_unit_max      = 0xFFFFU,   /**< Largest UTF-16 code unit.                   */
  k_utf_code_max      = 0x10FFFFU, /**< Largest code point Unicode defines.         */
  k_utf_byte_mask     = 0xFFU,     /**< Mask isolating a unit's low byte.           */
  k_utf_byte_shift    = 8U,        /**< Shift bringing a unit's high byte down.     */
  k_utf8_max_per_unit = 3U,        /**< Worst-case UTF-8 bytes per UTF-16 unit.     */
} ra8_fs_utf_t;

/**
 * @brief Convert a NUL-terminated UTF-8 name into UTF-16LE code units.
 *
 * @details Decodes each UTF-8 sequence to a code point, rejecting every
 *          malformed and non-shortest form (see the file header), then emits it
 *          as one BMP unit or as a high/low surrogate pair. The output is NOT
 *          NUL-terminated: a UTF-16 name on either format carries an explicit
 *          length, and appending a terminator here would invite a caller to use
 *          it as one.
 *
 *          The whole conversion is transactional in the sense that matters: on
 *          any failure @p out_units is set to zero, so a caller that ignores
 *          the return code writes an empty name rather than a truncated one.
 *
 * @param[in]  in        NUL-terminated UTF-8 name.
 * @param[out] out       Receives the code units.
 * @param[in]  cap       Capacity of @p out in UTF-16 units.
 * @param[out] out_units Receives the number of units written.
 *
 * @return Error code.
 * @retval k_ra8_ok              Converted; @p out holds `*out_units` units.
 * @retval k_ra8_err_null_ptr    @p in, @p out or @p out_units is NULL.
 * @retval k_ra8_err_invalid_arg @p in is not well-formed UTF-8.
 * @retval k_ra8_err_no_mem      The name needs more than @p cap units.
 *
 * @pre @p out addresses at least @p cap writable units.
 * @pre @p in is NUL-terminated within the caller's buffer.
 * @post On success `*out_units` is at most @p cap.
 * @post On failure `*out_units` is 0 and @p out holds nothing meaningful.
 *
 * @note Pure apart from @p out; trivially thread-safe against distinct buffers.
 *
 * @since 0.1.0
 */
RA8_PRIV
ra8_err_t priv_utf8_to_utf16(const char* in, uint16_t* out, uint32_t cap, uint32_t* out_units);

/**
 * @brief Convert UTF-16LE code units into a NUL-terminated UTF-8 name.
 *
 * @details The inverse of ::priv_utf8_to_utf16(): a high surrogate followed by
 *          a low one becomes the four-byte form of the supplementary code
 *          point, every other unit becomes its one-, two- or three-byte form.
 *
 *          An unpaired surrogate -- a high one not followed by a low one, or a
 *          low one on its own -- is a name no UTF-8 string can express, so it
 *          is reported rather than substituted. The caller's fallback (the 8.3
 *          alias on FAT) is a name that still opens the same file, which a
 *          replacement character would not be.
 *
 * @param[in]  in    Code units to convert.
 * @param[in]  units Number of units in @p in.
 * @param[out] out   Receives the NUL-terminated UTF-8 name.
 * @param[in]  cap   Capacity of @p out in bytes, including the terminator.
 *
 * @return Error code.
 * @retval k_ra8_ok              Converted; @p out is NUL-terminated.
 * @retval k_ra8_err_null_ptr    @p in or @p out is NULL, or @p cap is 0.
 * @retval k_ra8_err_invalid_arg @p in holds an unpaired surrogate.
 * @retval k_ra8_err_no_mem      The name plus its terminator exceeds @p cap.
 *
 * @pre @p out addresses at least @p cap writable bytes.
 * @pre @p in addresses at least @p units readable code units.
 * @post On success @p out holds well-formed UTF-8 and a NUL terminator.
 * @post On failure @p out[0] is NUL, so a caller that ignores the code sees an
 *       empty name rather than a partial one.
 *
 * @note Pure apart from @p out; trivially thread-safe against distinct buffers.
 *
 * @since 0.1.0
 */
RA8_PRIV
ra8_err_t priv_utf16_to_utf8(const uint16_t* in, uint32_t units, char* out, uint32_t cap);

/**
 * @brief Compare two UTF-16 names for case-insensitive equality.
 *
 * @details Folds every unit through ::priv_exfat_upcase_unit() -- the canonical
 *          up-case table -- and compares unit by unit. Lengths must match
 *          first: the table is a simple one-to-one map, so folding never
 *          changes a name's length and a length difference is a difference.
 *
 *          Surrogate units fold to themselves, because the table covers the BMP
 *          and a supplementary code point has no simple case mapping inside it.
 *          Two supplementary characters therefore compare exactly, which is
 *          what a host does with the same table.
 *
 * @param[in] a  First name's units.
 * @param[in] an Number of units in @p a.
 * @param[in] b  Second name's units.
 * @param[in] bn Number of units in @p b.
 *
 * @return Equality flag.
 * @retval 1U The names are equal after folding.
 * @retval 0U They differ in length or in at least one folded unit.
 *
 * @pre @p a addresses @p an units and @p b addresses @p bn units.
 * @pre Neither pointer is NULL unless its count is 0.
 * @post Neither input is modified.
 * @post The verdict depends only on the inputs and the fixed up-case table.
 *
 * @note Pure function; trivially thread-safe.
 *
 * @since 0.1.0
 */
RA8_PRIV
uint8_t priv_utf16_ieq(const uint16_t* a, uint32_t an, const uint16_t* b, uint32_t bn);

/**
 * @brief Is every unit of @p in inside the ASCII range?
 *
 * @details The question a caller asks before it relies on the volume's up-case
 *          table: an ASCII-only name folds identically under every conforming
 *          table, so it is safe even when the volume carries one this build
 *          cannot reproduce, while a name with any other unit is not.
 *
 * @param[in] in    Code units to inspect.
 * @param[in] units Number of units in @p in.
 *
 * @return ASCII-only flag.
 * @retval 1U Every unit is at most ::k_utf_ascii_max (or @p units is 0).
 * @retval 0U At least one unit is above it.
 *
 * @pre @p in addresses at least @p units readable units, or @p units is 0.
 * @pre The caller treats a 0 result as "needs the volume's own table".
 * @post @p in is not modified.
 * @post The verdict depends only on the inputs.
 *
 * @note Pure function; trivially thread-safe.
 *
 * @since 0.1.0
 */
RA8_PRIV
uint8_t priv_utf16_all_ascii(const uint16_t* in, uint32_t units);
