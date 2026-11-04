/**
 * @file ra8_fs_fat_name.c
 * @brief FAT 8.3 short-name pack/unpack and directory walking.
 *
 * @details
 * 8.3 short-name encode/decode, the directory-walk cursor, and the
 * short-name directory lookup primitives for the `ra8_fs` FAT adapter.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_fs.h"
#include "ra8_fs_fat_internal.h"

/**
 * @enum ra8_fs_name_local_t
 * @brief Small constants used only by this translation unit's name analysis.
 *
 * @details The case observations are a two-bit set rather than two booleans so
 *          that "mixed" is a value the code can name, instead of a conjunction
 *          the reader has to work out at each test.
 *
 * @invariant `k_case_seen_mixed` is exactly the two case bits together.
 * @see priv_name_case_kind()
 * @since 0.1.0
 */
typedef enum : uint16_t {
  k_lfn_space        = 0x20U, /**< Lowest character code a long name may hold. */
  k_lfn_del          = 0x7FU, /**< DEL: a control code, not a name character.  */
  k_case_seen_upper  = 0x01U, /**< An upper-case letter was seen.              */
  k_case_seen_lower  = 0x02U, /**< A lower-case letter was seen.               */
  k_case_seen_mixed  = 0x03U, /**< Both were: no NTRes flag can express it.    */
  k_alias_radix      = 10U,   /**< The alias tail is written in decimal.       */
  k_alias_digits_max = 6U,    /**< Digits in ::k_lfn_alias_tail_max.           */
} ra8_fs_name_local_t;

/* =============================================================================
 * 8.3 short name pack / unpack
 * =============================================================================
 */

/**
 * @brief Convert "FILE.TXT" (caller-supplied path) to packed 11-byte 8.3.
 *
 * @details
 * Result is space-padded as on-disk. Lower-case input is upper-cased.
 * Returns 0 on bad name (>8 base, >3 ext, missing chars), 1 on success.
 */


/**
 * @brief Pack the base portion of a path into out11[0..7]. Returns 0 on error.
 *
 * @details Reads characters from `*path_io` up to a `.` or NUL,
 *          upper-cases them, and writes them into `out11[0..7]`.
 *
 * @param[in,out] path_io Cursor into the input path; advanced on success.
 * @param[out]    out11   11-byte buffer; first 8 bytes are written.
 *
 * @return 1 on success, 0 on overflow or empty base.
 * @retval 1  Base name packed.
 * @retval 0  Base too long or zero-length.
 *
 * @pre `path_io`, `*path_io`, and `out11` are non-NULL.
 * @pre `out11` has been pre-padded with spaces by the caller.
 * @post On success, `out11[0..7]` holds the upper-cased base.
 * @post On success, `*path_io` points at the `.` or terminator.
 *
 * @note Helper used only by `priv_path_to_83`.
 *
 * @since 0.1.0
 */


/**
 * @brief Pack the extension portion of a path into out11[8..10]. Returns 0 on error.
 *
 * @details If `*path` is not `.`, returns success with no writes.
 *
 * @param[in]  path  Cursor at the `.` or terminator following the base.
 * @param[out] out11 11-byte buffer; bytes 8..10 are written.
 *
 * @return 1 on success, 0 on overflow.
 * @retval 1  Extension packed (or absent).
 * @retval 0  Extension too long.
 *
 * @pre `path` and `out11` are non-NULL.
 * @pre `out11` has been pre-padded with spaces by the caller.
 * @post On success, `out11[8..10]` holds the upper-cased extension.
 * @post `*path` is not modified.
 *
 * @note Helper used only by `priv_path_to_83`.
 *
 * @since 0.1.0
 */


RA8_INTERNAL
extern uint8_t internal_pack_base(const char** path_io, uint8_t* out11);
RA8_INTERNAL
extern uint8_t internal_pack_ext(const char* path, uint8_t* out11);



/**
 * @brief Lower-case one ASCII character when a `DIR_NTRes` flag asks for it.
 *
 * @details The FAT directory entry always stores the upper-case 8.3 form; the
 *          case a user typed survives in two bits of `DIR_NTRes`, one for the
 *          base and one for the extension. This applies the bit that governs
 *          the half of the name the character came from.
 *
 * @param[in] c     Character straight out of the packed 11-byte name field.
 * @param[in] lower Non-zero when this half of the name is flagged lower case.
 *
 * @return The character to render.
 * @retval 'a'..'z' @p c was 'A'..'Z' and @p lower was set.
 * @retval c        Otherwise, unchanged.
 *
 * @pre @p c is one byte of a packed 8.3 name field.
 * @pre @p lower is 0 or non-zero; no other interpretation is placed on it.
 * @post No state outside the return value is touched.
 * @post Characters outside 'A'..'Z' are returned verbatim whatever @p lower is.
 *
 * @note Pure function; trivially thread-safe.
 *
 * @since 0.1.0
 */


/* =============================================================================
 * Long-name classification -- which on-disk shape a leaf component needs
 * =============================================================================
 */



/**
 * @brief Is @p u a code unit a VFAT long name is allowed to hold?
 *
 * @details Rejects the control range, DEL, and the explicit illegal set from
 *          the FAT specification. Everything above ASCII is ALLOWED: the slots
 *          store UTF-16 and this adapter now reads them back as such, so the
 *          old "at or above DEL" cut -- which existed only because the reader
 *          substituted `?` for those units -- would refuse names the format and
 *          the reader both handle.
 *
 * @param[in] u Candidate code unit.
 *
 * @return Legality flag.
 * @retval 1U @p u may appear in a long name.
 * @retval 0U @p u may not; the whole leaf is rejected.
 *
 * @pre @p u is one code unit of a caller-supplied leaf component.
 * @pre The caller treats a 0 result as a fatal name error, not a fallback.
 * @post No state is modified.
 * @post The verdict depends only on @p u.
 *
 * @note Pure function; trivially thread-safe.
 *
 * @since 0.1.0
 */


/**
 * @brief Is @p c usable verbatim inside a packed 8.3 short name?
 *
 * @details Stricter than ::priv_unit_is_lfn_legal() by the extra short-name
 *          exclusions, by the dot -- which in an 8.3 name is a field separator
 *          rather than a character -- and by the ASCII cut, which belongs here
 *          rather than there: a packed 8.3 field is an OEM-code-page byte and
 *          this adapter only writes ASCII into one, while a long name has the
 *          whole of UTF-16 available to it.
 *
 * @param[in] u Candidate code unit.
 *
 * @return Legality flag.
 * @retval 1U @p u may appear in the base or extension field.
 * @retval 0U @p u may not; a name containing it needs a long-name chain.
 *
 * @pre @p u already passed ::priv_unit_is_lfn_legal().
 * @pre @p u is one code unit of a caller-supplied leaf component.
 * @post No state is modified.
 * @post The verdict depends only on @p u.
 *
 * @note Pure function; trivially thread-safe.
 *
 * @since 0.1.0
 */


/**
 * @brief Index of the field-separating dot, or @p n when there is none.
 *
 * @details The LAST dot, because that is the one an extension would follow.
 *          A name holding more than one is not 8.3 anyway: the earlier dots
 *          then sit inside the base, where ::priv_unit_is_83_legal() refuses
 *          them, so the rejection needs no special case here.
 *
 * @param[in] leaf Leaf component's code units (no slashes).
 * @param[in] n    Length of @p leaf in code units.
 *
 * @return Dot index, or @p n when there is none.
 * @retval 0..n-1 The last dot's index.
 * @retval n      There is no dot.
 *
 * @pre @p leaf is non-NULL and holds at least @p n units.
 * @pre @p n is at most ::k_lfn_write_max.
 * @post @p leaf is not modified.
 * @post The scan visits exactly @p n units.
 *
 * @note Pure function; trivially thread-safe.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static uint32_t RA8_INTERNAL
extern uint8_t internal_unit_is_lfn_legal(uint16_t u);


/**
 * @brief Can @p leaf be stored as a packed 8.3 name without losing anything?
 *
 * @details Requires a 1..8 character base, a 0 or 1..3 character extension,
 *          and every character legal in a short name. A second dot is caught
 *          by that last rule rather than by a count of its own: everything
 *          before the LAST dot is the base, so an earlier one is simply a
 *          character the base may not hold. A trailing dot (`"REPORT."`)
 *          fails the extension test and is therefore stored as a long name,
 *          which is the only way to read it back as it was written.
 *
 * @param[in] leaf Leaf component's code units (no slashes).
 * @param[in] n    Length of @p leaf in code units.
 *
 * @return Representability flag.
 * @retval 1U @p leaf packs to 8.3 losslessly, up to case.
 * @retval 0U @p leaf needs a long-name chain.
 *
 * @pre @p leaf is non-NULL and holds at least @p n units.
 * @pre @p n is non-zero and at most ::k_lfn_write_max.
 * @post @p leaf is not modified.
 * @post The verdict is independent of the volume's contents.
 *
 * @note Pure function; trivially thread-safe.
 *
 * @since 0.1.0
 */


/**
 * @brief Fold one half of a name into "has upper" / "has lower" observations.
 *
 * @details Walks @p leaf from @p from up to (not including) @p to and ORs a bit
 *          into @p seen for each case class present. Digits and punctuation set
 *          neither, so `"IMG_01"` is upper-only and `"01"` is neither, which is
 *          what makes a case-flagged short name possible for both.
 *
 * @param[in]     leaf Leaf component's code units.
 * @param[in]     from First index to inspect.
 * @param[in]     to   One past the last index to inspect.
 * @param[in,out] seen Bit 0 set when an upper-case letter is seen, bit 1 when
 *                     a lower-case one is.
 *
 * @return Nothing.
 *
 * @pre @p leaf is non-NULL and holds at least @p to units.
 * @pre @p from is at most @p to and @p seen is non-NULL.
 * @post @p seen has only bits it already had, plus the ones observed here.
 * @post @p leaf is not modified.
 *
 * @note Pure accumulation; trivially thread-safe against distinct @p seen.
 *
 * @since 0.1.0
 */


/**
 * @brief Decide the case shape of an 8.3-representable leaf.
 *
 * @details A half of the name that is all one case round-trips through a
 *          `DIR_NTRes` bit; a half that mixes cases does not, and the whole
 *          name then needs a long-name chain to survive. The two halves are
 *          judged independently, exactly as the two flag bits are.
 *
 * @param[in]  leaf       Leaf component's units, already known to pack to 8.3.
 * @param[in]  n          Length of @p leaf in code units.
 * @param[out] out_ntres  Receives the `DIR_NTRes` byte to store.
 *
 * @return The on-disk shape required.
 * @retval k_name_kind_short One entry plus @p out_ntres suffices.
 * @retval k_name_kind_long  A half mixes cases; a chain is required.
 *
 * @pre @p leaf and @p out_ntres are non-NULL; @p n is @p leaf's length.
 * @pre ::priv_name_is_83() has already returned 1 for @p leaf.
 * @post @p out_ntres is written on both outcomes (0 on the long one).
 * @post @p leaf is not modified.
 *
 * @note Pure function; trivially thread-safe.
 *
 * @since 0.1.0
 */




/* =============================================================================
 * Basis-name generation -- the `LONGNA~1.TXT` alias a long name is filed under
 * =============================================================================
 */

/**
 * @brief Map one long-name character into the short-name character set.
 *
 * @details Upper-cases, then substitutes `_` for anything an 8.3 name cannot
 *          hold. This is the FAT specification's "Basis-Name Generation
 *          Algorithm" step that makes the alias representable at all: without
 *          it a name like `Design (final).txt` would generate an alias with a
 *          space and parentheses in it, which no 8.3 reader accepts.
 *
 * @param[in] u Code unit from the caller's long name.
 *
 * @return The character to place in the alias.
 * @retval '_' @p u is not legal in an 8.3 name -- which now includes every unit
 *             above ASCII, exactly as VFAT does for a character its OEM code
 *             page cannot represent.
 * @retval other The upper-cased @p u.
 *
 * @pre @p u passed ::priv_unit_is_lfn_legal().
 * @pre The caller has already discarded spaces and dots.
 * @post No state is modified.
 * @post The result is always legal in a packed 8.3 name.
 *
 * @note Pure function; trivially thread-safe.
 *
 * @since 0.1.0
 */


