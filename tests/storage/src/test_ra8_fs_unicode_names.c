/**
 * @file test_ra8_fs_unicode_names.c
 * @brief A non-ASCII name on FAT: create it, list it, open it again (#606).
 *
 * @details
 * `priv_lfn_add()` used to substitute `?` for every UTF-16 unit above 0x7F, and
 * `priv_dir_find_long()` compared the caller's name against that SUBSTITUTED
 * text -- so a file whose name held an accent was listed with a `?` in it and
 * could not be opened by the name it was created with. Two names differing only
 * in an accent collided, because both became the same string of question marks.
 *
 * These cases are the proof that the whole loop closes: the name the caller
 * passes to `ra8_fs_open()` is the name `ra8_fs_listdir()` reports, is the name
 * that opens the file again, and is the name `ra8_fs_rename()` and
 * `ra8_fs_unlink()` act on. Every assertion goes through the PUBLIC API, except
 * the one that inspects the generated 8.3 alias, which has to look at the
 * directory entry because the alias is not something the API reports.
 *
 * @par Pure-ASCII sources:
 * Every non-ASCII name is a `static const char[]` of byte escapes, named for
 * what it spells. A literal would put non-ASCII bytes in a source file the
 * tree's encoding gate rejects, and would make the vector depend on whatever
 * encoding the editor used.
 *
 * @par Out-of-band `fsck.fat` evidence:
 * Setting `RA8_FS606_DUMP` writes the volume out so a real checker can be
 * pointed at it. The image is made by `ra8_fs_format()`, so `fsck.fat` has a
 * complete BPB to work from:
 * @code
 *   RA8_FS606_DUMP=/tmp/f606 ./test_ra8_fs_unicode_names
 *   /sbin/fsck.fat -n /tmp/f606.unicode      # three non-ASCII names, intact
 *   /sbin/fsck.fat -n /tmp/f606.mangle_ctrl  # the PRE-FIX chain, hand-built
 * @endcode
 * Confirmed 2026-08-04 on the Linux verification host, dosfstools 4.2:
 * @verbatim
 * unicode     -> no long-name finding; 3 files, 3/65264 clusters
 * mangle_ctrl -> Wrong checksum for long file name "R<E9>sum<E9>.txt".
 *                (Short name Z_SUM_~1.TXT may have changed ...)
 * @endverbatim
 * Two things at once, and the second is the better evidence. The control does
 * its job: the same checker, on the same volume, differing only in whether the
 * chain matches the 8.3 entry behind it. And `fsck.fat` -- which has never seen
 * this codebase -- reassembled the chain we wrote and got U+00E9 back at both
 * positions, printing it as byte 0xE9 in its own single-byte output charset.
 * That is a third party reading a real accented long name off a card this
 * library wrote; before #606 the same slot held `?`.
 *
 * (Both images also report `Label '' stored in boot sector is not valid`, which
 * is `ra8_fs_format()` leaving `BS_VolLab` blank and has nothing to do with
 * names; it is tracked separately.)
 *
 * @par macOS interoperability:
 * macOS normalises a file name it CREATES to NFD -- `e` followed by a combining
 * acute rather than the single U+00E9 -- and hands names back in the form they
 * are stored in. This library stores what it is given and compares what it is
 * given, so a name created here in NFC reads back as NFC on a Mac and vice
 * versa, and the two forms are DIFFERENT names to both sides. That is the same
 * behaviour Windows and Linux have on FAT, and normalising would be worse: it
 * would mean a name this API returned was not the name on the card. The
 * ::internal_test_nfc_and_nfd_are_different_names case pins it so the choice is a
 * recorded one rather than an accident.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fs_lfn_write_test_util.h"
#include "ra8_err.h"
#include "ra8_fs.h"
#include "ra8_test_file.h"
#include "ra8_test_file_posix.h"
#include "ra8_test_output.h"
#include "unity_minimal.h"

/**
 * @enum un_val_t
 * @brief Sizes and expected counts for these cases.
 *
 * @invariant `k_un_alias_underscores` is a CHARACTER count, which is the whole
 *            point: one underscore per unrepresentable character, not per byte.
 * @see internal_test_alias_is_one_underscore_per_character()
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_un_path_cap          = 320U, /**< Scratch path buffer.                     */
  k_un_alias_underscores = 2U,   /**< "ni hao" is two characters, six bytes.   */
  k_un_scan_slots        = 512U, /**< Root slots any raw scan here will visit. */
} un_val_t;

/* ===========================================================================
 * The names, spelled out in bytes
 * ===========================================================================
 */

/** @var s_resume_nfc
 *  @brief "R", U+00E9, "sum", U+00E9, ".txt" -- two-byte UTF-8, NFC form.
 *  @details The everyday case: a Latin name with accents. Ten UTF-16 units,
 *           twelve UTF-8 bytes, which is exactly the discrepancy that broke
 *           every length calculation before #606.
 *  @note Read-only; shared by several cases.
 *  @since 0.1.0
 */
static const char s_resume_nfc[] = {'R',
                                    (char)(unsigned char)0xC3U,
                                    (char)(unsigned char)0xA9U,
                                    's',
                                    'u',
                                    'm',
                                    (char)(unsigned char)0xC3U,
                                    (char)(unsigned char)0xA9U,
                                    '.',
                                    't',
                                    'x',
                                    't',
                                    '\0'};

/** @var s_resume_upper
 *  @brief The same name in upper case: U+00C9 where ::s_resume_nfc has U+00E9.
 *  @details What a case-insensitive lookup has to match. It only does if the
 *           fold covers the BMP rather than just a-z.
 *  @note Read-only.
 *  @since 0.1.0
 */
static const char s_resume_upper[] = {'R',
                                      (char)(unsigned char)0xC3U,
                                      (char)(unsigned char)0x89U,
                                      'S',
                                      'U',
                                      'M',
                                      (char)(unsigned char)0xC3U,
                                      (char)(unsigned char)0x89U,
                                      '.',
                                      'T',
                                      'X',
                                      'T',
                                      '\0'};

/** @var s_resume_nfd
 *  @brief "Re" + U+0301 + "sume" + U+0301 + ".txt" -- the decomposed form.
 *  @details Twelve units where ::s_resume_nfc is ten, so the two are different
 *           names by length alone. macOS produces this form; see the file
 *           header for why it is not normalised away.
 *  @note Read-only.
 *  @since 0.1.0
 */
static const char s_resume_nfd[] = {'R',
                                    'e',
                                    (char)(unsigned char)0xCCU,
                                    (char)(unsigned char)0x81U,
                                    's',
                                    'u',
                                    'm',
                                    'e',
                                    (char)(unsigned char)0xCCU,
                                    (char)(unsigned char)0x81U,
                                    '.',
                                    't',
                                    'x',
                                    't',
                                    '\0'};

/** @var s_resume_plain
 *  @brief "Resume.txt": the same letters with the accents removed.
 *  @details The collision control. Both names were `R?sum?.txt` before #606 --
 *           no, worse: this one was itself, and the accented one became a
 *           string of question marks that matched nothing the caller could
 *           type. Either way the two must be distinct files, and are.
 *  @note Read-only.
 *  @since 0.1.0
 */
static const char s_resume_plain[] = "Resume.txt";

/** @var s_cjk_name
 *  @brief U+4F60 U+597D ".txt" -- three-byte UTF-8.
 *  @details Seven UTF-16 units, ten UTF-8 bytes. The old exFAT reader returned
 *           the low byte of U+4F60, which is a backtick.
 *  @note Read-only.
 *  @since 0.1.0
 */
static const char s_cjk_name[] = {(char)(unsigned char)0xE4U,
                                  (char)(unsigned char)0xBDU,
                                  (char)(unsigned char)0xA0U,
                                  (char)(unsigned char)0xE5U,
                                  (char)(unsigned char)0xA5U,
                                  (char)(unsigned char)0xBDU,
                                  '.',
                                  't',
                                  'x',
                                  't',
                                  '\0'};

/** @var s_emoji_name
 *  @brief U+1F600 ".txt" -- four-byte UTF-8, and a SURROGATE PAIR on disk.
 *  @details Six UTF-16 units from eight UTF-8 bytes, because the character
 *           itself costs two units. Nothing else in this suite exercises the
 *           pair through the whole stack.
 *  @note Read-only.
 *  @since 0.1.0
 */
static const char s_emoji_name[] = {(char)(unsigned char)0xF0U,
                                    (char)(unsigned char)0x9FU,
                                    (char)(unsigned char)0x98U,
                                    (char)(unsigned char)0x80U,
                                    '.',
                                    't',
                                    'x',
                                    't',
                                    '\0'};

/* ===========================================================================
 * Fixture
 * ===========================================================================
 */

/**
 * @brief Allocate and format a 64 MiB FAT16 volume in the fixture's RAM disk.
 *
 * @details `ra8_fs_format()` rather than a hand-built BPB, because the image
 *          this suite dumps is handed to a real `fsck.fat`, which wants a
 *          complete boot sector to work from.
 *
 * @return Nothing.
 *
 * @pre `s_disk.bytes` is either null or owned by the fixture.
 * @pre The caller will release it afterwards.
 * @post `s_disk` holds a mountable, empty FAT16 volume.
 * @post Any previous image has been released.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_build_formatted_fat16(void)
{
  if (s_disk.bytes != nullptr) {
    free(s_disk.bytes);
    s_disk.bytes = nullptr;
  }
  s_disk.byte_count  = (uint32_t)k_lw_fmt_blocks * (uint32_t)k_geo_blk_sz;
  s_disk.bytes       = (uint8_t*)calloc(1, s_disk.byte_count);
  s_disk.block_count = (uint32_t)k_lw_fmt_blocks;
  if (s_disk.bytes == nullptr) {
    TEST_FAIL_FMT("%s", "calloc failed");
  }
  ra8_fs_format_opts_t opts = {};
  opts.type                 = k_ra8_fs_type_fat16;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_format(&s_backend, &opts));
}

/**
 * @brief Release the fixture's RAM disk.
 *
 * @return Nothing.
 *
 * @pre No mount is still open on it.
 * @pre `s_disk.bytes` was allocated by ::internal_build_formatted_fat16().
 * @post `s_disk.bytes` is null.
 * @post No other fixture state is touched.
 *
 * @note Not thread-safe.
 * @since 0.1.0

 * @details Performs one bounded, deterministic operation for this host test.
*/
RA8_INTERNAL static void internal_release_volume(void)
{
  free(s_disk.bytes);
  s_disk.bytes = nullptr;
}

/**
 * @brief Write the current RAM disk out when `RA8_FS606_DUMP` is set.
 *
 * @details A no-op unless the environment asks for it, so the suite is
 *          side-effect-free in CI. See this file's header for the `fsck.fat`
 *          invocation the dumps are for.
 *
 * @param[in] tag Suffix distinguishing this dump from the others.
 *
 * @return Nothing.
 *
 * @pre @p tag is non-NULL; `s_disk.bytes` is allocated.
 * @pre The caller has finished mutating the volume.
 * @post A file exists at `$RA8_FS606_DUMP.<tag>` when the variable is set.
 * @post Nothing is written when it is not.
 *
 * @note Not thread-safe (reads the fixture singleton).
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_maybe_dump_image(const char* tag)
{
  const char* base = getenv("RA8_FS606_DUMP");
  if (base == nullptr) {
    return;
  }
  char path[k_un_path_cap] = {};
  (void)snprintf(path, sizeof(path), "%s.%s", base, tag);
  const ra8_test_file_result_t result =
    internal_test_file_replace(path, s_disk.bytes, (size_t)s_disk.byte_count);
  if (result.status != k_ra8_test_file_ok) {
    return;
  }
  ra8_test_output_t    output = {};
  ra8_test_output_fd_t state  = {};
  TEST_ASSERT(internal_test_output_fd_init(&output, &state, STDOUT_FILENO));
  (void)internal_test_output_text(&output, "  [dump] ");
  (void)internal_test_output_text(&output, path);
  (void)internal_test_output_text(&output, "\n");
  TEST_ASSERT_EQ(k_ra8_test_output_ok, output.status);
}

/**
 * @brief Does @p want appear exactly once in a listing of the root?
 *
 * @param[in,out] h    Mounted volume.
 * @param[in]     want The name expected, as the caller spelled it.
 *
 * @return Presence flag.
 * @retval 1 Exactly one reported name equalled @p want, byte for byte.
 * @retval 0 It was absent, or reported under some other spelling.
 *
 * @pre @p h is mounted and @p want is non-NULL.
 * @pre The listing fits ::k_lw_names_cap entries.
 * @post No state is modified.
 * @post The comparison was byte-exact, so a substitution would fail it.
 *
 * @note Not thread-safe.
 * @since 0.1.0

 * @details Performs one bounded, deterministic operation for this host test.
*/
RA8_INTERNAL static uint8_t internal_listing_holds(ra8_fs_mount_t* h, const char* want)
{
  name_list_t l = {};
  if (ra8_fs_listdir(h, "/", internal_collect_cb, &l) != k_ra8_ok) {
    return 0U;
  }
  uint32_t hits = 0U;
  for (uint32_t i = 0U; i < l.count; i++) {
    if (strcmp(l.name[i], want) == 0) {
      hits++;
    }
  }
  return (hits == 1U) ? 1U : 0U;
}

/* ===========================================================================
 * Tests
 * ===========================================================================
 */

/**
 * @test internal_test_non_ascii_names_round_trip
 * @brief Create, list, re-open, rename and unlink, for 2-, 3- and 4-byte names.
 *
 * @details The whole verb set against one name at a time, because every verb
 *          reaches the name through a different helper and each of them had to
 *          learn units: `open` through `priv_dir_reserve` / `priv_dir_commit`,
 *          `listdir` through `priv_lfn_units_for`, `rename` and `unlink`
 *          through `priv_dir_lookup_any`.
 *
 *          The four-byte name is the one that proves surrogate PAIRS survive
 *          the whole stack: it is six UTF-16 units from eight UTF-8 bytes, so
 *          any code still counting bytes reserves the wrong number of slots.
 *
 * @par MC/DC:
 * No new compound decision; this is the composition the unit suites isolate
 * (`tests/storage/src/test_ra8_fs_utf.c` for the codec,
 * `tests/storage/src/test_ra8_fs_lfn_write*.c` for the chain layout). What it
 * adds is that they are wired together.
 *
 * @pre A formatted FAT16 volume; nothing open.
 * @post Each name was created, listed under its own spelling, re-opened by it,
 *       renamed to another non-ASCII name and finally unlinked.
 *
 * @since 0.1.0

 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
*/
RA8_INTERNAL static void internal_test_non_ascii_names_round_trip(void)
{
  TEST_BEGIN("fs unicode: 2-, 3- and 4-byte names survive every verb");
  internal_build_formatted_fat16();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  const char* names[] = {s_resume_nfc, s_cjk_name, s_emoji_name};
  for (uint32_t i = 0U; i < (uint32_t)(sizeof(names) / sizeof(names[0])); i++) {
    char path[k_un_path_cap] = {};
    (void)snprintf(path, sizeof(path), "/%s", names[i]);

    /* Create + read back through a fresh open: the name has to survive the
     * directory, not merely the open file handle. */
    internal_write_and_verify(h, path);
    TEST_ASSERT_EQ(1U, internal_listing_holds(h, names[i]));

    /* Rename to a name that is also non-ASCII, so both halves of the rename
     * exercise the conversion rather than one of them falling back to 8.3. */
    char renamed[k_un_path_cap] = {};
    (void)snprintf(renamed, sizeof(renamed), "/x%s", names[i]);
    TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_rename(h, path, renamed));
    TEST_ASSERT_EQ(0U, internal_listing_holds(h, names[i]));

    ra8_fs_file_t* f = nullptr;
    TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, renamed, k_ra8_fs_mode_read, &f));
    TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));

    TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unlink(h, renamed));
    TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_fs_open(h, renamed, k_ra8_fs_mode_read, &f));
  }

  /* Nothing left behind: an unlink that missed the chain would show here as an
   * entry the listing still reports. */
  name_list_t l = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_listdir(h, "/", internal_collect_cb, &l));
  TEST_ASSERT_EQ(0U, l.count);
  TEST_ASSERT_EQ(0U, internal_count_orphan_slots(h));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_release_volume();
  TEST_END("fs unicode: 2-, 3- and 4-byte names survive every verb");
}

/**
 * @test internal_test_mixed_case_lookup_folds_across_the_bmp
 * @brief An upper-case spelling opens a lower-case name, accents included.
 *
 * @details The fold is the canonical up-case table, so U+00E9 and U+00C9 are
 *          the same name for the same reason `s` and `S` are. An ASCII-only
 *          fold would open `RESUME.TXT` and miss this one, which is why the
 *          case asserts the accented spelling rather than a plain one.
 *
 * @par MC/DC:
 * Decision: `if (upcase(a[i]) != upcase(b[i]))` in
 * `libs/ra8_fs/src/ra8_fs_utf.c@priv_utf16_ieq`, reached here through
 * `priv_dir_find_long_sector` (1 condition).
 * - V1: every folded unit equal -> false -> the entry matches (the open here).
 * - V2: one folded unit differs -> true  -> no match (the accent-free control).
 *
 * @pre A formatted FAT16 volume; nothing open.
 * @post The upper-case spelling opened the file, and the accent-free spelling
 *       did not -- which is what makes the first assertion mean something.
 *
 * @since 0.1.0

 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
*/
RA8_INTERNAL static void internal_test_mixed_case_lookup_folds_across_the_bmp(void)
{
  TEST_BEGIN("fs unicode: an upper-case spelling opens an accented name");
  internal_build_formatted_fat16();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  char path[k_un_path_cap] = {};
  (void)snprintf(path, sizeof(path), "/%s", s_resume_nfc);
  internal_write_and_verify(h, path);

  char upper[k_un_path_cap] = {};
  (void)snprintf(upper, sizeof(upper), "/%s", s_resume_upper);
  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, upper, k_ra8_fs_mode_read, &f)); /* V1 */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));

  /* The control: same letters, no accents. Folding must not reach that far. */
  char plain[k_un_path_cap] = {};
  (void)snprintf(plain, sizeof(plain), "/%s", s_resume_plain);
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_fs_open(h, plain, k_ra8_fs_mode_read, &f)); /* V2 */

  /* And the name is still reported as it was written, not as it was matched. */
  TEST_ASSERT_EQ(1U, internal_listing_holds(h, s_resume_nfc));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_release_volume();
  TEST_END("fs unicode: an upper-case spelling opens an accented name");
}

/**
 * @test internal_test_accents_do_not_collide
 * @brief Two names differing only in their accents are two files.
 *
 * @details The concrete form of the old defect. With `?` substituted for every
 *          unit above 0x7F, `R?sum?.txt` was the only name the reader could
 *          report for the accented file, and any second name that mangled to
 *          the same string was indistinguishable from it. Here both files exist
 *          at once, each opens under its own spelling, and each holds its own
 *          bytes.
 *
 * @par MC/DC:
 * Decision: `if (an != bn)` in `priv_utf16_ieq` (1 condition), reached through
 * the directory scan.
 * - V1: the two names have the same unit count (both spellings here) -> false
 *   -> the fold loop decides, and it says they differ.
 * - V2: different unit counts (the NFD case in the sibling test) -> true.
 *
 * @pre A formatted FAT16 volume; nothing open.
 * @post Both names exist, are listed distinctly, and open independently.
 *
 * @since 0.1.0

 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
*/
RA8_INTERNAL static void internal_test_accents_do_not_collide(void)
{
  TEST_BEGIN("fs unicode: names differing only in an accent are two files");
  internal_build_formatted_fat16();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  char accented[k_un_path_cap] = {};
  char plain[k_un_path_cap]    = {};
  (void)snprintf(accented, sizeof(accented), "/%s", s_resume_nfc);
  (void)snprintf(plain, sizeof(plain), "/%s", s_resume_plain);
  internal_write_and_verify(h, accented);
  internal_write_and_verify(h, plain);

  name_list_t l = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_listdir(h, "/", internal_collect_cb, &l));
  TEST_ASSERT_EQ(2U, l.count);
  TEST_ASSERT_EQ(1U, internal_listing_holds(h, s_resume_nfc));
  TEST_ASSERT_EQ(1U, internal_listing_holds(h, s_resume_plain));

  /* Taking one away must not disturb the other -- the failure mode if the two
   * were ever the same entry underneath. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unlink(h, accented));
  TEST_ASSERT_EQ(0U, internal_listing_holds(h, s_resume_nfc));
  TEST_ASSERT_EQ(1U, internal_listing_holds(h, s_resume_plain));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_release_volume();
  TEST_END("fs unicode: names differing only in an accent are two files");
}

/**
 * @test internal_test_nfc_and_nfd_are_different_names
 * @brief Normalisation forms are not folded together, and that is deliberate.
 *
 * @details macOS composes a name it creates into NFD; this library stores and
 *          compares what it is given. So the precomposed and decomposed
 *          spellings of the same word are two names here, exactly as they are
 *          on Windows and Linux FAT drivers. Normalising would mean a name this
 *          API returned was not the name on the card, which is the class of
 *          defect #606 is about. This case exists so the choice is recorded and
 *          a future change to it has to be deliberate.
 *
 * @par MC/DC:
 * Decision: `if (an != bn)` in `priv_utf16_ieq` (1 condition), the TRUE arm:
 * ten units against twelve. Its false arm is every matching lookup above.
 *
 * @pre A formatted FAT16 volume; nothing open.
 * @post The two forms coexist and each opens only under its own spelling.
 *
 * @since 0.1.0

 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
*/
RA8_INTERNAL static void internal_test_nfc_and_nfd_are_different_names(void)
{
  TEST_BEGIN("fs unicode: NFC and NFD are different names, on purpose");
  internal_build_formatted_fat16();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  char nfc[k_un_path_cap] = {};
  char nfd[k_un_path_cap] = {};
  (void)snprintf(nfc, sizeof(nfc), "/%s", s_resume_nfc);
  (void)snprintf(nfd, sizeof(nfd), "/%s", s_resume_nfd);
  internal_write_and_verify(h, nfc);

  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_fs_open(h, nfd, k_ra8_fs_mode_read, &f));

  internal_write_and_verify(h, nfd);
  TEST_ASSERT_EQ(1U, internal_listing_holds(h, s_resume_nfc));
  TEST_ASSERT_EQ(1U, internal_listing_holds(h, s_resume_nfd));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_release_volume();
  TEST_END("fs unicode: NFC and NFD are different names, on purpose");
}

/**
 * @test internal_test_alias_is_one_underscore_per_character
 * @brief The 8.3 alias substitutes per CHARACTER, as VFAT does.
 *
 * @details The generated alias is the only place a non-ASCII character is
 *          deliberately lost, and it has to be lost at the right rate: VFAT
 *          replaces one character it cannot represent with one `_`. Working
 *          from UTF-8 BYTES would have written three underscores for the CJK
 *          character below, producing an alias of a different length and a
 *          different 8.3 checksum -- which every slot of the chain carries.
 *
 * @par MC/DC:
 * Decision: `if ((uint32_t)u > k_lfn_del)` in
 * `libs/ra8_fs/src/ra8_fs_fat_name.c@internal_alias_map_unit` (1 condition).
 * - V1: a CJK unit  -> true  -> `_` (the two underscores asserted here).
 * - V2: an ASCII unit -> false -> the up-cased character (the `TXT` extension).
 *
 * @pre A formatted FAT16 volume; nothing open.
 * @post The alias holds exactly ::k_un_alias_underscores underscores.
 *
 * @since 0.1.0

 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
*/
RA8_INTERNAL static void internal_test_alias_is_one_underscore_per_character(void)
{
  TEST_BEGIN("fs unicode: the 8.3 alias substitutes per character, not per byte");
  internal_build_formatted_fat16();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  char path[k_un_path_cap] = {};
  (void)snprintf(path, sizeof(path), "/%s", s_cjk_name);
  internal_write_and_verify(h, path);

  /* Find the one 8.3 entry: everything before it is the long-name chain. */
  const uint8_t* found = nullptr;
  for (uint32_t i = 0U; i < (uint32_t)k_un_scan_slots; i++) {
    const uint8_t* slot = internal_root_slot(h, i);
    if (slot[k_lw_off_name] == (uint8_t)k_lw_free_perm) {
      break;
    }
    if (slot[k_lw_off_name] == (uint8_t)k_lw_free_used) {
      continue;
    }
    if (slot[k_lw_off_attr] == (uint8_t)k_lw_attr_lfn) {
      continue;
    }
    found = slot;
    break;
  }
  TEST_ASSERT_NOT_NULL(found);

  uint32_t underscores = 0U;
  for (uint32_t i = 0U; i < (uint32_t)k_lw_name_len; i++) {
    if (found[i] == (uint8_t)'_') {
      underscores++;
    }
  }
  TEST_ASSERT_EQ(k_un_alias_underscores, underscores); /* V1 */
  /* And the ASCII half came through as itself, up-cased. */
  TEST_ASSERT_EQ('T', found[8]); /* V2 */
  TEST_ASSERT_EQ('X', found[9]);
  TEST_ASSERT_EQ('T', found[10]);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_release_volume();
  TEST_END("fs unicode: the 8.3 alias substitutes per character, not per byte");
}

/**
 * @test internal_test_malformed_utf8_is_refused
 * @brief A name that is not UTF-8 fails the create; nothing lands on disk.
 *
 * @details The half that keeps the round trip honest. A driver that stored
 *          malformed bytes verbatim would produce a name it could not hand
 *          back, which is where the whole class of defect starts. Each vector
 *          asserts the ERROR CODE and then that the directory is still empty --
 *          a create that failed halfway would show as an orphaned chain.
 *
 * @par MC/DC:
 * Decision: `if (kind == k_name_kind_invalid)` in
 * `libs/ra8_fs/src/ra8_fs_fat_lfn_write.c@priv_dir_reserve` (1 condition).
 * - V1: a malformed name -> true  -> `k_ra8_err_invalid_arg` (this case).
 * - V2: a well-formed one -> false -> the reservation proceeds (every other
 *   case in this file).
 *
 * @pre A formatted FAT16 volume; nothing open.
 * @post Every malformed name was refused and the root directory stayed empty.
 *
 * @since 0.1.0

 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
*/
RA8_INTERNAL static void internal_test_malformed_utf8_is_refused(void)
{
  TEST_BEGIN("fs unicode: a name that is not UTF-8 never reaches the disk");
  internal_build_formatted_fat16();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  static const char truncated[] = {'/', 'a', (char)(unsigned char)0xC3U, '\0'};
  static const char overlong[]  = {'/',
                                   'a',
                                   (char)(unsigned char)0xC0U,
                                   (char)(unsigned char)0x80U,
                                   '\0'};
  static const char surrogate[] = {'/',
                                   'a',
                                   (char)(unsigned char)0xEDU,
                                   (char)(unsigned char)0xA0U,
                                   (char)(unsigned char)0x80U,
                                   '\0'};
  const char*       bad[]       = {truncated, overlong, surrogate};

  for (uint32_t i = 0U; i < (uint32_t)(sizeof(bad) / sizeof(bad[0])); i++) {
    ra8_fs_file_t* f = nullptr;
    TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_fs_open(h, bad[i], k_ra8_fs_mode_write, &f));
  }

  name_list_t l = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_listdir(h, "/", internal_collect_cb, &l));
  TEST_ASSERT_EQ(0U, l.count);
  TEST_ASSERT_EQ(0U, internal_count_orphan_slots(h));

  /* The negative control: a well-formed non-ASCII name still gets through, so
   * the assertions above are not passing because creates fail generally. */
  char ok_path[k_un_path_cap] = {};
  (void)snprintf(ok_path, sizeof(ok_path), "/%s", s_resume_nfc);
  internal_write_and_verify(h, ok_path);
  TEST_ASSERT_EQ(1U, internal_listing_holds(h, s_resume_nfc));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_release_volume();
  TEST_END("fs unicode: a name that is not UTF-8 never reaches the disk");
}

/**
 * @test internal_test_dump_images_for_fsck
 * @brief Build the images the out-of-band `fsck.fat` evidence is taken from.
 *
 * @details Two states, because one proves nothing on its own: a volume holding
 *          three non-ASCII names written through the public API, and a control
 *          in which the trailing 8.3 entry's name field is corrupted so its
 *          checksum no longer matches the chain in front of it. `fsck.fat`
 *          reports the second as an orphaned long-name part and the first as
 *          clean, which is what shows the checker was actually looking.
 *
 * @par MC/DC:
 * No decision this file does not already cover; this case exists for the
 * artefacts and for the in-process orphan assertions either side of the
 * corruption.
 *
 * @pre dosfstools is available on the host for the out-of-band step (optional).
 * @pre `RA8_FS606_DUMP` is set for the dumps to appear.
 * @post The clean image holds no orphan; the control image holds one.
 *
 * @since 0.1.0

 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
*/
RA8_INTERNAL static void internal_test_dump_images_for_fsck(void)
{
  TEST_BEGIN("fs unicode: images for the out-of-band fsck.fat run");
  internal_build_formatted_fat16();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  const char* names[] = {s_resume_nfc, s_cjk_name, s_emoji_name};
  for (uint32_t i = 0U; i < (uint32_t)(sizeof(names) / sizeof(names[0])); i++) {
    char path[k_un_path_cap] = {};
    (void)snprintf(path, sizeof(path), "/%s", names[i]);
    internal_write_and_verify(h, path);
  }
  TEST_ASSERT_EQ(0U, internal_count_orphan_slots(h));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_maybe_dump_image("unicode");

  /* The control: break the FIRST 8.3 entry's name so the chain ahead of it
   * carries a checksum that answers to nothing. Same volume, same names, one
   * byte different -- and the same scanner has to report it. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  for (uint32_t i = 0U; i < (uint32_t)k_un_scan_slots; i++) {
    uint8_t* slot = internal_root_slot(h, i);
    if (slot[k_lw_off_name] == (uint8_t)k_lw_free_perm) {
      break;
    }
    if (slot[k_lw_off_attr] == (uint8_t)k_lw_attr_lfn) {
      continue;
    }
    slot[0] = (uint8_t)'Z';
    break;
  }
  TEST_ASSERT(internal_count_orphan_slots(h) > 0U);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_maybe_dump_image("mangle_ctrl");

  internal_release_volume();
  TEST_END("fs unicode: images for the out-of-band fsck.fat run");
}

/**
 * @brief Run every case in this suite.
 *
 * @return Process exit status.
 * @retval 0 Every case passed; a failure aborts inside the harness instead.
 *
 * @pre No other suite shares this process.
 * @pre The fixture's RAM disk is not held by anything else.
 * @post Every case in the file has run and released its volume.
 * @post Nothing remains allocated.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
int main(void)
{
  internal_test_non_ascii_names_round_trip();
  internal_test_mixed_case_lookup_folds_across_the_bmp();
  internal_test_accents_do_not_collide();
  internal_test_nfc_and_nfd_are_different_names();
  internal_test_alias_is_one_underscore_per_character();
  internal_test_malformed_utf8_is_refused();
  internal_test_dump_images_for_fsck();
  TEST_ASSERT_EQ(
    k_ra8_test_output_ok,
    internal_test_output_fd_text(STDERR_FILENO, "[OK  ] test_ra8_fs_unicode_names.c\n"));
  return 0;
}
