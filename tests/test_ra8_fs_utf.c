/**
 * @file test_ra8_fs_utf.c
 * @brief The UTF-8 <-> UTF-16LE seam, and what it refuses (#606).
 *
 * @details
 * `ra8_fs` used to mangle a non-ASCII name three different ways depending on
 * which path it took, and every one of them was silent. All three now go
 * through `libs/ra8_fs/src/ra8_fs_utf.c`, so this suite is where that one
 * conversion is held to the encoding rather than to the driver's convenience.
 *
 * Two halves, and the second is the load-bearing one:
 *
 *  1. **What must round-trip.** One-, two-, three- and four-byte sequences, the
 *     last of which becomes a surrogate PAIR on disk, so a supplementary
 *     character costs two UTF-16 units and the same four UTF-8 bytes coming
 *     back. Every vector asserts the UNIT COUNT as well as the bytes, because
 *     the unit count is what `NameLength` and the VFAT group count are.
 *
 *  2. **What must be refused, loudly.** A decoder that only assembles bits
 *     accepts an over-long encoding, a surrogate spelled in UTF-8, a truncated
 *     sequence and a five-byte lead. Each is a second spelling of something, or
 *     nothing at all, and accepting any of them is how a name stops
 *     round-tripping. There is no substitution character anywhere in the module
 *     under test, and this suite would notice one: it asserts the error code,
 *     not just that something went wrong.
 *
 * @par The up-case table:
 * The fold is asserted against externally checkable Unicode facts -- U+00E9
 * up-cases to U+00C9, U+03B1 to U+0391 -- rather than against the table this
 * tree embeds, so agreeing with our own bytes is not what makes the case pass.
 * The stronger statement, that ::priv_exfat_upcase_unit matches the table a
 * FORMATTED VOLUME carries across the whole BMP, lives in
 * `tests/test_ra8_fs_unicode_names.c`, which has a volume to read it from.
 *
 * @par Pure-ASCII sources:
 * Every non-ASCII vector is built from byte escapes or `uint16_t` literals.
 * Nothing in this file is a non-ASCII string literal, which the tree's encoding
 * gate would reject and which would make the vectors depend on the editor that
 * saved them.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_fs.h"
#include "ra8_fs_fat_internal.h"
#include "unity_minimal.h"

/**
 * @enum utf_test_val_t
 * @brief Buffer sizes and the code points these vectors pin.
 *
 * @details The code points are chosen one per UTF-8 length, and each is a
 *          character a real file name plausibly holds: an accented Latin
 *          letter, a CJK ideograph, an emoji.
 *
 * @invariant Each `k_ut_cp_*` needs exactly the length its name states.
 * @see test_decode_lengths()
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ut_units         = 32U,      /**< Units in a vector's scratch buffer.         */
  k_ut_bytes         = 128U,     /**< Bytes in a vector's scratch buffer.         */
  k_ut_cp_1          = 0x41U,    /**< 'A': one UTF-8 byte, one unit.              */
  k_ut_cp_2          = 0x00E9U,  /**< U+00E9 e-acute: two bytes, one unit.        */
  k_ut_cp_3          = 0x4F60U,  /**< U+4F60 CJK: three bytes, one unit.          */
  k_ut_cp_4          = 0x1F600U, /**< U+1F600 emoji: four bytes, TWO units.       */
  k_ut_cp_above_surr = 0xE000U,  /**< First code point above the surrogate block. */
  k_ut_unit_above_lo = 0xE001U,  /**< A non-surrogate unit above 0xDFFF.          */
  k_ut_sur_hi_of_4   = 0xD83DU,  /**< High surrogate of ::k_ut_cp_4.              */
  k_ut_sur_lo_of_4   = 0xDE00U,  /**< Low surrogate of ::k_ut_cp_4.               */
  k_ut_up_lower_e    = 0x00E9U,  /**< U+00E9, whose up-case is U+00C9.            */
  k_ut_up_upper_e    = 0x00C9U,  /**< U+00C9 E-acute.                             */
  k_ut_up_alpha      = 0x03B1U,  /**< U+03B1 Greek small alpha.                   */
  k_ut_up_capalpha   = 0x0391U,  /**< U+0391 Greek capital alpha.                 */
  k_ut_up_cyr_a      = 0x0430U,  /**< U+0430 Cyrillic small a.                    */
  k_ut_up_cyr_cap    = 0x0410U,  /**< U+0410 Cyrillic capital A.                  */
  k_ut_up_fw_a       = 0xFF41U,  /**< U+FF41 fullwidth small a.                   */
  k_ut_up_fw_cap     = 0xFF21U,  /**< U+FF21 fullwidth capital A.                 */
  k_ut_up_sharp_s    = 0x00DFU,  /**< U+00DF: no single-unit upper case exists.   */
  k_ut_up_last       = 0xFFFFU,  /**< U+FFFF: the table's dangling trailing tag.  */
  k_ut_ascii_count   = 128U,     /**< Code units in the ASCII fast path.          */
  k_ut_case_delta    = 32U,      /**< 'a' - 'A'.                                  */
  k_ut_chunk_mapped  = 0x0100U,  /**< A unit whose high byte is set.              */
  k_ut_chunk_lowbyte = 0x0041U,  /**< Shares only its low byte with the above.    */
  k_ut_poison        = 7U,       /**< A non-zero seed a failure must clear.       */
  k_ut_mixed_units   = 14U,      /**< UTF-16 units in the mixed-script vector.    */
} utf_test_val_t;

/* ===========================================================================
 * Helpers
 * ===========================================================================
 */

/**
 * @brief Decode @p in and assert it produced exactly @p want_units units.
 *
 * @param[in]  in         NUL-terminated UTF-8 vector.
 * @param[in]  want_units Expected UTF-16 unit count.
 * @param[out] out        Receives the units.
 *
 * @return Nothing; a mismatch is reported by the harness.
 *
 * @pre @p in and @p out are non-NULL; @p out holds ::k_ut_units units.
 * @pre @p in is expected to be well-formed UTF-8.
 * @post @p out holds @p want_units units.
 * @post No state outside @p out is modified.
 *
 * @note Not thread-safe (uses the harness's failure counter).
 * @since 0.1.0 @details Implements the bounded decode ok fixture step using caller-owned state.
 */
RA8_INTERNAL static void internal_decode_ok(const char* in, uint32_t want_units, uint16_t* out)
{
  uint32_t n = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, priv_utf8_to_utf16(in, out, (uint32_t)k_ut_units, &n));
  TEST_ASSERT_EQ(want_units, n);
}

/**
 * @brief Assert that @p in is refused with ::k_ra8_err_invalid_arg.
 *
 * @details The code matters, not merely the failure: `k_ra8_err_no_mem` would
 *          mean "too long", which is a different answer a caller may retry.
 *
 * @param[in] in NUL-terminated byte vector that is not valid UTF-8.
 *
 * @return Nothing; a mismatch is reported by the harness.
 *
 * @pre @p in is non-NULL.
 * @pre @p in is expected to be malformed.
 * @post The unit count was reset to zero, so a caller ignoring the code sees
 *       an empty name rather than a truncated one.
 * @post No state outside the local buffer is modified.
 *
 * @note Not thread-safe (uses the harness's failure counter).
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_decode_rejects(const char* in)
{
  uint16_t units[k_ut_units] = {};
  uint32_t n                 = (uint32_t)k_ut_poison; /* cleared on failure */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, priv_utf8_to_utf16(in, units, (uint32_t)k_ut_units, &n));
  TEST_ASSERT_EQ(0U, n);
}

/* ===========================================================================
 * Tests: what round-trips
 * ===========================================================================
 */

/**
 * @test test_decode_lengths
 * @brief One vector per UTF-8 sequence length, byte-for-byte both ways.
 *
 * @par MC/DC:
 * Decision: the four-arm lead-byte ladder in
 * `libs/ra8_fs/src/ra8_fs_utf.c@priv_utf8_lead` (4 conditions, one per arm).
 * - V1: 'A'  (0x41)          -> the ASCII arm      -> len 1.
 * - V2: U+00E9 (0xC3 0xA9)   -> the two-byte arm   -> len 2.
 * - V3: U+4F60 (0xE4 0xBD 0xA0) -> the three-byte arm -> len 3.
 * - V4: U+1F600 (0xF0 0x9F 0x98 0x80) -> the four-byte arm -> len 4.
 * Each vector takes exactly one arm's true branch and the false branch of every
 * arm before it, which is what makes the four independent.
 * Decision: `(cp >= k_utf_min_4byte)` in `priv_utf16_put` (1 condition).
 * - V4 above -> true  -> a surrogate PAIR, two units.
 * - V1..V3   -> false -> one unit.
 *
 * @pre None; the functions under test touch no volume.
 * @post Every vector produced the documented unit count and came back equal.
 *
 * @since 0.1.0 @details Runs the decode lengths vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_decode_lengths(void)
{
  TEST_BEGIN("ra8_fs utf: one vector per UTF-8 sequence length");
  uint16_t units[k_ut_units] = {};
  char     back[k_ut_bytes]  = {};

  static const char one[]   = {'A', '\0'};
  static const char two[]   = {(char)(unsigned char)0xC3U, (char)(unsigned char)0xA9U, '\0'};
  static const char three[] = {(char)(unsigned char)0xE4U,
                               (char)(unsigned char)0xBDU,
                               (char)(unsigned char)0xA0U,
                               '\0'};
  static const char four[]  = {(char)(unsigned char)0xF0U,
                               (char)(unsigned char)0x9FU,
                               (char)(unsigned char)0x98U,
                               (char)(unsigned char)0x80U,
                               '\0'};

  internal_decode_ok(one, 1U, units); /* V1 */
  TEST_ASSERT_EQ(k_ut_cp_1, units[0]);
  TEST_ASSERT_EQ(k_ra8_ok, priv_utf16_to_utf8(units, 1U, back, (uint32_t)k_ut_bytes));
  TEST_ASSERT_EQ(0, strcmp(back, one));

  internal_decode_ok(two, 1U, units); /* V2 */
  TEST_ASSERT_EQ(k_ut_cp_2, units[0]);
  TEST_ASSERT_EQ(k_ra8_ok, priv_utf16_to_utf8(units, 1U, back, (uint32_t)k_ut_bytes));
  TEST_ASSERT_EQ(0, strcmp(back, two));

  internal_decode_ok(three, 1U, units); /* V3 */
  TEST_ASSERT_EQ(k_ut_cp_3, units[0]);
  TEST_ASSERT_EQ(k_ra8_ok, priv_utf16_to_utf8(units, 1U, back, (uint32_t)k_ut_bytes));
  TEST_ASSERT_EQ(0, strcmp(back, three));

  /* V4: two units, and the pair halves are the ones the standard names. */
  internal_decode_ok(four, 2U, units);
  TEST_ASSERT_EQ(k_ut_sur_hi_of_4, units[0]);
  TEST_ASSERT_EQ(k_ut_sur_lo_of_4, units[1]);
  TEST_ASSERT_EQ(k_ra8_ok, priv_utf16_to_utf8(units, 2U, back, (uint32_t)k_ut_bytes));
  TEST_ASSERT_EQ(0, strcmp(back, four));

  /* V5: U+E000, the first code point ABOVE the surrogate block. It is the
   * partner vector the surrogate-range decisions need: on decode it makes
   * `priv_utf8_wellformed`'s `cp <= 0xDFFF` false while `cp >= 0xD800` is true
   * (well-formed), and on encode it makes `priv_utf16_take`'s `hi > 0xDFFF` true
   * while `hi < 0xD800` is false (a BMP unit, not a surrogate). Both are the
   * second-condition-varies-alone case the other vectors never reach. */
  static const char e000[] = {(char)(unsigned char)0xEEU,
                              (char)(unsigned char)0x80U,
                              (char)(unsigned char)0x80U,
                              '\0'};
  internal_decode_ok(e000, 1U, units);
  TEST_ASSERT_EQ(k_ut_cp_above_surr, units[0]);
  TEST_ASSERT_EQ(k_ra8_ok, priv_utf16_to_utf8(units, 1U, back, (uint32_t)k_ut_bytes));
  TEST_ASSERT_EQ(0, strcmp(back, e000));

  TEST_END("ra8_fs utf: one vector per UTF-8 sequence length");
}

/**
 * @test test_mixed_name_round_trip
 * @brief A realistic mixed name survives both directions unchanged.
 *
 * @details The single statement the whole feature rests on: a name a person
 *          would actually type goes in as UTF-8, becomes the units a directory
 *          entry stores, and comes back as the same bytes. The vector mixes all
 *          four sequence lengths so a length-specific bug cannot hide behind
 *          the others.
 *
 * @par MC/DC:
 * No new decision; this is the composition of the ones
 * ::test_decode_lengths() isolates.
 *
 * @pre None; the functions under test touch no volume.
 * @post The name came back byte-identical, at the documented unit count.
 *
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_mixed_name_round_trip(void)
{
  TEST_BEGIN("ra8_fs utf: a mixed-script name survives both directions");
  /* "Caf" U+00E9 " " U+4F60 U+597D " " U+1F600 ".txt" -- 4 + 1 + 1 + 2 + 1 + 2
   * + 4 = one-, two-, three- and four-byte forms in one name. */
  static const char name[]            = {'C',
                                         'a',
                                         'f',
                                         (char)(unsigned char)0xC3U,
                                         (char)(unsigned char)0xA9U,
                                         ' ',
                                         (char)(unsigned char)0xE4U,
                                         (char)(unsigned char)0xBDU,
                                         (char)(unsigned char)0xA0U,
                                         (char)(unsigned char)0xE5U,
                                         (char)(unsigned char)0xA5U,
                                         (char)(unsigned char)0xBDU,
                                         ' ',
                                         (char)(unsigned char)0xF0U,
                                         (char)(unsigned char)0x9FU,
                                         (char)(unsigned char)0x98U,
                                         (char)(unsigned char)0x80U,
                                         '.',
                                         't',
                                         'x',
                                         't',
                                         '\0'};
  uint16_t          units[k_ut_units] = {};
  char              back[k_ut_bytes]  = {};
  /* C,a,f,e-acute,space,ni,hao,space,(surrogate pair),.,t,x,t = 14 units. */
  internal_decode_ok(name, (uint32_t)k_ut_mixed_units, units);
  TEST_ASSERT_EQ(k_ra8_ok,
                 priv_utf16_to_utf8(units, (uint32_t)k_ut_mixed_units, back, (uint32_t)k_ut_bytes));
  TEST_ASSERT_EQ(0, strcmp(back, name));
  TEST_END("ra8_fs utf: a mixed-script name survives both directions");
}

/* ===========================================================================
 * Tests: what is refused
 * ===========================================================================
 */

/**
 * @test test_decode_rejects_malformed
 * @brief Every malformed-input class is an error, never a substitution.
 *
 * @par MC/DC:
 * Decision: `if (priv_utf8_lead(...) == 0U)` in `priv_utf8_next` (1 condition).
 * - V1: a bare continuation byte 0x80 -> true -> invalid_arg.
 * - V2: a five-byte lead 0xF8         -> true -> invalid_arg.
 * - The false arm is every well-formed vector in this file.
 * Decision: `if (priv_utf8_tail(...) == 0U)` in the same function (1 cond).
 * - V3: 0xC3 with nothing after it (the NUL ends the string) -> true.
 * - V4: 0xE4 0xBD, one byte short   -> true.
 * - V5: 0xC3 0x41, a lead where a continuation belongs -> true.
 * Decision: the surrogate-range test
 * `if ((cp >= k_utf_sur_hi_first) && (cp <= k_utf_sur_last))` and the two
 * over-long tests around it, in
 * `libs/ra8_fs/src/ra8_fs_utf.c@internal_utf8_wellformed` (3 conditions).
 * - V6: 0xC0 0x80        -> over-long two-byte form of U+0000.
 * - V7: 0xE0 0x80 0x80   -> over-long three-byte form of U+0000.
 * - V8: 0xED 0xA0 0x80   -> U+D800, a surrogate spelled in UTF-8 (CESU-8).
 * - V9: 0xF0 0x80 0x80 0x80 -> over-long four-byte form.
 * - V10: 0xF4 0x90 0x80 0x80 -> U+110000, past the last code point.
 * Each takes one arm's true branch; the false arm of all three is every
 * well-formed vector.
 *
 * @pre None; the functions under test touch no volume.
 * @post Every class reported `k_ra8_err_invalid_arg` and produced no units.
 *
 * @since 0.1.0 @details Runs the decode rejects malformed vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_decode_rejects_malformed(void)
{
  TEST_BEGIN("ra8_fs utf: every malformed class is refused, not patched");
  static const char cont_only[] = {(char)(unsigned char)0x80U, '\0'}; /* V1 */
  static const char five_byte[] = {(char)(unsigned char)0xF8U,
                                   (char)(unsigned char)0x88U,
                                   (char)(unsigned char)0x80U,
                                   (char)(unsigned char)0x80U,
                                   (char)(unsigned char)0x80U,
                                   '\0'};                             /* V2 */
  static const char trunc2[]    = {(char)(unsigned char)0xC3U, '\0'}; /* V3 */
  static const char trunc3[]    = {(char)(unsigned char)0xE4U,
                                   (char)(unsigned char)0xBDU,
                                   '\0'};                                  /* V4 */
  static const char bad_cont[]  = {(char)(unsigned char)0xC3U, 'A', '\0'}; /* V5 */
  static const char overlong2[] = {(char)(unsigned char)0xC0U,
                                   (char)(unsigned char)0x80U,
                                   '\0'}; /* V6 */
  static const char overlong3[] = {(char)(unsigned char)0xE0U,
                                   (char)(unsigned char)0x80U,
                                   (char)(unsigned char)0x80U,
                                   '\0'}; /* V7 */
  static const char surrogate[] = {(char)(unsigned char)0xEDU,
                                   (char)(unsigned char)0xA0U,
                                   (char)(unsigned char)0x80U,
                                   '\0'}; /* V8 */
  static const char overlong4[] = {(char)(unsigned char)0xF0U,
                                   (char)(unsigned char)0x80U,
                                   (char)(unsigned char)0x80U,
                                   (char)(unsigned char)0x80U,
                                   '\0'}; /* V9 */
  static const char past_max[]  = {(char)(unsigned char)0xF4U,
                                   (char)(unsigned char)0x90U,
                                   (char)(unsigned char)0x80U,
                                   (char)(unsigned char)0x80U,
                                   '\0'}; /* V10 */

  internal_decode_rejects(cont_only);
  internal_decode_rejects(five_byte);
  internal_decode_rejects(trunc2);
  internal_decode_rejects(trunc3);
  internal_decode_rejects(bad_cont);
  internal_decode_rejects(overlong2);
  internal_decode_rejects(overlong3);
  internal_decode_rejects(surrogate);
  internal_decode_rejects(overlong4);
  internal_decode_rejects(past_max);

  /* The negative control: the LAST legal code point of the four-byte form, one
   * below the first rejected one, is accepted. Without it the ladder above
   * would pass just as well with the bound off by one in the safe direction. */
  static const char max_ok[]          = {(char)(unsigned char)0xF4U,
                                         (char)(unsigned char)0x8FU,
                                         (char)(unsigned char)0xBFU,
                                         (char)(unsigned char)0xBFU,
                                         '\0'};
  uint16_t          units[k_ut_units] = {};
  internal_decode_ok(max_ok, 2U, units); /* U+10FFFF */
  TEST_END("ra8_fs utf: every malformed class is refused, not patched");
}

/**
 * @test test_encode_rejects_unpaired_surrogate
 * @brief Units no UTF-8 string encodes are reported, not replaced.
 *
 * @details The one direction that can legitimately be handed something
 *          impossible: those units already exist on somebody's volume. The
 *          answer is an error and an EMPTY output, which is what lets the
 *          listing paths fall back to a name that does re-open the file
 *          instead of reporting one that does not.
 *
 * @par MC/DC:
 * Decision: the three-arm refusal ladder in
 * `libs/ra8_fs/src/ra8_fs_utf.c@internal_utf16_take` (3 conditions).
 * - V1: a LOW surrogate first          -> the "no high one before it" arm.
 * - V2: a HIGH surrogate at the end    -> the "nothing after it" arm.
 * - V3: a high surrogate then 'A'      -> the "followed by something else" arm.
 * - V4: a correct pair                 -> false on all three -> one character.
 * V4 paired with each of V1..V3 shows that arm independently decides.
 *
 * @pre None; the function under test touches no volume.
 * @post Each refusal returned `k_ra8_err_invalid_arg` and left an empty string.
 *
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_encode_rejects_unpaired_surrogate(void)
{
  TEST_BEGIN("ra8_fs utf: an unpaired surrogate is reported, not replaced");
  char           out[k_ut_bytes] = {};
  const uint16_t lone_low[1]     = {(uint16_t)k_ut_sur_lo_of_4};
  const uint16_t lone_high[1]    = {(uint16_t)k_ut_sur_hi_of_4};
  const uint16_t high_then_a[2]  = {(uint16_t)k_ut_sur_hi_of_4, (uint16_t)'A'};
  const uint16_t high_then_hi[2] = {(uint16_t)k_ut_sur_hi_of_4, (uint16_t)k_ut_unit_above_lo};
  const uint16_t proper_pair[2]  = {(uint16_t)k_ut_sur_hi_of_4, (uint16_t)k_ut_sur_lo_of_4};

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 priv_utf16_to_utf8(lone_low, 1U, out, (uint32_t)k_ut_bytes)); /* V1 */
  TEST_ASSERT_EQ('\0', out[0]);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 priv_utf16_to_utf8(lone_high, 1U, out, (uint32_t)k_ut_bytes)); /* V2 */
  TEST_ASSERT_EQ('\0', out[0]);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 priv_utf16_to_utf8(high_then_a, 2U, out, (uint32_t)k_ut_bytes)); /* V3 */
  TEST_ASSERT_EQ('\0', out[0]);
  /* V3b: a high surrogate followed by a unit ABOVE 0xDFFF -- the low-unit
   * decision's `lo > 0xDFFF` true, `lo < 0xDC00` false, which V3 (lo = 'A')
   * cannot reach. Still an unpaired surrogate, still refused. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 priv_utf16_to_utf8(high_then_hi, 2U, out, (uint32_t)k_ut_bytes));
  TEST_ASSERT_EQ('\0', out[0]);
  TEST_ASSERT_EQ(k_ra8_ok, priv_utf16_to_utf8(proper_pair, 2U, out, (uint32_t)k_ut_bytes)); /* V4 */
  TEST_ASSERT_EQ(4U, strlen(out));
  TEST_END("ra8_fs utf: an unpaired surrogate is reported, not replaced");
}

/**
 * @test test_capacity_is_per_character
 * @brief A character that does not fit whole is refused whole.
 *
 * @details Half a surrogate pair, or three bytes of a four-byte sequence, is
 *          not a shorter name -- it is a different one, and on the read side it
 *          is one no UTF-8 decoder accepts. Both directions therefore test the
 *          WHOLE character against the remaining room.
 *
 * @par MC/DC:
 * Decision: `if ((n + need) > cap)` in `priv_utf16_put` (1 condition).
 * - V1: one unit of room, a BMP character  -> false -> fits.
 * - V2: one unit of room, a supplementary one -> true -> no_mem.
 * Decision: `if ((n + len + 1U) > cap)` in `priv_utf8_put` (1 condition).
 * - V3: room for the bytes and the NUL -> false -> fits.
 * - V4: room for the bytes but not the NUL -> true -> no_mem.
 *
 * @pre None; the functions under test touch no volume.
 * @post Each overflow reported `k_ra8_err_no_mem` and produced nothing partial.
 *
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_capacity_is_per_character(void)
{
  TEST_BEGIN("ra8_fs utf: capacity is tested per whole character");
  uint16_t          one_unit[1] = {};
  uint32_t          n           = 0U;
  static const char bmp[]       = {(char)(unsigned char)0xC3U, (char)(unsigned char)0xA9U, '\0'};
  static const char supp[]      = {(char)(unsigned char)0xF0U,
                                   (char)(unsigned char)0x9FU,
                                   (char)(unsigned char)0x98U,
                                   (char)(unsigned char)0x80U,
                                   '\0'};

  TEST_ASSERT_EQ(k_ra8_ok, priv_utf8_to_utf16(bmp, one_unit, 1U, &n)); /* V1 */
  TEST_ASSERT_EQ(1U, n);
  TEST_ASSERT_EQ(k_ra8_err_no_mem, priv_utf8_to_utf16(supp, one_unit, 1U, &n)); /* V2 */
  TEST_ASSERT_EQ(0U, n);

  const uint16_t units[1] = {(uint16_t)k_ut_cp_2}; /* two UTF-8 bytes */
  char           out[4]   = {};
  TEST_ASSERT_EQ(k_ra8_ok, priv_utf16_to_utf8(units, 1U, out, 3U)); /* V3: 2 + NUL */
  TEST_ASSERT_EQ(2U, strlen(out));
  TEST_ASSERT_EQ(k_ra8_err_no_mem, priv_utf16_to_utf8(units, 1U, out, 2U)); /* V4 */
  TEST_ASSERT_EQ('\0', out[0]);
  TEST_END("ra8_fs utf: capacity is tested per whole character");
}

/**
 * @test test_null_guards
 * @brief Every NULL argument is refused before anything is read or written.
 *
 * @par MC/DC:
 * Decision: `if ((in == nullptr) || (out == nullptr) || (out_units == nullptr))`
 * in `libs/ra8_fs/src/ra8_fs_utf.c@priv_utf8_to_utf16` (3 conditions).
 * - V1: all non-NULL -> F,F,F -> false (every other case in this file).
 * - V2: in NULL        -> T     -> true.
 * - V3: out NULL       -> F,T   -> true.
 * - V4: out_units NULL -> F,F,T -> true.
 * Decision: `if ((in == nullptr) || (out == nullptr) || (cap == 0U))` in
 * `libs/ra8_fs/src/ra8_fs_utf.c@priv_utf16_to_utf8` (3 conditions), with the
 * same shape.
 * - V5: in NULL, V6: out NULL, V7: cap 0.
 * N+1 per decision: minimal MC/DC on both.
 *
 * @pre None; the functions under test touch no volume.
 * @post Each NULL reported `k_ra8_err_null_ptr`.
 *
 * @since 0.1.0 @details Runs the null guards vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_null_guards(void)
{
  TEST_BEGIN("ra8_fs utf: NULL argument guards");
  uint16_t units[k_ut_units] = {};
  uint32_t n                 = 0U;
  char     out[k_ut_bytes]   = {};

  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 priv_utf8_to_utf16(nullptr, units, (uint32_t)k_ut_units, &n)); /* V2 */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 priv_utf8_to_utf16("A", nullptr, (uint32_t)k_ut_units, &n)); /* V3 */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 priv_utf8_to_utf16("A", units, (uint32_t)k_ut_units, nullptr)); /* V4 */

  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 priv_utf16_to_utf8(nullptr, 1U, out, (uint32_t)k_ut_bytes)); /* V5 */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 priv_utf16_to_utf8(units, 1U, nullptr, (uint32_t)k_ut_bytes)); /* V6 */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, priv_utf16_to_utf8(units, 1U, out, 0U));   /* V7 */
  TEST_END("ra8_fs utf: NULL argument guards");
}

/* ===========================================================================
 * Tests: folding
 * ===========================================================================
 */

/**
 * @test test_upcase_against_unicode
 * @brief The fold matches Unicode's simple upper-case mapping, not our table.
 *
 * @details Deliberately asserted against facts anyone can look up rather than
 *          against the bytes this tree embeds: agreeing with our own table is
 *          not evidence that the table is right. The three "no mapping" cases
 *          are the interesting half -- U+00DF has no single-unit upper case,
 *          U+4F60 has no case at all, and U+FFFF sits where the compressed
 *          table's trailing run tag has no length after it, which is the one
 *          place the walk can run off the end.
 *
 * @par MC/DC:
 * Decision: `if (v != k_exfat_upc_run_tag)` in
 * `libs/ra8_fs/src/ra8_fs_fat_exfat_upcase.c@priv_exfat_upcase_unit` (1 cond).
 * - V1: a mapped unit (U+00E9)      -> false arm -> the table's value.
 * - V2: a unit inside an identity run (U+4F60) -> true arm -> unchanged.
 * Decision: `if (want < (idx + run))` in the same function (1 condition).
 * - V2 above -> true  -> inside the run, identity.
 * - V3: U+FF41, past several runs -> false each time -> keeps walking, maps.
 * Decision: `if (w >= k_exfat_upc_words)` after a tag (1 condition).
 * - V4: U+FFFF -> true -> the trailing tag, identity.
 *
 * @pre None; the function under test touches no volume.
 * @post Every documented mapping held, in both the mapped and identity cases.
 *
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_upcase_against_unicode(void)
{
  TEST_BEGIN("ra8_fs utf: up-case matches Unicode's simple mapping");
  /* Mapped: Latin-1, Greek, Cyrillic, fullwidth Latin. */
  TEST_ASSERT_EQ(k_ut_up_upper_e, priv_exfat_upcase_unit((uint16_t)k_ut_up_lower_e)); /* V1 */
  TEST_ASSERT_EQ(k_ut_up_capalpha, priv_exfat_upcase_unit((uint16_t)k_ut_up_alpha));
  TEST_ASSERT_EQ(k_ut_up_cyr_cap, priv_exfat_upcase_unit((uint16_t)k_ut_up_cyr_a));
  TEST_ASSERT_EQ(k_ut_up_fw_cap, priv_exfat_upcase_unit((uint16_t)k_ut_up_fw_a)); /* V3 */

  /* Already upper case: the fold is idempotent, which is what makes it usable
   * for comparison at all. */
  TEST_ASSERT_EQ(k_ut_up_upper_e, priv_exfat_upcase_unit((uint16_t)k_ut_up_upper_e));

  /* Unmapped: no single-unit upper case, no case, and the trailing tag. */
  TEST_ASSERT_EQ(k_ut_up_sharp_s, priv_exfat_upcase_unit((uint16_t)k_ut_up_sharp_s));
  TEST_ASSERT_EQ(k_ut_cp_3, priv_exfat_upcase_unit((uint16_t)k_ut_cp_3));       /* V2 */
  TEST_ASSERT_EQ(k_ut_up_last, priv_exfat_upcase_unit((uint16_t)k_ut_up_last)); /* V4 */
  TEST_END("ra8_fs utf: up-case matches Unicode's simple mapping");
}

/**
 * @test test_upcase_ascii_fast_path_is_the_table
 * @brief The ASCII short-circuit is an optimisation, not a second rule.
 *
 * @details `priv_exfat_upcase_unit` answers the ASCII range without walking the
 *          compressed table. That is only legitimate if it gives the SAME
 *          answer, so all 128 units are checked against the rule the table
 *          encodes there -- 'a'..'z' map to 'A'..'Z', everything else maps to
 *          itself. A fast path that quietly disagreed would put a different
 *          NameHash on every ASCII file.
 *
 * @par MC/DC:
 * Decision: `if (want <= k_utf_ascii_max)` in the same function (1 condition).
 * - V1: 0x00..0x7F -> true  -> the fast path (this case, 128 times).
 * - V2: 0x80 and up -> false -> the table walk (::test_upcase_against_unicode).
 * The boundary is asserted from both sides here: 0x7F takes the fast path and
 * 0x80 does not, and both are identity, so an off-by-one would be invisible in
 * the RESULT and is caught by the boundary pair below instead.
 *
 * @pre None; the function under test touches no volume.
 * @post All 128 ASCII units folded exactly as the table does.
 *
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_upcase_ascii_fast_path_is_the_table(void)
{
  TEST_BEGIN("ra8_fs utf: the ASCII fast path agrees with the table");
  for (uint32_t u = 0U; u < (uint32_t)k_ut_ascii_count; u++) {
    uint32_t want = u;
    if ((u >= (uint32_t)'a') && (u <= (uint32_t)'z')) {
      want = u - (uint32_t)k_ut_case_delta;
    }
    TEST_ASSERT_EQ(want, priv_exfat_upcase_unit((uint16_t)u));
  }
  /* Either side of the boundary, both identity: the fast path must not change
   * what the walk would have said at 0x7F, and must not be taken at 0x80. */
  TEST_ASSERT_EQ(0x7FU, priv_exfat_upcase_unit(0x7FU));
  TEST_ASSERT_EQ(0x80U, priv_exfat_upcase_unit(0x80U));
  TEST_END("ra8_fs utf: the ASCII fast path agrees with the table");
}

/**
 * @test test_utf16_ieq
 * @brief Case-insensitive comparison, and what it must NOT call equal.
 *
 * @par MC/DC:
 * Decision: `if (an != bn)` in `libs/ra8_fs/src/ra8_fs_utf.c@priv_utf16_ieq`
 * (1 condition).
 * - V1: equal lengths   -> false -> the fold loop runs.
 * - V2: unequal lengths -> true  -> 0 without comparing anything.
 * Decision: `if (upcase(a[i]) != upcase(b[i]))` in the same function (1 cond).
 * - V3: every folded unit equal   -> false throughout -> 1.
 * - V4: one folded unit different -> true -> 0.
 *
 * @pre None; the function under test touches no volume.
 * @post Case-different names matched; genuinely different ones did not.
 *
 * @since 0.1.0 @details Runs the utf16 ieq vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_utf16_ieq(void)
{
  TEST_BEGIN("ra8_fs utf: case-insensitive comparison across the BMP");
  const uint16_t lower[3] = {(uint16_t)k_ut_up_lower_e, (uint16_t)'b', (uint16_t)k_ut_up_alpha};
  const uint16_t upper[3] = {(uint16_t)k_ut_up_upper_e, (uint16_t)'B', (uint16_t)k_ut_up_capalpha};
  const uint16_t other[3] = {(uint16_t)k_ut_up_upper_e, (uint16_t)'B', (uint16_t)k_ut_up_cyr_cap};

  TEST_ASSERT_EQ(1U, priv_utf16_ieq(lower, 3U, upper, 3U)); /* V1, V3 */
  TEST_ASSERT_EQ(0U, priv_utf16_ieq(lower, 3U, upper, 2U)); /* V2     */
  TEST_ASSERT_EQ(0U, priv_utf16_ieq(lower, 3U, other, 3U)); /* V4     */

  /* Surrogate units fold to themselves, so a supplementary character compares
   * exactly -- which is what a host does with the same BMP-only table. */
  const uint16_t emoji_a[2] = {(uint16_t)k_ut_sur_hi_of_4, (uint16_t)k_ut_sur_lo_of_4};
  const uint16_t emoji_b[2] = {(uint16_t)k_ut_sur_hi_of_4, (uint16_t)k_ut_sur_lo_of_4};
  TEST_ASSERT_EQ(1U, priv_utf16_ieq(emoji_a, 2U, emoji_b, 2U));
  TEST_END("ra8_fs utf: case-insensitive comparison across the BMP");
}

/**
 * @test test_utf16_all_ascii
 * @brief The predicate that decides whether a volume's own table is needed.
 *
 * @par MC/DC:
 * Decision: `if ((uint32_t)in[i] > k_utf_ascii_max)` in
 * `libs/ra8_fs/src/ra8_fs_utf.c@priv_utf16_all_ascii` (1 condition).
 * - V1: every unit at or below 0x7F -> false throughout -> 1.
 * - V2: one unit above it           -> true -> 0.
 * - V3: zero units                  -> the loop never runs -> 1.
 * The boundary matters here: 0x7F is ASCII and 0x80 is not, and the two
 * assertions below straddle it.
 *
 * @pre None; the function under test touches no volume.
 * @post The predicate agreed with the ASCII range at both its edges.
 *
 * @since 0.1.0 @details Runs the utf16 all ascii vector through production filesystem seams and checks observable state. @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_utf16_all_ascii(void)
{
  TEST_BEGIN("ra8_fs utf: the ASCII-only predicate");
  const uint16_t ascii[3]    = {(uint16_t)'A', 0x7FU, (uint16_t)'z'};
  const uint16_t nonascii[3] = {(uint16_t)'A', 0x80U, (uint16_t)'z'};
  TEST_ASSERT_EQ(1U, priv_utf16_all_ascii(ascii, 3U));    /* V1 */
  TEST_ASSERT_EQ(0U, priv_utf16_all_ascii(nonascii, 3U)); /* V2 */
  TEST_ASSERT_EQ(1U, priv_utf16_all_ascii(ascii, 0U));    /* V3 */
  TEST_END("ra8_fs utf: the ASCII-only predicate");
}

/**
 * @test test_name_chunk_eq_folds_a_non_ascii_unit
 * @brief One exFAT Name-entry slice, folded rather than rejected out of hand.
 *
 * @details Lives here rather than in the exFAT read-path coverage suite because
 *          what it asks about is the FOLD: `priv_exfat_name_chunk_eq()` used to
 *          reject any unit whose high byte was set, so an accented name matched
 *          nothing and could not be opened (#606). It folds like every other
 *          unit now, so U+0100 matches U+0100, does NOT match 0x0041 -- which
 *          shares only its low byte, the old comparison's exact failure -- and
 *          U+00E9 matches U+00C9.
 *
 * @par MC/DC:
 * Decision: `if (upcase(unit) != upcase(name[pos + i]))` in
 * `libs/ra8_fs/src/ra8_fs_fat_exfat_read.c@priv_exfat_name_chunk_eq` (1 cond).
 * - V1: equal after folding   -> false -> the scan continues (matches here).
 * - V2: unequal after folding -> true  -> return 0 (the mismatch here).
 *
 * @pre None; the function under test touches no volume.
 * @post A non-ASCII unit matches its equal and its case-fold, and nothing else.
 *
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_name_chunk_eq_folds_a_non_ascii_unit(void)
{
  TEST_BEGIN("ra8_fs utf: an exFAT name slice folds a non-ASCII unit");
  uint8_t entry[k_exfat_entry_bytes] = {};
  priv_wr16(&entry[k_exfat_name_off], (uint16_t)k_ut_chunk_mapped);
  const uint16_t same[1]    = {(uint16_t)k_ut_chunk_mapped};
  const uint16_t lowbyte[1] = {(uint16_t)k_ut_chunk_lowbyte};           /* only the low byte */
  TEST_ASSERT_EQ(1U, priv_exfat_name_chunk_eq(entry, same, 0U, 1U));    /* V1                */
  TEST_ASSERT_EQ(0U, priv_exfat_name_chunk_eq(entry, lowbyte, 0U, 1U)); /* V2                */

  /* Case-insensitive across the BMP: U+00E9 stored, U+00C9 asked for. */
  priv_wr16(&entry[k_exfat_name_off], (uint16_t)k_ut_up_lower_e);
  const uint16_t upper[1] = {(uint16_t)k_ut_up_upper_e};
  TEST_ASSERT_EQ(1U, priv_exfat_name_chunk_eq(entry, upper, 0U, 1U));
  TEST_END("ra8_fs utf: an exFAT name slice folds a non-ASCII unit");
}

/**
 * @brief Run every case in this suite.
 *
 * @return Process exit status.
 * @retval 0 Every case passed; a failure aborts inside the harness instead.
 *
 * @pre No other suite shares this process.
 * @pre The functions under test need no mounted volume.
 * @post Every vector in the file has run.
 * @post Nothing was allocated, so nothing needs freeing.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
int main(void)
{
  internal_test_decode_lengths();
  internal_test_mixed_name_round_trip();
  internal_test_decode_rejects_malformed();
  internal_test_encode_rejects_unpaired_surrogate();
  internal_test_capacity_is_per_character();
  internal_test_null_guards();
  internal_test_upcase_against_unicode();
  internal_test_upcase_ascii_fast_path_is_the_table();
  internal_test_utf16_ieq();
  internal_test_utf16_all_ascii();
  internal_test_name_chunk_eq_folds_a_non_ascii_unit();
  return 0;
}
