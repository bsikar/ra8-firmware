/**
 * @file test_ra8_fs_exfat_rename_long.c
 * @brief exFAT long-name rename: entry-set resize + the fsck.exfat evidence (#603).
 *
 * @details
 * #603(b): `priv_exfat_rename()` used to refuse any name past a single Name
 * entry (15 UTF-16 units) and return `k_ra8_err_not_supported`, yet
 * `ra8_fs_write_file()` writes names up to `k_exfat_name_cap` (64) -- so rename
 * was unusable for names the same library had just written. The fix rewrites the
 * WHOLE entry set under the new name: in place when the Name-entry count is
 * unchanged, and by relocating the set to a fresh slot run (then retiring the
 * old set) when the count grows or shrinks.
 *
 * This suite renames across that boundary in both directions and across a change
 * in Name-entry count, and in every case proves the file's data survives (same
 * bytes, reopened by the NEW name) and the old name is gone. It also proves the
 * refusals the contract still makes: a name over the cap, and a target that
 * already exists.
 *
 * @par Out-of-band `fsck.exfat` evidence:
 * Setting `RA8_FS603_DUMP` writes the PARTITION out -- not the whole RAM disk,
 * because `ra8_fs_format()` lays exFAT inside an MBR partition and a checker is
 * pointed at the volume, not the disk:
 * @code
 *   RA8_FS603_DUMP=/tmp/x603 ./test_ra8_fs_exfat_rename_long
 *   /usr/sbin/fsck.exfat -n -v /tmp/x603.renamed   # long renames, all directions
 *   /usr/sbin/fsck.exfat -n -v /tmp/x603.badset    # a Name entry dropped from a set
 * @endcode
 * Confirmed 2026-08-08 on the Linux verification host, exfatprogs 1.2.0:
 * @verbatim
 * renamed -> /tmp/x603.renamed: clean. directories 1, files 3
 * badset  -> ERROR: /LONGNAME...: has a wrong name length ...
 *            /tmp/x603.badset: corrupted. directories 1, files 3
 * @endverbatim
 * The clean line is the whole argument: a checker that has never seen this code
 * calls the relocated multi-Name-entry sets well-formed. The control drops one
 * Name entry from a set and repairs the SetChecksum around the edit, so what is
 * reported is the inconsistency itself -- the very dimension (b) is about, the
 * Name-entry count against the NameLength -- and not an artefact of a clumsy
 * patch.
 *
 * @par Pure-ASCII sources:
 * Every name here is a plain ASCII string literal; no non-ASCII byte appears.
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
#include "support/fs_fat_exfat_mutate_test_util.h"
#include "support/ra8_test_file.h"
#include "support/ra8_test_file_posix.h"
#include "support/ra8_test_output.h"
#include "unity_minimal.h"

/**
 * @enum rn_const_t
 * @brief Sizing / offset constants for the long-name rename suite.
 *
 * @details Every magic number this file needs appears here as a named value.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_rn_payload_len   = 200U,    /**< Distinctive payload: one cluster's worth.     */
  k_rn_fill_step     = 7U,      /**< Payload byte stride, coprime with 256.        */
  k_rn_read_cap      = 256U,    /**< Read-back buffer, larger than the payload.    */
  k_rn_path_cap      = 256U,    /**< Dump-path buffer capacity.                    */
  k_rn_strm_off_hash = 4U,      /**< Stream-ext NameHash byte offset.              */
  k_rn_file_off_csum = 2U,      /**< File entry SetChecksum byte offset.           */
  k_rn_set3_bytes    = 96U,     /**< Three 32-byte entries (a repaired short set). */
  k_rn_csum_hi_bit   = 0x8000U, /**< Wrap bit for the rotate-add checksum.         */
  k_rn_csum_mask     = 0xFFFFU, /**< 16-bit fold mask.                             */
} rn_const_t;

/* ---- helpers ------------------------------------------------------------ */

/**
 * @brief Fill @p buf with a position-dependent, non-constant payload.
 *
 * @details A byte pattern that varies down the buffer so a read-back that
 *          returned a shifted or zeroed cluster would be caught, not a run of
 *          one repeated value that hides such a fault.
 *
 * @param[out] buf Destination buffer.
 * @param[in]  len Number of bytes to fill.
 *
 * @pre @p buf is non-NULL and at least @p len bytes.
 * @post Every byte of @p buf[0..len) is set from its index.
 *
 * @since 0.1.0

 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
*/
RA8_INTERNAL static void internal_fill_payload(uint8_t* buf, uint32_t len)
{
  for (uint32_t i = 0U; i < len; i++) {
    buf[i] = (uint8_t)((i * (uint32_t)k_rn_fill_step) + 1U);
  }
}

/**
 * @brief Assert that @p path opens, reads back @p want, and closes cleanly.
 *
 * @details The direct proof that a rename kept the data: the FirstCluster and
 *          DataLength ride across in the Stream entry, so reopening by the NEW
 *          name must yield the exact bytes written under the old one.
 *
 * @param[in] h    Mounted exFAT volume.
 * @param[in] path Name to open for reading.
 * @param[in] want Expected file contents.
 * @param[in] len  Expected length in bytes.
 *
 * @pre @p h is mounted; @p path names a file; @p want has @p len bytes.
 * @post The file has been opened, fully read, compared, and closed.
 *
 * @since 0.1.0

 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
*/
RA8_INTERNAL static void
internal_expect_content(ra8_fs_mount_t* h, const char* path, const uint8_t* want, uint32_t len)
{
  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, path, k_ra8_fs_mode_read, &f));
  uint8_t  got[k_rn_read_cap] = {};
  uint32_t n                  = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_read(f, got, (uint32_t)k_rn_read_cap, &n));
  TEST_ASSERT_EQ(len, n);
  TEST_ASSERT(memcmp(want, got, (size_t)len) == 0);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
}

/**
 * @brief Assert that @p path does not resolve to any file.
 *
 * @param[in] h    Mounted exFAT volume.
 * @param[in] path Name that must be absent.
 *
 * @pre @p h is mounted.
 * @post No file slot is consumed (the open failed).
 *
 * @since 0.1.0

 * @details Performs one bounded, deterministic operation for this host test.
 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
*/
RA8_INTERNAL static void internal_expect_absent(ra8_fs_mount_t* h, const char* path)
{
  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_fs_open(h, path, k_ra8_fs_mode_read, &f));
}

/**
 * @brief Create @p from, rename it to @p to, and prove the data moved with it.
 *
 * @details The shared body of every direction test: write a distinctive payload
 *          under @p from, rename, then assert @p from is gone and @p to holds
 *          the same bytes. The listing must still report exactly one file, which
 *          catches a relocation that left the old set live (a duplicate name).
 *
 * @param[in] from Existing name.
 * @param[in] to   Replacement name.
 *
 * @pre `s_disk` is free; @p from and @p to are storable ASCII names.
 * @post The volume has been built, exercised, unmounted and released.
 *
 * @since 0.1.0

 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
*/
RA8_INTERNAL static void internal_do_rename_roundtrip(const char* from, const char* to)
{
  internal_build_exfat_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  uint8_t payload[k_rn_payload_len] = {};
  internal_fill_payload(payload, (uint32_t)k_rn_payload_len);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(h, from, payload, (uint32_t)k_rn_payload_len));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_rename(h, from, to));
  internal_expect_absent(h, from);
  internal_expect_content(h, to, payload, (uint32_t)k_rn_payload_len);

  mut_list_ctx_t ctx = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_listdir(h, "/", internal_count_cb, &ctx));
  TEST_ASSERT_EQ(1U, ctx.count);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
}

/**
 * @brief Recompute a repaired short set's SetChecksum straight from the spec.
 *
 * @details Rotate-add over the three entries at @p file_off, skipping the two
 *          bytes the File entry's own checksum occupies. Used only to leave the
 *          negative-control volume well-formed everywhere except the one field
 *          under test, so a checker stops at that field and not at the checksum.
 *
 * @param[in] file_off Byte offset of the set's File entry in the RAM disk.
 *
 * @return The checksum the File entry should carry over three entries.
 * @retval 0..0xFFFF The folded value.
 *
 * @pre `s_disk.bytes` holds a File + Stream + Name set at @p file_off.
 * @pre The caller has already made the edit it is repairing around.
 * @post No state is modified by this function.
 *
 * @since 0.1.0

 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
*/
RA8_INTERNAL static uint32_t internal_set_checksum3_of(uint32_t file_off)
{
  uint32_t cs = 0U;
  for (uint32_t i = 0U; i < (uint32_t)k_rn_set3_bytes; i++) {
    if ((i == (uint32_t)k_rn_file_off_csum) || (i == ((uint32_t)k_rn_file_off_csum + 1U))) {
      continue;
    }
    const uint32_t b = (uint32_t)s_disk.bytes[file_off + i];
    cs               = ((((cs & 1U) != 0U) ? (uint32_t)k_rn_csum_hi_bit : 0U) + (cs >> 1U) + b) &
                       (uint32_t)k_rn_csum_mask;
  }
  return cs;
}

/**
 * @brief Write the PARTITION out when `RA8_FS603_DUMP` is set.
 *
 * @details The partition rather than the whole RAM disk, because `fsck.exfat`
 *          expects a volume and reads the MBR if handed the disk. A no-op when
 *          the variable is unset, so CI stays side-effect-free.
 *
 * @param[in] tag      Suffix distinguishing this dump from the others.
 * @param[in] base_lba The volume's partition start, from the mount handle.
 *
 * @pre @p tag is non-NULL; `s_disk.bytes` is allocated.
 * @post A file exists at `$RA8_FS603_DUMP.<tag>` when the variable is set.
 * @post Nothing is written when it is not.
 *
 * @since 0.1.0

 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @note Test-only helper with no production ABI.
*/
RA8_INTERNAL static void internal_maybe_dump_image(const char* tag, uint32_t base_lba)
{
  const char* base = getenv("RA8_FS603_DUMP");
  if (base == nullptr) {
    return;
  }
  char path[k_rn_path_cap] = {};
  (void)snprintf(path, sizeof(path), "%s.%s", base, tag);
  const size_t                 off = (size_t)base_lba * (size_t)k_mut_block_size;
  const size_t                 all = (size_t)s_disk.block_count * (size_t)k_mut_block_size;
  const ra8_test_file_result_t result =
    internal_test_file_replace(path, &s_disk.bytes[off], all - off);
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

/* ---- tests -------------------------------------------------------------- */

/**
 * @test internal_test_exfat_rename_short_to_short
 * @brief A same-length rename keeps the in-place path and preserves the data.
 *
 * @details Both names need one Name entry, so the entry count is unchanged and
 *          `priv_exfat_place_rename` rewrites each slot where it sits.
 *
 * @par MC/DC:
 * Decision: `if (new_count == old_count)` in `priv_exfat_place_rename` -- 1
 * condition.
 * V1: 3 == 3 -> TRUE -> in-place rewrite (this test).
 * V2: counts differ -> FALSE -> relocation (the short<->long tests).
 *
 * @pre Volume is formatted and accessible.
 * @post The file reopens by its new name with identical bytes.
 *
 * @since 0.1.0

 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
*/
RA8_INTERNAL static void internal_test_exfat_rename_short_to_short(void)
{
  TEST_BEGIN("exfat rename: short -> short, in place");
  internal_do_rename_roundtrip("A.TXT", "B.TXT");
  TEST_END("exfat rename: short -> short, in place");
}

/**
 * @test internal_test_exfat_rename_short_to_long
 * @brief A one-entry name grows to a two-entry name, relocating the set.
 *
 * @details The new 24-unit name needs two Name entries (total 4), the old set
 *          had three, so `priv_exfat_place_rename` finds a fresh 4-slot run,
 *          writes the rebuilt set, and retires the old set. The file's clusters
 *          are untouched: reopening by the long name returns the same bytes.
 *
 * @par MC/DC:
 * Decision: `if (p < new_len)` in `priv_exfat_build_rename_set` -- 1 condition.
 * V1: p < 24 -> TRUE -> a real UTF-16 unit is stored (this test).
 * V2: p >= 24 -> FALSE -> the tail of the last Name entry is left zero (this
 *     test's 24 units leave slots 24..29 of the second entry blank).
 *
 * @pre Volume is formatted and accessible.
 * @post The file reopens by its long name with identical bytes; old name gone.
 *
 * @since 0.1.0

 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
*/
RA8_INTERNAL static void internal_test_exfat_rename_short_to_long(void)
{
  TEST_BEGIN("exfat rename: short -> long, relocate + grow entry count");
  internal_do_rename_roundtrip("SHORT.TXT", "LONGER_FILENAME_HERE.TXT");
  TEST_END("exfat rename: short -> long, relocate + grow entry count");
}

/**
 * @test internal_test_exfat_rename_long_to_short
 * @brief A two-entry name shrinks to a one-entry name, relocating the set.
 *
 * @details The mirror of the grow case: old set four entries, new set three, so
 *          the relocation branch runs again and the extra old slot is retired.
 *
 * @par MC/DC:
 * Decision: `if (new_count == old_count)` in `priv_exfat_place_rename` -- 1
 * condition, FALSE arm (3 != 4) reached from the shrinking side.
 *
 * @pre Volume is formatted and accessible.
 * @post The file reopens by its short name with identical bytes; long name gone.
 *
 * @since 0.1.0

 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
*/
RA8_INTERNAL static void internal_test_exfat_rename_long_to_short(void)
{
  TEST_BEGIN("exfat rename: long -> short, relocate + shrink entry count");
  internal_do_rename_roundtrip("A_LONG_NAME_FILE.DAT", "TINY.DAT");
  TEST_END("exfat rename: long -> short, relocate + shrink entry count");
}

/**
 * @test internal_test_exfat_rename_long_to_long_same_count
 * @brief Two long names that use the same Name-entry count rename in place.
 *
 * @details 16 units and 29 units both need two Name entries, so the entry count
 *          is unchanged and the in-place path handles a multi-Name-entry set --
 *          the case the old three-entry-only code could never reach.
 *
 * @par MC/DC:
 * Decision: `if (new_count == old_count)` in `priv_exfat_place_rename` -- 1
 * condition, TRUE arm at entry count 4 (not 3).
 *
 * @pre Volume is formatted and accessible.
 * @post The file reopens by its new long name with identical bytes.
 *
 * @since 0.1.0

 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
*/
RA8_INTERNAL static void internal_test_exfat_rename_long_to_long_same_count(void)
{
  TEST_BEGIN("exfat rename: long -> long, same entry count, in place");
  internal_do_rename_roundtrip("SIXTEEN_CHARS.AB", "TWENTYNINE_CHARS_NAME_EXAMPLE");
  TEST_END("exfat rename: long -> long, same entry count, in place");
}

/**
 * @test internal_test_exfat_rename_long_to_long_diff_count
 * @brief Two long names that differ in Name-entry count relocate the set.
 *
 * @details 16 units (two Name entries) to 32 units (three Name entries): the
 *          entry count changes from 4 to 5, so the relocation branch runs even
 *          though both names are "long". This is the "long -> long across a
 *          differing entry count" case #603 calls out.
 *
 * @par MC/DC:
 * Decision: `if (new_count == old_count)` in `priv_exfat_place_rename` -- 1
 * condition, FALSE arm at 5 != 4.
 *
 * @pre Volume is formatted and accessible.
 * @post The file reopens by its 31-unit name with identical bytes.
 *
 * @since 0.1.0

 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
*/
RA8_INTERNAL static void internal_test_exfat_rename_long_to_long_diff_count(void)
{
  TEST_BEGIN("exfat rename: long -> long, differing entry count, relocate");
  internal_do_rename_roundtrip("SIXTEEN_CHARS.AB", "THIRTYONE_CHARACTER_NAME_EXMPL.Z");
  TEST_END("exfat rename: long -> long, differing entry count, relocate");
}

/**
 * @test internal_test_exfat_rename_long_collision_refused
 * @brief Renaming a long name onto an existing long name is refused.
 *
 * @details The collision probe fires for multi-Name-entry names too: with both
 *          a 16-unit and a 24-unit file present, renaming one onto the other
 *          returns `k_ra8_err_exists` and leaves both intact.
 *
 * @par MC/DC:
 * Decision: `if (priv_exfat_find(...) == k_ra8_ok)` in `priv_exfat_rename` -- 1
 * condition.
 * V1: target present -> TRUE -> exists returned (this test).
 * V2: target absent -> FALSE -> rename proceeds (every roundtrip test).
 *
 * @pre Volume is formatted and accessible.
 * @post Both files still resolve and rename returned k_ra8_err_exists.
 *
 * @since 0.1.0

 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
*/
RA8_INTERNAL static void internal_test_exfat_rename_long_collision_refused(void)
{
  TEST_BEGIN("exfat rename: long onto existing long -> exists");
  internal_build_exfat_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  const uint8_t one = (uint8_t)'1';
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(h, "SIXTEEN_CHARS.AB", &one, 1U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(h, "LONGER_FILENAME_HERE.TXT", &one, 1U));
  TEST_ASSERT_EQ(k_ra8_err_exists,
                 ra8_fs_rename(h, "SIXTEEN_CHARS.AB", "LONGER_FILENAME_HERE.TXT"));
  /* Both survive: the refusal touched nothing. */
  ra8_fs_stat_t st = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_stat(h, "SIXTEEN_CHARS.AB", &st));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_stat(h, "LONGER_FILENAME_HERE.TXT", &st));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("exfat rename: long onto existing long -> exists");
}

/**
 * @test internal_test_exfat_rename_new_over_cap_refused
 * @brief A replacement name past the storage cap is an argument fault.
 *
 * @details `priv_exfat_needle_units` reports the over-cap name, which rename
 *          maps to `k_ra8_err_invalid_arg` -- distinct from the full-disk and
 *          not-supported verdicts. The source file is untouched.
 *
 * @par MC/DC:
 * Decision: `if (nue != k_ra8_ok)` in `priv_exfat_rename` -- 1 condition.
 * V1: 65-unit name -> needle_units reports over-cap -> TRUE -> invalid_arg.
 * V2: a storable name -> FALSE -> rename proceeds (every roundtrip test).
 *
 * @pre Volume is formatted and accessible.
 * @post rename returned k_ra8_err_invalid_arg and the source still resolves.
 *
 * @since 0.1.0

 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
*/
RA8_INTERNAL static void internal_test_exfat_rename_new_over_cap_refused(void)
{
  TEST_BEGIN("exfat rename: new name over the cap -> invalid_arg");
  internal_build_exfat_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  const uint8_t one = (uint8_t)'1';
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(h, "A.TXT", &one, 1U));
  /* 65 ASCII characters: one over k_exfat_name_cap (64 UTF-16 units). */
  const char* over = "ABCDEFGHIJKLMNOPQRSTUVWXYZABCDEFGHIJKLMNOPQRSTUVWXYZABCDEFGHIJKLM";
  TEST_ASSERT_EQ(65U, strlen(over));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_fs_rename(h, "A.TXT", over));
  ra8_fs_stat_t st = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_stat(h, "A.TXT", &st));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("exfat rename: new name over the cap -> invalid_arg");
}

/**
 * @test internal_test_exfat_rename_relocate_no_room
 * @brief A relocation with no free slot and no room to grow reports no_mem.
 *
 * @details Forces the failure arm of the relocation branch: after writing one
 *          short file, every remaining root slot is marked in use and the whole
 *          allocation bitmap is filled, so a short->long rename cannot find a
 *          4-slot run and cannot grow the directory into a new cluster.
 *          `priv_exfat_find_dir_space` returns `k_ra8_err_no_mem`, which
 *          `priv_exfat_place_rename` propagates.
 *
 * @par MC/DC:
 * Decision: `if (se != k_ra8_ok)` in `priv_exfat_place_rename` -- 1 condition.
 * V1: directory full and volume full -> no_mem -> TRUE -> propagated (this test).
 * V2: space found -> FALSE -> the set is written (every relocate roundtrip).
 *
 * @pre Volume is formatted and accessible.
 * @post rename returned k_ra8_err_no_mem (structural scan skipped: the fixture
 *       is deliberately inconsistent).
 *
 * @since 0.1.0

 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
*/
RA8_INTERNAL static void internal_test_exfat_rename_relocate_no_room(void)
{
  TEST_BEGIN("exfat rename: relocate with no slots and no growth -> no_mem");
  internal_build_exfat_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  const uint8_t one = (uint8_t)'1';
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(h, "SHORT.TXT", &one, 1U));

  /* Wall off the rest of the root cluster: from the first slot past the file's
   * set to the cluster's end, stamp an in-use, non-EOD type byte so no free run
   * remains. 0xFF has bit 7 set (in use) and is not the 0x00 end marker. */
  const uint32_t entries_per_cluster =
    (h->sectors_per_cluster * (uint32_t)k_mut_block_size) / (uint32_t)k_mut_entry_bytes;
  for (uint32_t idx = (uint32_t)k_mut_root_file1_idx; idx < entries_per_cluster; idx++) {
    s_disk.bytes[internal_root_byte(h, idx)] = (uint8_t)k_mut_mask_byte;
  }
  /* And leave the volume no cluster to grow the directory into. */
  internal_alloc_bitmap_fill(h);

  TEST_ASSERT_EQ(k_ra8_err_no_mem, ra8_fs_rename(h, "SHORT.TXT", "LONGER_FILENAME_HERE.TXT"));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("exfat rename: relocate with no slots and no growth -> no_mem");
}

/**
 * @test internal_test_exfat_rename_images_for_fsck
 * @brief Build the images the out-of-band `fsck.exfat` evidence is taken from.
 *
 * @details Two states, because a clean report on its own only proves the checker
 *          ran. The clean image renames three files -- short->long, long->short
 *          and long->long across a differing entry count -- through the public
 *          API, so every relocated multi-Name-entry set is one a real checker
 *          then vouches for. The control drops one Name entry from a long set by
 *          decrementing its SecondaryCount and repairs the SetChecksum around the
 *          edit, leaving a set whose NameLength no longer matches the Name
 *          entries present -- exactly the inconsistency the resize logic exists
 *          to avoid.
 *
 * @par MC/DC:
 * No decision this file does not already cover; this case exists for the
 * artefacts.
 *
 * @pre exfatprogs is available on the host for the out-of-band step (optional).
 * @pre `RA8_FS603_DUMP` is set for the dumps to appear.
 * @post The clean image is `fsck.exfat`-clean; the control is not.
 *
 * @since 0.1.0

 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
*/
RA8_INTERNAL static void internal_test_exfat_rename_images_for_fsck(void)
{
  TEST_BEGIN("exfat rename: images for the out-of-band fsck.exfat run");
  internal_build_exfat_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  uint8_t payload[k_rn_payload_len] = {};
  internal_fill_payload(payload, (uint32_t)k_rn_payload_len);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(h, "SHORT.TXT", payload, (uint32_t)k_rn_payload_len));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_fs_write_file(h, "A_LONG_NAME_FILE.DAT", payload, (uint32_t)k_rn_payload_len));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_fs_write_file(h, "SIXTEEN_CHARS.AB", payload, (uint32_t)k_rn_payload_len));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_rename(h, "SHORT.TXT", "LONGER_FILENAME_HERE.TXT"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_rename(h, "A_LONG_NAME_FILE.DAT", "TINY.DAT"));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_fs_rename(h, "SIXTEEN_CHARS.AB", "THIRTYONE_CHARACTER_NAME_EXMPL.Z"));

  const uint32_t base = h->partition_base_lba;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_maybe_dump_image("renamed", base);

  /* The control: one long-name file whose set loses a Name entry. Decrement the
   * File entry's SecondaryCount so the set stops one Name entry short of what its
   * NameLength needs, then repair the SetChecksum over the now-3-entry set so the
   * only fault a checker can find is the one under test. */
  internal_build_exfat_volume();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_fs_write_file(h, "LONGNAME_ONE_ENTRY_SHY.BIN", payload, (uint32_t)k_rn_payload_len));
  const uint32_t file = internal_root_byte(h, (uint32_t)k_mut_root_file0_idx);
  const uint8_t  sc   = s_disk.bytes[file + (uint32_t)k_mut_file_secnt_off];
  TEST_ASSERT(sc >= 3U); /* Stream + 2 Name entries for a 16..30-unit name. */
  s_disk.bytes[file + (uint32_t)k_mut_file_secnt_off] = (uint8_t)((uint32_t)sc - 1U);
  const uint32_t cs                                   = internal_set_checksum3_of(file);
  s_disk.bytes[file + (uint32_t)k_rn_file_off_csum]   = (uint8_t)(cs & (uint32_t)k_mut_mask_byte);
  s_disk.bytes[file + (uint32_t)k_rn_file_off_csum + 1U] =
    (uint8_t)((cs >> (uint32_t)k_mut_shift_byte8) & (uint32_t)k_mut_mask_byte);
  const uint32_t base2 = h->partition_base_lba;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_maybe_dump_image("badset", base2);

  internal_free_volume();
  TEST_END("exfat rename: images for the out-of-band fsck.exfat run");
}

/* ---- entry point -------------------------------------------------------- */

/**
 * @brief Run the long-name rename suite.
 *
 * @return Process exit status.
 * @retval 0 Every case passed; a failure aborts inside the harness instead.
 *
 * @pre No other suite shares this process.
 * @post Every case has run and released its volume.
 *
 * @since 0.1.0
 */
int main(void)
{
  internal_test_exfat_rename_short_to_short();
  internal_test_exfat_rename_short_to_long();
  internal_test_exfat_rename_long_to_short();
  internal_test_exfat_rename_long_to_long_same_count();
  internal_test_exfat_rename_long_to_long_diff_count();
  internal_test_exfat_rename_long_collision_refused();
  internal_test_exfat_rename_new_over_cap_refused();
  internal_test_exfat_rename_relocate_no_room();
  internal_test_exfat_rename_images_for_fsck();
  TEST_ASSERT_EQ(
    k_ra8_test_output_ok,
    internal_test_output_fd_text(STDERR_FILENO, "[OK  ] test_ra8_fs_exfat_rename_long.c\n"));
  return 0;
}
