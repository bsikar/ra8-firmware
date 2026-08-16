/**
 * @file test_ra8_fs_lfn_erase.c
 * @brief Taking a VFAT long name away again, chain and all (#600).
 *
 * @details
 * `ra8_fs_unlink()` wrote 0xE5 into the 8.3 entry only, leaving the whole
 * attr-0x0F run on disk with a checksum that matched nothing, and
 * `priv_fat_rename()` overwrote the eleven-byte name field in place, leaving
 * the chain spelling a name that no longer existed. In-tree readers skip both;
 * `fsck.fat` and `chkdsk` report them. These cases are the proof that neither
 * happens any more -- and, as importantly, that the proof can fail.
 *
 * @par The orphan scan, and its negative control:
 * ::count_orphan_slots() (in the shared fixture) reimplements what `fsck.fat`
 * reports as "orphaned long file name part": a live attr-0x0F slot that is not
 * followed, contiguously, by an 8.3 entry whose checksum it carries.
 * ::internal_test_unlink_leaves_no_orphans() proves it stays quiet after a real
 * `ra8_fs_unlink()`. That alone would prove nothing -- a scanner that can never
 * fire is also always quiet -- so ::internal_test_orphan_scan_negative_control() first
 * performs the PRE-FIX deletion by hand (0xE5 into the 8.3 entry only, which is
 * all `priv_unlink_locked()` used to do) and asserts the same scanner reports
 * the orphans it left.
 *
 * @par Out-of-band `fsck.fat` evidence:
 * Setting `RA8_FS600_DUMP` writes each interesting volume out so a real checker
 * can be pointed at it. The volumes dumped are made by `ra8_fs_format()`, not
 * hand-built, so `fsck.fat` has a complete BPB to work from:
 * @code
 *   RA8_FS600_DUMP=/tmp/f600 ./test_ra8_fs_lfn_erase
 *   /sbin/fsck.fat -n /tmp/f600.populated       # both long names, intact
 *   /sbin/fsck.fat -n /tmp/f600.orphan_control  # the PRE-FIX deletion
 *   /sbin/fsck.fat -n /tmp/f600.after_unlink    # the deletion this fix does
 * @endcode
 * Confirmed 2026-08-04 on the Linux verification host, dosfstools 4.2:
 * @verbatim
 * populated       -> (no long-name finding); 2 files, 2/65264 clusters
 * orphan_control  -> Orphaned long file name part "Quarterly Report 2026.pdf"
 * after_unlink    -> (no long-name finding); 1 files, 1/65264 clusters
 * @endverbatim
 * The middle line is the negative control doing its job: the same checker, on
 * the same volume, differing only in which slots the deletion cleared. (All
 * three also report `Label '' stored in boot sector is not valid`, which is
 * `ra8_fs_format()` leaving `BS_VolLab` blank and has nothing to do with long
 * names; it is tracked separately.)
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
#include "support/fs_lfn_write_test_util.h"
#include "support/ra8_test_file.h"
#include "support/ra8_test_output.h"
#include "unity_minimal.h"

/* ===========================================================================
 * Tests: taking a long name away again
 * ===========================================================================
 */

/**
 * @test internal_test_unlink_leaves_no_orphans
 * @brief `unlink` of a long-named file removes its whole chain.
 *
 * @details The defect this issue names: `ra8_fs_unlink()` wrote 0xE5 into the
 *          8.3 entry only, leaving the attr-0x0F run behind with a checksum
 *          that matched nothing. The scan afterwards asserts zero live slots of
 *          any kind, which is stronger than "no orphans": it would also catch a
 *          deletion that left the chain but happened to leave a matching entry
 *          too.
 *
 * @par MC/DC:
 * Decision: `if ((ent[attr] == lfn) && (ent[name] != 0xE5) && (ent[csum] ==
 * csum))` in `libs/ra8_fs/src/ra8_fs_fat_lfn_write.c@internal_dir_collect_chain`
 * (3 conditions).
 * - V1: a live long-name slot carrying this entry's checksum -> T,T,T ->
 *   collected (the chain in front of the file deleted here).
 * - V2: the 8.3 entry itself   -> C1=F -> the run ends (every deletion).
 * - V3: a deleted long-name slot -> C1=T, C2=F -> the run resets
 *   (test_ra8_fs_lfn_write_cov.c leaves exactly that shape behind, and
 *   internal_test_orphan_scan_negative_control below plants it deliberately).
 * - V4: a long-name slot with a DIFFERENT checksum -> C1=T, C2=T, C3=F -> the
 *   run resets, so an unrelated chain is not swept up
 *   (internal_test_rename_moves_the_chain leaves one in front of the new entry).
 * N+1 = 4 vectors for N=3: minimal MC/DC.
 *
 * Decision: `if ((w.cur_lba == tlba) && (off == toff))` in the same function
 * (2 conditions).
 * - V1: a slot before the target in the same sector -> C1=T, C2=F -> keep walking.
 * - V2: a slot in an earlier sector                 -> C1=F       -> keep walking.
 * - V3: the target itself                           -> C1=T, C2=T -> stop.
 *
 * @pre A hand-built FAT16 volume is mounted and empty.
 * @post The root directory holds no live entry and no long-name slot.
 *
 * @since 0.1.0

 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
*/
RA8_INTERNAL static void internal_test_unlink_leaves_no_orphans(void)
{
  TEST_BEGIN("lfn erase: unlink removes the whole chain");
  internal_build_fat16_vol();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  internal_write_and_verify(h, "/Quarterly Report 2026.pdf");
  scan_result_t before = {};
  internal_scan_root_of(h, &before);
  TEST_ASSERT_EQ(1U, before.live);
  TEST_ASSERT_EQ(2U, before.lfn_slots);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unlink(h, "/Quarterly Report 2026.pdf"));

  scan_result_t after = {};
  internal_scan_root_of(h, &after);
  TEST_ASSERT_EQ(0U, after.live);
  TEST_ASSERT_EQ(0U, after.lfn_slots);
  TEST_ASSERT_EQ(0U, after.orphans);

  name_list_t l = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_listdir(h, "/", internal_collect_cb, &l));
  TEST_ASSERT_EQ(0U, l.count);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_vol();
  TEST_END("lfn erase: unlink removes the whole chain");
}

/**
 * @test internal_test_orphan_scan_negative_control
 * @brief The orphan scanner fires on the deletion this fix replaced.
 *
 * @details Without this case, ::internal_test_unlink_leaves_no_orphans() proves nothing:
 *          a scanner that can never report an orphan is also always quiet. So
 *          the PRE-FIX deletion is performed by hand here -- 0xE5 into the 8.3
 *          entry's first byte and nothing else, exactly what
 *          `priv_unlink_locked()` used to do -- and the same scanner is asked
 *          the same question. It must say two.
 *
 * @par MC/DC:
 * Decision: `if (spec_checksum(...) == run_sum)` in ::scan_root() (1 condition).
 * - V1: the chain's 8.3 entry is still live -> true  -> counted as chained
 *   (test_long_name_round_trip).
 * - V2: it has been 0xE5'd                  -> the run is broken by the deleted
 *   slot before the comparison is reached, and the slots count as orphans.
 *
 * @pre A hand-built FAT16 volume is mounted and holds one long-named file.
 * @post The scanner reported the two orphaned slots the old deletion left.
 *
 * @since 0.1.0

 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
*/
RA8_INTERNAL static void internal_test_orphan_scan_negative_control(void)
{
  TEST_BEGIN("lfn erase: the orphan scanner fires on the pre-fix deletion");
  internal_build_fat16_vol();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  internal_write_and_verify(h, "/Quarterly Report 2026.pdf");
  TEST_ASSERT_EQ(0U, internal_count_orphan_slots(h));

  /* The pre-fix deletion, by hand: slot 0 and 1 are the chain, slot 2 the 8.3
   * entry. Clearing only the 8.3 entry is what ra8_fs_unlink() used to do. */
  const uint8_t* ent83 = internal_root_slot(h, 2U);
  TEST_ASSERT(ent83[k_lw_off_attr] != (uint8_t)k_lw_attr_lfn);
  internal_root_slot(h, 2U)[k_lw_off_name] = (uint8_t)k_lw_free_used;

  scan_result_t r = {};
  internal_scan_root_of(h, &r);
  TEST_ASSERT_EQ(0U, r.live);
  TEST_ASSERT_EQ(2U, r.lfn_slots);
  TEST_ASSERT_EQ(2U, r.orphans); /* <- the finding fsck.fat reports */
  TEST_ASSERT_EQ(0U, r.chained);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_vol();
  TEST_END("lfn erase: the orphan scanner fires on the pre-fix deletion");
}

/**
 * @test internal_test_rename_moves_the_chain
 * @brief Renaming a long name rebuilds its chain instead of breaking it.
 *
 * @details Rewriting the eleven-byte name field in place, which is what rename
 *          used to do, left the chain spelling the old name and pointing at a
 *          short name that no longer existed. Here the file is renamed long ->
 *          long, then long -> short, and after each the directory is scanned:
 *          exactly the chain the current name needs, and nothing else.
 *
 * @par MC/DC:
 * Decision: `if (priv_dir_lookup_any(...) == k_ra8_ok)` (the duplicate check)
 * in `priv_fat_rename()` (1 condition).
 * - V1: the new name is free  -> false -> the rename proceeds.
 * - V2: the new name is taken -> true  -> k_ra8_err_exists.
 *
 * @pre A hand-built FAT16 volume is mounted and empty.
 * @post Only the final name resolves, with no leftover chain.
 *
 * @since 0.1.0

 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
*/
RA8_INTERNAL static void internal_test_rename_moves_the_chain(void)
{
  TEST_BEGIN("lfn erase: rename rebuilds the chain");
  internal_build_fat16_vol();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  internal_write_and_verify(h, "/Draft Chapter One.txt");
  internal_write_and_verify(h, "/Occupied Name Here.txt");
  TEST_ASSERT_EQ(k_ra8_err_exists,
                 ra8_fs_rename(h, "/Draft Chapter One.txt", "/Occupied Name Here.txt"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unlink(h, "/Occupied Name Here.txt"));

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_fs_rename(h, "/Draft Chapter One.txt", "/Final Chapter One Revised.txt"));
  TEST_ASSERT_EQ(1U, internal_listdir_is_exactly(h, "/", "Final Chapter One Revised.txt"));
  TEST_ASSERT_EQ(0U, internal_count_orphan_slots(h));

  /* The payload survived: rename carried the entry, not just the name. */
  uint8_t  back[k_lw_payload] = {};
  uint8_t  want[k_lw_payload] = {};
  uint32_t got                = 0U;
  internal_fill_payload(want, (uint32_t)k_lw_payload);
  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_fs_open(h, "/Final Chapter One Revised.txt", k_ra8_fs_mode_read, &f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_read(f, back, (uint32_t)k_lw_payload, &got));
  TEST_ASSERT_EQ(k_lw_payload, got);
  TEST_ASSERT_EQ(0, memcmp(want, back, (size_t)k_lw_payload));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));

  /* long -> short: the chain has to go away entirely. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_rename(h, "/Final Chapter One Revised.txt", "/CH1.TXT"));
  scan_result_t r = {};
  internal_scan_root_of(h, &r);
  TEST_ASSERT_EQ(1U, r.live);
  TEST_ASSERT_EQ(0U, r.lfn_slots);
  TEST_ASSERT_EQ(0U, r.orphans);
  TEST_ASSERT_EQ(1U, internal_listdir_is_exactly(h, "/", "CH1.TXT"));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_vol();
  TEST_END("lfn erase: rename rebuilds the chain");
}

/* ===========================================================================
 * Tests: a long-named DIRECTORY, created and then removed
 * ===========================================================================
 */

/**
 * @test internal_test_long_named_directory
 * @brief mkdir, path traversal, and rmdir all work on a long directory name.
 *
 * @details A long name is only useful if paths THROUGH it resolve, so the file
 *          created inside is read back through the long component. `rmdir`
 *          closes the loop: a driver that can create a directory it cannot
 *          remove has made unreachable garbage.
 *
 * @par MC/DC:
 * Decision: `if ((len == 0U) || (len > k_lfn_write_max))` in
 * `priv_enter_subdir()` (2 conditions).
 * - V1: a 12-character component -> F,F -> resolved (test_long_name_round_trip
 *   has no components at all; this one has `Reading List`).
 * - V2: an over-long component   -> F,T -> refused
 *   (test_ra8_fs_fat_file_open_cov.c@test_enter_subdir_name_too_long).
 *
 * @pre A hand-built FAT16 volume is mounted and empty.
 * @post Both the directory and its child are gone and the root is clean.
 *
 * @since 0.1.0

 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
*/
RA8_INTERNAL static void internal_test_long_named_directory(void)
{
  TEST_BEGIN("lfn erase: a long-named directory, and paths through it");
  internal_build_fat16_vol();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mkdir(h, "/Reading List"));
  TEST_ASSERT_EQ(k_ra8_err_exists, ra8_fs_mkdir(h, "/Reading List"));
  TEST_ASSERT_EQ(1U, internal_listdir_is_exactly(h, "/", "Reading List"));

  internal_write_and_verify(h, "/Reading List/Chapter One.txt");
  TEST_ASSERT_EQ(1U, internal_listdir_is_exactly(h, "/Reading List", "Chapter One.txt"));

  ra8_fs_stat_t st = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_stat(h, "/Reading List", &st));
  TEST_ASSERT(st.is_directory);

  TEST_ASSERT_EQ(k_ra8_err_not_empty, ra8_fs_rmdir(h, "/Reading List"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unlink(h, "/Reading List/Chapter One.txt"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_rmdir(h, "/Reading List"));

  scan_result_t r = {};
  internal_scan_root_of(h, &r);
  TEST_ASSERT_EQ(0U, r.live);
  TEST_ASSERT_EQ(0U, r.lfn_slots);
  TEST_ASSERT_EQ(0U, r.orphans);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_vol();
  TEST_END("lfn erase: a long-named directory, and paths through it");
}

/* ===========================================================================
 * Tests: a formatted volume, dumpable for an out-of-band fsck.fat run
 * ===========================================================================
 */

/**
 * @brief Write the current RAM disk out when `RA8_FS600_DUMP` is set.
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
 * @post A file exists at `$RA8_FS600_DUMP.<tag>` when the variable is set.
 * @post Nothing is written when it is not.
 *
 * @note Not thread-safe (reads the fixture singleton).
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_maybe_dump_image(const char* tag)
{
  const char* base = getenv("RA8_FS600_DUMP");
  if (base == nullptr) {
    return;
  }
  char path[k_lw_path_cap] = {};
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
 * @brief Allocate and format a 64 MiB FAT16 volume in the fixture's RAM disk.
 *
 * @details `ra8_fs_format()` rather than the hand-built BPB the other cases
 *          use, because the images this one dumps are handed to a real
 *          `fsck.fat`, which wants a complete boot sector to work from.
 *
 * @return Nothing.
 *
 * @pre `s_disk.bytes` is either null or owned by the fixture.
 * @pre The caller will free_vol() afterwards.
 * @post `s_disk` holds a mountable FAT16 volume.
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
 * @test internal_test_formatted_volume_images
 * @brief Build the two images the out-of-band `fsck.fat` evidence is taken from.
 *
 * @details Same story as ::internal_test_unlink_leaves_no_orphans() and its negative
 *          control, on a formatted volume a real checker will accept. The
 *          in-process assertions still stand on their own; the dumps are what
 *          let `fsck.fat` be pointed at the same two states.
 *
 * @par MC/DC:
 * No compound decision is exercised here that is not already covered by
 * ::internal_test_unlink_leaves_no_orphans(); this case exists for the artefacts.
 *
 * @pre dosfstools is available on the host for the out-of-band step (optional).
 * @pre `RA8_FS600_DUMP` is set for the dumps to appear.
 * @post The post-unlink image holds no orphan; the control image holds two.
 * @post Both images are byte-identical runs of the same public API.
 *
 * @since 0.1.0

 * @note Test-only helper with no production ABI.
*/
RA8_INTERNAL static void internal_test_formatted_volume_images(void)
{
  TEST_BEGIN("lfn erase: formatted-volume images for fsck.fat");
  internal_build_formatted_fat16();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  TEST_ASSERT_EQ(k_ra8_fs_type_fat16, h->type);

  internal_write_and_verify(h, "/Quarterly Report 2026.pdf");
  internal_write_and_verify(h, "/Meeting Notes 2026-08-04.md");
  internal_maybe_dump_image("populated");

  /* The control: the pre-fix deletion of the FIRST file, by hand. */
  scan_result_t r = {};
  internal_scan_root_of(h, &r);
  TEST_ASSERT_EQ(2U, r.live);
  TEST_ASSERT_EQ(0U, r.orphans);
  const uint32_t chain_len                        = 2U; /* 25 characters -> 2 groups */
  internal_root_slot(h, chain_len)[k_lw_off_name] = (uint8_t)k_lw_free_used;
  internal_scan_root_of(h, &r);
  TEST_ASSERT_EQ(chain_len, r.orphans);
  internal_maybe_dump_image("orphan_control");

  /* Back to a real volume, then the real deletion of the second file. */
  internal_build_formatted_fat16();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  internal_write_and_verify(h, "/Quarterly Report 2026.pdf");
  internal_write_and_verify(h, "/Meeting Notes 2026-08-04.md");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unlink(h, "/Quarterly Report 2026.pdf"));
  internal_scan_root_of(h, &r);
  TEST_ASSERT_EQ(1U, r.live);
  TEST_ASSERT_EQ(0U, r.orphans);
  internal_maybe_dump_image("after_unlink");

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_vol();
  TEST_END("lfn erase: formatted-volume images for fsck.fat");
}

/**
 * @brief Run every case in this suite.
 *
 * @return Process exit status.
 * @retval 0 Every case passed; a failure aborts inside the harness instead.
 *
 * @pre The host allocator is available.
 * @pre No other suite shares this process.
 * @post Every volume allocated here has been freed.
 * @post The harness has printed one line per case.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
int main(void)
{
  internal_test_unlink_leaves_no_orphans();
  internal_test_orphan_scan_negative_control();
  internal_test_rename_moves_the_chain();
  internal_test_long_named_directory();
  internal_test_formatted_volume_images();
  TEST_ASSERT_EQ(k_ra8_test_output_ok,
                 internal_test_output_fd_text(STDERR_FILENO, "[OK  ] test_ra8_fs_lfn_erase.c\n"));
  return 0;
}
