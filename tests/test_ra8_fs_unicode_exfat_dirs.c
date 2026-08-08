/**
 * @file test_ra8_fs_unicode_exfat_dirs.c
 * @brief exFAT DIRECTORIES with non-ASCII names (#605 + #606).
 *
 * @details
 * exFAT directories (#605) and UTF-8 names (#606) landed as separate pieces of
 * work, and their intersection is surface neither one's own suite covers: a
 * directory's entry set is built by `priv_exfat_build_dir_set()`, which is a
 * SECOND copy of the name-writing logic the file side has. It carried the same
 * defect the file side did -- one UTF-16 unit per input BYTE, `NameLength`
 * counting bytes, and a hash folded with an ASCII-only up-case -- so a folder
 * called anything but ASCII was written wrong even after the file path was
 * right. `priv_exfat_enter()` had the mirror-image problem on the way back in:
 * it bounded a UTF-8 component's BYTE length by the format's UNIT cap, so a
 * short Cyrillic folder name looked over-long while descending.
 *
 * These cases pin the join. Every one of them ends with `exfat_verify()`, the
 * structural scan #605 brought with it, and that is the point of putting them
 * here rather than in the file-side suite: the scan recomputes each entry set's
 * SetChecksum AND its NameHash from the volume's own bytes, so a directory
 * whose name this library hashed with the wrong fold fails it. The hash is
 * checked by a scanner that predates the fix and knows nothing about it.
 *
 * @par Pure-ASCII sources:
 * Every non-ASCII name is a `static const char[]` of byte escapes.
 *
 * @par Out-of-band `fsck.exfat` evidence:
 * `exfat_dump()` (from the shared fixture) writes each volume out under
 * `RA8_FS_EXFAT_DUMP`, exactly as #605's own suites do, so these images join
 * that evidence set rather than inventing a second convention.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_fs.h"
#include "support/fs_exfat_dir_test_util.h"
#include "unity_minimal.h"

/**
 * @enum udir_const_t
 * @brief Sizes and expected counts for these cases.
 *
 * @invariant `k_udir_payload` is small enough to stay inside one cluster, so a
 *            failure here is about names rather than about allocation.
 * @see test_mkdir_non_ascii_round_trip()
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_udir_path_cap = 320U,  /**< Scratch path buffer.                */
  k_udir_payload  = 32U,   /**< Bytes written into each test file.  */
  k_udir_u8_lead2 = 0xC3U, /**< Lead byte of a two-byte UTF-8 form. */
  k_udir_u8_cap_e = 0x89U, /**< Continuation byte of U+00C9.        */
} udir_const_t;

/** @var s_dir_acc
 *  @brief "Caf" U+00E9 -- a two-byte UTF-8 directory name, four units.
 *  @details Four UTF-16 units from five UTF-8 bytes: the discrepancy that made
 *           the old directory builder write five bogus units and a NameLength
 *           of five.
 *  @note Read-only.
 *  @since 0.1.0
 */
static const char s_dir_acc[] =
  {'C', 'a', 'f', (char)(unsigned char)0xC3U, (char)(unsigned char)0xA9U, '\0'};

/** @var s_dir_cjk
 *  @brief U+4F60 U+597D -- a three-byte UTF-8 directory name, two units.
 *  @details Two units from six bytes, the widest ratio a BMP name reaches.
 *  @note Read-only.
 *  @since 0.1.0
 */
static const char s_dir_cjk[] = {(char)(unsigned char)0xE4U,
                                 (char)(unsigned char)0xBDU,
                                 (char)(unsigned char)0xA0U,
                                 (char)(unsigned char)0xE5U,
                                 (char)(unsigned char)0xA5U,
                                 (char)(unsigned char)0xBDU,
                                 '\0'};

/** @var s_dir_cyr
 *  @brief U+041F U+0430 U+043F U+043A U+0430 -- Cyrillic, five units.
 *  @details Ten UTF-8 bytes. Short enough to be an ordinary folder name and
 *           long enough that bounding its BYTES by a unit cap would have
 *           mattered on a deeper cap; it is here as the descent vector.
 *  @note Read-only.
 *  @since 0.1.0
 */
static const char s_dir_cyr[] = {(char)(unsigned char)0xD0U,
                                 (char)(unsigned char)0x9FU,
                                 (char)(unsigned char)0xD0U,
                                 (char)(unsigned char)0xB0U,
                                 (char)(unsigned char)0xD0U,
                                 (char)(unsigned char)0xBFU,
                                 (char)(unsigned char)0xD0U,
                                 (char)(unsigned char)0xBAU,
                                 (char)(unsigned char)0xD0U,
                                 (char)(unsigned char)0xB0U,
                                 '\0'};

/** @var s_file_acc
 *  @brief "n" U+00F6 "te.txt" -- a non-ASCII file name to put inside a folder.
 *  @note Read-only.
 *  @since 0.1.0
 */
static const char s_file_acc[] =
  {'n', (char)(unsigned char)0xC3U, (char)(unsigned char)0xB6U, 't', 'e', '.', 't', 'x', 't', '\0'};

/**
 * @brief Mount the fixture volume, asserting it came up as exFAT.
 *
 * @return The mounted volume.
 * @retval non-NULL The mount handle.
 *
 * @pre `build_exfat_volume()` has run.
 * @pre No other mount is outstanding.
 * @post The returned handle is an exFAT mount.
 * @post No on-disk state is modified.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static ra8_fs_mount_t* mount_fixture(void)
{
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  TEST_ASSERT_EQ(k_ra8_fs_type_exfat, h->type);
  return h;
}

/**
 * @test test_mkdir_non_ascii_round_trip
 * @brief A folder with a non-ASCII name is created, listed and removed by it.
 *
 * @details The directory equivalent of the file-side round trip, and the case
 *          that would have failed on `priv_exfat_build_dir_set()` alone: the
 *          name is written as units, read back as the same bytes, and the
 *          structural scan agrees the NameHash matches what it recomputes.
 *
 * @par MC/DC:
 * Decision: `if (pos < nlen)` in
 * `libs/ra8_fs/src/ra8_fs_fat_exfat_dir.c@priv_exfat_build_dir_set`
 * (1 condition).
 * - V1: inside the name -> false arm -> a whole unit is written (asserted by
 *   the round trip and by the scan's NameHash recomputation).
 * - V2: past it -> true arm -> the slot stays zero.
 *
 * @pre A formatted exFAT volume; nothing open.
 * @post Each folder was listed under its own spelling and then removed.
 *
 * @since 0.1.0
 */
static void test_mkdir_non_ascii_round_trip(void)
{
  TEST_BEGIN("exfat unicode dirs: a non-ASCII folder round-trips");
  build_exfat_volume();
  ra8_fs_mount_t* h = mount_fixture();

  const char* names[] = {s_dir_acc, s_dir_cjk, s_dir_cyr};
  for (uint32_t i = 0U; i < (uint32_t)(sizeof(names) / sizeof(names[0])); i++) {
    char path[k_udir_path_cap] = {};
    (void)snprintf(path, sizeof(path), "/%s", names[i]);
    TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mkdir(h, path));
  }
  /* Reported under the spelling they were created with, and as DIRECTORIES. */
  name_ctx_t listing = {};
  TEST_ASSERT_EQ(3U, list_names(h, "/", &listing));
  for (uint32_t i = 0U; i < (uint32_t)(sizeof(names) / sizeof(names[0])); i++) {
    TEST_ASSERT_EQ(1U, has_dir(&listing, names[i]));
  }
  /* The scan recomputes every SetChecksum and NameHash off the volume. */
  exfat_verify(h, "unicode_dirs_created");

  for (uint32_t i = 0U; i < (uint32_t)(sizeof(names) / sizeof(names[0])); i++) {
    char path[k_udir_path_cap] = {};
    (void)snprintf(path, sizeof(path), "/%s", names[i]);
    TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_rmdir(h, path));
  }
  name_ctx_t empty = {};
  TEST_ASSERT_EQ(0U, list_names(h, "/", &empty));
  exfat_verify(h, "unicode_dirs_removed");

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("exfat unicode dirs: a non-ASCII folder round-trips");
}

/**
 * @test test_nested_non_ascii_path
 * @brief A file resolves through non-ASCII path components.
 *
 * @details This is the case `priv_exfat_enter()` decides: every component but
 *          the last is resolved while descending, and it used to bound the
 *          component's UTF-8 BYTE length by the format's UTF-16 UNIT cap. The
 *          file below sits two non-ASCII folders deep and is opened by its full
 *          path, so both the descent and the leaf lookup have to agree about
 *          what the name is.
 *
 * @par MC/DC:
 * Decision: `if (len >= k_exfat_name_u8_cap)` in
 * `libs/ra8_fs/src/ra8_fs_fat_exfat_dir.c@priv_exfat_enter` (1 condition).
 * - V1: an ordinary component -> false -> the descent proceeds (this case).
 * - V2: an over-long one -> true -> `k_ra8_err_invalid_arg`
 *   (`test_ra8_fs_exfat_dirs_cov.c@test_lookup_and_resolve_edges`).
 *
 * @pre A formatted exFAT volume; nothing open.
 * @post The nested file was written, listed and read back through the path.
 *
 * @since 0.1.0
 */
static void test_nested_non_ascii_path(void)
{
  TEST_BEGIN("exfat unicode dirs: a nested non-ASCII path resolves");
  build_exfat_volume();
  ra8_fs_mount_t* h = mount_fixture();

  char outer[k_udir_path_cap] = {};
  char inner[k_udir_path_cap] = {};
  char file[k_udir_path_cap]  = {};
  (void)snprintf(outer, sizeof(outer), "/%s", s_dir_cyr);
  (void)snprintf(inner, sizeof(inner), "/%s/%s", s_dir_cyr, s_dir_cjk);
  (void)snprintf(file, sizeof(file), "/%s/%s/%s", s_dir_cyr, s_dir_cjk, s_file_acc);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mkdir(h, outer));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mkdir(h, inner));

  uint8_t payload[k_udir_payload] = {};
  for (uint32_t i = 0U; i < (uint32_t)k_udir_payload; i++) {
    payload[i] = (uint8_t)(i + 1U);
  }
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(h, file, payload, (uint32_t)k_udir_payload));

  /* Listed inside the innermost folder, under its own spelling. */
  name_ctx_t listing = {};
  TEST_ASSERT_EQ(1U, list_names(h, inner, &listing));
  TEST_ASSERT_EQ(0, strcmp(listing.names[0], s_file_acc));

  /* And read back through the whole non-ASCII path. */
  ra8_fs_file_t* f                    = nullptr;
  uint8_t        back[k_udir_payload] = {};
  uint32_t       got                  = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, file, k_ra8_fs_mode_read, &f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_read(f, back, (uint32_t)k_udir_payload, &got));
  TEST_ASSERT_EQ(k_udir_payload, got);
  TEST_ASSERT_EQ(0, memcmp(payload, back, (size_t)k_udir_payload));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));

  exfat_verify(h, "unicode_dirs_nested");
  exfat_dump(h, "unicode_dirs_nested");

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("exfat unicode dirs: a nested non-ASCII path resolves");
}

/**
 * @test test_non_ascii_folder_and_file_do_not_collide
 * @brief A folder and a file whose names differ only in an accent are distinct.
 *
 * @details The collision the `?`-substituting reader used to create, in the
 *          namespace where it does the most damage: `mkdir` refuses a name that
 *          already exists, so two names that folded together would make the
 *          second create fail outright rather than merely read back wrong.
 *
 * @par MC/DC:
 * Decision: `if (upcase(unit) != upcase(name[pos + i]))` in
 * `libs/ra8_fs/src/ra8_fs_fat_exfat_read.c@priv_exfat_name_chunk_eq`
 * (1 condition), reached here through `priv_exfat_mkdir_check`.
 * - V1: the accented and unaccented names fold differently -> true -> no match
 *   -> the second `mkdir` succeeds (this case).
 * - V2: a name that does fold equal -> false -> `k_ra8_err_exists` (the
 *   duplicate assertion below).
 *
 * @pre A formatted exFAT volume; nothing open.
 * @post Both folders exist; an exact-fold duplicate is still refused.
 *
 * @since 0.1.0
 */
static void test_non_ascii_folder_and_file_do_not_collide(void)
{
  TEST_BEGIN("exfat unicode dirs: an accent distinguishes two folders");
  build_exfat_volume();
  ra8_fs_mount_t* h = mount_fixture();

  char accented[k_udir_path_cap] = {};
  (void)snprintf(accented, sizeof(accented), "/%s", s_dir_acc);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mkdir(h, accented));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mkdir(h, "/Caf")); /* V1: same but for U+00E9 */

  name_ctx_t listing = {};
  TEST_ASSERT_EQ(2U, list_names(h, "/", &listing));
  TEST_ASSERT_EQ(1U, has_dir(&listing, s_dir_acc));
  TEST_ASSERT_EQ(1U, has_dir(&listing, "Caf"));

  /* V2: the fold still catches a real duplicate, in the other case. */
  char upper[k_udir_path_cap] = {};
  (void)snprintf(upper,
                 sizeof(upper),
                 "/CAF%c%c",
                 (char)(unsigned char)k_udir_u8_lead2,
                 (char)(unsigned char)k_udir_u8_cap_e);
  TEST_ASSERT_EQ(k_ra8_err_exists, ra8_fs_mkdir(h, upper));

  exfat_verify(h, "unicode_dirs_no_collision");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("exfat unicode dirs: an accent distinguishes two folders");
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
int32_t main(void)
{
  test_mkdir_non_ascii_round_trip();
  test_nested_non_ascii_path();
  test_non_ascii_folder_and_file_do_not_collide();
  (void)fprintf(stderr, "[OK  ] test_ra8_fs_unicode_exfat_dirs.c\n");
  return 0;
}
