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
typedef enum : uint8_t {
  k_lfn_space        = 0x20U, /**< Lowest character code a long name may hold. */
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
/* `priv_to_upper()`: see header for the documented contract. */
char priv_to_upper(char c)
{
  if (c >= 'a' && c <= 'z') {
    return (char)(c - 'a' + 'A');
  }
  return c;
}

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
RA8_INTERNAL
static uint8_t priv_pack_base(const char** path_io, uint8_t* out11)
{
  const char* path     = *path_io;
  uint8_t     base_len = 0;
  while (*path != '\0' && *path != '.') {
    if (base_len >= k_filename_base_len) {
      return 0U;
    }
    out11[base_len++] = (uint8_t)priv_to_upper(*path++);
  }
  if (base_len == 0U) {
    return 0U;
  }
  *path_io = path;
  return 1U;
}

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
static uint8_t priv_pack_ext(const char* path, uint8_t* out11)
{
  if (*path != '.') {
    return 1U;
  }
  path++;
  uint32_t ext_len = 0U;
  while (*path != '\0') {
    if (ext_len >= (uint32_t)k_filename_ext_len) {
      return 0U;
    }
    out11[k_filename_base_len + ext_len] = (uint8_t)priv_to_upper(*path++);
    ext_len++;
  }
  return 1U;
}

/* `priv_path_to_83()`: see header for the documented contract. */
uint8_t priv_path_to_83(const char* path, uint8_t* out11)
{
  if (path == nullptr || out11 == nullptr) {
    return 0U;
  }
  while (*path == '/') {
    path++;
  }
  for (uint32_t i = 0; i < (uint32_t)k_max_8_3_name; i++) {
    out11[i] = ' ';
  }
  if (priv_pack_base(&path, out11) == 0U) {
    return 0U;
  }
  if (priv_pack_ext(path, out11) == 0U) {
    return 0U;
  }
  if (out11[0] == k_dir_marker_free_used) {
    out11[0] = k_dir_marker_kanji_e5;
  }
  return 1U;
}

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
RA8_INTERNAL
static char priv_case_apply(char c, uint8_t lower)
{
  if (lower == 0U) {
    return c;
  }
  if ((c >= 'A') && (c <= 'Z')) {
    /* Arithmetic in an unsigned type, not on the chars themselves (MISRA 10.2
     * forbids + / - on essentially-character operands), and the cast lands on
     * a plain object rather than on the composite expression (MISRA 10.8). */
    const uint32_t lowered = (uint32_t)c + ((uint32_t)'a' - (uint32_t)'A');
    return (char)lowered;
  }
  return c;
}

/* `priv_83_to_str()`: see header for the documented contract. */
void priv_83_to_str(const uint8_t* in11, uint8_t ntres, char* out13)
{
  const uint8_t base_lower = (uint8_t)(ntres & (uint8_t)k_ntres_base_lower);
  const uint8_t ext_lower  = (uint8_t)(ntres & (uint8_t)k_ntres_ext_lower);
  uint32_t      i          = 0;
  uint32_t      j          = 0;
  for (i = 0; i < (uint32_t)k_filename_base_len; i++) {
    if (in11[i] == ' ') {
      break;
    }
    out13[j++] = priv_case_apply((char)in11[i], base_lower);
  }
  /* Restore kanji escape. */
  /* mcdc-deactivated: 3-condition AND on Shift-JIS kanji-escape directory entry; only reachable from a kanji-named FAT image, none of which exist in the test corpus. */
  if (j > 0 && (uint8_t)out13[0] == k_dir_marker_kanji_e5 && in11[0] == k_dir_marker_kanji_e5) {
    out13[0] = (char)k_dir_marker_free_used;
  }
  uint8_t has_ext = 0;
  for (i = 0; i < k_filename_ext_len; i++) {
    if (in11[k_filename_base_len + i] != ' ') {
      has_ext = 1;
      break;
    }
  }
  if (has_ext != 0U) {
    out13[j++] = '.';
    for (i = 0; i < k_filename_ext_len; i++) {
      if (in11[k_filename_base_len + i] == ' ') {
        break;
      }
      out13[j++] = priv_case_apply((char)in11[k_filename_base_len + i], ext_lower);
    }
  }
  out13[j] = '\0';
}

/* =============================================================================
 * Long-name classification -- which on-disk shape a leaf component needs
 * =============================================================================
 */

/**
 * @brief Does @p c appear in the NUL-terminated set @p set?
 *
 * @details Linear scan, bounded by ::k_lfn_name_cap so the loop has a static
 *          upper limit even though both call sites pass a string literal
 *          (NASA Power of 10 Rule 2).
 *
 * @param[in] c   Character to look for.
 * @param[in] set NUL-terminated set of characters.
 *
 * @return Membership flag.
 * @retval 1U @p c is in @p set.
 * @retval 0U @p c is not in @p set.
 *
 * @pre @p set is non-NULL and NUL-terminated.
 * @pre @p set is shorter than ::k_lfn_name_cap characters.
 * @post Neither @p c nor @p set is modified.
 * @post The scan visits at most ::k_lfn_name_cap characters.
 *
 * @note Pure function; trivially thread-safe.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static uint8_t priv_char_in_set(char c, const char* set)
{
  for (uint32_t i = 0U; i < (uint32_t)k_lfn_name_cap; i++) {
    if (set[i] == '\0') {
      return 0U;
    }
    if (set[i] == c) {
      return 1U;
    }
  }
  /* Unreachable: both sets are single-digit literals, so the NUL above always
   * ends the scan first. This is the Rule 2 bound's exit, not a second answer. */
  return 0U; /* GCOVR_EXCL_LINE */
}

/**
 * @brief Is @p c a character a VFAT long name is allowed to hold?
 *
 * @details Rejects the control range, everything at or above DEL (this
 *          adapter reassembles long names into ASCII, so a byte it could not
 *          read back is a byte it must not write), and the explicit illegal
 *          set from the FAT specification.
 *
 * @param[in] c Candidate character.
 *
 * @return Legality flag.
 * @retval 1U @p c may appear in a long name.
 * @retval 0U @p c may not; the whole leaf is rejected.
 *
 * @pre @p c is one character of a caller-supplied leaf component.
 * @pre The caller treats a 0 result as a fatal name error, not a fallback.
 * @post No state is modified.
 * @post The verdict depends only on @p c.
 *
 * @note Pure function; trivially thread-safe.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static uint8_t priv_char_is_lfn_legal(char c)
{
  /* Microsoft FAT specification section 7 ("Long File Name Implementation"):
   * the long name inherits the short name's illegal set minus the characters
   * VFAT deliberately allows (`+ , ; = [ ]` and space). Control characters are
   * excluded by range below, not by this table. Block scope because exactly
   * one function reads it (MISRA 8.9). */
  static const char s_lfn_illegal[] = "\"*/:<>?\\|";

  const uint32_t u = (uint32_t)(unsigned char)c;
  if (u < (uint32_t)k_lfn_space) {
    return 0U; /* control characters, including the NUL that ends the name */
  }
  if (u >= (uint32_t)k_lfn_ascii_max) {
    return 0U; /* DEL and everything above it: this adapter stores ASCII only */
  }
  if (priv_char_in_set(c, s_lfn_illegal) != 0U) {
    return 0U;
  }
  return 1U;
}

/**
 * @brief Is @p c usable verbatim inside a packed 8.3 short name?
 *
 * @details Stricter than ::priv_char_is_lfn_legal() by the extra short-name
 *          exclusions and by the dot, which in an 8.3 name is a field
 *          separator rather than a character. It does NOT repeat the long-name
 *          legality test: both call sites run after that test has passed, and
 *          a duplicate here would be a branch no input could take.
 *
 * @param[in] c Candidate character.
 *
 * @return Legality flag.
 * @retval 1U @p c may appear in the base or extension field.
 * @retval 0U @p c may not; a name containing it needs a long-name chain.
 *
 * @pre @p c already passed ::priv_char_is_lfn_legal().
 * @pre @p c is one character of a caller-supplied leaf component.
 * @post No state is modified.
 * @post The verdict depends only on @p c.
 *
 * @note Pure function; trivially thread-safe.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static uint8_t priv_char_is_83_legal(char c)
{
  /* Microsoft FAT specification section 6.1 ("Name Limitations"). A leaf
   * containing any of these is representable only as a long name, however
   * short it is -- `my file.txt` is eight characters of base and still cannot
   * be an 8.3 name, because of the space. Block scope: one reader (MISRA 8.9). */
  static const char s_sfn_extra_illegal[] = "+,;=[] ";

  if (c == '.') {
    return 0U;
  }
  if (priv_char_in_set(c, s_sfn_extra_illegal) != 0U) {
    return 0U;
  }
  return 1U;
}

/**
 * @brief Index of the field-separating dot, or @p n when there is none.
 *
 * @details The LAST dot, because that is the one an extension would follow.
 *          A name holding more than one is not 8.3 anyway: the earlier dots
 *          then sit inside the base, where ::priv_char_is_83_legal() refuses
 *          them, so the rejection needs no special case here.
 *
 * @param[in] leaf Leaf component (no slashes).
 * @param[in] n    Length of @p leaf in characters.
 *
 * @return Dot index, or @p n when there is none.
 * @retval 0..n-1 The last dot's index.
 * @retval n      There is no dot.
 *
 * @pre @p leaf is non-NULL and holds at least @p n characters.
 * @pre @p n is at most ::k_lfn_write_max.
 * @post @p leaf is not modified.
 * @post The scan visits exactly @p n characters.
 *
 * @note Pure function; trivially thread-safe.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static uint32_t priv_name_dot_index(const char* leaf, uint32_t n)
{
  uint32_t dot = n;
  for (uint32_t i = 0U; i < n; i++) {
    if (leaf[i] == '.') {
      dot = i;
    }
  }
  return dot;
}

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
 * @param[in] leaf Leaf component (no slashes).
 * @param[in] n    Length of @p leaf in characters.
 *
 * @return Representability flag.
 * @retval 1U @p leaf packs to 8.3 losslessly, up to case.
 * @retval 0U @p leaf needs a long-name chain.
 *
 * @pre @p leaf is non-NULL and holds at least @p n characters.
 * @pre @p n is non-zero and at most ::k_lfn_write_max.
 * @post @p leaf is not modified.
 * @post The verdict is independent of the volume's contents.
 *
 * @note Pure function; trivially thread-safe.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static uint8_t priv_name_is_83(const char* leaf, uint32_t n)
{
  const uint32_t dot      = priv_name_dot_index(leaf, n);
  const uint32_t base_len = dot;
  if ((base_len == 0U) || (base_len > (uint32_t)k_filename_base_len)) {
    return 0U;
  }
  if (dot < n) {
    const uint32_t ext_len = (n - dot) - 1U;
    if ((ext_len == 0U) || (ext_len > (uint32_t)k_filename_ext_len)) {
      return 0U;
    }
  }
  for (uint32_t i = 0U; i < n; i++) {
    if (i == dot) {
      continue;
    }
    if (priv_char_is_83_legal(leaf[i]) == 0U) {
      return 0U;
    }
  }
  return 1U;
}

/**
 * @brief Fold one half of a name into "has upper" / "has lower" observations.
 *
 * @details Walks @p leaf from @p from up to (not including) @p to and ORs a bit
 *          into @p seen for each case class present. Digits and punctuation set
 *          neither, so `"IMG_01"` is upper-only and `"01"` is neither, which is
 *          what makes a case-flagged short name possible for both.
 *
 * @param[in]     leaf Leaf component.
 * @param[in]     from First index to inspect.
 * @param[in]     to   One past the last index to inspect.
 * @param[in,out] seen Bit 0 set when an upper-case letter is seen, bit 1 when
 *                     a lower-case one is.
 *
 * @return Nothing.
 *
 * @pre @p leaf is non-NULL and holds at least @p to characters.
 * @pre @p from is at most @p to and @p seen is non-NULL.
 * @post @p seen has only bits it already had, plus the ones observed here.
 * @post @p leaf is not modified.
 *
 * @note Pure accumulation; trivially thread-safe against distinct @p seen.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static void priv_case_observe(const char* leaf, uint32_t from, uint32_t to, uint8_t* seen)
{
  for (uint32_t i = from; i < to; i++) {
    const char c = leaf[i];
    if ((c >= 'A') && (c <= 'Z')) {
      *seen |= (uint8_t)k_case_seen_upper;
    } else if ((c >= 'a') && (c <= 'z')) {
      *seen |= (uint8_t)k_case_seen_lower;
    } else {
      /* Digits and punctuation carry no case; they constrain nothing. */
    }
  }
}

/**
 * @brief Decide the case shape of an 8.3-representable leaf.
 *
 * @details A half of the name that is all one case round-trips through a
 *          `DIR_NTRes` bit; a half that mixes cases does not, and the whole
 *          name then needs a long-name chain to survive. The two halves are
 *          judged independently, exactly as the two flag bits are.
 *
 * @param[in]  leaf       Leaf component, already known to pack to 8.3.
 * @param[in]  n          Length of @p leaf in characters.
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
RA8_INTERNAL
static ra8_fs_name_kind_t priv_name_case_kind(const char* leaf, uint32_t n, uint8_t* out_ntres)
{
  const uint32_t dot       = priv_name_dot_index(leaf, n);
  uint8_t        base_seen = 0U;
  uint8_t        ext_seen  = 0U;
  priv_case_observe(leaf, 0U, dot, &base_seen);
  if (dot < n) {
    priv_case_observe(leaf, dot + 1U, n, &ext_seen);
  }
  *out_ntres = 0U;
  if (base_seen == (uint8_t)k_case_seen_mixed) {
    return k_name_kind_long;
  }
  if (ext_seen == (uint8_t)k_case_seen_mixed) {
    return k_name_kind_long;
  }
  if (base_seen == (uint8_t)k_case_seen_lower) {
    *out_ntres |= (uint8_t)k_ntres_base_lower;
  }
  if (ext_seen == (uint8_t)k_case_seen_lower) {
    *out_ntres |= (uint8_t)k_ntres_ext_lower;
  }
  return k_name_kind_short;
}

/* `priv_name_classify()`: see header for the documented contract. */
ra8_fs_name_kind_t priv_name_classify(const char* leaf, uint8_t* out83, uint8_t* out_ntres)
{
  if ((leaf == nullptr) || (out83 == nullptr) || (out_ntres == nullptr)) {
    return k_name_kind_invalid;
  }
  *out_ntres       = 0U;
  const uint32_t n = priv_strlen(leaf);
  if ((n == 0U) || (n > (uint32_t)k_lfn_write_max)) {
    return k_name_kind_invalid;
  }
  for (uint32_t i = 0U; i < n; i++) {
    if (priv_char_is_lfn_legal(leaf[i]) == 0U) {
      return k_name_kind_invalid;
    }
  }
  if (priv_name_is_83(leaf, n) == 0U) {
    return k_name_kind_long;
  }
  if (priv_path_to_83(leaf, out83) == 0U) {
    /* Unreachable: priv_name_is_83() already proved the base and extension
     * lengths priv_path_to_83() checks. Kept because the two functions are
     * separately maintained and a silent disagreement would create an
     * unpacked name. */
    return k_name_kind_long; /* GCOVR_EXCL_LINE */
  }
  return priv_name_case_kind(leaf, n, out_ntres);
}

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
 * @param[in] c Character from the caller's long name.
 *
 * @return The character to place in the alias.
 * @retval '_' @p c is not legal in an 8.3 name.
 * @retval other The upper-cased @p c.
 *
 * @pre @p c passed ::priv_char_is_lfn_legal().
 * @pre The caller has already discarded spaces and dots.
 * @post No state is modified.
 * @post The result is always legal in a packed 8.3 name.
 *
 * @note Pure function; trivially thread-safe.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static uint8_t priv_alias_map_char(char c)
{
  const char up = priv_to_upper(c);
  if (priv_char_is_83_legal(up) == 0U) {
    return (uint8_t)'_';
  }
  return (uint8_t)up;
}

/**
 * @brief Collect up to @p cap alias characters from @p from up to @p to.
 *
 * @details Spaces and dots are dropped rather than mapped -- the FAT
 *          specification removes them before translating -- so `My Report.v2`
 *          contributes `MYREPORT` and not `MY_REPORT`.
 *
 * @param[in]  leaf Leaf component.
 * @param[in]  from First index to read.
 * @param[in]  to   One past the last index to read.
 * @param[in]  cap  Maximum characters to emit.
 * @param[out] out  Receives up to @p cap mapped characters.
 *
 * @return Number of characters written.
 * @retval 0..cap How many of @p out were filled.
 *
 * @pre @p leaf and @p out are non-NULL; @p out holds @p cap bytes.
 * @pre @p from is at most @p to.
 * @post Exactly the returned count of @p out bytes were written.
 * @post @p leaf is not modified.
 *
 * @note Pure function; trivially thread-safe against distinct @p out.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static uint32_t
priv_alias_collect(const char* leaf, uint32_t from, uint32_t to, uint32_t cap, uint8_t* out)
{
  uint32_t got = 0U;
  for (uint32_t i = from; (i < to) && (got < cap); i++) {
    const char c = leaf[i];
    if ((c == ' ') || (c == '.')) {
      continue;
    }
    out[got] = priv_alias_map_char(c);
    got++;
  }
  return got;
}

/**
 * @brief Index of the dot that separates the alias extension, or @p n if none.
 *
 * @details The LAST dot, and only when it has at least one character on each
 *          side: a leading dot (`.profile`) names no extension, and a trailing
 *          one (`notes.`) supplies no characters for it.
 *
 * @param[in] leaf Leaf component.
 * @param[in] n    Length of @p leaf.
 *
 * @return Separator index, or @p n when the alias has no extension.
 * @retval 1..n-2 The separating dot.
 * @retval n      No usable extension.
 *
 * @pre @p leaf is non-NULL and holds at least @p n characters.
 * @pre @p n is non-zero.
 * @post @p leaf is not modified.
 * @post The result is either @p n or a valid interior index.
 *
 * @note Pure function; trivially thread-safe.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static uint32_t priv_alias_ext_dot(const char* leaf, uint32_t n)
{
  uint32_t dot = n;
  for (uint32_t i = 1U; (i + 1U) < n; i++) {
    if (leaf[i] == '.') {
      dot = i;
    }
  }
  return dot;
}

/**
 * @brief Decimal digit count of @p tail (1..::k_lfn_alias_tail_max).
 *
 * @details Drives how much of the basis name survives: the numeric tail and
 *          its `~` must fit inside the eight-character base field, so `~1`
 *          leaves six characters and `~999999` leaves one.
 *
 * @param[in] tail Alias sequence number.
 *
 * @return Number of decimal digits.
 * @retval 1..6 Digit count for the supported range.
 *
 * @pre @p tail is at least 1.
 * @pre @p tail is at most ::k_lfn_alias_tail_max.
 * @post No state is modified.
 * @post The result is at most `k_filename_base_len - 2`.
 *
 * @note Pure function; trivially thread-safe.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static uint32_t priv_alias_digits(uint32_t tail)
{
  uint32_t digits = 1U;
  uint32_t scale  = (uint32_t)k_alias_radix;
  /* Bounded by the six-digit cap the alias probe never exceeds. */
  while ((digits < (uint32_t)k_alias_digits_max) && (tail >= scale)) {
    digits++;
    scale *= (uint32_t)k_alias_radix;
  }
  return digits;
}

/* `priv_lfn_alias_basis()`: see header for the documented contract. */
void priv_lfn_alias_basis(const char* leaf, uint32_t tail, uint8_t* out11)
{
  for (uint32_t i = 0U; i < (uint32_t)k_max_8_3_name; i++) {
    out11[i] = ' ';
  }
  const uint32_t n                         = priv_strlen(leaf);
  const uint32_t dot                       = priv_alias_ext_dot(leaf, n);
  uint8_t        base[k_filename_base_len] = {};
  uint32_t       base_len = priv_alias_collect(leaf, 0U, dot, (uint32_t)k_filename_base_len, base);
  if (base_len == 0U) {
    base[0]  = (uint8_t)'_'; /* a name of nothing but dots and spaces */
    base_len = 1U;
  }
  if (dot < n) {
    (void)priv_alias_collect(leaf,
                             dot + 1U,
                             n,
                             (uint32_t)k_filename_ext_len,
                             &out11[k_filename_base_len]);
  }
  const uint32_t digits = priv_alias_digits(tail);
  uint32_t       keep   = ((uint32_t)k_filename_base_len - digits) - 1U;
  if (base_len < keep) {
    keep = base_len;
  }
  for (uint32_t i = 0U; i < keep; i++) {
    out11[i] = base[i];
  }
  out11[keep]   = (uint8_t)'~';
  uint32_t rest = tail;
  for (uint32_t d = 0U; d < digits; d++) {
    const uint32_t digit       = rest % (uint32_t)k_alias_radix;
    out11[(keep + digits) - d] = (uint8_t)((uint32_t)'0' + digit);
    rest /= (uint32_t)k_alias_radix;
  }
  /* No kanji escape is needed here: every byte written above is an upper-case
   * ASCII letter, a digit, '_', '~' or a space, and none of those is 0xE5. */
}

/* =============================================================================
 * Directory walking
 * =============================================================================
 */

/**
 * @brief Initialise a walker that iterates the volume root directory.
 *
 * @details FAT12/16 use a fixed root region; FAT32 uses a cluster
 *          chain rooted at `m->root_cluster`.
 *
 * @param[in]  m Mount providing geometry and FAT type.
 * @param[out] w Walker cursor to initialise.
 *
 * @pre `m` and `w` are non-NULL.
 * @pre `m` has been fully populated by `priv_compute_geometry`.
 * @post `w` points at the first sector of the root directory.
 * @post `w->entry_idx` is zero.
 *
 * @note Pure init -- does not touch the backend.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static void priv_dir_walk_init_root(const ra8_fs_mount_t* m, dir_walk_t* w)
{
  if (m->type == k_ra8_fs_type_fat32) {
    w->is_root_fixed     = 0;
    w->fixed_remaining   = 0;
    w->cluster           = m->root_cluster;
    w->sector_in_cluster = 0;
    w->cur_lba           = priv_cluster_to_lba(m, w->cluster);
  } else {
    const uint32_t root_dir_sectors =
      ((m->root_entries * k_ra8_fs_dir_entry_bytes) + (k_ra8_fs_bytes_per_sector - 1U)) /
      k_ra8_fs_bytes_per_sector;
    w->is_root_fixed     = 1;
    w->fixed_remaining   = root_dir_sectors;
    w->cluster           = 0;
    w->sector_in_cluster = 0;
    w->cur_lba           = m->first_root_lba;
  }
  w->entry_idx    = 0;
  w->cluster_hops = 0;
}

/* `priv_dir_walk_init_loc()`: see header for the documented contract. */
void priv_dir_walk_init_loc(const ra8_fs_mount_t* m, const dir_loc_t* loc, dir_walk_t* w)
{
  if (loc->is_root != 0U) {
    priv_dir_walk_init_root(m, w);
    return;
  }
  w->is_root_fixed     = 0;
  w->fixed_remaining   = 0;
  w->cluster           = loc->cluster;
  w->sector_in_cluster = 0;
  w->cur_lba           = priv_cluster_to_lba(m, loc->cluster);
  w->entry_idx         = 0;
  w->cluster_hops      = 0;
}

/* `priv_dir_walk_next_sector()`: see header for the documented contract. */
ra8_err_t priv_dir_walk_next_sector(const ra8_fs_mount_t* m, dir_walk_t* w, uint8_t* out_eod)
{
  *out_eod = 0;
  if (w->is_root_fixed != 0U) {
    if (w->fixed_remaining <= 1U) {
      *out_eod = 1;
      return k_ra8_ok;
    }
    w->fixed_remaining--;
    w->cur_lba++;
    w->entry_idx = 0;
    return k_ra8_ok;
  }
  /* FAT32 cluster-chain root. */
  w->sector_in_cluster++;
  if (w->sector_in_cluster >= m->sectors_per_cluster) {
    uint32_t  next = 0;
    ra8_err_t err  = priv_fat_get(m, w->cluster, &next);
    if (err != k_ra8_ok) {
      return err;
    }
    if (priv_is_eoc(m, next) != 0U) {
      *out_eod = 1;
      return k_ra8_ok;
    }
    /* Cycle guard (NASA Rule 2): a healthy chain visits at most
     * count_of_clusters distinct clusters; more means a corrupt loop. */
    w->cluster_hops++;
    if (w->cluster_hops > m->count_of_clusters) {
      return k_ra8_err_protocol_error;
    }
    w->cluster           = next;
    w->sector_in_cluster = 0;
    w->cur_lba           = priv_cluster_to_lba(m, w->cluster);
  } else {
    w->cur_lba++;
  }
  w->entry_idx = 0;
  return k_ra8_ok;
}

/* `priv_dir_find()`: see header for the documented contract. */
ra8_err_t priv_dir_find(const ra8_fs_mount_t* m,
                        const dir_loc_t*      loc,
                        const uint8_t*        name83,
                        uint32_t*             out_lba,
                        uint32_t*             out_entry_off,
                        uint8_t               out_entry[k_ra8_fs_dir_entry_bytes])
{
  dir_walk_t w = {};
  priv_dir_walk_init_loc(m, loc, &w);
  uint8_t eod                            = 0;
  uint8_t buf[k_ra8_fs_bytes_per_sector] = {};
  while (eod == 0U) {
    ra8_err_t err = priv_read_sector(m, w.cur_lba, buf);
    if (err != k_ra8_ok) {
      return err;
    }
    for (uint32_t e = 0; e < k_dir_entries_per_sector; e++) {
      uint8_t* ent = &buf[(size_t)e * (size_t)k_ra8_fs_dir_entry_bytes];
      if (ent[k_dir_off_name] == k_dir_marker_free_perm) {
        return k_ra8_err_not_found;
      }
      if (ent[k_dir_off_name] == k_dir_marker_free_used) {
        continue;
      }
      if (ent[k_dir_off_attr] == k_ra8_fs_attr_lfn) {
        continue;
      }
      if (priv_byte_equal(ent, name83, k_dir_name_field_len) != 0U) {
        *out_lba       = w.cur_lba;
        *out_entry_off = e * (uint32_t)k_ra8_fs_dir_entry_bytes;
        priv_byte_copy(out_entry, ent, k_ra8_fs_dir_entry_bytes);
        return k_ra8_ok;
      }
    }
    err = priv_dir_walk_next_sector(m, &w, &eod);
    if (err != k_ra8_ok) {
      return err;
    }
  }
  return k_ra8_err_not_found;
}
