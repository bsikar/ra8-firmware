/**
 * @file test_ra8_fs_lfn_write_mcdc.c
 * @brief MC/DC vectors for the pure halves of the long-name write seam (#600).
 *
 * @details
 * The three functions here decide, without touching a volume, everything about
 * how a name will be stored: whether it needs a chain at all
 * (`priv_name_classify`), what 8.3 alias it is filed under
 * (`priv_lfn_alias_basis`), and what one slot of the chain looks like
 * (`priv_lfn_fill_slot`). They are pure, so every branch is reachable by
 * calling them, and the vectors below say what they are worth rather than
 * reaching them through a mount that would obscure which condition varied.
 *
 * `priv_lfn_fill_slot()` is checked against `priv_lfn_add()`, the reassembler
 * that has read these entries since #101 -- a writer whose output its own
 * reader cannot recover is the one failure mode a byte-by-byte assertion on
 * the writer alone would not catch.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_fs.h"
#include "ra8_fs_fat_internal.h"
#include "unity_minimal.h"

/**
 * @enum lwm_val_t
 * @brief Expected field values and loop bounds for these vectors.
 *
 * @invariant Every constant here restates a value the FAT specification fixes.
 * @see test_fill_slot_shape()
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_lwm_ntres_base = 0x08U,   /**< DIR_NTRes: base is lower case.         */
  k_lwm_ntres_ext  = 0x10U,   /**< DIR_NTRes: extension is lower case.    */
  k_lwm_ntres_both = 0x18U,   /**< Both halves lower case.                */
  k_lwm_seq_last   = 0x40U,   /**< LDIR_Ord: last logical group.          */
  k_lwm_pad        = 0xFFFFU, /**< Slot padding past the name terminator. */
  k_lwm_per_group  = 13U,     /**< Characters carried by one slot.        */
  k_lwm_tail_1d    = 9U,      /**< Highest one-digit alias tail.          */
  k_lwm_tail_2d_lo = 10U,     /**< Lowest two-digit alias tail.           */
  k_lwm_tail_2d_hi = 99U,     /**< Highest two-digit alias tail.          */
  k_lwm_tail_3d    = 100U,    /**< Lowest three-digit alias tail.         */
  k_lwm_tail_4d    = 1000U,   /**< Lowest four-digit alias tail.          */
  k_lwm_tail_5d    = 10000U,  /**< Lowest five-digit alias tail.          */
  k_lwm_tail_6d    = 100000U, /**< Lowest six-digit alias tail.           */
  k_lwm_tail_max   = 999999U, /**< `k_lfn_alias_tail_max`: the last tail. */
  k_lwm_short_len  = 5U,      /**< Length of the five-character vector.   */
  k_lwm_two_groups = 26U,     /**< Length of the two-full-group vector.   */
  k_lwm_csum_a     = 0xABU,   /**< An arbitrary but pinned checksum byte. */
  k_lwm_csum_b     = 0x5AU,   /**< A second one, to tell the slots apart. */
  k_lwm_name1_0    = 1U,      /**< LDIR_Name1 char 0 (MS FAT spec sec 7). */
  k_lwm_name2_0    = 14U,     /**< LDIR_Name2 char 0.                     */
  k_lwm_name2_1    = 16U,     /**< LDIR_Name2 char 1.                     */
  k_lwm_name3_1    = 30U,     /**< LDIR_Name3 char 1: the 13th character. */
} lwm_val_t;

/* ===========================================================================
 * priv_name_classify
 * ===========================================================================
 */

/**
 * @brief Classify @p leaf and report the verdict, discarding the outputs.
 *
 * @param[in] leaf Name to classify.
 *
 * @return The verdict.
 * @retval k_name_kind_short   It fits an 8.3 entry (possibly with case flags).
 * @retval k_name_kind_long    It needs a chain.
 * @retval k_name_kind_invalid It cannot be stored.
 *
 * @pre @p leaf is non-NULL and NUL-terminated.
 * @pre The caller does not need the packed name or the flags.
 * @post No state is modified.
 * @post The verdict depends only on @p leaf.
 *
 * @note Pure wrapper; trivially thread-safe.
 * @since 0.1.0
 */
static ra8_fs_name_kind_t kind_of(const char* leaf)
{
  uint8_t name83[k_max_8_3_name] = {};
  uint8_t ntres                  = 0U;
  return priv_name_classify(leaf, name83, &ntres);
}

/**
 * @brief Classify @p leaf and report the `DIR_NTRes` byte it asked for.
 *
 * @param[in] leaf Name to classify.
 *
 * @return The case flags.
 * @retval 0x00 No half is lower case, or the name is not short.
 * @retval 0x08 / 0x10 / 0x18 The half or halves that are.
 *
 * @pre @p leaf is non-NULL and NUL-terminated.
 * @pre @p leaf is expected to classify as short.
 * @post No state is modified.
 * @post The result is one of the four documented values.
 *
 * @note Pure wrapper; trivially thread-safe.
 * @since 0.1.0
 */
static uint8_t ntres_of(const char* leaf)
{
  uint8_t name83[k_max_8_3_name] = {};
  uint8_t ntres                  = 0U;
  (void)priv_name_classify(leaf, name83, &ntres);
  return ntres;
}

/**
 * @test test_classify_length_bounds
 * @brief The empty name and the over-long name are both refused.
 *
 * @par MC/DC:
 * Decision: `if ((n == 0U) || (n > k_lfn_write_max))` in
 * `libs/ra8_fs/src/ra8_fs_fat_name.c@priv_name_classify` (2 conditions).
 * - V1: `"OK.TXT"` (6)   -> C1=F, C2=F -> false, classification continues.
 * - V2: `""`      (0)    -> C1=T       -> true, invalid.
 * - V3: 248 characters   -> C1=F, C2=T -> true, invalid.
 * V1+V2 prove C1 independently affects the outcome; V1+V3 prove the same for
 * C2. N+1 = 3 vectors for N=2: minimal MC/DC.
 *
 * @pre None; the function under test touches no volume.
 * @post Every vector's verdict matched, and 247 characters is still accepted.
 *
 * @since 0.1.0
 */
static void test_classify_length_bounds(void)
{
  TEST_BEGIN("lfn classify MC/DC: length bounds (n==0 || n>max)");
  char buf[k_lfn_name_cap + 8U] = {};

  TEST_ASSERT_EQ(k_name_kind_short, kind_of("OK.TXT")); /* V1 */
  TEST_ASSERT_EQ(k_name_kind_invalid, kind_of(""));     /* V2 */

  for (uint32_t i = 0U; i < ((uint32_t)k_lfn_write_max + 1U); i++) {
    buf[i] = 'x';
  }
  TEST_ASSERT_EQ(k_name_kind_invalid, kind_of(buf)); /* V3 */

  buf[(uint32_t)k_lfn_write_max] = '\0';
  TEST_ASSERT_EQ(k_name_kind_long, kind_of(buf)); /* the boundary itself */

  TEST_END("lfn classify MC/DC: length bounds (n==0 || n>max)");
}

/**
 * @test test_classify_illegal_characters
 * @brief A character no long name may hold makes the whole leaf unstorable.
 *
 * @par MC/DC:
 * Decision: `if (priv_char_is_lfn_legal(leaf[i]) == 0U)` in
 * `libs/ra8_fs/src/ra8_fs_fat_name.c@priv_name_classify` (1 condition), and the
 * three-way ladder inside `priv_char_is_lfn_legal` itself:
 * - Control range (`u < k_lfn_space`): a tab.
 * - At or above DEL (`u >= k_lfn_ascii_max`): a 0x7F byte.
 * - In the explicit illegal set: `?`, `*`, `|`, `:`, `<`, `>`, `"`, `\`.
 * Each is one vector; the legal control is `"Ordinary Name.txt"`, which takes
 * the false arm of all three.
 *
 * @pre None; the function under test touches no volume.
 * @post Every illegal character was refused and the control was not.
 *
 * @since 0.1.0
 */
static void test_classify_illegal_characters(void)
{
  TEST_BEGIN("lfn classify MC/DC: illegal characters");
  TEST_ASSERT_EQ(k_name_kind_long, kind_of("Ordinary Name.txt")); /* control */

  static const char* s_bad[] = {"who?.txt",
                                "star*.txt",
                                "pipe|.txt",
                                "colon:.txt",
                                "lt<.txt",
                                "gt>.txt",
                                "quote\".txt",
                                "back\\slash.txt",
                                "tab\there.txt",
                                "del\x7f.txt"};
  for (uint32_t i = 0U; i < (uint32_t)(sizeof(s_bad) / sizeof(s_bad[0])); i++) {
    TEST_ASSERT_EQ(k_name_kind_invalid, kind_of(s_bad[i]));
  }
  TEST_END("lfn classify MC/DC: illegal characters");
}

/**
 * @test test_classify_shape_rules
 * @brief What makes a legal name 8.3-representable, one rule at a time.
 *
 * @par MC/DC:
 * Decision: `if ((base_len == 0U) || (base_len > k_filename_base_len))` in
 * `libs/ra8_fs/src/ra8_fs_fat_name.c@priv_name_is_83` (2 conditions).
 * - V1: `"BASE.TXT"`     -> C1=F, C2=F -> false -> short.
 * - V2: `".profile"`     -> C1=T       -> true  -> long (empty base).
 * - V3: `"NINECHARS.TX"` -> C1=F, C2=T -> true  -> long (nine-character base).
 * V1+V2 isolate C1; V1+V3 isolate C2.
 *
 * Decision: `if ((ext_len == 0U) || (ext_len > k_filename_ext_len))` (2 more).
 * - V4: `"BASE.TXT"`  -> C1=F, C2=F -> false -> short (the same control).
 * - V5: `"REPORT."`   -> C1=T       -> true  -> long (a dot with no extension).
 * - V6: `"A.FOUR"`    -> C1=F, C2=T -> true  -> long (four-character extension).
 *
 * A second dot (`"a.b.c"`), a space (`"my file.txt"`) and an 8.3-illegal
 * punctuation character (`"a+b.txt"`) each take the long arm too: they are
 * legal in a long name, so they are stored rather than refused.
 *
 * @pre None; the function under test touches no volume.
 * @post Every shape landed in the arm its rule requires.
 *
 * @since 0.1.0
 */
static void test_classify_shape_rules(void)
{
  TEST_BEGIN("lfn classify MC/DC: 8.3 shape rules");
  TEST_ASSERT_EQ(k_name_kind_short, kind_of("BASE.TXT"));    /* V1 / V4 */
  TEST_ASSERT_EQ(k_name_kind_long, kind_of(".profile"));     /* V2      */
  TEST_ASSERT_EQ(k_name_kind_long, kind_of("NINECHARS.TX")); /* V3      */
  TEST_ASSERT_EQ(k_name_kind_long, kind_of("REPORT."));      /* V5      */
  TEST_ASSERT_EQ(k_name_kind_long, kind_of("A.FOUR"));       /* V6      */
  TEST_ASSERT_EQ(k_name_kind_long, kind_of("a.b.c"));
  TEST_ASSERT_EQ(k_name_kind_long, kind_of("my file.txt"));
  TEST_ASSERT_EQ(k_name_kind_long, kind_of("a+b.txt"));
  TEST_ASSERT_EQ(k_name_kind_short, kind_of("NOEXT"));
  TEST_ASSERT_EQ(k_name_kind_short, kind_of("EIGHTCHR.EXT"));
  TEST_END("lfn classify MC/DC: 8.3 shape rules");
}

/**
 * @test test_classify_case_flags
 * @brief Each half's case decides its own flag, and mixing forces a chain.
 *
 * @par MC/DC:
 * Decision: `if (base_seen == k_case_seen_mixed)` in
 * `libs/ra8_fs/src/ra8_fs_fat_name.c@priv_name_case_kind` (1 condition).
 * - V1: `"data.log"` -> false -> short.
 * - V2: `"Data.log"` -> true  -> long.
 *
 * Decision: `if (ext_seen == k_case_seen_mixed)` (1 condition).
 * - V3: `"data.log"` -> false -> short (the same control).
 * - V4: `"data.Log"` -> true  -> long.
 *
 * Decision: `if (base_seen == k_case_seen_lower)` (1 condition).
 * - V5: `"data.LOG"` -> true  -> flag 0x08.
 * - V6: `"DATA.LOG"` -> false -> flag 0x00.
 *
 * Decision: `if (ext_seen == k_case_seen_lower)` (1 condition).
 * - V7: `"DATA.log"` -> true  -> flag 0x10.
 * - V8: `"DATA.LOG"` -> false -> flag 0x00 (the same control).
 *
 * A half with no letters at all (`"0123.log"`) is neither upper nor lower, so
 * it takes the false arm of both of its decisions without being mixed.
 *
 * @pre None; the function under test touches no volume.
 * @post Each of the four `DIR_NTRes` values was produced by the name that
 *       requires it, and both mixed-case halves forced a chain.
 *
 * @since 0.1.0
 */
static void test_classify_case_flags(void)
{
  TEST_BEGIN("lfn classify MC/DC: NTRes case flags");
  TEST_ASSERT_EQ(k_name_kind_short, kind_of("data.log")); /* V1 / V3 */
  TEST_ASSERT_EQ(k_name_kind_long, kind_of("Data.log"));  /* V2      */
  TEST_ASSERT_EQ(k_name_kind_long, kind_of("data.Log"));  /* V4      */

  TEST_ASSERT_EQ(k_lwm_ntres_both, ntres_of("data.log"));
  TEST_ASSERT_EQ(k_lwm_ntres_base, ntres_of("data.LOG")); /* V5      */
  TEST_ASSERT_EQ(0U, ntres_of("DATA.LOG"));               /* V6 / V8 */
  TEST_ASSERT_EQ(k_lwm_ntres_ext, ntres_of("DATA.log"));  /* V7      */
  TEST_ASSERT_EQ(k_lwm_ntres_ext, ntres_of("0123.log"));
  TEST_ASSERT_EQ(0U, ntres_of("0123.LOG"));
  TEST_ASSERT_EQ(k_lwm_ntres_base, ntres_of("lower"));
  TEST_END("lfn classify MC/DC: NTRes case flags");
}

/**
 * @test test_classify_null_guards
 * @brief Any NULL argument is refused before anything is read.
 *
 * @par MC/DC:
 * Decision: `if ((leaf == nullptr) || (out83 == nullptr) || (out_ntres ==
 * nullptr))` in `libs/ra8_fs/src/ra8_fs_fat_name.c@priv_name_classify`
 * (3 conditions).
 * - V1: all non-NULL     -> F,F,F -> false (every other vector in this file).
 * - V2: leaf NULL        -> T     -> true.
 * - V3: out83 NULL       -> F,T   -> true.
 * - V4: out_ntres NULL   -> F,F,T -> true.
 * N+1 = 4 vectors for N=3: minimal MC/DC.
 *
 * @pre None; the function under test touches no volume.
 * @post Each NULL was refused with `k_name_kind_invalid`.
 *
 * @since 0.1.0
 */
static void test_classify_null_guards(void)
{
  TEST_BEGIN("lfn classify MC/DC: NULL argument triple");
  uint8_t name83[k_max_8_3_name] = {};
  uint8_t ntres                  = 0U;
  TEST_ASSERT_EQ(k_name_kind_invalid, priv_name_classify(nullptr, name83, &ntres));
  TEST_ASSERT_EQ(k_name_kind_invalid, priv_name_classify("A.TXT", nullptr, &ntres));
  TEST_ASSERT_EQ(k_name_kind_invalid, priv_name_classify("A.TXT", name83, nullptr));
  TEST_ASSERT_EQ(k_name_kind_short, priv_name_classify("A.TXT", name83, &ntres));
  TEST_END("lfn classify MC/DC: NULL argument triple");
}

/* ===========================================================================
 * priv_lfn_alias_basis
 * ===========================================================================
 */

/**
 * @brief Generate the alias for @p leaf at @p tail and compare it to @p want.
 *
 * @param[in] leaf Long name.
 * @param[in] tail Sequence number.
 * @param[in] want The expected 11-byte packed alias.
 *
 * @return Nothing; a mismatch is reported by the harness.
 *
 * @pre @p leaf and @p want are non-NULL; @p want is 11 characters.
 * @pre @p tail is at least 1.
 * @post The generated alias equalled @p want byte for byte.
 * @post No state outside the local buffer is modified.
 *
 * @note Not thread-safe (uses the harness's failure counter).
 * @since 0.1.0
 */
static void expect_alias(const char* leaf, uint32_t tail, const char* want)
{
  uint8_t got[k_max_8_3_name] = {};
  priv_lfn_alias_basis(leaf, tail, got);
  if (memcmp(got, want, (size_t)k_max_8_3_name) != 0) {
    TEST_FAIL_FMT("alias for \"%s\" ~%u: want \"%.11s\" got \"%.11s\"",
                  leaf,
                  (unsigned)tail,
                  want,
                  (const char*)got);
  }
}

/**
 * @test test_alias_basis_shapes
 * @brief The basis-name algorithm, one rule per vector.
 *
 * @par MC/DC:
 * Decision: `if ((c == ' ') || (c == '.'))` in
 * `libs/ra8_fs/src/ra8_fs_fat_name.c@priv_alias_collect` (2 conditions).
 * - V1: `'M'` in `"My Report.txt"` -> C1=F, C2=F -> false -> the character is kept.
 * - V2: the space in the same name -> C1=T       -> true  -> dropped.
 * - V3: the first dot in `"a.b.c"` -> C1=F, C2=T -> true  -> dropped.
 * V1+V2 isolate C1; V1+V3 isolate C2.
 *
 * Decision: `if (base_len == 0U)` in `priv_lfn_alias_basis` (1 condition).
 * - V4: `"..."`          -> true  -> the base becomes `_`.
 * - V5: `"My Report.txt"` -> false -> the collected characters are used.
 *
 * Decision: `if (base_len < keep)` (1 condition).
 * - V6: `"ab cd.txt"` (4 collected) -> true  -> keep shrinks to 4.
 * - V7: `"My Report.txt"` (9)       -> false -> keep stays at 6.
 *
 * @pre None; the function under test touches no volume.
 * @post Every expected alias matched byte for byte.
 *
 * @since 0.1.0
 */
static void test_alias_basis_shapes(void)
{
  TEST_BEGIN("lfn alias MC/DC: basis-name generation");
  expect_alias("My Report.txt", 1U, "MYREPO~1TXT"); /* V1,V2,V5,V7 */
  expect_alias("a.b.c", 1U, "AB~1    C  ");         /* V3          */
  expect_alias("...", 1U, "_~1        ");           /* V4          */
  expect_alias("ab cd.txt", 1U, "ABCD~1  TXT");     /* V6          */
  expect_alias("Design (final).ext", 1U, "DESIGN~1EXT");
  expect_alias(".profile", 1U, "PROFIL~1   ");
  expect_alias("has+plus.and,comma", 1U, "HAS_PL~1AND");
  TEST_END("lfn alias MC/DC: basis-name generation");
}

/**
 * @test test_alias_basis_tail_widths
 * @brief The numeric tail eats into the base exactly as many characters as it needs.
 *
 * @par MC/DC:
 * Decision: `while ((digits < k_alias_digits_max) && (tail >= scale))` in
 * `libs/ra8_fs/src/ra8_fs_fat_name.c@priv_alias_digits` (2 conditions).
 * - V1: tail = 1      -> C1=T, C2=F -> false on the first test -> 1 digit.
 * - V2: tail = 10     -> C1=T, C2=T -> true once               -> 2 digits.
 * - V3: tail = 999999 -> C1 goes F at the cap                  -> 6 digits.
 * V1+V2 prove C2 independently affects the loop; V2+V3 prove C1 does, by
 * stopping the loop while `tail >= scale` would still be true were the cap
 * absent.
 *
 * @pre None; the function under test touches no volume.
 * @post Each tail width left exactly `8 - digits - 1` basis characters.
 *
 * @since 0.1.0
 */
static void test_alias_basis_tail_widths(void)
{
  TEST_BEGIN("lfn alias MC/DC: numeric tail widths 1..6");
  expect_alias("Long Document Name.txt", 1U, "LONGDO~1TXT"); /* V1 */
  expect_alias("Long Document Name.txt", (uint32_t)k_lwm_tail_1d, "LONGDO~9TXT");
  expect_alias("Long Document Name.txt", (uint32_t)k_lwm_tail_2d_lo, "LONGD~10TXT"); /* V2 */
  expect_alias("Long Document Name.txt", (uint32_t)k_lwm_tail_2d_hi, "LONGD~99TXT");
  expect_alias("Long Document Name.txt", (uint32_t)k_lwm_tail_3d, "LONG~100TXT");
  expect_alias("Long Document Name.txt", (uint32_t)k_lwm_tail_4d, "LON~1000TXT");
  expect_alias("Long Document Name.txt", (uint32_t)k_lwm_tail_5d, "LO~10000TXT");
  expect_alias("Long Document Name.txt", (uint32_t)k_lwm_tail_6d, "L~100000TXT");
  expect_alias("Long Document Name.txt", (uint32_t)k_lwm_tail_max, "L~999999TXT"); /* V3 */
  TEST_END("lfn alias MC/DC: numeric tail widths 1..6");
}

/* ===========================================================================
 * priv_lfn_fill_slot
 * ===========================================================================
 */

/**
 * @test test_fill_slot_shape
 * @brief One slot's fixed fields, and the terminator/padding ladder.
 *
 * @par MC/DC:
 * Decision: the `pos < nlen` / `pos == nlen` ladder in
 * `libs/ra8_fs/src/ra8_fs_fat_lfn.c@priv_lfn_fill_slot` (2 conditions).
 * - V1: a 5-character name, character 0 -> `pos < nlen`  -> the character.
 * - V2: the same, character 5           -> `pos == nlen` -> 0x0000.
 * - V3: the same, character 6           -> neither       -> 0xFFFF.
 * - V4: a 13-character name, character 12 -> `pos < nlen`, and no slot in the
 *   group ever reaches `pos == nlen`: the group-boundary case that carries no
 *   terminator at all.
 *
 * Decision: `(is_last != 0U)` in the same function (1 condition).
 * - V5: is_last = 1 -> the order byte carries 0x40.
 * - V6: is_last = 0 -> it does not.
 *
 * @pre None; the function under test touches no volume.
 * @post Every fixed field held the value the specification requires.
 *
 * @since 0.1.0
 */
static void test_fill_slot_shape(void)
{
  TEST_BEGIN("lfn slot MC/DC: fixed fields, terminator and padding");
  uint8_t ent[k_ra8_fs_dir_entry_bytes] = {};

  priv_lfn_fill_slot(ent,
                     "HELLO",
                     (uint32_t)k_lwm_short_len,
                     1U,
                     1U,
                     (uint8_t)k_lwm_csum_a); /* V1,V2,V3,V5 */
  TEST_ASSERT_EQ(k_lwm_seq_last | 1U, ent[k_lfn_off_seq]);
  TEST_ASSERT_EQ(k_ra8_fs_attr_lfn, ent[k_dir_off_attr]);
  TEST_ASSERT_EQ(0U, ent[k_lfn_off_type]);
  TEST_ASSERT_EQ(k_lwm_csum_a, ent[k_lfn_off_checksum]);
  TEST_ASSERT_EQ(0U, priv_rd16(&ent[k_lfn_off_clus_lo]));
  TEST_ASSERT_EQ('H', priv_rd16(&ent[k_lwm_name1_0]));       /* V1              */
  TEST_ASSERT_EQ(0U, priv_rd16(&ent[k_lwm_name2_0]));        /* V2: pos == nlen */
  TEST_ASSERT_EQ(k_lwm_pad, priv_rd16(&ent[k_lwm_name2_1])); /* V3              */

  /* V4: exactly one group, so every character is a real one. */
  priv_lfn_fill_slot(ent, "THIRTEENCHARS", (uint32_t)k_lwm_per_group, 1U, 1U, 0U);
  TEST_ASSERT_EQ('S', priv_rd16(&ent[k_lwm_name3_1]));

  /* V6: not the last logical group. */
  priv_lfn_fill_slot(ent,
                     "TWENTYSIXCHARACTERSEXACTLY",
                     (uint32_t)k_lwm_two_groups,
                     1U,
                     0U,
                     (uint8_t)k_lwm_csum_b);
  TEST_ASSERT_EQ(1U, ent[k_lfn_off_seq]);
  TEST_ASSERT_EQ('T', priv_rd16(&ent[k_lwm_name1_0]));
  TEST_END("lfn slot MC/DC: fixed fields, terminator and padding");
}

/**
 * @test test_fill_slot_round_trips_through_the_reader
 * @brief What the writer lays down is what the reassembler recovers.
 *
 * @details The strongest single statement about the layout: a chain is built
 *          with `priv_lfn_fill_slot()` in the physical order a directory holds
 *          it, fed to `priv_lfn_add()` in that same order, and the name that
 *          comes back has to be the name that went in. Two lengths, because
 *          the interesting failure is at a group boundary: 26 characters is two
 *          exactly-full groups with no terminator anywhere.
 *
 * @par MC/DC:
 * Decision: `if (s->checksum != priv_sfn_checksum(name83))` in
 * `libs/ra8_fs/src/ra8_fs_fat_lfn.c@priv_lfn_name_for` (1 condition).
 * - V1: the chain carries the checksum of the entry it is offered -> false ->
 *   the name is returned.
 * - V2: it is offered a different 8.3 name -> true -> NULL, the chain is not
 *   claimed by an entry it does not belong to.
 *
 * @pre None; the functions under test touch no volume.
 * @post Both names round-tripped, and a mismatched checksum was refused.
 *
 * @since 0.1.0
 */
static void test_fill_slot_round_trips_through_the_reader(void)
{
  TEST_BEGIN("lfn slot MC/DC: writer output survives the reader");
  static const char* s_names[]        = {"A Perfectly Ordinary Name.txt",
                                         "TwentySixCharsHere.picture",
                                         "x"};
  const uint8_t alias[k_max_8_3_name] = {'A', 'L', 'I', 'A', 'S', '~', '1', ' ', 'T', 'X', 'T'};
  const uint8_t other[k_max_8_3_name] = {'O', 'T', 'H', 'E', 'R', '~', '1', ' ', 'T', 'X', 'T'};
  const uint8_t csum                  = priv_sfn_checksum(alias);

  for (uint32_t n = 0U; n < (uint32_t)(sizeof(s_names) / sizeof(s_names[0])); n++) {
    const char*    name   = s_names[n];
    const uint32_t nlen   = (uint32_t)strlen(name);
    const uint32_t groups = ((nlen + (uint32_t)k_lwm_per_group) - 1U) / (uint32_t)k_lwm_per_group;
    lfn_state_t    st     = {};
    priv_lfn_reset(&st);
    for (uint32_t i = 0U; i < groups; i++) {
      uint8_t ent[k_ra8_fs_dir_entry_bytes] = {};
      priv_lfn_fill_slot(ent, name, nlen, groups - i, (i == 0U) ? 1U : 0U, csum);
      priv_lfn_add(&st, ent);
    }
    const char* got = priv_lfn_name_for(&st, alias); /* V1 */
    TEST_ASSERT_NOT_NULL(got);
    TEST_ASSERT_EQ(0, strcmp(got, name));
    TEST_ASSERT_NULL(priv_lfn_name_for(&st, other)); /* V2 */
  }
  TEST_END("lfn slot MC/DC: writer output survives the reader");
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
int32_t main(void)
{
  test_classify_length_bounds();
  test_classify_illegal_characters();
  test_classify_shape_rules();
  test_classify_case_flags();
  test_classify_null_guards();
  test_alias_basis_shapes();
  test_alias_basis_tail_widths();
  test_fill_slot_shape();
  test_fill_slot_round_trips_through_the_reader();
  (void)fprintf(stderr, "[OK  ] test_ra8_fs_lfn_write_mcdc.c\n");
  return 0;
}
